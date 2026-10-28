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

