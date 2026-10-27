// no-op AudioProcessor so tests/e2e.cpp (pipeline test without webrtc) links without the library.
// Nothing calls it there: e2e.cpp never sets an echo canceller on the recorder / player.
#include "AudioProcessor.h"

struct AudioProcessor::Impl {};
AudioProcessor::AudioProcessor() : d(new Impl) {}
AudioProcessor::~AudioProcessor() {}
bool AudioProcessor::init(int, int, int, const Options&, std::string *err) { if(err) *err = "stub"; return false; }
bool AudioProcessor::init(int, int, int, std::string *err) { if(err) *err = "stub"; return false; }
bool AudioProcessor::isReady() const { return false; }
void AudioProcessor::render(const int16_t*, uint32_t) {}
void AudioProcessor::capture(const int16_t *pcm, uint32_t frames, Pcm &out) { if(pcm) out.insert(out.end(), pcm, pcm + frames); }
void AudioProcessor::setStreamDelayMs(int) {}
bool AudioProcessor::getStats(Stats&) const { return false; }
