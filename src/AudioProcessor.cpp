// the ONLY place that includes webrtc-audio-processing (version 1.x API).
//
// webrtc is OPTIONAL. With -DVOICE_WITH_WEBRTC this file is the real echo canceller / noise
// suppressor / AGC2; without it the bottom of this file builds a pass-through with the same
// interface (init() fails with a message, isReady() is false, capture() copies the mic as is),
// so the whole project builds and runs without the library. VoiceStream.pro defines
// VOICE_WITH_WEBRTC by itself when the library is found.
#include "AudioProcessor.h"

#ifdef VOICE_WITH_WEBRTC
//
// Compiler defines the webrtc headers need (the same ones the library's own pkg-config file hands out).
// They are set here so the .pro doesn't need them and they can't leak into the Qt / miniaudio sources.
#ifndef WEBRTC_LIBRARY_IMPL
#define WEBRTC_LIBRARY_IMPL
#endif
#if defined(_WIN32)
  #ifndef WEBRTC_WIN
  #define WEBRTC_WIN
  #endif
  #ifndef NOMINMAX
  #define NOMINMAX
  #endif
  #ifndef _USE_MATH_DEFINES
  #define _USE_MATH_DEFINES
  #endif
#else
  #ifndef WEBRTC_POSIX
  #define WEBRTC_POSIX
  #endif
#endif

#if defined(__GNUC__)       // gcc + clang: the webrtc header has an unused parameter, not ours to fix
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#endif
#include "modules/audio_processing/include/audio_processing.h"

// "meson install" does NOT install api/audio/echo_canceller3_factory.h. Without it the near-end
// protection tuning is not available (the canceller itself works, with webrtc defaults).
// To enable it, make this header visible: add <webrtc-audio-processing-1.3>/webrtc to the include path.
#if defined(__has_include)
  #if __has_include("api/audio/echo_canceller3_config.h") && __has_include("api/audio/echo_canceller3_factory.h")
    #include "api/audio/echo_canceller3_config.h"
    #include "api/audio/echo_canceller3_factory.h"
    #define AEC_HAVE_EC3_FACTORY 1
  #endif
#endif
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include "blockCutter.h"
#include <atomic>
#include <cstring>
#include <memory>
#include <vector>

struct AudioProcessor::Impl
{
    rtc::scoped_refptr<webrtc::AudioProcessing> apm;
    webrtc::StreamConfig capCfg;
    webrtc::StreamConfig renCfg;
    size_t channels = 1;
    size_t capBlock = 0;                // samples per 10 ms, ALL channels (interleaved)
    size_t renBlock = 0;
    std::atomic<bool> ready{false};
    std::atomic<int>  delayMs{0};

    // each FIFO and scratch buffer belongs to ONE thread: capture* -> mic thread, render* -> speaker thread.
    // The FIFOs only ever hold the incomplete tail (< 1 block) of the previous call, see cutBlocks().
    std::vector<int16_t> capFifo;
    std::vector<int16_t> renFifo;
    std::vector<int16_t> renScratch;    // ProcessReverseStream output (render side isn't modified, we don't use it)
    bool capErrorLogged = false;
    bool renErrorLogged = false;
};

AudioProcessor::AudioProcessor() : d(new Impl) {

}

AudioProcessor::~AudioProcessor() {

}

