Pipeline test without sound card / miniaudio / webrtc (fake threaded devices, real libopus).
AudioProcessor.cpp is built WITHOUT -DRAVAVA_WITH_WEBRTC here = pass-through, so this also checks the no-webrtc build:
g++ -std=c++17 -pthread -Itests/stub -Isrc tests/e2e.cpp tests/stub/audio_device_stub.cpp \
    src/ravava/AudioProcessor.cpp src/ravava/Recorder.cpp src/ravava/Player.cpp src/ravava/JitterBuffer.cpp src/ravava/OpusCodec.cpp -lopus -o e2e
(tests/stub/opus.h is only a stand-in for machines without opus headers; with real headers drop -Itests/stub)

Echo canceller tests need the REAL webrtc-audio-processing 1.3 (see libs/webrtc/README.md), here via pkg-config:
WEBRTC="$(pkg-config --cflags webrtc-audio-processing-1)"; WEBRTC_LIBS="$(pkg-config --libs --static webrtc-audio-processing-1)"

1) the canceller alone: simulated room echo (60 ms + reflections), echo only, then double talk.
   ./aec_test [renderRate] [noref]      (noref = control run without the far-end signal, must show ~0 dB)
g++ -std=c++17 -O2 -pthread -Isrc -DRAVAVA_WITH_WEBRTC $WEBRTC tests/aec_test.cpp src/ravava/AudioProcessor.cpp -o aec_test $WEBRTC_LIBS

2) whole pipeline, loopback through a simulated room (~20 s real time): recorder + opus + player + canceller
g++ -std=c++17 -O2 -pthread -Isrc -DRAVAVA_WITH_WEBRTC $WEBRTC tests/e2e_aec.cpp tests/stub/audio_device_room_stub.cpp src/ravava/AudioProcessor.cpp \
    src/ravava/Recorder.cpp src/ravava/Player.cpp src/ravava/JitterBuffer.cpp src/ravava/OpusCodec.cpp -lopus -o e2e_aec $WEBRTC_LIBS

Windows (MSVC 2019 x64): tests\build_tests.bat builds both echo canceller tests into tests\bin
(run it from "x64 Native Tools Command Prompt for VS 2019"; settings are at the top of the file).

Unit tests without webrtc / sound card (Linux; on Windows build_tests.bat builds them as unit_test.exe / noalloc_test.exe):
  g++ -std=c++17 -O1 -pthread -I/usr/include/opus -Isrc tests/unit_test.cpp src/ravava/JitterBuffer.cpp src/ravava/OpusCodec.cpp src/ravava/net/PacketRouter.cpp -lopus -o unit_test
      CutBlocks, PacketQueue (2 producer threads), AudioConfig::Validate / Opus encoder checks, JitterBuffer loss counters
  g++ -std=c++17 -O1 -pthread -I/usr/include/opus -Isrc tests/noalloc_test.cpp tests/stub/audio_device_stub.cpp src/ravava/Recorder.cpp src/ravava/Player.cpp src/ravava/JitterBuffer.cpp src/ravava/OpusCodec.cpp src/ravava/net/PacketRouter.cpp -lopus -o noalloc_test
      counts operator new while recorder and player run (lossy, reordering network): must be 0 in steady state
  g++ -std=c++17 -O1 -pthread -I/usr/include/opus -Isrc -Itests/ref tests/jitter_diff_test.cpp tests/ref/refJitterBuffer.cpp src/ravava/JitterBuffer.cpp src/ravava/OpusCodec.cpp -lopus -o jitter_diff_test
      real (fixed-slot) JitterBuffer vs the old std::map version in tests/ref: random loss / reorder / duplicates / DTX / restarts,
      every output frame and statistic must be identical

Receive volume (player gain, level meter independent of it; no webrtc, no sound card):
  g++ -std=c++17 -O1 -pthread -I/usr/include/opus -Isrc tests/volume_test.cpp tests/stub/audio_device_stub.cpp src/ravava/Player.cpp src/ravava/JitterBuffer.cpp src/ravava/OpusCodec.cpp src/ravava/AudioProcessor.cpp -lopus -o volume_test

Stream format from the packets (player takes frame size from the Opus packets; any sender rate / bandwidth / mono-stereo,
format switch while talking, stray packets, sender restart; no webrtc, no sound card):
  g++ -std=c++17 -O1 -pthread -I/usr/include/opus -Isrc tests/format_test.cpp tests/stub/audio_device_stub.cpp src/ravava/Player.cpp src/ravava/JitterBuffer.cpp src/ravava/OpusCodec.cpp src/ravava/AudioProcessor.cpp -lopus -o format_test

