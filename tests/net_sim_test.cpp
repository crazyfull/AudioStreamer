// Network simulation with a VIRTUAL clock (no sleeping, no sound card, real libopus packets): the same JitterBuffer the
// Player uses is fed by a simulated network (random loss, burst loss, jitter, link stalls, latency step, clock drift)
// and pulled once per frame by a simulated speaker. Every scenario prints what the listener would experience:
//   delay   : extra latency the jitter buffer adds (avg / p95 / max), in ms
//   bad%    : share of frames that were NOT real or FEC-recovered audio (PLC + silence + skipped)
//   events  : underruns, PLC frames, FEC frames, silent pops after start, skipped (dropped) frames, late packets
// Build:
//   g++ -std=c++17 -O2 -pthread -Ilibs/opus/win32/include -Isrc tests/net_sim_test.cpp src/ravava/JitterBuffer.cpp src/ravava/OpusCodec.cpp -lopus -o net_sim_test
// Run:  ./net_sim_test [legacy|timed] [speech] [--check] [name-filter]   (speech = speech-like signal with pauses instead of a steady tone)
//   legacy = Push(datagram, size) (no arrival time: the old reactive buffer),
//   timed  = SetAdaptive(true) + Push(datagram, size, arrivalUs) (what Player does).
//   --check turns the acceptance thresholds into the exit code.
#define _USE_MATH_DEFINES      // M_PI on MSVC
#include "ravava/JitterBuffer.h"
#include "ravava/VoicePacket.h"
#include "ravava/AudioConfig.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

using namespace ravava;

struct Scenario {
    const char *name;
    double lossPct;                     // independent loss, percent
    double burstStartPct;               // chance per packet that a loss burst starts, percent
    int burstMin, burstMax;             // burst length (packets)
    double baseMs, jitterMs;            // delay = base + jitter * |gauss|  (packets overtake each other = reordering)
    double stallEverySec, stallMs;      // link stall: everything sent during it arrives together at its end (0 = none)
    double stepAtSec, stepMs, stepForSec;   // extra delay for a while, then back to normal
    double driftPpm;                    // speaker clock faster (+) / slower (-) than the sender's
    double seconds;
    // acceptance thresholds (only used with --check): max bad%, max p95 delay in ms
    double maxBadPct, maxP95Ms;
};

struct Result {
    double avgMs = 0, p95Ms = 0, maxMs = 0, badPct = 0;
    JitterBuffer::Stats st;
    uint32_t silent = 0;                // PopFrame() returned false after the first frame was played
    int finalTarget = 0;
};

struct Arrival { int64_t atUs; int idx; bool operator<(const Arrival &o) const { return atUs < o.atUs || (atUs == o.atUs && idx < o.idx); } };

static bool g_speech = false;       // "speech" argument: talk bursts with syllable-like modulation, pauses with faint background noise

// 10 s of a speech-like signal: words of 0.3-1.4 s (harmonic tone, 4-5 Hz syllable modulation) separated by pauses of 0.1-0.7 s
// with faint noise (quiet frames, as the playout control looks for them). Deterministic.
static void speechFrames(int fs, int frames, std::vector<Pcm> &out)
{
    std::mt19937 r(12345);
    std::uniform_real_distribution<double> u(0.0, 1.0);
    std::normal_distribution<double> g(0.0, 1.0);
    out.assign(size_t(frames), Pcm(size_t(fs), int16_t(0)));
    int f = 0; double ph = 0, mph = 0;
    while(f < frames){
        const int word = int((0.3 + 1.1 * u(r)) * 50), pause = int((0.1 + 0.6 * u(r)) * 50);
        const double f0 = 100 + 120 * u(r);
        for(int i = 0; i < word && f < frames; i++, f++)
            for(int k = 0; k < fs; k++){
                ph += 2 * M_PI * f0 / 48000.0; mph += 2 * M_PI * (4.5 + u(r) * 0.0) / 48000.0;
                const double env = 0.55 + 0.45 * std::sin(mph);
                const double v = std::sin(ph) + 0.5 * std::sin(2 * ph) + 0.3 * std::sin(3 * ph) + 0.15 * std::sin(5 * ph);
                out[size_t(f)][size_t(k)] = int16_t(3500 * env * v + 25 * g(r));
            }
        for(int i = 0; i < pause && f < frames; i++, f++)
            for(int k = 0; k < fs; k++)
                out[size_t(f)][size_t(k)] = int16_t(12 * g(r));
    }
}

