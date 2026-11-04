// ConferencePlayer: several talkers, ONE speaker. Real ConferencePlayer + Opus, fake speaker (tests/stub/audio_device_stub.cpp), no sound card.
// Three synthetic talkers (500 / 900 / 1500 Hz) send independent streams (own sequence numbers, 5% loss, some reordering, real time).
//   - all three tones are heard together at the speaker, each at about its own level, and the room lists the three stream ids
//   - the SAME packets fed to ONE ordinary Player are shown for contrast (they share one jitter buffer: broken)
//   - per-talker volume (0 mutes only that talker), master volume (0 = silence)
//   - a talker that stops is released after the idle timeout and is heard again when it comes back
//   - SetMaxStreams(): one talker too many is rejected, not mixed in
//   - talkers coming and going all the time while the audio thread runs (run it under TSan / ASan too)
// Build line: tests/README.txt
#include "ravava/ConferencePlayer.h"
#include "ravava/OpusCodec.h"
#include "ravava/VoicePacket.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <random>
#include <thread>

using namespace ravava;

static int g_fail = 0;
#define CHECK(c) do{ if(!(c)){ std::printf("  FAIL line %d: %s\n", __LINE__, #c); g_fail++; } }while(0)

extern void (*g_stubSpeakerTap)(const int16_t*, uint32_t, int);

static const double kFreq[3] = {500, 900, 1500};
static const double kAmp = 6000;

// amplitude of each tone in what the speaker played (first channel): correlation with sin / cos at that frequency
static std::mutex g_mx;
static int g_rate = 48000, g_ch = 1;
static double g_cs[3][2]; static double g_sq = 0; static long g_n = 0;
static void tap(const int16_t *pcm, uint32_t frames, int ch)
{
    std::lock_guard<std::mutex> lk(g_mx);
    for(uint32_t i = 0; i < frames; i++, g_n++){
        const double v = pcm[size_t(i) * ch];
        g_sq += v * v;
        for(int k = 0; k < 3; k++){
            const double w = 2 * M_PI * kFreq[k] / g_rate * double(g_n);
            g_cs[k][0] += v * std::cos(w); g_cs[k][1] += v * std::sin(w);
        }
    }
}
struct Heard { double amp[3]; double rms; };
static Heard takeHeard()
{
    std::lock_guard<std::mutex> lk(g_mx);
    Heard h{{0, 0, 0}, g_n ? std::sqrt(g_sq / g_n) : 0.0};
    for(int k = 0; k < 3; k++){ h.amp[k] = g_n ? 2.0 * std::hypot(g_cs[k][0], g_cs[k][1]) / g_n : 0.0; g_cs[k][0] = g_cs[k][1] = 0; }
    g_sq = 0; g_n = 0;
    return h;
}

struct Talker {
    AudioConfig cfg; OpusCodec enc; uint16_t seq; double ph = 0; double freq; Bytes wire; bool on = true;
    Talker(double f, uint16_t seq0) : seq(seq0), freq(f) {
        cfg.frameMs = 20; cfg.Bitrate = 32000; cfg.useDtx = false;
        if(!enc.InitEncoder(cfg)){ std::printf("encoder init failed\n"); std::exit(2); }
    }
    Bytes next() {
        const int n = cfg.FrameSamples();
        std::vector<int16_t> pcm(static_cast<size_t>(n), int16_t(0));
        for(int i = 0; i < n; i++){ pcm[size_t(i)] = int16_t(kAmp * std::sin(ph)); ph += 2 * M_PI * freq / cfg.inputRate; }
        Bytes opus = enc.Encode(pcm.data(), n);
        VoicePacket::WrapInto(wire, seq, seq == 0, uint8_t(seq & 0x7F), opus); seq++;
        return wire;
    }
};

