// Music: the local copy ("monitor") and the two volumes. Real Recorder + Player + MusicPlayer, fake microphone / speaker / music file
// (tests/stub/audio_device_stub.cpp: the file "tone880" is an endless 880 Hz sine at 8000), packets decoded and measured, the speaker signal tapped.
//   - the monitor is off by default; on: the speaker plays the SAME music that is sent, with its own volume
//   - SetVolume() (sent) and SetMonitorVolume() (local) do not affect each other; neither affects the received-voice volume
//   - Pause / Resume / Stop silence / restore the local copy too
//   - the speaker may have another rate / channel count than the microphone, and the copy plays without gaps
// No sound card. Build line: tests/README.txt
#include "ravava/Recorder.h"
#include "ravava/Player.h"
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

extern void (*g_stubSpeakerTap)(const int16_t*, uint32_t, int);

// what the speaker played (first channel analysed): energy, zero crossings, biggest jumps
static std::mutex g_mx;
static int g_bigStep = 2500;                                 // a click / gap is bigger than the tone's own biggest step (depends on the rate)
static double g_spSq = 0; static long g_spN = 0, g_spCross = 0, g_spFrames = 0; static int g_spBig = 0, g_spLast = 0;
static void tap(const int16_t *pcm, uint32_t frames, int ch)
{
    std::lock_guard<std::mutex> lk(g_mx);
    for(uint32_t i = 0; i < frames; i++){
        const int v = pcm[i * ch];
        g_spSq += double(v) * v; g_spN++;
        if((v >= 0) != (g_spLast >= 0)) g_spCross++;
        if(std::abs(v - g_spLast) > g_bigStep) g_spBig++;
        g_spLast = v;
    }
    g_spFrames += frames;
}
struct Speaker { double rms; double hz; int big; };
static Speaker takeSpeaker(int rate)
{
    std::lock_guard<std::mutex> lk(g_mx);
    Speaker s{g_spN ? std::sqrt(g_spSq / g_spN) : 0.0, g_spFrames ? double(g_spCross) / 2.0 * rate / double(g_spFrames) : 0.0, g_spBig};
    g_spSq = 0; g_spN = 0; g_spCross = 0; g_spFrames = 0; g_spBig = 0;
    return s;
}

// what is SENT: decoded packets
static std::mutex g_tx; static double g_txSq = 0; static long g_txN = 0;
static double takeSent() { std::lock_guard<std::mutex> lk(g_tx); const double r = g_txN ? std::sqrt(g_txSq / g_txN) : 0; g_txSq = 0; g_txN = 0; return r; }
static void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

static void body(int spkRate, int spkCh)
{
    std::printf("speaker %d Hz, %d ch\n", spkRate, spkCh);
    g_bigStep = int(8000 * 2 * M_PI * 880 / spkRate * 1.5) + 500;
    g_stubSpeakerTap = tap;
    AudioConfig cfg; cfg.frameMs = 20; cfg.Bitrate = 64000; cfg.useDtx = false; cfg.outputRate = spkRate; cfg.channels = 1;
    AudioConfig spk = cfg; spk.channels = spkCh;                // the microphone is mono, the speaker may be stereo

    OpusCodec dec; dec.InitDecoder(cfg.inputRate, 1);
    Recorder rec;
    rec.SetAutoKeepDevice(false);
    rec.SetPacketHandler([&](const Bytes &body){
        uint16_t seq; bool marker; uint8_t tx; Bytes opus; Pcm pcm;
        if(!VoicePacket::Parse(body.data(), body.size(), seq, marker, tx, opus) || !dec.Decode(opus, pcm)) return;
        std::lock_guard<std::mutex> lk(g_tx);
        for(int16_t v : pcm) g_txSq += double(v) * v;
        g_txN += long(pcm.size());
    });
    Player play;
    play.SetAutoKeepDevice(false);
    play.SetMusicMonitor(rec.Music());                          // before SetConfig()
    rec.SetConfig(&cfg);
    play.SetConfig(&spk);
    rec.SetMicEnabled(false);                                   // only the music is in the stream
    rec.Start();
    sleepMs(300);

    auto music = rec.Music();
    CHECK(music->Play("tone880"));
    sleepMs(600); takeSent(); takeSpeaker(spkRate);
    sleepMs(800);
    const double sent0 = takeSent(); const Speaker off = takeSpeaker(spkRate);
    std::printf("  monitor off (default): sent rms %.0f, speaker rms %.0f\n", sent0, off.rms);
    CHECK(sent0 > 4500 && sent0 < 6800);
    CHECK(off.rms < 50);

    music->SetMonitor(true);
    sleepMs(500); takeSent(); takeSpeaker(spkRate);
    sleepMs(1500);
    const double sent1 = takeSent(); const Speaker on = takeSpeaker(spkRate);
    std::printf("  monitor on: sent rms %.0f, speaker rms %.0f at %.0f Hz, jumps (gaps) %d\n", sent1, on.rms, on.hz, on.big);
    CHECK(on.rms > 4500 && on.rms < 6800);                      // the same music (5657), at the same level
    CHECK(std::fabs(on.hz - 880) < 880 * 0.06);                 // the right pitch: no rate error in the conversion
    CHECK(on.big <= 2);                                         // played without gaps / clicks
    CHECK(std::fabs(sent1 - sent0) < sent0 * 0.1);              // the stream is not changed by listening to it

    // independent volumes: stream 50 % (-12 dB), the speaker copy stays
    music->SetVolume(50);
    sleepMs(500); takeSent(); takeSpeaker(spkRate);
    sleepMs(1000);
    const double sent2 = takeSent(); const Speaker on2 = takeSpeaker(spkRate);
    std::printf("  sent volume 50: sent %.1f dB, speaker %.1f dB\n", 20 * std::log10(sent2 / sent1), 20 * std::log10(on2.rms / on.rms));
    CHECK(20 * std::log10(sent2 / sent1) < -10.5 && 20 * std::log10(sent2 / sent1) > -13.5);
    CHECK(std::fabs(20 * std::log10(on2.rms / on.rms)) < 1.0);

    // ... and the other way round: speaker copy 50 %, the stream goes back to 100 %
    music->SetVolume(100); music->SetMonitorVolume(50);
    sleepMs(500); takeSent(); takeSpeaker(spkRate);
    sleepMs(1000);
    const double sent3 = takeSent(); const Speaker on3 = takeSpeaker(spkRate);
    std::printf("  speaker volume 50: sent %.1f dB, speaker %.1f dB\n", 20 * std::log10(sent3 / sent1), 20 * std::log10(on3.rms / on.rms));
    CHECK(std::fabs(20 * std::log10(sent3 / sent1)) < 1.0);
    CHECK(20 * std::log10(on3.rms / on.rms) < -10.5 && 20 * std::log10(on3.rms / on.rms) > -13.5);
    music->SetMonitorVolume(100);

    // the speaker volume of the received voice does not touch the copy
    play.SetVolume(0);
    sleepMs(500); takeSpeaker(spkRate);
    sleepMs(800);
    const Speaker mute = takeSpeaker(spkRate);
    std::printf("  Player::SetVolume(0): speaker rms %.0f (the copy has its own volume)\n", mute.rms);
    CHECK(mute.rms > 4000);
    play.SetVolume(100);

    // Pause / Resume / Stop act on the local copy too
    CHECK(music->Pause());
    sleepMs(500); takeSpeaker(spkRate);
    sleepMs(600);
    const Speaker paused = takeSpeaker(spkRate);
    std::printf("  paused: speaker rms %.0f\n", paused.rms);
    CHECK(paused.rms < 50);
    CHECK(music->Resume());
    sleepMs(600); takeSpeaker(spkRate);
    sleepMs(800);
    const Speaker back = takeSpeaker(spkRate);
    std::printf("  resumed: speaker rms %.0f\n", back.rms);
    CHECK(back.rms > 4500);
    music->Stop();
    sleepMs(500); takeSpeaker(spkRate);
    sleepMs(600);
    CHECK(takeSpeaker(spkRate).rms < 50);

    music->SetMonitor(false);
    rec.Stop();
}

