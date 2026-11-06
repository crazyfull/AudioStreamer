# libRavAva - the cross-platform audio library of VoiceStream (Opus over UDP, jitter buffer, echo cancel, device handling).
#
# Standalone CMake project (the Qt app is built by VoiceStream.pro and does not need this file):
#
#   cmake -S src/ravava -B build -DCMAKE_BUILD_TYPE=Release
#   cmake --build build
#   cmake --install build --prefix /some/prefix        # lib/libRavAva.a  +  include/ravava/*.h  +  lib/cmake/RavAva
#
# miniaudio (../../miniaudio) is compiled INTO the library: nothing to build separately.
#
# Options (-DNAME=value):
#   RAVAVA_BUILD_SHARED      OFF  OFF = static library (default), ON = shared library
#   RAVAVA_WITH_WEBRTC       AUTO AUTO = use libwebrtc-audio-processing-1 if it is found, ON = required, OFF = never
#                                 (found = echo cancellation / noise suppression / AGC are compiled in, RAVAVA_WITH_WEBRTC is defined)
#   RAVAVA_OPUS_ROOT         ""   where libopus is (<root>/include[/opus]/opus.h, <root>/lib[/<android abi>]/libopus.*)
#   RAVAVA_ENABLE_DRED       OFF  Opus 1.5+ Deep REDundancy; libopus must be built with --enable-dred (default libopus builds do NOT have it)
#   RAVAVA_WEBRTC_ROOT       ""   where webrtc-audio-processing 1.x is installed (otherwise pkg-config / system paths)
#   RAVAVA_INSTALL_OPUS_HEADERS ON  also put opus.h & co next to the ravava headers (OpusCodec.h includes "opus.h"), so the
#                                 installed include/ folder is complete
#   RAVAVA_STATIC_RUNTIME    OFF  MSVC only: /MT (/MTd) instead of /MD (/MDd)
#   RAVAVA_BUILD_TESTS       OFF  build the tests in ../../tests that need no sound card (ctest)
#
# Use it from another CMake project:
#   find_package(RavAva REQUIRED)            # CMAKE_PREFIX_PATH = the install prefix
#   target_link_libraries(myapp PRIVATE RavAva::RavAva)    # headers: #include <ravava/Player.h>
cmake_minimum_required(VERSION 3.16)
project(RavAva VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 99)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)      # a static libRavAva can be linked into a shared library / Android .so

option(RAVAVA_BUILD_SHARED "Build libRavAva as a shared library (default: static)" OFF)
option(RAVAVA_INSTALL_OPUS_HEADERS "Install the opus headers next to the ravava headers" ON)
option(RAVAVA_STATIC_RUNTIME "MSVC: link the C runtime statically (/MT)" OFF)
option(RAVAVA_BUILD_TESTS "Build the tests (no sound card needed)" OFF)
set(RAVAVA_WITH_WEBRTC "AUTO" CACHE STRING "Echo cancellation with webrtc-audio-processing 1.x: AUTO, ON or OFF")
set_property(CACHE RAVAVA_WITH_WEBRTC PROPERTY STRINGS AUTO ON OFF)
option(RAVAVA_ENABLE_DRED "Opus 1.5+ Deep REDundancy (AudioConfig::dredMs, Player::SetDredPlayback). libopus MUST be built with --enable-dred" OFF)
set(RAVAVA_OPUS_ROOT "" CACHE PATH "Opus install prefix")
set(RAVAVA_WEBRTC_ROOT "" CACHE PATH "webrtc-audio-processing 1.x install prefix")

if(MSVC AND RAVAVA_STATIC_RUNTIME)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

