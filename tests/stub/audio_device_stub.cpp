// thread-based fake devices so recorder -> network -> player can run without miniaudio / sound card
#include <mutex>
#include "ravava/AudioDevice.h"
#include <atomic>
#include <chrono>
#include <thread>
#include <cmath>

// tests can look at what the fake speaker receives (called on the fake audio thread after every callback)
void (*g_stubSpeakerTap)(const int16_t *pcm, uint32_t frames, int channels) = nullptr;

// the device id the last Start() was asked for (tests check that the config reaches the device layer)
std::string g_stubInputDevice, g_stubOutputDevice;
static std::mutex g_stubDeviceMutex;        // SetDevice() runs on the device-switch worker thread, the test reads from its own
std::atomic<int> g_stubInputStarts{0}, g_stubOutputStarts{0};         // how many times the device was opened
std::atomic<int> g_stubInputFailStarts{0}, g_stubOutputFailStarts{0};  // test switch: the next N Start() calls fail
std::atomic<bool> g_stubInputMoved{false}, g_stubOutputMoved{false};  // test switch: "the system moved the stream"
// how long the speaker callback (= Player::onDeviceData, on the fake audio thread) ran: longest, count, and how many took more
// than half / all of the device period (the deadline). tests/callback_time_test.cpp reads them.
std::atomic<long> g_stubOutCbMaxUs{0}, g_stubOutCbCount{0}, g_stubOutCbOverHalf{0}, g_stubOutCbOverDeadline{0};
std::string StubInputDevice()  { std::lock_guard<std::mutex> lk(g_stubDeviceMutex); return g_stubInputDevice; }
std::string StubOutputDevice() { std::lock_guard<std::mutex> lk(g_stubDeviceMutex); return g_stubOutputDevice; }

namespace ravava {

struct AudioInput::Impl { std::thread t; std::atomic<bool> run{false}; std::atomic<bool> running{false}; };
void SetOutputRouteHint(const std::string &) {}     // (the real ones are in AudioDevice.cpp: Android routing only)
void SetAndroidCallMode(bool) {}

AudioInput::AudioInput() : d(new Impl) {}
void AudioInput::SetDevice(const std::string &id) { std::lock_guard<std::mutex> lk(g_stubDeviceMutex); g_stubInputDevice = id; }
std::vector<AudioDeviceInfo> ListInputDevices()  { return { {"Stub mic", "aa", true}, {"Stub mic 2", "bb", false} }; }
std::vector<AudioDeviceInfo> ListOutputDevices() { return { {"Stub speaker", "cc", true}, {"Stub speaker 2", "dd", false} }; }
AudioInput::~AudioInput(){ Stop(); }
bool AudioInput::Start(int rate, int ch, int ms, Callback cb, std::string*){
    g_stubInputStarts++;
    Stop();
    if(g_stubInputFailStarts > 0){ g_stubInputFailStarts--; return false; } d->run = true; d->running = true;
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
bool AudioInput::DeviceMoved() { return g_stubInputMoved.exchange(false); }
std::string AudioInput::ActiveDevice() const { return "Stub mic"; }
bool AudioInput::IsRunning() const { return d->running; }

struct AudioOutput::Impl { std::thread t; std::atomic<bool> run{false}; std::atomic<bool> running{false}; };
AudioOutput::AudioOutput() : d(new Impl) {}
void AudioOutput::SetDevice(const std::string &id) { std::lock_guard<std::mutex> lk(g_stubDeviceMutex); g_stubOutputDevice = id; }
AudioOutput::~AudioOutput(){ Stop(); }
bool AudioOutput::Start(int rate, int ch, int ms, Callback cb, std::string*){
    g_stubOutputStarts++;
    Stop();
    if(g_stubOutputFailStarts > 0){ g_stubOutputFailStarts--; return false; } d->run = true; d->running = true;
    d->t = std::thread([=]{
        const uint32_t frames = rate*ms/1000; std::vector<int16_t> buf(frames*ch);
        auto next = std::chrono::steady_clock::now();
        while(d->run){
            const auto t0 = std::chrono::steady_clock::now();
            cb(buf.data(), frames);
            const long us = long(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count());
            g_stubOutCbCount++;
            if(us > g_stubOutCbMaxUs) g_stubOutCbMaxUs = us;                 // (one audio thread writes these)
            if(us * 2 > ms * 1000) g_stubOutCbOverHalf++;
            if(us > ms * 1000) g_stubOutCbOverDeadline++;
            if(g_stubSpeakerTap) g_stubSpeakerTap(buf.data(), frames, ch); next += std::chrono::milliseconds(ms); std::this_thread::sleep_until(next); }});
    return true; }
void AudioOutput::Stop(){ d->run=false; if(d->t.joinable()) d->t.join(); d->running=false; }
bool AudioOutput::DeviceMoved() { return g_stubOutputMoved.exchange(false); }
std::string AudioOutput::ActiveDevice() const { return "Stub speaker"; }
bool AudioOutput::IsRunning() const { return d->running; }
Pcm DecodeAudioFile(const std::string&, int, int, std::string *err){ if(err)*err="stub"; return Pcm(); }

// fake music file: path "tone" = an endless 440 Hz sine (amplitude 8000), "tone880" = the same at 880 Hz (differs from the fake
// microphone, so mixing the two is measurable), "short" = 440 Hz for 0.3 s and then the end of the file, anything else = "cannot open"
#include <cmath>
struct AudioFileReader::Impl { bool tone = false; double hz = 440; std::atomic<uint64_t> limit{~0ull}; int rate = 48000, ch = 1; std::atomic<uint64_t> pos{0}; std::string err; };   // (the real reader keeps its positions in atomics too)
AudioFileReader::AudioFileReader() : d(new Impl) {}
AudioFileReader::~AudioFileReader() {}
void AudioFileReader::Open(const std::string &path, int sampleRate, int channels)
{
    d->tone = (path == "tone" || path == "tone880" || path == "short"); d->rate = sampleRate; d->ch = channels < 1 ? 1 : channels; d->pos = 0;
    d->hz = path == "tone880" ? 880.0 : 440.0;
    d->limit.store(path == "short" ? uint64_t(sampleRate) * 3 / 10 : ~0ull);
    d->err = d->tone ? std::string() : std::string("cannot open '") + path + "' (stub)";
}
void AudioFileReader::Close() {}
size_t AudioFileReader::Read(int16_t *dst, size_t samples)
{
    if(!d->tone) return 0;
    size_t i = 0;
    for(; i + size_t(d->ch) <= samples && d->pos.load() < d->limit.load(); i += size_t(d->ch)){
        const int16_t v = int16_t(8000.0 * std::sin(2.0 * 3.14159265358979 * d->hz * double(d->pos.fetch_add(1)) / d->rate));
        for(int c = 0; c < d->ch; ++c) dst[i + size_t(c)] = v;
    }
    return i;
}
bool AudioFileReader::IsFinished() const { return d->pos.load() >= d->limit.load(); }
std::string AudioFileReader::Error() const { return d->err; }

} // namespace ravava
