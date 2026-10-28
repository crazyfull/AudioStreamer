// Proves the recorder's AND the player's audio-thread path allocate nothing once they run (global operator new is counted).
// Uses the fake devices and a pass-through AudioProcessor defined HERE that cuts 10 ms blocks exactly like the real
// one (src/ravava/AudioProcessor.cpp), so the "echo canceller ready" branch of the recorder is exercised too.
// Build (one line):
//   g++ -std=c++17 -O1 -pthread -I/usr/include/opus -Isrc tests/noalloc_test.cpp tests/stub/audio_device_stub.cpp src/ravava/Recorder.cpp src/ravava/Player.cpp src/ravava/JitterBuffer.cpp src/ravava/OpusCodec.cpp -lopus -o noalloc_test
#include "ravava/Recorder.h"
#include "ravava/Player.h"
#include "ravava/net/PacketRouter.h"
#include "ravava/BlockCutter.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <thread>

using namespace ravava;

static std::atomic<long> g_allocs{0};
static std::atomic<bool> g_count{false};
void *operator new(size_t n){ if(g_count) g_allocs++; void *p = std::malloc(n ? n : 1); if(!p) throw std::bad_alloc(); return p; }
void *operator new[](size_t n){ if(g_count) g_allocs++; void *p = std::malloc(n ? n : 1); if(!p) throw std::bad_alloc(); return p; }
void operator delete(void *p) noexcept { std::free(p); }
void operator delete[](void *p) noexcept { std::free(p); }
void operator delete(void *p, size_t) noexcept { std::free(p); }
void operator delete[](void *p, size_t) noexcept { std::free(p); }

namespace ravava {
// ---- a "ready" canceller that passes the signal through in exact 10 ms blocks
struct AudioProcessor::Impl { std::vector<int16_t> fifo; size_t block = 0; bool ready = false; };
bool AudioProcessor::IsAvailable() { return true; }
AudioProcessor::AudioProcessor() : d(new Impl) {}
AudioProcessor::~AudioProcessor() {}
bool AudioProcessor::Init(int rate, int, int ch, const Options&, std::string*){
    d->block = size_t(rate / 100) * ch; d->fifo.reserve(d->block); d->ready = true; return true; }
bool AudioProcessor::Init(int rate, int rr, int ch, std::string *e){ return Init(rate, rr, ch, Options(), e); }
bool AudioProcessor::IsReady() const { return d->ready; }
void AudioProcessor::Render(const int16_t*, uint32_t) {}
void AudioProcessor::Capture(const int16_t *pcm, uint32_t frames, Pcm &out){
    CutBlocks(d->fifo, d->block, pcm, size_t(frames), [&](const int16_t *b){ out.insert(out.end(), b, b + d->block); }); }
void AudioProcessor::SetStreamDelayMs(int) {}
bool AudioProcessor::GetStats(Stats&) const { return false; }
} // namespace ravava

static long runCase(bool useAec, int frameMs, bool dtx)
{
    AudioConfig cfg; cfg.frameMs = frameMs; cfg.opusBitrate = 16000; cfg.useDtx = dtx;
    Recorder rec;
    std::atomic<long> packets{0};
    rec.SetPacketHandler([&](const Bytes &d){ if(!d.empty()) packets++; });
    if(useAec){
        auto aec = std::make_shared<AudioProcessor>();
        aec->Init(cfg.inputRate, cfg.inputRate, cfg.channels);
        rec.SetAudioProcessor(aec);
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

// recorder -> UDP envelope -> lossy / reordering "network" -> PacketRouter -> player.
// Allocations anywhere in the process while it runs; also checks that the router hands only audio to the audio layer.
static long runPlayerCase(int frameMs, bool dtx)
{
    AudioConfig cfg; cfg.frameMs = frameMs; cfg.opusBitrate = 16000; cfg.useDtx = dtx;
    Recorder rec; Player play;
    PacketRouter router;
    const uint16_t streamId = 0x1234;
    unsigned audioPackets = 0, wrongStream = 0, controlSent = 0;
    router.SetHandler(Packet::Audio, [&](const Packet::Header &h, const uint8_t *payload, size_t size){
        audioPackets++;
        if(h.streamId != streamId) wrongStream++;
        play.Play(payload, size);
    });

    const Bytes control = {2, 0x34, 0x12, 1, 2, 3};               // a packet of another type (2): must never reach the audio layer
    Bytes wire; wire.reserve(2000);
    Bytes held; held.reserve(2000);
    bool haveHeld = false;
    unsigned n = 0;
    rec.SetPacketHandler([&](const Bytes &body){
        n++;
        Packet::Header h; h.type = Packet::Audio; h.streamId = streamId;
        Packet::WrapInto(wire, h, body);
        if(n % 7 == 0){ controlSent++; router.Dispatch(control); }
        if(n % 10 == 3) return;                                   // 10% lost
        if(n % 13 == 0 && !haveHeld){ held.assign(wire.begin(), wire.end()); haveHeld = true; return; }   // reordered: after the next one
        router.Dispatch(wire);
        if(haveHeld){ router.Dispatch(held); haveHeld = false; }
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
    std::printf("  player frame=%2dms dtx=%d : received %u, played %u, fec %u, plc %u, lost %u, router: audio %u, other types %u, allocations in steady state: %ld\n",
                frameMs, dtx, st.received, st.played, st.fec, st.plc, st.lost, router.GetStats().dispatched, router.GetStats().unhandled, g_allocs.load());
    long bad = g_allocs.load();
    if(st.played < 20){ std::printf("  FAIL: nothing was played\n"); bad++; }
    if(wrongStream){ std::printf("  FAIL: wrong stream id at the audio handler\n"); bad++; }
    if(router.GetStats().unhandled != controlSent || router.GetStats().malformed != 0){ std::printf("  FAIL: router counters\n"); bad++; }
    // everything the router called "audio" arrived in the jitter buffer, nothing else did
    if(st.received > audioPackets || audioPackets - st.received > 3){ std::printf("  FAIL: jitter buffer got %u, router sent %u audio packets\n", st.received, audioPackets); bad++; }
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