get_filename_component(RAVAVA_REPO "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(RAVAVA_MINIAUDIO_DIR "${RAVAVA_REPO}/miniaudio")
if(NOT EXISTS "${RAVAVA_MINIAUDIO_DIR}/miniaudio.c")
    message(FATAL_ERROR "miniaudio not found at ${RAVAVA_MINIAUDIO_DIR}")
endif()

find_package(Threads REQUIRED)

# ======================================================================================================== Opus (required)
set(RAVAVA_OPUS_LIBS "")
find_package(Opus CONFIG QUIET)                      # opus >= 1.4 installs a CMake package (Opus::opus)
if(TARGET Opus::opus AND NOT RAVAVA_OPUS_ROOT)
    set(RAVAVA_OPUS_LIBS Opus::opus)
    get_target_property(_opus_inc Opus::opus INTERFACE_INCLUDE_DIRECTORIES)
    set(RAVAVA_OPUS_INCLUDE_DIR "${_opus_inc}")
else()
    set(_opus_hints "")
    set(_opus_libhints "")
    # RAVAVA_OPUS_ROOT is a folder. A path to the library file (libopus.a) is accepted too: its folder and the one above are used
    if(RAVAVA_OPUS_ROOT AND EXISTS "${RAVAVA_OPUS_ROOT}" AND NOT IS_DIRECTORY "${RAVAVA_OPUS_ROOT}")
        get_filename_component(_opus_file_dir "${RAVAVA_OPUS_ROOT}" DIRECTORY)
        set(RAVAVA_OPUS_LIB "${RAVAVA_OPUS_ROOT}" CACHE FILEPATH "libopus" FORCE)
        set(RAVAVA_OPUS_ROOT "${_opus_file_dir}/.." CACHE PATH "Opus install prefix" FORCE)
    endif()
    if(RAVAVA_OPUS_ROOT)
        list(APPEND _opus_hints "${RAVAVA_OPUS_ROOT}")
        list(APPEND _opus_libhints "${RAVAVA_OPUS_ROOT}/lib/${ANDROID_ABI}" "${RAVAVA_OPUS_ROOT}/lib" "${RAVAVA_OPUS_ROOT}")
    endif()
    if(MSVC AND EXISTS "${RAVAVA_REPO}/libs/opus/win32")
        # the copy that is shipped with the repository (VoiceStream.pro uses it too)
        list(APPEND _opus_hints "${RAVAVA_REPO}/libs/opus/win32")
        if(CMAKE_SIZEOF_VOID_P EQUAL 8)
            set(_opus_winlib "${RAVAVA_REPO}/libs/opus/win32/libopus_x64.lib")
        else()
            set(_opus_winlib "${RAVAVA_REPO}/libs/opus/win32/libopus_x86.lib")
        endif()
    endif()
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND AND NOT RAVAVA_OPUS_ROOT)
        pkg_check_modules(PC_OPUS QUIET opus)
    endif()
    find_path(RAVAVA_OPUS_INCLUDE_DIR opus.h HINTS ${_opus_hints} ${PC_OPUS_INCLUDE_DIRS}
              PATH_SUFFIXES include/opus opus include
              NO_DEFAULT_PATH)
    find_path(RAVAVA_OPUS_INCLUDE_DIR opus.h PATH_SUFFIXES opus)
    if(_opus_winlib AND EXISTS "${_opus_winlib}")
        set(RAVAVA_OPUS_LIB "${_opus_winlib}")
    else()
        find_library(RAVAVA_OPUS_LIB NAMES opus libopus HINTS ${_opus_libhints} ${PC_OPUS_LIBRARY_DIRS} NO_DEFAULT_PATH)
        find_library(RAVAVA_OPUS_LIB NAMES opus libopus)
    endif()
    if(NOT RAVAVA_OPUS_INCLUDE_DIR OR NOT RAVAVA_OPUS_LIB)
        message(FATAL_ERROR "libopus not found (root='${RAVAVA_OPUS_ROOT}', header='${RAVAVA_OPUS_INCLUDE_DIR}', library='${RAVAVA_OPUS_LIB}').\n"
                            "Install it (libopus-dev) or pass -DRAVAVA_OPUS_ROOT=<prefix> where <prefix>/include/opus/opus.h and "
                            "<prefix>/lib/libopus.a exist, or give the files directly: -DRAVAVA_OPUS_INCLUDE_DIR=<folder with opus.h> "
                            "-DRAVAVA_OPUS_LIB=<path of libopus.a>.")
    endif()
    set(RAVAVA_OPUS_LIBS "${RAVAVA_OPUS_LIB}")
endif()
message(STATUS "RavAva: opus headers ${RAVAVA_OPUS_INCLUDE_DIR}, library ${RAVAVA_OPUS_LIBS}")

