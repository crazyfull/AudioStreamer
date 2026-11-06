/*
 * RavAva - real-time voice library (Opus over any datagram transport)
 * Copyright (c) 2026 Mojtaba Kahvand. All rights reserved.
 * Author: Mojtaba Kahvand
 *
 * Proprietary software, NOT free / open source. Use, copying, modification and distribution are forbidden without a
 * written license from the author. See the LICENSE file in the repository root.
 * SPDX-License-Identifier: LicenseRef-RavAva-Proprietary
 */
#ifndef RAVAVA_MUSICPLAYER_H
#define RAVAVA_MUSICPLAYER_H

// A music file that is MIXED into the transmitted stream, with its own transport controls, and (optionally) heard LOCALLY too.
//
//   Recorder owns one (Recorder::Music()): its audio thread calls Mix() once per capture chunk, so the music is sent together with the
//   microphone. The application only uses the control / volume functions below.
//
//   Play() starts from the beginning, Stop() ends and forgets the position, Pause() keeps the position, Resume() continues from it.
//   Play / Pause / Resume / Stop are separate calls, all from ONE control thread (the GUI), all return at once.
//
//   Two volumes, independent of each other and of the speaker volume (Player::SetVolume):
//     SetVolume()         what is SENT (0..200 %): the people who listen hear this.
//     SetMonitorVolume()  what the SPEAKER OF THIS DEVICE plays (0..200 %), only while SetMonitor(true): the one who plays the music hears
//                         exactly the position that is being sent (it comes out of the same samples), e.g. to sing along or read lyrics.
//   The local copy needs the player: Player::SetMusicMonitor(recorder.Music()). Without it (or with SetMonitor(false), the default) nothing is
//   played locally. With a speaker and a microphone in the same room use the echo canceller, or the music is picked up again by the microphone.
//
// Audio-thread rules: Mix() and MixMonitor() never lock, wait or allocate. The file is decoded on a background thread (AudioFileReader).

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include "AudioTypes.h"
#include "AudioDevice.h"
#include "GainRamp.h"

namespace ravava {

class MusicPlayer
{
public:
    enum class State { Stopped, Playing, Paused, Finished };

    MusicPlayer();
    ~MusicPlayer();
    MusicPlayer(const MusicPlayer&) = delete;
    MusicPlayer& operator=(const MusicPlayer&) = delete;

    // ---- control (GUI thread) ----
    bool Play(const std::string &path);     // from the beginning. false: the recorder is not started (the microphone is the clock)
    bool Pause();                           // false unless Playing
    bool Resume();                          // false unless Paused
    void Stop();
    State GetState() const;
    std::string Error() const;              // "" = fine so far; why the file could not be opened / decoded (any thread)

    void SetVolume(int percent) { m_gain.Set(percent); }              // sent audio, 0..200, default 100, no clicks
    int  GetVolume() const { return m_gain.Get(); }
    void SetMonitor(bool on);                                         // hear it locally (default off)
    bool IsMonitor() const { return m_monitorOn.load(std::memory_order_relaxed); }
    void SetMonitorVolume(int percent) { m_monGain.Set(percent); }    // local audio, 0..200, default 100
    int  GetMonitorVolume() const { return m_monGain.Get(); }
    // Singing / speaking along with the music: what you hear locally comes out of your speaker some ms AFTER it was read (the local queue
    // + the speaker buffer, much more on Bluetooth), and your voice follows THAT sound. So in the stream the music is delayed by the same
    // time, or the listeners hear your voice late against the music. ms = 0..500; -1 (default) = automatic: the local queue + a typical
    // speaker buffer while SetMonitor(true), nothing while it is off. On Bluetooth / slow phones set it by ear (about 150-250 ms).
    // Only the SENT music is delayed; the local copy is not.
    void SetStreamDelayMs(int ms) { m_delayMs.store(ms < 0 ? -1 : (ms > kMaxStreamDelayMs ? kMaxStreamDelayMs : ms), std::memory_order_relaxed); }
    int  GetStreamDelayMs() const; // the delay in use now (resolves the automatic value)

    // ---- used by Recorder / Player (not by applications) ----
    void Prepare(int rate, int channels);   // Recorder::Start(): the format of the capture chunks; before the device runs. Reserves all buffers
    void Release();                         // Recorder::Stop(): stops the music, Play() is refused again
    // Recorder audio thread: `src` = the microphone chunk (n interleaved samples). Returns what to encode: `src` itself when no music plays
    // (active = false), otherwise a buffer owned by this object with the music added (saturating, no wrap-around).
    const int16_t *Mix(const int16_t *src, size_t n, bool &active);
    // Player: the speaker format (before its device runs, or while the recorder is not mixing); then, on the Player's audio thread, adds the
    // local copy to the speaker buffer `out` (frames * channels samples).
    void SetMonitorFormat(int rate, int channels);
    void MixMonitor(int16_t *out, uint32_t frames);

    static constexpr int kMaxStreamDelayMs = 500;

private:
    void writeMonitor(const int16_t *raw, size_t frames);
    void reserveConverter();
    void flushMonitor() { m_monFlush.store(true, std::memory_order_release); }

    AudioFileReader m_file;
    std::mutex m_ctl;                       // control calls among themselves
    int m_rate = 0, m_ch = 0;               // capture format (Prepare)
    std::atomic<bool> m_prepared{false};
    std::atomic<int> m_mode{0};             // 0 stopped, 1 playing, 2 paused
    std::atomic<bool> m_inRead{false};      // the audio thread is inside m_file.Read(): Play / Stop wait for it (a few microseconds)
    GainRamp m_gain, m_monGain;
    Pcm m_raw, m_mixBuf;                    // music of this chunk / mic + music
    // delay line of the SENT music (recorder thread only; allocated in Prepare)
    std::atomic<int> m_delayMs{-1};         // -1 automatic
    Pcm m_dly;                              // ring, power of two
    size_t m_dlyMask = 0;
    uint64_t m_dlyWr = 0, m_dlyBase = 0;    // samples written so far / where the music of this play starts (older = silence)
    std::atomic<bool> m_dlyReset{false};    // Play / Stop: forget what is in the delay line
    Pcm m_dlyOut;

    // local monitor: the SAME samples the stream gets, converted to the speaker format, through a lock-free ring (writer: recorder thread,
    // reader: player thread). A speaker that runs a little faster / slower than the microphone just skips / repeats a few ms now and then.
    std::atomic<bool> m_monitorOn{false};
    std::atomic<bool> m_monReady{false};    // ring + converter match the speaker format
    std::atomic<bool> m_monInWrite{false};
    std::atomic<bool> m_monFlush{false};    // the reader drops what is queued (pause / stop / play)
    int m_monRate = 0, m_monCh = 0;
    std::vector<int16_t> m_ring;
    size_t m_ringMask = 0;
    std::atomic<uint64_t> m_wr{0}, m_rd{0};
    Pcm m_chanBuf, m_convBuf, m_monScratch;
    double m_convPos = 0;                   // resampler: position (input frames) of the next output frame, relative to the chunk start
    int16_t m_convPrev[2] = {0, 0};
    bool m_primed = false;                  // player thread: the ring holds enough to play from
};

} // namespace ravava

#endif // RAVAVA_MUSICPLAYER_H
