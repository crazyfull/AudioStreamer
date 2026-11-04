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
    AudioConfig cfg; cfg.frameMs = 20; cfg.Bitrate = 64000; cfg.useDtx = false;
    OpusCodec dec; dec.InitDecoder(cfg.inputRate, 1);
    Recorder rec;
    rec.SetAutoKeepDevice(false);                                       // the KeepDevice() checks below run on their own timing
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
    rec.Start();                                                        // the microphone is opened by the device worker
    for(int i = 0; i < 50 && StubInputDevice() != "bb"; i++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
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

    // level of the SENT audio (myVolume bar): a tone reads loud, half gain reads lower, mute reads 0
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        const int loud = rec.GetLevel();
        std::printf("  sent level: gain 100 -> %d\n", loud);
        CHECK(loud > 30 && loud <= 100);
        rec.SetInputGain(25);
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        const int quiet = rec.GetLevel();
        std::printf("  sent level: gain 25  -> %d\n", quiet);
        CHECK(quiet < loud);
        rec.SetInputGain(0);
        std::this_thread::sleep_for(std::chrono::milliseconds(2000));
        CHECK(rec.GetLevel() == 0);
        rec.SetInputGain(100);
    }

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

    // PlayMusic: the music file replaces the microphone (the stub reads "tone" as a 440 Hz sine, anything else cannot be opened)
    {
        std::printf("music file\n");
        AudioConfig mc = cfg; mc.playMusic = true; mc.musicPath = "tone";
        Recorder mrec;
        std::atomic<int> packets{0};
        mrec.SetPacketHandler([&](const Bytes &){ packets++; });
        mrec.SetConfig(&mc);
        mrec.Start();
        std::this_thread::sleep_for(std::chrono::milliseconds(800));
        std::printf("  tone file: %d packets, level %d, error '%s'\n", packets.load(), mrec.GetLevel(), mrec.MusicError().c_str());
        CHECK(packets > 20);
        CHECK(mrec.GetLevel() > 30);
        CHECK(mrec.MusicError().empty());
        mrec.Stop();

        AudioConfig bad = cfg; bad.playMusic = true; bad.musicPath = "/no/such/file.mp3";
        Recorder brec;
        brec.SetPacketHandler([&](const Bytes &){});
        brec.SetConfig(&bad);
        brec.Start();
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        std::printf("  missing file: error '%s'\n", brec.MusicError().c_str());
        CHECK(!brec.MusicError().empty());                              // the user is told, not left with silence
        CHECK(brec.GetLevel() == 0);
        brec.Stop();
    }

    // music mixed in at run time: Play / Pause / Resume / Stop are separate, and the microphone can talk over the music
    {
        std::printf("music + talk\n");
        AudioConfig xc = cfg;                                           // playMusic stays off: the file is MIXED with the microphone
        Recorder mx;
        std::atomic<int> pk{0};
        std::mutex m2; double sq = 0; long cnt = 0;
        OpusCodec d2; d2.InitDecoder(xc.inputRate, 1);
        mx.SetPacketHandler([&](const Bytes &body){
            uint16_t seq; bool marker; uint8_t tx; Bytes opus; Pcm pcm;
            if(!VoicePacket::Parse(body.data(), body.size(), seq, marker, tx, opus) || !d2.Decode(opus, pcm)) return;
            pk++;
            std::lock_guard<std::mutex> lk(m2);
            for(int16_t v : pcm){ sq += double(v) * v; } cnt += long(pcm.size());
        });
        mx.SetConfig(&xc);
        CHECK(!mx.MusicPlay("tone880"));                                // not started yet
        mx.Start();
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        auto settle = [&](int ms){ std::this_thread::sleep_for(std::chrono::milliseconds(ms)); { std::lock_guard<std::mutex> lk(m2); sq = 0; cnt = 0; } pk = 0;
                                   std::this_thread::sleep_for(std::chrono::milliseconds(500)); };
        auto rms = [&]{ std::lock_guard<std::mutex> lk(m2); return cnt ? std::sqrt(sq / double(cnt)) : 0.0; };

        settle(300); const double micOnly = rms();                      // talk on, no music
        std::printf("  mic only: rms %.0f, %d packets\n", micOnly, pk.load());
        CHECK(micOnly > 5000 && pk > 15);
        CHECK(mx.GetMusicState() == Recorder::MusicState::Stopped);

        mx.SetMicEnabled(false);                                        // talk off, no music: nothing is sent at all
        settle(300);
        std::printf("  talk off, no music: %d packets\n", pk.load());
        CHECK(pk == 0);

        CHECK(mx.MusicPlay("tone880"));                                 // music alone (talk still off)
        CHECK(mx.GetMusicState() == Recorder::MusicState::Playing);
        settle(300); const double musicOnly = rms();
        std::printf("  music only: rms %.0f, %d packets\n", musicOnly, pk.load());
        CHECK(musicOnly > 4500 && musicOnly < 6800 && pk > 15);

        mx.SetMicEnabled(true);                                         // talk + music: both are in the stream
        settle(300); const double both = rms();
        std::printf("  talk + music: rms %.0f (expected ~%.0f = both tones)\n", both, std::sqrt(micOnly * micOnly + musicOnly * musicOnly));
        CHECK(both > micOnly * 1.25 && both > musicOnly * 1.25);

        mx.SetMicEnabled(false);
        CHECK(mx.MusicPause());                                         // Pause: nothing is sent (talk off), position is kept
        CHECK(mx.GetMusicState() == Recorder::MusicState::Paused);
        CHECK(!mx.MusicPause());                                        // already paused
        settle(300);
        std::printf("  paused: %d packets\n", pk.load());
        CHECK(pk == 0);
        CHECK(mx.MusicPlay("tone880") && mx.MusicPause());              // keep the state paused for the Resume check
        CHECK(mx.MusicResume());                                        // Resume: music again
        CHECK(!mx.MusicResume());
        CHECK(mx.GetMusicState() == Recorder::MusicState::Playing);
        settle(300);
        CHECK(pk > 15 && rms() > 4500);

        mx.MusicStop();                                                 // Stop: state stopped, nothing sent, Resume has nothing to resume
        CHECK(mx.GetMusicState() == Recorder::MusicState::Stopped);
        CHECK(!mx.MusicResume() && !mx.MusicPause());
        settle(300);
        CHECK(pk == 0);

        CHECK(mx.MusicPlay("short"));                                   // a file that ends: Finished, and the stream goes quiet again
        std::this_thread::sleep_for(std::chrono::milliseconds(700));
        CHECK(mx.GetMusicState() == Recorder::MusicState::Finished);
        settle(200);
        CHECK(pk == 0);

        CHECK(mx.MusicPlay("/no/such/file.mp3"));                       // accepted (opened in the background) ...
        CHECK(!mx.MusicError().empty());                                // ... and the failure is reported, not silently ignored
        mx.MusicStop();
        mx.SetMusicVolume(300); CHECK(mx.GetMusicVolume() == 200);
        mx.Stop();
        CHECK(!mx.MusicPlay("tone"));                                   // stopped recorder: refused
    }

    // The recorder repairs its microphone by itself, and works on its own copy of the config (the caller's may be destroyed after Start())
    {
        std::printf("automatic KeepDevice + config copy\n");
        AudioConfig *hc = new AudioConfig(cfg);
        Recorder r4;
        std::atomic<int> got{0};
        r4.SetPacketHandler([&](const Bytes &){ got++; });
        r4.SetConfig(hc);
        r4.SetKeepIntervalMs(100);
        r4.Start();
        delete hc;                                          // dangling from here on: must not matter any more
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        CHECK(got > 10);
        r4.SetAudioProcessor(nullptr);                      // after Start(): refused with a warning
        const int o0 = g_stubInputStarts;
        g_stubInputMoved = true;                            // the system moved the stream, nobody calls KeepDevice()
        std::this_thread::sleep_for(std::chrono::milliseconds(2600));
        std::printf("  re-opens after the route moved: %d, packets %d\n", int(g_stubInputStarts) - o0, got.load());
        CHECK(g_stubInputStarts == o0 + 1);
        const int before = got; std::this_thread::sleep_for(std::chrono::milliseconds(300));
        CHECK(got > before);                                // recording again
        r4.Stop();
        const int o1 = g_stubInputStarts;
        g_stubInputMoved = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2200));
        CHECK(g_stubInputStarts == o1);                     // stopped: the watchdog does not bring the microphone back
    }

    std::printf(g_fail ? "FAILED (%d)\n" : "PASS\n", g_fail);
    return g_fail ? 1 : 0;
}
