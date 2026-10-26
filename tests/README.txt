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
