/*
 * RavAva - real-time voice library (Opus over any datagram transport)
 * Copyright (c) 2026 Mojtaba Kahvand. All rights reserved.
 * Author: Mojtaba Kahvand
 *
 * Proprietary software, NOT free / open source. Use, copying, modification and distribution are forbidden without a
 * written license from the author. See the LICENSE file in the repository root.
 * SPDX-License-Identifier: LicenseRef-RavAva-Proprietary
 */
#ifndef RAVAVA_PLAYER_H
#define RAVAVA_PLAYER_H

#include <atomic>
#include <memory>
#include <mutex>
#include "AudioTypes.h"
#include "AudioConfig.h"
#include "AudioDevice.h"
#include "JitterBuffer.h"
#include "GainRamp.h"
#include "LevelMeter.h"
#include "DeviceSwitcher.h"
#include "AudioProcessor.h"
#include "PacketQueue.h"
#include "DeviceWatchdog.h"
#include "MusicPlayer.h"

namespace ravava {

// Receive side: datagram -> JitterBuffer (reorder / FEC / PLC) -> opus decode -> speaker.
//
// The player needs NO description of the stream: the frame duration comes from the Opus packets themselves
// (first byte, see OpusCodec::Inspect). The sender may use any frame size / bitrate / bandwidth / input rate /
// mono or stereo, and may change it while talking; the decoder always outputs the speaker's own rate and channels
// (AudioConfig::renderRate() / channels), Opus converts. Only the speaker side of the config is used here.
// No Qt. Play() may be called from any thread; the speaker pulls frames from the audio thread.
//
// Threading: Play() only copies the datagram into a lock-free queue (PacketQueue). The audio thread
// drains that queue and is the ONLY user of the jitter buffer and the Opus decoder, so it never waits
// for the network/GUI thread (no lock shared with Play()), and Play() never waits for a decode.
class Player
{
    AudioConfig *m_pConfig;

    // Decoder + jitter buffer for ONE stream format (= frame duration). Built on the network thread when a packet with a
    // new duration shows up, handed to the audio thread through m_next, and the audio thread hands the old one
    // back through m_retired: nothing is ever allocated or freed on the audio thread.
    struct Stream {
        OpusCodec codec;
        std::unique_ptr<JitterBuffer> jitter;
        int frameMs = 0;
        bool adaptive = false;              // the jitter buffer gets arrival times
        Stream *retiredLink = nullptr;      // m_retired list
    };
    Stream *m_cur;                          // audio thread only: the stream being played (null until the first packet)
    std::atomic<Stream*> m_next;            // network thread -> audio thread: a new stream to switch to
    std::atomic<Stream*> m_retired;         // audio thread -> network thread: old streams, freed there

    // network side (Play() callers, serialized by m_netMutex; the audio thread never touches these)
    std::mutex m_netMutex;
    int m_netFrameMs;                       // frame duration the audio thread plays / is about to play (0 = none yet)
    int m_candidateMs;                      // a different duration that is currently being seen ...
    int m_candidateCount;                   // ... this many packets in a row
    std::shared_ptr<AudioProcessor> m_aec;   // shared with Recorder (we feed it what the speaker plays)
    std::shared_ptr<MusicPlayer> m_musicMon; // the recorder's music: its local copy is added to what the speaker plays (SetMusicMonitor)

    PacketQueue m_queue;                    // network/GUI thread(s) -> audio thread
    std::atomic<bool> m_ready;              // true once the speaker runs: Play() accepts datagrams
    std::atomic<uint32_t> m_ingestDropped;  // datagrams dropped because the queue was full
    std::atomic<bool> m_adaptive{false};     // see SetAdaptivePlayout()
    std::atomic<int> m_limMinMs{0}, m_limMaxMs{0};
    std::atomic<bool> m_dred{false};
    std::atomic<int> m_dredFloorMs{0};

    // stats for the GUI: the audio thread publishes a copy (never blocks: try_lock), GetStats() reads it
    mutable std::mutex m_statsMutex;
    JitterBuffer::Stats m_statsCopy;
    int m_depthCopy;

    GainRamp m_volume;              // 0..100 %, GUI -> audio thread (see GainRamp.h)
    LevelMeter m_level;             // loudness of what goes to the speaker (audio thread writes, anyone reads)

    Pcm m_pending;                  // decoded PCM of the current frame, not consumed by the device yet (audio thread)
    size_t m_pendingPos;
    int m_rate;
    int m_channels;

    std::string m_deviceChoice;             // the device id the user chose (GUI thread)
    std::mutex m_keepMutex;                 // KeepDevice() runs on the GUI thread AND on the watchdog
    std::atomic<bool> m_autoKeep{true};
    DeviceWatchdog m_watchdog;
    int64_t m_lastKeepMs = 0;               // GUI thread: when KeepDevice() last re-opened the device
    int m_keepIntervalMs = 3000;            // GUI thread
    std::atomic<bool> m_switching{false};   // the device worker is busy
    std::atomic<int> m_failedOpens{0};      // consecutive failed (re)opens of the speaker
    std::atomic<bool> m_deviceWanted;       // the speaker should be running (SetOutputDevice() then re-opens it)
    DeviceSwitcher m_switcher;              // moves the speaker to another device off the GUI thread
    AudioOutput m_output;                   // declared last: destroyed first

    int m_frameMsCopy;                      // frame duration of the stream behind m_statsCopy
    std::atomic<uint32_t> m_formatDropped;  // datagrams that were not valid Opus or did not match the current stream format

