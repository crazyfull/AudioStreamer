#ifndef RAVAVA_AUDIODEVICE_H
#define RAVAVA_AUDIODEVICE_H

// Thin Qt-free wrappers around miniaudio (https://github.com/mackron/miniaudio).
// miniaudio.h is only included by AudioDevice.cpp, so nothing else depends on it.

#include <functional>
#include <memory>
#include <string>
#include "AudioTypes.h"

namespace ravava {

// ---- microphone -----------------------------------------------------------------
// The callback runs on the real-time audio thread: don't block, don't wait on the GUI.
// It receives chunks of ~periodMs (the size can vary); `frames` = samples per channel.
class AudioInput
{
public:
    using Callback = std::function<void(const int16_t *pcm, uint32_t frames)>;

    AudioInput();
    ~AudioInput();
    AudioInput(const AudioInput&) = delete;
    AudioInput& operator=(const AudioInput&) = delete;

    bool Start(int sampleRate, int channels, int periodMs, Callback cb, std::string *err = nullptr);
    void Stop();            // blocks until the callback is no longer running
    bool IsRunning() const;

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

// ---- speaker --------------------------------------------------------------------
// The callback must fill exactly `frames` frames (interleaved s16). miniaudio converts
// rate / channel count to whatever the real device needs.
class AudioOutput
{
public:
    using Callback = std::function<void(int16_t *out, uint32_t frames)>;

    AudioOutput();
    ~AudioOutput();
    AudioOutput(const AudioOutput&) = delete;
    AudioOutput& operator=(const AudioOutput&) = delete;

    bool Start(int sampleRate, int channels, int periodMs, Callback cb, std::string *err = nullptr);
    void Stop();            // blocks until the callback is no longer running
    bool IsRunning() const;

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

// ---- music file -> PCM ---------------------------------------------------------
// Decodes a whole file (wav / mp3 / flac, + ogg if stb_vorbis is added) and converts it to
// the requested rate/channels. Returns interleaved s16 samples; empty on failure.
Pcm DecodeAudioFile(const std::string &path, int sampleRate, int channels, std::string *err = nullptr);

} // namespace ravava

#endif // RAVAVA_AUDIODEVICE_H