// The "network": every 20 ms one packet of each talker that is on, 5% lost, 5% sent after the next one. Runs on its own thread.
struct Net {
    Talker t[3] = {Talker(kFreq[0], 0), Talker(kFreq[1], 20000), Talker(kFreq[2], 40000)};
    std::atomic<bool> stop{false};
    std::atomic<bool> on[3] = {{true}, {true}, {true}};
    std::thread th;
    template<class F> void start(F send) {
        th = std::thread([this, send]{
            std::mt19937 rng(7);
            Bytes held[3]; bool haveHeld[3] = {false, false, false};
            auto next = std::chrono::steady_clock::now();
            while(!stop){
                for(int i = 0; i < 3; i++){
                    if(!on[i]) continue;
                    Bytes w = t[i].next();
                    const unsigned r = rng() % 100;
                    if(r < 5) continue;                                       // lost
                    if(r < 10 && !haveHeld[i]){ held[i] = w; haveHeld[i] = true; continue; }   // reordered: after the next one
                    send(i, w);
                    if(haveHeld[i]){ send(i, held[i]); haveHeld[i] = false; }
                }
                next += std::chrono::milliseconds(20);                         // absolute clock: no drift
                std::this_thread::sleep_until(next);
            }
        });
    }
    void join() { stop = true; if(th.joinable()) th.join(); }
};
static void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
static const uint16_t kId[3] = {0x1111, 0x2222, 0x3333};

