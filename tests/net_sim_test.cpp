// Network simulation with a VIRTUAL clock (no sleeping, no sound card, real libopus packets): the same JitterBuffer the
// Player uses is fed by a simulated network (random loss, burst loss, jitter, link stalls, latency step, clock drift)
// and pulled once per frame by a simulated speaker. Every scenario prints what the listener would experience:
//   delay   : extra latency the jitter buffer adds (avg / p95 / max), in ms
//   bad%    : share of frames that were NOT real or FEC-recovered audio (PLC + silence + skipped)
//   events  : underruns, PLC frames, FEC frames, silent pops after start, skipped (dropped) frames, late packets
// Build:
//   g++ -std=c++17 -O2 -pthread -Ilibs/opus/win32/include -Isrc tests/net_sim_test.cpp src/ravava/JitterBuffer.cpp src/ravava/OpusCodec.cpp -lopus -o net_sim_test
// Run:  ./net_sim_test [legacy|timed] [speech|dtx|wav <file|folder>] [--check] [name-filter]   |   ./net_sim_test compare [speech|dtx] [name-filter]
//   wav = real speech from .wav files (any rate / channels; the folder voice-sample/ of the repo), combine with dtx
//   dtx = speech-like signal AND the sender skips the silent frames (Opus DTX, as Recorder does); "silent" is then not counted as bad
//   compare = legacy and adaptive side by side, fails if adaptive is much worse than legacy anywhere (guard rail, see main())   (speech = speech-like signal with pauses instead of a steady tone)
//   legacy = Push(datagram, size) (no arrival time: the old reactive buffer),
//   timed  = SetAdaptive(true) + Push(datagram, size, arrivalUs) (what Player does).
//   --check turns the acceptance thresholds into the exit code.
#define _USE_MATH_DEFINES      // M_PI on MSVC
#include "ravava/JitterBuffer.h"
#include "ravava/VoicePacket.h"
#include "ravava/AudioConfig.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <dirent.h>
#include <fstream>
#include <string>
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
    uint32_t plcInSilence = 0;          // DTX only: concealment of a frame the sender never sent (a pause), nobody is harmed
    int finalTarget = 0;
};

struct Arrival { int64_t atUs; int idx; bool operator<(const Arrival &o) const { return atUs < o.atUs || (atUs == o.atUs && idx < o.idx); } };

static bool g_speech = false;
static bool g_dtx = false;          // "dtx" argument: the sender skips silent frames like Recorder does (marker on the next packet, tx counts only sent ones)       // "speech" argument: talk bursts with syllable-like modulation, pauses with faint background noise

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


// ---- real speech: .wav files (8/16/24/32-bit PCM or 32-bit float, any rate, mono or more channels) -> 48 kHz mono 20 ms frames
static std::vector<Pcm> g_wavFrames;        // "wav <file|folder>": the sender plays these (all files one after another, 0.5 s apart), looping
static std::string g_wavNames;

static bool readWav(const std::string &path, std::vector<float> &mono48)
{
    std::ifstream f(path, std::ios::binary);
    if(!f) return false;
    std::vector<uint8_t> b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    auto u16 = [&](size_t o){ return unsigned(b[o]) | unsigned(b[o + 1]) << 8; };
    auto u32 = [&](size_t o){ return u16(o) | u16(o + 2) << 16; };
    if(b.size() < 44 || std::memcmp(b.data(), "RIFF", 4) || std::memcmp(b.data() + 8, "WAVE", 4)) return false;
    unsigned fmt = 0, ch = 0, rate = 0, bits = 0; size_t dataOff = 0, dataLen = 0;
    for(size_t o = 12; o + 8 <= b.size();){
        const size_t len = u32(o + 4);
        if(!std::memcmp(b.data() + o, "fmt ", 4) && o + 8 + 16 <= b.size()){
            fmt = u16(o + 8); ch = u16(o + 10); rate = u32(o + 12); bits = u16(o + 22);
            if(fmt == 0xFFFE && len >= 26 && o + 8 + 26 <= b.size()) fmt = u16(o + 8 + 24);      // extensible: the sub-format
        }else if(!std::memcmp(b.data() + o, "data", 4)){
            dataOff = o + 8; dataLen = std::min(len, b.size() - dataOff); break;                 // (len may be wrong for streamed files)
        }
        o += 8 + len + (len & 1);
    }
    if(!ch || !rate || !dataLen || !((fmt == 1 && (bits == 8 || bits == 16 || bits == 24 || bits == 32)) || (fmt == 3 && bits == 32))) return false;
    const size_t bps = bits / 8, frames = dataLen / (bps * ch);
    std::vector<float> mono(frames);
    for(size_t i = 0; i < frames; i++){
        double s = 0;
        for(unsigned c = 0; c < ch; c++){
            const uint8_t *q = b.data() + dataOff + (i * ch + c) * bps;
            double v;
            if(fmt == 3){ float x; std::memcpy(&x, q, 4); v = x; }
            else if(bits == 8) v = (int(q[0]) - 128) / 128.0;
            else if(bits == 16) v = int16_t(q[0] | q[1] << 8) / 32768.0;
            else if(bits == 24) v = (int32_t(q[0] << 8 | q[1] << 16 | q[2] << 24) >> 8) / 8388608.0;
            else v = int32_t(uint32_t(q[0]) | uint32_t(q[1]) << 8 | uint32_t(q[2]) << 16 | uint32_t(q[3]) << 24) / 2147483648.0;
            s += v;
        }
        mono[i] = float(s / ch);
    }
    // to 48 kHz by linear interpolation (speech sits far below the Nyquist of the source rates that matter: 8..44.1 kHz; when going
    // DOWN from 96/192 kHz a short box average first keeps the worst aliasing out)
    const double step = double(rate) / 48000.0;
    const size_t outN = size_t(double(frames) / step);
    const int box = step > 1.5 ? int(step) : 1;
    for(size_t i = 0; i < outN; i++){
        const double pos = double(i) * step; const size_t i0 = size_t(pos); const double fr = pos - double(i0);
        double v;
        if(box > 1){ double a = 0; int n = 0; for(int k = 0; k < box && i0 + size_t(k) < frames; k++, n++) a += mono[i0 + size_t(k)]; v = n ? a / n : 0; }
        else v = i0 + 1 < frames ? mono[i0] * (1 - fr) + mono[i0 + 1] * fr : mono[i0];
        mono48.push_back(float(v));
    }
    return true;
}