bool AudioProcessor::init(int captureRate, int renderRate, int channels, const Options &opt, std::string *err)
{
    auto fail = [&](const char *msg){
        if(err) *err = msg;
        return false;
    };

    d->ready = false;
    d->apm = nullptr;

    // webrtc processes 10 ms blocks: rate / 100 must be a whole number of samples
    if(captureRate < 8000 || captureRate > 192000 || captureRate % 100 != 0)
        return fail("echo canceller: unsupported capture sample rate");
    if(renderRate < 8000 || renderRate > 192000 || renderRate % 100 != 0)
        return fail("echo canceller: unsupported render sample rate");
    if(channels < 1 || channels > 2)
        return fail("echo canceller: only 1 or 2 channels are supported");

    // Create() returns an object with reference count 0: the scoped_refptr takes the first reference.
    webrtc::AudioProcessingBuilder builder;
#ifdef AEC_HAVE_EC3_FACTORY
    if(opt.nearendProtection > 0){
        // Tune only the "is the near end (my voice) dominant?" detector of the suppressor: the earlier it
        // decides "dominant", the less the suppressor gates the mic. Level 0 keeps the webrtc defaults.
        struct Level { float enr; float enrExit; float snr; int hold; };
        static const Level levels[] = {
            { 0.25f, 0.10f, 30.f,  50 },    // 0: defaults (not used here)
            { 0.40f, 0.20f, 25.f,  75 },    // 1
            { 0.60f, 0.30f, 20.f, 100 },    // 2
            { 1.00f, 0.50f, 15.f, 150 },    // 3
        };
        const int lv = opt.nearendProtection > 3 ? 3 : opt.nearendProtection;

        webrtc::EchoCanceller3Config ec3;
        ec3.suppressor.dominant_nearend_detection.enr_threshold = levels[lv].enr;
        ec3.suppressor.dominant_nearend_detection.enr_exit_threshold = levels[lv].enrExit;
        ec3.suppressor.dominant_nearend_detection.snr_threshold = levels[lv].snr;
        ec3.suppressor.dominant_nearend_detection.hold_duration = levels[lv].hold;
        webrtc::EchoCanceller3Config::Validate(&ec3);     // clamps anything out of range
        builder.SetEchoControlFactory(std::make_unique<webrtc::EchoCanceller3Factory>(ec3));
    }
#else
    if(opt.nearendProtection > 0)
        AUDIO_WARN("echo canceller: echo_canceller3_factory.h not found, near-end protection ignored (see AudioProcessor.cpp)");
#endif
    rtc::scoped_refptr<webrtc::AudioProcessing> apm(builder.Create());
    if(!apm)
        return fail("echo canceller: cannot create AudioProcessing");

    webrtc::AudioProcessing::Config cfg;
    cfg.echo_canceller.enabled = opt.echoCancel;
    cfg.echo_canceller.mobile_mode = false;          // AEC3 (full), not AECM
    cfg.high_pass_filter.enabled = opt.highPassFilter;

    if(opt.noiseSuppression > 0){
        using Level = webrtc::AudioProcessing::Config::NoiseSuppression::Level;
        cfg.noise_suppression.enabled = true;
        switch(opt.noiseSuppression){
        case 1:  cfg.noise_suppression.level = Level::kLow; break;
        case 2:  cfg.noise_suppression.level = Level::kModerate; break;
        case 3:  cfg.noise_suppression.level = Level::kHigh; break;
        default: cfg.noise_suppression.level = Level::kVeryHigh; break;
        }
    }
    if(opt.autoGain){
        // AGC2 digital only (no OS volume control): adaptive gain + limiter
        cfg.gain_controller2.enabled = true;
        cfg.gain_controller2.adaptive_digital.enabled = true;
    }
    apm->ApplyConfig(cfg);

    d->capCfg = webrtc::StreamConfig(captureRate, size_t(channels));
    d->renCfg = webrtc::StreamConfig(renderRate, size_t(channels));
    d->channels = size_t(channels);
    d->capBlock = size_t(captureRate / 100) * size_t(channels);
    d->renBlock = size_t(renderRate / 100) * size_t(channels);
    d->delayMs = opt.streamDelayMs;
    d->capErrorLogged = d->renErrorLogged = false;

    d->capFifo.clear();
    d->renFifo.clear();
    d->capFifo.reserve(d->capBlock);                 // exactly enough: the tail is always < 1 block, never reallocates
    d->renFifo.reserve(d->renBlock);
    d->renScratch.assign(d->renBlock, 0);

    // The first call of each stream allocates and initializes AEC3 (takes milliseconds). Do it here
    // with one block of silence so the real-time audio threads never pay for it.
    {
        std::vector<int16_t> silenceCap(d->capBlock, 0), outCap(d->capBlock, 0);
        std::vector<int16_t> silenceRen(d->renBlock, 0);
        apm->ProcessReverseStream(silenceRen.data(), d->renCfg, d->renCfg, d->renScratch.data());
        apm->set_stream_delay_ms(opt.streamDelayMs);
        apm->ProcessStream(silenceCap.data(), d->capCfg, d->capCfg, outCap.data());
    }

    d->apm = apm;
    d->ready = true;
    return true;
}

bool AudioProcessor::init(int captureRate, int renderRate, int channels, std::string *err)
{
    return init(captureRate, renderRate, channels, Options(), err);
}

