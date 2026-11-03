// How long does the audio callback take when the network is rough? Real Player + real Opus, fake speaker thread that times every
// callback (tests/stub/audio_device_stub.cpp). The network side hammers Play() with bursts of up to 256 datagrams (the size of
// the PacketQueue, so one callback may have to drain a full queue), a second thread reads GetStats() / sets the volume at ~1 kHz
// (the only place where the audio thread shares a mutex: Player::publishStats() uses try_lock, it never waits, but a reader
// that holds the lock makes it skip, and an unlock with a waiting reader may cost a system call).
// Reports the longest / average callback, how many took more than half of the 10 ms period and how many missed it.
// Limits are loose (a loaded CI machine can preempt any thread): longest < 5 ms, none over the deadline.
// Build: g++ -std=c++17 -O2 -pthread -Ilibs/opus/win32/include -Isrc tests/callback_time_test.cpp tests/stub/audio_device_stub.cpp \
//        src/ravava/Player.cpp src/ravava/JitterBuffer.cpp src/ravava/OpusCodec.cpp src/ravava/AudioProcessor.cpp -lopus -o callback_time_test
// Run:   ./callback_time_test [adaptive|legacy] [seconds]      (default: both, 15 s each)
#define _USE_MATH_DEFINES
#include "ravava/Player.h"
#include "ravava/OpusCodec.h"
#include "ravava/VoicePacket.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <thread>

using namespace ravava;
extern std::atomic<long> g_stubOutCbMaxUs, g_stubOutCbCount, g_stubOutCbOverHalf, g_stubOutCbOverDeadline;

static int run(bool adaptive, int seconds)
{
    g_stubOutCbMaxUs = 0; g_stubOutCbCount = 0; g_stubOutCbOverHalf = 0; g_stubOutCbOverDeadline = 0;
    AudioConfig spk;
    Player player;
    player.SetAdaptivePlayout(adaptive);
    player.SetConfig(&spk);

    AudioConfig cfg; cfg.frameMs = 20; cfg.Bitrate = 24000; cfg.useDtx = false; cfg.useFec = true; cfg.expectedLossPerc = 10;
    OpusCodec enc; enc.InitEncoder(cfg);
    const int fs = cfg.FrameSamples();
    std::vector<Bytes> pool;
    for(int i = 0; i < 50; i++){
        Pcm pcm(static_cast<size_t>(fs), int16_t(0));
        for(int k = 0; k < fs; k++) pcm[size_t(k)] = int16_t(6000 * std::sin(2 * M_PI * (200 + 13 * i) * (k + i * fs) / 48000.0));
        Bytes b; enc.Encode(pcm.data(), fs, b); pool.push_back(b);
    }

    std::atomic<bool> stop{false};
    std::atomic<long> polls{0};
    std::thread gui([&]{                                 // GUI: statistics + volume, as fast as it likes
        JitterBuffer::Stats st; int depth, ms; int v = 0;
        while(!stop){
            player.GetStats(st, depth, ms);
            player.SetVolume(50 + (v++ & 31));
            polls++;
            std::this_thread::sleep_for(std::chrono::microseconds(300));
        }
    });

    std::mt19937 rng(5);
    uint16_t seq = 0; uint32_t tx = 0; Bytes wire;
    long sent = 0, maxBurst = 0;
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while(std::chrono::steady_clock::now() < end){
        // a calm stretch (steady 20 ms packets), then a burst as after a link stall: up to a whole queue at once
        const int calm = 5 + int(rng() % 20);
        for(int i = 0; i < calm && std::chrono::steady_clock::now() < end; i++){
            VoicePacket::WrapInto(wire, seq, seq == 0, uint8_t(tx++ & 0x7F), pool[seq % pool.size()]); seq++;
            player.Play(wire); sent++;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        const int burst = (rng() % 4 == 0) ? 256 : 20 + int(rng() % 120);
        for(int i = 0; i < burst; i++){
            VoicePacket::WrapInto(wire, seq, false, uint8_t(tx++ & 0x7F), pool[seq % pool.size()]); seq++;
            player.Play(wire); sent++;
        }
        maxBurst = std::max<long>(maxBurst, burst);
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
    }
    stop = true; gui.join();

    JitterBuffer::Stats st; int depth = 0, ms = 0;
    player.GetStats(st, depth, ms);
    const long n = g_stubOutCbCount;
    std::printf("%-8s | callbacks %6ld | longest %6ld us | over 5 ms (half) %ld | over 10 ms (deadline) %ld | sent %ld (biggest burst %ld), queue-dropped %u, "
                "GetStats calls %ld\n", adaptive ? "adaptive" : "legacy", n, long(g_stubOutCbMaxUs), long(g_stubOutCbOverHalf),
                long(g_stubOutCbOverDeadline), sent, maxBurst, st.ingestDropped, polls.load());
    int fails = 0;
    if(n < seconds * 50){ std::printf("  FAIL: the speaker callback barely ran (%ld)\n", n); fails++; }
    if(g_stubOutCbOverDeadline != 0){ std::printf("  FAIL: %ld callbacks missed the 10 ms deadline\n", long(g_stubOutCbOverDeadline)); fails++; }
    if(g_stubOutCbMaxUs >= 5000){ std::printf("  FAIL: longest callback %ld us (limit 5000)\n", long(g_stubOutCbMaxUs)); fails++; }
    return fails;
}

int main(int argc, char **argv)
{
    int seconds = 15; bool a = true, l = true;
    for(int i = 1; i < argc; i++){
        if(!std::strcmp(argv[i], "adaptive")) l = false;
        else if(!std::strcmp(argv[i], "legacy")) a = false;
        else seconds = std::atoi(argv[i]);
    }
    int fails = 0;
    if(l) fails += run(false, seconds);
    if(a) fails += run(true, seconds);
    std::printf(fails ? "FAIL\n" : "PASS\n");
    return fails ? 1 : 0;
}
