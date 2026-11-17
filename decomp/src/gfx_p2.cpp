// Decompiled from src\GFX_P2.c. Other parts of the file are still recompiled.

#include "rc_decomp.h"

namespace rc {

namespace {
constexpr uint32_t kT0Count = 0x10000000u;
constexpr uint32_t kSceGsSyncPath = sym::sceGsSyncPath;
constexpr uint32_t kSceGsSyncV = sym::sceGsSyncV;
constexpr uint32_t kSceGsSetHalfOffset = sym::sceGsSetHalfOffset;
constexpr uint32_t kSceGsSwapDBuff = sym::sceGsSwapDBuff;
constexpr uint32_t kGfxP2_RenderDmaSendActual = sym::GfxP2_RenderDmaSendActual;
} // namespace

// Ends a frame: waits for the GS to finish drawing, kicks the next frame's DMA, waits for vertical
// blank and flips the buffers. The three swapTime values record how long each stage took.
void GfxP2_SwapBuffers()
{
    sRdr &rdr = G(gRdr);
    sceGsDBuff &dbuff = G(gGfxP2_DBuff);

    G(swapTime0) = hw_read32(kT0Count);
    guest_call(kSceGsSyncPath, 0, 0);
    guest_call(kGfxP2_RenderDmaSendActual);
    guest_call(kSceGsSyncPath, 0, 0);

    const int32_t field = guest_call<int32_t>(kSceGsSyncV, 0);
    const uint32_t draw = guest_addr((rdr.frameNumber & 1u) != 0u ? &dbuff.draw1 : &dbuff.draw0);
    guest_call(kSceGsSetHalfOffset, draw, 0x800, 0x800, static_cast<int16_t>(field ^ 1));

    G(swapTime1) = hw_read32(kT0Count);
    guest_call(kSceGsSwapDBuff, guest_addr(&dbuff), rdr.frameNumber);

    const uint32_t now = hw_read32(kT0Count);
    const uint32_t flipStart = G(swapTime1);
    G(swapTime2) = now - flipStart;
    G(displayVarG).uiVSyncInc += 1u;
    G(swapTime1) = flipStart - G(swapTime0);
}

} // namespace rc

RC_DECOMP_FN_BLOCKING(GfxP2_SwapBuffers)
