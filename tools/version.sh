# Sourced by the run scripts (play.sh, shadow.sh, capture_dumps.sh, oracle.sh): the paths of the game version chosen by
# RC_VERSION, usa (SLUS-20153, the default) or eu (SLES-50077). Expects $root (the repository folder).
#   RC_DISC        extracted disc files            RC_ELF     the executable's file name
#   RC_ISO         the disc image the game reads   RC_GAME_BUILD / RC_ORACLE_BUILD   the build folders
# tools/setup.sh puts the disc files in game/<version>/ (disc/ and the image, named as the game code expects).
case "${RC_VERSION:-usa}" in
    usa) RC_DISC="$root/game/usa/disc"; RC_ELF=SLUS_201.53; RC_ISO="$root/game/usa/RC Revenge Pro (USA).iso"
         RC_GAME_BUILD="$root/build-decomp"; RC_ORACLE_BUILD="$root/build-oracle" ;;
    eu)  RC_DISC="$root/game/eu/disc"; RC_ELF=SLES_500.77; RC_ISO="$root/game/eu/RC Revenge Pro (Europe).iso"
         RC_GAME_BUILD="$root/build-eu"; RC_ORACLE_BUILD="$root/build-oracle-eu" ;;
    *) echo "RC_VERSION must be usa or eu, not ${RC_VERSION}" >&2; exit 2 ;;
esac
[ -e "$RC_DISC/$RC_ELF" ] || echo "warning: no $RC_DISC/$RC_ELF: run tools/setup.sh with your ${RC_VERSION:-usa} disc image" >&2

# Makes a sandbox disc folder (links to the extracted files, empty memory cards) and links the disc image beside it.
rc_make_sandbox() {
    local sandbox="$1"
    rm -rf "$sandbox"; mkdir -p "$sandbox/disc/mc0" "$sandbox/disc/mc1"
    for entry in "$RC_DISC"/*; do
        case "$(basename "$entry")" in mc0|mc1) ;; *) ln -s "$entry" "$sandbox/disc/" ;; esac
    done
    ln -s "$RC_ISO" "$sandbox/" # The game reads its files through the disc image.
}

# Selects the discrete NVIDIA GPU (PRIME render offload) for the game, after checking it really works: being listed is
# not enough (after an Xid such as 175, "GPU memory subsystem timeout", `nvidia-smi -L` still lists it while every query
# says "GPU requires reset" and the game only gets a software renderer). Returns 1 with a message when the GPU is there
# but unhealthy; PLAY_ALLOW_IGPU=1 then runs on the integrated GPU instead. No NVIDIA driver: returns 0, nothing set.
rc_use_nvidia() {
    command -v nvidia-smi >/dev/null || return 0
    local query full problem=""
    query=$(timeout 15 nvidia-smi --query-gpu=name,pstate,temperature.gpu,memory.used --format=csv,noheader 2>&1) || problem="nvidia-smi failed (exit $?)"
    full=$(timeout 15 nvidia-smi 2>&1) || problem="${problem:-nvidia-smi failed (exit $?)}"
    if [ -z "$problem" ]; then
        case "$query$full" in
            *"requires reset"*) problem="the GPU requires a reset" ;;
            *"ERR!"*|*"Unknown Error"*|*"No devices were found"*|*"has fallen off the bus"*) problem="nvidia-smi reports errors" ;;
        esac
    fi
    if [ -n "$problem" ]; then
        echo "The NVIDIA GPU is not working ($problem), so the game was not started." >&2
        echo "  nvidia-smi: $(echo "$query" | head -1)" >&2
        echo "  Reboot to reset it (see 'journalctl -k | grep Xid'). PLAY_ALLOW_IGPU=1 runs on the integrated GPU instead." >&2
        [ "${PLAY_ALLOW_IGPU:-0}" = 1 ] && return 0
        return 1
    fi
    # An Xid since boot that the driver recovered from: say so, but run.
    local xid
    xid=$(journalctl -k -b --no-pager -q 2>/dev/null | grep -m1 "NVRM: Xid") && echo "warning: NVIDIA error earlier in this boot: ${xid##*kernel: }" >&2
    export __NV_PRIME_RENDER_OFFLOAD=1 __VK_LAYER_NV_optimus=NVIDIA_only __GLX_VENDOR_LIBRARY_NAME=nvidia
    return 0
}
