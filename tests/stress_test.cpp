// Stress / fuzz for the receive side. Meant to run under sanitizers (see tests/README.txt):
//  1) Player: 3 threads call Play() while the sender format flips between frame sizes (stream rebuilds on the network
//     thread, hand-over to the audio thread), a 4th thread polls GetStats / GetLevel / TakePeakLevel / SetVolume,
//     and garbage datagrams are mixed in. No crash, no data race, no leak.
//  2) JitterBuffer: random loss / reorder / duplicates / DTX gaps / sender restarts (sequence back to 0, far jumps).
//     Invariants: every PopFrame is empty or exactly one frame, counters never go crazy, and after a restart the
//     new talker is heard again within a bounded time.
#include "ravava/Player.h"
#include "ravava/JitterBuffer.h"
#include "ravava/OpusCodec.h"
#include "ravava/VoicePacket.h"
#include <atomic>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <memory>
#include <random>
#include <thread>

using namespace ravava;

static int g_fail = 0;
#define CHECK(c) do{ if(!(c)){ std::printf("  FAIL line %d: %s\n", __LINE__, #c); g_fail++; } }while(0)

static Bytes makePacket(OpusCodec &enc, int frameSamples, uint16_t seq, bool marker, uint8_t tx, double &ph, int rate)
{
    std::vector<int16_t> pcm(static_cast<size_t>(frameSamples), int16_t(0));
    for(auto &v : pcm){ v = int16_t(6000.0 * std::sin(ph)); ph += 2 * M_PI * 500 / rate; }
    Bytes opus = enc.Encode(pcm.data(), frameSamples);
    return VoicePacket::Wrap(seq, marker, tx, opus);
}

