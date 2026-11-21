#pragma once

// The vk host's side of live 2D (renderer plan P1 step 7 batch 5; live2d.h). platform/vk_host.cpp calls these; the
// header names nothing but the RHI.
//
//   RC_VK_PICTURE=native   (with RC_RENDERER=vk) present the native picture: the frame reader on the EE thread, the cycle's
//                          list (uploads, the clear, K2, K1) drawn through Gs2dVk, target[DISPFB2.FBP] presented (step 8
//                          batch 4: the whole picture). F7 toggles to the emulated GS picture and back (the window title
//                          says which). Unset or "gs": the emulated GS picture as before, and nothing of this runs.
//   RC_VK_BLEND=ff|read    the fractional blends by fixed function (divergence D-2D-1) or by the local read; default
//                          the read where the device has it (Gs2dVk::kBlendAuto)
//   RC_VK_SHOT_DIR=<dir>   with RC_VK_SHOT_AT=<cycle>[,<cycle>...]: the native picture of the first presented cycle at or
//                          after each, read back, as <dir>/native_<cycle>.png and .rgba (640 x lines, RGBA as stored), and
//                          the emulated GS picture handed to the same present as gs_<cycle>.png and .rgba (its own size)
//   RC_VK_NATIVE_REPORT=<n> a reader line every n display cycles and a presenter line every n frames with lists (GPU time,
//                          K2's GPU time a VIF1 kick; default 600; 0 none)
//   RC_FRAME_RECORD_DRAWLISTS=1 (with RC_FRAME_RECORD) each recorded cycle's list into the dump as DLST (the live check)
//   RC_VK_PRESENT_FILTER=nearest  (vk_host) the native picture sampled nearest instead of linear when presented

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
    // After the frame's present: screenshots that came due (RC_VK_SHOT_*); the emulated GS picture of the same present beside
    // them when given.
    void afterPresent(const uint8_t *gsPixels = nullptr, uint32_t gsWidth = 0, uint32_t gsHeight = 0);
    // The closing report (reader and presenter figures).
    void report();
}
