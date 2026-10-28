#include "clsAudioPlayer.h"
#include "opusCodec.h"
#include <algorithm>
#include <cstring>

clsAudioPlayer::clsAudioPlayer()
{
    m_pConfig = nullptr;
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
    m_output.stop();        // after this the audio callback can no longer touch the jitter buffer
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

    auto codec = std::make_unique<opusCodec>();
    if(!codec->InitDecoder(m_rate, m_channels))
        return;

    // the speaker is not running yet, so nothing else touches these
    m_pOpus = std::move(codec);
    m_pJitter = std::make_unique<JitterBuffer>(m_pOpus.get(), m_rate, m_channels, m_pConfig->frameMs);
    m_pending.clear();
    m_pending.reserve(size_t(m_rate) * 120 / 1000 * m_channels);   // the biggest Opus frame (120 ms)
    m_pendingPos = 0;
    m_queue.clear();
    m_ingestDropped = 0;
    {
        std::lock_guard<std::mutex> lk(m_statsMutex);
        m_statsCopy = JitterBuffer::Stats();
        m_depthCopy = m_pJitter->targetDepth();
    }

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

    if(m_pJitter){
        // datagrams that arrived since the last callback (no lock: see PacketQueue)
        m_queue.drain([this](const uint8_t *d, size_t n){ m_pJitter->Push(d, n); });

        while(done < need){
            if(m_pendingPos >= m_pending.size()){
                m_pJitter->PopFrame(m_pending);     // may be real audio, FEC or PLC
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
    m_statsCopy = m_pJitter->stats();
    m_statsCopy.ingestDropped = m_ingestDropped.load(std::memory_order_relaxed);
    m_depthCopy = m_pJitter->targetDepth();
}

void clsAudioPlayer::Play(const uint8_t *datagram, size_t size)
{
    if(!m_ready.load(std::memory_order_acquire))
        return;
    if(!m_queue.push(datagram, size))
        m_ingestDropped.fetch_add(1, std::memory_order_relaxed);
}

bool clsAudioPlayer::GetStats(JitterBuffer::Stats &stats, int &bufferFrames, int &frameMs) const
{
    if(!m_ready.load(std::memory_order_acquire) || !m_pConfig)
        return false;

    std::lock_guard<std::mutex> lk(m_statsMutex);
    stats = m_statsCopy;
    bufferFrames = m_depthCopy;
    frameMs = m_pConfig->frameMs;
    return true;
}
