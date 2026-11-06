// The low-cut (webrtc high-pass filter) of AudioProcessor: a 40 Hz rumble is cut, speech frequencies stay, and with the filter
// off nothing is cut. Needs the webrtc build; without it the test says so and passes (AudioProcessor is a pass-through then).
// Build: see tests/README.txt. Run: ./hpf_test
#include "ravava/AudioProcessor.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace ravava;

static const double kPi = 3.14159265358979323846;
static int g_fail = 0;
#define CHECK(c) do{ if(!(c)){ std::printf("  FAIL line %d: %s\n", __LINE__, #c); g_fail++; } }while(0)

// amplitude of a sine of `hz` inside x (correlation with sin / cos, so the phase does not matter)
static double amplitudeAt(const std::vector<int16_t> &x, size_t from, double hz, int rate)
{
    double s = 0, c = 0; size_t n = 0;
    for(size_t i = from; i < x.size(); ++i, ++n){
        const double ph = 2 * kPi * hz * double(i) / rate;
        s += x[i] * std::sin(ph); c += x[i] * std::cos(ph);
    }
    return 2.0 * std::sqrt(s * s + c * c) / double(n);
}

// rumble 40 Hz (8000) + voice band 1 kHz (4000), 6 s, through the processor in odd-sized pieces
static void run(bool hpf, double &rumble, double &voice)
{
    const int rate = 48000;
    AudioProcessor::Options opt;
    opt.echoCancel = false; opt.noiseSuppression = 0; opt.autoGain = false; opt.highPassFilter = hpf;
    AudioProcessor ap;
    std::string err;
    if(!ap.Init(rate, rate, 1, opt, &err)){ std::printf("  init failed: %s\n", err.c_str()); g_fail++; rumble = voice = 0; return; }
    const size_t total = size_t(rate) * 6;
    Pcm in(total);
    for(size_t i = 0; i < total; ++i)
        in[i] = int16_t(8000 * std::sin(2 * kPi * 40.0 * double(i) / rate) + 4000 * std::sin(2 * kPi * 1000.0 * double(i) / rate));
    Pcm out; out.reserve(total + 1000);
    for(size_t pos = 0; pos < total; ){
        const size_t n = std::min<size_t>(total - pos, 357);       // not a multiple of 10 ms: the re-cutting is part of the test
        ap.Capture(in.data() + pos, uint32_t(n), out);
        pos += n;
    }
    const size_t skip = size_t(rate) * 2;                           // let the filter settle
    rumble = amplitudeAt(out, skip, 40.0, rate);
    voice  = amplitudeAt(out, skip, 1000.0, rate);
}

int main()
{
    if(!AudioProcessor::IsAvailable()){
        std::printf("hpf_test: this build has no webrtc-audio-processing, skipped\n");
        return 0;
    }
    double r0, v0, r1, v1;
    run(false, r0, v0);
    run(true, r1, v1);
    const double cutDb = 20 * std::log10(r1 / r0);
    const double voiceDb = 20 * std::log10(v1 / v0);
    std::printf("low-cut off: 40 Hz %.0f, 1 kHz %.0f\n", r0, v0);
    std::printf("low-cut on : 40 Hz %.0f (%.1f dB), 1 kHz %.0f (%.1f dB)\n", r1, cutDb, v1, voiceDb);
    CHECK(r0 > 7000 && v0 > 3500);                  // off: both pass untouched (8000 / 4000)
    CHECK(cutDb < -10.0);                           // on: the rumble is at least 10 dB down
    CHECK(std::fabs(voiceDb) < 1.5);                // on: the voice band is not changed
    std::printf(g_fail ? "FAILED (%d)\n" : "PASS\n", g_fail);
    return g_fail ? 1 : 0;
}
