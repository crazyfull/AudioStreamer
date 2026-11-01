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
