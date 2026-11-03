/*
 * RavAva - real-time voice library (Opus over any datagram transport)
 * Copyright (c) 2026 Mojtaba Kahvand. All rights reserved.
 * Author: Mojtaba Kahvand
 *
 * Proprietary software, NOT free / open source. Use, copying, modification and distribution are forbidden without a
 * written license from the author. See the LICENSE file in the repository root.
 * SPDX-License-Identifier: LicenseRef-RavAva-Proprietary
 */
#include "JitterBuffer.h"
#include "VoicePacket.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace ravava {

namespace {
// mean |sample| of every 4th sample: a cheap loudness (a tenth of a microsecond per frame)
inline int frameLevel(const Pcm &pcm)
{
    const size_t n = pcm.size();
    if(n < 4)
        return 0;
    int64_t sum = 0;
    size_t cnt = 0;
    for(size_t i = 0; i < n; i += 4, cnt++){
        const int v = pcm[i];
        sum += v < 0 ? -v : v;
    }
    return int(sum / int64_t(cnt));
}
}

JitterBuffer::JitterBuffer(OpusCodec *codec, int decoderRate, int channels, int frameMs, int startMs)
    : m_count(0), m_seenTop(0), m_seenAny(false),
      m_pCodec(codec), m_channels(channels), m_frameSamples(0),
      m_lastTx(0), m_dtxSeen(false), m_playing(false), m_haveSeq(false), m_next(0), m_highest(0),
      m_target(3), m_plcRun(0), m_goodRun(0), m_underrunPending(false)
{
    m_slots.resize(Slots);              // the only allocations of the jitter buffer
    m_data.resize(size_t(Slots) * MaxPayload);
    seenReset();

    if(frameMs < 1) frameMs = 20;
    m_frameMs = frameMs;
    m_frameSamples = decoderRate * frameMs / 1000;

    auto frames = [frameMs](int ms){ return std::max(1, (ms + frameMs - 1) / frameMs); };   // ceil(ms / frameMs)

    m_minTarget    = frames(40);                    // 20ms:2  40ms:1  60ms:1
    m_target       = std::max(m_minTarget, frames(startMs));
    m_maxTarget    = std::max(m_target + 1, frames(200));
    m_maxPlcRun    = frames(80);                    // conceal at most ~80ms, then re-buffer
    m_catchUpSlack = std::max(1, 120 / frameMs);
    m_shrinkAfter  = std::max(10, 10000 / frameMs); // ~10s

    // adaptive playout (only used after SetAdaptive(true)), all in TIME so 20 ms and 60 ms frames behave the same
    m_frameUs      = int64_t(frameMs) * 1000;
    m_leakUs       = std::max<int64_t>(1, m_frameUs / 2000);        // 500 ppm: faster than any real clock drift
    const int perPeak = std::max(2, 15000 / frameMs);               // the peak forgets over ~15 s (stalls on 3G come back within seconds)
    m_envShift     = 1;
    while((1 << m_envShift) < perPeak)
        m_envShift++;
    m_warmPackets  = std::max(10, 500 / frameMs);                   // ~0.5 s of samples before the estimate is trusted
    m_slipGap      = std::max(2, 100 / frameMs);                    // at most one dropped / inserted frame per ~100 ms
    m_winLen       = std::max(5, 600 / frameMs);                    // the fill is judged by its peak over ~0.6 s
    m_maxWait      = std::max(3, 240 / frameMs);                    // no quiet frame within ~240 ms: do it anyway
}

void JitterBuffer::SetAdaptive(bool on)
{
    if(on == m_adaptive)
        return;
    m_adaptive = on;
    if(on){
        m_startFrames = m_target;       // what the plain buffer starts with is kept until the estimate is warm
        m_target = m_minTarget;         // from now on m_target only holds what underruns taught (the estimate does the rest)
    }else{
        m_target = std::max(m_minTarget, m_startFrames);
    }
}

void JitterBuffer::SetTargetLimitsMs(int minMs, int maxMs)
{
    auto frames = [this](int ms){ return std::max(1, (ms + m_frameMs - 1) / m_frameMs); };
    m_minTarget = frames(minMs);
    m_maxTarget = std::max(m_minTarget + 1, frames(maxMs));
    m_target = std::min(m_maxTarget, std::max(m_minTarget, m_target));
    m_startFrames = std::min(m_maxTarget, std::max(m_minTarget, m_startFrames));
}

