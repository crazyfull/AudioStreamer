#include "clsAudioPlayer.h"
#include "opusCodec.h"
#include <algorithm>
#include <cstring>

clsAudioPlayer::clsAudioPlayer()
{
    m_pConfig = nullptr;
    m_pendingPos = 0;
    m_rate = 48000;
    m_channels = 1;
}

clsAudioPlayer::~clsAudioPlayer()
{
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

    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_pOpus = std::move(codec);
        m_pJitter = std::make_unique<JitterBuffer>(m_pOpus.get(), m_rate, m_channels, m_pConfig->frameMs);
        m_pending.clear();
        m_pendingPos = 0;
    }

    // 10ms device periods: the real buffering is done by the jitter buffer, not by the device
    std::string err;
    bool ok = m_output.start(m_rate, m_channels, 10, [this](int16_t *out, uint32_t frames){ onDeviceData(out, frames); }, &err);
    if(!ok)
        AUDIO_WARN("player: %s", err.c_str());
    else
        AUDIO_INFO("player started: %d Hz, %d ch", m_rate, m_channels);
}

// Runs on the audio thread, exactly when the device needs data.
// Whatever the jitter buffer can't deliver (prefilling, DTX silence) is played as silence.
void clsAudioPlayer::onDeviceData(int16_t *out, uint32_t frames)
{
    size_t need = size_t(frames) * m_channels;
    size_t done = 0;

    {
        std::lock_guard<std::mutex> lk(m_mutex);
        while(done < need && m_pJitter){
            if(m_pendingPos >= m_pending.size()){
                m_pending = m_pJitter->PopFrame();     // may be real audio, FEC or PLC
                m_pendingPos = 0;
                if(m_pending.empty())
                    break;
            }
            size_t n = std::min(need - done, m_pending.size() - m_pendingPos);
            std::memcpy(out + done, m_pending.data() + m_pendingPos, n * sizeof(int16_t));
            m_pendingPos += n;
            done += n;
        }
    }

    if(done < need)
        std::memset(out + done, 0, (need - done) * sizeof(int16_t));

    // the canceller must see EXACTLY what goes to the speaker, silence included.
    // Outside the lock: it must not delay the network thread that calls Play().
    if(m_aec)
        m_aec->render(out, frames);
}

void clsAudioPlayer::Play(const uint8_t *datagram, size_t size)
{
    std::lock_guard<std::mutex> lk(m_mutex);
    if(!m_pJitter)
        return;
    m_pJitter->Push(datagram, size);
}

bool clsAudioPlayer::GetStats(JitterBuffer::Stats &stats, int &bufferFrames, int &frameMs) const
{
    std::lock_guard<std::mutex> lk(m_mutex);
    if(!m_pJitter || !m_pConfig)
        return false;

    stats = m_pJitter->stats();
    bufferFrames = m_pJitter->targetDepth();
    frameMs = m_pConfig->frameMs;
    return true;
}
