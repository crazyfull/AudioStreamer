#include "jitterBuffer.h"
#include "VoicePacket.h"
#include <QDebug>

JitterBuffer::JitterBuffer(opusCodec *codec, int decoderRate, int channels, int frameMs, int startMs)
    : m_pCodec(codec), m_channels(channels), m_frameSamples(decoderRate * frameMs / 1000),
      m_playing(false), m_haveSeq(false), m_next(0), m_highest(0),
      m_lastTx(0), m_target(3), m_plcRun(0), m_goodRun(0), m_underrunPending(false)
{
    if(frameMs < 1) frameMs = 20;
    auto frames = [frameMs](int ms){ return qMax(1, (ms + frameMs - 1) / frameMs); };   // ceil(ms / frameMs)

    m_minTarget    = frames(40);                    // 20ms:2  40ms:1  60ms:1
    m_target       = qMax(m_minTarget, frames(startMs));
    m_maxTarget    = qMax(m_target + 1, frames(200));
    m_maxPlcRun    = frames(80);                    // conceal at most ~80ms, then re-buffer
    m_catchUpSlack = qMax(1, 120 / frameMs);
    m_shrinkAfter  = qMax(10, 10000 / frameMs);     // ~10s
}

void JitterBuffer::Push(const QByteArray &datagram)
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
    }
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
        if(!marker && m_target < m_maxTarget)
            m_target++;
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
    if(m_playing && m_highest - m_next > m_target + m_catchUpSlack){
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
        m_next = m_buf.firstKey();
        m_playing = true;
        m_plcRun = 0;
    }

    // ---- normal case
    if(m_buf.contains(m_next)){
        m_plcRun = 0;
        m_stats.played++;
        if(++m_goodRun > m_shrinkAfter && m_target > m_minTarget){   // ~10s without trouble: shrink latency
            m_target--;
            m_goodRun = 0;
        }
        return takeNext();
    }

    // ---- missing packet
    m_goodRun = 0;

    if(m_buf.isEmpty()){
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
