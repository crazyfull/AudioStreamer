// the ONLY place that includes miniaudio. miniaudio/ holds the split version (miniaudio.h + miniaudio.c,
// the .c is compiled by VoiceStream.pro). Don't define MINIAUDIO_IMPLEMENTATION here: it would
// duplicate the implementation (link errors) if miniaudio.h is ever swapped for the single-file version.
#include "miniaudio.h"

#include "AudioDevice.h"
#include "AudioTypes.h"
#include <cstring>
#include <cstdio>

namespace ravava {

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

static std::vector<AudioDeviceInfo> listDevices(bool capture)
{
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
    return out;
}

std::vector<AudioDeviceInfo> ListInputDevices()  { return listDevices(true); }
std::vector<AudioDeviceInfo> ListOutputDevices() { return listDevices(false); }

// =============================================================== AudioInput
struct AudioInput::Impl
{
    ma_device dev;
    bool opened = false;
    bool running = false;
    std::string deviceId;
    Callback cb;

    static void onData(ma_device *pDevice, void *, const void *pInput, ma_uint32 frameCount)
    {
        Impl *s = static_cast<Impl*>(pDevice->pUserData);
        if(s->cb && pInput)
            s->cb(static_cast<const int16_t*>(pInput), frameCount);
    }
};

AudioInput::AudioInput() : d(new Impl) {

}

AudioInput::~AudioInput() {
    Stop();
}

void AudioInput::SetDevice(const std::string &deviceId) { d->deviceId = deviceId; }

bool AudioInput::Start(int sampleRate, int channels, int periodMs, Callback cb, std::string *err)
{
    Stop();
    d->cb = std::move(cb);

    ma_device_config cfg = ma_device_config_init(ma_device_type_capture);
    cfg.capture.format   = ma_format_s16;
    cfg.capture.channels = ma_uint32(channels);
    cfg.sampleRate       = ma_uint32(sampleRate);
    cfg.periodSizeInMilliseconds = ma_uint32(periodMs);
    cfg.dataCallback     = Impl::onData;
    cfg.pUserData        = d.get();

    ma_device_id chosen;
    ma_result r = MA_ERROR;
    if(!d->deviceId.empty() && stringToId(d->deviceId, chosen)){
        cfg.capture.pDeviceID = &chosen;
        r = ma_device_init(nullptr, &cfg, &d->dev);
        if(r != MA_SUCCESS)
            RAVAVA_WARN("capture: chosen device can't be opened (%s), using the default one", ma_result_description(r));
    }
    if(r != MA_SUCCESS){
        cfg.capture.pDeviceID = nullptr;
        r = ma_device_init(nullptr, &cfg, &d->dev);
    }
    if(r != MA_SUCCESS){
        if(err) *err = std::string("capture device init failed: ") + ma_result_description(r);
        return false;
    }
    d->opened = true;

    r = ma_device_start(&d->dev);
    if(r != MA_SUCCESS){
        if(err) *err = std::string("capture device start failed: ") + ma_result_description(r);
        Stop();
        return false;
    }
    d->running = true;
    return true;
}

void AudioInput::Stop()
{
    if(d->opened){
        ma_device_uninit(&d->dev);      // stops the device and waits for the callback
        d->opened = false;
    }
    d->running = false;
}

bool AudioInput::IsRunning() const {
    return d->running;
}

// ============================================================== AudioOutput
struct AudioOutput::Impl
{
    ma_device dev;
    bool opened = false;
    bool running = false;
    int channels = 1;
    std::string deviceId;
    Callback cb;

    static void onData(ma_device *pDevice, void *pOutput, const void *, ma_uint32 frameCount)
    {
        Impl *s = static_cast<Impl*>(pDevice->pUserData);
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
    Stop();
    d->cb = std::move(cb);
    d->channels = channels;

    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format   = ma_format_s16;
    cfg.playback.channels = ma_uint32(channels);
    cfg.sampleRate        = ma_uint32(sampleRate);
    cfg.periodSizeInMilliseconds = ma_uint32(periodMs);
    cfg.dataCallback      = Impl::onData;
    cfg.pUserData         = d.get();

    ma_device_id chosen;
    ma_result r = MA_ERROR;
    if(!d->deviceId.empty() && stringToId(d->deviceId, chosen)){
        cfg.playback.pDeviceID = &chosen;
        r = ma_device_init(nullptr, &cfg, &d->dev);
        if(r != MA_SUCCESS)
            RAVAVA_WARN("playback: chosen device can't be opened (%s), using the default one", ma_result_description(r));
    }
    if(r != MA_SUCCESS){
        cfg.playback.pDeviceID = nullptr;
        r = ma_device_init(nullptr, &cfg, &d->dev);
    }
    if(r != MA_SUCCESS){
        if(err) *err = std::string("playback device init failed: ") + ma_result_description(r);
        return false;
    }
    d->opened = true;

    r = ma_device_start(&d->dev);
    if(r != MA_SUCCESS){
        if(err) *err = std::string("playback device start failed: ") + ma_result_description(r);
        Stop();
        return false;
    }
    d->running = true;
    return true;
}

void AudioOutput::Stop()
{
    if(d->opened){
        ma_device_uninit(&d->dev);
        d->opened = false;
    }
    d->running = false;
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

} // namespace ravava
