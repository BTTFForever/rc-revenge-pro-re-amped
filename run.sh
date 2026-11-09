#!/bin/bash
# Runs RC Revenge Pro with native VU1 and the GPU renderer. Extra arguments are not used.
# VU1 runs on its own thread, which is worth about a third more frame rate at the menu.
# Set PS2X_DEBUG_PANEL=1 to show the Runtime Debugger; F1 toggles it in game.
cd "$(dirname "$0")"
export PS2X_DEBUG_PANEL="${PS2X_DEBUG_PANEL:-0}"
export PS2X_GS_GPU=1 PS2X_GS_THREAD=1 PS2X_MTVU=1 PS2X_VU1_TIMING=0 PS2X_EE_HOST_PACING=1
export PS2X_GS_SHADER_PRELOAD=0 PS2X_GS_SHADER_CACHE=1 PS2X_GS_SHADER_CACHE_DIR="$PWD/gs_shaders"
exec build/rc_revenge_pro ../disc/SLUS_201.53
