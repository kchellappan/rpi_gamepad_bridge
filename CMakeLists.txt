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

add_library(gpb_core STATIC
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
target_include_directories(gpb_core PUBLIC include)
find_package(Threads REQUIRED)
target_link_libraries(gpb_core PUBLIC Threads::Threads)

add_executable(gpbridge src/main.cpp)
target_link_libraries(gpbridge PRIVATE gpb_core)

add_executable(gpb-discover tools/discover.cpp)
target_link_libraries(gpb-discover PRIVATE gpb_core)

# Synthetic gamepad for testing the evdev path without hardware or a human.
add_executable(gpb-fakepad tools/fake_pad.cpp)

# Loopback latency measurement: needs the OTG data leg plugged into the Pi's own USB-A port.
add_executable(gpb-latency tools/latency.cpp)

install(TARGETS gpbridge gpb-discover gpb-fakepad gpb-latency RUNTIME DESTINATION bin)