static void body()
{
    std::printf("conference_test\n");
    g_stubSpeakerTap = tap;
    AudioConfig cfg; cfg.frameMs = 20;
    g_rate = cfg.RenderRate(); g_ch = cfg.channels;

    // ---------------------------------------------------------------- 1. three talkers, one speaker
    {
        ConferencePlayer room;
        room.SetAutoKeepDevice(false);
        room.SetAdaptivePlayout(true);
        room.SetConfig(&cfg);
        Net net;
        net.start([&](int i, const Bytes &w){ room.Play(kId[i], w); });
        sleepMs(1500); takeHeard();                                  // warm-up
        sleepMs(3000);
        const Heard h = takeHeard();
        std::printf("  3 talkers: tone amplitudes %.0f / %.0f / %.0f (sent %.0f), mix rms %.0f, level %d\n", h.amp[0], h.amp[1], h.amp[2], kAmp, h.rms, room.GetLevel());
        for(int k = 0; k < 3; k++) CHECK(h.amp[k] > kAmp * 0.7 && h.amp[k] < kAmp * 1.3);
        auto ids = room.ActiveStreams();
        CHECK(ids.size() == 3);
        for(int i = 0; i < 3; i++){
            JitterBuffer::Stats st; int depth, ms;
            CHECK(room.GetStreamStats(kId[i], st, depth, ms));
            std::printf("    talker %04x: received %u played %u fec %u plc %u lost %u late %u\n", kId[i], st.received, st.played, st.fec, st.plc, st.lost, st.late);
            CHECK(st.played > 100 && st.received > 100);
            CHECK(room.GetStreamLevel(kId[i]) >= 0);
        }
        CHECK(room.Rejected() == 0);

        // ------------------------------------------------------------ 3. per-talker and master volume
        CHECK(room.SetStreamVolume(kId[1], 0));
        sleepMs(300); takeHeard(); sleepMs(1500);
        const Heard m = takeHeard();
        std::printf("  talker 2 muted: %.0f / %.0f / %.0f\n", m.amp[0], m.amp[1], m.amp[2]);
        CHECK(m.amp[1] < kAmp * 0.05);
        CHECK(m.amp[0] > kAmp * 0.7 && m.amp[2] > kAmp * 0.7);
        CHECK(!room.SetStreamVolume(0x9999, 50));                    // no such talker
        room.SetStreamVolume(kId[1], 100);
        room.SetVolume(0);
        sleepMs(300); takeHeard(); sleepMs(1000);
        const Heard z = takeHeard();
        std::printf("  master volume 0: rms %.1f\n", z.rms);
        CHECK(z.rms < 20);
        room.SetVolume(100);
        net.join();
    }

    // ---------------------------------------------------------------- 2. the same packets into ONE ordinary Player (contrast)
    {
        Player one;
        one.SetAutoKeepDevice(false);
        one.SetAdaptivePlayout(true);
        one.SetConfig(&cfg);
        Net net;
        net.start([&](int, const Bytes &w){ one.Play(w); });
        sleepMs(1500); takeHeard(); sleepMs(3000);
        const Heard h = takeHeard();
        JitterBuffer::Stats st; int depth, ms; one.GetStats(st, depth, ms);
        std::printf("  ONE player for 3 talkers (what the demo did): tone amplitudes %.0f / %.0f / %.0f, received %u played %u lost %u late %u plc %u\n",
                    h.amp[0], h.amp[1], h.amp[2], st.received, st.played, st.lost, st.late, st.plc);
        net.join();
    }

    // ---------------------------------------------------------------- 4. idle timeout, coming back
    {
        ConferencePlayer room;
        room.SetAutoKeepDevice(false);
        room.SetIdleTimeoutMs(600);
        room.SetConfig(&cfg);
        Net net;
        net.start([&](int i, const Bytes &w){ room.Play(kId[i], w); });
        sleepMs(1500);
        CHECK(room.ActiveStreams().size() == 3);
        net.on[2] = false;
        sleepMs(2000);
        CHECK(room.ActiveStreams().size() == 2);
        takeHeard(); sleepMs(1000);
        const Heard h = takeHeard();
        CHECK(h.amp[2] < kAmp * 0.05 && h.amp[0] > kAmp * 0.7);
        net.on[2] = true;
        sleepMs(2500); takeHeard(); sleepMs(1500);
        const Heard b = takeHeard();
        std::printf("  talker 3 left and came back: %zu in the room, tone 3 amplitude %.0f\n", room.ActiveStreams().size(), b.amp[2]);
        CHECK(room.ActiveStreams().size() == 3);
        CHECK(b.amp[2] > kAmp * 0.7);
        net.join();
    }

    // ---------------------------------------------------------------- 5. at most N talkers
    {
        ConferencePlayer room;
        room.SetAutoKeepDevice(false);
        room.SetMaxStreams(2);
        room.SetConfig(&cfg);
        Net net;
        net.start([&](int i, const Bytes &w){ room.Play(kId[i], w); });
        sleepMs(2500);
        std::printf("  max 2 talkers: %zu in the room, %u packets rejected\n", room.ActiveStreams().size(), room.Rejected());
        CHECK(room.ActiveStreams().size() == 2);
        CHECK(room.Rejected() > 50);
        net.join();
    }

    // ---------------------------------------------------------------- 6. talkers coming and going while the audio thread runs
    {
        ConferencePlayer room;
        room.SetAutoKeepDevice(false);
        room.SetIdleTimeoutMs(120);
        room.SetConfig(&cfg);
        Net net;
        std::atomic<int> sent{0};
        net.start([&](int i, const Bytes &w){ room.Play(uint16_t(kId[i] + (sent / 40)), w); sent++; });   // a new set of ids every ~0.3 s
        long polls = 0;
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(4);
        while(std::chrono::steady_clock::now() < end){
            room.ActiveStreams(); room.GetLevel();
            for(int i = 0; i < 3; i++){ room.GetStreamLevel(uint16_t(kId[i] + (sent / 40))); room.SetStreamVolume(uint16_t(kId[i] + (sent / 40)), 80); }
            polls++; sleepMs(3);
        }
        net.join();
        std::printf("  churn: %d packets, %ld GUI polls, %zu talkers left\n", sent.load(), polls, room.ActiveStreams().size());
        CHECK(sent > 300);
    }
}

int main()
{
    body();
    g_stubSpeakerTap = nullptr;                                      // after every player is gone: the audio thread no longer reads it
    std::printf(g_fail ? "FAIL: %d check(s)\n" : "PASS\n", g_fail);
    return g_fail ? 1 : 0;
}
