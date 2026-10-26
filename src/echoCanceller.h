#ifndef ECHOCANCELLER_H
#define ECHOCANCELLER_H

// Acoustic echo cancellation (AEC3 from webrtc-audio-processing 1.3) + optional noise suppression
// and automatic gain (AGC2) on the same mic stream, Qt-free.
// The webrtc headers are only included by echoCanceller.cpp, nothing else depends on them.
//
// How it works: the AEC must know what the speaker plays (far end / "render") to remove it from
// what the microphone records (near end / "capture"). So ONE instance is shared:
//
//     speaker thread : render(pcmSentToSpeaker)            <- clsAudioPlayer
//     mic thread     : capture(pcmFromMic, outCleanPcm)    <- clsAudioRecorder
//
// The two calls may run on different threads at the same time (that is how WebRTC APM is built):
// render() touches only the render state, capture() only the capture state. Everything else
// (init) must happen before the audio devices are started.
//
// WebRTC works in blocks of exactly 10 ms; device chunks have arbitrary sizes, so both calls
// re-cut the data internally.

#include <memory>
#include <string>
#include "AudioTypes.h"

class EchoCanceller
{
public:
    struct Options
    {
        // Estimated time from render() to the speaker output + from the mic to capture().
        // WebRTC refines it by itself, it is only a starting hint. 0 = don't give a hint.
        int  streamDelayMs  = 50;
        bool highPassFilter = true;     // removes DC / rumble from the mic (recommended with AEC)

        // How hard the AEC3 suppressor protects YOUR voice while the speaker plays at the same time
        // (double talk). 0 = webrtc defaults. Higher = your voice is cut less, but a little more echo
        // can pass. Try 1..3 when your voice breaks up (mic very close to the speaker, loud speaker).
        int  nearendProtection = 0;     // 0..3

        bool echoCancel = true;         // false = only the other processing below (render() is then ignored)

        // other webrtc processing on the mic signal (same 10 ms blocks, no extra latency)
        int  noiseSuppression = 0;      // 0 = off, 1 = low, 2 = moderate, 3 = high, 4 = very high
        bool autoGain = false;          // AGC2: evens out the level (adaptive digital gain) + limiter against clipping
    };

    struct Stats
    {
        bool   hasErle = false;
        double erleDb = 0;              // echo return loss enhancement: how much echo is removed (higher = better)
        bool   hasErl = false;
        double erlDb = 0;               // echo return loss: echo attenuation of the room itself
        bool   hasDelay = false;
        int    delayMs = 0;             // delay estimated by AEC3
    };

    EchoCanceller();
    ~EchoCanceller();
    EchoCanceller(const EchoCanceller&) = delete;
    EchoCanceller& operator=(const EchoCanceller&) = delete;

    // captureRate = mic sample rate, renderRate = speaker sample rate (can differ), both >= 8000
    // and a multiple of 100 (10 ms = whole samples). `channels` is the same for both (1 or 2).
    bool init(int captureRate, int renderRate, int channels, const Options &opt, std::string *err = nullptr);
    bool init(int captureRate, int renderRate, int channels, std::string *err = nullptr);   // default Options
    bool isReady() const;

    // far end: the exact interleaved s16 samples that go to the speaker (silence included, so the
    // timeline never has gaps). Call it from the speaker callback AFTER the buffer is filled.
    void render(const int16_t *pcm, uint32_t frames);

    // near end: mic samples in, echo-free samples APPENDED to `out`. Output can lag the input by
    // less than 10 ms (incomplete block) and its size per call varies, the total is the same.
    // If processing fails the unprocessed samples are passed through, audio is never lost.
    void capture(const int16_t *pcm, uint32_t frames, Pcm &out);

    void setStreamDelayMs(int ms);      // any thread
    bool getStats(Stats &st) const;     // any thread (GUI timer); false if not ready

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

#endif // ECHOCANCELLER_H
