// Receive volume: a tone goes through the real player (jitter buffer + Opus decode), the fake speaker
// (tests/stub/audioDevice_stub.cpp) reports what it gets. Checks 100 = unchanged, 50 = -12 dB, 0 = mute,
// a click-free slider move, and that the received-level meter is NOT affected by the volume.
// No sound card, no webrtc. Build line: tests/README.txt
#include "clsAudioPlayer.h"
#include "opusCodec.h"
#include "VoicePacket.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>

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

int main()
{
    g_stubSpeakerTap = tap;
    AudioConfig cfg; cfg.frameMs = 20; cfg.opusBitrate = 32000; cfg.useDtx = false;
    opusCodec enc; enc.InitEncoder(cfg);
    clsAudioPlayer play;
    play.SetConfig(&cfg);

    // 1 kHz tone at about -12 dBFS, sent as real 20 ms packets in real time
    const int N = cfg.inputRate * cfg.frameMs / 1000;
    std::vector<int16_t> frame(N);
    uint16_t seq = 0; double ph = 0; Bytes wire;
    auto sendMs = [&](int ms){
        for(int k = 0; k < ms / cfg.frameMs; k++){
            for(int i = 0; i < N; i++){ frame[i] = int16_t(8000.0 * std::sin(ph)); ph += 2 * M_PI * 1000 / cfg.inputRate; }
            Bytes opus = enc.Encode(frame.data(), N);
            VoicePacket::wrapInto(wire, seq, seq == 0, 0, opus); seq++;
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

    // a jump of the slider from 0 to 100 must not click: no step bigger than the tone itself makes
    play.SetVolume(0); sendMs(300); rms();
    play.SetVolume(100); sendMs(200);
    std::printf("  biggest sample step during a 0 -> 100 jump: %d\n", g_maxJump.load());
    CHECK(g_maxJump.load() < 8000 * 2 * std::sin(M_PI * 1000 / cfg.inputRate) * 1.5 + 200);   // not bigger than the tone itself can step

    std::printf(g_fail ? "FAILED (%d)\n" : "PASS\n", g_fail);
    return g_fail ? 1 : 0;
}
