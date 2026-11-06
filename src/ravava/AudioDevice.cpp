/*
 * RavAva - real-time voice library (Opus over any datagram transport)
 * Copyright (c) 2026 Mojtaba Kahvand. All rights reserved.
 * Author: Mojtaba Kahvand
 *
 * Proprietary software, NOT free / open source. Use, copying, modification and distribution are forbidden without a
 * written license from the author. See the LICENSE file in the repository root.
 * SPDX-License-Identifier: LicenseRef-RavAva-Proprietary
 */
// the ONLY place that includes miniaudio. miniaudio/ holds the split version (miniaudio.h + miniaudio.c,
// the .c is compiled by VoiceStream.pro). Don't define MINIAUDIO_IMPLEMENTATION here: it would
// duplicate the implementation (link errors) if miniaudio.h is ever swapped for the single-file version.
#include "miniaudio.h"
#if defined(__APPLE__)
#include <TargetConditionals.h>
#if TARGET_OS_IPHONE
#define RAVAVA_IOS 1
#endif
#endif

#include "AudioDevice.h"
#include "AudioTypes.h"

// Android: miniaudio can't list devices there and the phone's mic is used even with a Bluetooth headset connected,
// platform/AndroidAudioRoute.cpp fixes both through AudioManager. RAVAVA_FORCE_ANDROID_ROUTE: syntax check on a PC.
#if defined(__ANDROID__) || defined(RAVAVA_FORCE_ANDROID_ROUTE)
#define RAVAVA_ANDROID 1
#ifdef __ANDROID__
#define RAVAVA_AAUDIO_STREAMS 1     // ma_device::aaudio.pStreamCapture / pStreamPlayback exist (not in the PC syntax check)
#endif
#include "platform/AndroidAudioRoute.h"
#endif
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>
#include <mutex>
#include <cstring>
#include <cstdio>

