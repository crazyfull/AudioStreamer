#ifndef JITTERBUFFER_H
#define JITTERBUFFER_H

#include <map>
#include <set>
#include "AudioTypes.h"
#include "opusCodec.h"

// Receiver side: reorders packets, hides loss (Opus FEC, then PLC) and gives the
// audio device an even stream of frames, no matter how bursty the network is.
// Pure logic (no Qt, no timers, no audio device, NOT thread safe: the caller locks).
class JitterBuffer
{
public:
    struct Stats {
        uint32_t received = 0, played = 0, fec = 0, plc = 0, late = 0, skipped = 0, underrun = 0, dtxJump = 0;
        uint32_t lost = 0;        // packets that were sent but never arrived (DTX silence is NOT counted)
        uint32_t reordered = 0;   // arrived out of order (first counted as lost, then taken back)
    };

    JitterBuffer(opusCodec *codec, int decoderRate, int channels, int frameMs, int startMs = 120);

    void Push(const uint8_t *datagram, size_t size);
    // Returns exactly one frame of PCM, or an empty vector when there is nothing to
    // play (prefilling after start / after a long gap or DTX silence).
    Pcm PopFrame();
    // true if PopFrame() would return real, on-time audio (no concealment needed).
    bool HasFrameReady() const;

    int frameSamples() const { return m_frameSamples; }
    int frameBytes() const { return m_frameSamples * m_channels * 2; }
    const Stats &stats() const { return m_stats; }
    int targetDepth() const { return m_target; }

private:
    struct Pkt { Bytes payload; bool marker; };

    opusCodec *m_pCodec;
    int m_channels;
    int m_frameSamples;

    std::set<int64_t> m_seen;         // recently received sequence numbers
    uint8_t m_lastTx;                 // tx counter of the newest packet
    std::map<int64_t, Pkt> m_buf;     // key = extended (32bit+) sequence
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

    Pcm takeNext();
};

#endif // JITTERBUFFER_H
