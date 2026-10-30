#ifndef RAVAVA_AUDIODEVICE_H
#define RAVAVA_AUDIODEVICE_H

// Thin Qt-free wrappers around miniaudio (https://github.com/mackron/miniaudio).
// miniaudio.h is only included by AudioDevice.cpp, so nothing else depends on it.

#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "AudioTypes.h"

namespace ravava {

// ---- device list ----------------------------------------------------------------
// `id` is an opaque string that identifies the device (stable across runs, unlike the list position).
// Empty id = the system default device. Pass it to AudioInput/AudioOutput::SetDevice().
struct AudioDeviceInfo
{
    std::string name;       // UTF-8, for the UI
    std::string id;         // opaque, never empty for a listed device
    bool isDefault = false;
};
std::vector<AudioDeviceInfo> ListInputDevices();    // microphones
std::vector<AudioDeviceInfo> ListOutputDevices();   // speakers / headphones

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

    // which device the NEXT Start() opens. "" = system default. If the chosen device can't be opened (unplugged...),
    // Start() falls back to the default one and tells so in *err (and RAVAVA_WARN) only when even that fails.
    void SetDevice(const std::string &deviceId);
    bool Start(int sampleRate, int channels, int periodMs, Callback cb, std::string *err = nullptr);
    void Stop();            // blocks until the callback is no longer running
    bool IsRunning() const;
    // name of the device that is REALLY in use ("" = unknown / not running). On a PC the one miniaudio opened; on Android
    // what AudioManager reports (it can differ from the choice, e.g. when the phone's microphone is used although a
    // Bluetooth headset was selected). May call into Java: GUI thread, not the audio thread.
    std::string ActiveDevice() const;
    // Android: the system moved the stream to another device than the one that was asked for (a notification sound, a
    // Bluetooth connection...) and it stayed there for two calls in a row. Call it about once a second from the GUI
    // thread; the owner then re-opens the chosen device. Always false elsewhere. Logs the route state when it is true.
    bool DeviceMoved();

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

    // which device the NEXT Start() opens. "" = system default. If the chosen device can't be opened (unplugged...),
    // Start() falls back to the default one and tells so in *err (and RAVAVA_WARN) only when even that fails.
    void SetDevice(const std::string &deviceId);
    bool Start(int sampleRate, int channels, int periodMs, Callback cb, std::string *err = nullptr);
    void Stop();            // blocks until the callback is no longer running
    bool IsRunning() const;
    // name of the device that is REALLY in use ("" = unknown / not running). On a PC the one miniaudio opened; on Android
    // what AudioManager reports (it can differ from the choice, e.g. when the phone's microphone is used although a
    // Bluetooth headset was selected). May call into Java: GUI thread, not the audio thread.
    std::string ActiveDevice() const;
    // Android: the system moved the stream to another device than the one that was asked for (a notification sound, a
    // Bluetooth connection...) and it stayed there for two calls in a row. Call it about once a second from the GUI
    // thread; the owner then re-opens the chosen device. Always false elsewhere. Logs the route state when it is true.
    bool DeviceMoved();

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
