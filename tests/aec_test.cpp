// Echo canceller test with the REAL webrtc-audio-processing, no sound card needed.
//
// A speech-like "far end" signal is played (render) and comes back through a simulated room
// (60 ms delay + a few reflections) into the "mic" (capture). Phase 1: only echo, phase 2: the
// near-end person talks at the same time (double talk).
//
// Build: see tests/README.txt        Run: ./aec_test [renderRate] [noref]
//   renderRate  speaker rate, must divide 48000 (default 48000; try 24000 16000)
//   noref       control run: the far end is NOT given to the canceller, so nothing can be removed.
//               It must show ~0 dB attenuation, which proves the normal run measures the canceller.
#include "AudioProcessor.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

static const int    kCapRate   = 48000;
static const double kPi        = 3.14159265358979323846;

// voiced "speech": harmonics of a slowly moving pitch, syllable envelope, pauses between words
struct VoiceGen
{
    double f0Base, f0Mod, syllHz, phase = 0, t = 0, gate = 1, nextGate = 0, amp;
    std::mt19937 rng;
    std::normal_distribution<double> nd{0.0, 1.0};
    VoiceGen(double f0, double mod, double syll, double amplitude, unsigned seed)
        : f0Base(f0), f0Mod(mod), syllHz(syll), amp(amplitude), rng(seed) {}

    double next()
    {
        const double dt = 1.0 / kCapRate;
        t += dt;
        if(t >= nextGate){
            gate = (rng() % 4 == 0) ? 0.0 : 1.0;                         // ~25% pauses
            nextGate = t + 0.4 + (rng() % 1000) / 1000.0;
        }
        double f0 = f0Base + f0Mod * std::sin(2 * kPi * 0.7 * t);
        phase += 2 * kPi * f0 * dt;
        double s = 0;
        for(int h = 1; h <= 30; ++h){
            double hz = f0 * h;
            if(hz > 3800) break;
            double formant = 1.0 / h + 1.5 * std::exp(-std::pow((hz - 700) / 250.0, 2)) + 1.0 * std::exp(-std::pow((hz - 1800) / 350.0, 2));
            s += formant * std::sin(h * phase);
        }
        double env = std::pow(std::fmax(0.0, std::sin(2 * kPi * syllHz * t)), 1.3);
        return amp * 0.25 * s * env * gate + nd(rng) * 15.0;
    }
};

static double energy(const int16_t *p, size_t n)
{
    double e = 0;
    for(size_t i = 0; i < n; ++i) e += double(p[i]) * p[i];
    return e;
}

