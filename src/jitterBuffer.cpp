#include "jitterBuffer.h"
#include "VoicePacket.h"
#include <QDebug>
#include <vector>
#include <algorithm>

JitterBuffer::JitterBuffer(opusCodec *codec, int decoderRate, int channels, int frameMs, int startMs, int maxMs)
    : m_pCodec(codec), m_channels(channels), m_frameSamples(decoderRate * frameMs / 1000),
      m_playing(false), m_haveSeq(false), m_next(0), m_highest(0),
      m_lastTx(0), m_target(3), m_plcRun(0), m_goodRun(0), m_underrunPending(false),
      m_frameMs(frameMs < 1 ? 20 : frameMs), m_samples(0), m_everPlayed(false), m_needed(1)
{
    if(frameMs < 1) frameMs = 20;
    m_clock.start();
    auto frames = [frameMs](int ms){ return qMax(1, (ms + frameMs - 1) / frameMs); };   // ceil(ms / frameMs)

    m_minTarget    = frames(40);                    // 20ms:2  40ms:1  60ms:1
    m_target       = qMax(m_minTarget, frames(startMs));
    m_maxTarget    = qMax(m_target + 1, frames(maxMs));   // 3G can need >1s
    m_maxPlcRun    = frames(80);                    // conceal at most ~80ms, then re-buffer
    m_catchUpSlack = qMax(1, 120 / frameMs);
    m_shrinkAfter  = qMax(10, 4000 / frameMs);      // ~4s
    m_warmup       = qMax(4, 600 / frameMs);        // listen to the network ~0.6s before the first playout
    m_needed       = m_minTarget;
}

void JitterBuffer::Push(const QByteArray &datagram)
{
    Push(datagram, m_clock.nsecsElapsed() / 1e6);
}

void JitterBuffer::MeasureDelay(qint64 ext, double nowMs)
{
    // delay of this packet relative to the sender's clock (sequence * frame time).
    // The absolute value is meaningless (clocks differ), only the variation counts.
    double d = nowMs - double(ext) * m_frameMs;
    m_delays.push_back({nowMs, d});
    m_samples++;

    while(m_delays.size() > 500 || (m_delays.size() > 30 && nowMs - m_delays.front().at > 30000))
        m_delays.pop_front();

    if(m_delays.size() < 8)
        return;

    double base = d;
    for(const DelaySample &x : m_delays)
        base = qMin(base, x.delay);

    std::vector<double> v;
    v.reserve(m_delays.size());
    for(const DelaySample &x : m_delays)
        v.push_back(x.delay - base);

    size_t k = size_t(v.size() * 0.97);
    if(k >= v.size()) k = v.size() - 1;
    std::nth_element(v.begin(), v.begin() + k, v.end());
    double jitter = v[k];
    m_stats.jitterMs = quint32(jitter + 0.5);

    // depth needed = jitter + one frame of margin, in frames
    int need = int((jitter + m_frameMs + m_frameMs - 1) / m_frameMs);
    m_needed = qBound(m_minTarget, need, m_maxTarget);
    if(m_needed > m_target)
        m_target = m_needed;        // grow at once, shrink slowly (see PopFrame)
}

