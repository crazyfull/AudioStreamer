// thread-based fake devices so recorder -> network -> player can run without miniaudio / sound card
#include "audioDevice.h"
#include <atomic>
#include <chrono>
#include <thread>
#include <cmath>

struct AudioInput::Impl { std::thread t; std::atomic<bool> run{false}; bool running=false; };
AudioInput::AudioInput() : d(new Impl) {}
AudioInput::~AudioInput(){ stop(); }
bool AudioInput::start(int rate, int ch, int ms, Callback cb, std::string*){
    stop(); d->run = true; d->running = true;
    d->t = std::thread([=]{
        const uint32_t frames = rate*ms/1000; std::vector<int16_t> buf(frames*ch); double ph=0; 
        auto next = std::chrono::steady_clock::now();
        while(d->run){
            for(uint32_t i=0;i<frames;i++){ int16_t v=int16_t(8000*std::sin(ph)); ph+=2*M_PI*440.0/rate; for(int c=0;c<ch;c++) buf[i*ch+c]=v; }
            cb(buf.data(), frames);
            next += std::chrono::milliseconds(ms); std::this_thread::sleep_until(next);
        }});
    return true; }
void AudioInput::stop(){ d->run=false; if(d->t.joinable()) d->t.join(); d->running=false; }
bool AudioInput::isRunning() const { return d->running; }

struct AudioOutput::Impl { std::thread t; std::atomic<bool> run{false}; bool running=false; };
AudioOutput::AudioOutput() : d(new Impl) {}
AudioOutput::~AudioOutput(){ stop(); }
bool AudioOutput::start(int rate, int ch, int ms, Callback cb, std::string*){
    stop(); d->run = true; d->running = true;
    d->t = std::thread([=]{
        const uint32_t frames = rate*ms/1000; std::vector<int16_t> buf(frames*ch);
        auto next = std::chrono::steady_clock::now();
        while(d->run){ cb(buf.data(), frames); next += std::chrono::milliseconds(ms); std::this_thread::sleep_until(next); }});
    return true; }
void AudioOutput::stop(){ d->run=false; if(d->t.joinable()) d->t.join(); d->running=false; }
bool AudioOutput::isRunning() const { return d->running; }
Pcm decodeAudioFile(const std::string&, int, int, std::string *err){ if(err)*err="stub"; return Pcm(); }
