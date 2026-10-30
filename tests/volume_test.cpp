// Receive volume: a tone goes through the real player (jitter buffer + Opus decode), the fake speaker
// (tests/stub/audio_device_stub.cpp) reports what it gets. Checks 100 = unchanged, 50 = -12 dB, 0 = mute,
// a click-free slider move, and that the received-level meter is NOT affected by the volume.
// No sound card, no webrtc. Build line: tests/README.txt
#include "ravava/Player.h"
#include "ravava/OpusCodec.h"
#include "ravava/VoicePacket.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>

using namespace ravava;

extern void (*g_stubSpeakerTap)(const int16_t*, uint32_t, int);

static std::atomic<double> g_sumSq{0};
static std::atomic<long>   g_n{0};
static std::atomic<int>    g_maxJump{0};      // biggest sample-to-sample step: a click shows up here
static int g_last = 0;

static void tap(const int16_t *pcm, uint32_t frames, int ch)
{
    double s = g_sumSq.load();
    int jump = g_maxJump.load();
    for(uint32_t i = 0; i < frames * ch; i++){
        s += double(pcm[i]) * pcm[i];
        jump = std::max(jump, std::abs(int(pcm[i]) - g_last));
        g_last = pcm[i];
    }
    g_sumSq = s; g_maxJump = jump; g_n += frames * ch;
}

static double rms()                      // of what arrived since the last call
{
    const double r = g_n.load() ? std::sqrt(g_sumSq.load() / g_n.load()) : 0;
    g_sumSq = 0; g_n = 0; g_maxJump = 0;
    return r;
}

static int g_fail = 0;
#define CHECK(c) do{ if(!(c)){ std::printf("  FAIL line %d: %s\n", __LINE__, #c); g_fail++; } }while(0)

extern std::atomic<int> g_stubOutputStarts;    // how many times the fake speaker was opened
extern std::atomic<bool> g_stubOutputMoved;
extern std::atomic<int> g_stubOutputFailStarts;   // test switch: the next N Start() calls fail    // test switch: "the system moved the stream to another device"
std::string StubOutputDevice();     // from tests/stub/audio_device_stub.cpp (the device id the last Start() was asked for)

