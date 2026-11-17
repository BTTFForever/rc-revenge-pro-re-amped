# Sourced by the run scripts (play.sh, shadow.sh, capture_dumps.sh, oracle.sh): the paths of the game version chosen by
# RC_VERSION, usa (SLUS-20153, the default) or eu (SLES-50077). Expects $root (this folder's parent).
#   RC_DISC        extracted disc files            RC_ELF     the executable's file name
#   RC_ISO         the disc image the game reads   RC_GAME_BUILD / RC_ORACLE_BUILD   the build folders
case "${RC_VERSION:-usa}" in
    usa) RC_DISC="$root/../disc"; RC_ELF=SLUS_201.53; RC_ISO="$root/../RC Revenge Pro (USA).iso"
         RC_GAME_BUILD="$root/build-decomp"; RC_ORACLE_BUILD="$root/build-oracle" ;;
    eu)  RC_DISC="$root/../disc_eu"; RC_ELF=SLES_500.77; RC_ISO="$root/../RC Revenge Pro (Europe).iso"
         RC_GAME_BUILD="$root/build-eu"; RC_ORACLE_BUILD="$root/build-oracle-eu" ;;
    *) echo "RC_VERSION must be usa or eu, not ${RC_VERSION}" >&2; exit 2 ;;
esac

# Makes a sandbox disc folder (links to the extracted files, empty memory cards) and links the disc image beside it.
rc_make_sandbox() {
    local sandbox="$1"
    rm -rf "$sandbox"; mkdir -p "$sandbox/disc/mc0" "$sandbox/disc/mc1"
    for entry in "$RC_DISC"/*; do
        case "$(basename "$entry")" in mc0|mc1) ;; *) ln -s "$entry" "$sandbox/disc/" ;; esac
    done
    ln -s "$RC_ISO" "$sandbox/" # The game reads its files through the disc image.
}