void JitterBuffer::Push(const QByteArray &datagram, double nowMs)
{
    quint16 seq; bool marker; quint8 tx; QByteArray payload;
    if(!VoicePacket::parse(datagram, seq, marker, tx, payload))
        return;

    // extend 16bit sequence to a monotonically growing number
    qint64 ext;
    if(!m_haveSeq){
        m_haveSeq = true;
        ext = seq;
        m_highest = ext;
        m_lastTx = tx;
        m_seen.insert(ext);
    }else{
        qint16 diff = qint16(quint16(seq - quint16(m_highest & 0xFFFF)));
        ext = m_highest + diff;
        if(qAbs(diff) > 3000){      // sender restarted / huge jump: resync
            m_buf.clear();
            m_seen.clear();
            m_delays.clear();
            m_lastTx = tx;
            m_playing = false;
            m_plcRun = 0;
            ext = seq;
            m_highest = ext;
            m_seen.insert(ext);
        }
    }

    m_stats.received++;

    // ---- loss accounting (network loss only)
    bool firstSeen = true;
    if(ext > m_highest){
        // newest packet: a hole in the tx counter = packets that were sent but never arrived.
        // (a hole in the sequence alone can just be DTX silence, so it isn't used here)
        quint8 holes = quint8((tx - m_lastTx - 1) & VoicePacket::TxMask);
        m_stats.lost += holes;
        m_lastTx = tx;
        m_seen.insert(ext);
    }else if(!m_seen.contains(ext)){
        // older than the newest one and never seen before: it was only late / reordered, not lost
        if(m_stats.lost > 0) m_stats.lost--;
        m_stats.reordered++;
        m_seen.insert(ext);
    }else{
        firstSeen = false;      // duplicate (or the very first packet): not a measurement
    }
    if(firstSeen)
        MeasureDelay(ext, nowMs);
    if(m_seen.size() > 1024){       // forget old entries
        for(auto it = m_seen.begin(); it != m_seen.end(); ){
            if(*it < m_highest - 512)
                it = m_seen.erase(it);
            else
                ++it;
        }
    }

    if(m_playing || !m_buf.isEmpty()){
        if(ext < m_next && m_playing){      // too late, its time has passed
            m_stats.late++;
            return;
        }
    }
    if(m_buf.contains(ext))
        return;                              // duplicate

    // After a dry buffer: a marker packet means the sender had been silent (normal),
    // a plain packet means the network was late -> buffer deeper from now on.
    if(m_underrunPending && !m_playing){
        if(!marker){
            m_goodRun = 0;
            if(m_target < m_maxTarget)
                m_target++;
        }
        m_underrunPending = false;
    }

    // A marker packet starts a new talkspurt: whatever older is still sitting in the
    // buffer while we're idle is a stray DTX/comfort-noise packet, not part of it.
    if(marker && !m_playing){
        while(!m_buf.isEmpty() && m_buf.firstKey() < ext)
            m_buf.erase(m_buf.begin());
    }

    m_buf.insert(ext, Pkt{payload, marker});
    if(ext > m_highest)
        m_highest = ext;

    // we're far ahead of playout (burst after a stall): drop the oldest audio
    // instead of letting latency grow
    if(m_playing && m_highest - m_next > m_target + m_target / 2 + m_catchUpSlack){
        qint64 newNext = m_highest - m_target;
        m_stats.skipped += quint32(newNext - m_next);
        m_next = newNext;
        while(!m_buf.isEmpty() && m_buf.firstKey() < m_next)
            m_buf.erase(m_buf.begin());
    }
}

QByteArray JitterBuffer::takeNext()
{
    Pkt p = m_buf.take(m_next);
    m_next++;
    QByteArray pcm = m_pCodec->Decode(p.payload);
    return pcm;
}

bool JitterBuffer::HasFrameReady() const
{
    if(!m_playing)
        return m_buf.size() >= m_target;
    return m_buf.contains(m_next);
}

QByteArray JitterBuffer::PopFrame()
{
    if(!m_playing){
        if(m_buf.size() < m_target)
            return QByteArray();            // prefill
        if(!m_everPlayed && m_samples < m_warmup)
            return QByteArray();            // first start: measure the network for a moment first
        m_everPlayed = true;
        m_next = m_buf.firstKey();
        m_playing = true;
        m_plcRun = 0;
    }

    // ---- normal case
    if(m_buf.contains(m_next)){
        m_plcRun = 0;
        m_stats.played++;
        if(++m_goodRun > m_shrinkAfter && m_target > qMax(m_minTarget, m_needed)){
            // a while without trouble: shrink latency, a bit at a time, towards what the
            // measured jitter still asks for
            m_target -= qMax(1, (m_target - qMax(m_minTarget, m_needed)) / 8);
            m_goodRun = 0;
        }
        return takeNext();
    }

    // ---- missing packet
    if(m_buf.isEmpty()){
        // (a dry buffer is usually just the end of a talkspurt, so it does not reset m_goodRun here;
        //  if it was really the network, Push() raises the target and resets it)
        // underrun: nothing at all queued. Conceal a few frames, then stop and
        // re-buffer with a deeper target.
        if(m_plcRun == 0)
            m_stats.underrun++;
        if(m_plcRun >= m_maxPlcRun){
            m_playing = false;
            m_underrunPending = true;
            return QByteArray();
        }
        m_plcRun++;
        m_next++;
        m_stats.plc++;
        return m_pCodec->DecodePlc(m_frameSamples);
    }

    m_goodRun = 0;      // real loss / reordering
    const qint64 first = m_buf.firstKey();
    const Pkt &fp = m_buf.first();

    if(fp.marker && first > m_next){
        // gap + marker = sender went silent (DTX), not a network loss
        m_stats.dtxJump++;
        m_next = first;
        m_plcRun = 0;
        m_stats.played++;
        return takeNext();
    }

    m_plcRun = 0;
    if(first == m_next + 1){
        // next packet carries a low-bitrate copy of the lost frame (inband FEC)
        QByteArray pcm = m_pCodec->DecodeFec(fp.payload, m_frameSamples);
        m_next++;
        m_stats.fec++;
        return pcm;
    }

    m_next++;
    m_stats.plc++;
    return m_pCodec->DecodePlc(m_frameSamples);
}
