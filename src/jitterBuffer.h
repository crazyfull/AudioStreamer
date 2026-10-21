#ifndef JITTERBUFFER_H
#define JITTERBUFFER_H

#include <QByteArray>
#include <QMap>
#include "opusCodec.h"

// Receiver side: reorders packets, hides loss (Opus FEC, then PLC) and gives the
// audio device an even stream of 20ms frames, no matter how bursty the network is.
// Pure logic (no timers, no Qt Multimedia) so it can be unit tested.
class JitterBuffer
{
public:
    struct Stats {
        quint32 received = 0, played = 0, fec = 0, plc = 0, late = 0, skipped = 0, underrun = 0, dtxJump = 0;
    };

    JitterBuffer(opusCodec *codec, int decoderRate, int channels, int frameMs);

    void Push(const QByteArray &datagram);
    // Returns exactly one frame of PCM, or an empty array when there is nothing to
    // play (prefilling after start / after a long gap or DTX silence).
    QByteArray PopFrame();
    // true if PopFrame() would return real, on-time audio (no concealment needed).
    bool HasFrameReady() const;

    int frameBytes() const { return m_frameSamples * m_channels * 2; }
    const Stats &stats() const { return m_stats; }
    int targetDepth() const { return m_target; }

private:
    struct Pkt { QByteArray payload; bool marker; };

    opusCodec *m_pCodec;
    int m_channels;
    int m_frameSamples;

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

    static const int MinTarget = 2;
    static const int MaxTarget = 10;
    static const int MaxPlcRun = 4;

    QByteArray takeNext();
};

#endif // JITTERBUFFER_H