static Result run(const Scenario &sc, bool timed, unsigned seed)
{
    std::mt19937 rng(seed);
    std::normal_distribution<double> gauss(0.0, 1.0);
    std::uniform_real_distribution<double> uni(0.0, 1.0);

    AudioConfig cfg; cfg.frameMs = 20; cfg.opusBitrate = 16000; cfg.useDtx = false; cfg.useFec = true; cfg.expectedLossPerc = 10;
    OpusCodec enc; enc.InitEncoder(cfg);
    const int fs = cfg.FrameSamples();
    std::vector<Bytes> pool;
    if(g_speech){
        std::vector<Pcm> sp; speechFrames(fs, 500, sp);
        for(Pcm &pcm : sp){ Bytes b; enc.Encode(pcm.data(), fs, b); pool.push_back(b); }
    }else{
    for(int i = 0; i < 64; i++){
        Pcm pcm(size_t(fs), 0);
        for(int k = 0; k < fs; k++) pcm[size_t(k)] = int16_t(6000 * std::sin(2 * M_PI * (300 + 17 * i) * (k + i * fs) / 48000.0));
        Bytes b; enc.Encode(pcm.data(), fs, b); pool.push_back(b);
    }
    }

    const double frameUs = 20000.0;
    const int total = int(sc.seconds * 1000000.0 / frameUs);
    std::vector<Bytes> wire(static_cast<size_t>(total));
    std::vector<Arrival> arr;
    arr.reserve(size_t(total));

    // ---- the network
    uint16_t seq0 = 65400;                                  // crosses the 16 bit wrap
    int burstLeft = 0;
    double nextStall = sc.stallEverySec > 0 ? -std::log(1.0 - uni(rng)) * sc.stallEverySec : 1e18;
    double stallEnd = -1;
    for(int i = 0; i < total; i++){
        const double sendS = i * frameUs / 1e6;
        wire[size_t(i)] = VoicePacket::Wrap(uint16_t(seq0 + i), i == 0, uint8_t(i & 127), pool[size_t(i) % pool.size()]);
        if(burstLeft > 0){ burstLeft--; continue; }
        if(sc.burstStartPct > 0 && uni(rng) * 100 < sc.burstStartPct){
            burstLeft = sc.burstMin + int(rng() % unsigned(sc.burstMax - sc.burstMin + 1)) - 1;
            continue;
        }
        if(uni(rng) * 100 < sc.lossPct) continue;
        double delayMs = sc.baseMs + sc.jitterMs * std::fabs(gauss(rng));
        if(sc.stepMs > 0 && sendS >= sc.stepAtSec && sendS < sc.stepAtSec + sc.stepForSec) delayMs += sc.stepMs;
        double arriveS = sendS + delayMs / 1000.0;
        if(sendS >= nextStall && stallEnd < 0){ stallEnd = sendS + sc.stallMs / 1000.0; }
        if(stallEnd >= 0){
            if(sendS < stallEnd) arriveS = std::max(arriveS, stallEnd + 0.0002 * (i % 50));    // the link is blocked: released in a burst
            else { stallEnd = -1; nextStall = sendS - std::log(1.0 - uni(rng)) * sc.stallEverySec; }
        }
        arr.push_back({int64_t(arriveS * 1e6), i});
    }
    std::stable_sort(arr.begin(), arr.end());

    // ---- the receiver: speaker pulls one frame per frame period of ITS clock
    OpusCodec dec; dec.InitDecoder(48000, 1);
    JitterBuffer jb(&dec, 48000, 1, 20);
    if(timed) jb.SetAdaptive(true);
    const double pullUs = frameUs / (1.0 + sc.driftPpm * 1e-6);
    std::vector<int> delays;
    delays.reserve(size_t(total));
    Result r;
    Pcm pcm;
    bool started = false;
    size_t ai = 0;
    const int pulls = int(total * (1.0 + sc.driftPpm * 1e-6));
    const int64_t offsetUs = 0;
    uint32_t okFrames = 0, playedPrev = 0;
    for(int k = 0; k < pulls; k++){
        const int64_t nowUs = offsetUs + int64_t(k * pullUs);
        while(ai < arr.size() && arr[ai].atUs <= nowUs){
            const Bytes &d = wire[size_t(arr[ai].idx)];
            if(timed) jb.Push(d.data(), d.size(), arr[ai].atUs);
            else      jb.Push(d.data(), d.size());
            ai++;
        }
        const bool got = jb.PopFrame(pcm);
        if(got){
            started = true;
            okFrames++;
            delays.push_back(jb.DelayFrames() * 20);
        }else if(started){
            r.silent++;
        }
        (void)playedPrev;
    }
    r.st = jb.GetStats();
    r.finalTarget = jb.TargetDepth();
    if(!delays.empty()){
        double sum = 0; int mx = 0;
        for(int d : delays){ sum += d; mx = std::max(mx, d); }
        r.avgMs = sum / double(delays.size());
        r.maxMs = mx;
        std::sort(delays.begin(), delays.end());
        r.p95Ms = delays[size_t(double(delays.size()) * 0.95)];
    }
    const double bad = double(r.silent) + double(r.st.plc) + double(r.st.skipped) + double(r.st.stretched);
    r.badPct = 100.0 * bad / double(pulls);
    return r;
}

