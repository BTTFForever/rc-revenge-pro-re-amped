# ico-pc's render hardware interface, Vulkan backend only (VENDORED.md). Written for RC; upstream's own
# port/rhi/CMakeLists.txt also builds the D3D12, D3D11 and OpenGL backends and finds its dependencies under
# ico-pc's tools/toolchain/deps.
#
# Targets:
#   rc_ico_rhi_vulkan  the Vulkan backend (vk/ and the shared helpers), static C library
#   rc_ico_rhi         the rhi.h entry points (rhi_backend.c) forwarded to it; link this one
#
# Dependencies:
#   Vulkan-Headers and volk: FetchContent at the same pins as third_party/port_engine (the backend loads
#     libvulkan at run time through volk, so nothing links a Vulkan library). Targets or variables already made
#     by the including project are used as they are: RC_VULKAN_INCLUDE_DIR, RC_VOLK_DIR; RC_VOLK_TARGET names a volk
#     library target the including project already builds (the game: third_party/port_engine's volk::volk), so
#     the program has one volk and volk.c is not compiled here.
#   SDL3: the window surface (SDL_Vulkan_CreateSurface), window size, mutexes. A system SDL3 (find_package)
#     when there is one, else FetchContent at RC_SDL3_TAG. RC_RHI_FETCH_SDL3=ON skips the system search.
cmake_minimum_required(VERSION 3.21)

include(FetchContent)

option(RC_RHI_FETCH_SDL3 "Always build SDL3 from source instead of using the system's" OFF)
set(RC_SDL3_TAG "release-3.4.20" CACHE STRING "SDL3 release tag fetched when no system SDL3 is found")

# --- Vulkan-Headers and volk ----------------------------------------------------------------------------------------
# Only the sources are needed: SOURCE_SUBDIR points at a folder that does not exist, so FetchContent downloads
# without adding either project's own CMake (volk.c is compiled into rc_ico_rhi, as ico-pc does).
if(NOT RC_VULKAN_INCLUDE_DIR)
    FetchContent_Declare(rc_vulkan_headers GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
                         GIT_TAG v1.4.363 GIT_SHALLOW TRUE SOURCE_SUBDIR rc-sources-only)
    FetchContent_MakeAvailable(rc_vulkan_headers)
    set(RC_VULKAN_INCLUDE_DIR "${rc_vulkan_headers_SOURCE_DIR}/include")
endif()
if(NOT RC_VOLK_DIR)
    FetchContent_Declare(rc_volk GIT_REPOSITORY https://github.com/zeux/volk.git GIT_TAG vulkan-sdk-1.4.363.0
                         GIT_SHALLOW TRUE SOURCE_SUBDIR rc-sources-only)
    FetchContent_MakeAvailable(rc_volk)
    set(RC_VOLK_DIR "${rc_volk_SOURCE_DIR}")
endif()

# --- SDL3 -----------------------------------------------------------------------------------------------------------
if(NOT TARGET SDL3::SDL3)
    if(NOT RC_RHI_FETCH_SDL3)
        find_package(SDL3 CONFIG QUIET COMPONENTS SDL3-shared)
    endif()
    if(SDL3_FOUND)
        message(STATUS "ico_rhi: system SDL3 ${SDL3_VERSION} (${SDL3_DIR})")
    else()
        message(STATUS "ico_rhi: building SDL3 ${RC_SDL3_TAG} from source")
        set(SDL_SHARED ON CACHE BOOL "" FORCE)
        set(SDL_STATIC OFF CACHE BOOL "" FORCE)
        set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
        set(SDL_TESTS OFF CACHE BOOL "" FORCE)
        set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(rc_sdl3 GIT_REPOSITORY https://github.com/libsdl-org/SDL.git GIT_TAG ${RC_SDL3_TAG}
                             GIT_SHALLOW TRUE)
        FetchContent_MakeAvailable(rc_sdl3)
    endif()
endif()

# --- rc_ico_rhi_vulkan: the backend (upstream's ico_rhi_vulkan) ----------------------------------------------------
set(_rc_rhi_vk_sources
    rhi_adapter.c rhi_bind_cache.c rhi_driver_version.c
    vk/vk_cmd.c vk/vk_device.c vk/vk_memory.c vk/vk_native.c vk/vk_pipeline.c vk/vk_present_mode.c
    vk/vk_renderpass.c vk/vk_resource.c vk/vk_swapchain.c vk/vk_sync.c)
if(RC_VOLK_TARGET)
    add_library(rc_ico_rhi_vulkan STATIC ${_rc_rhi_vk_sources})
    target_link_libraries(rc_ico_rhi_vulkan PRIVATE ${RC_VOLK_TARGET})
else()
    add_library(rc_ico_rhi_vulkan STATIC ${_rc_rhi_vk_sources} "${RC_VOLK_DIR}/volk.c")
endif()
target_include_directories(rc_ico_rhi_vulkan
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}"
    PRIVATE "${RC_VOLK_DIR}" "${RC_VULKAN_INCLUDE_DIR}")
# As upstream: the entry points' prefix (rhi_backend_names.h), volk loads every Vulkan entry point, SDL gives the
# surface.
target_compile_definitions(rc_ico_rhi_vulkan PRIVATE RHI_BACKEND_PREFIX=vk VK_NO_PROTOTYPES ICO_RHI_HAVE_SDL=1)
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    # As upstream: Debug builds ask for the validation layer even when RhiDeviceDesc.debugLayers is false;
    # ICO_VK_VALIDATION=0/1 overrides at run time.
    target_compile_definitions(rc_ico_rhi_vulkan PRIVATE ICO_RHI_DEBUG_DEFAULT=1)
endif()
find_package(Threads REQUIRED)
target_link_libraries(rc_ico_rhi_vulkan PUBLIC SDL3::SDL3 PRIVATE ${CMAKE_DL_LIBS} Threads::Threads m)

# --- rc_ico_rhi: the rhi.h entry points forwarded to the backend (upstream's ico_rhi) --------------------------------
add_library(rc_ico_rhi STATIC rhi_backend.c)
target_include_directories(rc_ico_rhi PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(rc_ico_rhi PRIVATE ICO_RHI_HAVE_VK=1)
target_link_libraries(rc_ico_rhi PUBLIC rc_ico_rhi_vulkan)

foreach(t rc_ico_rhi_vulkan rc_ico_rhi)
    set_target_properties(${t} PROPERTIES C_STANDARD 11 C_EXTENSIONS ON POSITION_INDEPENDENT_CODE ON)
    # ico-pc's ICO_SEMANTIC_OPTIONS (cmake/IcoFlags.cmake) and its warning set without -Werror (the compiler here
    # may be newer than upstream's)
    target_compile_options(${t} PRIVATE -fno-strict-aliasing -fwrapv -ffp-contract=off -fno-fast-math
                           -Wall -Wextra -Wno-unused-parameter)
endforeach()
