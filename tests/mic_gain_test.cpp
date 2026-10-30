// Microphone gain: the real Recorder (fake mic = 440 Hz tone at 8000, tests/stub/audio_device_stub.cpp) with Opus; the
// packets are decoded and measured. 100 = unchanged, 50 = -12 dB, 200 = +12 dB, 0 = mute, and no clicks when the gain jumps.
// No sound card, no webrtc. Build line: tests/README.txt
#include "ravava/Recorder.h"
#include "ravava/OpusCodec.h"
#include "ravava/VoicePacket.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <thread>

using namespace ravava;

static int g_fail = 0;
#define CHECK(c) do{ if(!(c)){ std::printf("  FAIL line %d: %s\n", __LINE__, #c); g_fail++; } }while(0)

static std::mutex g_mx;
static std::atomic<int> g_markers{0};
static double g_sumSq = 0; static long g_n = 0; static int g_maxStep = 0; static int g_last = 0;

static double takeRms(int *maxStep = nullptr)
{
    std::lock_guard<std::mutex> lk(g_mx);
    const double r = g_n ? std::sqrt(g_sumSq / g_n) : 0;
    if(maxStep) *maxStep = g_maxStep;
    g_sumSq = 0; g_n = 0; g_maxStep = 0;
    return r;
}

extern std::atomic<int> g_stubInputStarts;     // how many times the fake microphone was opened
extern std::atomic<int> g_stubInputFailStarts;     // test switch: the next N Start() calls fail
extern std::atomic<bool> g_stubInputMoved;     // test switch: "the system moved the stream to another device"
std::string StubInputDevice();      // from tests/stub/audio_device_stub.cpp (the device id the last Start() was asked for)

int main()
{
    AudioConfig cfg; cfg.frameMs = 20; cfg.opusBitrate = 64000; cfg.useDtx = false;
    OpusCodec dec; dec.InitDecoder(cfg.inputRate, 1);
    Recorder rec;
    rec.SetPacketHandler([&](const Bytes &body){
        uint16_t seq; bool marker; uint8_t tx; Bytes opus; Pcm pcm;
        if(!VoicePacket::Parse(body.data(), body.size(), seq, marker, tx, opus)) return;
        if(marker) g_markers++;
        if(!dec.Decode(opus, pcm)) return;
        std::lock_guard<std::mutex> lk(g_mx);
        for(int16_t v : pcm){ g_sumSq += double(v) * v; g_maxStep = std::max(g_maxStep, std::abs(int(v) - g_last)); g_last = v; }
        g_n += long(pcm.size());
    });
    rec.SetConfig(&cfg);
    CHECK(rec.GetInputGain() == 100 && Recorder::MaxInputGain() == 200);
    cfg.inputDeviceId = "bb";                                           // the config choice must reach the device layer
    rec.Start();
    CHECK(StubInputDevice() == "bb");

    int lastStep = 0;                                                   // biggest sample step while a gain was steady
    auto measure = [&](int gain){
        rec.SetInputGain(gain);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));    // ramp + encoder settles
        takeRms();
        std::this_thread::sleep_for(std::chrono::milliseconds(700));
        return takeRms(&lastStep);
    };

    std::printf("microphone gain\n");
    const double unity = measure(100);

    // switching the microphone while recording: no Stop / Start, the stream goes on, the next packet is marked
    {
        const int before = g_markers;
        takeRms();
        const auto t0 = std::chrono::steady_clock::now();
        CHECK(rec.SetInputDevice("aa"));
        const auto callMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        CHECK(callMs < 50);                                             // the GUI thread is not blocked by the switch
        std::this_thread::sleep_for(std::chrono::milliseconds(700));
        CHECK(StubInputDevice() == "aa");                               // done by the worker thread
        const double after = takeRms();
        std::printf("  live device switch: level %.0f -> %.0f, markers %d -> %d\n", unity, after, before, int(g_markers));
        CHECK(after > unity * 0.7 && after < unity * 1.3);
        CHECK(g_markers == before + 1);

        // the system moves the stream to another device: KeepDevice() re-opens the chosen one
        const int opened0 = g_stubInputStarts;
        g_stubInputMoved = true; rec.KeepDevice();
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        CHECK(g_stubInputStarts == opened0 + 1);
        CHECK(StubInputDevice() == "aa");
        takeRms();

        // a re-open fails (a phone call took the device): the microphone is silent until KeepDevice() opens it again
        rec.SetKeepIntervalMs(100);
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        g_stubInputFailStarts = 1;
        rec.SetInputDevice("bb");
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        takeRms();
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        CHECK(takeRms() == 0);                              // really silent: no packets
        rec.KeepDevice();
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        CHECK(takeRms() > unity * 0.7);                     // recording again
        CHECK(StubInputDevice() == "bb");
    }
    std::printf("  100 -> rms %.0f (tone amplitude 8000 -> %.0f expected)\n", unity, 8000 / std::sqrt(2.0));
    CHECK(unity > 5000 && unity < 6300);

    const double half = measure(50), dbHalf = 20 * std::log10(half / unity);
    const double dbl = measure(200), dbDbl = 20 * std::log10(dbl / unity);
    const int steadyStep = lastStep;                                    // what the +12 dB tone itself (with codec ripple) steps by
    const double mute = measure(0);
    std::printf("  50 -> %.1f dB (expect -12)   200 -> %+.1f dB (expect +12)   0 -> rms %.1f\n", dbHalf, dbDbl, mute);
    CHECK(dbHalf < -11 && dbHalf > -13);
    CHECK(dbDbl > 11 && dbDbl < 13);
    CHECK(mute < 5);
    rec.SetInputGain(400); CHECK(rec.GetInputGain() == 200);            // clamped

    // a jump from 0 to 200 must not click: best of 3 (late packets on a busy machine can glitch a single trial)
    int best = 1 << 30;
    for(int trial = 0; trial < 3; trial++){
        rec.SetInputGain(0); std::this_thread::sleep_for(std::chrono::milliseconds(300)); takeRms();
        rec.SetInputGain(200); std::this_thread::sleep_for(std::chrono::milliseconds(300));
        int step; takeRms(&step); best = std::min(best, step);
    }
    std::printf("  biggest sample step during a 0 -> 200 jump (best of 3): %d (steady +12 dB tone: %d)\n", best, steadyStep);
    CHECK(best < steadyStep * 3 / 2 + 300);                             // a gain click would step by thousands more

    rec.Stop();
    std::printf(g_fail ? "FAILED (%d)\n" : "PASS\n", g_fail);
    return g_fail ? 1 : 0;
}