// ---------------------------------------------------------------- network jitter (adaptive playout)
// transit = arrival - sequence * frame time: constant on a perfect network, so what rises above its recent minimum is the
// delay the buffer has to absorb. The estimate is the PEAK of that, which rises at once and falls slowly (long enough
// to remember a stall for several seconds), so a bad moment is covered before the next underrun and the delay is given
// back only when the network has been calm for a while.
void JitterBuffer::trackArrival(int64_t ext, int64_t arrivalUs)
{
    const int64_t tr = arrivalUs - ext * m_frameUs;
    if(!m_trInit){
        m_trInit = true;
        m_trBase = tr;
        m_env = 0;
        m_trN = 0;
    }else{
        m_trBase += m_leakUs;           // the two clocks never run exactly alike: let the baseline follow them
        if(tr < m_trBase)
            m_trBase = tr;
        const int64_t x = tr - m_trBase;
        if(x >= m_env)
            m_env = x;
        else
            m_env -= (m_env - x) >> m_envShift;
    }
    if(m_trN < (1 << 30))
        m_trN++;
    m_jitFrames = int(std::min<int64_t>((m_env + m_frameUs - 1) / m_frameUs + 1, 1000));
    m_stats.jitterUs = uint32_t(std::min<int64_t>(m_env, 0xFFFFFFFFll));
}

