// Full pipeline test WITH echo cancellation: recorder -> opus -> player -> speaker -> (simulated room) -> mic.
// This is the "Loopback" situation of the app: the player's output comes straight back into the
// microphone. Without echo cancellation the loop keeps ringing after the talker stops.
//
// Real webrtc-audio-processing + real libopus, fake devices (tests/stub/audioDevice_room_stub.cpp).
// Build: see tests/README.txt. Takes ~20 s (real time).
#include "clsAudioRecorder.h"
#include "clsAudioPlayer.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

extern std::atomic<bool>   g_talker;
extern std::atomic<double> g_echoGain;
extern std::atomic<int>    g_echoDelayMs;
std::vector<double> roomSpeakerEnergy();
void roomReset();

// seconds are measured in 10 ms speaker blocks
static double blockDb(const std::vector<double> &e, double fromSec, double toSec)
{
    size_t a = size_t(fromSec * 100), b = std::min(e.size(), size_t(toSec * 100));
    double sum = 0;
    for(size_t i = a; i < b; ++i) sum += e[i];
    return 10 * std::log10(std::fmax(sum, 1.0) / double(b - a));
}

// returns {level while the person talks (dB), level of the tail 3.5..5.5 s after he stopped}
static std::pair<double,double> run(bool withAec, int *outLostPackets)
{
    roomReset();
    g_talker = true;
    g_echoGain = 0.9;           // very strong coupling on purpose (loop gain close to 1)
    g_echoDelayMs = 60;

    AudioConfig cfg;
    cfg.frameMs = 20; cfg.opusBitrate = 24000; cfg.expectedLossPerc = 0; cfg.useDtx = false;
    cfg.useEchoCancel = withAec;

    auto rec  = std::make_unique<clsAudioRecorder>();
    auto play = std::make_unique<clsAudioPlayer>();

    std::shared_ptr<AudioProcessor> aec;
    if(withAec){
        aec = std::make_shared<AudioProcessor>();
        AudioProcessor::Options o; o.streamDelayMs = cfg.aecDelayMs;
        std::string err;
        if(!aec->init(cfg.inputRate, cfg.renderRate(), cfg.channels, o, &err)){
            std::printf("FAIL: aec init: %s\n", err.c_str());
            std::exit(1);
        }
    }
    rec->SetAudioProcessor(aec);
    play->SetAudioProcessor(aec);

    rec->SetPacketHandler([&](const Bytes &d){ play->Play(d); });     // loopback, called on the mic thread
    rec->SetConfig(&cfg);
    play->SetConfig(&cfg);
    rec->Start();

    std::this_thread::sleep_for(std::chrono::seconds(4));
    g_talker = false;                                                 // the person stops talking
    std::this_thread::sleep_for(std::chrono::milliseconds(5500));

    rec->Stop();
    JitterBuffer::Stats st; int depth, ms;
    play->GetStats(st, depth, ms);
    if(outLostPackets) *outLostPackets = int(st.lost);

    std::vector<double> e = roomSpeakerEnergy();
    rec.reset();
    play.reset();
    return { blockDb(e, 1.5, 4.0), blockDb(e, 5.5, 9.0) };
}

int main()
{
    int lostOff = 0, lostOn = 0;
    auto off = run(false, &lostOff);
    auto on  = run(true,  &lostOn);

    std::printf("speaker level while talking : AEC off %6.1f dB   AEC on %6.1f dB\n", off.first, on.first);
    std::printf("speaker level after he stops: AEC off %6.1f dB   AEC on %6.1f dB   (loop ringing)\n", off.second, on.second);
    std::printf("lost packets                : AEC off %d   AEC on %d\n", lostOff, lostOn);

    const double ringOff = off.second - off.first;      // how loud the tail is relative to speech
    const double ringOn  = on.second  - on.first;
    std::printf("tail relative to speech     : AEC off %6.1f dB   AEC on %6.1f dB\n", ringOff, ringOn);

    bool ok = true;
    if(ringOn > ringOff - 15){ std::printf("FAIL: echo canceller removes less than 15 dB of the ringing loop\n"); ok = false; }
    if(on.first < off.first - 12){ std::printf("FAIL: the talker's voice got much quieter with AEC on (%.1f dB)\n", on.first - off.first); ok = false; }
    std::printf(ok ? "PASS\n" : "FAILED\n");
    return ok ? 0 : 1;
}
