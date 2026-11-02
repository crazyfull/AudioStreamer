/*
 * RavAva - real-time voice library (Opus over any datagram transport)
 * Copyright (c) 2026 Mojtaba Kahvand. All rights reserved.
 * Author: Mojtaba Kahvand
 *
 * Proprietary software, NOT free / open source. Use, copying, modification and distribution are forbidden without a
 * written license from the author. See the LICENSE file in the repository root.
 * SPDX-License-Identifier: LicenseRef-RavAva-Proprietary
 */
#ifndef RAVAVA_JITTERBUFFER_H
#define RAVAVA_JITTERBUFFER_H

#include <algorithm>
#include <vector>
#include "AudioTypes.h"
#include "OpusCodec.h"

namespace ravava {

// Receiver side: reorders packets, hides loss (Opus FEC, then PLC) and gives the
// audio device an even stream of frames, no matter how bursty the network is.
// Pure logic (no Qt, no timers, no audio device, NOT thread safe: one thread uses it, in Player
// that is the audio thread).
//
// No allocation after the constructor: packets live in a fixed ring of slots (copied once, from the
// network datagram into the slot, decoded straight out of it) instead of std::map / std::vector per packet.
class JitterBuffer
{
public:
    struct Stats {
        uint32_t received = 0, played = 0, fec = 0, plc = 0, late = 0, skipped = 0, underrun = 0, dtxJump = 0;
        uint32_t lost = 0;        // packets that were sent but never arrived (DTX silence is NOT counted).
                                  // Net value: goes DOWN again when a packet that was counted lost arrives late.
        uint32_t reordered = 0;   // arrived out of order (first counted as lost, then taken back)
        uint32_t lostEvents = 0;  // every packet ever counted lost, NEVER decreased. For windowed figures use
                                  // delta(lostEvents) - delta(reordered), not delta(lost) (which can go backwards).
        uint32_t ingestDropped = 0; // datagrams dropped before the jitter buffer (Player's queue was full)
        uint32_t formatDropped = 0; // datagrams Player refused: not valid Opus, or another frame duration than the current stream
        uint32_t duplicate = 0;     // the same packet received twice
        uint32_t burstLoss = 0;     // loss runs of 2 or more packets in a row
        uint32_t rebuffer = 0;      // playback stopped and waited for the buffer to fill again (underrun, sender restart)
        uint32_t trimmed = 0;       // frames dropped one at a time by the adaptive playout (also counted in `skipped`)
        uint32_t stretched = 0;     // concealment frames inserted by the adaptive playout to rebuild the cushion
        uint32_t slipForced = 0;    // of trimmed + stretched: those that could NOT wait for a quiet frame (audible, if anything is)
        uint32_t occupancy = 0;     // packets waiting right now
        uint32_t jitterUs = 0;      // measured network jitter (peak of the late arrivals), 0 while arrival times are not used
    };

    JitterBuffer(OpusCodec *codec, int decoderRate, int channels, int frameMs, int startMs = 120);

    // `arrivalUs` = when the datagram came off the network, microseconds of any MONOTONIC clock (only differences are used).
    // Without it (first overload) nothing is known about the network except what the packets themselves show.
    void Push(const uint8_t *datagram, size_t size) { pushImpl(datagram, size, 0, false); }
    void Push(const uint8_t *datagram, size_t size, int64_t arrivalUs) { pushImpl(datagram, size, arrivalUs, true); }
    // Adaptive playout (call once, before the first Push; Player does): the depth follows the measured arrival jitter
    // (it goes up at once, down slowly) and the delay is kept near it by dropping / inserting single frames, preferably small
    // (= quiet) ones, instead of staying wherever the start-up or the last burst left it. Needs the Push() with arrival time.
    // Off (default) = the plain reactive buffer.
    void SetAdaptive(bool on);
    // limits of the target depth in ms (defaults 40 .. 200). Before the first Push.
    void SetTargetLimitsMs(int minMs, int maxMs);
    int MinTarget() const { return m_minTarget; }
    int MaxTarget() const { return m_maxTarget; }
    // Returns exactly one frame of PCM, or an empty vector when there is nothing to
    // play (prefilling after start / after a long gap or DTX silence).
    Pcm PopFrame();
    // Same, but writes into `out` (reuses its capacity: no allocation once warmed up). false = nothing to play.
    bool PopFrame(Pcm &out);
    // true if PopFrame() would return real, on-time audio (no concealment needed).
    bool HasFrameReady() const;

