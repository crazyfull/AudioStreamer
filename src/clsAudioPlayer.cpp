#include "clsAudioPlayer.h"
#include "opusCodec.h"
#include "VoicePacket.h"
#include <algorithm>
#include <cstring>

clsAudioPlayer::clsAudioPlayer()
{
    m_pConfig = nullptr;
    m_cur = nullptr;
    m_next = nullptr;
    m_retired = nullptr;
    m_netFrameMs = 0;
    m_candidateMs = 0;
    m_candidateCount = 0;
    m_frameMsCopy = 0;
    m_formatDropped = 0;
    m_ready = false;
    m_ingestDropped = 0;
    m_volume = 100;
    m_gain = 1.f;
    m_depthCopy = 0;
    m_pendingPos = 0;
    m_rate = 48000;
    m_channels = 1;
}

clsAudioPlayer::~clsAudioPlayer()
{
    m_ready = false;
    m_output.stop();        // after this the audio callback can no longer touch the streams
    delete m_cur;
    delete m_next.exchange(nullptr);
    reapRetired();
}

// network thread (or destructor): free streams the audio thread has given back
void clsAudioPlayer::reapRetired()
{
    Stream *s = m_retired.exchange(nullptr, std::memory_order_acquire);
    while(s){
        Stream *n = s->retiredLink;
        delete s;
        s = n;
    }
}

// audio thread: switch to the stream the network thread prepared (no allocation, no free)
void clsAudioPlayer::adoptNextStream()
{
    Stream *n = m_next.exchange(nullptr, std::memory_order_acquire);
    if(!n)
        return;
    Stream *old = m_cur;
    m_cur = n;
    m_pending.clear();
    m_pendingPos = 0;
    if(old){
        old->retiredLink = m_retired.load(std::memory_order_relaxed);
        while(!m_retired.compare_exchange_weak(old->retiredLink, old, std::memory_order_release, std::memory_order_relaxed)) {}
    }
}

void clsAudioPlayer::SetConfig(AudioConfig *pConfig)
{
    m_pConfig = pConfig;
    init();
}

void clsAudioPlayer::init()
{
    if(m_output.isRunning() || !m_pConfig)
        return;

    // Opus decoder can output 8/12/16/24/48k. miniaudio converts rate/channels to what the
    // real speaker needs, so there is no "format not supported" fallback anymore.
    m_rate = m_pConfig->renderRate();
    m_channels = m_pConfig->channels;

    // the speaker is not running yet, so nothing else touches these. The decoder + jitter buffer are created
    // when the first packet arrives (Play()), because only then is the frame duration known.
    delete m_cur; m_cur = nullptr;
    delete m_next.exchange(nullptr);
    reapRetired();
    m_netFrameMs = 0; m_candidateMs = 0; m_candidateCount = 0;
    m_pending.clear();
    m_pending.reserve(size_t(m_rate) * 120 / 1000 * m_channels);   // the biggest Opus frame (120 ms)
    m_pendingPos = 0;
    m_queue.clear();
    m_ingestDropped = 0;
    {
        std::lock_guard<std::mutex> lk(m_statsMutex);
        m_statsCopy = JitterBuffer::Stats();
        m_depthCopy = 0;
        m_frameMsCopy = 0;
    }
    m_formatDropped = 0;

    // 10ms device periods: the real buffering is done by the jitter buffer, not by the device
    std::string err;
    bool ok = m_output.start(m_rate, m_channels, 10, [this](int16_t *out, uint32_t frames){ onDeviceData(out, frames); }, &err);
    if(!ok)
        AUDIO_WARN("player: %s", err.c_str());
    else{
        m_ready = true;
        AUDIO_INFO("player started: %d Hz, %d ch", m_rate, m_channels);
    }
}

