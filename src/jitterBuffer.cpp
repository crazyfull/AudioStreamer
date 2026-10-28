#include "jitterBuffer.h"
#include "VoicePacket.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>

JitterBuffer::JitterBuffer(opusCodec *codec, int decoderRate, int channels, int frameMs, int startMs)
    : m_count(0), m_seenTop(0), m_seenAny(false),
      m_pCodec(codec), m_channels(channels), m_frameSamples(0),
      m_lastTx(0), m_dtxSeen(false), m_playing(false), m_haveSeq(false), m_next(0), m_highest(0),
      m_target(3), m_plcRun(0), m_goodRun(0), m_underrunPending(false)
{
    m_slots.resize(Slots);              // the only allocation of the jitter buffer
    seenReset();

    if(frameMs < 1) frameMs = 20;
    m_frameSamples = decoderRate * frameMs / 1000;

    auto frames = [frameMs](int ms){ return std::max(1, (ms + frameMs - 1) / frameMs); };   // ceil(ms / frameMs)

    m_minTarget    = frames(40);                    // 20ms:2  40ms:1  60ms:1
    m_target       = std::max(m_minTarget, frames(startMs));
    m_maxTarget    = std::max(m_target + 1, frames(200));
    m_maxPlcRun    = frames(80);                    // conceal at most ~80ms, then re-buffer
    m_catchUpSlack = std::max(1, 120 / frameMs);
    m_shrinkAfter  = std::max(10, 10000 / frameMs); // ~10s
}

// ---------------------------------------------------------------- packet slots
JitterBuffer::Slot *JitterBuffer::findSlot(int64_t seq)
{
    Slot &s = m_slots[size_t(seq) & (Slots - 1)];
    return (s.used && s.key == seq) ? &s : nullptr;
}

JitterBuffer::Slot *JitterBuffer::lowestSlot()
{
    Slot *best = nullptr;
    for(Slot &s : m_slots)
        if(s.used && (!best || s.key < best->key))
            best = &s;
    return best;
}

void JitterBuffer::eraseSlot(Slot *s)
{
    if(s && s->used){
        s->used = false;
        m_count--;
    }
}

void JitterBuffer::eraseBelow(int64_t seq)
{
    for(Slot &s : m_slots)
        if(s.used && s.key < seq)
            eraseSlot(&s);
}

void JitterBuffer::clearPackets()
{
    for(Slot &s : m_slots)
        s.used = false;
    m_count = 0;
}

void JitterBuffer::putPacket(int64_t seq, bool marker, const uint8_t *data, size_t len)
{
    Slot &s = m_slots[size_t(seq) & (Slots - 1)];
    if(s.used){
        if(s.key >= seq)
            return;                 // slot holds the same or a NEWER packet: this one is too old for the window
        m_count--;                  // evict the older packet that shares the slot
    }
    s.key = seq;
    s.marker = marker;
    s.len = uint16_t(len);
    std::memcpy(s.data, data, len);
    s.used = true;
    m_count++;
}

// ---------------------------------------------------------------- "already received" window
void JitterBuffer::seenReset()
{
    std::memset(m_seenBits, 0, sizeof(m_seenBits));
    m_seenAny = false;
    m_seenTop = 0;
}

bool JitterBuffer::seenTest(int64_t seq) const
{
    if(!m_seenAny || seq > m_seenTop || seq <= m_seenTop - 1024)
        return false;
    const size_t bit = size_t(seq) & 1023;
    return (m_seenBits[bit >> 6] >> (bit & 63)) & 1;
}

void JitterBuffer::seenMark(int64_t seq)
{
    if(!m_seenAny){
        std::memset(m_seenBits, 0, sizeof(m_seenBits));
        m_seenAny = true;
        m_seenTop = seq;
    }else if(seq > m_seenTop){
        // the positions of seenTop+1 .. seq still hold sequence numbers 1024 older: clear them
        const int64_t from = (seq - m_seenTop >= 1024) ? seq - 1023 : m_seenTop + 1;
        for(int64_t k = from; k <= seq; k++){
            const size_t b = size_t(k) & 1023;
            m_seenBits[b >> 6] &= ~(uint64_t(1) << (b & 63));
        }
        m_seenTop = seq;
    }else if(seq <= m_seenTop - 1024){
        return;                     // older than the window, not tracked
    }
    const size_t bit = size_t(seq) & 1023;
    m_seenBits[bit >> 6] |= uint64_t(1) << (bit & 63);
}

