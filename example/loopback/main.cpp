/*
 * RavAva - real-time voice library (Opus over any datagram transport)
 * Copyright (c) 2026 Mojtaba Kahvand. All rights reserved.
 * Author: Mojtaba Kahvand
 *
 * Proprietary software, NOT free / open source. Use, copying, modification and distribution are forbidden without a
 * written license from the author. See the LICENSE file in the repository root.
 * SPDX-License-Identifier: LicenseRef-RavAva-Proprietary
 */
// RavAva loopback: what the microphone records is Opus-encoded by ravava::Recorder, handed straight to ravava::Player
// (optionally through a simulated lossy network) and played by the speaker. Fixed format: 48000 Hz, mono, fullband.
// Use headphones (or --echo 1..4 with the webrtc build) or the speaker will howl into the microphone.
#include <ravava/Recorder.h>
#include <ravava/Player.h>
#include <ravava/AudioDevice.h>
#include <ravava/AudioProcessor.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>

using namespace ravava;

static std::atomic<bool> g_stop{false};
static void onSignal(int) { g_stop = true; }

static void usage(const char *me)
{
    std::printf(
        "RavAva loopback: microphone -> Opus -> jitter buffer -> speaker   (48000 Hz, mono, fullband)\n\n"
        "usage: %s [options]\n"
        "  -l, --list           list the input / output devices and exit\n"
        "  -i, --in N           microphone number from --list (default: system default)\n"
        "  -o, --out N          speaker number from --list (default: system default)\n"
        "  -b, --bitrate BPS    Opus bitrate (default 64000)\n"
        "  -f, --frame MS       frame length 5/10/20/40/60/80/100/120 (default 20)\n"
        "      --music          music mode (Opus AUDIO + MUSIC instead of VOIP + VOICE)\n"
        "      --dtx            do not send silence\n"
        "      --no-fec         switch the in-band FEC off\n"
        "      --loss PCT       drop PCT %% of the packets (simulated network, default 0)\n"
        "      --reorder PCT    deliver PCT %% of the packets one packet late (default 0)\n"
        "  -g, --gain PCT       microphone gain 0..200 (default 100)\n"
        "  -v, --volume PCT     playback volume 0..100 (default 100)\n"
        "      --echo N         echo cancellation 0 = off, 1..4 = on, higher protects your voice more (needs webrtc)\n"
        "      --ns N           noise suppression 0..4 (needs webrtc)\n"
        "      --agc            automatic gain control (needs webrtc)\n"
        "      --hpf            low-cut (high-pass, below ~100 Hz) filter on the microphone (needs webrtc; off by default)\n"
        "  -t, --time SEC       stop after SEC seconds (default: until Ctrl+C)\n"
        "  -q, --quiet          no live status line\n"
        "  -h, --help\n", me);
}

static const char *argValue(int argc, char **argv, int &i)
{
    if(i + 1 >= argc){
        std::fprintf(stderr, "missing value for %s\n", argv[i]);
        std::exit(2);
    }
    return argv[++i];
}

static void listDevices()
{
    const auto in = ListInputDevices(), out = ListOutputDevices();
    std::printf("microphones:\n");
    for(size_t k = 0; k < in.size(); ++k)
        std::printf("  [%zu] %s%s\n", k, in[k].name.c_str(), in[k].isDefault ? "   (default)" : "");
    std::printf("speakers:\n");
    for(size_t k = 0; k < out.size(); ++k)
        std::printf("  [%zu] %s%s\n", k, out[k].name.c_str(), out[k].isDefault ? "   (default)" : "");
}

static std::string bar(int level)
{
    std::string s(size_t(level / 5), '#');
    s.append(size_t(20 - level / 5), '-');
    return s;
}

