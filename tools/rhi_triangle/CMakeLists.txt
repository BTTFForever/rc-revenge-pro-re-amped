# rhi_triangle: a stand-alone check of the vendored ico-pc RHI (third_party/ico_rhi), renderer plan P0 step 2.
# Its own CMake project, so the game's build is not touched:
#   cmake -S tools/rhi_triangle -B build-rhi -DCMAKE_BUILD_TYPE=Debug && cmake --build build-rhi
#   tools/rhi_triangle/run.sh build-rhi        (on the discrete GPU, with the validation layer; see the script)
# The shaders are GLSL compiled to SPIR-V at build time by glslangValidator: the system's (pacman glslang, the
# Vulkan SDK) when found, else glslang built from source at RC_GLSLANG_TAG.
cmake_minimum_required(VERSION 3.21)
project(RCRhiTriangle C)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

include(FetchContent)
add_subdirectory(../../third_party/ico_rhi ico_rhi)

# --- glslangValidator ------------------------------------------------------------------------------------------------
option(RC_RHI_FETCH_GLSLANG "Always build glslang from source instead of using the system's glslangValidator" OFF)
set(RC_GLSLANG_TAG "vulkan-sdk-1.4.363.0" CACHE STRING "glslang tag built when no glslangValidator is found")
if(NOT RC_RHI_FETCH_GLSLANG)
    find_program(RC_GLSLANG_VALIDATOR glslangValidator)
endif()
if(RC_GLSLANG_VALIDATOR)
    message(STATUS "rhi_triangle: glslangValidator ${RC_GLSLANG_VALIDATOR}")
    set(_rc_glslang "${RC_GLSLANG_VALIDATOR}")
    set(_rc_glslang_dep "")
else()
    message(STATUS "rhi_triangle: building glslang ${RC_GLSLANG_TAG} from source")
    set(ENABLE_OPT OFF CACHE BOOL "" FORCE)
    set(GLSLANG_TESTS OFF CACHE BOOL "" FORCE)
    set(GLSLANG_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
    set(ENABLE_GLSLANG_BINARIES ON CACHE BOOL "" FORCE)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(rc_glslang GIT_REPOSITORY https://github.com/KhronosGroup/glslang.git GIT_TAG ${RC_GLSLANG_TAG}
                         GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(rc_glslang)
    set(_rc_glslang "$<TARGET_FILE:glslang-standalone>")
    set(_rc_glslang_dep glslang-standalone)
endif()

# Each shader becomes a C header with the SPIR-V words in an array (spv_<name>_<stage>).
set(_rc_shader_headers "")
foreach(stage vert frag)
    set(_src "${CMAKE_CURRENT_SOURCE_DIR}/triangle.${stage}")
    set(_out "${CMAKE_CURRENT_BINARY_DIR}/shaders/triangle_${stage}.h")
    add_custom_command(OUTPUT "${_out}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/shaders"
        COMMAND ${_rc_glslang} -V --target-env vulkan1.0 --vn spv_triangle_${stage} -o "${_out}" "${_src}"
        DEPENDS "${_src}" ${_rc_glslang_dep}
        COMMENT "glslangValidator triangle.${stage}"
        VERBATIM)
    list(APPEND _rc_shader_headers "${_out}")
endforeach()

add_executable(rhi_triangle rhi_triangle.c ${_rc_shader_headers})
target_include_directories(rhi_triangle PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/shaders")
set_target_properties(rhi_triangle PROPERTIES C_STANDARD 11 C_EXTENSIONS ON)
target_compile_options(rhi_triangle PRIVATE -Wall -Wextra)
target_link_libraries(rhi_triangle PRIVATE rc_ico_rhi m)
