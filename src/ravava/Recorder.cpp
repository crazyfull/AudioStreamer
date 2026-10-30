#include "Recorder.h"
#include "VoicePacket.h"
#include <algorithm>
#include <chrono>

namespace ravava {

Recorder::Recorder() : m_inputGain(200)
{
    m_pConfig = nullptr;
    m_seq = 0;
    m_tx = 0;
    m_marker = true;
    m_musicPos = 0;
}

Recorder::~Recorder()
{
    m_switcher.Cancel();    // no device switch may run any more
    m_input.Stop();
}

void Recorder::SetConfig(AudioConfig *pConfig)
{
    m_pConfig = pConfig;
    init();
}

void Recorder::init()
{
    if(m_pOpus || !m_pConfig)
        return;

    auto codec = std::make_unique<OpusCodec>();
    if(!codec->InitEncoder(*m_pConfig)){
        RAVAVA_WARN("Opus encoder init failed");
        return;
    }
    m_pOpus = std::move(codec);

    if(m_pConfig->playMusic)
        loadMusic();
}

// The whole file is decoded once, up front (the old code also buffered all of it), so the
// real-time capture callback never touches the disk.
bool Recorder::loadMusic()
{
    std::string err;
    m_music = DecodeAudioFile(m_pConfig->musicPath, m_pConfig->inputRate, m_pConfig->channels, &err);
    m_musicPos = 0;
    if(m_music.empty()){
        RAVAVA_WARN("music: %s", err.c_str());
        return false;
    }
    RAVAVA_INFO("music loaded: %s (%zu samples)", m_pConfig->musicPath.c_str(), m_music.size());
    return true;
}

void Recorder::Start()
{
    if(!m_pOpus || !m_pConfig){
        RAVAVA_WARN("Recorder not ready");
        return;
    }
    if(m_deviceWanted || m_input.IsRunning())
        return;

    const size_t frameChunk = size_t(m_pConfig->FrameSamples()) * m_pConfig->channels;
    m_cache.clear();
    m_cache.reserve(frameChunk);                    // holds the < 1 frame tail
    // device chunks are 10 ms; 100 ms of room means even odd driver chunk sizes never reallocate
    m_scratch.clear();
    m_scratch.reserve(size_t(m_pConfig->inputRate) / 10 * m_pConfig->channels + frameChunk);
    m_gained.clear();
    m_gained.reserve(size_t(m_pConfig->inputRate) / 10 * m_pConfig->channels + frameChunk);
    m_encoded.reserve(1500);
    m_datagram.reserve(1500 + VoicePacket::HeaderSize);
    m_marker = true;
    m_musicPos = 0;

    // 10ms capture periods; they are re-cut into exact opus frames in onCapture()
    m_input.SetDevice(m_pConfig->inputDeviceId);
    m_deviceChoice = m_pConfig->inputDeviceId;
    m_deviceWanted = true;
    startDevice();
}

bool Recorder::startDevice()
{
    std::string err;
    const bool ok = m_input.Start(m_pConfig->inputRate, m_pConfig->channels, 10,  [this](const int16_t *pcm, uint32_t frames){ onCapture(pcm, frames); }, &err);
    if(!ok)
        RAVAVA_WARN("Recorder: %s", err.c_str());
    return ok;
}

// GUI thread. Only the microphone device is replaced: sequence numbers, the encoder and the echo canceller continue.
bool Recorder::SetInputDevice(const std::string &deviceId)
{
    m_deviceChoice = deviceId;
    if(!m_deviceWanted){
        m_input.SetDevice(deviceId);    // not recording: Start() opens it
        return true;
    }
    m_switcher.Request(deviceId, [this](const std::string &id){
        m_input.Stop();                 // returns when the capture callback is guaranteed to be finished
        m_cache.clear();                // the part of a frame captured from the old device is dropped
        m_marker = true;                // the next packet follows a gap: the receiver re-syncs its buffer
        m_input.SetDevice(id);
        startDevice();                  // failure is logged
    });
    return true;
}

void Recorder::KeepDevice()
{
    if(!m_deviceWanted || !m_input.DeviceMoved())
        return;
    const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    if(now - m_lastKeepMs < 3000)
        return;                         // not in a loop: at most one re-open per 3 s
    m_lastKeepMs = now;
    RAVAVA_WARN("recorder: re-opening the chosen microphone");
    SetInputDevice(m_deviceChoice);
}

void Recorder::Stop()
{
    m_deviceWanted = false;
    m_switcher.Cancel();    // a switch in progress finishes first, then nothing restarts the mic
    m_input.Stop();         // returns when the callback is guaranteed to be finished
    m_cache.clear();
    m_scratch.clear();
    m_gained.clear();
}

// audio thread
void Recorder::onCapture(const int16_t *pcm, uint32_t frames)
{
    if(!m_pOpus || !m_pConfig)
        return;

    const size_t count = size_t(frames) * m_pConfig->channels;
    const int16_t *src = pcm;       // what goes to the encoder for this device chunk
    size_t n = count;

    if(m_pConfig->playMusic){
        // the mic only acts as a clock: the same amount of samples is taken from the music file
        const size_t avail = m_musicPos < m_music.size() ? m_music.size() - m_musicPos : 0;
        const size_t take = std::min(count, avail);
        m_scratch.resize(count);
        std::copy(m_music.begin() + m_musicPos, m_music.begin() + m_musicPos + take, m_scratch.begin());
        std::fill(m_scratch.begin() + take, m_scratch.end(), int16_t(0));   // file ended -> silence
        m_musicPos += take;
        src = m_scratch.data();
    }else{
        // microphone gain first, on a copy (the device buffer is read-only); at 100 % nothing is copied
        if(m_inputGain.Active()){
            m_gained.assign(pcm, pcm + count);
            m_inputGain.Process(m_gained.data(), frames, m_pConfig->channels);
            pcm = m_gained.data();
            src = pcm;
        }
        if(m_aec && m_aec->IsReady()){
            // echo-free samples (less than 10 ms can stay inside the canceller)
            m_scratch.clear();
            m_aec->Capture(pcm, frames, m_scratch);
            src = m_scratch.data();
            n = m_scratch.size();
        }
    }

    // opus needs EXACT frames (e.g. 20ms), device chunks have arbitrary sizes -> cut them
    const size_t chunk = size_t(m_pConfig->FrameSamples()) * m_pConfig->channels;
    CutBlocks(m_cache, chunk, src, n, [this](const int16_t *frame){ onFrame(frame); });
}

// audio thread: one frame -> (maybe) one datagram
void Recorder::onFrame(const int16_t *frame)
{
    const uint16_t seq = m_seq++;     // every frame consumes a number, even silent ones

    if(!m_pOpus->Encode(frame, m_pConfig->FrameSamples(), m_encoded)){
        m_marker = true;
        return;
    }

    // DTX: 1-2 byte packets mean "silence", nothing to transmit.
    // The next real packet gets the marker so the receiver knows it was silence, not loss.
    if(m_pConfig->useDtx && m_encoded.size() <= 2){
        m_marker = true;
        return;
    }

    VoicePacket::WrapInto(m_datagram, seq, m_marker, m_tx++, m_encoded);
    m_marker = false;
    if(m_handler)
        m_handler(m_datagram);
}

} // namespace ravava
