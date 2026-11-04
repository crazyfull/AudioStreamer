/*
 * RavAva - real-time voice library (Opus over any datagram transport)
 * Copyright (c) 2026 Mojtaba Kahvand. All rights reserved.
 * Author: Mojtaba Kahvand
 *
 * Proprietary software, NOT free / open source. Use, copying, modification and distribution are forbidden without a
 * written license from the author. See the LICENSE file in the repository root.
 * SPDX-License-Identifier: LicenseRef-RavAva-Proprietary
 */
#include "Recorder.h"
#include "VoicePacket.h"
#include <algorithm>
#include <chrono>
#include <thread>

namespace ravava {

Recorder::Recorder() : m_inputGain(200), m_musicPlayer(std::make_shared<MusicPlayer>())
{
    m_pConfig = nullptr;
    m_seq = 0;
    m_tx = 0;
    m_marker = true;
}

Recorder::~Recorder()
{
    m_watchdog.Stop();
    m_switcher.Cancel();    // no device switch may run any more
    m_input.Stop();
}

void Recorder::SetAudioProcessor(std::shared_ptr<AudioProcessor> aec)
{
    if(m_deviceWanted){
        RAVAVA_WARN("recorder: SetAudioProcessor() after Start() is ignored (call it before Start())");
        return;
    }
    m_aec = std::move(aec);
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
}

void Recorder::Start()
{
    if(!m_pOpus || !m_pConfig){
        RAVAVA_WARN("Recorder not ready");
        return;
    }
    if(m_deviceWanted || m_input.IsRunning())
        return;
    m_cfg = *m_pConfig;                 // from here on only the copy is used (also by the audio thread)

    const size_t frameChunk = size_t(m_cfg.FrameSamples()) * m_cfg.channels;
    m_cache.clear();
    m_cache.reserve(frameChunk);                    // holds the < 1 frame tail
    // device chunks are 10 ms; 100 ms of room means even odd driver chunk sizes never reallocate
    m_scratch.clear();
    m_scratch.reserve(size_t(m_cfg.inputRate) / 10 * m_cfg.channels + frameChunk);
    m_gained.clear();
    m_gained.reserve(size_t(m_cfg.inputRate) / 10 * m_cfg.channels + frameChunk);
    if(!m_cfg.playMusic)
        m_musicPlayer->Prepare(m_cfg.inputRate, m_cfg.channels);    // Music()->Play() works from now on
    m_idle = false;
    m_encoded.reserve(1500);
    m_datagram.reserve(1500 + VoicePacket::HeaderSize);
    m_marker = true;
    const auto t0 = std::chrono::steady_clock::now();
    if(m_cfg.playMusic)
        m_file.Open(m_cfg.musicPath, m_cfg.inputRate, m_cfg.channels);    // from the beginning, decoded in the background

    // 10ms capture periods; they are re-cut into exact opus frames in onCapture()
    m_input.SetDevice(m_cfg.inputDeviceId);
    m_deviceChoice = m_cfg.inputDeviceId;
    m_deviceWanted = true;
    m_failedOpens = 0;
    m_switching = true;
    requestOpen(m_deviceChoice);        // on the device worker: opening the microphone can take seconds (Android)
    m_watchdog.Start([this]{ if(m_autoKeep.load(std::memory_order_relaxed)) KeepDevice(); });
    RAVAVA_INFO("recorder: Start() took %d ms%s", int(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count()),
                m_cfg.playMusic ? " (music file is opened in the background)" : "");
}

bool Recorder::startDevice()
{
    std::string err;
    const bool ok = m_input.Start(m_cfg.inputRate, m_cfg.channels, 10,  [this](const int16_t *pcm, uint32_t frames){ onCapture(pcm, frames); }, &err);
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
    requestOpen(deviceId);
    return true;
}

void Recorder::requestOpen(const std::string &deviceId)
{
    m_switcher.Request(deviceId, [this](const std::string &id){
        m_switching = true;
        m_input.Stop();                 // returns when the capture callback is guaranteed to be finished
        m_cache.clear();                // the part of a frame captured from the old device is dropped
        m_marker = true;                // the next packet follows a gap: the receiver re-syncs its buffer
        m_input.SetDevice(id);
        const bool ok = startDevice();  // failure is logged
        m_failedOpens = ok ? 0 : m_failedOpens + 1;
        m_switching = false;
    });
}

void Recorder::KeepDevice()
{
    std::lock_guard<std::mutex> lk(m_keepMutex);        // GUI thread and watchdog
    if(!m_deviceWanted || m_switching)
        return;
    const bool running = m_input.IsRunning();
    if(running && !m_input.DeviceMoved())
        return;                         // all is well
    const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    if(now - m_lastKeepMs < m_keepIntervalMs)
        return;                         // not in a loop: at most one re-open per interval
    m_lastKeepMs = now;

    std::string id = m_deviceChoice;
    if(!running && m_failedOpens >= 2){
        id.clear();
        RAVAVA_WARN("recorder: the chosen microphone can't be opened, using the system default");
    }else if(!running){
        RAVAVA_WARN("recorder: the microphone is not running, opening it again");
    }else{
        RAVAVA_WARN("recorder: re-opening the chosen microphone");
    }
    requestOpen(id);
}

void Recorder::Stop()
{
    m_watchdog.Stop();      // first: it must not re-open the microphone while we are stopping
    m_deviceWanted = false;
    m_switcher.Cancel();    // a switch in progress finishes first, then nothing restarts the mic
    m_switching = false;
    m_input.Stop();         // returns when the callback is guaranteed to be finished
    m_musicPlayer->Release();
    m_file.Close();
    m_cache.clear();
    m_scratch.clear();
    m_gained.clear();
}

// audio thread
void Recorder::onCapture(const int16_t *pcm, uint32_t frames)
{
    if(!m_pOpus)
        return;

    const size_t count = size_t(frames) * m_cfg.channels;
    const int16_t *src = pcm;       // what goes to the encoder for this device chunk
    size_t n = count;

    if(m_cfg.playMusic){
        // the mic only acts as a clock: the same amount of samples is taken from the music file
        // (the file is decoded on a background thread, this only copies ready samples)
        m_scratch.resize(count);
        const size_t take = m_file.Read(m_scratch.data(), count);
        std::fill(m_scratch.begin() + take, m_scratch.end(), int16_t(0));   // still buffering / file ended -> silence
        src = m_scratch.data();
    }else{
        const bool micOn = m_micOn.load(std::memory_order_relaxed);
        if(!micOn){
            // "talk" is off: the microphone is not sent, so it is not processed either (no gain, no echo canceller)
            m_scratch.assign(count, int16_t(0));
            src = m_scratch.data();
        }else{
            // microphone gain first, on a copy (the device buffer is read-only); at 100 % nothing is copied
            if(m_inputGain.Active()){
                m_gained.assign(pcm, pcm + count);
                m_inputGain.Process(m_gained.data(), frames, m_cfg.channels);
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

        // music from Music()->Play(): mixed into whatever the microphone gives (saturating add, no wrap-around)
        bool musicOn = false;
        src = m_musicPlayer->Mix(src, n, musicOn);
        m_idle = !micOn && !musicOn;
    }

    m_level.Feed(src, uint32_t(n / size_t(m_cfg.channels)), int(m_cfg.inputRate), int(m_cfg.channels));

    // opus needs EXACT frames (e.g. 20ms), device chunks have arbitrary sizes -> cut them
    const size_t chunk = size_t(m_cfg.FrameSamples()) * m_cfg.channels;
    CutBlocks(m_cache, chunk, src, n, [this](const int16_t *frame){ onFrame(frame); });
}

// audio thread: one frame -> (maybe) one datagram
void Recorder::onFrame(const int16_t *frame)
{
    const uint16_t seq = m_seq++;     // every frame consumes a number, even silent ones

    if(m_idle){                       // no microphone (talk off) and no music: nothing to send, the receiver sees a pause (marker on the next packet)
        m_marker = true;
        return;
    }

    if(!m_pOpus->Encode(frame, m_cfg.FrameSamples(), m_encoded)){
        m_marker = true;
        return;
    }

    // DTX: 1-2 byte packets mean "silence", nothing to transmit.
    // The next real packet gets the marker so the receiver knows it was silence, not loss.
    if(m_cfg.useDtx && m_encoded.size() <= 2){
        m_marker = true;
        return;
    }

    VoicePacket::WrapInto(m_datagram, seq, m_marker, m_tx++, m_encoded);
    m_marker = false;
    if(m_handler)
        m_handler(m_datagram);
}

} // namespace ravava
