/*
 * RavAva - real-time voice library (Opus over any datagram transport)
 * Copyright (c) 2026 Mojtaba Kahvand. All rights reserved.
 * Author: Mojtaba Kahvand
 *
 * Proprietary software, NOT free / open source. Use, copying, modification and distribution are forbidden without a
 * written license from the author. See the LICENSE file in the repository root.
 * SPDX-License-Identifier: LicenseRef-RavAva-Proprietary
 */
#ifndef RAVAVA_AUDIOTYPES_H
#define RAVAVA_AUDIOTYPES_H

// Qt-free basic types (namespace ravava) + logging macros (RAVAVA_WARN / RAVAVA_INFO) used by the whole audio library.
#include <cstdint>
#include <cstdio>
#include <vector>

namespace ravava {

using Bytes = std::vector<uint8_t>;   // encoded packets / datagrams
using Pcm   = std::vector<int16_t>;   // interleaved 16bit PCM samples
using PcmF  = std::vector<float>;     // interleaved float PCM, -1.0 .. +1.0 (full precision: 24 bit and more)

// Sample converters for 24 / 32 bit sources and sinks (a sound card, a file, a DAW). 24 bit is the same as float for
// Opus: a float has 24 bits of mantissa, so the conversion below loses nothing.
inline float  S24ToFloat(int32_t v)  { return float(v) * (1.0f / 8388608.0f); }                 // v: -8388608 .. 8388607
inline float  S32ToFloat(int32_t v)  { return float(double(v) * (1.0 / 2147483648.0)); }
inline int32_t FloatToS24(float f)   { f = f > 1.0f ? 1.0f : (f < -1.0f ? -1.0f : f); const float x = f * 8388608.0f; return int32_t(x >= 8388607.0f ? 8388607.0f : (x < -8388608.0f ? -8388608.0f : x)); }

} // namespace ravava

#ifdef __ANDROID__
// stdout / stderr go nowhere on Android: write to logcat (adb logcat -s ravava). Needs -llog (VoiceStream.pro)
#include <android/log.h>
#define RAVAVA_WARN(...) __android_log_print(ANDROID_LOG_WARN, "ravava", __VA_ARGS__)
#define RAVAVA_INFO(...) __android_log_print(ANDROID_LOG_INFO, "ravava", __VA_ARGS__)
#else
#define RAVAVA_WARN(...) do { std::fprintf(stderr, "[ravava] " __VA_ARGS__); std::fputc('\n', stderr); } while(0)
#define RAVAVA_INFO(...) do { std::fprintf(stdout, "[ravava] " __VA_ARGS__); std::fputc('\n', stdout); } while(0)
#endif

#endif // RAVAVA_AUDIOTYPES_H
