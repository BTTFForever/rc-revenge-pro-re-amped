#!/usr/bin/env bash
# Runs rhi_triangle on the discrete NVIDIA GPU (PRIME offload) with the Khronos validation layer, synchronisation
# validation on, and fails on anything short of a clean run: a non-zero exit, a device other than the wanted one, a
# validation error or warning, or the layer missing.
#
#   tools/rhi_triangle/run.sh [build-dir] [rhi_triangle arguments...]
#   e.g. tools/rhi_triangle/run.sh build-rhi --frames 120 --screenshot build-rhi/triangle.png
#
# The validation layer: whatever the Vulkan loader already finds (pacman vulkan-validation-layers, the Vulkan SDK,
# VK_ADD_LAYER_PATH). Without one, RC_VVL_LIB may name a libVkLayer_khronos_validation.so (for example the copy in
# Steam's ubuntu12_64 folder); the script writes a manifest for it into <build-dir>/vvl and adds that folder.
# RC_RHI_GPU (default NVIDIA) is the text the adapter name must contain; empty skips the check.
set -uo pipefail
BUILD="${1:-build-rhi}"
shift || true
EXE="$BUILD/rhi_triangle"
GPU="${RC_RHI_GPU-NVIDIA}"
if [[ ! -x "$EXE" ]]; then
    echo "run.sh: $EXE not built (cmake -S tools/rhi_triangle -B $BUILD && cmake --build $BUILD)" >&2
    exit 2
fi

have_layer() {
    local d
    local IFS=:
    for d in ${VK_LAYER_PATH:-} ${VK_ADD_LAYER_PATH:-} ${VULKAN_SDK:+$VULKAN_SDK/share/vulkan/explicit_layer.d} \
             "${XDG_DATA_HOME:-$HOME/.local/share}/vulkan/explicit_layer.d" /etc/vulkan/explicit_layer.d \
             /usr/local/share/vulkan/explicit_layer.d /usr/share/vulkan/explicit_layer.d; do
        [[ -n "$d" && -f "$d/VkLayer_khronos_validation.json" ]] && return 0
    done
    return 1
}

if ! have_layer; then
    if [[ -n "${RC_VVL_LIB:-}" && -f "$RC_VVL_LIB" ]]; then
        mkdir -p "$BUILD/vvl"
        lib="$(cd "$(dirname "$RC_VVL_LIB")" && pwd)/$(basename "$RC_VVL_LIB")"
        cat > "$BUILD/vvl/VkLayer_khronos_validation.json" <<EOF
{
  "file_format_version": "1.2.0",
  "layer": {
    "name": "VK_LAYER_KHRONOS_validation",
    "type": "GLOBAL",
    "library_path": "$lib",
    "api_version": "1.4.300",
    "implementation_version": "1",
    "description": "Khronos validation layer (manifest written by tools/rhi_triangle/run.sh)"
  }
}
EOF
        export VK_ADD_LAYER_PATH="$(cd "$BUILD/vvl" && pwd)${VK_ADD_LAYER_PATH:+:$VK_ADD_LAYER_PATH}"
        echo "run.sh: validation layer from RC_VVL_LIB ($lib)"
    else
        echo "run.sh: FAIL: VK_LAYER_KHRONOS_validation is not installed (pacman -S vulkan-validation-layers," \
             "or set RC_VVL_LIB to a libVkLayer_khronos_validation.so)" >&2
        exit 1
    fi
fi

LOG="$BUILD/rhi_triangle.log"
__NV_PRIME_RENDER_OFFLOAD=1 __VK_LAYER_NV_optimus=NVIDIA_only VK_KHRONOS_VALIDATION_VALIDATE_SYNC=true \
    ICO_VK_VALIDATION=1 "$EXE" "$@" 2> "$LOG"
rc=$?
cat "$LOG" >&2
fail=0
if [[ $rc -ne 0 ]]; then
    echo "run.sh: FAIL: rhi_triangle exited $rc"
    fail=1
fi
if grep -q 'VK_LAYER_KHRONOS_validation not installed' "$LOG"; then
    echo "run.sh: FAIL: the RHI did not find the validation layer"
    fail=1
fi
nerr=$(grep -c 'validation error' "$LOG")
nwarn=$(grep -c 'validation warning' "$LOG")
echo "run.sh: validation: $nerr error(s), $nwarn warning(s)"
if [[ $nerr -ne 0 || $nwarn -ne 0 ]]; then
    fail=1
fi
# the adapter line vk_device.c writes once the device is made:
# "rhi_vk: <name> (Vulkan <version>; device <vendor>:<id>; driver ...)"
adapter=$(grep -m 1 -E '^rhi_vk: .* \(Vulkan [0-9.]+; device [0-9a-f]+:[0-9a-f]+; driver' "$LOG" | sed -E 's/^rhi_vk: //; s/; index values.*//')
echo "run.sh: device: ${adapter:-none}"
if [[ -n "$GPU" && "${adapter%% (Vulkan*}" != *"$GPU"* ]]; then
    echo "run.sh: FAIL: the device in use is not $GPU"
    fail=1
fi
[[ $fail -eq 0 ]] && echo "run.sh: PASS" || echo "run.sh: FAIL"
exit $fail
