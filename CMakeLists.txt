cmake_minimum_required(VERSION 3.16)
project(rpi_gamepad_bridge CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra -Wpedantic)
# No -ffast-math: the stick shaping is the only float work and it is not hot enough to
# justify relaxing FP semantics on a value that ends up quantized to 8 bits anyway.
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -g")

add_library(rgb_core STATIC
  src/config.cpp
  src/rt.cpp
  src/transform.cpp
  src/recorder.cpp
  src/bridge.cpp
  src/factory.cpp
  src/sources/evdev_source.cpp
  src/sources/socket_source.cpp
  src/sinks/ns_hid_sink.cpp
)
target_include_directories(rgb_core PUBLIC include)
find_package(Threads REQUIRED)
target_link_libraries(rgb_core PUBLIC Threads::Threads)

add_executable(rgb-bridge src/main.cpp)
target_link_libraries(rgb-bridge PRIVATE rgb_core)

add_executable(rgb-discover tools/discover.cpp)
target_link_libraries(rgb-discover PRIVATE rgb_core)

install(TARGETS rgb-bridge rgb-discover RUNTIME DESTINATION bin)
