// thread-based fake devices so recorder -> network -> player can run without miniaudio / sound card
#include "ravava/AudioDevice.h"
#include <atomic>
#include <chrono>
#include <thread>
#include <cmath>

// tests can look at what the fake speaker receives (called on the fake audio thread after every callback)
void (*g_stubSpeakerTap)(const int16_t *pcm, uint32_t frames, int channels) = nullptr;

// the device id the last Start() was asked for (tests check that the config reaches the device layer)
std::string g_stubInputDevice, g_stubOutputDevice;

namespace ravava {

struct AudioInput::Impl { std::thread t; std::atomic<bool> run{false}; bool running=false; };
AudioInput::AudioInput() : d(new Impl) {}
void AudioInput::SetDevice(const std::string &id) { g_stubInputDevice = id; }
std::vector<AudioDeviceInfo> ListInputDevices()  { return { {"Stub mic", "aa", true}, {"Stub mic 2", "bb", false} }; }
std::vector<AudioDeviceInfo> ListOutputDevices() { return { {"Stub speaker", "cc", true}, {"Stub speaker 2", "dd", false} }; }
AudioInput::~AudioInput(){ Stop(); }
bool AudioInput::Start(int rate, int ch, int ms, Callback cb, std::string*){
    Stop(); d->run = true; d->running = true;
    d->t = std::thread([=]{
        const uint32_t frames = rate*ms/1000; std::vector<int16_t> buf(frames*ch); double ph=0; 
        auto next = std::chrono::steady_clock::now();
        while(d->run){
            for(uint32_t i=0;i<frames;i++){ int16_t v=int16_t(8000*std::sin(ph)); ph+=2*M_PI*440.0/rate; for(int c=0;c<ch;c++) buf[i*ch+c]=v; }
            cb(buf.data(), frames);
            next += std::chrono::milliseconds(ms); std::this_thread::sleep_until(next);
        }});
    return true; }
void AudioInput::Stop(){ d->run=false; if(d->t.joinable()) d->t.join(); d->running=false; }
bool AudioInput::IsRunning() const { return d->running; }

struct AudioOutput::Impl { std::thread t; std::atomic<bool> run{false}; bool running=false; };
AudioOutput::AudioOutput() : d(new Impl) {}
void AudioOutput::SetDevice(const std::string &id) { g_stubOutputDevice = id; }
AudioOutput::~AudioOutput(){ Stop(); }
bool AudioOutput::Start(int rate, int ch, int ms, Callback cb, std::string*){
    Stop(); d->run = true; d->running = true;
    d->t = std::thread([=]{
        const uint32_t frames = rate*ms/1000; std::vector<int16_t> buf(frames*ch);
        auto next = std::chrono::steady_clock::now();
        while(d->run){ cb(buf.data(), frames); if(g_stubSpeakerTap) g_stubSpeakerTap(buf.data(), frames, ch); next += std::chrono::milliseconds(ms); std::this_thread::sleep_until(next); }});
    return true; }
void AudioOutput::Stop(){ d->run=false; if(d->t.joinable()) d->t.join(); d->running=false; }
bool AudioOutput::IsRunning() const { return d->running; }
Pcm DecodeAudioFile(const std::string&, int, int, std::string *err){ if(err)*err="stub"; return Pcm(); }

} // namespace ravava
