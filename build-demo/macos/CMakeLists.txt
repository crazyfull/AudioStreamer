# RavAva demo for macOS (a .app bundle), statically linked: Qt (static, from AllanChain/install-qt-static), Opus (static, built by the
# workflow). Nothing has to be installed on the tester's Mac. Built per CPU (arm64 / x86_64); the workflow also joins them into a
# universal app. The sources are in the repository root, so this file can live here:
#   qt-cmake -S build-demo/macos -B build-macos -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64 -DOPUS_ROOT=/path/to/opus-install
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

# --------------------------------------------------
# Qt (static)
# --------------------------------------------------

find_package(Qt6 6.12 REQUIRED COMPONENTS Core Gui Widgets Network)

# --------------------------------------------------
# Opus (static): -DOPUS_ROOT=<install prefix with include/opus/opus.h and lib/libopus.a>
# --------------------------------------------------

set(OPUS_ROOT "" CACHE PATH "Opus install prefix (include/opus, lib/libopus.a)")

find_path(OPUS_INCLUDE_DIR opus.h
    HINTS "${OPUS_ROOT}/include/opus" "${OPUS_ROOT}/include"
    NO_DEFAULT_PATH
)
find_library(OPUS_LIBRARY NAMES libopus.a opus
    HINTS "${OPUS_ROOT}/lib"
    NO_DEFAULT_PATH
)
if(NOT OPUS_INCLUDE_DIR OR NOT OPUS_LIBRARY)
    message(FATAL_ERROR "Opus not found under OPUS_ROOT='${OPUS_ROOT}' (include: ${OPUS_INCLUDE_DIR}, lib: ${OPUS_LIBRARY})")
endif()

# --------------------------------------------------
# Sources (the same list as build-demo/windows)
# --------------------------------------------------

set(RAVAVA_SOURCES
    ${REPO_ROOT}/main.cpp
    ${REPO_ROOT}/mainwindow.cpp

    ${REPO_ROOT}/miniaudio/miniaudio.c

    ${REPO_ROOT}/src/ravava/AudioProcessor.cpp
    ${REPO_ROOT}/src/ravava/AudioDevice.cpp
    ${REPO_ROOT}/src/ravava/Player.cpp
    ${REPO_ROOT}/src/ravava/MusicPlayer.cpp
    ${REPO_ROOT}/src/ravava/Recorder.cpp
    ${REPO_ROOT}/src/ravava/JitterBuffer.cpp
    ${REPO_ROOT}/src/ravava/net/PacketRouter.cpp
    ${REPO_ROOT}/src/ravava/OpusCodec.cpp
)

set(RAVAVA_HEADERS
    ${REPO_ROOT}/mainwindow.h

    ${REPO_ROOT}/src/ravava/AudioConfig.h
    ${REPO_ROOT}/src/ravava/AudioProcessor.h
    ${REPO_ROOT}/src/ravava/AudioTypes.h
    ${REPO_ROOT}/src/ravava/AudioDevice.h
    ${REPO_ROOT}/src/ravava/BlockCutter.h
    ${REPO_ROOT}/src/ravava/Player.h
    ${REPO_ROOT}/src/ravava/MusicPlayer.h
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
)

# --------------------------------------------------
# App bundle. Info.plist carries NSMicrophoneUsageDescription: without it macOS silently gives
# the app no microphone (and never shows the permission dialog).
# --------------------------------------------------

qt_add_executable(RavAva_Demo MACOSX_BUNDLE
    ${RAVAVA_SOURCES}
    ${RAVAVA_HEADERS}
    ${REPO_ROOT}/mainwindow.ui
    ${REPO_ROOT}/resource.qrc
)

set_target_properties(RavAva_Demo PROPERTIES
    OUTPUT_NAME "RavAva_Demo"
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_LIST_DIR}/Info.plist.in"
    MACOSX_BUNDLE_GUI_IDENTIFIER "com.crazyfull.ravava.demo"
    MACOSX_BUNDLE_BUNDLE_NAME "RavAva Demo"
    MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/out"
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
    # used by miniaudio (Core Audio backend)
    "-framework CoreFoundation"
    "-framework CoreAudio"
    "-framework AudioToolbox"
    "-framework AudioUnit"
)

message(STATUS "RavAva Demo (macOS) configuration:")
message(STATUS "  Architectures: ${CMAKE_OSX_ARCHITECTURES}")
message(STATUS "  Qt: static ${Qt6_VERSION}")
message(STATUS "  Opus: ${OPUS_LIBRARY}")
message(STATUS "  WebRTC: OFF")