// ---------------------------------------------------------------- receive
void JitterBuffer::Push(const uint8_t *datagram, size_t size)
{
    uint16_t seq; bool marker; uint8_t tx;
    const uint8_t *payload; size_t payloadSize;
    if(!VoicePacket::parseHeader(datagram, size, seq, marker, tx, payload, payloadSize) || payloadSize > MaxPayload)
        return;

    // extend 16bit sequence to a monotonically growing number
    int64_t ext;
    if(!m_haveSeq){
        m_haveSeq = true;
        ext = seq;
        m_highest = ext;
        m_lastTx = tx;
        seenMark(ext);
    }else{
        int16_t diff = int16_t(uint16_t(seq - uint16_t(m_highest & 0xFFFF)));
        ext = m_highest + diff;
        // Sender restarted (new talker / app restarted: sequence starts again at 0, always with a marker) or a huge jump:
        // resync. A marker packet that is further behind than the buffer can ever reorder is a restart too; without this
        // a restart at seq 0 would be "too late" until the old sequence number is reached again (minutes of silence).
        if(std::abs(int(diff)) > 3000 || (marker && int(diff) < -(int(m_maxTarget) + 16))){
            clearPackets();
            seenReset();
            m_lastTx = tx;
            m_playing = false;
            m_plcRun = 0;
            ext = seq;
            m_highest = ext;
            seenMark(ext);
        }
    }

    m_stats.received++;

    // ---- loss accounting (network loss only)
    if(ext > m_highest){
        // The tx counter has 7 bits (it counts transmitted packets only, so DTX silence doesn't show up in it):
        // alone it can only tell "holes mod 128", a loss run of 128+ packets would wrap around.
        // The sequence gap is an UPPER bound (silent frames add to it but are not losses).
        //  - A marker packet whose gap is bigger than the tx counter explains proves the sender skips silence
        //    (DTX). From then on the gap can't be trusted, only the tx counter is used.
        //  - Without DTX every frame is transmitted, so take the largest value that matches the tx counter and
        //    fits in the gap: exact for loss runs of any length.
        const uint32_t holesMod = uint32_t((tx - m_lastTx - 1) & VoicePacket::TxMask);
        const uint32_t holesMax = uint32_t(ext - m_highest - 1);
        if(marker && holesMax > holesMod)
            m_dtxSeen = true;
        uint32_t holes = holesMod;
        if(!m_dtxSeen && holesMax > holesMod)
            holes += ((holesMax - holesMod) / (VoicePacket::TxMask + 1)) * (VoicePacket::TxMask + 1);
        m_stats.lost += holes;
        m_stats.lostEvents += holes;
        m_lastTx = tx;
        seenMark(ext);
    }else if(!seenTest(ext)){
        // older than the newest one and never seen before: it was only late / reordered, not lost
        if(m_stats.lost > 0) m_stats.lost--;
        m_stats.reordered++;
        seenMark(ext);
    }

    if(m_playing && ext < m_next){          // too late, its time has passed
        m_stats.late++;
        return;
    }
    if(findSlot(ext))
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
    if(marker && !m_playing)
        eraseBelow(ext);

    putPacket(ext, marker, payload, payloadSize);
    if(ext > m_highest)
        m_highest = ext;

    // we're far ahead of playout (burst after a stall): drop the oldest audio
    // instead of letting latency grow
    if(m_playing && m_highest - m_next > m_target + m_catchUpSlack){
        int64_t newNext = m_highest - m_target;
        m_stats.skipped += uint32_t(newNext - m_next);
        m_next = newNext;
        eraseBelow(m_next);
    }
}

bool JitterBuffer::takeNext(Pcm &out)
{
    Slot *p = findSlot(m_next);
    m_next++;
    if(!p){
        out.clear();
        return false;
    }
    const bool ok = m_pCodec->Decode(p->data, p->len, out);     // straight from the slot, no copy
    eraseSlot(p);
    return ok;
}

bool JitterBuffer::HasFrameReady() const
{
    if(!m_playing)
        return m_count >= m_target;
    return hasSlot(m_next);
}

Pcm JitterBuffer::PopFrame()
{
    Pcm out;
    PopFrame(out);
    return out;
}

bool JitterBuffer::PopFrame(Pcm &out)
{
    out.clear();
    if(!m_playing){
        if(m_count < m_target)
            return false;                   // prefill
        m_next = lowestSlot()->key;
        m_playing = true;
        m_plcRun = 0;
    }

    // ---- normal case
    if(findSlot(m_next)){
        m_plcRun = 0;
        m_stats.played++;
        if(++m_goodRun > m_shrinkAfter && m_target > m_minTarget){   // ~10s without trouble: shrink latency
            m_target--;
            m_goodRun = 0;
        }
        return takeNext(out);
    }

    // ---- missing packet
    m_goodRun = 0;

    if(m_count == 0){
        // underrun: nothing at all queued. Conceal a few frames, then stop and
        // re-buffer with a deeper target.
        if(m_plcRun == 0)
            m_stats.underrun++;
        if(m_plcRun >= m_maxPlcRun){
            m_playing = false;
            m_underrunPending = true;
            return false;
        }
        m_plcRun++;
        m_next++;
        m_stats.plc++;
        return m_pCodec->DecodePlc(m_frameSamples, out);
    }

    const Slot *fp = lowestSlot();
    const int64_t first = fp->key;

    if(fp->marker && first > m_next){
        // gap + marker = sender went silent (DTX), not a network loss
        m_stats.dtxJump++;
        m_next = first;
        m_plcRun = 0;
        m_stats.played++;
        return takeNext(out);
    }

    m_plcRun = 0;
    if(first == m_next + 1){
        // next packet carries a low-bitrate copy of the lost frame (inband FEC)
        const bool ok = m_pCodec->DecodeFec(fp->data, fp->len, m_frameSamples, out);
        m_next++;
        m_stats.fec++;
        return ok;
    }

    m_next++;
    m_stats.plc++;
    return m_pCodec->DecodePlc(m_frameSamples, out);
}
