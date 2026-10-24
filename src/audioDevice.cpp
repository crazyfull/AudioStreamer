// the ONLY place that includes miniaudio. miniaudio/ holds the split version (miniaudio.h + miniaudio.c,
// the .c is compiled by VoiceStream.pro). Don't define MINIAUDIO_IMPLEMENTATION here: it would
// duplicate the implementation (link errors) if miniaudio.h is ever swapped for the single-file version.
#include "miniaudio.h"

#include "audioDevice.h"
#include <cstring>

// =============================================================== AudioInput
struct AudioInput::Impl
{
    ma_device dev;
    bool opened = false;
    bool running = false;
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
    stop();
}

bool AudioInput::start(int sampleRate, int channels, int periodMs, Callback cb, std::string *err)
{
    stop();
    d->cb = std::move(cb);

    ma_device_config cfg = ma_device_config_init(ma_device_type_capture);
    cfg.capture.format   = ma_format_s16;
    cfg.capture.channels = ma_uint32(channels);
    cfg.sampleRate       = ma_uint32(sampleRate);
    cfg.periodSizeInMilliseconds = ma_uint32(periodMs);
    cfg.dataCallback     = Impl::onData;
    cfg.pUserData        = d.get();

    ma_result r = ma_device_init(nullptr, &cfg, &d->dev);
    if(r != MA_SUCCESS){
        if(err) *err = std::string("capture device init failed: ") + ma_result_description(r);
        return false;
    }
    d->opened = true;

    r = ma_device_start(&d->dev);
    if(r != MA_SUCCESS){
        if(err) *err = std::string("capture device start failed: ") + ma_result_description(r);
        stop();
        return false;
    }
    d->running = true;
    return true;
}

void AudioInput::stop()
{
    if(d->opened){
        ma_device_uninit(&d->dev);      // stops the device and waits for the callback
        d->opened = false;
    }
    d->running = false;
}

bool AudioInput::isRunning() const {
    return d->running;
}

// ============================================================== AudioOutput
struct AudioOutput::Impl
{
    ma_device dev;
    bool opened = false;
    bool running = false;
    int channels = 1;
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
    stop();
}

bool AudioOutput::start(int sampleRate, int channels, int periodMs, Callback cb, std::string *err)
{
    stop();
    d->cb = std::move(cb);
    d->channels = channels;

    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format   = ma_format_s16;
    cfg.playback.channels = ma_uint32(channels);
    cfg.sampleRate        = ma_uint32(sampleRate);
    cfg.periodSizeInMilliseconds = ma_uint32(periodMs);
    cfg.dataCallback      = Impl::onData;
    cfg.pUserData         = d.get();

    ma_result r = ma_device_init(nullptr, &cfg, &d->dev);
    if(r != MA_SUCCESS){
        if(err) *err = std::string("playback device init failed: ") + ma_result_description(r);
        return false;
    }
    d->opened = true;

    r = ma_device_start(&d->dev);
    if(r != MA_SUCCESS){
        if(err) *err = std::string("playback device start failed: ") + ma_result_description(r);
        stop();
        return false;
    }
    d->running = true;
    return true;
}

void AudioOutput::stop()
{
    if(d->opened){
        ma_device_uninit(&d->dev);
        d->opened = false;
    }
    d->running = false;
}

bool AudioOutput::isRunning() const { return d->running; }

// ============================================================ decodeAudioFile
Pcm decodeAudioFile(const std::string &path, int sampleRate, int channels, std::string *err)
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
