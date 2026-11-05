/*
 * RavAva - real-time voice library (Opus over any datagram transport)
 * Copyright (c) 2026 Mojtaba Kahvand. All rights reserved.
 * Author: Mojtaba Kahvand
 *
 * Proprietary software, NOT free / open source. Use, copying, modification and distribution are forbidden without a
 * written license from the author. See the LICENSE file in the repository root.
 * SPDX-License-Identifier: LicenseRef-RavAva-Proprietary
 */
#ifndef RAVAVA_RECORDER_H
#define RAVAVA_RECORDER_H

#include <atomic>
#include <mutex>
#include <functional>
#include <memory>
#include "AudioTypes.h"
#include "AudioConfig.h"
#include "AudioDevice.h"
#include "OpusCodec.h"
#include "AudioProcessor.h"
#include "DeviceWatchdog.h"
#include "MusicPlayer.h"
#include "BlockCutter.h"
#include "GainRamp.h"
#include "LevelMeter.h"
#include "DeviceSwitcher.h"

namespace ravava {

// Capture side: microphone (or a music file) -> opus packets. No Qt.
// The packet handler is called on the AUDIO THREAD, once per datagram:
// 3 byte audio header + opus packet (see VoicePacket.h); the network layer puts it into a UDP packet (net/Packet.h). Keep it short; hand the data over to your
// own thread (the Qt GUI uses a queued invoke).
class Recorder
{
public:
    using PacketHandler = std::function<void(const Bytes &datagram)>;

private:
    AudioConfig *m_pConfig;           // only read by SetConfig() and Start(); after Start() returns the recorder works on its own copy m_cfg
    AudioConfig m_cfg;                // snapshot taken by Start(): the caller's AudioConfig may change or be destroyed afterwards
    std::unique_ptr<OpusCodec> m_pOpus;
    PacketHandler m_handler;
    std::shared_ptr<AudioProcessor> m_aec;   // shared with Player (it feeds the speaker signal)

    // Audio thread rule: after Start() nothing below allocates. Everything is sized once in Start()
    // (frames are cut straight out of the device buffer, only the < 1 frame tail is kept).
    Pcm m_cache;                      // tail of mic samples that don't fill a whole opus frame yet (< 1 frame)
    Pcm m_scratch;                    // echo canceller output / music samples for the current device chunk
    LevelMeter m_level;               // loudness of what is sent (audio thread writes)
    Pcm m_gained;                     // mic chunk after the input gain (only used while the gain is not 100 %)
    GainRamp m_inputGain;             // microphone gain 0..200 %, GUI -> audio thread (see GainRamp.h)
    Bytes m_encoded;                  // one opus packet
    Bytes m_datagram;                 // header + opus packet, handed to the packet handler
    uint16_t m_seq;                   // +1 per frame (also for frames skipped by DTX)
    uint8_t m_tx;                     // counts transmitted packets (loss statistics on the receiver)
    bool m_marker;                    // next packet follows a silence gap

    AudioFileReader m_file;           // music file of the playMusic config mode, decoded on a background thread

    // music mixed into the stream at run time: its own class (MusicPlayer), shared with the Player for the local copy
    std::shared_ptr<MusicPlayer> m_musicPlayer;
    std::atomic<bool> m_micOn{true};          // SetMicEnabled(): the "talk" gate
    bool m_idle = false;                      // audio thread: this chunk has neither microphone nor music: nothing is encoded or sent

    // the chosen device id: written by the GUI thread (SetXDevice / Start) and read by the watchdog thread (KeepDevice),
    // so it is only touched under its own mutex (a std::string read while another thread assigns it can crash)
    std::string m_deviceChoice;
    mutable std::mutex m_choiceMutex;
    void setChoice(const std::string &id) { std::lock_guard<std::mutex> lk(m_choiceMutex); m_deviceChoice = id; }
    std::string choice() const { std::lock_guard<std::mutex> lk(m_choiceMutex); return m_deviceChoice; }
    std::mutex m_keepMutex;           // KeepDevice() runs on the GUI thread AND on the watchdog
    std::atomic<bool> m_autoKeep{true};
    DeviceWatchdog m_watchdog;
    int64_t m_lastKeepMs = 0;         // GUI thread: when KeepDevice() last re-opened the device
    int m_keepIntervalMs = 3000;      // GUI thread
    std::atomic<int> m_failedOpens{0};      // consecutive failed (re)opens of the microphone
    std::atomic<bool> m_deviceWanted{false};  // recording is on (set by Start, cleared by Stop): SetInputDevice() re-opens the mic
    DeviceSwitcher m_switcher;        // moves the microphone to another device off the GUI thread
    AudioInput m_input;               // declared last: destroyed first

