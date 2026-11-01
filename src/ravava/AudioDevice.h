/*
 * RavAva - real-time voice library (Opus over any datagram transport)
 * Copyright (c) 2026 Mojtaba Kahvand. All rights reserved.
 * Author: Mojtaba Kahvand
 *
 * Proprietary software, NOT free / open source. Use, copying, modification and distribution are forbidden without a
 * written license from the author. See the LICENSE file in the repository root.
 * SPDX-License-Identifier: LicenseRef-RavAva-Proprietary
 */
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

// ---- music file -> PCM, streaming -----------------------------------------------------------------------------------
// Decodes a file (wav / mp3 / flac) on a background thread into a ring buffer of about 2 seconds, converted to the requested
// rate / channels. Nothing is loaded into memory as a whole and nothing blocks: Open() returns at once, the audio thread
// only copies ready samples out of the ring. (DecodeAudioFile() above decodes the whole file in the calling thread: a long
// mp3 means seconds of work and tens of megabytes.)
class AudioFileReader
{
public:
    AudioFileReader();
    ~AudioFileReader();
    AudioFileReader(const AudioFileReader&) = delete;
    AudioFileReader& operator=(const AudioFileReader&) = delete;

    // Starts decoding from the beginning (a running decode is stopped first). Returns at once; the file is opened by the
    // background thread, a failure shows up in Error().
    void Open(const std::string &path, int sampleRate, int channels);
    void Close();                               // stops the thread, waits for it
    // Audio thread: copies up to `samples` interleaved s16 samples into dst and returns how many were ready (fewer = still
    // buffering, or the end of the file). Lock-free, no allocation.
    size_t Read(int16_t *dst, size_t samples);
    bool IsFinished() const;                    // end of the file reached and everything was read
    std::string Error() const;                  // "" = fine so far; why the file could not be opened / decoded (any thread)

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace ravava

#endif // RAVAVA_AUDIODEVICE_H
