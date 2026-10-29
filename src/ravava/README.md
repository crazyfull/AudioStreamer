# ravava — the audio library part of VoiceStream

Everything in this folder is the future **libRavAva**: Opus voice over any datagram transport, jitter buffer
(reorder / FEC / PLC), echo cancellation / noise suppression / AGC (optional webrtc), receive volume + level meter.

* No Qt, no sockets: it never opens a socket. The app hands it received datagrams and takes datagrams back.
* Everything lives in `namespace ravava`, including the network layer (`net/`: UDP envelope = packet type + stream id,
  `PacketRouter`). Not part of the library (stays in the app): `mainwindow.*`, `main.cpp` (UI, QUdpSocket).
* Build switch: `-DRAVAVA_WITH_WEBRTC` links the real echo canceller / noise suppressor / AGC, without it
  `AudioProcessor` is a pass-through (`AudioProcessor::IsAvailable()` tells which one you got).

## Layout

| File | What |
|---|---|
| `Recorder` | mic -> (AudioProcessor) -> Opus -> audio packet bodies |
| `Player` | audio packet bodies -> `JitterBuffer` -> Opus -> speaker; volume, level meter, stats |
| `JitterBuffer`, `OpusCodec`, `AudioProcessor` | the pieces the two above are made of |
| `AudioDevice` | `AudioInput` / `AudioOutput` (miniaudio), `DecodeAudioFile` |
| `AudioConfig`, `AudioTypes` | settings, `Bytes` / `Pcm`, `RAVAVA_WARN` / `RAVAVA_INFO` |
| `VoicePacket` | the audio packet body: 3 byte header (marker/tx, sequence) + one Opus packet |
| `net/Packet`, `net/PacketRouter` | network layer: UDP envelope [type:1][streamId:2] and the router that hands each payload to the handler of its type (audio never sees other types) |
| `PacketQueue`, `BlockCutter`, `LevelMeter` | small lock-free / allocation-free helpers for the audio thread |

## Naming standard (whole project)

| Thing | Style | Example |
|---|---|---|
| namespace | lower case | `ravava` |
| file | `PascalCase`, named after the main class in it (header + source pair) | `JitterBuffer.h/.cpp` |
| class, struct, enum, type alias | `PascalCase`, no prefix (no `cls`) | `Player`, `PacketInfo` |
| public function / method (also free functions) | `PascalCase` verb | `SetVolume()`, `GetStats()`, `IsReady()`, `DecodeAudioFile()` |
| private / internal method, lambda, local | `camelCase` | `adoptNextStream()`, `onDeviceData()` |
| data member | `m_` + camelCase | `m_frameSamples` |
| public data field of a settings/stat struct, constant-like enum value | camelCase field, `PascalCase` constant | `cfg.frameMs`, `Packet::Audio` |
| macro | `RAVAVA_UPPER_SNAKE` | `RAVAVA_WARN`, `RAVAVA_WITH_WEBRTC` |
| header guard | `RAVAVA_<FILE>_H` | `RAVAVA_PLAYER_H` |
| test executables / test stubs | `snake_case` | `unit_test.cpp`, `audio_device_stub.cpp` |

Boolean queries start with `Is` / `Has`; getters are `GetX()` (a method named like its own result type, e.g. `Stats()`
next to `struct Stats`, is not allowed in C++).

Real-time rule for everything the audio thread calls: no allocation, no lock shared with another thread, no I/O.

## Android: device list and Bluetooth handsfree (`platform/AndroidAudioRoute.*`)

miniaudio's AAudio / OpenSL ES backends cannot list devices, and Android uses the phone's own microphone even while a
Bluetooth handsfree headset is connected. On Android `AudioDevice.cpp` therefore asks `AudioManager` (plain JNI, Qt-free):

- `ListInputDevices()` / `ListOutputDevices()` list the built-in, wired, USB and Bluetooth devices. Ids: `aaudio:<n>`
  (the AAudio device id) and `btsco:<n>` (a Bluetooth handsfree headset).
- `aaudio:<n>` is passed to AAudio as the device id. `btsco:<n>` puts the phone in `MODE_IN_COMMUNICATION`, selects the
  headset (API 31+: `setCommunicationDevice`, older: `startBluetoothSco`) and opens the stream with the voice-communication
  usage / input preset; the route is undone when the stream is closed.
- Needs the permissions in `android/AndroidManifest.xml`; on Android 12+ `BLUETOOTH_CONNECT` (Settings > Apps > Permissions >
  "Nearby devices") must be granted or the headset is not listed (the app asks for it at start: `androidpermissions.cpp`, Qt 6).
- Logs go to logcat: `adb logcat -s ravava`.

On other platforms none of this is compiled (`RAVAVA_ANDROID` is only defined for `__ANDROID__`).
