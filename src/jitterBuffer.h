#ifndef JITTERBUFFER_H
#define JITTERBUFFER_H

#include <QByteArray>
#include <QMap>
#include <QSet>
#include <QElapsedTimer>
#include <deque>
#include "opusCodec.h"

// Receiver side: reorders packets, hides loss (Opus FEC, then PLC) and gives the
// audio device an even stream of 20ms frames, no matter how bursty the network is.
// Pure logic (no timers, no Qt Multimedia) so it can be unit tested.
class JitterBuffer
{
public:
    struct Stats {
        quint32 received = 0, played = 0, fec = 0, plc = 0, late = 0, skipped = 0, underrun = 0, dtxJump = 0;
        quint32 lost = 0;        // packets that were sent but never arrived (DTX silence is NOT counted)
        quint32 reordered = 0;   // arrived out of order (first counted as lost, then taken back)
        quint32 jitterMs = 0;    // measured network delay variation (97th percentile of the recent packets)
    };

    // startMs: initial depth; maxMs: the deepest the buffer may grow on a very bad (3G) network
    JitterBuffer(opusCodec *codec, int decoderRate, int channels, int frameMs, int startMs = 120, int maxMs = 1500);

    void Push(const QByteArray &datagram);                       // uses the internal clock
    void Push(const QByteArray &datagram, double nowMs);         // explicit clock (unit tests)
    // Returns exactly one frame of PCM, or an empty array when there is nothing to
    // play (prefilling after start / after a long gap or DTX silence).
    QByteArray PopFrame();
    // true if PopFrame() would return real, on-time audio (no concealment needed).
    bool HasFrameReady() const;

    int frameBytes() const { return m_frameSamples * m_channels * 2; }
    const Stats &stats() const { return m_stats; }
    int targetDepth() const { return m_target; }   // frames

private:
    struct Pkt { QByteArray payload; bool marker; };

    opusCodec *m_pCodec;
    int m_channels;
    int m_frameSamples;

    QSet<qint64> m_seen;         // recently received sequence numbers (to tell reordering from duplicates)
    quint8 m_lastTx;             // tx counter of the newest packet
    QMap<qint64, Pkt> m_buf;     // key = extended (32bit+) sequence
    bool m_playing;
    bool m_haveSeq;
    qint64 m_next;               // next sequence to play
    qint64 m_highest;
    int m_target;                // frames to collect before (re)starting playback
    int m_plcRun;
    int m_goodRun;
    bool m_underrunPending;      // buffer ran dry; was it end of speech (DTX) or a late network?
    Stats m_stats;

    // all limits are derived from frameMs so 20ms and 60ms frames behave the same in TIME
    int m_minTarget;
    int m_maxTarget;
    int m_maxPlcRun;
    int m_catchUpSlack;
    int m_shrinkAfter;      // frames without trouble before the target shrinks

    // ---- network delay measurement: how much later than the fastest recent packet does a
    // packet arrive? The buffer must be about as deep as the (almost) worst of those delays.
    QElapsedTimer m_clock;
    int m_frameMs;
    struct DelaySample { double at; double delay; };
    std::deque<DelaySample> m_delays;
    int m_samples;           // packets measured so far
    int m_warmup;            // measured packets needed before the very first playout
    bool m_everPlayed;
    int m_needed;            // frames the measured jitter asks for
    void MeasureDelay(qint64 ext, double nowMs);

    QByteArray takeNext();
};

#endif // JITTERBUFFER_H