// The sent music is delayed by the time the local copy needs to reach the ear (singing along). Direct calls of Mix(), no devices, no timing.
static bool chunkSilent(const int16_t *p, size_t n) { for(size_t i = 0; i < n; i++) if(p[i]) return false; return true; }
static int firstSoundChunk(MusicPlayer &m, int maxChunks)
{
    const size_t n = 80;                                    // 10 ms at 8000 Hz
    Pcm mic(n, 0);
    for(int c = 0; c < maxChunks; c++){
        bool active = false;
        const int16_t *out = m.Mix(mic.data(), n, active);
        if(active && !chunkSilent(out, n)) return c;
    }
    return -1;
}
static void delayTest()
{
    std::printf("stream delay (music sent later than the local copy)\n");
    {
        MusicPlayer m; m.Prepare(8000, 1);
        CHECK(m.GetStreamDelayMs() == 0);                   // monitor off: nothing to compensate
        m.SetMonitor(true);
        CHECK(m.GetStreamDelayMs() == 50);                  // automatic: local queue + speaker buffer
        m.SetStreamDelayMs(120);
        CHECK(m.GetStreamDelayMs() == 120);
        m.SetStreamDelayMs(9999);
        CHECK(m.GetStreamDelayMs() == MusicPlayer::kMaxStreamDelayMs);
        m.SetStreamDelayMs(-1);
        CHECK(m.GetStreamDelayMs() == 50);
    }
    {
        MusicPlayer m; m.Prepare(8000, 1); m.SetStreamDelayMs(0);
        CHECK(m.Play("tone880"));
        CHECK(firstSoundChunk(m, 20) == 0);                 // no delay: the music is in the very first chunk
    }
    {
        MusicPlayer m; m.Prepare(8000, 1); m.SetStreamDelayMs(100);
        CHECK(m.Play("tone880"));
        const int c = firstSoundChunk(m, 40);
        std::printf("  delay 100 ms: the music starts in chunk %d (10 ms chunks)\n", c);
        CHECK(c == 10);                                     // 10 silent chunks = 100 ms, then the tone
        // stop + play again: the delay line starts empty again (no tail of the old play)
        m.Stop();
        CHECK(m.Play("tone880"));
        CHECK(firstSoundChunk(m, 40) == 10);
    }
}

static void run(int spkRate, int spkCh)
{
    body(spkRate, spkCh);                   // (the recorder and the player are gone when it returns)
    g_stubSpeakerTap = nullptr;
}

int main()
{
    run(48000, 1);                  // the same format as the microphone
    run(24000, 2);                  // another rate and stereo: converted
    run(16000, 1);
    delayTest();
    std::printf(g_fail ? "FAILED (%d)\n" : "PASS\n", g_fail);
    return g_fail ? 1 : 0;
}
