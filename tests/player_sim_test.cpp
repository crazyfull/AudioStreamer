// The WHOLE receive path in real time: simulated network (loss, jitter) -> Player::Play() (arrival stamp) -> PacketQueue ->
// audio thread -> JitterBuffer -> Opus -> fake speaker. net_sim_test feeds the JitterBuffer directly on a virtual clock;
// this one checks that Player really wires the pieces together: the arrival times reach the jitter buffer (jitter is measured
// only when adaptive playout is on), the depth follows the network, and the speaker keeps playing.
// Real time (~12 s per scenario and mode), so the limits are loose. No sound card (tests/stub/audio_device_stub.cpp).
// Build: g++ -std=c++17 -O2 -pthread -Ilibs/opus/win32/include -Isrc tests/player_sim_test.cpp tests/stub/audio_device_stub.cpp \
//        src/ravava/Player.cpp src/ravava/JitterBuffer.cpp src/ravava/OpusCodec.cpp src/ravava/AudioProcessor.cpp -lopus -o player_sim_test
#define _USE_MATH_DEFINES
#include "ravava/Player.h"
#include "ravava/OpusCodec.h"
#include "ravava/VoicePacket.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <queue>
#include <random>
#include <thread>

using namespace ravava;
using Clock = std::chrono::steady_clock;

extern void (*g_stubSpeakerTap)(const int16_t*, uint32_t, int);
static std::atomic<long> g_callbacks{0}, g_silent{0};       // 10 ms speaker buffers, and those that were all zeros
static void tap(const int16_t *pcm, uint32_t frames, int ch)
{
    bool any = false;
    for(uint32_t i = 0; i < frames * uint32_t(ch) && !any; i++) any = pcm[i] != 0;
    g_callbacks++;
    if(!any) g_silent++;
}

struct Scenario { const char *name; double lossPct, baseMs, jitterMs; double seconds; };
struct Result { JitterBuffer::Stats st; int depth = 0, frameMs = 0; double silentPct = 0; bool ok = false; };

static Result run(const Scenario &sc, bool adaptive)
{
    AudioConfig spk;                                     // 48 kHz mono speaker
    Player player;
    player.SetAdaptivePlayout(adaptive);
    player.SetConfig(&spk);

    AudioConfig cfg; cfg.frameMs = 20; cfg.opusBitrate = 16000; cfg.useDtx = false; cfg.useFec = true; cfg.expectedLossPerc = 10;
    OpusCodec enc; enc.InitEncoder(cfg);
    const int fs = cfg.FrameSamples();
    Pcm pcm(static_cast<size_t>(fs), int16_t(0));
    std::mt19937 rng(7);
    std::normal_distribution<double> gauss(0.0, 1.0);
    std::uniform_real_distribution<double> uni(0.0, 1.0);

    struct Due { Clock::time_point at; Bytes data; bool operator<(const Due &o) const { return at > o.at; } };
    std::priority_queue<Due> net;
    Bytes opus, wire;
    uint16_t seq = 0;
    double ph = 0;

    g_callbacks = 0; g_silent = 0;
    const auto t0 = Clock::now();
    auto nextSend = t0;
    const auto end = t0 + std::chrono::milliseconds(int(sc.seconds * 1000));
    long warmCb = 0, warmSilent = 0;
    bool warmed = false;
    while(Clock::now() < end){
        const auto now = Clock::now();
        if(!warmed && now - t0 > std::chrono::seconds(2)){      // judge the steady state, not the start-up
            warmed = true; warmCb = g_callbacks; warmSilent = g_silent;
        }
        if(now >= nextSend){
            for(int k = 0; k < fs; k++){ pcm[size_t(k)] = int16_t(6000 * std::sin(ph)); ph += 2 * M_PI * 300 / 48000.0; }
            enc.Encode(pcm.data(), fs, opus);
            VoicePacket::WrapInto(wire, seq, seq == 0, uint8_t(seq & 0x7F), opus); seq++;
            if(uni(rng) * 100 >= sc.lossPct){
                const double ms = sc.baseMs + sc.jitterMs * std::fabs(gauss(rng));   // later packets overtake: reordering
                net.push({now + std::chrono::microseconds(int64_t(ms * 1000)), wire});
            }
            nextSend += std::chrono::milliseconds(20);
        }
        while(!net.empty() && net.top().at <= Clock::now()){
            player.Play(net.top().data);
            net.pop();
        }
        std::this_thread::sleep_for(std::chrono::microseconds(500));
    }
    Result r;
    r.ok = player.GetStats(r.st, r.depth, r.frameMs);
    const long cb = g_callbacks - warmCb;
    r.silentPct = cb > 0 ? 100.0 * double(g_silent - warmSilent) / double(cb) : 100.0;
    return r;
}

int main()
{
    g_stubSpeakerTap = tap;
    const Scenario sc[] = {
        {"clean",       0, 40,  0, 12},
        {"jitter 30ms", 0, 40, 30, 12},
        {"loss 5%",     5, 40,  3, 12},
    };
    int fails = 0;
    auto check = [&](bool c, const char *what, const char *name, bool adaptive){
        if(!c){ std::printf("  FAIL [%s, %s]: %s\n", name, adaptive ? "adaptive" : "legacy", what); fails++; }
    };
    std::printf("%-12s %-8s | depth  jitter ms | underrun   plc   fec  lost late | silent%%\n", "scenario", "mode");
    for(const Scenario &s : sc){
        Result byMode[2];
        for(int m = 0; m < 2; m++){
            const bool adaptive = (m == 1);
            Result r = byMode[m] = run(s, adaptive);
            std::printf("%-12s %-8s | %5d %9.1f | %8u %5u %5u %5u %4u | %6.2f\n", s.name, adaptive ? "adaptive" : "legacy",
                        r.depth, r.st.jitterUs / 1000.0, r.st.underrun, r.st.plc, r.st.fec, r.st.lost, r.st.late, r.silentPct);
            check(r.ok && r.frameMs == 20, "no stream", s.name, adaptive);
            check(r.st.received > 400, "packets did not reach the jitter buffer", s.name, adaptive);
            check(r.silentPct < 12.0, "speaker silent too often", s.name, adaptive);
            // the arrival time only reaches the jitter buffer in adaptive mode (this is what the Player wiring is for)
            check(adaptive ? (s.jitterMs < 5 || r.st.jitterUs > 5000) : r.st.jitterUs == 0, "jitter estimate", s.name, adaptive);
        }
        // the estimate follows the network: with 30 ms jitter the adaptive buffer is deeper than on a clean line
        if(&s == &sc[1])
            check(byMode[1].depth >= 3, "adaptive depth did not follow the jitter", s.name, true);
        if(&s == &sc[0])
            check(byMode[1].depth <= 4 && byMode[1].st.underrun == 0, "clean line: shallow and no underrun", s.name, true);
    }
    std::printf(fails ? "FAIL: %d checks\n" : "PASS\n", fails);
    return fails ? 1 : 0;
}
