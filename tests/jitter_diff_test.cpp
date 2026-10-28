// Differential test: the real (fixed-slot, allocation-free) JitterBuffer against the old std::map based
// implementation kept in tests/ref/. Same random network (loss, reordering, duplicates, DTX silence with
// markers, sender restarts) goes into both; every PopFrame() result and every statistic must be identical.
// Build:
//   g++ -std=c++17 -O1 -pthread -I/usr/include/opus -Isrc -Itests/ref tests/jitter_diff_test.cpp tests/ref/refJitterBuffer.cpp src/ravava/JitterBuffer.cpp src/ravava/OpusCodec.cpp -lopus -o jitter_diff_test
#include "ravava/JitterBuffer.h"
#include "refJitterBuffer.h"
#include "ravava/VoicePacket.h"
#include <cmath>
#include <cstdio>
#include <deque>
#include <random>

using namespace ravava;

static bool sameStats(const JitterBuffer::Stats &a, const RefJitterBuffer::Stats &b)
{
    return a.received == b.received && a.played == b.played && a.fec == b.fec && a.plc == b.plc && a.late == b.late &&
           a.skipped == b.skipped && a.underrun == b.underrun && a.dtxJump == b.dtxJump && a.lost == b.lost &&
           a.reordered == b.reordered && a.lostEvents == b.lostEvents;
}

struct Case { int frameMs; int lossPct; int reorderDepth; int dupPct; bool dtx; bool restarts; int stallPct; };

static int runCase(const Case &c, unsigned seed, long &pushes, long &pops)
{
    std::mt19937 rng(seed);
    auto chance = [&](int pct){ return int(rng() % 100) < pct; };

    // a pool of real Opus packets (the decoder state evolves, so decoding must really agree)
    AudioConfig cfg; cfg.frameMs = c.frameMs; cfg.opusBitrate = 12000; cfg.useDtx = false;
    OpusCodec enc; if(!enc.InitEncoder(cfg)){ std::printf("encoder init failed\n"); return 1; }
    const int fs = cfg.FrameSamples();
    std::vector<Bytes> pool;
    for(int i = 0; i < 64; i++){
        Pcm pcm(static_cast<size_t>(fs), 0);
        for(int k = 0; k < fs; k++) pcm[size_t(k)] = int16_t(6000 * std::sin(2 * M_PI * (300 + 17 * i) * (k + i * fs) / 48000.0));
        Bytes b; enc.Encode(pcm.data(), fs, b); pool.push_back(b);
    }

    OpusCodec decA, decB; decA.InitDecoder(48000, 1); decB.InitDecoder(48000, 1);
    JitterBuffer a(&decA, 48000, 1, c.frameMs);
    RefJitterBuffer b(&decB, 48000, 1, c.frameMs);

    std::deque<Bytes> held;                 // packets delayed by the "network"
    std::deque<int> heldDelay;
    uint16_t seq = uint16_t(rng()); uint8_t tx = uint8_t(rng() & 127);
    bool marker = true, talking = true;
    int step = 0;

    auto feed = [&](const Bytes &d){
        a.Push(d.data(), d.size()); b.Push(d.data(), d.size()); pushes++;
    };
    auto popBoth = [&]() -> int {
        Pcm pa, pb;
        bool ra = a.PopFrame(pa), rb = b.PopFrame(pb);
        pops++;
        if(ra != rb || pa != pb || !sameStats(a.GetStats(), b.GetStats()) || a.TargetDepth() != b.TargetDepth()){
            std::printf("  MISMATCH seed=%u frameMs=%d step=%d: ret %d/%d size %zu/%zu stats played %u/%u plc %u/%u fec %u/%u lost %u/%u late %u/%u target %d/%d\n",
                seed, c.frameMs, step, ra, rb, pa.size(), pb.size(), a.GetStats().played, b.GetStats().played, a.GetStats().plc, b.GetStats().plc,
                a.GetStats().fec, b.GetStats().fec, a.GetStats().lost, b.GetStats().lost, a.GetStats().late, b.GetStats().late, a.TargetDepth(), b.TargetDepth());
            return 1;
        }
        return 0;
    };

    const int frames = 6000;
    for(step = 0; step < frames; step++){
        // talk / silence phases (DTX only makes silence skip packets)
        if(c.dtx && chance(2)) talking = !talking;

        // sender restart: sequence jumps far away (> 3000) -> both buffers must resync
        if(c.restarts && chance(1) && step > 100 && step % 997 == 0){ seq = uint16_t(seq + 5000 + rng() % 20000); marker = true; }

        const uint16_t thisSeq = seq++;           // every frame uses a number
        if(c.dtx && !talking){ marker = true; }
        else{
            Bytes d = VoicePacket::Wrap(thisSeq, marker, tx++, pool[size_t(step) % pool.size()]);
            marker = false;
            // network
            if(chance(c.lossPct)){ /* lost */ }
            else if(c.reorderDepth > 0 && chance(15)){ held.push_back(d); heldDelay.push_back(1 + int(rng() % unsigned(c.reorderDepth))); }
            else{
                feed(d);
                if(chance(c.dupPct)) feed(d);     // duplicate
            }
        }
        // release delayed packets
        for(size_t i = 0; i < held.size(); ){
            if(--heldDelay[i] <= 0){ feed(held[i]); held.erase(held.begin() + long(i)); heldDelay.erase(heldDelay.begin() + long(i)); }
            else i++;
        }
        // the speaker pulls a frame every frameMs; sometimes the consumer stalls (several frames at once later)
        int pulls = chance(c.stallPct) ? 0 : 1;
        if(pulls == 0 && chance(50)) pulls = 2;
        for(int p = 0; p < pulls; p++) if(popBoth()) return 1;
    }
    return 0;
}

int main()
{
    const Case cases[] = {
        // frameMs loss reorder dup dtx restarts stall
        {20,  0, 0,  0, false, false, 0},
        {20, 10, 0,  0, false, false, 0},
        {20, 10, 6,  2, false, false, 5},
        {20, 30, 8,  3, false, false, 10},
        {20, 10, 6,  2, true,  false, 5},
        {20, 15, 10, 2, true,  true,  8},
        {60, 10, 4,  2, false, false, 5},
        {60, 20, 4,  2, true,  true,  10},
        {10, 10, 12, 2, true,  true,  10},
        {40,  5, 3,  1, true,  false, 3},
    };
    long pushes = 0, pops = 0; int bad = 0, runs = 0;
    for(const Case &c : cases)
        for(unsigned seed = 1; seed <= 25; seed++){ bad += runCase(c, seed, pushes, pops); runs++; }
    std::printf("%d runs, %ld pushes, %ld pops compared\n", runs, pushes, pops);
    std::printf(bad ? "FAIL: %d runs differ\n" : "PASS: identical behaviour\n", bad);
    return bad ? 1 : 0;
}