// Called for every packet that is about to be played: should the delay (newest packet - playout position) change?
// The change is ONE frame at a time, never more often than every m_slipGap frames, and popFrame() carries it out only
// at a quiet frame (or after waiting m_maxWait frames for one). 0 = no, 1 = drop a frame, 2 = repeat a frame.
int JitterBuffer::playoutWant(const Slot *p)
{
    const int span = int(std::min<int64_t>(m_highest - m_next + 1, 100000));        // frames from the one to play up to the newest
    if(span > m_winMax)
        m_winMax = span;
    if(++m_winCnt >= m_winLen){                 // a window is complete: its peak is the delay the network leaves us
        m_spanPeak = m_winMax;
        m_winCnt = 0;
        m_winMax = 0;
    }
    if(m_sinceSlip < m_slipGap){
        m_sinceSlip++;
        return 0;
    }
    if(m_spanPeak < 0)
        return 0;                               // no complete window since the last change: nothing is known yet
    const int want = depth();
    if(m_spanPeak > want + 2 && !p->marker && hasSlot(m_next + 1))
        return 1;                               // too much delay
    if(m_spanPeak < want)
        return 2;                               // cushion thinner than the jitter needs
    return 0;
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
    std::memcpy(slotData(&s), data, len);
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
void JitterBuffer::pushImpl(const uint8_t *datagram, size_t size, int64_t arrivalUs, bool timed)
{
    uint16_t seq; bool marker; uint8_t tx;
    const uint8_t *payload; size_t payloadSize;
    if(!VoicePacket::ParseHeader(datagram, size, seq, marker, tx, payload, payloadSize) || payloadSize > MaxPayload)
        return;

    // extend 16bit sequence to a monotonically growing number
    int64_t ext;
    bool fresh = false;                 // the first packet of a stream: nothing to compare it with
    if(!m_haveSeq){
        fresh = true;
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
            if(m_playing)
                m_stats.rebuffer++;
            m_playing = false;
            m_plcRun = 0;
            m_trInit = false;
            m_insertPending = false;
            m_trN = 0;
            m_env = 0;
            m_jitFrames = 0;
            fresh = true;
            ext = seq;
            m_highest = ext;
            seenMark(ext);
        }
    }

    m_stats.received++;
    bool dupSeen = false;

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
        if(holes >= 2)
            m_stats.burstLoss++;
        m_lastTx = tx;
        seenMark(ext);
    }else if(!seenTest(ext)){
        // older than the newest one and never seen before: it was only late / reordered, not lost
        if(m_stats.lost > 0) m_stats.lost--;
        m_stats.reordered++;
        seenMark(ext);
    }else if(!fresh){
        dupSeen = true;                 // received before (and maybe already played)
        m_stats.duplicate++;
    }

    if(m_adaptive && !dupSeen){
        // late packets count too: they are exactly what the estimate has to learn from
        if(timed)
            trackArrival(ext, arrivalUs);
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
        // (adaptive: not needed and wrong after a loss burst, where the buffer ran dry although nothing was late;
        //  late arrivals are measured directly)
        if(!m_adaptive && !marker && m_target < m_maxTarget)
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
    if(m_playing && m_highest - m_next > depth() + m_catchUpSlack){
        int64_t newNext = m_highest - depth();
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
    const bool ok = m_pCodec->Decode(slotData(p), p->len, out);     // straight from the slot, no copy
    eraseSlot(p);
    return ok;
}

bool JitterBuffer::HasFrameReady() const
{
    if(!m_playing)
        return m_count >= depth();
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
    const bool ok = popFrame(out);
    m_stats.occupancy = uint32_t(m_count);
    return ok;
}

bool JitterBuffer::popFrame(Pcm &out)
{
    out.clear();
    if(!m_playing){
        if(m_count < depth())
            return false;                   // prefill
        m_next = lowestSlot()->key;
        m_playing = true;
        m_plcRun = 0;
        m_sinceSlip = 0;
        m_wait = 0;
        m_insertPending = false;
        m_spanPeak = -1; m_winCnt = 0; m_winMax = 0;
    }

    // ---- normal case
    if(Slot *p = findSlot(m_next)){
        m_plcRun = 0;
        if(m_insertPending){
            // the playout control asked for one frame more after a quiet one: the concealment a lost packet gets, m_next stays
            m_insertPending = false;
            m_stats.stretched++;
            return m_pCodec->DecodePlc(m_frameSamples, out);
        }
        const int want = m_adaptive ? playoutWant(p) : 0;
        if(++m_goodRun > m_shrinkAfter && m_target > m_minTarget){   // ~10s without trouble: shrink latency
            m_target--;
            m_goodRun = 0;
        }
        bool ok = takeNext(out);
        if(m_adaptive && ok){
            // Slip only where nobody hears it: background noise between words. If there is none for m_maxWait frames, do it anyway.
            const int level = frameLevel(out);
            const bool quiet = level < 40 || level * 3 < (m_lvlAvg >> 4);
            m_lvlAvg = m_lvlAvg ? m_lvlAvg + (((level << 4) - m_lvlAvg) >> 5) : (level << 4);
            if(want && (quiet || ++m_wait >= m_maxWait)){
                if(!quiet)
                    m_stats.slipForced++;
                m_wait = 0;
                m_sinceSlip = 0;
                m_spanPeak = -1;                // judge the next change by a window that started after this one
                m_winCnt = 0;
                m_winMax = 0;
                if(want == 1){
                    // too far behind: this frame goes unplayed (it was decoded, so the decoder state stays continuous) and
                    // the next one takes its place; playoutWant() made sure that one is here
                    m_stats.skipped++;
                    m_stats.trimmed++;
                    ok = takeNext(out);
                }else{
                    m_insertPending = true;
                }
            }else if(!want){
                m_wait = 0;
            }
        }
        m_stats.played++;
        return ok;
    }

    // ---- missing packet
    m_goodRun = 0;
    m_insertPending = false;

    if(m_count == 0){
        // underrun: nothing at all queued. Conceal a few frames, then stop and
        // re-buffer with a deeper target.
        if(m_plcRun == 0)
            m_stats.underrun++;
        if(m_plcRun >= m_maxPlcRun){
            m_playing = false;
            m_underrunPending = true;
            m_stats.rebuffer++;
            return false;
        }
        m_plcRun++;
        // Adaptive: nothing newer is queued, so there is no sign that frame m_next is LOST, only that it has not come YET (late
        // packet, link stall, a pause of a DTX sender). Conceal, but keep waiting for it: when it arrives it is played (the stream
        // is one frame later, the trim brings the delay back) instead of being thrown away as "late" after concealment took its
        // place. The plain buffer moves on at once (its behaviour is pinned by tests/ref).
        if(!m_adaptive)
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

    const int concealed = m_plcRun;       // frames of concealment already played while waiting for m_next (adaptive only, see above)
    m_plcRun = 0;
    if(m_adaptive && concealed > 0){
        // m_next did not come while we waited and something newer is here: those frames are lost, and the concealment that was
        // played while waiting already stands for `concealed` of them (concealing them again, or FEC for a frame that was concealed,
        // would add frames of delay): skip them, and if that reaches the packet we have, play it.
        m_next = std::min<int64_t>(first, m_next + concealed);
        if(m_next == first){
            m_stats.played++;
            return takeNext(out);
        }
    }
    if(first > m_next + 1 && m_pCodec->DredReady()){
        // several packets in a row are missing: the packet we have may carry a neural, compact copy of the last frames before it
        // (DRED, Opus 1.5+). A single loss is left to the inband FEC below, which is cheaper and good enough there.
        if(m_dredKey != first){
            m_dredKey = first;
            m_dredAvail = m_pCodec->DredParse(slotData(fp), fp->len, int(std::min<int64_t>(first - m_next, 100)) * m_frameSamples);
        }
        const int64_t offset = (first - m_next) * m_frameSamples;       // distance from the start of that packet back to the END of frame m_next
        if(offset <= m_dredAvail && m_pCodec->DecodeDred(int(offset), m_frameSamples, out)){
            m_next++;
            m_stats.dred++;
            return true;
        }
    }
    if(first == m_next + 1){
        // next packet carries a low-bitrate copy of the lost frame (inband FEC)
        const bool ok = m_pCodec->DecodeFec(slotData(fp), fp->len, m_frameSamples, out);
        m_next++;
        m_stats.fec++;
        return ok;
    }

    m_next++;
    m_stats.plc++;
    return m_pCodec->DecodePlc(m_frameSamples, out);
}

} // namespace ravava
