#ifndef RAVAVA_ANDROIDAUDIOROUTE_H
#define RAVAVA_ANDROIDAUDIOROUTE_H

// Android only (compiled when RAVAVA_ANDROID is defined, see AudioDevice.cpp). Qt-free: plain JNI.
//
// Why this exists: miniaudio's AAudio / OpenSL ES backends can NOT list devices (they only report "the default one"),
// and Android sends the microphone of a phone call-less app to the phone's own mic even when a Bluetooth handsfree
// headset is connected. Both are solved through android.media.AudioManager:
//   * ListDevices()  : AudioManager.getDevices() (+ the Bluetooth headsets that can be used for communication).
//   * Route          : for a normal device (speaker, wired, USB...) its AudioDeviceInfo id is handed to AAudio
//                      (AAudioStreamBuilder_setDeviceId). For a Bluetooth headset the phone has to be put in
//                      MODE_IN_COMMUNICATION and the headset selected as the communication device (API 31+:
//                      setCommunicationDevice, older: startBluetoothSco), after that Android routes the microphone
//                      AND the speaker of voice-communication streams to it.
//
// Device ids produced here (opaque strings for the rest of the library):
//   "aaudio:<n>"  AudioDeviceInfo.getId() = the AAudio device id n
//   "btsco:<n>"   Bluetooth headset (handsfree), n = AudioDeviceInfo.getId() (0 = any)
//
// Needed in AndroidManifest.xml: RECORD_AUDIO (already there), MODIFY_AUDIO_SETTINGS, and for Bluetooth BLUETOOTH_CONNECT
// (API 31+, runtime permission) / BLUETOOTH + BLUETOOTH_ADMIN (up to API 30).

#include <cstdint>
#include <string>
#include <vector>
#include "../AudioDevice.h"

namespace ravava {
namespace android {

std::vector<AudioDeviceInfo> ListDevices(bool capture);

// Prepares the phone for a stream on `deviceId` ("" = system default) and tells how to open it.
//   *aaudioId       : value for AAudioStreamBuilder_setDeviceId (0 = let the system choose)
//   *voiceCall      : true -> open the stream with the voice-communication usage / input preset (Bluetooth route)
// Must be paired with ReleaseStream() when that stream is closed. Never blocks for long, not for the audio thread.
void PrepareStream(const std::string &deviceId, int32_t *aaudioId, bool *voiceCall);
void ReleaseStream(bool voiceCall);

} // namespace android
} // namespace ravava

#endif // RAVAVA_ANDROIDAUDIOROUTE_H