bool AudioProcessor::isReady() const
{
    return d->ready.load();
}

void AudioProcessor::setStreamDelayMs(int ms)
{
    d->delayMs = ms;
}

// speaker thread
void AudioProcessor::render(const int16_t *pcm, uint32_t frames)
{
    Impl &s = *d;
    if(!s.ready.load() || !pcm || !frames)
        return;

    // 10 ms blocks are taken straight from `pcm`; only a tail of < 10 ms is copied (no allocation, no big memmove)
    cutBlocks(s.renFifo, s.renBlock, pcm, size_t(frames) * s.channels, [&s](const int16_t *block){
        int rc = s.apm->ProcessReverseStream(block, s.renCfg, s.renCfg, s.renScratch.data());
        if(rc != webrtc::AudioProcessing::kNoError && !s.renErrorLogged){
            s.renErrorLogged = true;
            AUDIO_WARN("echo canceller: ProcessReverseStream error %d", rc);
        }
    });
}

// mic thread
void AudioProcessor::capture(const int16_t *pcm, uint32_t frames, Pcm &out)
{
    Impl &s = *d;

    if(!s.ready.load()){
        // not usable: act as a plain pass-through
        if(pcm && frames)
            out.insert(out.end(), pcm, pcm + size_t(frames) * s.channels);
        return;
    }
    if(!pcm || !frames)
        return;

    // `out` is the caller's scratch (cleared each call, so its capacity is reused)
    cutBlocks(s.capFifo, s.capBlock, pcm, size_t(frames) * s.channels, [&s, &out](const int16_t *block){
        const size_t pos = out.size();
        out.resize(pos + s.capBlock);

        // "must be called before every ProcessStream() when echo processing is on"
        s.apm->set_stream_delay_ms(s.delayMs.load());
        int rc = s.apm->ProcessStream(block, s.capCfg, s.capCfg, out.data() + pos);
        if(rc != webrtc::AudioProcessing::kNoError){
            // keep the audio flowing even if webrtc complains
            std::memcpy(out.data() + pos, block, s.capBlock * sizeof(int16_t));
            if(!s.capErrorLogged){
                s.capErrorLogged = true;
                AUDIO_WARN("echo canceller: ProcessStream error %d", rc);
            }
        }
    });
}

bool AudioProcessor::getStats(Stats &st) const
{
    if(!d->ready.load())
        return false;

    webrtc::AudioProcessingStats a = d->apm->GetStatistics();

    st = Stats();
    if(a.echo_return_loss_enhancement.has_value()){
        st.hasErle = true;
        st.erleDb = *a.echo_return_loss_enhancement;
    }
    if(a.echo_return_loss.has_value()){
        st.hasErl = true;
        st.erlDb = *a.echo_return_loss;
    }
    if(a.delay_ms.has_value()){
        st.hasDelay = true;
        st.delayMs = int(*a.delay_ms);
    }
    return true;
}

bool AudioProcessor::available()
{
    return true;
}

#else   // ---------------------------------------------------------------- built without webrtc

#include <cstring>

struct AudioProcessor::Impl
{
    int channels = 1;
};

AudioProcessor::AudioProcessor() : d(new Impl) {}
AudioProcessor::~AudioProcessor() {}

bool AudioProcessor::available()
{
    return false;
}

bool AudioProcessor::init(int, int, int channels, const Options&, std::string *err)
{
    d->channels = channels;
    if(err)
        *err = "built without webrtc-audio-processing (define VOICE_WITH_WEBRTC to enable echo cancellation, noise suppression and AGC)";
    return false;
}

bool AudioProcessor::init(int captureRate, int renderRate, int channels, std::string *err)
{
    return init(captureRate, renderRate, channels, Options(), err);
}

bool AudioProcessor::isReady() const { return false; }
void AudioProcessor::render(const int16_t*, uint32_t) {}
void AudioProcessor::setStreamDelayMs(int) {}
bool AudioProcessor::getStats(Stats&) const { return false; }

void AudioProcessor::capture(const int16_t *pcm, uint32_t frames, Pcm &out)
{
    if(pcm && frames)
        out.insert(out.end(), pcm, pcm + size_t(frames) * size_t(d->channels));
}

#endif  // VOICE_WITH_WEBRTC