    int FrameSamples() const { return m_frameSamples; }
    int FrameBytes() const { return m_frameSamples * m_channels * 2; }
    const Stats &GetStats() const { return m_stats; }
    int TargetDepth() const { return depth(); }
    int Occupancy() const { return m_count; }       // packets waiting right now
    // extended sequence number of the frame that is played next (diagnostics / tests; valid while playing)
    int64_t PlayoutPosition() const { return m_next; }
    // how far the newest packet is ahead of the playout position, in frames = the delay this buffer adds (0 while prefilling)
    int DelayFrames() const { return m_playing ? int(m_highest - m_next) : 0; }

private:
    // Packets waiting to be played. Slot for sequence `s` = s & (Slots-1). Everything in the buffer lies
    // within a few frames of each other (the catch-up logic caps it), so two live packets never share a slot
    // in practice; if they do (a gap of 512+ frames) the newer one wins.
    static const int Slots = 512;               // power of two (10 s of 20 ms frames)
    static const size_t MaxPayload = 1500;      // an Opus packet is at most 1275 bytes
    // The bookkeeping (16 bytes per slot, 8 KB for all) is kept apart from the payload bytes (768 KB): the full-ring scans
    // (lowestSlot / eraseBelow, on the audio thread) then walk one small array that stays in the cache instead of touching
    // 512 cache lines that are 1.5 KB apart.
    struct Slot {
        int64_t key = 0;                        // extended sequence number
        uint16_t len = 0;
        bool used = false;
        bool marker = false;
    };
    std::vector<Slot> m_slots;                  // allocated once
    std::vector<uint8_t> m_data;                // allocated once: Slots * MaxPayload, payload of slot i at i * MaxPayload
    uint8_t *slotData(const Slot *s) { return m_data.data() + size_t(s - m_slots.data()) * MaxPayload; }
    int m_count;                                // used slots

    Slot *findSlot(int64_t seq);
    bool hasSlot(int64_t seq) const { const Slot &s = m_slots[size_t(seq) & (Slots - 1)]; return s.used && s.key == seq; }
    Slot *lowestSlot();                         // oldest packet, nullptr if empty
    void putPacket(int64_t seq, bool marker, const uint8_t *data, size_t len);
    void eraseSlot(Slot *s);
    void eraseBelow(int64_t seq);               // drop every packet older than seq
    void clearPackets();

    // which sequence numbers arrived recently (window of 1024 behind the newest seen), for the loss statistics
    uint64_t m_seenBits[16];
    int64_t m_seenTop;
    bool m_seenAny;
    bool seenTest(int64_t seq) const;
    void seenMark(int64_t seq);
    void seenReset();

    OpusCodec *m_pCodec;
    int m_channels;
    int m_frameSamples;

    uint8_t m_lastTx;                 // tx counter of the newest packet
    bool m_dtxSeen;                   // the sender skips silent frames (DTX): sequence gaps are then NOT losses
    bool m_playing;
    bool m_haveSeq;
    int64_t m_next;                   // next sequence to play
    int64_t m_highest;
    int m_target;                     // frames to collect before (re)starting playback
    int m_plcRun;
    int m_goodRun;
    bool m_underrunPending;
    Stats m_stats;

    // all limits are derived from frameMs so 20ms and 60ms frames behave the same in TIME
    int m_minTarget;
    int m_maxTarget;
    int m_maxPlcRun;
    int m_catchUpSlack;
    int m_shrinkAfter;

    bool takeNext(Pcm &out);
    bool popFrame(Pcm &out);
    void pushImpl(const uint8_t *datagram, size_t size, int64_t arrivalUs, bool timed);

    // ---- adaptive playout (see SetAdaptive)
    int depth() const {                         // frames to hold back: what Push/PopFrame use instead of m_target
        if(!m_adaptive)
            return m_target;
        const int d = m_trN < m_warmPackets ? m_startFrames : std::max(m_target, m_jitFrames);
        return std::min(m_maxTarget, std::max(m_minTarget, d));
    }
    void trackArrival(int64_t ext, int64_t arrivalUs);
    int playoutWant(const Slot *p);             // 0 = leave the delay, 1 = drop a frame, 2 = repeat a frame

    bool m_adaptive = false;
    int m_frameMs = 20;
    int m_startFrames = 6;                      // depth until the estimate has m_warmPackets samples
    int m_warmPackets = 25;
    int64_t m_frameUs = 20000;
    int64_t m_leakUs = 10;                      // the baseline rises this much per packet (follows clock drift)
    int m_envShift = 8;                         // the peak falls by 1/2^shift of the gap per packet (~15 s)
    bool m_trInit = false;
    int m_trN = 0;                              // samples seen
    int64_t m_trBase = 0;                       // smallest transit time lately (arrival - sequence * frame time)
    int64_t m_env = 0;                          // peak of (transit - base), us
    int m_jitFrames = 0;
    int m_lvlAvg = 0;                           // average loudness of the played frames * 16
    bool m_insertPending = false;               // repeat one frame (concealment) at the next normal frame
    int m_sinceSlip = 0, m_slipGap = 5, m_wait = 0, m_maxWait = 12;
    // The buffer fill (newest packet - playout position) dips whenever a packet is late and recovers when it arrives, so a single
    // reading says nothing about the delay. The delay is the PEAK over a short window (= the fill when the network is on time);
    // playoutWant() decides on the peak of the last complete window, never more than once per window.
    int m_winLen = 30, m_winCnt = 0, m_winMax = 0, m_spanPeak = -1;
};

} // namespace ravava

#endif // RAVAVA_JITTERBUFFER_H