static void playerStress()
{
    std::printf("Player: concurrent Play() + format flips + polling\n");
    AudioConfig spk;
    Player play; play.SetConfig(&spk);

    struct Fmt { int ms; AudioConfig cfg; OpusCodec enc; };
    static const int msList[] = {10, 20, 40, 60};
    std::vector<std::unique_ptr<Fmt>> fm;
    for(int ms : msList){
        auto f = std::make_unique<Fmt>(); f->ms = ms; f->cfg.frameMs = ms; f->cfg.opusBitrate = 24000;
        CHECK(f->enc.InitEncoder(f->cfg));
        fm.push_back(std::move(f));
    }

    std::atomic<bool> stop{false};
    std::atomic<int> current{1};                 // index into fm: the "real" sender format, changes over time
    std::atomic<long> sent{0};

    auto sender = [&](int id){
        std::mt19937 rng(100 + id);
        // every thread owns its encoders (OpusEncoder is not thread safe)
        std::vector<std::unique_ptr<OpusCodec>> enc;
        for(auto &f : fm){ auto e = std::make_unique<OpusCodec>(); e->InitEncoder(f->cfg); enc.push_back(std::move(e)); }
        uint16_t seq = uint16_t(id * 1000); double ph = 0;
        while(!stop){
            int idx = current.load();
            if(rng() % 9 == 0) idx = int(rng() % fm.size());            // a stray other-format packet
            Bytes p = makePacket(*enc[size_t(idx)], 48 * fm[size_t(idx)]->ms, seq, seq % 50 == 0, uint8_t(seq & 0x7F), ph, 48000);
            seq++;
            play.Play(p);
            sent++;
            if(rng() % 17 == 0){ Bytes junk(size_t(rng() % 40 + 1)); for(auto &b : junk) b = uint8_t(rng()); play.Play(junk); }
            std::this_thread::sleep_for(std::chrono::milliseconds(2 + rng() % 4));
        }
    };
    auto poller = [&]{
        std::mt19937 rng(7);
        while(!stop){
            JitterBuffer::Stats st; int d, f; play.GetStats(st, d, f);
            (void)play.GetLevel(); (void)play.TakePeakLevel();
            play.SetVolume(int(rng() % 101));
            std::this_thread::sleep_for(std::chrono::milliseconds(3));
        }
    };

    std::thread t0(sender, 0), t1(sender, 1), t2(sender, 2), tp(poller);
    for(int i = 0; i < 25; i++){                                        // flip the "real" format every 200 ms
        current = i % int(fm.size());
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    stop = true;
    t0.join(); t1.join(); t2.join(); tp.join();
    JitterBuffer::Stats st; int d = 0, f = 0;
    CHECK(play.GetStats(st, d, f));
    std::printf("  sent %ld packets\n", sent.load());
    std::printf("  final: played %u frames, format-refused %u, stream %d ms\n", st.played, st.formatDropped, f);
    CHECK(st.formatDropped > 100);                  // strays + garbage were refused, not decoded

    // after the chaos the player must still work: one clean 20 ms talker has to be heard
    // (the chaos above mixes three unrelated sequence numbers into one stream, so what was played during it means little)
    const uint32_t playedBefore = st.played;
    {
        AudioConfig c; c.frameMs = 20; c.opusBitrate = 24000;
        OpusCodec enc; enc.InitEncoder(c);
        double ph = 0; uint16_t seq = 40000;
        play.TakePeakLevel();
        for(int i = 0; i < 100; i++){                               // 2 s in real time
            Bytes p = makePacket(enc, 960, seq, seq == 40000, uint8_t(seq & 0x7F), ph, 48000); seq++;
            play.Play(p);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
    CHECK(play.GetStats(st, d, f));
    std::printf("  after the chaos, one clean talker: played %u more frames, stream %d ms, level peak %d\n",
                st.played - playedBefore, f, play.TakePeakLevel());
    CHECK(f == 20 && st.played - playedBefore > 60);
}

static void jitterFuzz()
{
    std::printf("JitterBuffer: random loss / reorder / DTX / restarts\n");
    const int rate = 48000;
    for(int ms : {10, 20, 60}){
        for(int seed = 1; seed <= 10; seed++){
            std::mt19937 rng(unsigned(seed * 7919 + ms));
            AudioConfig cfg; cfg.frameMs = ms; cfg.opusBitrate = 24000; cfg.useDtx = true;
            OpusCodec enc, dec; enc.InitEncoder(cfg); dec.InitDecoder(rate, 1);
            JitterBuffer jb(&dec, rate, 1, ms);
            const int N = rate * ms / 1000;
            double ph = 0;
            uint16_t seq = uint16_t(rng()); uint8_t tx = 0; bool marker = true;
            std::vector<Bytes> held;
            Pcm out;
            int64_t heardAfterRestart = -1, stepsSinceRestart = -1;
            for(int step = 0; step < 2000; step++){
                const unsigned r = rng() % 1000;
                if(r < 3){                                              // sender restarts: sequence jumps (back to 0 or anywhere)
                    seq = (rng() % 2) ? uint16_t(0) : uint16_t(rng()); tx = 0; marker = true;
                    stepsSinceRestart = 0; heardAfterRestart = jb.GetStats().played;
                }
                const bool silent = (rng() % 100) < 12;                 // DTX: this frame is not sent, but consumes a seq
                if(silent){ seq++; marker = true; }
                else{
                    Bytes p = makePacket(enc, N, seq, marker, tx, ph, rate);
                    seq++; tx = uint8_t((tx + 1) & VoicePacket::TxMask); marker = false;
                    const unsigned q = rng() % 100;
                    if(q < 8) { /* lost */ }
                    else if(q < 16) held.push_back(p);                  // reordered: delivered a few steps later
                    else { jb.Push(p.data(), p.size()); if(q < 19) jb.Push(p.data(), p.size()); }   // sometimes duplicated
                }
                if(!held.empty() && rng() % 3 == 0){ jb.Push(held.front().data(), held.front().size()); held.erase(held.begin()); }
                if(held.size() > 6) held.clear();
                // one frame per step, like the speaker pulling
                const bool got = jb.PopFrame(out);
                if(got) CHECK(out.size() == size_t(N));
                if(stepsSinceRestart >= 0){
                    stepsSinceRestart++;
                    if(stepsSinceRestart == 150){                       // within 150 frames of a restart audio must flow again
                        CHECK(jb.GetStats().played > heardAfterRestart);
                        stepsSinceRestart = -1;
                    }
                }
            }
            const JitterBuffer::Stats &s = jb.GetStats();
            CHECK(s.received > 1000);
            CHECK(s.played > 500);
            CHECK(jb.TargetDepth() >= 1 && jb.TargetDepth() < 200);
        }
    }
    std::printf("  30 randomized runs done\n");
}

int main()
{
    jitterFuzz();
    playerStress();
    std::printf(g_fail ? "FAILED (%d)\n" : "PASS\n", g_fail);
    return g_fail ? 1 : 0;
}
