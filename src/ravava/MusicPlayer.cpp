/*
 * RavAva - real-time voice library (Opus over any datagram transport)
 * Copyright (c) 2026 Mojtaba Kahvand. All rights reserved.
 * Author: Mojtaba Kahvand
 *
 * Proprietary software, NOT free / open source. Use, copying, modification and distribution are forbidden without a
 * written license from the author. See the LICENSE file in the repository root.
 * SPDX-License-Identifier: LicenseRef-RavAva-Proprietary
 */
#include "MusicPlayer.h"
#include <algorithm>
#include <cmath>
#include <thread>

namespace ravava {

namespace {
inline int16_t sat(int v) { return int16_t(v > 32767 ? 32767 : (v < -32768 ? -32768 : v)); }
const int kMonitorRingMs = 500;         // capacity of the local copy's ring
const int kMonitorMaxLatMs = 100;       // longer than this queued: skip ahead ...
const int kMonitorTargetLatMs = 30;     // ... to this much
const int kSpeakerBufferMs = 20;        // typical output buffer of the speaker (10 ms period x 2): part of the automatic stream delay
}

MusicPlayer::MusicPlayer() : m_gain(200), m_monGain(200) {}

MusicPlayer::~MusicPlayer()
{
    Release();
}

// ------------------------------------------------------------------------------------------------------------------- control
// Takes the file away from the audio thread: mode = stopped first, then wait until it is not inside Read() (at most one device chunk)
static void detach(std::atomic<int> &mode, std::atomic<bool> &inRead)
{
    mode.store(0, std::memory_order_seq_cst);
    while(inRead.load(std::memory_order_seq_cst))
        std::this_thread::yield();
}

bool MusicPlayer::Play(const std::string &path)
{
    std::lock_guard<std::mutex> lk(m_ctl);
    if(!m_prepared.load(std::memory_order_acquire))
        return false;
    detach(m_mode, m_inRead);
    flushMonitor();
    m_dlyReset.store(true, std::memory_order_release);
    m_file.Open(path, m_rate, m_ch);                    // from the beginning; the file is opened by the background thread
    m_mode.store(1, std::memory_order_release);
    return true;
}

bool MusicPlayer::Pause()
{
    int expect = 1;
    const bool ok = m_mode.compare_exchange_strong(expect, 2);     // the ring of the file keeps its content: Resume() goes on where it stopped
    if(ok)
        flushMonitor();
    return ok;
}

bool MusicPlayer::Resume()
{
    int expect = 2;
    return m_mode.compare_exchange_strong(expect, 1);
}

void MusicPlayer::Stop()
{
    std::lock_guard<std::mutex> lk(m_ctl);
    detach(m_mode, m_inRead);
    m_file.Close();
    flushMonitor();
    m_dlyReset.store(true, std::memory_order_release);
}

int MusicPlayer::GetStreamDelayMs() const
{
    const int d = m_delayMs.load(std::memory_order_relaxed);
    if(d >= 0)
        return d;
    return m_monitorOn.load(std::memory_order_relaxed) ? kMonitorTargetLatMs + kSpeakerBufferMs : 0;
}

MusicPlayer::State MusicPlayer::GetState() const
{
    switch(m_mode.load(std::memory_order_acquire)){
    case 1:  return m_file.IsFinished() ? State::Finished : State::Playing;
    case 2:  return State::Paused;
    default: return State::Stopped;
    }
}

std::string MusicPlayer::Error() const
{
    return m_file.Error();
}

void MusicPlayer::SetMonitor(bool on)
{
    m_monitorOn.store(on, std::memory_order_relaxed);
    flushMonitor();
}

void MusicPlayer::Prepare(int rate, int channels)
{
    Stop();                                             // (also waits for the audio thread of an earlier run)
    m_prepared.store(false, std::memory_order_release);
    m_rate = rate;
    m_ch = std::max(1, std::min(channels, 2));
    const size_t chunk = size_t(m_rate) / 5 * size_t(m_ch);             // 200 ms: more than any device chunk
    m_raw.clear();    m_raw.reserve(chunk);
    m_mixBuf.clear(); m_mixBuf.reserve(chunk);
    m_dlyOut.clear(); m_dlyOut.reserve(chunk);
    size_t cap = 1024;                                                  // the longest delay + one chunk
    while(cap < (size_t(m_rate) * size_t(m_ch) * size_t(kMaxStreamDelayMs) / 1000) + chunk)
        cap <<= 1;
    m_dly.assign(cap, 0);
    m_dlyMask = cap - 1;
    m_dlyWr = m_dlyBase = 0;
    m_monInWrite.store(true, std::memory_order_seq_cst);                // (the recorder is not running; this only keeps the rules the same)
    reserveConverter();
    m_monInWrite.store(false, std::memory_order_release);
    m_prepared.store(true, std::memory_order_release);
}

void MusicPlayer::Release()
{
    Stop();
    m_prepared.store(false, std::memory_order_release);
}

// ------------------------------------------------------------------------------------------------------------------- stream (recorder thread)
const int16_t *MusicPlayer::Mix(const int16_t *src, size_t n, bool &active)
{
    active = false;
    if(m_mode.load(std::memory_order_acquire) != 1 || m_file.IsFinished() || n > m_mixBuf.capacity())
        return src;
    active = true;

    m_raw.resize(n);
    size_t take = 0;
    m_inRead.store(true, std::memory_order_seq_cst);
    if(m_mode.load(std::memory_order_seq_cst) == 1)
        take = m_file.Read(m_raw.data(), n);
    m_inRead.store(false, std::memory_order_release);
    std::fill(m_raw.begin() + take, m_raw.end(), int16_t(0));           // still buffering -> silence

    if(m_monitorOn.load(std::memory_order_relaxed))
        writeMonitor(m_raw.data(), n / size_t(m_ch));                   // before the stream volume: the monitor has its own

    // the SENT music is delayed by what the local copy takes to reach the ear (SetStreamDelayMs); the copy above is not
    const size_t D = size_t(GetStreamDelayMs()) * size_t(m_rate) / 1000 * size_t(m_ch);
    const int16_t *music = m_raw.data();
    if(m_dlyReset.exchange(false, std::memory_order_acq_rel))
        m_dlyBase = m_dlyWr;                                            // a new play: nothing older than this exists
    if(!m_dly.empty()){
        for(size_t i = 0; i < n; ++i)                                   // always keep the history: the delay can be raised at any time
            m_dly[size_t((m_dlyWr + i) & m_dlyMask)] = m_raw[i];
        if(D > 0){
            m_dlyOut.resize(n);
            for(size_t i = 0; i < n; ++i){
                const uint64_t at = m_dlyWr + i;
                m_dlyOut[i] = at >= m_dlyBase + D ? m_dly[size_t((at - D) & m_dlyMask)] : int16_t(0);   // the first D ms: silence
            }
            music = m_dlyOut.data();
        }
        m_dlyWr += n;
    }
    if(music != m_raw.data())
        std::copy(music, music + n, m_raw.begin());
    m_gain.Process(m_raw.data(), uint32_t(n / size_t(m_ch)), m_ch);
    m_mixBuf.resize(n);
    for(size_t i = 0; i < n; ++i)
        m_mixBuf[i] = sat(int(src[i]) + int(m_raw[i]));
    return m_mixBuf.data();
}

// ------------------------------------------------------------------------------------------------------------------- local copy
void MusicPlayer::SetMonitorFormat(int rate, int channels)
{
    m_monReady.store(false, std::memory_order_seq_cst);
    while(m_monInWrite.load(std::memory_order_seq_cst))
        std::this_thread::yield();
    m_monRate = rate;
    m_monCh = std::max(1, std::min(channels, 2));
    size_t cap = 1024;
    while(cap < size_t(m_monRate) * size_t(m_monCh) * kMonitorRingMs / 1000)
        cap <<= 1;
    m_ring.assign(cap, 0);
    m_ringMask = cap - 1;
    m_wr.store(0); m_rd.store(0);
    m_monScratch.assign(size_t(m_monRate) / 5 * size_t(m_monCh), 0);    // (the reader's pieces)
    reserveConverter();
    m_monReady.store(true, std::memory_order_release);
}

// reserves the scratch of the capture-format -> speaker-format conversion (needs both formats); a writer is not running
void MusicPlayer::reserveConverter()
{
    if(m_rate <= 0 || m_monRate <= 0)
        return;
    const size_t inFrames = size_t(m_rate) / 5;
    m_chanBuf.clear(); m_chanBuf.reserve(inFrames * size_t(m_monCh));
    m_convBuf.clear(); m_convBuf.reserve((inFrames * size_t(m_monRate) / size_t(m_rate) + 4) * size_t(m_monCh));
    m_convPos = 0;
    m_convPrev[0] = m_convPrev[1] = 0;
}

// recorder thread: raw music (capture format) -> speaker format -> ring
void MusicPlayer::writeMonitor(const int16_t *raw, size_t frames)
{
    if(!m_monReady.load(std::memory_order_acquire))
        return;
    m_monInWrite.store(true, std::memory_order_seq_cst);
    if(m_monReady.load(std::memory_order_seq_cst) && frames > 0 && frames <= size_t(m_rate) / 5){
        const int cin = m_ch, cout = m_monCh;
        const int16_t *pcm = raw;                       // after the channel conversion
        if(cin != cout){
            m_chanBuf.resize(frames * size_t(cout));
            for(size_t i = 0; i < frames; ++i){
                if(cin == 2)  m_chanBuf[i] = int16_t((int(raw[2 * i]) + int(raw[2 * i + 1])) / 2);     // stereo -> mono
                else          { m_chanBuf[2 * i] = raw[i]; m_chanBuf[2 * i + 1] = raw[i]; }            // mono -> stereo
            }
            pcm = m_chanBuf.data();
        }
        const int16_t *outp = pcm;
        size_t outFrames = frames;
        if(m_rate != m_monRate){
            // linear interpolation; the last input frame is kept for the next chunk so the chunks join without a seam
            const double step = double(m_rate) / double(m_monRate);
            m_convBuf.resize((size_t(double(frames) / step) + 4) * size_t(cout));
            size_t o = 0;
            while(m_convPos < double(frames) - 1.0){
                const long i = long(std::floor(m_convPos));
                const double fr = m_convPos - double(i);
                for(int c = 0; c < cout; ++c){
                    const double a = i < 0 ? m_convPrev[c] : pcm[size_t(i) * size_t(cout) + size_t(c)];
                    const double b = pcm[size_t(i + 1) * size_t(cout) + size_t(c)];
                    m_convBuf[o * size_t(cout) + size_t(c)] = int16_t(a + (b - a) * fr);
                }
                ++o;
                m_convPos += step;
            }
            m_convPos -= double(frames);
            for(int c = 0; c < cout; ++c)
                m_convPrev[c] = pcm[(frames - 1) * size_t(cout) + size_t(c)];
            outp = m_convBuf.data();
            outFrames = o;
        }
        const size_t count = outFrames * size_t(cout);
        const uint64_t w = m_wr.load(std::memory_order_relaxed);
        if(m_ring.size() - size_t(w - m_rd.load(std::memory_order_acquire)) >= count){   // full (player stalled): drop this chunk
            for(size_t i = 0; i < count; ++i)
                m_ring[size_t((w + i) & m_ringMask)] = outp[i];
            m_wr.store(w + count, std::memory_order_release);
        }
    }
    m_monInWrite.store(false, std::memory_order_release);
}

// player thread
void MusicPlayer::MixMonitor(int16_t *out, uint32_t frames)
{
    if(!m_monReady.load(std::memory_order_acquire))
        return;
    uint64_t rd = m_rd.load(std::memory_order_relaxed);
    const uint64_t wr = m_wr.load(std::memory_order_acquire);
    if(!m_monitorOn.load(std::memory_order_relaxed) || m_monFlush.exchange(false, std::memory_order_acquire)){
        m_rd.store(wr, std::memory_order_release);      // nothing queued is played (off, or a new position)
        m_primed = false;
        return;
    }
    const size_t ch = size_t(m_monCh);
    const uint64_t primeAt = uint64_t(m_monRate) * kMonitorTargetLatMs / 1000 * ch;
    if(!m_primed){
        if(wr - rd < primeAt)                           // the writer is a 10 ms chunk ahead at best: wait until there is a cushion
            return;
        m_primed = true;
    }
    if(wr - rd < size_t(frames) * ch){                  // ran dry (a late chunk): play nothing now, refill the cushion, no half-chunks
        m_primed = false;
        return;
    }
    const uint64_t maxLat = uint64_t(m_monRate) * kMonitorMaxLatMs / 1000 * ch;
    if(wr - rd > maxLat){                               // the speaker clock is slower than the microphone's: catch up
        rd = wr - uint64_t(m_monRate) * kMonitorTargetLatMs / 1000 * ch;
    }
    size_t need = size_t(frames) * ch;
    size_t avail = size_t(wr - rd);
    size_t done = 0;
    while(need > 0 && avail > 0){
        const size_t piece = std::min(std::min(need, avail), m_monScratch.size());
        for(size_t i = 0; i < piece; ++i)
            m_monScratch[i] = m_ring[size_t((rd + i) & m_ringMask)];
        m_monGain.Process(m_monScratch.data(), uint32_t(piece / ch), m_monCh);
        int16_t *dst = out + done;
        for(size_t i = 0; i < piece; ++i)
            dst[i] = sat(int(dst[i]) + int(m_monScratch[i]));
        rd += piece; done += piece; need -= piece; avail -= piece;
    }
    m_rd.store(rd, std::memory_order_release);
}

} // namespace ravava
