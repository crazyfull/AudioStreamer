# Loopback CLI: microphone -> Opus -> (simulated network) -> jitter buffer -> speaker, 48000 Hz mono fullband.
#   cmake -S example/loopback -B build-loopback && cmake --build build-loopback
#   ./build-loopback/ravava_loopback --help
# By default libRavAva is built from this repository (../../src/ravava); with -DRAVAVA_USE_INSTALLED=ON an installed copy
# is used instead (find_package(RavAva), -DCMAKE_PREFIX_PATH=<prefix>). The RAVAVA_* options of src/ravava/CMakeLists.txt
# (RAVAVA_WITH_WEBRTC, RAVAVA_OPUS_ROOT, ...) can be passed here too.
cmake_minimum_required(VERSION 3.16)
project(ravava_loopback CXX)

option(RAVAVA_USE_INSTALLED "Use an installed libRavAva (find_package) instead of building it from ../../src/ravava" OFF)
if(RAVAVA_USE_INSTALLED)
    find_package(RavAva REQUIRED)
else()
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../src/ravava" ravava)
endif()

add_executable(ravava_loopback main.cpp)
target_compile_features(ravava_loopback PRIVATE cxx_std_17)
target_link_libraries(ravava_loopback PRIVATE RavAva::RavAva)
