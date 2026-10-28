#ifndef RAVAVA_AUDIOTYPES_H
#define RAVAVA_AUDIOTYPES_H

// Qt-free basic types (namespace ravava) + logging macros (RAVAVA_WARN / RAVAVA_INFO) used by the whole audio library.
#include <cstdint>
#include <cstdio>
#include <vector>

namespace ravava {

using Bytes = std::vector<uint8_t>;   // encoded packets / datagrams
using Pcm   = std::vector<int16_t>;   // interleaved 16bit PCM samples

} // namespace ravava

#define RAVAVA_WARN(...) do { std::fprintf(stderr, "[ravava] " __VA_ARGS__); std::fputc('\n', stderr); } while(0)
#define RAVAVA_INFO(...) do { std::fprintf(stdout, "[ravava] " __VA_ARGS__); std::fputc('\n', stdout); } while(0)

#endif // RAVAVA_AUDIOTYPES_H