# ====================================================================================== webrtc-audio-processing (optional)
set(RAVAVA_WEBRTC_FOUND FALSE)
set(RAVAVA_WEBRTC_INCLUDES "")
set(RAVAVA_WEBRTC_LIBS "")
string(TOUPPER "${RAVAVA_WITH_WEBRTC}" _webrtc_mode)
if(NOT _webrtc_mode STREQUAL "OFF")
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND AND NOT RAVAVA_WEBRTC_ROOT)
        pkg_check_modules(RAVAVA_PC_WEBRTC QUIET IMPORTED_TARGET webrtc-audio-processing-1)
        if(RAVAVA_PC_WEBRTC_FOUND)
            set(RAVAVA_WEBRTC_FOUND TRUE)
            set(RAVAVA_WEBRTC_LIBS PkgConfig::RAVAVA_PC_WEBRTC)
            if(NOT RAVAVA_BUILD_SHARED)
                list(APPEND RAVAVA_WEBRTC_LIBS ${RAVAVA_PC_WEBRTC_STATIC_LDFLAGS})
            endif()
        endif()
    endif()
    if(NOT RAVAVA_WEBRTC_FOUND)
        set(_wr_hints "")
        if(RAVAVA_WEBRTC_ROOT)
            list(APPEND _wr_hints "${RAVAVA_WEBRTC_ROOT}")
        endif()
        if(MSVC AND EXISTS "${RAVAVA_REPO}/libs/webrtc/win32")
            list(APPEND _wr_hints "${RAVAVA_REPO}/libs/webrtc/win32")      # made by libs/webrtc/build_win.bat
        endif()
        find_path(RAVAVA_WEBRTC_INCLUDE_DIR modules/audio_processing/include/audio_processing.h
                  HINTS ${_wr_hints} PATH_SUFFIXES include/webrtc-audio-processing-1 include
                  NO_DEFAULT_PATH)
        find_path(RAVAVA_WEBRTC_INCLUDE_DIR modules/audio_processing/include/audio_processing.h
                  PATH_SUFFIXES include/webrtc-audio-processing-1 webrtc-audio-processing-1)
        if(MSVC)
            if(CMAKE_SIZEOF_VOID_P EQUAL 8)
                set(_wr_names webrtc_apm_x64 libwebrtc-audio-processing-1)
                set(_wr_dnames webrtc_apm_x64d)
            else()
                set(_wr_names webrtc_apm_x86 libwebrtc-audio-processing-1)
                set(_wr_dnames webrtc_apm_x86d)
            endif()
        else()
            set(_wr_names webrtc-audio-processing-1)
            set(_wr_dnames "")
        endif()
        find_library(RAVAVA_WEBRTC_LIB NAMES ${_wr_names} HINTS ${_wr_hints}
                     PATH_SUFFIXES lib lib/x86_64-linux-gnu lib/aarch64-linux-gnu NO_DEFAULT_PATH)
        find_library(RAVAVA_WEBRTC_LIB NAMES ${_wr_names})
        if(_wr_dnames)
            find_library(RAVAVA_WEBRTC_LIB_DEBUG NAMES ${_wr_dnames} HINTS ${_wr_hints} PATH_SUFFIXES lib NO_DEFAULT_PATH)
        endif()
        find_library(RAVAVA_WEBRTC_CODING_LIB NAMES webrtc-audio-coding-1 HINTS ${_wr_hints}
                     PATH_SUFFIXES lib lib/x86_64-linux-gnu lib/aarch64-linux-gnu NO_DEFAULT_PATH)
        if(RAVAVA_WEBRTC_INCLUDE_DIR AND RAVAVA_WEBRTC_LIB)
            set(RAVAVA_WEBRTC_FOUND TRUE)
            set(RAVAVA_WEBRTC_INCLUDES "${RAVAVA_WEBRTC_INCLUDE_DIR}")
            if(RAVAVA_WEBRTC_LIB_DEBUG)
                list(APPEND RAVAVA_WEBRTC_LIBS optimized "${RAVAVA_WEBRTC_LIB}" debug "${RAVAVA_WEBRTC_LIB_DEBUG}")
            else()
                list(APPEND RAVAVA_WEBRTC_LIBS "${RAVAVA_WEBRTC_LIB}")
            endif()
            if(RAVAVA_WEBRTC_CODING_LIB)
                list(APPEND RAVAVA_WEBRTC_LIBS "${RAVAVA_WEBRTC_CODING_LIB}")
            endif()
            if(MSVC)
                list(APPEND RAVAVA_WEBRTC_LIBS winmm dbghelp advapi32)      # used by webrtc / abseil
            endif()
        endif()
    endif()
    if(NOT RAVAVA_WEBRTC_FOUND AND _webrtc_mode STREQUAL "ON")
        message(FATAL_ERROR "RAVAVA_WITH_WEBRTC=ON but webrtc-audio-processing 1.x was not found "
                            "(libwebrtc-audio-processing-1-dev, or -DRAVAVA_WEBRTC_ROOT=<prefix>, see libs/webrtc/README.md)")
    endif()
