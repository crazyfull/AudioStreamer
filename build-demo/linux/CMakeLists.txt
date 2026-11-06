# RavAva demo for Linux: Qt (shared, from install-qt-action), Opus + webrtc static from CI.
#   qt-cmake -S build-demo/linux -B build-linux \
#     -DCMAKE_BUILD_TYPE=Release \
#     -DOPUS_ROOT=/path/to/opus-install \
#     [-DWEBRTC_ROOT=/path/to/webrtc-install]
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

find_package(Qt6 6.4 REQUIRED COMPONENTS Core Gui Widgets Network)

# --------------------------------------------------
# Opus (static): -DOPUS_ROOT=<prefix>
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
    message(FATAL_ERROR "Opus not found under OPUS_ROOT='${OPUS_ROOT}'")
endif()

# --------------------------------------------------
# webrtc-audio-processing 1.3 (optional)
# --------------------------------------------------
set(WEBRTC_ROOT "" CACHE PATH "webrtc install (lib/libwebrtc_apm.a, include/)")
set(RAVAVA_WEBRTC OFF)
if(WEBRTC_ROOT AND EXISTS "${WEBRTC_ROOT}/lib/libwebrtc_apm.a"
   AND EXISTS "${WEBRTC_ROOT}/include/modules/audio_processing/include/audio_processing.h")
    set(RAVAVA_WEBRTC ON)
elseif(WEBRTC_ROOT)
    message(WARNING "WEBRTC_ROOT incomplete: building without echo cancellation")
endif()

set(RAVAVA_SOURCES
    ${REPO_ROOT}/main.cpp
    ${REPO_ROOT}/mainwindow.cpp
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
)

qt_add_executable(RavAva_Demo
    ${RAVAVA_SOURCES}
    ${RAVAVA_HEADERS}
    ${REPO_ROOT}/mainwindow.ui
    ${REPO_ROOT}/resource.qrc
)

set_target_properties(RavAva_Demo PROPERTIES
    OUTPUT_NAME "RavAva_Demo"
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
    pthread
    dl
    m
)

if(RAVAVA_WEBRTC)
    target_compile_definitions(RavAva_Demo PRIVATE RAVAVA_WITH_WEBRTC)
    target_include_directories(RavAva_Demo PRIVATE "${WEBRTC_ROOT}/include")
    target_link_libraries(RavAva_Demo PRIVATE "${WEBRTC_ROOT}/lib/libwebrtc_apm.a")
endif()

message(STATUS "RavAva Demo (Linux):")
message(STATUS "  Qt: ${Qt6_VERSION}")
message(STATUS "  Opus: ${OPUS_LIBRARY}")
message(STATUS "  WebRTC: ${RAVAVA_WEBRTC}")
