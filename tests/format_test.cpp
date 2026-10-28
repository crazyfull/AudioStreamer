// The player takes the stream format from the packets, not from AudioConfig: any sender input rate, frame size,
// bitrate, bandwidth and mono / stereo, switching while it talks, and a stray other-format packet that must not
// disturb the stream. Real player + real Opus, fake speaker (tests/stub/audio_device_stub.cpp), no sound card.
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
static std::atomic<long> g_n{0};
static void tap(const int16_t *pcm, uint32_t frames, int ch)
{
    double s = g_sumSq.load();
    for(uint32_t i = 0; i < frames * ch; i++) s += double(pcm[i]) * pcm[i];
    g_sumSq = s; g_n += frames * ch;
}
static double rms() { const double r = g_n.load() ? std::sqrt(g_sumSq.load() / g_n.load()) : 0; g_sumSq = 0; g_n = 0; return r; }

static int g_fail = 0;
#define CHECK(c) do{ if(!(c)){ std::printf("  FAIL line %d: %s\n", __LINE__, #c); g_fail++; } }while(0)

struct Sender {
    AudioConfig cfg; OpusCodec enc; uint16_t seq = 0; double ph = 0; Bytes wire;
    Sender(int rate, int ch, int frameMs, int bitrate, int bandwidth, bool dtx = false) {
        cfg.inputRate = rate; cfg.channels = ch; cfg.frameMs = frameMs; cfg.opusBitrate = bitrate;
        cfg.maxBandwidth = bandwidth; cfg.useDtx = dtx;
        if(!enc.InitEncoder(cfg)){ std::printf("encoder init failed\n"); std::exit(2); }
    }
    Bytes next() {                                      // one packet of a 1 kHz tone
        const int n = cfg.FrameSamples();
        std::vector<int16_t> pcm(size_t(n) * cfg.channels);
        for(int i = 0; i < n; i++){
            const int16_t v = int16_t(8000.0 * std::sin(ph)); ph += 2 * M_PI * 1000 / cfg.inputRate;
            for(int c = 0; c < cfg.channels; c++) pcm[size_t(i) * cfg.channels + c] = v;
        }
        Bytes opus = enc.Encode(pcm.data(), n);
        VoicePacket::WrapInto(wire, seq, seq == 0, uint8_t(seq & 0x7F), opus); seq++;      // tx counts packets, like the recorder
        return wire;
    }
};

static void talk(Player &p, Sender &s, int ms)
{
    for(int t = 0; t < ms; t += s.cfg.frameMs){
        p.Play(s.next());
        std::this_thread::sleep_for(std::chrono::milliseconds(s.cfg.frameMs));
    }
}

int main()
{
    g_stubSpeakerTap = tap;
    // the speaker side: stereo 24 kHz here, to show the sender formats don't have to match it
    AudioConfig spk; spk.channels = 2; spk.outputRate = 24000;
    Player play; play.SetConfig(&spk);

    struct Case { const char *name; int rate, ch, ms, bitrate, bw; } cases[] = {
        { "48k mono   20ms 24kbps fullband  ", 48000, 1, 20, 24000, OPUS_BANDWIDTH_FULLBAND },
        { "16k mono   60ms 16kbps wideband  ", 16000, 1, 60, 16000, OPUS_BANDWIDTH_WIDEBAND },
        { "48k stereo 40ms 64kbps fullband  ", 48000, 2, 40, 64000, OPUS_BANDWIDTH_FULLBAND },
        { "24k stereo 20ms 32kbps superwide ", 24000, 2, 20, 32000, OPUS_BANDWIDTH_SUPERWIDEBAND },
        { " 8k mono   10ms 12kbps narrowband", 8000,  1, 10, 12000, OPUS_BANDWIDTH_NARROWBAND },
    };
    std::printf("stream format from the packets (speaker: 24000 Hz, 2 ch)\n");
    for(const Case &c : cases){
        Sender s(c.rate, c.ch, c.ms, c.bitrate, c.bw);
        talk(play, s, 700); rms();                      // the format switch + prefill
        talk(play, s, 800);
        const double r = rms();
        JitterBuffer::Stats st; int depth = 0, frameMs = 0;
        const bool ok = play.GetStats(st, depth, frameMs);
        std::printf("  %s -> plays %5.0f rms, player frame %d ms, lost %u\n", c.name, r, frameMs, st.lost);
        CHECK(ok && frameMs == c.ms);
        CHECK(st.lost <= 3);                            // clean network: nothing counted lost
        CHECK(r > 2500);                                // the tone arrives (~-12 dBFS = 8000/sqrt2 = 5650 before codec/mixing)
    }

    // (the last case above is 10 ms, so the 20 ms sender below starts a fresh stream, as a real new talker would)
    // stray packets of another duration inside a running stream: must not restart the stream or disturb the audio
    Sender main20(48000, 1, 20, 24000, OPUS_BANDWIDTH_FULLBAND), stray60(16000, 1, 60, 16000, OPUS_BANDWIDTH_WIDEBAND);
    talk(play, main20, 800); rms();
    JitterBuffer::Stats before; int d, fm; play.GetStats(before, d, fm);
    for(int i = 0; i < 40; i++){
        play.Play(main20.next());
        if(i % 2 == 0) play.Play(stray60.next());       // every other packet is a stray 60 ms one
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    const double r = rms();
    JitterBuffer::Stats after; play.GetStats(after, d, fm);
    std::printf("  stray 60 ms packets: stream stays at %d ms, plays %.0f rms, refused %u\n", fm, r, after.formatDropped);
    CHECK(fm == 20 && r > 2500 && after.formatDropped >= 15);

    // the sender restarts with the SAME format (sequence number back to 0, marker set): audio must resume at once,
    // not after the old sequence number is reached again
    Sender again(48000, 1, 20, 24000, OPUS_BANDWIDTH_FULLBAND);
    talk(play, again, 300); rms();
    talk(play, again, 800);
    const double rr = rms();
    std::printf("  sender restart (seq back to 0), same format: plays %.0f rms\n", rr);
    CHECK(rr > 2500);

    // garbage is refused, never reaches the decoder
    const uint32_t refused = after.formatDropped;
    Bytes junk = {0, 0, 0};                             // audio header only, no Opus byte
    play.Play(junk);
    junk = {0, 0, 0, 0xFF, 0xFF, 0xFF};
    play.Play(junk);
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    JitterBuffer::Stats last; play.GetStats(last, d, fm);
    CHECK(last.formatDropped > refused && fm == 20);

    std::printf(g_fail ? "FAILED (%d)\n" : "PASS\n", g_fail);
    return g_fail ? 1 : 0;
}