int main(int argc, char **argv)
{
    bool timed = false, check = false;
    const char *only = nullptr;             // any other argument: run only the scenarios whose name contains it
    for(int i = 1; i < argc; i++){
        if(!std::strcmp(argv[i], "timed")) timed = true;
        else if(!std::strcmp(argv[i], "legacy")) timed = false;
        else if(!std::strcmp(argv[i], "--check")) check = true;
        else if(!std::strcmp(argv[i], "speech")) g_speech = true;
        else only = argv[i];
    }

    //                name              loss burst%  bmin bmax base jit  stallEvery stallMs stepAt stepMs stepFor drift  sec   maxBad maxP95
    const Scenario sc[] = {
        {"clean",                     0,   0,    0,   0,   40,  0,    0,   0,     0,   0,   0,     0,    120, 0.2,  140},
        {"loss 5%",                   5,   0,    0,   0,   40,  3,    0,   0,     0,   0,   0,     0,    120, 6.0,  160},
        {"loss 20%",                 20,   0,    0,   0,   40,  3,    0,   0,     0,   0,   0,     0,    120, 25,   160},
        {"burst loss 3-10",           1,   1.0,  3,  10,   40,  3,    0,   0,     0,   0,   0,     0,    120, 12,   160},
        {"jitter 10ms",               0,   0,    0,   0,   40,  10,   0,   0,     0,   0,   0,     0,    120, 1.0,  160},
        {"jitter 30ms",               0,   0,    0,   0,   40,  30,   0,   0,     0,   0,   0,     0,    120, 3.0,  220},
        {"jitter 60ms",               0,   0,    0,   0,   40,  60,   0,   0,     0,   0,   0,     0,    120, 6.0,  300},
        {"3G: jitter 25 + stalls",    2,   0,    0,   0,   80,  25,   12,  300,   0,   0,   0,     0,    240, 8.0,  300},
        {"latency step +150ms 20s",   0,   0,    0,   0,   40,  5,    0,   0,     60,  150, 20,    0,    180, 4.0,  220},
        {"drift speaker +300ppm",     0,   0,    0,   0,   40,  3,    0,   0,     0,   0,   0,     300,  600, 1.0,  200},
        {"drift speaker -300ppm",     0,   0,    0,   0,   40,  3,    0,   0,     0,   0,   0,    -300,  600, 1.0,  260},
    };

    std::printf("mode: %s\n", timed ? "timed (arrival time given)" : "legacy (no arrival time)");
    std::printf("%-26s | delay ms avg/p95/max | bad%%  | underrun  plc  fec silent skipped stretch forced late | target\n", "scenario");
    int fails = 0;
    for(const Scenario &s : sc){
        if(only && !std::strstr(s.name, only))
            continue;
        // average over a few seeds so one lucky run doesn't decide
        const int seeds = 5;
        Result agg; double a = 0, p = 0, m = 0, b = 0;
        uint32_t ur = 0, plc = 0, fec = 0, sil = 0, skp = 0, late = 0, str = 0, frc = 0; int tg = 0;
        for(int sd = 1; sd <= seeds; sd++){
            Result r = run(s, timed, unsigned(sd));
            a += r.avgMs; p += r.p95Ms; m = std::max(m, r.maxMs); b += r.badPct;
            ur += r.st.underrun; plc += r.st.plc; fec += r.st.fec; sil += r.silent; skp += r.st.skipped; str += r.st.stretched; frc += r.st.slipForced; late += r.st.late; tg += r.finalTarget;
        }
        a /= seeds; p /= seeds; b /= seeds;
        const bool ok = b <= s.maxBadPct && p <= s.maxP95Ms;
        std::printf("%-26s | %6.0f %6.0f %6.0f   | %5.2f | %6u %5u %5u %5u %6u %7u %6u %4u | %4.1f  %s\n", s.name, a, p, m, b,
                    ur / seeds, plc / seeds, fec / seeds, sil / seeds, skp / seeds, str / seeds, frc / seeds, late / seeds, double(tg) / seeds,
                    check ? (ok ? "ok" : "FAIL") : "");
        if(!ok) fails++;
    }
    if(check){
        std::printf(fails ? "FAIL: %d scenarios outside the limits\n" : "PASS\n", fails);
        return fails ? 1 : 0;
    }
    return 0;
}
