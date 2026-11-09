# RC Revenge Pro (SLUS-20153) built against the Shadow of the Colossus PS2Recomp fork:
# https://github.com/LightVelox/PS2Recomp @ d06a4ab
cmake_minimum_required(VERSION 3.21)
project(RCRevengeProFork C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(PS2RECOMP_DIR "$ENV{HOME}/PS2Recomp-sotc" CACHE PATH "PS2Recomp fork checkout")

foreach(dep raylib imgui rlimgui)
    string(TOUPPER ${dep} DEP)
    set(src "$ENV{HOME}/PS2Recomp/out/build/_deps/${dep}-src")
    if(EXISTS "${src}")
        set(FETCHCONTENT_SOURCE_DIR_${DEP} "${src}" CACHE PATH "")
    endif()
endforeach()

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

file(GLOB GAME_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/output/*.cpp")

add_executable(rc_revenge_pro
    ${GAME_SOURCES}
    "${PS2RECOMP_DIR}/ps2xRuntime/src/main.cpp"
    "${PS2RECOMP_DIR}/ps2xRuntime/src/lib/ps2_debug_panel.cpp"
)
target_include_directories(rc_revenge_pro PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/output")
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
set_source_files_properties(${OVERRIDE_SOURCES} PROPERTIES SKIP_UNITY_BUILD_INCLUSION TRUE)

file(GLOB VU1_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/vu1/*.cpp")
if(VU1_SOURCES)
    add_library(rc_vu1_code STATIC ${VU1_SOURCES})
    target_include_directories(rc_vu1_code PRIVATE "${PS2RECOMP_DIR}/ps2xRuntime/src/lib/vu")
    target_link_libraries(rc_vu1_code PUBLIC ps2_runtime)
    target_compile_options(rc_vu1_code PRIVATE ${RC_VU1_OPT_LIST} -w)
    # Generated files only register themselves through static initializers.
    target_link_libraries(rc_revenge_pro PRIVATE -Wl,--whole-archive rc_vu1_code -Wl,--no-whole-archive)
endif()

# Profile-guided build. RC_PGO=generate builds an instrumented executable; running it (see
# tools/pgo.sh) writes a profile, and RC_PGO=use rebuilds with it in the same build folder.
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
    set_source_files_properties(${OVERRIDE_SOURCES} PROPERTIES COMPILE_DEFINITIONS RC_PGO_GEN=1)
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
