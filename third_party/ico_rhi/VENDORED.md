# ico_rhi: ico-pc's render hardware interface (Vulkan backend)

- Upstream: https://github.com/nathanialf/ico-pc (MIT, see `LICENSE`, copied from the repository root unchanged)
- Commit: `4e62244959841c9ff87f4cb2b8968f0499fd7865` (2026-11-09)
- Upstream folder: `port/rhi/`. Documentation upstream: `docs/port/RHI.md`.
- Vendored 2026-11-19 for the native Vulkan renderer (`RENDERER_PLAN.md`, P0 step 2).

## What was taken

25 files, 16,614 lines, byte-identical to upstream at that commit, at the same relative paths (so the `../rhi.h`
style includes resolve unchanged):

| here | upstream `port/rhi/` |
|---|---|
| `rhi.h` | the interface (C, `extern "C"` guarded) |
| `rhi_native.h` | native handles for external compute (the Vulkan backend includes it) |
| `rhi_backend.c`, `rhi_backend.h`, `rhi_backend_names.h` | the dispatcher: `rhi.h` entry points forwarded to the selected backend |
| `rhi_pipeline_pool.h`, `rhi_bind_cache.c/.h`, `rhi_adapter.c/.h`, `rhi_driver_version.c/.h` | helpers the Vulkan backend links |
| `vk/` (12 files) | the Vulkan backend: `rhi_vk.h`, `vk_cmd.c`, `vk_device.c`, `vk_enums.h`, `vk_internal.h`, `vk_memory.c`, `vk_native.c`, `vk_pipeline.c`, `vk_present_mode.c`, `vk_renderpass.c`, `vk_resource.c`, `vk_swapchain.c`, `vk_sync.c` |

Left out: the D3D12 (`d3d12/`), Direct3D 11 (`d3d11/`) and OpenGL (`gl/`) backends, AMD FidelityFX glue (`ffx/`),
the Android surface (`vk/vk_surface_android.c`), upstream's tests (`test/`, which need ico-pc's DXC-built shader
headers and its thread library) and upstream's `CMakeLists.txt` (it builds every backend and finds its dependencies
under ico-pc's `tools/toolchain/deps`).

## Local changes

- **Source files: none.** Every `.c`/`.h` file is upstream's. Keep it that way where possible; if one must change,
  mark the edit with an `RC:` comment and list it here.
- **`CMakeLists.txt`: written for RC** (not upstream's). Two static C libraries as upstream's `ico_rhi_vulkan` and
  `ico_rhi`: `rc_ico_rhi_vulkan` (the backend, compiled with `RHI_BACKEND_PREFIX=vk`, `VK_NO_PROTOTYPES`,
  `ICO_RHI_HAVE_SDL=1`, and `ICO_RHI_DEBUG_DEFAULT=1` in Debug builds, as upstream) and `rc_ico_rhi` (the dispatcher,
  `ICO_RHI_HAVE_VK=1` only, so Vulkan is the only backend in the table). Compile options are upstream's semantic set
  (`-fno-strict-aliasing -fwrapv -ffp-contract=off -fno-fast-math`) and its warnings without `-Werror`.
- **Dependencies, found differently from upstream:**
  - Vulkan-Headers `v1.4.363` and volk `vulkan-sdk-1.4.363.0`: FetchContent (sources only; `volk.c` is compiled into
    the backend as upstream does), the same pins as `third_party/port_engine`. An including project can pass
    `RC_VULKAN_INCLUDE_DIR` / `RC_VOLK_DIR` instead.
  - SDL3: the system's (`find_package(SDL3 CONFIG)`, e.g. pacman `sdl3`), else FetchContent of `RC_SDL3_TAG`
    (default `release-3.4.20`); `RC_RHI_FETCH_SDL3=ON` forces the source build.
  - The backend loads `libvulkan` at run time through volk; nothing links a Vulkan library.

## Things to know when using it

- **Clip space is y-up** (D3D convention): `rhi_cmd_set_viewport` sets a negative-height Vulkan viewport
  (`vk/vk_cmd.c`). GLSL written for OpenGL's y-up clip space draws the right way up; GLSL written for Vulkan's y-down
  convention comes out upside down.
- **Binding model** (`rhi.h`, "bind groups"): descriptor set = bind group (0 per-frame, 1 per-draw, 2 textures and
  samplers); binding = slot + 0 for uniform buffers, + 16 for textures, + 32 for samplers, + 48 for storage writes
  (upstream's DXC `-fvk-*-shift` flags). In GLSL: texture slot 1 of group 2 is `layout(set = 2, binding = 17)`,
  sampler slot 1 is `binding = 33`. Shaders are SPIR-V blobs passed to `rhi_create_shader`; RC compiles GLSL with
  `glslangValidator` (no DXC).
- **Validation:** `RhiDeviceDesc.debugLayers` (or `ICO_VK_VALIDATION=1`, or a Debug build) enables
  `VK_LAYER_KHRONOS_validation` if the loader finds it; messages go to stderr as `rhi_vk validation error:` /
  `rhi_vk validation warning:`; `rhi_vk_validation_error_count()` (`vk/rhi_vk.h`) counts the errors.
- **Device choice:** the highest-scoring device (discrete over integrated); `ICO_VK_DEVICE=<index or name part>`
  forces one. The chosen device is logged as `rhi_vk: <name> (Vulkan ...; device <vendor>:<id>; driver ...)`.
- Many `ICO_VK_*` environment switches (fake limits, fallbacks) are documented in upstream's `docs/port/RHI.md`.

## Updating

Copy the same files from a newer ico-pc `port/rhi/`, re-check that nothing new outside this list is included
(`grep '#include' *.c *.h vk/*`), update the commit above, and rebuild and run `tools/rhi_triangle/run.sh`.
