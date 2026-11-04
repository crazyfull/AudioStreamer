/*
 * RavAva - real-time voice library (Opus over any datagram transport)
 * Copyright (c) 2026 Mojtaba Kahvand. All rights reserved.
 * Author: Mojtaba Kahvand
 *
 * Proprietary software, NOT free / open source. Use, copying, modification and distribution are forbidden without a
 * written license from the author. See the LICENSE file in the repository root.
 * SPDX-License-Identifier: LicenseRef-RavAva-Proprietary
 */
#ifndef RAVAVA_CONFERENCEPLAYER_H
#define RAVAVA_CONFERENCEPLAYER_H

#include <atomic>
#include <memory>
#include <mutex>
#include <vector>
#include "Player.h"

namespace ravava {

// Receive side for a CONFERENCE: several people talk at the same time, ONE speaker.
//
//   datagram + streamId -> that talker's own jitter buffer / FEC / PLC / Opus decoder  (a Player in "external output" mode)
//                       -> all talkers are added together (saturating) -> volume -> echo canceller reference -> the speaker
//
// This is what WebRTC (one NetEq per remote stream + AudioMixer), Meet or Discord do: the talkers must NOT share a jitter buffer (it follows
// ONE sender's sequence numbers), and they must not each open a sound device either. The talker is created by its first packet and
// released after it has been silent for a while (SetIdleTimeoutMs). The hub that owns the speaker is an ordinary Player, so
// device selection, KeepDevice / auto-keep, the echo canceller, the music monitor, master volume and level work exactly as there.
//
// Threading: Play() from any thread(s) (it only copies the datagram; creating a talker allocates, on that thread, never on the audio
// thread). The audio thread walks a fixed table of talkers without a lock. Everything else: GUI thread, as in Player.
class ConferencePlayer : private AudioMixSource
{
public:
    static constexpr int MaxStreams = 32;       // hard limit of talkers heard at the same time

    ConferencePlayer();
    ~ConferencePlayer();

    // ---- like Player (same meaning); the settings of the talkers apply to talkers created AFTER the call: call them before the first Play()
    void SetConfig(AudioConfig *pConfig);
    void SetAudioProcessor(std::shared_ptr<AudioProcessor> aec) { m_hub.SetAudioProcessor(std::move(aec)); }     // before SetConfig()
    void SetMusicMonitor(std::shared_ptr<MusicPlayer> music)     { m_hub.SetMusicMonitor(std::move(music)); }    // before SetConfig()
    bool SetOutputDevice(const std::string &deviceId) { return m_hub.SetOutputDevice(deviceId); }
    void KeepDevice()                                 { m_hub.KeepDevice(); }
    void SetAutoKeepDevice(bool on)                   { m_hub.SetAutoKeepDevice(on); }
    std::string ActiveOutputDevice() const            { return m_hub.ActiveOutputDevice(); }
    void SetAdaptivePlayout(bool on)                  { m_adaptive.store(on ? 1 : 0, std::memory_order_relaxed); }
    void SetDredPlayback(bool on)                     { m_dred.store(on, std::memory_order_relaxed); }
    void SetDredFloorMs(int maxMs)                    { m_dredFloorMs.store(maxMs, std::memory_order_relaxed); }
    void SetJitterLimitsMs(int minMs, int maxMs)      { m_limMinMs.store(minMs, std::memory_order_relaxed); m_limMaxMs.store(maxMs, std::memory_order_relaxed); }

    // ---- conference
    // how many talkers at most (1..MaxStreams, default 16). A packet of one more talker is dropped (counted, see Rejected()) until a
    // talker has been idle. Before SetConfig().
    void SetMaxStreams(int n);
    // a talker that sent nothing for this long is released (default 10 s). Its next packet starts a fresh one. 0 = never.
    void SetIdleTimeoutMs(int ms) { m_idleMs.store(ms, std::memory_order_relaxed); }

    // one received datagram (audio header + opus packet, the network envelope already removed) of the talker `streamId`.
    void Play(uint16_t streamId, const uint8_t *datagram, size_t size);
    void Play(uint16_t streamId, const Bytes &datagram) { Play(streamId, datagram.data(), datagram.size()); }

    // master volume of everything that is heard, 0..100 (like Player::SetVolume), and of one talker (0 = mute that person). Any thread.
    void SetVolume(int percent) { m_hub.SetVolume(percent); }
    int  GetVolume() const      { return m_hub.GetVolume(); }
    bool SetStreamVolume(uint16_t streamId, int percent);
    // loudness 0..100 (see LevelMeter.h): of the whole mix, and of one talker as RECEIVED (before its volume): who is talking right now.
    int GetLevel() const { return m_hub.GetLevel(); }
    int TakePeakLevel()  { return m_hub.TakePeakLevel(); }
    int GetStreamLevel(uint16_t streamId) const;
    // the talkers that exist right now (heard within the idle timeout)
    std::vector<uint16_t> ActiveStreams() const;
    // receive statistics of one talker, like Player::GetStats. false = no such talker / nothing received yet. GUI thread.
    bool GetStreamStats(uint16_t streamId, JitterBuffer::Stats &stats, int &bufferFrames, int &frameMs) const;
    uint32_t Rejected() const { return m_rejected.load(std::memory_order_relaxed); }    // datagrams dropped: no free talker slot

private:
    struct Talker {
        uint16_t id = 0;
        bool used = false;
        int64_t lastMs = 0;
        std::unique_ptr<Player> player;
    };
    struct Retired { std::unique_ptr<Player> player; uint32_t seq; };

    void MixInto(int16_t *out, uint32_t frames) override;       // audio thread (hub's callback)
    int64_t nowMs() const;
    Talker *find(uint16_t id) const;                            // under m_netMutex
    void sweep(int64_t now);                                    // under m_netMutex: release idle talkers, free retired ones
    void reapRetired();                                         // under m_netMutex

    AudioConfig *m_pConfig = nullptr;
    int m_maxStreams = 16;
    std::atomic<int> m_idleMs{10000};
    std::atomic<int> m_adaptive{-1};                            // -1 = leave the Player default
    std::atomic<bool> m_dred{false};
    std::atomic<int> m_dredFloorMs{0}, m_limMinMs{0}, m_limMaxMs{0};
    std::atomic<uint32_t> m_rejected{0};

    // network side
    mutable std::mutex m_netMutex;
    Talker m_talkers[MaxStreams];
    std::vector<Retired> m_retired;
    int64_t m_lastSweepMs = 0;

    // audio side: what the audio thread reads. A talker is published by a release store, unpublished by storing nullptr; its Player is
    // freed only after the audio thread has left the callback it could have been in (m_cbSeq is odd inside MixInto()).
    std::atomic<Player*> m_slot[MaxStreams];
    std::atomic<uint32_t> m_cbSeq{0};
    std::vector<int16_t> m_scratch;                             // one chunk of one talker (audio thread only)
    uint32_t m_chunkFrames = 0;
    int m_channels = 1;

    Player m_hub;                                               // declared last: destroyed first (stops the speaker before the talkers go)
};

} // namespace ravava

#endif // RAVAVA_CONFERENCEPLAYER_H
