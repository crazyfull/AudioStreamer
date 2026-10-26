// no-op EchoCanceller so tests/e2e.cpp (pipeline test without webrtc) links without the library.
// Nothing calls it there: e2e.cpp never sets an echo canceller on the recorder / player.
#include "echoCanceller.h"

struct EchoCanceller::Impl {};
EchoCanceller::EchoCanceller() : d(new Impl) {}
EchoCanceller::~EchoCanceller() {}
bool EchoCanceller::init(int, int, int, const Options&, std::string *err) { if(err) *err = "stub"; return false; }
bool EchoCanceller::init(int, int, int, std::string *err) { if(err) *err = "stub"; return false; }
bool EchoCanceller::isReady() const { return false; }
void EchoCanceller::render(const int16_t*, uint32_t) {}
void EchoCanceller::capture(const int16_t *pcm, uint32_t frames, Pcm &out) { if(pcm) out.insert(out.end(), pcm, pcm + frames); }
void EchoCanceller::setStreamDelayMs(int) {}
bool EchoCanceller::getStats(Stats&) const { return false; }
