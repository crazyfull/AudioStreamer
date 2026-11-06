# Proves that an INSTALLED libRavAva is usable from another CMake project: find_package(RavAva), headers, Opus, link.
# cmake -S ci/consumer -B build -DCMAKE_PREFIX_PATH=<RavAva install prefix>
cmake_minimum_required(VERSION 3.16)
project(ravava_consumer CXX)
set(CMAKE_CXX_STANDARD 17)
find_package(RavAva 0.1 REQUIRED)
add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE RavAva::RavAva)