// a file, or every *.wav in a folder (sorted)
static bool loadWavs(const std::string &path, int fs)
{
    std::vector<std::string> files;
    if(DIR *d = opendir(path.c_str())){
        while(dirent *e = readdir(d)){
            const std::string n = e->d_name;
            if(n.size() > 4 && (n.compare(n.size() - 4, 4, ".wav") == 0 || n.compare(n.size() - 4, 4, ".WAV") == 0)) files.push_back(path + "/" + n);
        }
        closedir(d);
        std::sort(files.begin(), files.end());
    }else files.push_back(path);
    std::vector<float> all;
    for(const std::string &fn : files){
        std::vector<float> m;
        if(!readWav(fn, m)){ std::fprintf(stderr, "cannot read %s (not a PCM/float WAV?)\n", fn.c_str()); continue; }
        // peak-normalise to ~-6 dBFS: recordings differ a lot in level, the DTX / quiet-frame logic should see comparable material
        float pk = 0; for(float x : m) pk = std::max(pk, std::fabs(x));
        const float g = pk > 1e-4f ? 0.5f / pk : 1.0f;
        for(float x : m) all.push_back(x * g);
        all.insert(all.end(), 24000, 0.0f);                              // 0.5 s of silence between the files
        g_wavNames += (g_wavNames.empty() ? "" : ", ") + fn.substr(fn.find_last_of('/') + 1) + " (" + std::to_string(int(m.size() / 48000)) + " s)";
    }
    for(size_t o = 0; o + size_t(fs) <= all.size(); o += size_t(fs)){
        Pcm p(static_cast<size_t>(fs), int16_t(0));
        for(int k = 0; k < fs; k++) p[size_t(k)] = int16_t(std::max(-1.0f, std::min(1.0f, all[o + size_t(k)])) * 32000.0f);
        g_wavFrames.push_back(std::move(p));
    }
    return !g_wavFrames.empty();
}

