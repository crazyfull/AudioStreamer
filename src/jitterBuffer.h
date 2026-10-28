#ifndef JITTERBUFFER_H
#define JITTERBUFFER_H

#include <vector>
#include "AudioTypes.h"
#include "opusCodec.h"

// Receiver side: reorders packets, hides loss (Opus FEC, then PLC) and gives the
// audio device an even stream of frames, no matter how bursty the network is.
// Pure logic (no Qt, no timers, no audio device, NOT thread safe: one thread uses it, in clsAudioPlayer
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
        uint32_t ingestDropped = 0; // datagrams dropped before the jitter buffer (clsAudioPlayer's queue was full)
    };

    JitterBuffer(opusCodec *codec, int decoderRate, int channels, int frameMs, int startMs = 120);

    void Push(const uint8_t *datagram, size_t size);
    // Returns exactly one frame of PCM, or an empty vector when there is nothing to
    // play (prefilling after start / after a long gap or DTX silence).
    Pcm PopFrame();
    // Same, but writes into `out` (reuses its capacity: no allocation once warmed up). false = nothing to play.
    bool PopFrame(Pcm &out);
    // true if PopFrame() would return real, on-time audio (no concealment needed).
    bool HasFrameReady() const;

    int frameSamples() const { return m_frameSamples; }
    int frameBytes() const { return m_frameSamples * m_channels * 2; }
    const Stats &stats() const { return m_stats; }
    int targetDepth() const { return m_target; }

private:
    // Packets waiting to be played. Slot for sequence `s` = s & (Slots-1). Everything in the buffer lies
    // within a few frames of each other (the catch-up logic caps it), so two live packets never share a slot
    // in practice; if they do (a gap of 512+ frames) the newer one wins.
    static const int Slots = 512;               // power of two (10 s of 20 ms frames)
    static const size_t MaxPayload = 1500;      // an Opus packet is at most 1275 bytes
    struct Slot {
        int64_t key = 0;                        // extended sequence number
        uint16_t len = 0;
        bool used = false;
        bool marker = false;
        uint8_t data[MaxPayload];
    };
    std::vector<Slot> m_slots;                  // allocated once
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

    opusCodec *m_pCodec;
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
};

#endif // JITTERBUFFER_H