int main(int argc, char **argv)
{
    AudioConfig cfg;
    cfg.inputRate = cfg.outputRate = 48000;     // fixed: 48000 Hz, mono, fullband
    cfg.channels = 1;
    cfg.maxBandwidth = 1105;                    // OPUS_BANDWIDTH_FULLBAND
    cfg.Bitrate = 64000;
    cfg.frameMs = 20;
    cfg.useEchoCancel = false;
    cfg.expectedLossPerc = 0;

    int inIndex = -1, outIndex = -1, gain = 100, volume = 100, seconds = 0, echo = 0;
    double lossPct = 0, reorderPct = 0;
    bool quiet = false;

    for(int i = 1; i < argc; ++i){
        const std::string a = argv[i];
        if(a == "-h" || a == "--help"){ usage(argv[0]); return 0; }
        else if(a == "-l" || a == "--list"){ listDevices(); return 0; }
        else if(a == "-i" || a == "--in")       inIndex = std::atoi(argValue(argc, argv, i));
        else if(a == "-o" || a == "--out")      outIndex = std::atoi(argValue(argc, argv, i));
        else if(a == "-b" || a == "--bitrate")  cfg.Bitrate = std::atoi(argValue(argc, argv, i));
        else if(a == "-f" || a == "--frame")    cfg.frameMs = std::atoi(argValue(argc, argv, i));
        else if(a == "--music")                 cfg.musicMode = true;
        else if(a == "--dtx")                   cfg.useDtx = true;
        else if(a == "--no-fec")                cfg.useFec = false;
        else if(a == "--loss")                  lossPct = std::atof(argValue(argc, argv, i));
        else if(a == "--reorder")               reorderPct = std::atof(argValue(argc, argv, i));
        else if(a == "-g" || a == "--gain")     gain = std::atoi(argValue(argc, argv, i));
        else if(a == "-v" || a == "--volume")   volume = std::atoi(argValue(argc, argv, i));
        else if(a == "--echo")                  echo = std::atoi(argValue(argc, argv, i));
        else if(a == "--ns")                    cfg.noiseSuppression = std::atoi(argValue(argc, argv, i));
        else if(a == "--agc")                   cfg.autoGain = true;
        else if(a == "--hpf")                   cfg.highPassFilter = true;
        else if(a == "-t" || a == "--time")     seconds = std::atoi(argValue(argc, argv, i));
        else if(a == "-q" || a == "--quiet")    quiet = true;
        else { std::fprintf(stderr, "unknown option %s (try --help)\n", a.c_str()); return 2; }
    }
    if(lossPct > 0)
        cfg.expectedLossPerc = int(lossPct + 0.5) < 5 ? 5 : int(lossPct + 0.5);   // tell the encoder to spend FEC on it
    cfg.useEchoCancel = echo > 0;
    cfg.aecNearendProtection = echo > 0 ? echo - 1 : 0;

    std::string err;
    if(!cfg.Validate(&err)){ std::fprintf(stderr, "%s\n", err.c_str()); return 2; }

    if(inIndex >= 0){
        const auto in = ListInputDevices();
        if(size_t(inIndex) >= in.size()){ std::fprintf(stderr, "no microphone number %d (see --list)\n", inIndex); return 2; }
        cfg.inputDeviceId = in[size_t(inIndex)].id;
    }
    if(outIndex >= 0){
        const auto out = ListOutputDevices();
        if(size_t(outIndex) >= out.size()){ std::fprintf(stderr, "no speaker number %d (see --list)\n", outIndex); return 2; }
        cfg.outputDeviceId = out[size_t(outIndex)].id;
    }

    Recorder rec;
    Player play;
    play.SetVolume(volume);
    rec.SetInputGain(gain);

    // ONE echo canceller shared by the recorder and the player (before SetConfig: that opens the speaker)
    std::shared_ptr<AudioProcessor> aec;
    if(cfg.useEchoCancel || cfg.noiseSuppression > 0 || cfg.autoGain || cfg.highPassFilter){
        if(!AudioProcessor::IsAvailable()){
            std::fprintf(stderr, "note: this build has no webrtc-audio-processing: --echo / --ns / --agc are ignored\n");
        }else{
            aec = std::make_shared<AudioProcessor>();
            AudioProcessor::Options opt;
            opt.streamDelayMs = cfg.aecDelayMs;
            opt.nearendProtection = cfg.aecNearendProtection;
            opt.echoCancel = cfg.useEchoCancel;
            opt.noiseSuppression = cfg.noiseSuppression;
            opt.autoGain = cfg.autoGain;
            opt.highPassFilter = cfg.highPassFilter || cfg.useEchoCancel;     // (the echo canceller always had it)
            if(!aec->Init(cfg.inputRate, cfg.RenderRate(), cfg.channels, opt, &err)){
                std::fprintf(stderr, "echo canceller disabled: %s\n", err.c_str());
                aec.reset();
            }
        }
    }
    rec.SetAudioProcessor(aec);
    play.SetAudioProcessor(aec);

    // the "network": optional loss and reordering, then straight into the player
    std::mutex netMutex;
    std::mt19937 rng(std::random_device{}());
    std::uniform_real_distribution<double> dice(0.0, 100.0);
    Bytes held;
    bool haveHeld = false;
    std::atomic<unsigned> sent{0}, dropped{0};
    rec.SetPacketHandler([&](const Bytes &d){
        std::lock_guard<std::mutex> lk(netMutex);
        sent++;
        if(lossPct > 0 && dice(rng) < lossPct){ dropped++; return; }
        if(reorderPct > 0 && !haveHeld && dice(rng) < reorderPct){ held = d; haveHeld = true; return; }
        play.Play(d);
        if(haveHeld){ play.Play(held); haveHeld = false; }
    });

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    rec.SetConfig(&cfg);
    play.SetConfig(&cfg);
    rec.Start();

    std::printf("loopback: 48000 Hz mono fullband, Opus %d bps, %d ms frames%s%s%s, loss %.1f%%%s\n",
                cfg.Bitrate, cfg.frameMs, cfg.musicMode ? ", music mode" : "", cfg.useDtx ? ", DTX" : "",
                cfg.useFec ? ", FEC" : "", lossPct, aec ? (cfg.useEchoCancel ? ", echo cancellation on" : ", webrtc processing on") : "");
    if(!aec)
        std::printf("use headphones, the speaker would howl into the microphone.   Ctrl+C stops.\n");
    else
        std::printf("Ctrl+C stops.\n");

    const auto t0 = std::chrono::steady_clock::now();
    while(!g_stop){
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - t0).count();
        if(seconds > 0 && elapsed >= seconds)
            break;
        if(quiet)
            continue;
        JitterBuffer::Stats st;
        int depth = 0, frameMs = 0;
        const bool ok = play.GetStats(st, depth, frameMs);
        std::printf("\rmic [%s] spk [%s]  sent %u (dropped %u)  recv %u lost %u fec %u plc %u  buf %d ms  underruns %u   ",
                    bar(rec.GetLevel()).c_str(), bar(play.GetLevel()).c_str(), sent.load(), dropped.load(),
                    ok ? st.received : 0u, ok ? st.lost : 0u, ok ? st.fec : 0u, ok ? st.plc : 0u,
                    ok ? depth * frameMs : 0, ok ? st.underrun : 0u);
        std::fflush(stdout);
    }
    std::printf("\n");

    rec.Stop();
    JitterBuffer::Stats st;
    int depth = 0, frameMs = 0;
    if(play.GetStats(st, depth, frameMs))
        std::printf("done: sent %u, dropped %u, received %u, lost %u, recovered by FEC %u, concealed (PLC) %u, too late %u, underruns %u\n",
                    sent.load(), dropped.load(), st.received, st.lost, st.fec, st.plc, st.late, st.underrun);
    else
        std::printf("done: nothing was received (is the microphone / speaker available?)\n");
    return 0;
}
