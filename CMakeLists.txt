# RC Revenge Pro (SLUS-20153, SLES-50077) built against the Shadow of the Colossus PS2Recomp fork
# (https://github.com/LightVelox/PS2Recomp @ d06a4ab plus local changes), vendored in third_party/ps2recomp.
# tools/setup.sh goes from a game disc to a build; see README.md.
cmake_minimum_required(VERSION 3.21)
project(RCRevengeProFork C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# The runtime (third_party/ps2recomp/VENDORED.md). A normal variable, so it overrides a PS2RECOMP_DIR left in an older cache.
set(PS2RECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ps2recomp")

# The runtime fetches raylib, imgui and rlImGui with FetchContent. RC_FETCH_CACHE can name a folder that already holds
# <name>-src checkouts (for example another build's _deps) to build offline.
set(RC_FETCH_CACHE "" CACHE PATH "Optional folder with raylib-src, imgui-src and rlimgui-src checkouts")
if(RC_FETCH_CACHE)
    foreach(dep raylib imgui rlimgui)
        string(TOUPPER ${dep} DEP)
        if(EXISTS "${RC_FETCH_CACHE}/${dep}-src")
            set(FETCHCONTENT_SOURCE_DIR_${DEP} "${RC_FETCH_CACHE}/${dep}-src" CACHE PATH "")
        endif()
    endforeach()
endif()

set(PS2X_ENABLE_AGRESSIVE_LOGS OFF CACHE BOOL "")
set(PS2X_ENABLE_SCCACHE OFF CACHE BOOL "")

# The fork targets AVX2. FMA stays off and contraction is disabled because the PS2's
# float results depend on each operation being rounded separately.
add_compile_options(-mavx2 -msse4.2 -ffp-contract=off)
# Clang applies type-based aliasing from -O1 up; recompiled code reads guest memory through casts.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    add_compile_options(-fno-strict-aliasing)
    add_link_options(-fuse-ld=lld) # The runtime is built with LTO, which GNU ld cannot read from Clang.
endif()

add_subdirectory("${PS2RECOMP_DIR}/ps2xIOP" ps2xIOP EXCLUDE_FROM_ALL)
add_subdirectory("${PS2RECOMP_DIR}/ps2xRuntime" ps2xRuntime EXCLUDE_FROM_ALL)

# Each EE thread runs on a fiber from ico-pc's platform layer.
add_subdirectory(third_party/ico_platform EXCLUDE_FROM_ALL)
target_link_libraries(ps2_runtime PUBLIC rc_ico_platform)
target_compile_definitions(ps2_runtime PUBLIC PS2X_EE_FIBERS=1)

# Which executable the build is for: usa (SLUS_201.53, game/usa) or eu (SLES_500.77, game/eu). Each version has its own
# recompiled code (output/, output_eu/) and its own address headers (decomp/generated/<version>/, tools/gen_addrs.py);
# the native code is the same source, compiled against them. See NOTES.md, "Europe version".
set(RC_VERSION "usa" CACHE STRING "Game version: usa or eu")
set_property(CACHE RC_VERSION PROPERTY STRINGS usa eu)
if(RC_VERSION STREQUAL "usa")
    set(RC_OUTPUT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/output")
    set(RC_ELF_NAME "SLUS_201.53")
    set(RC_VERSION_EU 0)
elseif(RC_VERSION STREQUAL "eu")
    set(RC_OUTPUT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/output_eu")
    set(RC_ELF_NAME "SLES_500.77")
    set(RC_VERSION_EU 1)
else()
    message(FATAL_ERROR "RC_VERSION must be usa or eu, not ${RC_VERSION}")
endif()
set(RC_GENERATED_VERSION_DIR "${CMAKE_CURRENT_SOURCE_DIR}/decomp/generated/${RC_VERSION}")
foreach(f rc_addrs.h rc_fns.h rc_globals.h rc_types.h rc_layout_check.cpp)
    if(NOT EXISTS "${RC_GENERATED_VERSION_DIR}/${f}")
        message(FATAL_ERROR "${RC_GENERATED_VERSION_DIR}/${f} is missing: run tools/setup.sh (README.md)")
    endif()
endforeach()

file(GLOB GAME_SOURCES CONFIGURE_DEPENDS "${RC_OUTPUT_DIR}/*.cpp")

add_executable(rc_revenge_pro
    ${GAME_SOURCES}
    "${PS2RECOMP_DIR}/ps2xRuntime/src/main.cpp"
    "${PS2RECOMP_DIR}/ps2xRuntime/src/lib/ps2_debug_panel.cpp"
)
target_include_directories(rc_revenge_pro PRIVATE "${RC_OUTPUT_DIR}")
target_link_libraries(rc_revenge_pro PRIVATE ps2_runtime rlImGui imgui)
target_compile_definitions(rc_revenge_pro PRIVATE PS2X_ENABLE_DEBUG_UI=1)
set_target_properties(rc_revenge_pro PROPERTIES UNITY_BUILD ON UNITY_BUILD_BATCH_SIZE 32)
# Set on the target: per-source options would exclude the files from unity builds.
set(RC_GAME_OPT "-O1" CACHE STRING "Optimization flags for the recompiled game code")
set(RC_VU1_OPT "-O2" CACHE STRING "Optimization flags for the recompiled VU1 microcode")
separate_arguments(RC_GAME_OPT_LIST UNIX_COMMAND "${RC_GAME_OPT}")
separate_arguments(RC_VU1_OPT_LIST UNIX_COMMAND "${RC_VU1_OPT}")
target_compile_options(rc_revenge_pro PRIVATE ${RC_GAME_OPT_LIST})

file(GLOB OVERRIDE_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/overrides/*.cpp")
target_sources(rc_revenge_pro PRIVATE ${OVERRIDE_SOURCES})
set_source_files_properties(${OVERRIDE_SOURCES} PROPERTIES SKIP_UNITY_BUILD_INCLUSION TRUE
    INCLUDE_DIRECTORIES "${RC_GENERATED_VERSION_DIR}")

# Decompiled game code. Its headers are generated from the game's debug data: see tools/stabs2c.py.
file(GLOB DECOMP_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/decomp/*.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/decomp/src/*.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/decomp/vu1/*.cpp")
list(APPEND DECOMP_SOURCES "${RC_GENERATED_VERSION_DIR}/rc_layout_check.cpp")
target_sources(rc_revenge_pro PRIVATE ${DECOMP_SOURCES})
set_source_files_properties(${DECOMP_SOURCES} PROPERTIES SKIP_UNITY_BUILD_INCLUSION TRUE
    COMPILE_OPTIONS "-O2;-fwrapv;-fno-strict-aliasing"
    INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/decomp;${RC_GENERATED_VERSION_DIR};${CMAKE_CURRENT_SOURCE_DIR}/decomp/generated;${CMAKE_CURRENT_SOURCE_DIR}/decomp/vu1;${PS2RECOMP_DIR}/ps2xRuntime/src/lib/vu")

# The version defines reach only the hand-written code (the recompiled code is per version already).
set_property(SOURCE ${OVERRIDE_SOURCES} ${DECOMP_SOURCES} "${CMAKE_CURRENT_SOURCE_DIR}/decomp/oracle/rc_oracle.cpp"
    APPEND PROPERTY COMPILE_DEFINITIONS RC_VERSION_EU=${RC_VERSION_EU} RC_ELF_NAME="${RC_ELF_NAME}")
# Side tags (render/gfx/tags.h, RENDERER_FRAME_RECORD.md section 5): on in the game's own build of the native and
# override code only. The same sources compiled into rc_oracle (or any other target) keep RC_GFX_TAGS 0, so every tag
# call compiles to nothing there.
set_property(SOURCE ${OVERRIDE_SOURCES} ${DECOMP_SOURCES}
    APPEND PROPERTY COMPILE_DEFINITIONS "$<$<STREQUAL:$<TARGET_PROPERTY:NAME>,rc_revenge_pro>:RC_GFX_TAGS=1>")

# The track shader is compiled into the game from the same file tools/shader_check.cpp tests.
set(RC_TRACK_SHADER "${CMAKE_CURRENT_SOURCE_DIR}/decomp/shaders/trdr_shape.vert")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${RC_TRACK_SHADER}")
file(READ "${RC_TRACK_SHADER}" RC_TRACK_SHADER_TEXT)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/generated/trdr_shape_glsl.h" "R\"RCGLSL(${RC_TRACK_SHADER_TEXT})RCGLSL\"\n")
set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/decomp/vu1/track_draw.cpp" APPEND PROPERTY INCLUDE_DIRECTORIES "${CMAKE_CURRENT_BINARY_DIR}/generated")

file(GLOB VU1_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/vu1/*.cpp")
if(VU1_SOURCES)
    add_library(rc_vu1_code STATIC ${VU1_SOURCES})
    target_include_directories(rc_vu1_code PRIVATE "${PS2RECOMP_DIR}/ps2xRuntime/src/lib/vu")
    target_link_libraries(rc_vu1_code PUBLIC ps2_runtime)
    target_compile_options(rc_vu1_code PRIVATE ${RC_VU1_OPT_LIST} -w)
    # Generated files only register themselves through static initializers.
    target_link_libraries(rc_revenge_pro PRIVATE -Wl,--whole-archive rc_vu1_code -Wl,--no-whole-archive)
endif()

add_subdirectory(third_party/port_engine EXCLUDE_FROM_ALL)
target_sources(rc_revenge_pro PRIVATE third_party/port_engine/rc_backend.cpp)
set_source_files_properties(third_party/port_engine/rc_backend.cpp PROPERTIES SKIP_UNITY_BUILD_INCLUSION TRUE COMPILE_OPTIONS "-w")
target_link_libraries(rc_revenge_pro PRIVATE rc_port_engine)

# RC_RENDERER=vk (renderer plan P0 step 3, NOTES.md "RC_RENDERER=vk"): an SDL3 window and input and the vendored RHI
# (third_party/ico_rhi, Vulkan) presenting, in place of the runtime's raylib window. platform/vk_host.cpp installs
# the runtime's host platform hooks only when RC_RENDERER=vk, so the default run is unchanged. The game only: the
# oracle neither links it nor installs hooks. The RHI uses port_engine's Vulkan-Headers and volk (one volk).
include(FetchContent)
FetchContent_GetProperties(vulkan_headers)
FetchContent_GetProperties(volk)
set(RC_VULKAN_INCLUDE_DIR "${vulkan_headers_SOURCE_DIR}/include")
set(RC_VOLK_DIR "${volk_SOURCE_DIR}")
set(RC_VOLK_TARGET volk::volk)
add_subdirectory(third_party/ico_rhi EXCLUDE_FROM_ALL)
set(RC_PLATFORM_SHADER_HEADERS "")
foreach(stage vert frag)
    set(_src "${CMAKE_CURRENT_SOURCE_DIR}/platform/present.${stage}")
    set(_out "${CMAKE_CURRENT_BINARY_DIR}/generated/present_${stage}.h")
    add_custom_command(OUTPUT "${_out}"
        COMMAND ${RC_GLSLANG} -V --target-env vulkan1.0 --vn spv_present_${stage} -o "${_out}" "${_src}"
        DEPENDS "${_src}" COMMENT "glslangValidator present.${stage}" VERBATIM)
    list(APPEND RC_PLATFORM_SHADER_HEADERS "${_out}")
endforeach()
target_sources(rc_revenge_pro PRIVATE platform/vk_host.cpp ${RC_PLATFORM_SHADER_HEADERS})
set_source_files_properties(platform/vk_host.cpp PROPERTIES SKIP_UNITY_BUILD_INCLUSION TRUE
    INCLUDE_DIRECTORIES "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(rc_revenge_pro PRIVATE rc_ico_rhi)

# The frame record (renderer plan P1 step 4, RENDERER_FRAME_RECORD.md; NOTES.md "Frame record"). rc_record: the format,
# page shadow, chain walker, writer and reader (needs only zlib); rc_record_emu: keyframe state <-> the runtime. The game
# links both and render/record/capture.cpp, which installs the runtime's capture hooks only when RC_FRAME_RECORD is set.
# The oracle links none of it. No usage requirements leak into the game's other sources (their compile lines stay
# as they were): include folders are set per source.
find_package(ZLIB REQUIRED)
set(RC_RECORD_DIR "${CMAKE_CURRENT_SOURCE_DIR}/render/record")
add_library(rc_record STATIC "${RC_RECORD_DIR}/page_tracker.cpp" "${RC_RECORD_DIR}/chain_walk.cpp" "${RC_RECORD_DIR}/writer.cpp"
    "${RC_RECORD_DIR}/reader.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/render/gfx/tags.cpp")
target_link_libraries(rc_record PRIVATE ZLIB::ZLIB)
target_compile_definitions(rc_record PRIVATE RC_GFX_TAGS=1)
target_compile_options(rc_record PRIVATE -O2)
add_library(rc_record_emu STATIC "${RC_RECORD_DIR}/emu_state.cpp")
target_link_libraries(rc_record_emu PRIVATE ps2_runtime)
target_compile_options(rc_record_emu PRIVATE -O2)
execute_process(COMMAND git describe --always --dirty WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    OUTPUT_VARIABLE RC_BUILD_DESCRIBE OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT RC_BUILD_DESCRIBE)
    set(RC_BUILD_DESCRIBE "unknown")
endif()
target_sources(rc_revenge_pro PRIVATE "${RC_RECORD_DIR}/capture.cpp")
set_source_files_properties("${RC_RECORD_DIR}/capture.cpp" PROPERTIES SKIP_UNITY_BUILD_INCLUSION TRUE COMPILE_OPTIONS "-O2"
    COMPILE_DEFINITIONS "RC_VERSION_EU=${RC_VERSION_EU};RC_ELF_NAME=\"${RC_ELF_NAME}\";RC_BUILD_DESCRIBE=\"${RC_BUILD_DESCRIBE}\";RC_GFX_TAGS=1")
target_link_libraries(rc_revenge_pro PRIVATE rc_record_emu rc_record)
# rc_replay (tools/rc_replay): reads dumps and feeds consumers; links the runtime and the VU1 library, no recompiled
# game code, no disc. ninja -C build-decomp rc_replay. rc_record_test: the container's round trip without the game.
add_executable(rc_replay EXCLUDE_FROM_ALL tools/rc_replay/rc_replay.cpp tools/rc_replay/emu.cpp)
target_include_directories(rc_replay PRIVATE "${RC_RECORD_DIR}")
target_link_libraries(rc_replay PRIVATE rc_record rc_record_emu ps2_runtime ZLIB::ZLIB)
if(TARGET rc_vu1_code)
    target_compile_definitions(rc_replay PRIVATE RC_HAVE_VU1_CODE=1)
    target_link_libraries(rc_replay PRIVATE -Wl,--whole-archive rc_vu1_code -Wl,--no-whole-archive)
endif()
# rc_tex (render/tex; renderer plan P1 step 5, RENDERER_FRAME_RECORD.md section 17): the host's shadow of GS texture
# memory, the GIF walker, texture decode and the texture cache. Linked by rc_replay (the tex consumer) and rc_tex_test
# only: nothing of it is in the game or the oracle yet (the live reader of steps 6-9 will link it).
set(RC_TEX_DIR "${CMAKE_CURRENT_SOURCE_DIR}/render/tex")
add_library(rc_tex STATIC EXCLUDE_FROM_ALL "${RC_TEX_DIR}/gs_layout.cpp" "${RC_TEX_DIR}/gs_memory.cpp" "${RC_TEX_DIR}/gif_walk.cpp"
    "${RC_TEX_DIR}/tex_decode.cpp" "${RC_TEX_DIR}/tex_cache.cpp" "${RC_TEX_DIR}/tex_shadow.cpp")
target_compile_options(rc_tex PRIVATE -O2)
target_link_libraries(rc_tex PRIVATE rc_record)
target_sources(rc_replay PRIVATE tools/rc_replay/tex.cpp)
target_link_libraries(rc_replay PRIVATE rc_tex)
# rc_blit (render/blit; renderer plan P1 step 6, RENDERER_FRAME_RECORD.md section 18): the displayed picture from uploads
# into the frame buffer and DISPFB2 (FrameBlit, CPU, on rc_tex), and rc_blit_vk, its GPU half on the vendored RHI
# (BlitPresenter: upload, draw with the presentation transform, read back). Linked by rc_replay (the blit consumer)
# only: nothing of it is in the game or the oracle yet.
set(RC_BLIT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/render/blit")
add_library(rc_blit STATIC EXCLUDE_FROM_ALL "${RC_BLIT_DIR}/frame_blit.cpp")
target_compile_options(rc_blit PRIVATE -O2)
target_link_libraries(rc_blit PRIVATE rc_tex rc_record)
set(RC_BLIT_SHADER_HEADERS "")
foreach(stage vert frag)
    set(_src "${RC_BLIT_DIR}/blit.${stage}")
    set(_out "${CMAKE_CURRENT_BINARY_DIR}/generated/blit_${stage}.h")
    add_custom_command(OUTPUT "${_out}"
        COMMAND ${RC_GLSLANG} -V --target-env vulkan1.0 --vn spv_blit_${stage} -o "${_out}" "${_src}"
        DEPENDS "${_src}" COMMENT "glslangValidator blit.${stage}" VERBATIM)
    list(APPEND RC_BLIT_SHADER_HEADERS "${_out}")
endforeach()
add_library(rc_blit_vk STATIC EXCLUDE_FROM_ALL "${RC_BLIT_DIR}/blit_presenter.cpp" ${RC_BLIT_SHADER_HEADERS})
target_include_directories(rc_blit_vk PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_compile_options(rc_blit_vk PRIVATE -O2)
target_link_libraries(rc_blit_vk PUBLIC rc_ico_rhi)
target_sources(rc_replay PRIVATE tools/rc_replay/blit.cpp tools/rc_replay/gif2d_survey.cpp)
target_link_libraries(rc_replay PRIVATE rc_blit rc_blit_vk rc_tex)
add_executable(rc_tex_test EXCLUDE_FROM_ALL tools/rc_replay/tex_test.cpp)
target_compile_options(rc_tex_test PRIVATE -O2)
target_link_libraries(rc_tex_test PRIVATE rc_tex rc_record ZLIB::ZLIB)
add_executable(rc_record_test EXCLUDE_FROM_ALL tools/rc_replay/record_test.cpp)
target_include_directories(rc_record_test PRIVATE "${RC_RECORD_DIR}")
target_link_libraries(rc_record_test PRIVATE rc_record)
foreach(t rc_record rc_record_emu)
    target_include_directories(${t} PRIVATE "${RC_RECORD_DIR}")
endforeach()
set_property(SOURCE "${RC_RECORD_DIR}/capture.cpp" APPEND PROPERTY INCLUDE_DIRECTORIES "${RC_RECORD_DIR}")

# Profile-guided build. RC_PGO=generate builds an instrumented executable; running it (a scripted game run, for
# example tools/play.sh replay) writes a profile, and RC_PGO=use rebuilds with it in the same build folder.
# Only the runtime and VU1 code are instrumented: they hold the hot paths, and the game code
# then compiles once for all modes.
set(RC_PGO "off" CACHE STRING "off, generate or use")
set(RC_PGO_TARGETS ps2_runtime ps2_iop)
if(TARGET rc_vu1_code)
    list(APPEND RC_PGO_TARGETS rc_vu1_code)
endif()
if(RC_PGO STREQUAL "generate")
    foreach(t ${RC_PGO_TARGETS})
        target_compile_options(${t} PRIVATE -fprofile-generate -fprofile-update=prefer-atomic)
    endforeach()
    target_link_options(rc_revenge_pro PRIVATE -fprofile-generate)
    set_property(SOURCE ${OVERRIDE_SOURCES} APPEND PROPERTY COMPILE_DEFINITIONS RC_PGO_GEN=1)
elseif(RC_PGO STREQUAL "use")
    foreach(t ${RC_PGO_TARGETS})
        target_compile_options(${t} PRIVATE -fprofile-use -fprofile-correction
            -Wno-missing-profile -Wno-coverage-mismatch)
    endforeach()
endif()

if(NOT TARGET vu1_recomp)
    add_executable(vu1_recomp EXCLUDE_FROM_ALL "${PS2RECOMP_DIR}/ps2xTest/tools/vu1_recomp.cpp")
    target_link_libraries(vu1_recomp PRIVATE ps2_runtime)
    target_include_directories(vu1_recomp PRIVATE "${PS2RECOMP_DIR}/ps2xRuntime/src/lib/vu")
endif()

# The offline oracle (decomp/oracle/rc_oracle.cpp, tools/oracle.sh): the same recompiled, decompiled and override code as
# the game, run on RAM dumps without the window, game loop or threads. Built in its own folder so the game's build is
# left alone: cmake -S . -B build-oracle -G Ninja && ninja -C build-oracle rc_oracle
set(RC_ORACLE_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/decomp/oracle/rc_oracle.cpp")
add_executable(rc_oracle EXCLUDE_FROM_ALL ${GAME_SOURCES} ${OVERRIDE_SOURCES} ${DECOMP_SOURCES} "${RC_ORACLE_SOURCE}"
    third_party/port_engine/rc_backend.cpp)
target_include_directories(rc_oracle PRIVATE "${RC_OUTPUT_DIR}")
target_link_libraries(rc_oracle PRIVATE ps2_runtime rc_port_engine)
set_target_properties(rc_oracle PROPERTIES UNITY_BUILD ON UNITY_BUILD_BATCH_SIZE 32)
target_compile_options(rc_oracle PRIVATE ${RC_GAME_OPT_LIST})
set_source_files_properties("${RC_ORACLE_SOURCE}" PROPERTIES SKIP_UNITY_BUILD_INCLUSION TRUE
    COMPILE_OPTIONS "-O2;-fwrapv;-fno-strict-aliasing"
    INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/decomp;${RC_GENERATED_VERSION_DIR};${CMAKE_CURRENT_SOURCE_DIR}/decomp/generated;${CMAKE_CURRENT_SOURCE_DIR}/decomp/oracle")
if(TARGET rc_vu1_code)
    target_link_libraries(rc_oracle PRIVATE -Wl,--whole-archive rc_vu1_code -Wl,--no-whole-archive)
endif()
