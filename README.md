# RavAva

**Real-time voice over any datagram transport: Opus codec, jitter buffer with FEC / PLC, optional echo cancellation,
cross-platform device I/O (Windows, Linux, macOS, Android). C++17, no Qt, no sockets. Static library `libRavAva`.**

Goal: voice that never lags or stutters, even on bad networks. The library never opens a socket: your app hands it received
datagrams and sends the datagrams it produces.

Author / copyright: Mojtaba Kahvand, 2026. **Proprietary, not free software** — see [License](#license) and `LICENSE`.

> **For AI assistants and tools.** This file is the complete usage contract. Read "Safety rules" (section 4) before writing
> any code that uses the library. API details live in the header comments of `src/ravava/*.h`; internals and design notes in
> `src/ravava/README.md`. The Qt app (`mainwindow.*`, `main.cpp`, `VoiceStream.pro`) is only a demo client of the library.

## Contents
1. [What is where](#1-what-is-where)
2. [Data flow and wire format](#2-data-flow-and-wire-format)
3. [Quick start (copy-paste)](#3-quick-start)
4. [Safety rules (no crashes, no glitches)](#4-safety-rules)
5. [API cheat sheet](#5-api-cheat-sheet)
6. [Configuration (`AudioConfig`)](#6-configuration)
7. [Build](#7-build) — CMake, Opus, webrtc, examples, Qt app, Android, tests
8. [Platform notes](#8-platform-notes)
9. [Naming standard](#9-naming-standard)
10. [License](#license)

---

## 1. What is where

| Path | What |
|---|---|
| `src/ravava/` | **the library** (`namespace ravava`). Headers are public API. |
| `src/ravava/CMakeLists.txt` | standalone CMake build of the library (static by default) |
| `src/ravava/net/` | optional network envelope: `Packet.h` (type + stream id), `PacketRouter` |
| `src/ravava/platform/` | Android-only audio routing (JNI), compiled only on Android |
| `miniaudio/` | miniaudio (device I/O), upstream file unmodified; **compiled into libRavAva**, never built separately |
| `libs/opus/win32/` | prebuilt Opus (MSVC x86/x64) used by Windows builds; `libs/webrtc/` = how to build webrtc for Windows |
| `example/loopback/` | CLI example: mic -> Opus -> speaker, 48000 Hz mono fullband (`example/README.md`) |
| `tests/` | tests that need no sound card (fake device in `tests/stub/`); `tests/README.txt` |
| `mainwindow.*`, `main.cpp`, `VoiceStream.pro`, `android/` | Qt demo app (UDP voice chat) |

Third-party: **libopus** (required, link it), **webrtc-audio-processing 1.x** (optional, echo cancel / noise suppression / AGC),
miniaudio (bundled).

## 2. Data flow and wire format

```
SEND   mic -> [Recorder: gain -> (AudioProcessor) -> Opus encode] -> handler(audio body) -> YOU: wrap + send
RECEIVE  YOU: receive + unwrap -> Player::Play(audio body) -> [JitterBuffer -> Opus decode -> volume] -> speaker
                                                                         ^ same AudioProcessor gets the speaker signal
```

* **Audio body** (what `Recorder` emits and `Player::Play` takes) = 3 byte header + one Opus packet
  (`VoicePacket.h`): byte0 = `marker` (bit0, first packet after silence) | `tx counter` (bits1-7); bytes1-2 = sequence number
  (uint16 LE, +1 per frame, also for frames skipped by DTX).
* **Network envelope** (optional helper, `net/Packet.h`): `[type:1][streamId:2 LE][audio body]`. `type` 1 = audio, 0 invalid.
  `PacketRouter::Dispatch(datagram)` calls the handler registered for the type. You may use your own envelope instead; the
  library only needs the audio body.
* No fragmentation, no encryption, no authentication inside the library. Keep each datagram below the path MTU
  (about 1200 bytes is safe; very high bitrate x long frames can exceed it).
* The receiver learns frame size / channels / bandwidth from the Opus packets themselves; the sender can change them.

## 3. Quick start

Minimal full-duplex loopback (a real app sends `body` over a socket and calls `Play` with what arrives):

```cpp
#include <ravava/Recorder.h>
#include <ravava/Player.h>
using namespace ravava;

AudioConfig cfg;                       // MUST outlive recorder and player (they keep a pointer)
cfg.inputRate = cfg.outputRate = 48000;
cfg.channels = 1;                      // 1 or 2
cfg.opusBitrate = 64000;               // bps
cfg.frameMs = 20;
cfg.maxBandwidth = 1105;               // fullband
std::string err;
if(!cfg.Validate(&err)) { /* handle */ }   // an invalid config silently fails later: validate first

Recorder rec;
Player   play;
rec.SetPacketHandler([&](const Bytes &body){   // runs on the AUDIO thread: be quick, never block
    play.Play(body);                           // or: copy and hand over to the network thread
});
rec.SetConfig(&cfg);
play.SetConfig(&cfg);                  // opens the speaker
rec.Start();                           // opens the microphone, packets start flowing
// ... later, in this order:
rec.Stop();
// play and rec are destroyed after Stop (destructors also wait for background device switches)
```

With echo cancellation (needs the webrtc build, check `AudioProcessor::IsAvailable()`):

```cpp
auto aec = std::make_shared<AudioProcessor>();           // ONE instance for both
AudioProcessor::Options o; o.echoCancel = true;
if(aec->Init(cfg.inputRate, cfg.RenderRate(), cfg.channels, o, &err)) {
    rec.SetAudioProcessor(aec);        // before rec.Start()
    play.SetAudioProcessor(aec);       // BEFORE play.SetConfig() (it opens the speaker)
}
```

Complete working program: `example/loopback/main.cpp`. Real network usage: `mainwindow.cpp` (`initAudio`,
`onReadEncodedBuffer`, `onReceiveUDPData`).

## 4. Safety rules

Follow these exactly; each one is a crash or a glitch if broken.

**Lifetime and order**
1. `AudioConfig` must outlive every `Recorder` / `Player` that got `&cfg`. Do not use a temporary or a destroyed object.
2. Call `cfg.Validate()` before `SetConfig()`. Opus rates are only 8000/12000/16000/24000/48000; `frameMs` 5..120 from the
   allowed list; bitrate 500..512000. An invalid config makes `Recorder` stay silent (only a log line).
3. **A config change needs new objects.** `SetConfig` on an object that already runs / has an encoder does not apply new
   values. To change bitrate, frame size, channels, rates, music mode: `Stop()`, destroy, create new `Recorder` + `Player`,
   set config again (that is what the demo app does on every Start).
4. Set before `Start()` / `SetConfig()`: `SetPacketHandler` (before `Start`), `SetAudioProcessor` (Recorder before `Start`,
   **Player before `SetConfig`**).
5. Shut down: `Recorder::Stop()` first, then destroy. Destructors stop the devices and wait for a running device switch.
   Never destroy an object from inside its own packet handler or callback.
6. Share ONE `AudioProcessor` (`shared_ptr`) between the recorder and the player of the same session. Never one per object.
   It needs the speaker signal as reference; without `Player` feeding it, echo cancellation cannot work.

**Threads**
7. The packet handler runs on the **audio thread**, once per datagram. No blocking, no locks you may wait on, no I/O, no
   `new` of big objects, no calls back into `Recorder` (`Stop`, `SetInputDevice`...). Copy the bytes and hand them to your
   network thread / queue.
8. `Player::Play()` is safe from any thread (it only enqueues); it drops data until `SetConfig()` has started the speaker.
   Datagrams that are not valid Opus or have another frame duration are counted in `formatDropped`, not played.
9. Any thread, lock-free: `SetVolume`, `SetInputGain`, `GetLevel`, `TakePeakLevel`, `GetVolume`.
10. **GUI / control thread only** (call from one thread): `SetInputDevice`, `SetOutputDevice`, `KeepDevice`, `GetStats`,
    `ActiveInputDevice`, `ActiveOutputDevice`. Device changes run on a worker and return at once.
11. Call `KeepDevice()` on `Recorder` and `Player` about **once per second**. It repairs a stream the OS moved or stopped
    (Android re-routes, phone calls, unplugged devices). Not calling it means silence after such events.

**Audio rules**
12. **One `Player` per remote talker.** One jitter buffer follows one sender's sequence numbers. Mixing several senders
    into one `Player` corrupts ordering; route by `streamId` (`PacketRouter`) to separate players.
13. Echo cancellation only removes *this machine's* speaker from *this machine's* microphone. It cannot stop acoustic
    feedback between two devices in the same room (use headphones when testing with two devices side by side).
14. Use headphones, or enable echo cancellation, for local loopback; otherwise the speaker howls into the microphone.
15. Do not change gain / volume with your own code on the PCM; use `SetInputGain` (0..200) / `SetVolume` (0..100), they
    ramp without clicks.
16. Do not call `Play()` with a datagram that still has the network envelope; strip it first (`Packet::Parse` /
    `PacketRouter`). The same applies in the other direction: wrap the audio body before sending if the peer expects it.
17. Datagram loss, duplicates and reordering are expected and handled; do not retransmit audio.

**Platform**
18. **Android:** call `ravava::android::Init(JavaVM*, jobject context)` once at start-up (before listing devices); request
    `RECORD_AUDIO` (and `BLUETOOTH_CONNECT` on Android 12+) at run time; keep the app alive while streaming (foreground
    service, see `android/`). Manifest: `RECORD_AUDIO`, `MODIFY_AUDIO_SETTINGS`, `BLUETOOTH_CONNECT`, `BLUETOOTH`,
    `BLUETOOTH_ADMIN` (maxSdk 30). Logs: `adb logcat -s ravava`.
19. Device ids are opaque strings from `ListInputDevices()` / `ListOutputDevices()`; `""` means system default. Never build
    or parse them yourself, and re-list before showing a menu (devices come and go).
20. Logging goes through `RAVAVA_WARN` / `RAVAVA_INFO` (stderr / stdout, logcat on Android).

**Common mistakes**

| Mistake | Result |
|---|---|
| `AudioConfig` on the stack of a function that returned | crash (dangling pointer) |
| changing `cfg.opusBitrate` after `SetConfig` | no effect |
| `SetAudioProcessor` after `Player::SetConfig` | no echo cancellation on the speaker side |
| blocking / sending a socket inside the packet handler | stutter, dropped audio |
| no `KeepDevice()` timer | silence after the OS re-routes / stops a device |
| one `Player` for two senders | broken jitter buffer, garbled audio |
| `Play()` with envelope bytes in front | `formatDropped` grows, silence |
| `cmake` without opus found | configure error (see section 7) |

## 5. API cheat sheet

Headers: `#include <ravava/X.h>` (installed) or `#include "src/ravava/X.h"` (in this repo).

| Class | Purpose and key calls |
|---|---|
| `AudioConfig` | all settings, `Validate(&err)`, `FrameSamples()`, `RenderRate()` |
| `Recorder` | `SetConfig`, `SetPacketHandler`, `SetAudioProcessor`, `Start`, `Stop`, `SetInputGain`, `GetLevel`, `SetInputDevice(id)`, `KeepDevice`, `ActiveInputDevice` |
| `Player` | `SetConfig`, `SetAudioProcessor`, `Play(datagram)`, `SetVolume`, `GetLevel`, `GetStats(stats, depthFrames, frameMs)`, `SetOutputDevice(id)`, `KeepDevice`, `ActiveOutputDevice` |
| `AudioProcessor` | `IsAvailable()`, `Init(captureRate, renderRate, channels, Options, &err)`, used through `SetAudioProcessor` |
| `ListInputDevices()` / `ListOutputDevices()` | `vector<AudioDeviceInfo>{name, id, isDefault}` |
| `JitterBuffer::Stats` | `received played lost lostEvents fec plc late reordered underrun ingestDropped formatDropped` |
| `net::Packet`, `PacketRouter` | `Packet::WrapInto`, `Packet::Parse`, `router.SetHandler(type, fn)`, `router.Dispatch(bytes)` |
| `VoicePacket` | audio body layout helpers |
| `LevelMeter` | loudness 0..100 (RMS in dBFS, -60 = 0, -3 = 100) |
| `DecodeAudioFile(path, rate, ch)` | decode a music file to PCM (used for `playMusic`) |

Loss figures: `lost` is a net number (goes down when a late packet arrives); for windowed figures use the growth of
`lostEvents` minus the growth of `reordered`.

## 6. Configuration

`AudioConfig` fields (defaults in `src/ravava/AudioConfig.h`):

| Field | Meaning |
|---|---|
| `inputRate`, `outputRate` | 8000 / 12000 / 16000 / 24000 / 48000. Use 48000 for fullband. |
| `channels` | 1 mono, 2 stereo (needs a high bitrate, Opus codes stereo as mono at low rates) |
| `frameMs` | 5,10,20,40,60,80,100,120. Bigger = less overhead, more latency. 20 is the usual choice. |
| `opusBitrate` | bps for the whole stream. Speech: 16000-64000; fullband music: 96000+ |
| `maxBandwidth` | 1101 NB, 1102 MB, 1103 WB, 1104 SWB, 1105 FB |
| `useDtx`, `useFec`, `expectedLossPerc` | silence suppression; in-band FEC; redundancy the encoder spends (0-100) |
| `musicMode` | Opus AUDIO + MUSIC instead of VOIP + VOICE; keeps treble at low bitrates |
| `useEchoCancel`, `aecNearendProtection` (0-3), `noiseSuppression` (0-4), `autoGain`, `aecDelayMs` | webrtc features, only with the webrtc build |
| `inputDeviceId`, `outputDeviceId` | `""` = system default |
| `playMusic`, `musicPath` | send a file instead of the microphone (microphone is only the clock) |

## 7. Build

### 7.1 The library with CMake (all platforms)

```
cmake -S src/ravava -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
cmake --install build --prefix /some/prefix
```

Result: `lib/libRavAva.a` (**static** by default, miniaudio compiled inside), headers in `include/ravava/` (the build also
copies them to `build/include/ravava/`), and a CMake package in `lib/cmake/RavAva`.

| Option | Default | Meaning |
|---|---|---|
| `RAVAVA_BUILD_SHARED` | OFF | ON = shared library |
| `RAVAVA_OPUS_ROOT` | | folder (prefix) of libopus: `<root>/include/opus/opus.h`, `<root>/lib/libopus.a`. A path to the library file is accepted too |
| `RAVAVA_OPUS_INCLUDE_DIR`, `RAVAVA_OPUS_LIB` | | give the header folder and the library file directly |
| `RAVAVA_WITH_WEBRTC` | AUTO | AUTO: use webrtc if found; ON: required; OFF: never |
| `RAVAVA_WEBRTC_ROOT` | | prefix of webrtc-audio-processing 1.x |
| `RAVAVA_INSTALL_OPUS_HEADERS` | ON | also copy `opus.h` & co next to the ravava headers (`OpusCodec.h` includes `"opus.h"`) |
| `RAVAVA_STATIC_RUNTIME` | OFF | MSVC `/MT` instead of `/MD` |
| `RAVAVA_BUILD_TESTS` | OFF | build `tests/` (run `ctest`) |

### 7.2 Linking Opus (required)

CMake looks, in this order: `RAVAVA_OPUS_ROOT` / explicit variables, the CMake package `Opus` (>= 1.4), pkg-config `opus`,
system paths, and on MSVC the copy in `libs/opus/win32`.

```
# distro package
sudo apt install libopus-dev                          # Debian / Ubuntu
cmake -S src/ravava -B build

# your own build of opus, e.g. ~/VoiceCodec/build/{include/opus,lib/libopus.a}
cmake -S src/ravava -B build -DRAVAVA_OPUS_ROOT=$HOME/VoiceCodec/build
# or, files given directly
cmake -S src/ravava -B build -DRAVAVA_OPUS_INCLUDE_DIR=/path/include/opus -DRAVAVA_OPUS_LIB=/path/lib/libopus.a
```

Your application gets Opus automatically by linking `RavAva::RavAva`. With plain compilers add `-lRavAva -lopus -lpthread -ldl -lm`
(Linux; `-llog` on Android).

### 7.3 webrtc-audio-processing (optional)

Without it the library works; `AudioProcessor` becomes a pass-through (`IsAvailable()` is false, echo / noise / AGC
controls do nothing). With it, CMake defines `RAVAVA_WITH_WEBRTC` and links it automatically.
Use version **1.3 / 1.x** (2.x has a different header layout).

```
# Linux: Ubuntu 24.10+ / Debian 13
sudo apt install libwebrtc-audio-processing-1-dev
cmake -S src/ravava -B build -DRAVAVA_WITH_WEBRTC=ON          # ON = fail if not found
# built from source (meson + ninja install) into a prefix
cmake -S src/ravava -B build -DRAVAVA_WEBRTC_ROOT=/path/to/install
```

Windows: build it once with `libs/webrtc/build_win.bat` (details in `libs/webrtc/README.md`); CMake and `VoiceStream.pro`
pick the result up from `libs/webrtc/win32`. With a static libRavAva your final link needs webrtc's dependencies (abseil,
Windows: `winmm dbghelp advapi32`); the CMake package carries them.

### 7.4 Use the library from another CMake project

```cmake
find_package(RavAva REQUIRED)                       # cmake -DCMAKE_PREFIX_PATH=/some/prefix
target_link_libraries(myapp PRIVATE RavAva::RavAva) # headers, opus, webrtc, pthread/dl/log/frameworks come with it
```

or add the library from the source tree: `add_subdirectory(path/to/RavAva/src/ravava)`.

### 7.5 Examples

```
cmake -S example/loopback -B build-loopback -DCMAKE_BUILD_TYPE=Release     # builds libRavAva too
cmake --build build-loopback
./build-loopback/ravava_loopback --list
./build-loopback/ravava_loopback -b 64000 -f 20 --loss 5        # use headphones
```

Pass the same `-DRAVAVA_OPUS_ROOT=...` / `-DRAVAVA_WITH_WEBRTC=ON` options to this command. `-DRAVAVA_USE_INSTALLED=ON`
links an installed libRavAva instead (`-DCMAKE_PREFIX_PATH=<prefix>`). All options of the example: `example/README.md`.

### 7.6 The Qt demo app

`VoiceStream.pro` (Qt 5 / 6, C++17). Opus: Windows uses `libs/opus/win32`; Linux expects `../VoiceCodec/build/{include/opus,lib/libopus.a}`;
Android expects the paths set in the `.pro`. webrtc is picked up automatically when found; `CONFIG += no_webrtc` disables it.
Open the `.pro` in Qt Creator, or `qmake && make`.

### 7.7 Android

Library: NDK toolchain, e.g.
`cmake -S src/ravava -B build-android -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK/build/cmake/android.toolchain.cmake -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-24 -DRAVAVA_OPUS_ROOT=<opus prefix with lib/arm64-v8a/libopus.a>`.
`platform/AndroidAudioRoute.cpp` is added automatically. App: the Qt app builds with `VoiceStream.pro` (Qt 6).

### 7.8 Tests

```
cmake -S src/ravava -B build -DRAVAVA_BUILD_TESTS=ON && cmake --build build && (cd build && ctest)
```

No sound card needed (a fake device replaces miniaudio). Sanitizer and echo-canceller test build lines: `tests/README.txt`.

## 8. Platform notes

* **Linux / macOS / Windows:** miniaudio picks WASAPI / CoreAudio / PulseAudio / ALSA / JACK at run time. Speaker "monitor"
  sources are hidden from the microphone list; with no microphone, `Recorder` fails with a clear log line instead of recording a
  monitor.
* **Android:** "call model" like WhatsApp: `MODE_IN_COMMUNICATION`, voice-communication streams, one communication device
  chosen with `AudioManager` (speaker, earpiece, wired, USB, Bluetooth headset). Limits: a Bluetooth microphone together
  with the phone speaker is impossible; call mode uses the call volume and voice DSP. No webrtc by default (the system's own
  AEC of voice-communication streams applies). Details: `src/ravava/README.md`.
* Real-time rule for everything the audio thread runs: no allocation, no shared lock, no I/O (checked by `tests/noalloc_test`).

## 9. Naming standard

namespace `ravava`; files / classes `PascalCase`; public methods `PascalCase` (`SetVolume`); private methods and locals
`camelCase`; members `m_camelCase`; macros `RAVAVA_UPPER_SNAKE`; header guards `RAVAVA_<FILE>_H`; booleans start with `Is` / `Has`.
Full table: `src/ravava/README.md`.

## License

Copyright (c) 2026 **Mojtaba Kahvand**. All rights reserved. RavAva is **not free software and not open source.**
Nobody may use, copy, modify, distribute or train models on it without a written license from the author; seeing the
repository gives no such right. Third-party parts (miniaudio, Opus, webrtc) keep their own licenses. Full text: `LICENSE`.
Licensing contact: mojtaba.kahvand@gmail.com