static int16_t clip(double v)
{
    return int16_t(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
}

int main(int argc, char **argv)
{
    // arguments in any order: "noref" and/or a render rate
    int renderRate = 48000;
    bool noref = false;
    for(int i = 1; i < argc; i++){
        if(std::string(argv[i]) == "noref") noref = true;
        else renderRate = std::atoi(argv[i]);
    }
    if(renderRate <= 0 || kCapRate % renderRate != 0){ std::printf("renderRate must divide 48000\n"); return 2; }
    const int decim = kCapRate / renderRate;

    // pass-through sanity: not initialized -> output == input
    {
        AudioProcessor dead;
        int16_t in[300]; for(int i = 0; i < 300; ++i) in[i] = int16_t(i);
        Pcm out; dead.capture(in, 300, out);
        if(out.size() != 300 || out[299] != 299){ std::printf("FAIL: uninitialized pass-through\n"); return 1; }
    }

    AudioProcessor aec;
    std::string err;
    AudioProcessor::Options opt; opt.streamDelayMs = 50;
    if(!aec.init(kCapRate, renderRate, 1, opt, &err)){ std::printf("FAIL: init: %s\n", err.c_str()); return 1; }

    // bad parameters must be rejected, not crash
    { AudioProcessor x; if(x.init(44123, 48000, 1) || x.init(48000, 48000, 5)){ std::printf("FAIL: bad params accepted\n"); return 1; } }

    const double totalSec = 14, phase1End = 8;
    const int ticks = int(totalSec * 100);
    const int D = kCapRate * 60 / 1000;                         // 60 ms echo delay
    struct Tap { int d; double g; };
    const Tap path[] = { {0, 0.50}, {40, 0.25}, {150, -0.15}, {400, 0.08} };

    VoiceGen far(120, 25, 3.3, 9000, 1), near(190, 30, 2.7, 7000, 2);
    std::mt19937 rng(7);

    std::vector<double> master(size_t(ticks) * 480 + 8192, 0.0);    // far end @48k, index n
    std::vector<int16_t> micIn, micOut, nearOnly;
    Pcm out;
    long gen = 0;
    AudioProcessor::Stats stEcho;       // webrtc's own numbers at the end of the echo-only phase

    for(int tick = 0; tick < ticks; ++tick){
        // ---- far end for this 10 ms
        for(int i = 0; i < 480; ++i) master[size_t(gen + i)] = far.next();

        // render: 10 ms at renderRate, delivered in random pieces (device chunks are never exact)
        {
            const int n = renderRate / 100;
            std::vector<int16_t> r(n);
            for(int i = 0; i < n; ++i) r[i] = clip(master[size_t(gen) + size_t(i) * decim]);
            int pos = 0;
            while(pos < n){
                int c = std::min(n - pos, 1 + int(rng() % unsigned(n)));
                if(!noref)
                    aec.render(r.data() + pos, uint32_t(c));
                pos += c;
            }
        }

        // ---- mic for this 10 ms = echo (+ near-end talker in phase 2)
        std::vector<int16_t> mic(480), nearBlk(480);
        const bool doubleTalk = tick >= phase1End * 100;
        for(int i = 0; i < 480; ++i){
            double echo = 0;
            for(const Tap &t : path){
                long idx = gen + i - D - t.d;
                if(idx >= 0) echo += t.g * master[size_t(idx)];
            }
            double nv = doubleTalk ? near.next() : 0.0;
            mic[i] = clip(echo + nv);
            nearBlk[i] = clip(nv);
        }
        gen += 480;
        if(tick == int(phase1End * 100) - 1)
            aec.getStats(stEcho);

        micIn.insert(micIn.end(), mic.begin(), mic.end());
        nearOnly.insert(nearOnly.end(), nearBlk.begin(), nearBlk.end());

        int pos = 0;
        while(pos < 480){
            int c = std::min(480 - pos, 1 + int(rng() % 479));
            out.clear();
            aec.capture(mic.data() + pos, uint32_t(c), out);
            micOut.insert(micOut.end(), out.begin(), out.end());
            pos += c;
        }
    }

    // the output has exactly all complete 10 ms blocks of the input
    if(micOut.size() != micIn.size()){
        std::printf("FAIL: output samples %zu != input samples %zu\n", micOut.size(), micIn.size());
        return 1;
    }

    // ---- phase 1: echo only, last 3 s before double talk (after convergence)
    const size_t a = size_t((phase1End - 3) * kCapRate), b = size_t(phase1End * kCapRate);
    double erle = 10 * std::log10(energy(&micIn[a], b - a) / std::fmax(1.0, energy(&micOut[a], b - a)));
    // first second, to show convergence
    double early = 10 * std::log10(energy(&micIn[0], kCapRate) / std::fmax(1.0, energy(&micOut[0], kCapRate)));

    // ---- phase 2: double talk, last 3 s; near-end level in the output vs. what the talker produced
    const size_t c0 = size_t((totalSec - 3) * kCapRate), c1 = size_t(totalSec * kCapRate);
    double nearKeep = 10 * std::log10(energy(&micOut[c0], c1 - c0) / std::fmax(1.0, energy(&nearOnly[c0], c1 - c0)));
    double echoLeft = 10 * std::log10(energy(&micIn[c0], c1 - c0) / std::fmax(1.0, energy(&nearOnly[c0], c1 - c0)));

    AudioProcessor::Stats st;
    aec.getStats(st);

    std::printf("render rate %d Hz, capture %d Hz\n", renderRate, kCapRate);
    std::printf("echo only      : first 1s attenuation %.1f dB, steady state (5-8s) %.1f dB\n", early, erle);
    std::printf("double talk    : mic/near-end %.1f dB (echo present) -> output/near-end %.1f dB (0 = near end untouched)\n", echoLeft, nearKeep);
    std::printf("webrtc stats   : end of echo-only phase: ERLE %s%.1f dB, ERL %s%.1f dB, delay %s%d ms\n",
                stEcho.hasErle ? "" : "n/a ", stEcho.erleDb, stEcho.hasErl ? "" : "n/a ", stEcho.erlDb, stEcho.hasDelay ? "" : "n/a ", stEcho.delayMs);
    std::printf("                 end of run (double talk): ERLE %s%.1f dB, delay %s%d ms\n",
                st.hasErle ? "" : "n/a ", st.erleDb, st.hasDelay ? "" : "n/a ", st.delayMs);

    if(noref){
        bool ok = erle < 3;
        std::printf(ok ? "CONTROL OK (no reference -> no echo removal)\n" : "CONTROL FAILED: echo removed without reference, the test is not measuring the canceller\n");
        return ok ? 0 : 1;
    }

    bool ok = true;
    if(erle < 20){ std::printf("FAIL: steady-state echo attenuation %.1f dB < 20 dB\n", erle); ok = false; }
    if(nearKeep < -6 || nearKeep > 3){ std::printf("FAIL: near end level changed by %.1f dB (allowed -6..+3)\n", nearKeep); ok = false; }
    std::printf(ok ? "PASS\n" : "FAILED\n");
    return ok ? 0 : 1;
}
