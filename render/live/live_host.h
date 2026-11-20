#pragma once

// The vk host's side of live 2D (renderer plan P1 step 7 batch 5; live2d.h). platform/vk_host.cpp calls these; the
// header names nothing but the RHI.
//
//   RC_VK_PICTURE=native   (with RC_RENDERER=vk) present the native picture: the frame reader on the EE thread, the cycle's
//                          list drawn through Gs2dVk, target[DISPFB2.FBP] (or an upload-only picture) presented. K2 (the
//                          3D) is not drawn yet: black where only K2 draws. F7 toggles to the emulated GS picture and
//                          back (the window title says which). Unset or "gs": the emulated GS picture as before, and
//                          nothing of this runs.
//   RC_VK_BLEND=ff|read    the fractional blends by fixed function (divergence D-2D-1) or by the local read; default
//                          the read where the device has it (Gs2dVk::kBlendAuto)
//   RC_VK_SHOT_DIR=<dir>   with RC_VK_SHOT_AT=<cycle>[,<cycle>...]: the native picture of the first presented cycle at or
//                          after each, read back, as <dir>/native_<cycle>.png and .rgba (640 x lines, RGBA as stored)
//   RC_VK_NATIVE_REPORT=<n> a reader line every n display cycles (default 600; 0 none)

#include "rhi.h"

#include <cstdint>
#include <string>

namespace rc::live
{
    // From a static initializer (vk_host's, when RC_RENDERER=vk): RC_VK_PICTURE=native makes the reader and adds it to
    // the capture's live consumers. True when native is on.
    bool installFromEnvironment();
    bool nativeEnabled();

    // After rhi_init (the window's device). False (and why) when Gs2dVk cannot run here; the host then shows the GS picture.
    bool createGpu(std::string *why);
    void destroyGpu();

    struct HostView
    {
        RhiTexture texture{};
        uint32_t width = 0, height = 0, texHeight = 0; // rows [0, height) of a texHeight-row texture, 640 wide
        bool valid = false;
    };
    // In the host's frame, after rhi_wait_frame: everything published since the last frame recorded into cl.
    HostView recordFrame(RhiCommandList cl);
    // After the frame's present: screenshots that came due (RC_VK_SHOT_*).
    void afterPresent();
    // The closing report (reader and presenter figures).
    void report();
}