endif()
if(RAVAVA_WEBRTC_FOUND)
    message(STATUS "RavAva: webrtc-audio-processing FOUND -> echo cancellation, noise suppression and AGC are enabled")
else()
    message(STATUS "RavAva: webrtc-audio-processing not used -> echo / noise / AGC controls are off")
endif()

# ======================================================================================================== the library
set(RAVAVA_SOURCES
    AudioProcessor.cpp
    AudioDevice.cpp
    Player.cpp
    MusicPlayer.cpp
    ConferencePlayer.cpp
    Recorder.cpp
    JitterBuffer.cpp
    OpusCodec.cpp
    net/PacketRouter.cpp
    "${RAVAVA_MINIAUDIO_DIR}/miniaudio.c")

set(RAVAVA_PUBLIC_HEADERS
    AudioConfig.h AudioDevice.h AudioProcessor.h AudioTypes.h BlockCutter.h DeviceSwitcher.h DeviceWatchdog.h GainRamp.h
    ConferencePlayer.h JitterBuffer.h LevelMeter.h MusicPlayer.h OpusCodec.h PacketQueue.h Player.h Recorder.h VoicePacket.h)
set(RAVAVA_NET_HEADERS net/Packet.h net/PacketRouter.h)
set(RAVAVA_PLATFORM_HEADERS "")

if(ANDROID)
    list(APPEND RAVAVA_SOURCES platform/AndroidAudioRoute.cpp)
    list(APPEND RAVAVA_PLATFORM_HEADERS platform/AndroidAudioRoute.h)
endif()

if(IOS AND RAVAVA_BUILD_SHARED)
    message(FATAL_ERROR "RavAva on iOS is a static library (RAVAVA_BUILD_SHARED=OFF); link it into your app")
endif()
if(IOS)
    # miniaudio's Core Audio backend uses AVAudioSession (Objective-C) on iOS: this one C file is compiled as Objective-C
    set_source_files_properties("${RAVAVA_MINIAUDIO_DIR}/miniaudio.c" PROPERTIES COMPILE_OPTIONS "-x;objective-c")
endif()