    void init();
    void onCapture(const int16_t *pcm, uint32_t frames);   // audio thread
    void onFrame(const int16_t *frame);                     // audio thread: exactly one opus frame

public:
    Recorder();
    ~Recorder();
    void SetConfig(AudioConfig *pConfig);
    void SetPacketHandler(PacketHandler handler) { m_handler = std::move(handler); }   // call before Start()
    // optional echo canceller, the SAME instance as the player's. Call before Start(). Ignored while playing music.
    // Ignored (with a warning) once recording has started: the audio thread is already using it.
    void SetAudioProcessor(std::shared_ptr<AudioProcessor> aec);
    // microphone gain in percent: 0 = mute, 100 = unchanged (default), up to 200 = +12 dB (clips instead of wrapping).
    // Any thread, takes effect within ~10 ms without clicks. Applied to the raw mic signal BEFORE the echo canceller /
    // noise suppression / AGC (like an analog mic gain). Not used while playing a music file.
    // loudness of the audio that is SENT (after gain / echo cancel, 0..100, see LevelMeter.h). Any thread, lock-free.
    int GetLevel() const { return m_level.Level(); }
    int TakePeakLevel() { return m_level.TakePeak(); }
    void SetInputGain(int percent) { m_inputGain.Set(percent); }
    int  GetInputGain() const { return m_inputGain.Get(); }
    static int MaxInputGain() { return 200; }
    void Start();
    void Stop();
    // playMusic (config) mode only: why the music file could not be opened / decoded ("" = fine), and whether it was played to the end.
    // Any thread. Without a readable file the recorder sends silence. (For Music() use its own Error() / GetState().)
    std::string MusicError() const { return m_file.Error(); }
    bool IsMusicFinished() const { return m_file.IsFinished(); }
    // ---- music ---------------------------------------------------------------------------------------------------------------------
    // A file (wav / mp3 / flac) MIXED into the stream together with the microphone, so the speaker can talk over it: Play / Pause / Resume /
    // Stop as separate calls, a volume for what is sent and a separate one for the local copy. See MusicPlayer.h. The recorder must be started
    // (the microphone is the clock) and the config's playMusic must be off (that mode is "music INSTEAD of the microphone", fixed at Start).
    // For the local copy give the same object to the player: Player::SetMusicMonitor(recorder.Music()).
    std::shared_ptr<MusicPlayer> Music() const { return m_musicPlayer; }
    // The "talk" gate. false = the microphone is NOT sent (and not processed); music keeps going. When neither the microphone nor music is
    // active nothing is sent at all (a pause for the receiver, like DTX), so a push-to-talk button should just call this and send every packet.
    // Default true: the microphone is always sent (the old behaviour). Any thread.
    void SetMicEnabled(bool on) { m_micOn.store(on, std::memory_order_relaxed); }
    bool IsMicEnabled() const { return m_micOn.load(std::memory_order_relaxed); }
    // Switch the microphone while recording (no Stop / Start needed). "" = system default. The switch runs on a worker
    // thread, so this returns at once and never freezes the GUI; several quick calls are merged (the last one wins). It is
    // a short gap in the stream (longer for Bluetooth); the next packet carries the "marker" flag. If the device can't be
    // opened the default one is used, if that fails too recording stays silent (logged). Not recording: just remembered,
    // Start() uses the config. GUI thread only. Returns false only if there is no config.
    bool SetInputDevice(const std::string &deviceId);
    // Call about once a second from the GUI thread. It repairs the microphone: if the system moved the stream to another
    // device than the chosen one (Android: a Bluetooth connection...), or the microphone stopped (a re-open failed, a phone
    // call took the device) the chosen device is opened again. At most once per 3 s; after two failed attempts in a row it
    // falls back to the system default device (the choice in the UI is not touched).
    void KeepDevice();
    void SetKeepIntervalMs(int ms) { m_keepIntervalMs = ms; }       // (tests)
    // By default the recorder runs KeepDevice() itself about once a second while it is started (own thread), so the 1 s timer in the app
    // is no longer needed (calling KeepDevice() yourself as well is harmless). false = only your calls repair the device. Any thread.
    void SetAutoKeepDevice(bool on) { m_autoKeep.store(on, std::memory_order_relaxed); }
    // the microphone that is REALLY in use (see AudioInput::ActiveDevice), "" = unknown. GUI thread.
    std::string ActiveInputDevice() const { return m_input.ActiveDevice(); }
private:
    bool startDevice();
    void requestOpen(const std::string &deviceId);   // re-opens the microphone on the device worker
};

} // namespace ravava

#endif // RAVAVA_RECORDER_H