Stress / fuzz (run under sanitizers; ~25 s plain, ~1-2 min with TSan or ASan+UBSan):
  Player: 3 threads Play() while the sender frame size flips + garbage + a polling thread (GetStats / levels / SetVolume),
  then one clean talker must be heard again. JitterBuffer: random loss / reorder / duplicates / DTX / sender restarts,
  restart must be audible again within 150 frames.
  g++ -std=c++17 -O1 -g -pthread -fsanitize=thread -I/usr/include/opus -Isrc tests/stress_test.cpp tests/stub/audio_device_stub.cpp src/ravava/Player.cpp src/ravava/JitterBuffer.cpp src/ravava/OpusCodec.cpp src/ravava/AudioProcessor.cpp -lopus -o stress_tsan
  (same with -fsanitize=address,undefined for ASan/UBSan)

Microphone gain (real Recorder + Opus, fake mic; 100 = unchanged, 50 = -12 dB, 200 = +12 dB, 0 = mute, no click on a jump):
  g++ -std=c++17 -O1 -pthread -I/usr/include/opus -Isrc tests/mic_gain_test.cpp tests/stub/audio_device_stub.cpp src/ravava/Recorder.cpp src/ravava/OpusCodec.cpp src/ravava/AudioProcessor.cpp -lopus -o mic_gain_test

Network simulation (virtual clock, real libopus packets, no sound card; ~1 min): random / burst loss, jitter, 3G link stalls, latency step,
speaker clock drift +-300 ppm. Prints delay (avg/p95/max), bad%, underrun / PLC / FEC / silent / skipped / stretched / late per scenario;
--check turns the limits into the exit code. "legacy" = reactive buffer (no arrival time), "timed" = adaptive playout (what Player uses):
  g++ -std=c++17 -O2 -pthread -I/usr/include/opus -Isrc tests/net_sim_test.cpp src/ravava/JitterBuffer.cpp src/ravava/OpusCodec.cpp -lopus -o net_sim_test
  ./net_sim_test legacy --check ; ./net_sim_test timed --check ; ./net_sim_test timed "3G"   (a name filter runs one scenario)
  add "speech" for a speech-like signal (words, pauses with faint noise) instead of a steady tone: the quiet frames the playout control
  slips on exist then, and "forced" shows how many slips had to happen on a loud frame anyway. A steady tone is the worst case.

The whole receive path in REAL time (~75 s): simulated network -> Player::Play (arrival stamp) -> PacketQueue -> audio thread -> JitterBuffer -> Opus -> fake speaker,
legacy vs adaptive, clean / jitter 30 ms / loss 5%. Checks that the arrival times really reach the jitter buffer (jitter is measured only when adaptive),
that the depth follows the network and that the speaker keeps playing. (net_sim_test above feeds the JitterBuffer directly, on a virtual clock.)
  g++ -std=c++17 -O2 -pthread -I/usr/include/opus -Isrc tests/player_sim_test.cpp tests/stub/audio_device_stub.cpp src/ravava/Player.cpp src/ravava/JitterBuffer.cpp src/ravava/OpusCodec.cpp src/ravava/AudioProcessor.cpp -lopus -o player_sim_test

More net_sim_test modes:
  fixed burst lengths 3 / 5 / 10 / 20 / 30 packets (columns plc, silent, rebuf show what each length costs);
  ./net_sim_test compare [speech]   legacy and adaptive side by side; fails if adaptive is much worse than legacy anywhere
                                    (bad% <= legacy*1.25+0.5, underrun <= legacy*2+30, delay <= legacy*1.8+20 ms). Guard rail, not tuning.
  ./net_sim_test compare dtx        (also "timed dtx") the sender skips silent frames like Recorder does. NOT part of ctest: it fails today.
                                    With DTX the adaptive playout (default limits 40..200 ms) is clearly worse than the plain buffer: about twice
                                    the underruns / PLC frames and hundreds of late packets when the speaker clock drifts. The plain buffer's
                                    deeper cushion bridges the short silences inside words; the shallow adaptive one runs dry before the next packet.
                                    Setting the minimum depth to ~100 ms (Player::SetJitterLimitsMs(100, 200)) brings the underruns back to the plain
                                    buffer's level but not the late packets. => with DTX keep SetAdaptivePlayout(false). (Counted PLC frames in the
                                    natural silence are not audible damage, so bad% is pessimistic here; the late packets are real losses.)

Audio callback time (real Player, fake speaker thread that times every callback; bursts of up to 256 datagrams = a full PacketQueue,
plus a GUI thread polling GetStats() / SetVolume() at ~3 kHz; ~30 s):
  g++ -std=c++17 -O2 -pthread -I/usr/include/opus -Isrc tests/callback_time_test.cpp tests/stub/audio_device_stub.cpp src/ravava/Player.cpp src/ravava/JitterBuffer.cpp src/ravava/OpusCodec.cpp src/ravava/AudioProcessor.cpp -lopus -o callback_time_test
  Reports longest / count / over half the period / over the deadline; fails when one callback takes 5 ms or more, or one misses the 10 ms deadline.

