cmake_minimum_required(VERSION 3.21)

cmake_policy(SET CMP0091 NEW)

project(RavAva_Demo
    VERSION 1.0.0
    LANGUAGES C CXX
)

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
    ${CMAKE_CURRENT_SOURCE_DIR}
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
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/opus/win32"
)

set(OPUS_INCLUDE_DIR
    "${OPUS_ROOT}/include"
)

set(OPUS_LIBRARY
    "${OPUS_ROOT}/libopus_x64.lib"
)

if(NOT EXISTS "${OPUS_LIBRARY}")
    message(FATAL_ERROR
        "Opus x64 library not found: ${OPUS_LIBRARY}"
    )
endif()

if(NOT EXISTS "${OPUS_INCLUDE_DIR}/opus.h")
    message(FATAL_ERROR
        "Opus headers not found: ${OPUS_INCLUDE_DIR}"
    )
endif()

# --------------------------------------------------
# Sources
# --------------------------------------------------

set(RAVAVA_SOURCES

    main.cpp
    mainwindow.cpp

    miniaudio/miniaudio.c

    src/ravava/AudioProcessor.cpp
    src/ravava/AudioDevice.cpp
    src/ravava/Player.cpp
    src/ravava/MusicPlayer.cpp
    src/ravava/ConferencePlayer.cpp
    src/ravava/Recorder.cpp
    src/ravava/JitterBuffer.cpp
    src/ravava/net/PacketRouter.cpp
    src/ravava/OpusCodec.cpp
)

# --------------------------------------------------
# Headers
# --------------------------------------------------

set(RAVAVA_HEADERS

    mainwindow.h

    src/ravava/AudioConfig.h
    src/ravava/AudioProcessor.h
    src/ravava/AudioTypes.h
    src/ravava/AudioDevice.h
    src/ravava/BlockCutter.h
    src/ravava/Player.h
    src/ravava/MusicPlayer.h
    src/ravava/ConferencePlayer.h
    src/ravava/Recorder.h
    src/ravava/DeviceWatchdog.h
    src/ravava/GainRamp.h
    src/ravava/JitterBuffer.h
    src/ravava/LevelMeter.h
    src/ravava/net/Packet.h
    src/ravava/net/PacketRouter.h
    src/ravava/OpusCodec.h
    src/ravava/PacketQueue.h
    src/ravava/VoicePacket.h
)

# --------------------------------------------------
# Qt executable
# --------------------------------------------------

qt_add_executable(RavAva_Demo WIN32

    ${RAVAVA_SOURCES}
    ${RAVAVA_HEADERS}

    mainwindow.ui
    resource.qrc
)

target_include_directories(RavAva_Demo PRIVATE

    "${CMAKE_CURRENT_SOURCE_DIR}/miniaudio"
    "${OPUS_INCLUDE_DIR}"
)

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

# --------------------------------------------------
# Compiler settings
# --------------------------------------------------

if(MSVC)

    target_compile_options(RavAva_Demo PRIVATE
        /utf-8
    )

    set_property(TARGET RavAva_Demo PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded"
    )

endif()

# --------------------------------------------------
# Output
# --------------------------------------------------

set_target_properties(RavAva_Demo PROPERTIES

    OUTPUT_NAME "RavAva_Demo"

    RUNTIME_OUTPUT_DIRECTORY
        "${CMAKE_BINARY_DIR}/out"

    RUNTIME_OUTPUT_DIRECTORY_RELEASE
        "${CMAKE_BINARY_DIR}/out"
)

message(STATUS "RavAva Demo configuration:")
message(STATUS "  Architecture: Windows x64")
message(STATUS "  Qt: Static Qt 6.12")
message(STATUS "  Opus: Static x64")
message(STATUS "  CRT: /MT")
message(STATUS "  WebRTC: OFF")