if(RAVAVA_BUILD_SHARED)
    add_library(RavAva SHARED ${RAVAVA_SOURCES})
    set_target_properties(RavAva PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
else()
    add_library(RavAva STATIC ${RAVAVA_SOURCES})
endif()
add_library(RavAva::RavAva ALIAS RavAva)
set_target_properties(RavAva PROPERTIES
    OUTPUT_NAME RavAva VERSION ${PROJECT_VERSION} SOVERSION ${PROJECT_VERSION_MAJOR}
    C_VISIBILITY_PRESET default CXX_VISIBILITY_PRESET default)

# The headers that are copied into <build>/include/ravava (and installed): "#include <ravava/Player.h>".
# The sources use relative includes, so the same tree also works for them.
set(RAVAVA_STAGE_INCLUDE "${CMAKE_CURRENT_BINARY_DIR}/include")
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/Version.h.in" "${RAVAVA_STAGE_INCLUDE}/ravava/Version.h" @ONLY)
set(_stage_cmds "")
set(_stage_outputs "")
foreach(h ${RAVAVA_PUBLIC_HEADERS} ${RAVAVA_NET_HEADERS} ${RAVAVA_PLATFORM_HEADERS})
    set(_out "${RAVAVA_STAGE_INCLUDE}/ravava/${h}")
    add_custom_command(OUTPUT "${_out}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/${h}" "${_out}"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${h}" VERBATIM)
    list(APPEND _stage_outputs "${_out}")
endforeach()
if(RAVAVA_INSTALL_OPUS_HEADERS AND RAVAVA_OPUS_INCLUDE_DIR)
    foreach(h opus.h opus_types.h opus_defines.h opus_multistream.h opus_projection.h)
        if(EXISTS "${RAVAVA_OPUS_INCLUDE_DIR}/${h}")
            set(_out "${RAVAVA_STAGE_INCLUDE}/ravava/${h}")
            add_custom_command(OUTPUT "${_out}"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "${RAVAVA_OPUS_INCLUDE_DIR}/${h}" "${_out}"
                DEPENDS "${RAVAVA_OPUS_INCLUDE_DIR}/${h}" VERBATIM)
            list(APPEND _stage_outputs "${_out}")
        endif()
    endforeach()
endif()
add_custom_target(RavAvaHeaders ALL DEPENDS ${_stage_outputs})
add_dependencies(RavAva RavAvaHeaders)

target_include_directories(RavAva
    PUBLIC  $<BUILD_INTERFACE:${RAVAVA_STAGE_INCLUDE}> $<INSTALL_INTERFACE:include>
    PRIVATE "${RAVAVA_MINIAUDIO_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}")
if(RAVAVA_OPUS_INCLUDE_DIR AND NOT TARGET Opus::opus)
    target_include_directories(RavAva PRIVATE "${RAVAVA_OPUS_INCLUDE_DIR}")      # opus.h for the sources
endif()

target_link_libraries(RavAva PUBLIC Threads::Threads ${RAVAVA_OPUS_LIBS})
if(RAVAVA_ENABLE_DRED)
    target_compile_definitions(RavAva PUBLIC RAVAVA_ENABLE_DRED)        # PUBLIC: OpusCodec.h's class layout depends on it
endif()
if(RAVAVA_WEBRTC_FOUND)
    target_compile_definitions(RavAva PRIVATE RAVAVA_WITH_WEBRTC)       # only AudioProcessor.cpp looks at it
    target_include_directories(RavAva PRIVATE ${RAVAVA_WEBRTC_INCLUDES} ${RAVAVA_PC_WEBRTC_INCLUDE_DIRS})
    target_link_libraries(RavAva PUBLIC ${RAVAVA_WEBRTC_LIBS})
endif()

# what miniaudio needs on each OS (it loads ALSA / Pulse / JACK / WASAPI / CoreAudio dynamically)
if(ANDROID)
    target_link_libraries(RavAva PUBLIC log dl m)
elseif(IOS)
    target_link_libraries(RavAva PUBLIC "-framework Foundation" "-framework CoreFoundation" "-framework AudioToolbox" "-framework AVFoundation")
elseif(APPLE)
    target_link_libraries(RavAva PUBLIC "-framework CoreFoundation" "-framework CoreAudio" "-framework AudioToolbox")
elseif(UNIX)
    target_link_libraries(RavAva PUBLIC ${CMAKE_DL_LIBS} m)
endif()
if(MSVC)
    target_compile_definitions(RavAva PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
endif()

# ======================================================================================================== install
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
install(TARGETS RavAva EXPORT RavAvaTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(DIRECTORY "${RAVAVA_STAGE_INCLUDE}/ravava" DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT RavAvaTargets NAMESPACE RavAva:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/RavAva)

# A static libRavAva needs its dependencies at the consumer's link step: the config file finds them again.
set(RAVAVA_CONFIG_DEPS "")
if(TARGET Opus::opus AND NOT RAVAVA_OPUS_ROOT)
    string(APPEND RAVAVA_CONFIG_DEPS "find_dependency(Opus CONFIG)\n")
endif()
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/RavAvaConfig.cmake.in" [=[
@PACKAGE_INIT@
include(CMakeFindDependencyMacro)
find_dependency(Threads)
@RAVAVA_CONFIG_DEPS@
include("${CMAKE_CURRENT_LIST_DIR}/RavAvaTargets.cmake")
check_required_components(RavAva)
]=])
configure_package_config_file("${CMAKE_CURRENT_BINARY_DIR}/RavAvaConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/RavAvaConfig.cmake" INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/RavAva)
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/RavAvaConfigVersion.cmake"
    VERSION ${PROJECT_VERSION} COMPATIBILITY SameMajorVersion)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/RavAvaConfig.cmake" "${CMAKE_CURRENT_BINARY_DIR}/RavAvaConfigVersion.cmake"
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/RavAva)