namespace ravava {

// what miniaudio tells about a device (started / stopped / rerouted...), for the log. Not called from the audio callback.
static void onNotification(const ma_device_notification *n)
{
    static const char *names[] = {"started", "stopped", "rerouted", "interruption began", "interruption ended", "unlocked"};
    const int t = int(n->type);
    RAVAVA_INFO("%s device %s", n->pDevice->type == ma_device_type_capture ? "capture" : "playback",
                (t >= 0 && t < 6) ? names[t] : "notification");
}

// ============================================================ device ids
// ma_device_id is a plain union of bytes: the id string is just its hex dump.
static std::string idToString(const ma_device_id &id)
{
    static const char *hex = "0123456789abcdef";
    std::string s;
    const unsigned char *p = reinterpret_cast<const unsigned char*>(&id);
    for(size_t i = 0; i < sizeof(ma_device_id); ++i){
        s += hex[p[i] >> 4];
        s += hex[p[i] & 15];
    }
    return s;
}

static bool stringToId(const std::string &s, ma_device_id &id)
{
    if(s.size() != sizeof(ma_device_id) * 2)
        return false;
    unsigned char *p = reinterpret_cast<unsigned char*>(&id);
    for(size_t i = 0; i < sizeof(ma_device_id); ++i){
        unsigned v = 0;
        for(int k = 0; k < 2; ++k){
            char ch = s[i * 2 + k];
            unsigned d = (ch >= '0' && ch <= '9') ? unsigned(ch - '0') : (ch >= 'a' && ch <= 'f') ? unsigned(ch - 'a' + 10) : 99u;
            if(d > 15)
                return false;
            v = v * 16 + d;
        }
        p[i] = static_cast<unsigned char>(v);
    }
    return true;
}

// One lock for everything that opens, closes or enumerates sound devices. The backends (ALSA especially, also PulseAudio and
// the Android JNI side) are not safe when a device is being opened / closed on the device worker while the GUI enumerates
// the devices (which happens every time the user opens the device combo box) - that was a rare crash while switching.
// The audio callbacks never take it, so ma_device_uninit() (which waits for the callback) can't deadlock on it.
static std::mutex g_deviceMutex;

#ifdef RAVAVA_IOS
// iOS has ONE audio session for the whole app, and the microphone and the speaker are two devices here. Both must ask for
// the same session: play-and-record (a call), speaker instead of the earpiece, Bluetooth headsets allowed. One shared context
// does that; it lives until the app ends.
static ma_context *deviceContext()
{
    static ma_context ctx;
    static const bool ok = []{
        ma_context_config cc = ma_context_config_init();
        cc.coreaudio.sessionCategory = ma_ios_session_category_play_and_record;
        cc.coreaudio.sessionCategoryOptions = ma_ios_session_category_option_default_to_speaker | ma_ios_session_category_option_allow_bluetooth;
        return ma_context_init(nullptr, 0, &cc, &ctx) == MA_SUCCESS;
    }();
    return ok ? &ctx : nullptr;                 // nullptr = miniaudio makes its own default context
}
#else
static ma_context *deviceContext() { return nullptr; }
#endif

static std::vector<AudioDeviceInfo> listDevices(bool capture, bool wait = false)
{
    // the GUI must not freeze behind a Bluetooth headset that is being opened (seconds): then the last list is shown again
    static std::vector<AudioDeviceInfo> cache[2];
    static std::mutex cacheMutex;
    std::unique_lock<std::mutex> lk(g_deviceMutex, std::defer_lock);
    if(wait)
        lk.lock();                  // internal callers (a device is about to be opened) need the real, current list
    else
        lk.try_lock();
    if(!lk.owns_lock()){
        std::lock_guard<std::mutex> ck(cacheMutex);
        return cache[capture ? 1 : 0];
    }
    std::vector<AudioDeviceInfo> out;
    ma_context ctx;
    if(ma_context_init(nullptr, 0, nullptr, &ctx) != MA_SUCCESS)
        return out;
    ma_device_info *playback = nullptr, *captureInfos = nullptr;
    ma_uint32 nPlayback = 0, nCapture = 0;
    if(ma_context_get_devices(&ctx, &playback, &nPlayback, &captureInfos, &nCapture) == MA_SUCCESS){
        ma_device_info *list = capture ? captureInfos : playback;
        ma_uint32 n = capture ? nCapture : nPlayback;
        for(ma_uint32 i = 0; i < n; ++i){
            AudioDeviceInfo d;
            d.name = list[i].name;
            d.id = idToString(list[i].id);
            d.isDefault = list[i].isDefault != 0;
            out.push_back(std::move(d));
        }
    }
    ma_context_uninit(&ctx);
    {
        std::lock_guard<std::mutex> ck(cacheMutex);
        cache[capture ? 1 : 0] = out;
    }
    return out;
}

#ifndef RAVAVA_ANDROID
// PulseAudio / PipeWire list "Monitor of <speaker>" as capture devices: they record what the speaker plays, not a microphone.
// With no microphone connected the default capture device is often such a monitor (or nothing), and recording from it gives
// static / feedback instead of an error.
static bool isMonitorName(const std::string &n)
{
    std::string l;
    for(char c : n) l += char(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    return l.compare(0, 10, "monitor of") == 0 || l.find(".monitor") != std::string::npos || l.find("monitor of ") != std::string::npos;
}
#endif

#ifdef RAVAVA_ANDROID
// AAudio / OpenSL ES only know "the default device": the list comes from AudioManager (empty = fall back to miniaudio)
static std::vector<AudioDeviceInfo> listAny(bool capture)
{
    std::vector<AudioDeviceInfo> l = android::ListDevices(capture);
    return l.empty() ? listDevices(capture) : l;
}
std::vector<AudioDeviceInfo> ListInputDevices()  { return listAny(true); }
std::vector<AudioDeviceInfo> ListOutputDevices() { return listAny(false); }
#else
static std::vector<AudioDeviceInfo> realMicrophones(bool wait)
{
    std::vector<AudioDeviceInfo> all = listDevices(true, wait), out;
    for(AudioDeviceInfo &d : all)
        if(!isMonitorName(d.name))
            out.push_back(std::move(d));
    return out;
}
std::vector<AudioDeviceInfo> ListInputDevices() { return realMicrophones(false); }
std::vector<AudioDeviceInfo> ListOutputDevices() { return listDevices(false); }
#endif

// =============================================================== AudioInput
struct AudioInput::Impl
{
    ma_device dev;
    bool opened = false;
    std::atomic<bool> running{false};
    bool voiceRoute = false;        // Android: this stream holds the Bluetooth handsfree route
    mutable std::mutex nameMutex;   // activeName is written by the device-switch worker, read by the GUI
    std::string activeName;         // name of the device that was really opened (not Android: see ActiveDevice())
    std::atomic<int> wantedId{0};   // Android: AudioDeviceInfo id the stream was opened on (0 = system default / unknown)
    std::atomic<int> movedPolls{0}; // consecutive DeviceMoved() calls that saw another device
    std::atomic<int> openEpoch{0};  // Android: android::ModeEpoch() when the stream was opened (the speaker choice switched call <-> media since = re-open)
    std::atomic<int> activeId{-1};  // Android: AudioDeviceInfo id the stream is REALLY on (refreshed by the audio callback)
    unsigned cbCount = 0;           // audio thread only
    std::atomic<unsigned> cbTotal{0};   // audio callbacks since Start (Android: a stream the system silently stopped has none)
    unsigned lastCbTotal = 0;           // DeviceMoved() (GUI thread)
    int stalledPolls = 0;               // consecutive DeviceMoved() calls that saw no new callback
    std::string deviceId;
    Callback cb;

    static void onData(ma_device *pDevice, void *, const void *pInput, ma_uint32 frameCount)
    {
        Impl *s = static_cast<Impl*>(pDevice->pUserData);
#ifdef RAVAVA_AAUDIO_STREAMS
        s->cbTotal.fetch_add(1, std::memory_order_relaxed);
        if((s->cbCount++ & 63) == 0)    // about every 0.6 s: which device is the stream really on (the system may re-route it)
            s->activeId.store(android::StreamDeviceId(pDevice->aaudio.pStreamCapture), std::memory_order_relaxed);
#endif
        if(s->cb && pInput)
            s->cb(static_cast<const int16_t*>(pInput), frameCount);
    }
};

void SetOutputRouteHint(const std::string &outputDeviceId)
{
#ifdef RAVAVA_ANDROID
    android::SetOutputChoice(outputDeviceId);
#else
    (void)outputDeviceId;
#endif
}

void SetAndroidCallMode(bool on)
{
#ifdef RAVAVA_ANDROID
    android::SetCallMode(on);
#else
    (void)on;
#endif
}

AudioInput::AudioInput() : d(new Impl) {

}

AudioInput::~AudioInput() {
    Stop();
}

void AudioInput::SetDevice(const std::string &deviceId) { d->deviceId = deviceId; }

bool AudioInput::Start(int sampleRate, int channels, int periodMs, Callback cb, std::string *err)
{
    const auto t0 = std::chrono::steady_clock::now();
    auto sinceMs = [&t0]{ return int(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count()); };
    int prepareMs = 0;
    Stop();
    d->cb = std::move(cb);

    ma_device_config cfg = ma_device_config_init(ma_device_type_capture);
    cfg.capture.format   = ma_format_s16;
    cfg.capture.channels = ma_uint32(channels);
    cfg.sampleRate       = ma_uint32(sampleRate);
    cfg.periodSizeInMilliseconds = ma_uint32(periodMs);
    cfg.dataCallback     = Impl::onData;
    cfg.notificationCallback = onNotification;
    cfg.pUserData        = d.get();

    ma_device_id chosen;
    std::memset(&chosen, 0, sizeof(chosen));
    ma_result r = MA_ERROR;
    bool haveChosen = false;
#ifdef RAVAVA_ANDROID
    {
        int32_t aid = 0; bool voice = false, held = false;
        d->openEpoch = android::ModeEpoch();            // read BEFORE preparing: a switch in between only costs one extra re-open
        android::PrepareStream(d->deviceId, true, &aid, &voice, &held);
        prepareMs = sinceMs();
        d->voiceRoute = held;
        if(voice){
            cfg.aaudio.inputPreset = ma_aaudio_input_preset_voice_communication;    // lets Android use the headset's mic
            cfg.aaudio.usage = ma_aaudio_usage_voice_communication;
        }
        if(aid != 0){ chosen.aaudio = aid; haveChosen = true; }
        d->wantedId = aid;
        d->movedPolls = 0;
    }
#else
    haveChosen = !d->deviceId.empty() && stringToId(d->deviceId, chosen);
    if(!haveChosen){
        // "system default": make sure there is a real microphone, and never record a speaker monitor
        const std::vector<AudioDeviceInfo> mics = realMicrophones(true);
        if(mics.empty()){
            if(err) *err = "no microphone found (connect one and start again)";
            return false;
        }
        bool defaultIsReal = false;
        for(const AudioDeviceInfo &m : mics)
            if(m.isDefault) defaultIsReal = true;
        if(!defaultIsReal)
            haveChosen = stringToId(mics.front().id, chosen);       // the default is a monitor: take the first real microphone
    }
#endif
    std::unique_lock<std::mutex> devLock(g_deviceMutex);
    if(haveChosen){
        cfg.capture.pDeviceID = &chosen;
        r = ma_device_init(deviceContext(), &cfg, &d->dev);
        if(r != MA_SUCCESS)
            RAVAVA_WARN("capture: chosen device can't be opened (%s), using the default one", ma_result_description(r));
    }
    if(r != MA_SUCCESS){
        cfg.capture.pDeviceID = nullptr;
        r = ma_device_init(deviceContext(), &cfg, &d->dev);
    }
    devLock.unlock();
    if(r != MA_SUCCESS){
        if(err) *err = std::string("capture device init failed: ") + ma_result_description(r);
#ifdef RAVAVA_ANDROID
    if(d->voiceRoute){
        android::ReleaseStream(true);
        d->voiceRoute = false;
    }
#endif
        return false;
    }
    d->opened = true;
    d->activeId = -1;
    d->cbCount = 0;
    d->cbTotal = 0; d->lastCbTotal = 0; d->stalledPolls = 0;
    {
        char nm[256] = {0};
        std::lock_guard<std::mutex> lk(d->nameMutex);
        d->activeName = ma_device_get_name(&d->dev, ma_device_type_capture, nm, sizeof(nm), nullptr) == MA_SUCCESS ? nm : "";
    }

    r = ma_device_start(&d->dev);
    if(r != MA_SUCCESS){
        if(err) *err = std::string("capture device start failed: ") + ma_result_description(r);
        Stop();
        return false;
    }
    RAVAVA_INFO("capture: opened in %d ms (route setup %d ms)", sinceMs(), prepareMs);
    d->running = true;
    return true;
}

void AudioInput::Stop()
{
    if(d->opened){
        {
            std::lock_guard<std::mutex> lk(g_deviceMutex);
            ma_device_uninit(&d->dev);  // stops the device and waits for the callback
        }
        d->opened = false;
    }
    d->running = false;
#ifdef RAVAVA_ANDROID
    if(d->voiceRoute){
        android::ReleaseStream(true);
        d->voiceRoute = false;
    }
#endif
}

std::string AudioInput::ActiveDevice() const
{
#ifdef RAVAVA_ANDROID
    if(!d->running)
        return std::string();
    const int id = d->activeId.load(std::memory_order_relaxed);
    std::string name = id > 0 ? android::DescribeDeviceId(id, true) : std::string();
    return name.empty() ? android::DescribeActiveDevice(true) : name;
#else
    std::lock_guard<std::mutex> lk(d->nameMutex);
    return d->running ? d->activeName : std::string();
#endif
}

bool AudioInput::DeviceMoved()
{
#ifdef RAVAVA_ANDROID
    if(d->voiceRoute)
        android::ReassertRoute();      // the system resets the Bluetooth route on calls / USB: put it back
    if(!d->running)
        return false;
    if(d->openEpoch.load() != android::ModeEpoch()){
        RAVAVA_INFO("capture: the speaker route changed (call <-> Bluetooth media), re-opening the microphone for it");
        return true;
    }
#ifdef RAVAVA_AAUDIO_STREAMS
    // The system can stop a stream without telling (a phone call, a USB / Bluetooth change): miniaudio then still says
    // "started" and nothing is heard until the user stops and starts again. A running stream calls back every few
    // milliseconds, so no callback during 3 polls (about 3 s) = stalled: report it, the caller re-opens the device.
    const unsigned total = d->cbTotal.load(std::memory_order_relaxed);
    if(total == d->lastCbTotal){
        if(++d->stalledPolls >= 3){
            d->stalledPolls = 0;
            RAVAVA_WARN("capture: no audio callback for 3 s, the system stopped the stream - %s", android::DescribeRouteState().c_str());
            return true;
        }
    }else{
        d->lastCbTotal = total;
        d->stalledPolls = 0;
    }
#endif
    const int want = d->wantedId.load(), act = d->activeId.load();
    if(want <= 0 || act <= 0 || act == want){
        d->movedPolls = 0;
        return false;
    }
    if(++d->movedPolls < 2)
        return false;
    d->movedPolls = 0;
    RAVAVA_WARN("capture: the system moved the stream from device #%d to #%d (%s) - %s", want, act,
                android::DescribeDeviceId(act, true).c_str(), android::DescribeRouteState().c_str());
    return true;
#else
    return false;
#endif
}

bool AudioInput::IsRunning() const {
    return d->running;
}

// ============================================================== AudioOutput
struct AudioOutput::Impl
{
    ma_device dev;
    bool opened = false;
    std::atomic<bool> running{false};
    int channels = 1;
    bool voiceRoute = false;        // Android: this stream holds the Bluetooth handsfree route
    mutable std::mutex nameMutex;   // activeName is written by the device-switch worker, read by the GUI
    std::string activeName;         // name of the device that was really opened (not Android: see ActiveDevice())
    std::atomic<int> wantedId{0};   // Android: AudioDeviceInfo id the stream was opened on (0 = system default / unknown)
    std::atomic<int> movedPolls{0}; // consecutive DeviceMoved() calls that saw another device
    std::atomic<int> activeId{-1};  // Android: AudioDeviceInfo id the stream is REALLY on (refreshed by the audio callback)
    unsigned cbCount = 0;           // audio thread only
    std::atomic<unsigned> cbTotal{0};   // audio callbacks since Start (Android: a stream the system silently stopped has none)
    unsigned lastCbTotal = 0;           // DeviceMoved() (GUI thread)
    int stalledPolls = 0;               // consecutive DeviceMoved() calls that saw no new callback
    std::string deviceId;
    Callback cb;

    static void onData(ma_device *pDevice, void *pOutput, const void *, ma_uint32 frameCount)
    {
        Impl *s = static_cast<Impl*>(pDevice->pUserData);
#ifdef RAVAVA_AAUDIO_STREAMS
        s->cbTotal.fetch_add(1, std::memory_order_relaxed);
        if((s->cbCount++ & 63) == 0)
            s->activeId.store(android::StreamDeviceId(pDevice->aaudio.pStreamPlayback), std::memory_order_relaxed);
#endif
        if(s->cb)
            s->cb(static_cast<int16_t*>(pOutput), frameCount);
        else
            std::memset(pOutput, 0, size_t(frameCount) * s->channels * sizeof(int16_t));
    }
};

AudioOutput::AudioOutput() : d(new Impl) {

}

AudioOutput::~AudioOutput() {
    Stop();
}

void AudioOutput::SetDevice(const std::string &deviceId) { d->deviceId = deviceId; }

bool AudioOutput::Start(int sampleRate, int channels, int periodMs, Callback cb, std::string *err)
{
    const auto t0 = std::chrono::steady_clock::now();
    auto sinceMs = [&t0]{ return int(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count()); };
    int prepareMs = 0;
    Stop();
    d->cb = std::move(cb);
    d->channels = channels;

    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format   = ma_format_s16;
    cfg.playback.channels = ma_uint32(channels);
    cfg.sampleRate        = ma_uint32(sampleRate);
    cfg.periodSizeInMilliseconds = ma_uint32(periodMs);
    cfg.dataCallback      = Impl::onData;
    cfg.notificationCallback = onNotification;
    cfg.pUserData         = d.get();

    ma_device_id chosen;
    std::memset(&chosen, 0, sizeof(chosen));
    ma_result r = MA_ERROR;
    bool haveChosen = false;
#ifdef RAVAVA_ANDROID
    {
        int32_t aid = 0; bool voice = false, held = false;
        android::PrepareStream(d->deviceId, false, &aid, &voice, &held);
        prepareMs = sinceMs();
        d->voiceRoute = held;
        if(voice)
            cfg.aaudio.usage = ma_aaudio_usage_voice_communication;
        if(aid != 0){ chosen.aaudio = aid; haveChosen = true; }
        d->wantedId = aid;
        d->movedPolls = 0;
    }
#else
    haveChosen = !d->deviceId.empty() && stringToId(d->deviceId, chosen);
#endif
    std::unique_lock<std::mutex> devLock(g_deviceMutex);
    if(haveChosen){
        cfg.playback.pDeviceID = &chosen;
        r = ma_device_init(deviceContext(), &cfg, &d->dev);
        if(r != MA_SUCCESS)
            RAVAVA_WARN("playback: chosen device can't be opened (%s), using the default one", ma_result_description(r));
    }
    if(r != MA_SUCCESS){
        cfg.playback.pDeviceID = nullptr;
        r = ma_device_init(deviceContext(), &cfg, &d->dev);
    }
    devLock.unlock();
    if(r != MA_SUCCESS){
        if(err) *err = std::string("playback device init failed: ") + ma_result_description(r);
#ifdef RAVAVA_ANDROID
    if(d->voiceRoute){
        android::ReleaseStream(true);
        d->voiceRoute = false;
    }
#endif
        return false;
    }
    d->opened = true;
    d->activeId = -1;
    d->cbCount = 0;
    d->cbTotal = 0; d->lastCbTotal = 0; d->stalledPolls = 0;
    {
        char nm[256] = {0};
        std::lock_guard<std::mutex> lk(d->nameMutex);
        d->activeName = ma_device_get_name(&d->dev, ma_device_type_playback, nm, sizeof(nm), nullptr) == MA_SUCCESS ? nm : "";
    }

    r = ma_device_start(&d->dev);
    if(r != MA_SUCCESS){
        if(err) *err = std::string("playback device start failed: ") + ma_result_description(r);
        Stop();
        return false;
    }
    RAVAVA_INFO("playback: opened in %d ms (route setup %d ms)", sinceMs(), prepareMs);
    d->running = true;
    return true;
}

void AudioOutput::Stop()
{
    if(d->opened){
        {
            std::lock_guard<std::mutex> lk(g_deviceMutex);
            ma_device_uninit(&d->dev);
        }
        d->opened = false;
    }
    d->running = false;
#ifdef RAVAVA_ANDROID
    if(d->voiceRoute){
        android::ReleaseStream(true);
        d->voiceRoute = false;
    }
#endif
}

std::string AudioOutput::ActiveDevice() const
{
#ifdef RAVAVA_ANDROID
    if(!d->running)
        return std::string();
    const int id = d->activeId.load(std::memory_order_relaxed);
    std::string name = id > 0 ? android::DescribeDeviceId(id, false) : std::string();
    return name.empty() ? android::DescribeActiveDevice(false) : name;
#else
    std::lock_guard<std::mutex> lk(d->nameMutex);
    return d->running ? d->activeName : std::string();
#endif
}

bool AudioOutput::DeviceMoved()
{
#ifdef RAVAVA_ANDROID
    if(d->voiceRoute)
        android::ReassertRoute();      // the system resets the Bluetooth route on calls / USB: put it back
    if(!d->running)
        return false;
#ifdef RAVAVA_AAUDIO_STREAMS
    // The system can stop a stream without telling (a phone call, a USB / Bluetooth change): miniaudio then still says
    // "started" and nothing is heard until the user stops and starts again. A running stream calls back every few
    // milliseconds, so no callback during 3 polls (about 3 s) = stalled: report it, the caller re-opens the device.
    const unsigned total = d->cbTotal.load(std::memory_order_relaxed);
    if(total == d->lastCbTotal){
        if(++d->stalledPolls >= 3){
            d->stalledPolls = 0;
            RAVAVA_WARN("playback: no audio callback for 3 s, the system stopped the stream - %s", android::DescribeRouteState().c_str());
            return true;
        }
    }else{
        d->lastCbTotal = total;
        d->stalledPolls = 0;
    }
#endif
    const int want = d->wantedId.load(), act = d->activeId.load();
    if(want <= 0 || act <= 0 || act == want){
        d->movedPolls = 0;
        return false;
    }
    if(++d->movedPolls < 2)
        return false;
    d->movedPolls = 0;
    RAVAVA_WARN("playback: the system moved the stream from device #%d to #%d (%s) - %s", want, act,
                android::DescribeDeviceId(act, false).c_str(), android::DescribeRouteState().c_str());
    return true;
#else
    return false;
#endif
}

bool AudioOutput::IsRunning() const { return d->running; }

// ============================================================ DecodeAudioFile
Pcm DecodeAudioFile(const std::string &path, int sampleRate, int channels, std::string *err)
{
    ma_decoder_config cfg = ma_decoder_config_init(ma_format_s16, ma_uint32(channels), ma_uint32(sampleRate));

    ma_uint64 frameCount = 0;
    void *pFrames = nullptr;
    ma_result r = ma_decode_file(path.c_str(), &cfg, &frameCount, &pFrames);
    if(r != MA_SUCCESS || !pFrames){
        if(err) *err = std::string("cannot decode '") + path + "': " + ma_result_description(r);
        return Pcm();
    }

    const int16_t *p = static_cast<const int16_t*>(pFrames);
    Pcm out(p, p + size_t(frameCount) * size_t(channels));
    ma_free(pFrames, nullptr);
    return out;
}

// ============================================================ AudioFileReader
struct AudioFileReader::Impl
{
    std::vector<int16_t> ring;              // capacity is a power of two (samples)
    size_t mask = 0;
    std::atomic<uint64_t> wr{0}, rd{0};     // total samples written / read; free-running, index = value & mask
    std::atomic<bool> stop{false};
    std::atomic<bool> eof{false};           // the decoder reached the end of the file
    std::thread th;
    mutable std::mutex errMutex;
    std::string err;

    void setError(const std::string &e) { std::lock_guard<std::mutex> lk(errMutex); err = e; }

    void run(std::string path, int rate, int channels)
    {
        ma_decoder_config cfg = ma_decoder_config_init(ma_format_s16, ma_uint32(channels), ma_uint32(rate));
        ma_decoder dec;
        ma_result r = ma_decoder_init_file(path.c_str(), &cfg, &dec);
        if(r != MA_SUCCESS){
            std::string e = std::string("cannot open '") + path + "': " + ma_result_description(r);
#ifdef RAVAVA_ANDROID
            e += " (Android: the app needs the permission to read audio files, and content:// paths cannot be used)";
#endif
            setError(e);
            RAVAVA_WARN("music: %s", e.c_str());
            eof = true;
            return;
        }
        const size_t cap = ring.size();
        const size_t chunkFrames = 2048;
        std::vector<int16_t> tmp(chunkFrames * size_t(channels));
        while(!stop.load(std::memory_order_relaxed)){
            const uint64_t w = wr.load(std::memory_order_relaxed);
            const uint64_t used = w - rd.load(std::memory_order_acquire);
            if(cap - used < tmp.size()){
                std::this_thread::sleep_for(std::chrono::milliseconds(5));      // ring is full: wait for the audio thread
                continue;
            }
            ma_uint64 got = 0;
            r = ma_decoder_read_pcm_frames(&dec, tmp.data(), chunkFrames, &got);
            const size_t n = size_t(got) * size_t(channels);
            for(size_t i = 0; i < n; ++i)
                ring[size_t((w + i) & mask)] = tmp[i];
            wr.store(w + n, std::memory_order_release);
            if(r != MA_SUCCESS || got == 0){
                if(r != MA_SUCCESS && r != MA_AT_END){
                    setError(std::string("decoding '") + path + "' failed: " + ma_result_description(r));
                    RAVAVA_WARN("music: %s", err.c_str());
                }
                break;
            }
        }
        ma_decoder_uninit(&dec);
        eof = true;
    }
};

AudioFileReader::AudioFileReader() : d(new Impl) {}
AudioFileReader::~AudioFileReader() { Close(); }

void AudioFileReader::Open(const std::string &path, int sampleRate, int channels)
{
    Close();
    if(channels < 1) channels = 1;
    size_t cap = 1024;
    while(cap < size_t(sampleRate) * size_t(channels) * 2)      // about 2 s
        cap <<= 1;
    d->ring.assign(cap, 0);
    d->mask = cap - 1;
    d->wr = 0; d->rd = 0;
    d->stop = false; d->eof = false;
    d->setError(std::string());
    d->th = std::thread([this, path, sampleRate, channels]{ d->run(path, sampleRate, channels); });
}

void AudioFileReader::Close()
{
    d->stop = true;
    if(d->th.joinable())
        d->th.join();
}

size_t AudioFileReader::Read(int16_t *dst, size_t samples)
{
    const uint64_t r = d->rd.load(std::memory_order_relaxed);
    const uint64_t avail = d->wr.load(std::memory_order_acquire) - r;
    const size_t n = size_t(avail < samples ? avail : samples);
    for(size_t i = 0; i < n; ++i)
        dst[i] = d->ring[size_t((r + i) & d->mask)];
    d->rd.store(r + n, std::memory_order_release);
    return n;
}

bool AudioFileReader::IsFinished() const
{
    return d->eof.load(std::memory_order_acquire) && d->rd.load() == d->wr.load();
}

std::string AudioFileReader::Error() const
{
    std::lock_guard<std::mutex> lk(d->errMutex);
    return d->err;
}

} // namespace ravava