    void onDeviceData(int16_t *out, uint32_t frames);
    void adoptNextStream();
    void reapRetired();
    void init();                            // called by SetConfig()
    bool startDevice();                     // opens the speaker with the current m_output device choice
    void requestOpen(const std::string &deviceId);   // re-opens the speaker on the device worker
    void publishStats();

public:
    Player();
    ~Player();
    void SetConfig(AudioConfig *pConfig);
    // Switch the speaker while running (no Stop / Start needed): only the device is re-opened, the received stream,
    // jitter buffer and statistics are untouched; a short gap (tens of ms, longer for Bluetooth) is audible. "" = system
    // default. The switch runs on a worker thread, so this returns at once and never freezes the GUI; several quick calls
    // are merged (the last one wins). If the new device can't be opened the default one is used, if that fails too the
    // player stays silent (logged). Not running yet: just remembered for the first start. Returns false only if the
    // player has no config. GUI thread.
    bool SetOutputDevice(const std::string &deviceId);
    // Call about once a second from the GUI thread. It repairs the speaker: if the system moved the stream to another
    // device than the chosen one (Android: a notification sound, a Bluetooth connection...), or the speaker stopped (a re-open
    // failed, a phone call took the device) the chosen device is opened again. At most once per 3 s; after two failed
    // attempts in a row it falls back to the system default device (the choice in the UI is not touched).
    void KeepDevice();
    void SetKeepIntervalMs(int ms) { m_keepIntervalMs = ms; }       // (tests)
    // By default the player runs KeepDevice() itself about once a second (own thread), so the 1 s timer in the app is no longer needed
    // (calling KeepDevice() yourself as well is harmless). false = only your calls repair the device. Any thread.
    void SetAutoKeepDevice(bool on) { m_autoKeep.store(on, std::memory_order_relaxed); }
    // the speaker that is REALLY in use (see AudioOutput::ActiveDevice), "" = unknown. GUI thread.
    std::string ActiveOutputDevice() const { return m_output.ActiveDevice(); }
    // optional echo canceller, the SAME instance as the recorder's. Call BEFORE SetConfig() (it starts the speaker).
    // Ignored (with a warning) once the speaker runs (after SetConfig()): the audio thread is already using it.
    void SetAudioProcessor(std::shared_ptr<AudioProcessor> aec);
    // The local copy of the music the recorder sends (Recorder::Music()): the one who plays it hears exactly what is being sent, e.g. to sing
    // along. Off until MusicPlayer::SetMonitor(true); its volume is MusicPlayer::SetMonitorVolume(), separate from SetVolume() here (which only
    // scales the received voice) and from the volume of what is sent. It goes through the echo canceller reference, like everything the speaker
    // plays. Call BEFORE SetConfig() (that starts the speaker); refused afterwards.
    void SetMusicMonitor(std::shared_ptr<MusicPlayer> music);

    // Adaptive playout (default ON): the player stamps every datagram when Play() gets it and the jitter buffer follows the
    // measured network jitter (deeper BEFORE the audio breaks up, shallower again when the network is calm), and keeps the
    // delay near what the network needs by dropping / repeating single frames at quiet moments. OFF = the plain reactive
    // buffer (grows only after an underrun). Both calls apply to streams created afterwards: call them BEFORE the first Play().
    void SetAdaptivePlayout(bool on) { m_adaptive.store(on, std::memory_order_relaxed); }
    // Use Opus DRED data (deep redundancy, Opus 1.5+) to rebuild runs of lost packets. Needs libopus built with --enable-dred and
    // RAVAVA_ENABLE_DRED; otherwise it does nothing. The sender must send DRED (AudioConfig::dredMs). Call BEFORE the first Play().
    void SetDredPlayback(bool on) { m_dred.store(on, std::memory_order_relaxed); }
    // With adaptive playout + DRED: after a loss burst of n packets keep at least n+1 frames (at most maxMs) buffered, so the packet that
    // carries the DRED data has arrived when the lost frames are due; the floor falls back after ~10 s without bursts. 0 = off (default).
    // Costs delay: measured on real speech, ~60 ms extra mouth-to-ear for about half of DRED's gain. Before the first Play().
    void SetDredFloorMs(int maxMs) { m_dredFloorMs.store(maxMs, std::memory_order_relaxed); }
    // limits of the jitter buffer depth in ms (default 40 .. 200); 0 = leave that limit at its default.
    void SetJitterLimitsMs(int minMs, int maxMs) { m_limMinMs.store(minMs, std::memory_order_relaxed); m_limMaxMs.store(maxMs, std::memory_order_relaxed); }

    // live receive statistics (loss, FEC/PLC, buffer depth). false if the player isn't running.
    // The figures are at most ~10 ms old.
    bool GetStats(JitterBuffer::Stats &stats, int &bufferFrames, int &frameMs) const;
    // loudness of the received audio, 0..100 (0 = silence, see LevelMeter.h). Any thread, lock-free.
    // It is measured on the decoded PCM that is sent to the speaker, so DTX silence / lost audio read 0.
    int GetLevel() const { return m_level.Level(); }        // smooth, "right now"
    int TakePeakLevel() { return m_level.TakePeak(); }      // loudest since the last call (for slow GUI timers)
    // playback volume of the received audio: 0 = mute .. 100 = unchanged (default). Any thread, takes effect
    // within ~10 ms without clicks. Quadratic curve (50 = -12 dB), which sounds like an even slider. Never amplifies,
    // so it can't clip. GetLevel()/TakePeakLevel() measure BEFORE it (the level that was received).
    void SetVolume(int percent) { m_volume.Set(percent); }
    int  GetVolume() const { return m_volume.Get(); }
    // one received datagram (audio header + opus packet). Never blocks the audio thread.
    void Play(const uint8_t *datagram, size_t size);
    void Play(const Bytes &datagram) { Play(datagram.data(), datagram.size()); }
};

} // namespace ravava

#endif // RAVAVA_PLAYER_H
