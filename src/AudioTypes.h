#ifndef AUDIOTYPES_H
#define AUDIOTYPES_H

// Qt-free basic types + logging used by the whole audio part.
#include <cstdint>
#include <cstdio>
#include <vector>

using Bytes = std::vector<uint8_t>;   // encoded packets / datagrams
using Pcm   = std::vector<int16_t>;   // interleaved 16bit PCM samples

#define AUDIO_WARN(...) do { std::fprintf(stderr, "[audio] " __VA_ARGS__); std::fputc('\n', stderr); } while(0)
#define AUDIO_INFO(...) do { std::fprintf(stdout, "[audio] " __VA_ARGS__); std::fputc('\n', stdout); } while(0)

#endif // AUDIOTYPES_H
