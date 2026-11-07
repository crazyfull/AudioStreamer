# Changelog

RavAva library (`src/ravava`). The version is the `project(RavAva VERSION ...)` line in `src/ravava/CMakeLists.txt`; it ends
up in `<ravava/Version.h>` (`RAVAVA_VERSION_STRING`) and in the CMake package (`find_package(RavAva 0.1)`).
Not yet released as a number: everything below is in 0.1.0.

## 0.1.0 (in development)

### Added
- Standalone CMake build of the library (static by default, shared option), `RavAva::RavAva` package, headers installed to
  `include/ravava/` with the Opus headers next to them; optional webrtc-audio-processing (echo cancellation, noise
  suppression, AGC); `RAVAVA_OPUS_ROOT` / `RAVAVA_OPUS_INCLUDE_DIR` / `RAVAVA_OPUS_LIB`.
- Loopback command line example (`example/loopback`), built for macOS (arm64 + Intel) by GitHub Actions.
- iOS static build (`.github/workflows/build-ios.yml` -> `RavAva.xcframework`): miniaudio as Objective-C, one shared
  play-and-record audio session, speaker + Bluetooth. Built and linked in CI, not yet run on an iPhone.
- `OpusCodec::EncodeFloat` / `DecodeFloat` and 24 / 32 bit sample converters (float / 24-bit PCM straight into Opus).
- `MusicPlayer` (play, pause, resume, stop, local monitor with its own volume), streamed from a background decoder thread.
- `ConferencePlayer`: several talkers into one speaker, routed by stream id.
- Adaptive playout (late-frame wait, trim / stretch), DRED playback (Opus 1.5+), jitter buffer sized from measured jitter.
- Level meters (`GetLevel`, `TakePeakLevel`), input gain, Packet header with sequence / marker.
- Android: call model like WhatsApp (`MODE_IN_COMMUNICATION`, communication device, Bluetooth SCO, speaker), recovery
  after calls and route changes, stalled-stream detection.

### Changed
- Low-cut (webrtc high-pass filter, below ~100 Hz) is a setting of its own: `AudioConfig::highPassFilter`, a check box in the demo,
  `--hpf` in the loopback example (off by default). It no longer needs the echo canceller / noise suppression to be switched on.
- Android: Bluetooth headphones are listed twice: "(Bluetooth handsfree)" = call quality with the headset's microphone, and
  "(Bluetooth high quality, phone microphone)" = A2DP media, no call mode, the phone's own microphone.
- Android: `AudioConfig::androidCallMode = false` runs without call mode (plain media streams), so the phone's own notification
  sounds keep working. Default stays on.
- Microphone and speaker are opened on a device worker (no UI freeze, no multi-second Start on Android).
- `Player` accepts packets even if the first speaker open failed (no silent playback after Stop / Start).
- Linux: a speaker "monitor" is never used as a microphone; "no microphone" is an error, not noise.

### Fixed
- Singing / speaking along with the music while "hear music" (the local monitor) is on: the listeners heard the voice late against the
  music, because the singer follows the delayed local sound. The SENT music is now delayed by the same time
  (`MusicPlayer::SetStreamDelayMs`, automatic by default; set it by ear on Bluetooth).
- Crashes while switching the microphone / speaker during a call: one lock around device open / close / listing,
  `DeviceSwitcher::Busy()` instead of a flag with a gap, the chosen device id is no longer read and written by two threads
  at once.
- Android: no more restore loops after a phone call, fresh Bluetooth microphone id for every open.
