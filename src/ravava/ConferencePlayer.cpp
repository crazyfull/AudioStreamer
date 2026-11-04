/*
 * RavAva - real-time voice library (Opus over any datagram transport)
 * Copyright (c) 2026 Mojtaba Kahvand. All rights reserved.
 * Author: Mojtaba Kahvand
 *
 * Proprietary software, NOT free / open source. Use, copying, modification and distribution are forbidden without a
 * written license from the author. See the LICENSE file in the repository root.
 * SPDX-License-Identifier: LicenseRef-RavAva-Proprietary
 */
#include "ConferencePlayer.h"
#include <algorithm>
#include <chrono>

namespace ravava {

ConferencePlayer::ConferencePlayer()
{
    for(auto &s : m_slot)
        s.store(nullptr, std::memory_order_relaxed);
    m_hub.SetMixSource(this);
}

ConferencePlayer::~ConferencePlayer()
{
    // the hub is destroyed first (member order) and stops the speaker, so no callback runs while the talkers are freed
}

int64_t ConferencePlayer::nowMs() const
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void ConferencePlayer::SetMaxStreams(int n)
{
    std::lock_guard<std::mutex> lk(m_netMutex);
    m_maxStreams = std::max(1, std::min(MaxStreams, n));
}

void ConferencePlayer::SetConfig(AudioConfig *pConfig)
{
    if(!pConfig)
        return;
    m_pConfig = pConfig;
    m_channels = pConfig->channels;
    // one talker is pulled in chunks of 20 ms: the device may ask for more than that in one callback
    m_chunkFrames = uint32_t(std::max(1, pConfig->RenderRate() / 50));
    m_scratch.assign(size_t(m_chunkFrames) * m_channels, 0);
    m_hub.SetConfig(pConfig);               // opens the speaker
}

ConferencePlayer::Talker *ConferencePlayer::find(uint16_t id) const
{
    for(const Talker &t : m_talkers)
        if(t.used && t.id == id)
            return const_cast<Talker*>(&t);
    return nullptr;
}

// network thread, under m_netMutex
void ConferencePlayer::reapRetired()
{
    if(m_retired.empty())
        return;
    const uint32_t cur = m_cbSeq.load();
    // free what the audio thread cannot be holding any more: it was unpublished while the audio thread was outside MixInto() (even
    // sequence), or the audio thread has left that call since (the sequence moved on)
    m_retired.erase(std::remove_if(m_retired.begin(), m_retired.end(),
                        [cur](Retired &r){ return (r.seq & 1u) == 0 || r.seq != cur; }), m_retired.end());
}

void ConferencePlayer::sweep(int64_t now)
{
    const int idle = m_idleMs.load(std::memory_order_relaxed);
    if(idle > 0 && now - m_lastSweepMs >= 500){
        m_lastSweepMs = now;
        for(int i = 0; i < MaxStreams; ++i){
            Talker &t = m_talkers[i];
            if(t.used && now - t.lastMs > idle){
                m_slot[i].store(nullptr);                       // seq_cst: see reapRetired()
                m_retired.push_back({std::move(t.player), m_cbSeq.load()});
                t.used = false;
                RAVAVA_INFO("conference: talker %u left (silent for %d ms)", unsigned(t.id), int(now - t.lastMs));
            }
        }
    }
    reapRetired();
}

void ConferencePlayer::Play(uint16_t streamId, const uint8_t *datagram, size_t size)
{
    if(!m_pConfig)
        return;
    const int64_t now = nowMs();
    Player *target = nullptr;
    {
        std::lock_guard<std::mutex> lk(m_netMutex);
        sweep(now);
        Talker *t = find(streamId);
        if(!t){
            int free = -1, inUse = 0;
            for(int i = 0; i < MaxStreams; ++i){
                if(m_talkers[i].used) ++inUse;
                else if(free < 0) free = i;
            }
            if(free < 0 || inUse >= m_maxStreams){
                m_rejected.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            // a new talker: its Player is built HERE (allocates) and only then handed to the audio thread
            std::unique_ptr<Player> p(new Player);
            p->SetExternalOutput(true);
            if(m_adaptive.load(std::memory_order_relaxed) >= 0)
                p->SetAdaptivePlayout(m_adaptive.load(std::memory_order_relaxed) != 0);
            p->SetDredPlayback(m_dred.load(std::memory_order_relaxed));
            p->SetDredFloorMs(m_dredFloorMs.load(std::memory_order_relaxed));
            p->SetJitterLimitsMs(m_limMinMs.load(std::memory_order_relaxed), m_limMaxMs.load(std::memory_order_relaxed));
            p->SetConfig(m_pConfig);
            Talker &n = m_talkers[free];
            n.id = streamId;
            n.used = true;
            n.player = std::move(p);
            m_slot[free].store(n.player.get());
            RAVAVA_INFO("conference: talker %u joined (%d in the room)", unsigned(streamId), inUse + 1);
            t = &n;
        }
        t->lastMs = now;
        target = t->player.get();
        // Play() only copies into a lock-free queue; it stays under the mutex so the talker cannot be released meanwhile
        target->Play(datagram, size);
    }
}

// audio thread
void ConferencePlayer::MixInto(int16_t *out, uint32_t frames)
{
    m_cbSeq.fetch_add(1);                                       // odd: inside
    const size_t ch = size_t(m_channels);
    for(int i = 0; i < MaxStreams; ++i){
        Player *p = m_slot[i].load();
        if(!p)
            continue;
        uint32_t done = 0;
        while(done < frames){
            const uint32_t n = std::min(m_chunkFrames, frames - done);
            if(p->Pull(m_scratch.data(), n)){
                int16_t *o = out + size_t(done) * ch;
                const int16_t *s = m_scratch.data();
                for(size_t k = 0, cnt = size_t(n) * ch; k < cnt; ++k){      // saturating add (no wrap-around when several talk)
                    const int v = int(o[k]) + int(s[k]);
                    o[k] = int16_t(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
                }
            }
            done += n;
        }
    }
    m_cbSeq.fetch_add(1);                                       // even: outside
}

bool ConferencePlayer::SetStreamVolume(uint16_t streamId, int percent)
{
    std::lock_guard<std::mutex> lk(m_netMutex);
    Talker *t = find(streamId);
    if(!t)
        return false;
    t->player->SetVolume(percent);
    return true;
}

int ConferencePlayer::GetStreamLevel(uint16_t streamId) const
{
    std::lock_guard<std::mutex> lk(m_netMutex);
    const Talker *t = find(streamId);
    return t ? t->player->GetLevel() : 0;
}

std::vector<uint16_t> ConferencePlayer::ActiveStreams() const
{
    std::vector<uint16_t> v;
    std::lock_guard<std::mutex> lk(m_netMutex);
    for(const Talker &t : m_talkers)
        if(t.used)
            v.push_back(t.id);
    return v;
}

bool ConferencePlayer::GetStreamStats(uint16_t streamId, JitterBuffer::Stats &stats, int &bufferFrames, int &frameMs) const
{
    std::lock_guard<std::mutex> lk(m_netMutex);
    const Talker *t = find(streamId);
    return t && t->player->GetStats(stats, bufferFrames, frameMs);
}

} // namespace ravava