# ======================================================================================================== tests
if(RAVAVA_BUILD_TESTS)
    enable_testing()
    set(_t "${RAVAVA_REPO}/tests")
    # the tests replace the sound card (AudioDevice.cpp / miniaudio) by a fake device, so they compile the sources themselves
    set(_core JitterBuffer.cpp OpusCodec.cpp AudioProcessor.cpp MusicPlayer.cpp)
    function(ravava_test name)
        cmake_parse_arguments(T "" "" "SOURCES;STUB" ${ARGN})
        set(srcs "${_t}/${name}.cpp")
        foreach(s ${T_SOURCES})
            list(APPEND srcs "${CMAKE_CURRENT_SOURCE_DIR}/${s}")
        endforeach()
        if(T_STUB)
            list(APPEND srcs "${_t}/stub/${T_STUB}.cpp")
        endif()
        add_executable(${name} ${srcs})
        target_include_directories(${name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/.." "${RAVAVA_OPUS_INCLUDE_DIR}")
        target_link_libraries(${name} PRIVATE Threads::Threads ${RAVAVA_OPUS_LIBS})
        if(RAVAVA_ENABLE_DRED)
            target_compile_definitions(${name} PRIVATE RAVAVA_ENABLE_DRED)
        endif()
        if(RAVAVA_WEBRTC_FOUND)
            target_compile_definitions(${name} PRIVATE RAVAVA_WITH_WEBRTC)
            target_include_directories(${name} PRIVATE ${RAVAVA_WEBRTC_INCLUDES} ${RAVAVA_PC_WEBRTC_INCLUDE_DIRS})
            target_link_libraries(${name} PRIVATE ${RAVAVA_WEBRTC_LIBS})
        endif()
        add_test(NAME ${name} COMMAND ${name})
    endfunction()
    ravava_test(unit_test SOURCES JitterBuffer.cpp OpusCodec.cpp net/PacketRouter.cpp)
    ravava_test(hpf_test SOURCES AudioProcessor.cpp)                  # the low-cut filter (needs webrtc, skips itself without)
    ravava_test(noalloc_test STUB audio_device_stub
        SOURCES Recorder.cpp Player.cpp ConferencePlayer.cpp MusicPlayer.cpp JitterBuffer.cpp OpusCodec.cpp net/PacketRouter.cpp)   # defines its own AudioProcessor
    ravava_test(net_sim_test SOURCES JitterBuffer.cpp OpusCodec.cpp)
    ravava_test(player_sim_test STUB audio_device_stub SOURCES Player.cpp ${_core})      # real time, ~75 s
    ravava_test(callback_time_test STUB audio_device_stub SOURCES Player.cpp ${_core})   # real time, ~30 s
    add_test(NAME net_sim_legacy COMMAND net_sim_test legacy --check)
    add_test(NAME net_sim_timed  COMMAND net_sim_test timed --check)
    add_test(NAME net_sim_timed_speech COMMAND net_sim_test timed speech --check)
    add_test(NAME net_sim_legacy_speech COMMAND net_sim_test legacy speech --check)
    add_test(NAME net_sim_compare        COMMAND net_sim_test compare)           # adaptive may not be much worse than legacy anywhere
    add_test(NAME net_sim_compare_speech COMMAND net_sim_test compare speech)
    add_test(NAME net_sim_compare_dtx    COMMAND net_sim_test compare dtx)
    ravava_test(volume_test STUB audio_device_stub SOURCES Player.cpp ${_core})
    ravava_test(format_test STUB audio_device_stub SOURCES Player.cpp ${_core})
    ravava_test(mic_gain_test STUB audio_device_stub SOURCES Recorder.cpp MusicPlayer.cpp OpusCodec.cpp AudioProcessor.cpp)
    ravava_test(conference_test STUB audio_device_stub SOURCES ConferencePlayer.cpp Player.cpp ${_core})      # real time, ~20 s
    ravava_test(music_monitor_test STUB audio_device_stub SOURCES Recorder.cpp Player.cpp MusicPlayer.cpp JitterBuffer.cpp OpusCodec.cpp AudioProcessor.cpp)
endif()
