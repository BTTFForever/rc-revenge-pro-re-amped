# The launcher (launcher/main.cpp): standalone, no game code, only SDL3. Built on its own:
#   cmake -S launcher -B build-launcher -G Ninja && ninja -C build-launcher
cmake_minimum_required(VERSION 3.21)
project(rc_launcher CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()
find_package(SDL3 CONFIG REQUIRED COMPONENTS SDL3-shared)
add_executable(rc-launcher main.cpp disc_image.cpp)
target_link_libraries(rc-launcher PRIVATE SDL3::SDL3)
target_compile_options(rc-launcher PRIVATE -Wall -Wextra)
if(WIN32)
    # A windowed program (no console window); --disc's messages still reach a console it was started from.
    set_target_properties(rc-launcher PROPERTIES WIN32_EXECUTABLE ON)
endif()
if(WIN32)
    target_link_libraries(rc-launcher PRIVATE shell32) # CommandLineToArgvW
endif()
