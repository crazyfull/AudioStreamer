#ifndef ANDROIDPERMISSIONS_H
#define ANDROIDPERMISSIONS_H

// Android + Qt6 only (VoiceStream.pro adds androidpermissions.cpp for android). Asks the user, at run time, for the
// permissions the audio part needs:
//   RECORD_AUDIO       microphone
//   BLUETOOTH_CONNECT  Android 12+ (API 31): without it a Bluetooth handsfree headset is not listed and can't be selected
// Only what is missing is requested; nothing happens if everything is already granted. The answer arrives later: the
// device lists are refreshed on every Start, so the headset shows up on the next Start.
void RequestAndroidAudioPermissions();

// "PlayMusic" reads an audio file by its path: Android 13+ needs READ_MEDIA_AUDIO, older versions READ_EXTERNAL_STORAGE.
// Asked only when the user starts with PlayMusic checked (not at every start-up). Nothing happens if it is granted.
void RequestAndroidMusicPermission();

// Hands the Java VM and the Activity to the audio library (ravava::android::Init). Call it right after QApplication is
// created, BEFORE the first MainWindow: the constructor already lists the sound devices.
void InitAndroidAudio();

#endif // ANDROIDPERMISSIONS_H
