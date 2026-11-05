# RavAva demo for Android: an APK per ABI (arm64-v8a, armeabi-v7a, x86_64, x86), Opus linked statically, webrtc-audio-processing
# (echo cancellation / noise suppression / AGC) optional and static. Qt is the normal Qt for Android kit (its libraries go into the APK).
# The sources are in the repository root, so this file can live here:
#   qt-cmake -S build-demo/android -B build-android -DCMAKE_BUILD_TYPE=Release -DQT_HOST_PATH=<Qt desktop kit> \
#            -DOPUS_ROOT=<opus install prefix for this ABI> [-DWEBRTC_ROOT=<libs/webrtc/build_android.sh output>]
#   cmake --build build-android --target apk
# (qt-cmake of the Qt for Android kit of ONE ABI; ANDROID_SDK_ROOT / ANDROID_NDK_ROOT in the environment.)
cmake_minimum_required(VERSION 3.21)

project(RavAva_Demo
    VERSION 1.0.0
    LANGUAGES C CXX
)

get_filename_component(REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC_SEARCH_PATHS "${REPO_ROOT}")

find_package(Qt6 6.2 REQUIRED COMPONENTS Core Gui Widgets Network)

# --------------------------------------------------
# Opus (static, for this ABI): -DOPUS_ROOT=<prefix with include/opus/opus.h and lib/libopus.a>
# --------------------------------------------------

set(OPUS_ROOT "" CACHE PATH "Opus install prefix for this ABI (include/opus, lib/libopus.a)")

find_path(OPUS_INCLUDE_DIR opus.h HINTS "${OPUS_ROOT}/include/opus" "${OPUS_ROOT}/include" NO_DEFAULT_PATH)
find_library(OPUS_LIBRARY NAMES libopus.a opus HINTS "${OPUS_ROOT}/lib" NO_DEFAULT_PATH)
if(NOT OPUS_INCLUDE_DIR OR NOT OPUS_LIBRARY)
    message(FATAL_ERROR "Opus not found under OPUS_ROOT='${OPUS_ROOT}' (include: ${OPUS_INCLUDE_DIR}, lib: ${OPUS_LIBRARY})")
endif()

# --------------------------------------------------
# webrtc-audio-processing 1.3 (optional): -DWEBRTC_ROOT=<output of libs/webrtc/build_android.sh>
# --------------------------------------------------

set(WEBRTC_ROOT "" CACHE PATH "libs/webrtc/build_android.sh output for this ABI (lib/libwebrtc_apm.a, include/)")
set(RAVAVA_WEBRTC OFF)
if(WEBRTC_ROOT AND EXISTS "${WEBRTC_ROOT}/lib/libwebrtc_apm.a" AND EXISTS "${WEBRTC_ROOT}/include/modules/audio_processing/include/audio_processing.h")
    set(RAVAVA_WEBRTC ON)
elseif(WEBRTC_ROOT)
    message(WARNING "WEBRTC_ROOT='${WEBRTC_ROOT}' has no lib/libwebrtc_apm.a + include/: building WITHOUT echo cancellation")
endif()

# --------------------------------------------------
# Sources (the same list as build-demo/windows, plus the Android parts)
# --------------------------------------------------

set(RAVAVA_SOURCES
    ${REPO_ROOT}/main.cpp
    ${REPO_ROOT}/mainwindow.cpp
    ${REPO_ROOT}/androidpermissions.cpp

    ${REPO_ROOT}/miniaudio/miniaudio.c

    ${REPO_ROOT}/src/ravava/AudioProcessor.cpp
    ${REPO_ROOT}/src/ravava/AudioDevice.cpp
    ${REPO_ROOT}/src/ravava/Player.cpp
    ${REPO_ROOT}/src/ravava/MusicPlayer.cpp
    ${REPO_ROOT}/src/ravava/ConferencePlayer.cpp
    ${REPO_ROOT}/src/ravava/Recorder.cpp
    ${REPO_ROOT}/src/ravava/JitterBuffer.cpp
    ${REPO_ROOT}/src/ravava/net/PacketRouter.cpp
    ${REPO_ROOT}/src/ravava/OpusCodec.cpp
    # device list + Bluetooth handsfree route (AudioManager through JNI)
    ${REPO_ROOT}/src/ravava/platform/AndroidAudioRoute.cpp
)

