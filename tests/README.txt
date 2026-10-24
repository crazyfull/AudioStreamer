Pipeline test without sound card / miniaudio (fake threaded devices, real libopus):
g++ -std=c++17 -pthread -Itests/stub -Isrc tests/e2e.cpp tests/stub/audioDevice_stub.cpp \
    src/clsAudioRecorder.cpp src/clsAudioPlayer.cpp src/jitterBuffer.cpp src/opusCodec.cpp -lopus -o e2e
(tests/stub/opus.h is only a stand-in for machines without opus headers; with real headers drop -Itests/stub)
