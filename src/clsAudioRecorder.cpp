#include "clsAudioRecorder.h"
#include "VoicePacket.h"
#include <algorithm>

clsAudioRecorder::clsAudioRecorder()
{
    m_pConfig = nullptr;
    m_seq = 0;
    m_tx = 0;
    m_marker = true;
    m_musicPos = 0;
}

clsAudioRecorder::~clsAudioRecorder()
{
    m_input.stop();
}

void clsAudioRecorder::SetConfig(AudioConfig *pConfig)
{
    m_pConfig = pConfig;
    init();
}

void clsAudioRecorder::init()
{
    if(m_pOpus || !m_pConfig)
        return;

    auto codec = std::make_unique<opusCodec>();
    if(!codec->InitEncoder(*m_pConfig)){
        AUDIO_WARN("Opus encoder init failed");
        return;
    }
    m_pOpus = std::move(codec);

    if(m_pConfig->playMusic)
        LoadMusic();
}

// The whole file is decoded once, up front (the old code also buffered all of it), so the
// real-time capture callback never touches the disk.
bool clsAudioRecorder::LoadMusic()
{
    std::string err;
    m_music = decodeAudioFile(m_pConfig->musicPath, m_pConfig->inputRate, m_pConfig->channels, &err);
    m_musicPos = 0;
    if(m_music.empty()){
        AUDIO_WARN("music: %s", err.c_str());
        return false;
    }
    AUDIO_INFO("music loaded: %s (%zu samples)", m_pConfig->musicPath.c_str(), m_music.size());
    return true;
}

void clsAudioRecorder::Start()
{
    if(!m_pOpus || !m_pConfig){
        AUDIO_WARN("Recorder not ready");
        return;
    }
    if(m_input.isRunning())
        return;

    m_cache.clear();
    m_cache.reserve(size_t(m_pConfig->frameSamples()) * m_pConfig->channels * 2);
    m_marker = true;
    m_musicPos = 0;

    // 10ms capture periods; they are re-cut into exact opus frames in onCapture()
    std::string err;
    bool ok = m_input.start(m_pConfig->inputRate, m_pConfig->channels, 10,  [this](const int16_t *pcm, uint32_t frames){ onCapture(pcm, frames); }, &err);
    if(!ok)
        AUDIO_WARN("Recorder: %s", err.c_str());
}

void clsAudioRecorder::Stop()
{
    m_input.stop();         // returns when the callback is guaranteed to be finished
    m_cache.clear();
}

// audio thread
void clsAudioRecorder::onCapture(const int16_t *pcm, uint32_t frames)
{
    if(!m_pOpus || !m_pConfig)
        return;

    const size_t count = size_t(frames) * m_pConfig->channels;

    if(m_pConfig->playMusic){
        // the mic only acts as a clock: the same amount of samples is taken from the music file
        size_t avail = m_musicPos < m_music.size() ? m_music.size() - m_musicPos : 0;
        size_t n = std::min(count, avail);
        m_cache.insert(m_cache.end(), m_music.begin() + m_musicPos, m_music.begin() + m_musicPos + n);
        m_cache.insert(m_cache.end(), count - n, int16_t(0));   // file ended -> silence
        m_musicPos += n;
    }else{
        m_cache.insert(m_cache.end(), pcm, pcm + count);
    }

    // opus needs EXACT frames (e.g. 20ms), device chunks have arbitrary sizes -> cut them
    const int frameSamples = m_pConfig->frameSamples();
    const size_t chunk = size_t(frameSamples) * m_pConfig->channels;
    size_t off = 0;

    while(m_cache.size() - off >= chunk){
        Bytes encoded = m_pOpus->Encode(m_cache.data() + off, frameSamples);
        off += chunk;

        const uint16_t seq = m_seq++;     // every frame consumes a number, even silent ones

        if(encoded.empty()){
            m_marker = true;
            continue;
        }

        // DTX: 1-2 byte packets mean "silence", nothing to transmit.
        // The next real packet gets the marker so the receiver knows it was silence, not loss.
        if(m_pConfig->useDtx && encoded.size() <= 2){
            m_marker = true;
            continue;
        }

        Bytes datagram = VoicePacket::wrap(seq, m_marker, m_tx++, encoded);
        m_marker = false;
        if(m_handler)
            m_handler(datagram);
    }

    if(off)
        m_cache.erase(m_cache.begin(), m_cache.begin() + off);
}
