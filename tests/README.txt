Pipeline test without sound card / miniaudio / webrtc (fake threaded devices, real libopus):
g++ -std=c++17 -pthread -Itests/stub -Isrc tests/e2e.cpp tests/stub/audioDevice_stub.cpp tests/stub/echoCanceller_stub.cpp \
    src/clsAudioRecorder.cpp src/clsAudioPlayer.cpp src/jitterBuffer.cpp src/opusCodec.cpp -lopus -o e2e
(tests/stub/opus.h is only a stand-in for machines without opus headers; with real headers drop -Itests/stub)

Echo canceller tests need the REAL webrtc-audio-processing 1.3 (see libs/webrtc/README.md), here via pkg-config:
WEBRTC="$(pkg-config --cflags webrtc-audio-processing-1)"; WEBRTC_LIBS="$(pkg-config --libs --static webrtc-audio-processing-1)"

1) the canceller alone: simulated room echo (60 ms + reflections), echo only, then double talk.
   ./aec_test [renderRate] [noref]      (noref = control run without the far-end signal, must show ~0 dB)
g++ -std=c++17 -O2 -pthread -Isrc $WEBRTC tests/aec_test.cpp src/echoCanceller.cpp -o aec_test $WEBRTC_LIBS

2) whole pipeline, loopback through a simulated room (~20 s real time): recorder + opus + player + canceller
g++ -std=c++17 -O2 -pthread -Isrc $WEBRTC tests/e2e_aec.cpp tests/stub/audioDevice_room_stub.cpp src/echoCanceller.cpp \
    src/clsAudioRecorder.cpp src/clsAudioPlayer.cpp src/jitterBuffer.cpp src/opusCodec.cpp -lopus -o e2e_aec $WEBRTC_LIBS

Windows (MSVC 2019 x64): tests\build_tests.bat builds both echo canceller tests into tests\bin
(run it from "x64 Native Tools Command Prompt for VS 2019"; settings are at the top of the file).

Unit tests without webrtc / sound card (Linux; on Windows build_tests.bat builds them as unit_test.exe / noalloc_test.exe):
  g++ -std=c++17 -O1 -pthread -I/usr/include/opus -Isrc tests/unit_test.cpp src/jitterBuffer.cpp src/opusCodec.cpp -lopus -o unit_test
      cutBlocks, PacketQueue (2 producer threads), AudioConfig::validate / Opus encoder checks, JitterBuffer loss counters
  g++ -std=c++17 -O1 -pthread -I/usr/include/opus -Isrc tests/noalloc_test.cpp tests/stub/audioDevice_stub.cpp src/clsAudioRecorder.cpp src/opusCodec.cpp -lopus -o noalloc_test
      counts operator new while the recorder runs: must be 0 in steady state
