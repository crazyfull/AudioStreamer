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

#include <jni.h>
#include <cstdint>
#include <string>
#include <vector>
#include "../AudioDevice.h"

namespace ravava {
namespace android {

// Give the library the Java VM and the application / activity Context. Call it once at start-up from the GUI layer
// (Qt: QJniEnvironment::javaVM() and QNativeInterface::QAndroidApplication::context()), BEFORE the first device list.
// Without it the library tries to find them itself (JNI_GetCreatedJavaVMs, ActivityThread.currentApplication), which
// some phones don't allow: then no device is listed at all (everything is logged to logcat, tag "ravava").
void Init(JavaVM *vm, jobject context);

std::vector<AudioDeviceInfo> ListDevices(bool capture);

// Prepares the phone for a stream on `deviceId` ("" = system default) and tells how to open it.
//   capture         : true for the microphone, false for the speaker
//   *aaudioId       : value for AAudioStreamBuilder_setDeviceId (0 = let the system choose). For a Bluetooth handsfree
//                     headset this is the id of ITS microphone / speaker (found after the SCO link is up, waits up to
//                     ~3 s), so the stream is pinned to the headset and can't fall back to the phone's mic.
//   *voicePreset    : true -> open the stream with the voice-communication usage / input preset
//   *routeHeld      : true -> this stream holds the Bluetooth route: call ReleaseStream(true) when it is closed
// While a Bluetooth route is active, "System default" follows it (so mic and speaker never end up on different devices).
void PrepareStream(const std::string &deviceId, bool capture, int32_t *aaudioId, bool *voicePreset, bool *routeHeld);
void ReleaseStream(bool routeHeld);

// What Android really uses right now, e.g. "Bluetooth headset (Bluetooth handsfree)" or "Phone microphone"; "" if unknown.
// Microphone: the recording configuration of this app (API 28+). Speaker: the communication device while a Bluetooth
// route is active (API 31+). Any thread except the audio thread (JNI calls).
std::string DescribeActiveDevice(bool capture);

} // namespace android
} // namespace ravava

#endif // RAVAVA_ANDROIDAUDIOROUTE_H
