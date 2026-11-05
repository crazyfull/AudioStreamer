cmake_minimum_required(VERSION 3.21)

cmake_policy(SET CMP0091 NEW)

project(RavAva_Demo
    VERSION 1.0.0
    LANGUAGES C CXX
)

# --------------------------------------------------
# Repo root (this file lives in build-demo/windows/)
# --------------------------------------------------
get_filename_component(RAVAVA_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

# --------------------------------------------------
# C / C++ standard
# --------------------------------------------------
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

set(CMAKE_AUTOUIC_SEARCH_PATHS
    ${RAVAVA_ROOT}
)

# Static MSVC runtime: /MT
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

# --------------------------------------------------
# Qt
# --------------------------------------------------
find_package(Qt6 6.12 REQUIRED COMPONENTS
    Core
    Gui
    Widgets
    Network
)

# --------------------------------------------------
# Opus
# --------------------------------------------------
set(OPUS_ROOT
    "${RAVAVA_ROOT}/libs/opus/win32"
)

set(OPUS_INCLUDE_DIR
    "${OPUS_ROOT}/include"
)

set(OPUS_LIBRARY
    "${OPUS_ROOT}/libopus_x64.lib"
)

if(NOT EXISTS "${OPUS_LIBRARY}")
    message(FATAL_ERROR "Opus x64 library not found: ${OPUS_LIBRARY}")
endif()

if(NOT EXISTS "${OPUS_INCLUDE_DIR}/opus.h")
    message(FATAL_ERROR "Opus headers not found: ${OPUS_INCLUDE_DIR}")
endif()

# --------------------------------------------------
# WebRTC (optional, built in CI)
# --------------------------------------------------
set(WEBRTC_ROOT "${RAVAVA_ROOT}/libs/webrtc/win32")
set(WEBRTC_LIB  "${WEBRTC_ROOT}/webrtc_apm_x64.lib")
set(WEBRTC_INC  "${WEBRTC_ROOT}/include")

set(RAVAVA_WITH_WEBRTC FALSE)
if(EXISTS "${WEBRTC_LIB}" AND EXISTS "${WEBRTC_INC}/modules/audio_processing/include/audio_processing.h")
    set(RAVAVA_WITH_WEBRTC TRUE)
    message(STATUS "WebRTC found -> echo cancellation / NS / AGC enabled")
else()
    message(STATUS "WebRTC not found -> building without echo cancellation")
endif()

# --------------------------------------------------
# Sources
# --------------------------------------------------
set(RAVAVA_SOURCES
    ${RAVAVA_ROOT}/main.cpp
    ${RAVAVA_ROOT}/mainwindow.cpp
    ${RAVAVA_ROOT}/miniaudio/miniaudio.c
    ${RAVAVA_ROOT}/src/ravava/AudioProcessor.cpp
    ${RAVAVA_ROOT}/src/ravava/AudioDevice.cpp
    ${RAVAVA_ROOT}/src/ravava/Player.cpp
    ${RAVAVA_ROOT}/src/ravava/MusicPlayer.cpp
    ${RAVAVA_ROOT}/src/ravava/ConferencePlayer.cpp
    ${RAVAVA_ROOT}/src/ravava/Recorder.cpp
    ${RAVAVA_ROOT}/src/ravava/JitterBuffer.cpp
    ${RAVAVA_ROOT}/src/ravava/net/PacketRouter.cpp
    ${RAVAVA_ROOT}/src/ravava/OpusCodec.cpp
)

set(RAVAVA_HEADERS
    ${RAVAVA_ROOT}/mainwindow.h
    ${RAVAVA_ROOT}/src/ravava/AudioConfig.h
    ${RAVAVA_ROOT}/src/ravava/AudioProcessor.h
    ${RAVAVA_ROOT}/src/ravava/AudioTypes.h
    ${RAVAVA_ROOT}/src/ravava/AudioDevice.h
    ${RAVAVA_ROOT}/src/ravava/BlockCutter.h
    ${RAVAVA_ROOT}/src/ravava/Player.h
    ${RAVAVA_ROOT}/src/ravava/MusicPlayer.h
    ${RAVAVA_ROOT}/src/ravava/ConferencePlayer.h
    ${RAVAVA_ROOT}/src/ravava/Recorder.h
    ${RAVAVA_ROOT}/src/ravava/DeviceWatchdog.h
    ${RAVAVA_ROOT}/src/ravava/GainRamp.h
    ${RAVAVA_ROOT}/src/ravava/JitterBuffer.h
    ${RAVAVA_ROOT}/src/ravava/LevelMeter.h
    ${RAVAVA_ROOT}/src/ravava/net/Packet.h
    ${RAVAVA_ROOT}/src/ravava/net/PacketRouter.h
    ${RAVAVA_ROOT}/src/ravava/OpusCodec.h
    ${RAVAVA_ROOT}/src/ravava/PacketQueue.h
    ${RAVAVA_ROOT}/src/ravava/VoicePacket.h
)

# --------------------------------------------------
# Qt executable
# --------------------------------------------------
qt_add_executable(RavAva_Demo WIN32
    ${RAVAVA_SOURCES}
    ${RAVAVA_HEADERS}
    ${RAVAVA_ROOT}/mainwindow.ui
    ${RAVAVA_ROOT}/resource.qrc
)

target_include_directories(RavAva_Demo PRIVATE
    "${RAVAVA_ROOT}/miniaudio"
    "${RAVAVA_ROOT}/src"
    "${OPUS_INCLUDE_DIR}"
)

if(RAVAVA_WITH_WEBRTC)
    target_include_directories(RavAva_Demo PRIVATE "${WEBRTC_INC}")
    target_compile_definitions(RavAva_Demo PRIVATE RAVAVA_WITH_WEBRTC)
endif()

target_link_libraries(RavAva_Demo PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Network
    "${OPUS_LIBRARY}"
    # Windows audio dependencies used by miniaudio
    ole32
    uuid
    winmm
    avrt
)

if(RAVAVA_WITH_WEBRTC)
    target_link_libraries(RavAva_Demo PRIVATE
        "${WEBRTC_LIB}"
        winmm
        dbghelp
        advapi32
    )
endif()

# --------------------------------------------------
# Compiler settings
# --------------------------------------------------
if(MSVC)
    target_compile_options(RavAva_Demo PRIVATE /utf-8)
    set_property(TARGET RavAva_Demo PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded"
    )
endif()

# --------------------------------------------------
# Output
# --------------------------------------------------
set_target_properties(RavAva_Demo PROPERTIES
    OUTPUT_NAME "RavAva_Demo"
    RUNTIME_OUTPUT_DIRECTORY        "${CMAKE_BINARY_DIR}/out"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/out"
)

message(STATUS "RavAva Demo configuration:")
message(STATUS "  Architecture: Windows x64")
message(STATUS "  Qt: Static Qt 6.12")
message(STATUS "  Opus: Static x64")
message(STATUS "  CRT: /MT")
if(RAVAVA_WITH_WEBRTC)
    message(STATUS "  WebRTC: ON")
else()
    message(STATUS "  WebRTC: OFF")
endif()
