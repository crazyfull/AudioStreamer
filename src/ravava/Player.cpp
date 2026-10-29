#include "Player.h"
#include "OpusCodec.h"
#include "VoicePacket.h"
#include <algorithm>
#include <cstring>

namespace ravava {

Player::Player()
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
    m_deviceWanted = false;
    m_ingestDropped = 0;
    m_depthCopy = 0;
    m_pendingPos = 0;
    m_rate = 48000;
    m_channels = 1;
}

Player::~Player()
{
    m_ready = false;
    m_switcher.Cancel();    // no device switch may run any more
    m_output.Stop();        // after this the audio callback can no longer touch the streams
    delete m_cur;
    delete m_next.exchange(nullptr);
    reapRetired();
}

// network thread (or destructor): free streams the audio thread has given back
void Player::reapRetired()
{
    Stream *s = m_retired.exchange(nullptr, std::memory_order_acquire);
    while(s){
        Stream *n = s->retiredLink;
        delete s;
        s = n;
    }
}

// audio thread: switch to the stream the network thread prepared (no allocation, no free)
void Player::adoptNextStream()
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

void Player::SetConfig(AudioConfig *pConfig)
{
    m_pConfig = pConfig;
    init();
}

void Player::init()
{
    if(m_output.IsRunning() || !m_pConfig)
        return;

    // Opus decoder can output 8/12/16/24/48k. miniaudio converts rate/channels to what the
    // real speaker needs, so there is no "format not supported" fallback anymore.
    m_rate = m_pConfig->RenderRate();
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
    m_queue.Clear();
    m_ingestDropped = 0;
    {
        std::lock_guard<std::mutex> lk(m_statsMutex);
        m_statsCopy = JitterBuffer::Stats();
        m_depthCopy = 0;
        m_frameMsCopy = 0;
    }
    m_formatDropped = 0;

    // 10ms device periods: the real buffering is done by the jitter buffer, not by the device
    m_output.SetDevice(m_pConfig->outputDeviceId);
    m_deviceWanted = true;
    if(startDevice()){
        m_ready = true;
        RAVAVA_INFO("player started: %d Hz, %d ch", m_rate, m_channels);
    }
}

bool Player::startDevice()
{
    std::string err;
    const bool ok = m_output.Start(m_rate, m_channels, 10, [this](int16_t *out, uint32_t frames){ onDeviceData(out, frames); }, &err);
    if(!ok)
        RAVAVA_WARN("player: %s", err.c_str());
    return ok;
}

// GUI thread. Only the sound device is replaced; the streams, jitter buffer and counters stay as they are.
bool Player::SetOutputDevice(const std::string &deviceId)
{
    if(!m_deviceWanted){
        m_output.SetDevice(deviceId);   // not started yet: the next init() opens it
        return true;
    }
    m_switcher.Request(deviceId, [this](const std::string &id){
        m_output.Stop();                // returns when the audio callback is guaranteed to be finished
        m_output.SetDevice(id);
        m_pendingPos = m_pending.size();    // what was cut in half by the switch is dropped, the next callback pops a fresh frame
        startDevice();                  // packets that arrive meanwhile wait in the queue; failure is logged
    });
    return true;
}

// Runs on the audio thread, exactly when the device needs data.
// Whatever the jitter buffer can't deliver (prefilling, DTX silence) is played as silence.
void Player::onDeviceData(int16_t *out, uint32_t frames)
{
    const size_t need = size_t(frames) * m_channels;
    size_t done = 0;

    adoptNextStream();
    if(m_cur){
        // datagrams that arrived since the last callback (no lock: see PacketQueue). Only packets of the current
        // format go in; if the network thread has just prepared a new format, switch to it first.
        m_queue.Drain([this](const uint8_t *d, size_t n){
            uint16_t seq; bool marker; uint8_t tx; const uint8_t *opus; size_t opusSize;
            OpusCodec::PacketInfo info;
            if(!VoicePacket::ParseHeader(d, n, seq, marker, tx, opus, opusSize) || !OpusCodec::Inspect(opus, opusSize, info)){
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

    m_level.Feed(out, frames, m_rate, m_channels);
    m_volume.Process(out, frames, m_channels);

    // the canceller must see EXACTLY what goes to the speaker, silence included.
    if(m_aec)
        m_aec->Render(out, frames);
}

// audio thread: copy the numbers for the GUI. If the GUI is reading right now, skip, the next callback (10 ms) tries again.
void Player::publishStats()
{
    std::unique_lock<std::mutex> lk(m_statsMutex, std::try_to_lock);
    if(!lk.owns_lock())
        return;
    m_statsCopy = m_cur->jitter->GetStats();
    m_statsCopy.ingestDropped = m_ingestDropped.load(std::memory_order_relaxed);
    m_statsCopy.formatDropped = m_formatDropped.load(std::memory_order_relaxed);
    m_depthCopy = m_cur->jitter->TargetDepth();
    m_frameMsCopy = m_cur->frameMs;
}

void Player::Play(const uint8_t *datagram, size_t size)
{
    if(!m_ready.load(std::memory_order_acquire))
        return;

    // what is this packet? (audio header, then the Opus packet whose first byte tells its duration)
    uint16_t seq; bool marker; uint8_t tx; const uint8_t *opus; size_t opusSize;
    OpusCodec::PacketInfo info;
    if(!VoicePacket::ParseHeader(datagram, size, seq, marker, tx, opus, opusSize) || !OpusCodec::Inspect(opus, opusSize, info)){
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
            RAVAVA_INFO("player: stream format %d ms frames, %d ch, bandwidth %d", info.frameMs, info.channels, info.bandwidth);
        }
        m_candidateCount = 0;
    }

    if(!m_queue.Push(datagram, size))
        m_ingestDropped.fetch_add(1, std::memory_order_relaxed);
}

bool Player::GetStats(JitterBuffer::Stats &stats, int &bufferFrames, int &frameMs) const
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

} // namespace ravava
