# RavAva on iOS

Built by the **Build iOS** workflow (`.github/workflows/build-ios.yml`): `RavAva.xcframework` (device arm64 + simulator arm64,
Opus inside, no echo cancellation). It proves the library compiles and links; the audio itself has to be tried on an iPhone.

## Use it in an app
1. Drag `RavAva.xcframework` into the Xcode target ("Frameworks, Libraries"). Headers are inside (`#include <ravava/Recorder.h>`).
2. Link `AVFoundation`, `AudioToolbox`, `Foundation`, `CoreFoundation` (Xcode adds them when asked for the symbols).
3. `Info.plist`: `NSMicrophoneUsageDescription` (the app is killed on the first microphone use without it).
4. Ask for the microphone permission (`AVAudioApplication.requestRecordPermission`) BEFORE `Recorder::Start()`.
5. The audio session is set by the library: play-and-record, speaker (not the earpiece), Bluetooth headsets allowed.
   Call it from Swift through an Objective-C++ (`.mm`) file: RavAva is C++.
6. Optional: "Audio, AirPlay, and Picture in Picture" background mode so a call survives the screen lock.

## Not there yet
- Echo cancellation: webrtc is not built for iOS. Use a headset, or add the system voice-processing unit later.
- Interruptions (a phone call, Siri) are not handled: after one, `Recorder::KeepDevice()` / `Player::KeepDevice()` reopen the devices.