int main()
{
    g_stubSpeakerTap = tap;
    AudioConfig cfg; cfg.frameMs = 20; cfg.opusBitrate = 32000; cfg.useDtx = false;
    OpusCodec enc; enc.InitEncoder(cfg);
    Player play;
    cfg.outputDeviceId = "dd";                              // the config choice must reach the device layer
    play.SetConfig(&cfg);
    CHECK(StubOutputDevice() == "dd");

    // 1 kHz tone at about -12 dBFS, sent as real 20 ms packets in real time
    const int N = cfg.inputRate * cfg.frameMs / 1000;
    std::vector<int16_t> frame(N);
    uint16_t seq = 0; double ph = 0; Bytes wire;
    auto sendMs = [&](int ms){
        for(int k = 0; k < ms / cfg.frameMs; k++){
            for(int i = 0; i < N; i++){ frame[i] = int16_t(8000.0 * std::sin(ph)); ph += 2 * M_PI * 1000 / cfg.inputRate; }
            Bytes opus = enc.Encode(frame.data(), N);
            VoicePacket::WrapInto(wire, seq, seq == 0, 0, opus); seq++;
            play.Play(wire);
            std::this_thread::sleep_for(std::chrono::milliseconds(cfg.frameMs));
        }
    };

    std::printf("receive volume\n");
    sendMs(400); rms();                                 // fill the jitter buffer, discard
    play.SetVolume(100); sendMs(300); rms();
    sendMs(600); const double full = rms();
    const int lvFull = play.TakePeakLevel();
    CHECK(full > 3000);

    // switching the speaker while playing: no Stop / Start, the received stream and its counters go on
    {
        JitterBuffer::Stats st0, st1; int d0, d1, f0, f1;
        CHECK(play.GetStats(st0, d0, f0));
        const auto t0 = std::chrono::steady_clock::now();
        CHECK(play.SetOutputDevice("cc"));
        const auto callMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        CHECK(callMs < 50);                                 // the GUI thread is not blocked by the switch
        sendMs(300); rms();
        CHECK(StubOutputDevice() == "cc");                  // done by the worker thread
        sendMs(600); const double after = rms();
        CHECK(play.GetStats(st1, d1, f1));
        std::printf("  live device switch: rms %.0f -> %.0f, played frames %u -> %u\n", full, after, unsigned(st0.played), unsigned(st1.played));
        CHECK(after > full * 0.7 && after < full * 1.2);       // a short gap at the switch is allowed
        CHECK(st1.played > st0.played);                  // counters were not reset

        // the system moves the stream to another device (notification, Bluetooth): KeepDevice() re-opens the chosen one,
        // but not more often than once per 3 s
        const int opened0 = g_stubOutputStarts;
        play.KeepDevice();                                  // nothing moved: nothing happens
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        CHECK(g_stubOutputStarts == opened0);
        g_stubOutputMoved = true; play.KeepDevice();
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        CHECK(g_stubOutputStarts == opened0 + 1);
        CHECK(StubOutputDevice() == "cc");                  // re-opened on the CHOSEN device
        g_stubOutputMoved = true; play.KeepDevice();        // again at once: rate limited
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        CHECK(g_stubOutputStarts == opened0 + 1);

        // a re-open fails (a phone call took the device): KeepDevice() tries again, after two failures it uses the default
        play.SetKeepIntervalMs(100);
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        g_stubOutputFailStarts = 2;
        const int o1 = g_stubOutputStarts;
        play.SetOutputDevice("dd");                         // the open fails (1st failure)
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        play.KeepDevice();                                  // not running -> tries "dd" again, fails (2nd failure)
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        CHECK(StubOutputDevice() == "dd");
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        play.KeepDevice();                                  // two failures: falls back to the system default ("")
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        CHECK(StubOutputDevice() == "");
        CHECK(g_stubOutputStarts == o1 + 3);
        play.KeepDevice();                                  // running again: nothing more happens
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        CHECK(g_stubOutputStarts == o1 + 3);
        sendMs(300); rms(); sendMs(300); CHECK(rms() > 1000);      // and it plays again
    }

    play.SetVolume(50); sendMs(300); rms();             // let the ramp finish
    sendMs(600); const double half = rms();
    const int lvHalf = play.TakePeakLevel();
    const double dbHalf = 20 * std::log10(half / full);
    std::printf("  volume 50: %.1f dB (expected about -12), level meter %d -> %d\n", dbHalf, lvFull, lvHalf);
    CHECK(dbHalf < -11 && dbHalf > -13);
    CHECK(std::abs(lvFull - lvHalf) <= 2);              // the meter shows what was received, not what is played

    play.SetVolume(0); sendMs(300); rms();
    sendMs(400); CHECK(rms() < 1.0);                    // muted
    CHECK(play.GetVolume() == 0);

    play.SetVolume(150); CHECK(play.GetVolume() == 100);    // clamped
    play.SetVolume(-5);  CHECK(play.GetVolume() == 0);

    // a jump of the slider from 0 to 100 must not click: no step bigger than the tone itself makes.
    // A click from the gain would show in EVERY trial; a random glitch from late packets (real-time test on a busy
    // machine) only in some, so the best of 3 trials decides.
    int best = 1 << 30;
    for(int trial = 0; trial < 3; trial++){
        play.SetVolume(0); sendMs(300); rms();
        play.SetVolume(100); sendMs(200);
        best = std::min(best, g_maxJump.load());
    }
    std::printf("  biggest sample step during a 0 -> 100 jump (best of 3): %d\n", best);
    CHECK(best < 8000 * 2 * std::sin(M_PI * 1000 / cfg.inputRate) * 1.5 + 200);   // not bigger than the tone itself can step

    std::printf(g_fail ? "FAILED (%d)\n" : "PASS\n", g_fail);
    return g_fail ? 1 : 0;
}