static Result run(const Scenario &sc, bool timed, unsigned seed)
{
    std::mt19937 rng(seed);
    std::normal_distribution<double> gauss(0.0, 1.0);
    std::uniform_real_distribution<double> uni(0.0, 1.0);

    AudioConfig cfg; cfg.frameMs = 20; cfg.opusBitrate = 16000; cfg.useDtx = g_dtx; cfg.useFec = true; cfg.expectedLossPerc = 10;
    OpusCodec enc; enc.InitEncoder(cfg);
    const int fs = cfg.FrameSamples();
    std::vector<Bytes> pool;
    if(!g_wavFrames.empty()){
        for(const Pcm &pcm : g_wavFrames){ Bytes b; enc.Encode(pcm.data(), fs, b); pool.push_back(b); }
    }else if(g_speech){
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
    uint32_t txc = 0; bool dtxMarker = false;
    double nextStall = sc.stallEverySec > 0 ? -std::log(1.0 - uni(rng)) * sc.stallEverySec : 1e18;
    double stallEnd = -1;
    for(int i = 0; i < total; i++){
        const double sendS = i * frameUs / 1e6;
        const Bytes &pk = pool[size_t(i) % pool.size()];
        if(g_dtx && pk.size() <= 2){ dtxMarker = true; continue; }        // silence: nothing is sent (see Recorder.cpp)
        wire[size_t(i)] = VoicePacket::Wrap(uint16_t(seq0 + i), i == 0 || dtxMarker, uint8_t(txc++ & 127), pk);
        dtxMarker = false;
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
        const uint32_t plcBefore = jb.GetStats().plc;
        const int64_t posBefore = jb.PlayoutPosition();
        const bool got = jb.PopFrame(pcm);
        if(g_dtx && jb.GetStats().plc != plcBefore){
            const int64_t idx = posBefore - seq0;                // first packet's extended sequence = seq0
            if(idx >= 0 && idx < total && wire[size_t(idx)].empty())
                r.plcInSilence += jb.GetStats().plc - plcBefore;
        }
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
    const double bad = (g_dtx ? 0.0 : double(r.silent)) +   // with DTX a silent pop is normal (the sender is quiet)
                        double(r.st.plc) - double(r.plcInSilence) +        // ... and so is concealing a frame the sender skipped (a pause)
                        double(r.st.skipped) + double(r.st.stretched);
    r.badPct = 100.0 * bad / double(pulls);
    return r;
}

struct Agg {
    double avg = 0, p95 = 0, max = 0, bad = 0, target = 0;
    uint32_t underrun = 0, plc = 0, fec = 0, silent = 0, skipped = 0, stretched = 0, forced = 0, late = 0, rebuffer = 0;
};

// average over a few seeds so one lucky run doesn't decide
static Agg aggregate(const Scenario &s, bool timed)
{
    const int seeds = 5;
    Agg g;
    for(int sd = 1; sd <= seeds; sd++){
        const Result r = run(s, timed, unsigned(sd));
        g.avg += r.avgMs; g.p95 += r.p95Ms; g.max = std::max(g.max, r.maxMs); g.bad += r.badPct; g.target += r.finalTarget;
        g.underrun += r.st.underrun; g.plc += r.st.plc; g.fec += r.st.fec; g.silent += r.silent; g.skipped += r.st.skipped;
        g.stretched += r.st.stretched; g.forced += r.st.slipForced; g.late += r.st.late; g.rebuffer += r.st.rebuffer;
    }
    g.avg /= seeds; g.p95 /= seeds; g.bad /= seeds; g.target /= seeds;
    g.underrun /= seeds; g.plc /= seeds; g.fec /= seeds; g.silent /= seeds; g.skipped /= seeds; g.stretched /= seeds;
    g.forced /= seeds; g.late /= seeds; g.rebuffer /= seeds;
    return g;
}

int main(int argc, char **argv)
{
    bool timed = false, check = false, compare = false;
    const char *only = nullptr;             // any other argument: run only the scenarios whose name contains it
    for(int i = 1; i < argc; i++){
        if(!std::strcmp(argv[i], "timed")) timed = true;
        else if(!std::strcmp(argv[i], "legacy")) timed = false;
        else if(!std::strcmp(argv[i], "--check")) check = true;
        else if(!std::strcmp(argv[i], "speech")) g_speech = true;
        else if(!std::strcmp(argv[i], "dtx")) { g_dtx = true; g_speech = true; }     // DTX only makes sense with pauses
        else if(!std::strcmp(argv[i], "compare")) compare = true;
        else if(!std::strcmp(argv[i], "wav") && i + 1 < argc){
            if(!loadWavs(argv[++i], 960)){ std::fprintf(stderr, "no usable .wav in %s\n", argv[i]); return 2; }
            g_speech = true;
        }
        else only = argv[i];
    }

    //                name              loss burst%  bmin bmax base jit  stallEvery stallMs stepAt stepMs stepFor drift  sec   maxBad maxP95
    const Scenario sc[] = {
        {"clean",                     0,   0,    0,   0,   40,  0,    0,   0,     0,   0,   0,     0,    120, 0.2,  140},
        {"loss 5%",                   5,   0,    0,   0,   40,  3,    0,   0,     0,   0,   0,     0,    120, 6.0,  160},
        {"loss 20%",                 20,   0,    0,   0,   40,  3,    0,   0,     0,   0,   0,     0,    120, 25,   160},
        {"burst loss 3-10",           1,   1.0,  3,  10,   40,  3,    0,   0,     0,   0,   0,     0,    120, 12,   160},
        // fixed burst lengths (one burst per ~7 s on average): what each length costs in PLC / re-buffering / silence
        {"burst 3",                   0,   0.3,  3,   3,   40,  3,    0,   0,     0,   0,   0,     0,    120, 6,    200},
        {"burst 5",                   0,   0.3,  5,   5,   40,  3,    0,   0,     0,   0,   0,     0,    120, 8,    200},
        {"burst 10",                  0,   0.3, 10,  10,   40,  3,    0,   0,     0,   0,   0,     0,    120, 12,   200},
        {"burst 20",                  0,   0.3, 20,  20,   40,  3,    0,   0,     0,   0,   0,     0,    120, 20,   200},
        {"burst 30",                  0,   0.3, 30,  30,   40,  3,    0,   0,     0,   0,   0,     0,    120, 28,   200},
        {"jitter 10ms",               0,   0,    0,   0,   40,  10,   0,   0,     0,   0,   0,     0,    120, 1.0,  160},
        {"jitter 30ms",               0,   0,    0,   0,   40,  30,   0,   0,     0,   0,   0,     0,    120, 3.0,  220},
        {"jitter 60ms",               0,   0,    0,   0,   40,  60,   0,   0,     0,   0,   0,     0,    120, 6.0,  300},
        {"3G: jitter 25 + stalls",    2,   0,    0,   0,   80,  25,   12,  300,   0,   0,   0,     0,    240, 8.0,  300},
        {"latency step +150ms 20s",   0,   0,    0,   0,   40,  5,    0,   0,     60,  150, 20,    0,    180, 4.0,  220},
        {"drift speaker +300ppm",     0,   0,    0,   0,   40,  3,    0,   0,     0,   0,   0,     300,  600, 1.0,  200},
        {"drift speaker -300ppm",     0,   0,    0,   0,   40,  3,    0,   0,     0,   0,   0,    -300,  600, 1.0,  260},
    };

    if(compare){
        // Guard rail: the adaptive playout may not be much WORSE than the plain buffer in any scenario. Limits (generous, they catch
        // a broken change, not tuning noise): bad% <= legacy * 1.25 + 0.5 (1.0 with dtx: a pause makes the figure noisier), underruns <= legacy * 2 + 30 (cheap: a loss the buffer could not hide is concealed either way, bad% is the
        // audible figure), average delay <= legacy * 1.8 + 20 ms.
        std::printf("signal: %s%s | adaptive vs legacy (legacy -> adaptive)\n", !g_wavFrames.empty() ? "real speech" : g_speech ? "speech-like" : "steady tone", g_dtx ? " + DTX" : "");
        std::printf("%-26s | delay ms        | bad%%          | underrun   | rebuffer  | late\n", "scenario");
        int fails = 0;
        for(const Scenario &s : sc){
            if(only && !std::strstr(s.name, only))
                continue;
            const Agg l = aggregate(s, false), a = aggregate(s, true);
            const bool ok = a.bad <= l.bad * 1.25 + (g_dtx ? 1.0 : 0.5) && a.underrun <= l.underrun * 2.0 + 30 && a.avg <= l.avg * 1.8 + 20;
            std::printf("%-26s | %5.0f -> %5.0f  | %5.2f -> %5.2f | %3u -> %3u | %3u -> %3u | %3u -> %3u  %s\n", s.name, l.avg, a.avg, l.bad, a.bad,
                        l.underrun, a.underrun, l.rebuffer, a.rebuffer, l.late, a.late, ok ? "ok" : "FAIL");
            if(!ok) fails++;
        }
        std::printf(fails ? "FAIL: %d scenarios where adaptive is much worse than legacy\n" : "PASS\n", fails);
        return fails ? 1 : 0;
    }

    std::printf("mode: %s, signal: %s%s\n", timed ? "timed (arrival time given)" : "legacy (no arrival time)",
                !g_wavFrames.empty() ? "real speech" : g_speech ? "speech-like" : "steady tone", g_dtx ? " + DTX" : "");
    std::printf("%-26s | delay ms avg/p95/max | bad%%  | underrun  plc  fec silent rebuf skipped stretch forced late | target\n", "scenario");
    int fails = 0;
    for(const Scenario &s : sc){
        if(only && !std::strstr(s.name, only))
            continue;
        const Agg g = aggregate(s, timed);
        const bool ok = g.bad <= s.maxBadPct && g.p95 <= s.maxP95Ms;
        std::printf("%-26s | %6.0f %6.0f %6.0f   | %5.2f | %6u %5u %5u %5u %5u %6u %7u %6u %4u | %4.1f  %s\n", s.name, g.avg, g.p95, g.max, g.bad,
                    g.underrun, g.plc, g.fec, g.silent, g.rebuffer, g.skipped, g.stretched, g.forced, g.late, g.target,
                    check ? (ok ? "ok" : "FAIL") : "");
        if(!ok) fails++;
    }
    if(check){
        std::printf(fails ? "FAIL: %d scenarios outside the limits\n" : "PASS\n", fails);
        return fails ? 1 : 0;
    }
    return 0;
}
