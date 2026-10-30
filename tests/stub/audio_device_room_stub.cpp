// fake devices that share a simulated ROOM: what the speaker plays comes back into the microphone
// after a delay (and a gain), plus an optional local talker. Real-time threads, 10 ms periods.
// Used by tests/e2e_aec.cpp. Mono, one rate (48000) only.
#include "ravava/AudioDevice.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <random>
#include <thread>


// ---- test control / measurement (declared again with `extern` in tests/e2e_aec.cpp)
std::atomic<bool>   g_talker{false};         // a person talks into the mic
std::atomic<double> g_echoGain{0.9};         // speaker -> mic coupling
std::atomic<int>    g_echoDelayMs{60};
static std::mutex          g_mx;
static std::vector<int16_t> g_ring(48000 * 2, 0);   // speaker history, absolute sample index modulo size
static uint64_t            g_written = 0;
static std::vector<double> g_spkEnergy;      // energy of every 10 ms speaker block, for the test to evaluate

std::vector<double> roomSpeakerEnergy()
{
    std::lock_guard<std::mutex> lk(g_mx);
    return g_spkEnergy;
}

void roomReset()
{
    std::lock_guard<std::mutex> lk(g_mx);
    std::fill(g_ring.begin(), g_ring.end(), int16_t(0));
    g_written = 0;
    g_spkEnergy.clear();
}

// ------------------------------------------------------------------ microphone
// the device id the last Start() was asked for (tests check that the config reaches the device layer)
std::string g_stubInputDevice, g_stubOutputDevice;
static std::mutex g_stubDeviceMutex;        // SetDevice() runs on the device-switch worker thread, the test reads from its own
std::atomic<int> g_stubInputStarts{0}, g_stubOutputStarts{0};         // how many times the device was opened
std::atomic<bool> g_stubInputMoved{false}, g_stubOutputMoved{false};  // test switch: "the system moved the stream"
std::string StubInputDevice()  { std::lock_guard<std::mutex> lk(g_stubDeviceMutex); return g_stubInputDevice; }
std::string StubOutputDevice() { std::lock_guard<std::mutex> lk(g_stubDeviceMutex); return g_stubOutputDevice; }

namespace ravava {

struct AudioInput::Impl { std::thread t; std::atomic<bool> run{false}; bool running = false; };
AudioInput::AudioInput() : d(new Impl) {}
void AudioInput::SetDevice(const std::string &id) { std::lock_guard<std::mutex> lk(g_stubDeviceMutex); g_stubInputDevice = id; }
std::vector<AudioDeviceInfo> ListInputDevices()  { return { {"Stub mic", "aa", true}, {"Stub mic 2", "bb", false} }; }
std::vector<AudioDeviceInfo> ListOutputDevices() { return { {"Stub speaker", "cc", true}, {"Stub speaker 2", "dd", false} }; }
AudioInput::~AudioInput() { Stop(); }

bool AudioInput::Start(int rate, int ch, int ms, Callback cb, std::string*)
{
    Stop();
    d->run = true; d->running = true;
    d->t = std::thread([=]{
        const uint32_t frames = uint32_t(rate * ms / 1000);
        std::vector<int16_t> buf(frames * ch);
        std::mt19937 rng(5);
        std::normal_distribution<double> noise(0.0, 8.0);
        double phase = 0, t = 0;
        auto next = std::chrono::steady_clock::now();
        while(d->run){
            {
                std::lock_guard<std::mutex> lk(g_mx);
                const int delay = rate * g_echoDelayMs.load() / 1000;
                const double gain = g_echoGain.load();
                for(uint32_t i = 0; i < frames; ++i){
                    // the sample the speaker played `delay` samples before "now"
                    double echo = 0;
                    long long idx = (long long)g_written - delay - (long long)(frames - i);
                    if(idx >= 0) echo = gain * g_ring[size_t(idx) % g_ring.size()];

                    double talk = 0;
                    if(g_talker.load()){
                        phase += 2 * M_PI * (150.0 + 30.0 * std::sin(2 * M_PI * 0.8 * t)) / rate;
                        double env = std::pow(std::fmax(0.0, std::sin(2 * M_PI * 2.9 * t)), 1.2);
                        for(int h = 1; h <= 12; ++h) talk += std::sin(h * phase) / h;
                        talk *= 3500.0 * env;
                    }
                    t += 1.0 / rate;
                    double v = echo + talk + noise(rng);
                    v = v > 32767 ? 32767 : (v < -32768 ? -32768 : v);
                    for(int c = 0; c < ch; ++c) buf[i * ch + c] = int16_t(v);
                }
            }
            cb(buf.data(), frames);
            next += std::chrono::milliseconds(ms);
            std::this_thread::sleep_until(next);
        }
    });
    return true;
}
void AudioInput::Stop() { d->run = false; if(d->t.joinable()) d->t.join(); d->running = false; }
bool AudioInput::DeviceMoved() { return g_stubInputMoved.exchange(false); }
std::string AudioInput::ActiveDevice() const { return "Stub mic"; }
bool AudioInput::IsRunning() const { return d->running; }

// ------------------------------------------------------------------ speaker
struct AudioOutput::Impl { std::thread t; std::atomic<bool> run{false}; bool running = false; };
AudioOutput::AudioOutput() : d(new Impl) {}
void AudioOutput::SetDevice(const std::string &id) { std::lock_guard<std::mutex> lk(g_stubDeviceMutex); g_stubOutputDevice = id; }
AudioOutput::~AudioOutput() { Stop(); }

bool AudioOutput::Start(int rate, int ch, int ms, Callback cb, std::string*)
{
    Stop();
    d->run = true; d->running = true;
    d->t = std::thread([=]{
        const uint32_t frames = uint32_t(rate * ms / 1000);
        std::vector<int16_t> buf(frames * ch);
        auto next = std::chrono::steady_clock::now();
        while(d->run){
            cb(buf.data(), frames);
            {
                std::lock_guard<std::mutex> lk(g_mx);
                double e = 0;
                for(uint32_t i = 0; i < frames; ++i){
                    g_ring[size_t(g_written++) % g_ring.size()] = buf[i * ch];
                    e += double(buf[i * ch]) * buf[i * ch];
                }
                g_spkEnergy.push_back(e);
            }
            next += std::chrono::milliseconds(ms);
            std::this_thread::sleep_until(next);
        }
    });
    return true;
}
void AudioOutput::Stop() { d->run = false; if(d->t.joinable()) d->t.join(); d->running = false; }
bool AudioOutput::DeviceMoved() { return g_stubOutputMoved.exchange(false); }
std::string AudioOutput::ActiveDevice() const { return "Stub speaker"; }
bool AudioOutput::IsRunning() const { return d->running; }

Pcm DecodeAudioFile(const std::string&, int, int, std::string *err) { if(err) *err = "stub"; return Pcm(); }

} // namespace ravava