set(RAVAVA_HEADERS
    ${REPO_ROOT}/mainwindow.h
    ${REPO_ROOT}/androidpermissions.h

    ${REPO_ROOT}/src/ravava/AudioConfig.h
    ${REPO_ROOT}/src/ravava/AudioProcessor.h
    ${REPO_ROOT}/src/ravava/AudioTypes.h
    ${REPO_ROOT}/src/ravava/AudioDevice.h
    ${REPO_ROOT}/src/ravava/BlockCutter.h
    ${REPO_ROOT}/src/ravava/Player.h
    ${REPO_ROOT}/src/ravava/MusicPlayer.h
    ${REPO_ROOT}/src/ravava/ConferencePlayer.h
    ${REPO_ROOT}/src/ravava/Recorder.h
    ${REPO_ROOT}/src/ravava/DeviceWatchdog.h
    ${REPO_ROOT}/src/ravava/GainRamp.h
    ${REPO_ROOT}/src/ravava/JitterBuffer.h
    ${REPO_ROOT}/src/ravava/LevelMeter.h
    ${REPO_ROOT}/src/ravava/net/Packet.h
    ${REPO_ROOT}/src/ravava/net/PacketRouter.h
    ${REPO_ROOT}/src/ravava/OpusCodec.h
    ${REPO_ROOT}/src/ravava/PacketQueue.h
    ${REPO_ROOT}/src/ravava/VoicePacket.h
    ${REPO_ROOT}/src/ravava/platform/AndroidAudioRoute.h
)

qt_add_executable(RavAva_Demo
    ${RAVAVA_SOURCES}
    ${RAVAVA_HEADERS}
    ${REPO_ROOT}/mainwindow.ui
    ${REPO_ROOT}/resource.qrc
)

target_include_directories(RavAva_Demo PRIVATE
    "${REPO_ROOT}"
    "${REPO_ROOT}/miniaudio"
    "${OPUS_INCLUDE_DIR}"
)

target_link_libraries(RavAva_Demo PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Network
    "${OPUS_LIBRARY}"
    dl m log          # miniaudio (dlopen of AAudio / OpenSL ES), logging
)

if(RAVAVA_WEBRTC)
    target_compile_definitions(RavAva_Demo PRIVATE RAVAVA_WITH_WEBRTC)
    target_include_directories(RavAva_Demo PRIVATE "${WEBRTC_ROOT}/include")
    target_link_libraries(RavAva_Demo PRIVATE "${WEBRTC_ROOT}/lib/libwebrtc_apm.a")
endif()

# --------------------------------------------------
# APK: the manifest (permissions, the foreground service) and the Java service live in android/
# --------------------------------------------------

# The package folder (AndroidManifest.xml + Java) can be generated for the Qt in use: see make_android_package.py (the manifest of
# android/ is written for a recent Qt; Qt 6.4, needed for Android 6, has other mandatory entries).
set(ANDROID_PACKAGE_DIR "${REPO_ROOT}/android" CACHE PATH "folder with AndroidManifest.xml and src/ (Java)")
set(ANDROID_TARGET_SDK "34" CACHE STRING "targetSdkVersion")

set_target_properties(RavAva_Demo PROPERTIES
    QT_ANDROID_PACKAGE_SOURCE_DIR "${ANDROID_PACKAGE_DIR}"
    QT_ANDROID_TARGET_SDK_VERSION "${ANDROID_TARGET_SDK}"
    QT_ANDROID_VERSION_NAME "${PROJECT_VERSION}"
    QT_ANDROID_VERSION_CODE 1
)

message(STATUS "RavAva Demo (Android) configuration:")
message(STATUS "  ABI: ${ANDROID_ABI}")
message(STATUS "  Qt: ${Qt6_VERSION}")
message(STATUS "  Opus: ${OPUS_LIBRARY}")
message(STATUS "  WebRTC (echo cancellation): ${RAVAVA_WEBRTC}")