// Runs on the audio thread, exactly when the device needs data.
// Whatever the jitter buffer can't deliver (prefilling, DTX silence) is played as silence.
void clsAudioPlayer::onDeviceData(int16_t *out, uint32_t frames)
{
    const size_t need = size_t(frames) * m_channels;
    size_t done = 0;

    adoptNextStream();
    if(m_cur){
        // datagrams that arrived since the last callback (no lock: see PacketQueue). Only packets of the current
        // format go in; if the network thread has just prepared a new format, switch to it first.
        m_queue.drain([this](const uint8_t *d, size_t n){
            uint16_t seq; bool marker; uint8_t tx; const uint8_t *opus; size_t opusSize;
            opusCodec::PacketInfo info;
            if(!VoicePacket::parseHeader(d, n, seq, marker, tx, opus, opusSize) || !opusCodec::Inspect(opus, opusSize, info)){
                m_formatDropped.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            if(info.frameMs != m_cur->frameMs)
                adoptNextStream();
            if(info.frameMs != m_cur->frameMs){
                m_formatDropped.fetch_add(1, std::memory_order_relaxed);    // left over from the previous format
                return;
            }
            m_cur->jitter->Push(d, n);
        });

        while(done < need){
            if(m_pendingPos >= m_pending.size()){
                m_cur->jitter->PopFrame(m_pending);     // may be real audio, FEC or PLC
                m_pendingPos = 0;
                if(m_pending.empty())
                    break;
            }
            const size_t n = std::min(need - done, m_pending.size() - m_pendingPos);
            std::memcpy(out + done, m_pending.data() + m_pendingPos, n * sizeof(int16_t));
            m_pendingPos += n;
            done += n;
        }
        publishStats();
    }

    if(done < need)
        std::memset(out + done, 0, (need - done) * sizeof(int16_t));

    m_level.feed(out, frames, m_rate, m_channels);
    applyVolume(out, frames);

    // the canceller must see EXACTLY what goes to the speaker, silence included.
    if(m_aec)
        m_aec->render(out, frames);
}

void clsAudioPlayer::SetVolume(int percent)
{
    m_volume.store(percent < 0 ? 0 : (percent > 100 ? 100 : percent), std::memory_order_relaxed);
}

// audio thread: scale what goes to the speaker. The gain moves linearly from the previous block's value to the new one
// across this block, so a slider move never clicks. Unity (100) is a no-op.
void clsAudioPlayer::applyVolume(int16_t *out, uint32_t frames)
{
    const float v = float(m_volume.load(std::memory_order_relaxed)) / 100.f;
    const float target = v * v;
    const float start = m_gain;
    m_gain = target;
    if(start == 1.f && target == 1.f)
        return;

    const int ch = m_channels;
    const float step = (target - start) / float(frames);
    float g = start;
    for(uint32_t f = 0; f < frames; f++){
        g += step;
        for(int c = 0; c < ch; c++){
            int16_t &x = out[size_t(f) * ch + c];
            x = int16_t(float(x) * g);
        }
    }
}

// audio thread: copy the numbers for the GUI. If the GUI is reading right now, skip, the next callback (10 ms) tries again.
void clsAudioPlayer::publishStats()
{
    std::unique_lock<std::mutex> lk(m_statsMutex, std::try_to_lock);
    if(!lk.owns_lock())
        return;
    m_statsCopy = m_cur->jitter->stats();
    m_statsCopy.ingestDropped = m_ingestDropped.load(std::memory_order_relaxed);
    m_statsCopy.formatDropped = m_formatDropped.load(std::memory_order_relaxed);
    m_depthCopy = m_cur->jitter->targetDepth();
    m_frameMsCopy = m_cur->frameMs;
}

void clsAudioPlayer::Play(const uint8_t *datagram, size_t size)
{
    if(!m_ready.load(std::memory_order_acquire))
        return;

    // what is this packet? (audio header, then the Opus packet whose first byte tells its duration)
    uint16_t seq; bool marker; uint8_t tx; const uint8_t *opus; size_t opusSize;
    opusCodec::PacketInfo info;
    if(!VoicePacket::parseHeader(datagram, size, seq, marker, tx, opus, opusSize) || !opusCodec::Inspect(opus, opusSize, info)){
        m_formatDropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    {
        std::lock_guard<std::mutex> lk(m_netMutex);
        reapRetired();

        if(info.frameMs != m_netFrameMs){
            // A different frame duration. The first packet ever switches at once; after that it takes 3 packets
            // in a row, so one stray packet (or two senders mixed up) can't make the buffer restart over and over.
            if(m_netFrameMs != 0){
                if(m_candidateMs != info.frameMs){
                    m_candidateMs = info.frameMs;
                    m_candidateCount = 0;
                }
                if(++m_candidateCount < 3){
                    m_formatDropped.fetch_add(1, std::memory_order_relaxed);
                    return;
                }
            }

            // new decoder + jitter buffer for this format, built HERE (allocates), adopted by the audio thread
            std::unique_ptr<Stream> st(new Stream);
            if(!st->codec.InitDecoder(m_rate, m_channels)){
                m_formatDropped.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            st->frameMs = info.frameMs;
            st->jitter.reset(new JitterBuffer(&st->codec, m_rate, m_channels, info.frameMs));
            delete m_next.exchange(st.release(), std::memory_order_acq_rel);    // an older one the audio thread never took
            m_netFrameMs = info.frameMs;
            m_candidateMs = 0;
            m_candidateCount = 0;
            AUDIO_INFO("player: stream format %d ms frames, %d ch, bandwidth %d", info.frameMs, info.channels, info.bandwidth);
        }
        m_candidateCount = 0;
    }

    if(!m_queue.push(datagram, size))
        m_ingestDropped.fetch_add(1, std::memory_order_relaxed);
}

bool clsAudioPlayer::GetStats(JitterBuffer::Stats &stats, int &bufferFrames, int &frameMs) const
{
    if(!m_ready.load(std::memory_order_acquire) || !m_pConfig)
        return false;

    std::lock_guard<std::mutex> lk(m_statsMutex);
    if(!m_frameMsCopy)
        return false;                   // no stream yet: nothing has been received
    stats = m_statsCopy;
    bufferFrames = m_depthCopy;
    frameMs = m_frameMsCopy;
    return true;
}
