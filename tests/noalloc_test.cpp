// Proves the recorder's AND the player's audio-thread path allocate nothing once they run (global operator new is counted).
// Uses the fake devices and a pass-through EchoCanceller defined HERE that cuts 10 ms blocks exactly like the real
// one (src/echoCanceller.cpp), so the "echo canceller ready" branch of the recorder is exercised too.
// Build (one line):
//   g++ -std=c++17 -O1 -pthread -I/usr/include/opus -Isrc tests/noalloc_test.cpp tests/stub/audioDevice_stub.cpp src/clsAudioRecorder.cpp src/clsAudioPlayer.cpp src/jitterBuffer.cpp src/opusCodec.cpp -lopus -o noalloc_test
#include "clsAudioRecorder.h"
#include "clsAudioPlayer.h"
#include "blockCutter.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <thread>

static std::atomic<long> g_allocs{0};
static std::atomic<bool> g_count{false};
void *operator new(size_t n){ if(g_count) g_allocs++; void *p = std::malloc(n ? n : 1); if(!p) throw std::bad_alloc(); return p; }
void *operator new[](size_t n){ if(g_count) g_allocs++; void *p = std::malloc(n ? n : 1); if(!p) throw std::bad_alloc(); return p; }
void operator delete(void *p) noexcept { std::free(p); }
void operator delete[](void *p) noexcept { std::free(p); }
void operator delete(void *p, size_t) noexcept { std::free(p); }
void operator delete[](void *p, size_t) noexcept { std::free(p); }

// ---- a "ready" canceller that passes the signal through in exact 10 ms blocks
struct EchoCanceller::Impl { std::vector<int16_t> fifo; size_t block = 0; bool ready = false; };
EchoCanceller::EchoCanceller() : d(new Impl) {}
EchoCanceller::~EchoCanceller() {}
bool EchoCanceller::init(int rate, int, int ch, const Options&, std::string*){
    d->block = size_t(rate / 100) * ch; d->fifo.reserve(d->block); d->ready = true; return true; }
bool EchoCanceller::init(int rate, int rr, int ch, std::string *e){ return init(rate, rr, ch, Options(), e); }
bool EchoCanceller::isReady() const { return d->ready; }
void EchoCanceller::render(const int16_t*, uint32_t) {}
void EchoCanceller::capture(const int16_t *pcm, uint32_t frames, Pcm &out){
    cutBlocks(d->fifo, d->block, pcm, size_t(frames), [&](const int16_t *b){ out.insert(out.end(), b, b + d->block); }); }
void EchoCanceller::setStreamDelayMs(int) {}
bool EchoCanceller::getStats(Stats&) const { return false; }

static long runCase(bool useAec, int frameMs, bool dtx)
{
    AudioConfig cfg; cfg.frameMs = frameMs; cfg.opusBitrate = 16000; cfg.useDtx = dtx;
    clsAudioRecorder rec;
    std::atomic<long> packets{0};
    rec.SetPacketHandler([&](const Bytes &d){ if(!d.empty()) packets++; });
    if(useAec){
        auto aec = std::make_shared<EchoCanceller>();
        aec->init(cfg.inputRate, cfg.inputRate, cfg.channels);
        rec.SetEchoCanceller(aec);
    }
    rec.SetConfig(&cfg);
    rec.Start();
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));       // warm-up: first packets may allocate
    g_allocs = 0; g_count = true;
    std::this_thread::sleep_for(std::chrono::milliseconds(3000));
    g_count = false;
    rec.Stop();
    std::printf("  aec=%d frame=%2dms dtx=%d : %ld packets, allocations in steady state: %ld\n",
                useAec, frameMs, dtx, packets.load(), g_allocs.load());
    return g_allocs.load();
}

// recorder -> lossy / reordering "network" -> player: allocations anywhere in the process while it runs
static long runPlayerCase(int frameMs, bool dtx)
{
    AudioConfig cfg; cfg.frameMs = frameMs; cfg.opusBitrate = 16000; cfg.useDtx = dtx;
    clsAudioRecorder rec; clsAudioPlayer play;
    Bytes held; held.reserve(2000);
    bool haveHeld = false;
    unsigned n = 0;
    rec.SetPacketHandler([&](const Bytes &d){
        n++;
        if(n % 10 == 3) return;                                   // 10% lost
        if(n % 13 == 0 && !haveHeld){ held.assign(d.begin(), d.end()); haveHeld = true; return; }   // reordered: after the next one
        play.Play(d);
        if(haveHeld){ play.Play(held); haveHeld = false; }
    });
    rec.SetConfig(&cfg); play.SetConfig(&cfg);
    rec.Start();
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));      // warm-up
    g_allocs = 0; g_count = true;
    std::this_thread::sleep_for(std::chrono::milliseconds(4000));
    g_count = false;
    rec.Stop();
    JitterBuffer::Stats st; int depth, ms;
    play.GetStats(st, depth, ms);
    std::printf("  player frame=%2dms dtx=%d : received %u, played %u, fec %u, plc %u, lost %u, allocations in steady state: %ld\n",
                frameMs, dtx, st.received, st.played, st.fec, st.plc, st.lost, g_allocs.load());
    long bad = g_allocs.load();
    if(st.played < 20){ std::printf("  FAIL: nothing was played\n"); bad++; }
    return bad;
}

int main()
{
    long bad = 0;
    for(bool aec : {false, true})
        for(int ms : {20, 60})
            bad += runCase(aec, ms, false);
    bad += runCase(true, 20, true);
    bad += runPlayerCase(20, false);
    bad += runPlayerCase(60, false);
    bad += runPlayerCase(20, true);
    std::printf(bad ? "FAIL: the audio thread allocates\n" : "PASS: no allocation on the audio thread\n");
    return bad ? 1 : 0;
}
