// Decompiled from src\GFX_P2.c: the GS set-up and tear-down, the buffer swap, the double-buffered packet's send and the
// texture uploads (the rest of the file, the packet builders, is in rdr.cpp). The SDK's GS, DMA and device libraries
// stay guest calls.

#include "rc_widescreen.h" // enhanced recomp: HUD and menu scale
#include "display_fuzz.h"
#include "../rc_frame_tick.h"

RC_VERSION_VARIANT(GfxP2_Init)                // Europe: the PAL display mode
RC_VERSION_VARIANT(GfxP2_RenderDmaSendActual) // Europe: gLastTex cleared later, the same behaviour

namespace rc {

bool_ MemBlockAllocLo(ptr32<memHandle> pHandle, uint32 size); // mem.cpp

namespace {
constexpr uint32_t kT0Count = 0x10000000u;
constexpr uint32_t kSceGsSyncPath = sym::sceGsSyncPath;
constexpr uint32_t kSceGsSyncV = sym::sceGsSyncV;
constexpr uint32_t kSceGsSetHalfOffset = sym::sceGsSetHalfOffset;
constexpr uint32_t kSceGsSwapDBuff = sym::sceGsSwapDBuff;
constexpr uint32_t kGfxP2_RenderDmaSendActual = sym::GfxP2_RenderDmaSendActual;

template <typename T>
T &mem(uint32_t address) { return *static_cast<T *>(guest_host(address)); }
uint64_t &mem64(uint32_t address) { return mem<uint64_t>(address); }
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
    {
        // Side tag: the flip (DISPFB2, which closes the display cycle) and the next frame's draw environment and clear.
        const rc::gfx::SpanKicks flip(rc::gfx::KickLabel{rc::gfx::KickFlip, guest_addr(&dbuff), rdr.frameNumber, 0u});
        guest_call(kSceGsSwapDBuff, guest_addr(&dbuff), rdr.frameNumber);
    }

    const uint32_t now = hw_read32(kT0Count);
    const uint32_t flipStart = G(swapTime1);
    G(swapTime2) = now - flipStart;
    if (frametick::logicPass()) // enhancement: counts logic frames, not the pictures drawn between them
        G(displayVarG).uiVSyncInc += 1u;
    G(swapTime1) = flipStart - G(swapTime0);
}

// Resets the VIFs, VUs, GS path and DMA, sets the GS's display mode and the double buffer (each with a clear to the
// background colour gR, gG, gB), and allocates the two DMA packets and the texture buffer.
void GfxP2_Init(s32 wd, s32 ht)
{
    GuestFrame frame(0xc0);
    for (const uint32_t reset : {sym::sceDevVif0Reset, sym::sceDevVif1Reset, sym::sceDevVu0Reset, sym::sceDevVu1Reset, sym::sceGsResetPath})
        guest_call(reset);
    guest_call(sym::sceDmaReset, 1);
#if RC_VERSION_EU
    guest_call(sym::sceGsResetGraph, 0, 1, 3, 1); // PAL, interlaced, frame mode
#else
    guest_call(sym::sceGsResetGraph, 0, 1, 2, 1); // NTSC, interlaced, frame mode
#endif
    sceGsDBuff &dbuff = G(gGfxP2_DBuff);
    const int32_t w = static_cast<int16_t>(wd), h = static_cast<int16_t>(ht);
    guest_call(sym::sceGsSetDefDBuff, guest_addr(&dbuff), 0, w, h, 3, 0x30, 1);
    const int32_t x = static_cast<int16_t>(0x800 - wd / 2), y = static_cast<int16_t>(0x800 - ht / 2);
    sGfxP2 &gfx = G(gGfxP2);
    // The clear colour's blue, alpha and q go on the stack (arguments 9-11).
    const auto clear = [&](uint32_t at)
    {
        frame.local<uint32_t>(0)[0] = G(gB);
        frame.local<uint32_t>(8)[0] = 0xff;
        frame.local<uint32_t>(0x10)[0] = 1;
        guest_call(sym::sceGsSetDefClear, at, 3, x, y, w, h, static_cast<uint32_t>(G(gR)), static_cast<uint32_t>(G(gG)));
    };
    frame.local<uint32_t>(0)[0] = G(gB);
    gfx.ht = ht;
    gfx.wd = wd;
    clear(guest_addr(&dbuff) + 0xe0u);
    clear(guest_addr(&dbuff) + 0x1d0u);
    MemBlockAllocLo(ptr32<memHandle>(sym::gpGfx_DmaBuff0), 0x50000);
    MemBlockAllocLo(ptr32<memHandle>(sym::gpGfx_DmaBuff1), 0x50000);
    gfx.buffIdx = 0;
    gfx.pDmaBuff = G(gpGfx_DmaBuff0);
    gfx.gifTagIdx = 0;
    MemBlockAllocLo(ptr32<memHandle>(guest_addr(&gfx.pTexBuff)), 0x40000);
    gfx.pDmaGif.address = guest_call<uint32_t>(sym::sceDmaGetChan, 2);
    gfx.currTexAddr = static_cast<u32>(gfx.ht) * static_cast<u32>(gfx.wd) * 12u;
}

// The packets and the texture buffer go back with free, although MemBlockAllocLo gave them (a game bug, kept). The
// last free is a tail jump.
void GfxP2_Term()
{
    {
        GuestFrame frame(0x20);
        guest_call(sym::free, G(gpGfx_DmaBuff0).address);
        guest_call(sym::free, G(gpGfx_DmaBuff1).address);
    }
    guest_call(sym::free, G(gGfxP2).pTexBuff.address);
}

// The double-buffered packet: ends the packet being filled (its last GIF tag gets EOP, bit 15), makes the other buffer
// the one to fill and, if the full one holds more than five quadwords, sends it to the GIF channel.
void GfxP2_RenderDmaSendActual()
{
    rc::widescreen::packetSending(); // enhanced recomp: an open HUD/menu scale scope's vertices placed first
    GuestFrame frame(0x20);
    sGfxP2 &gfx = G(gGfxP2);
    const uint32_t full = gfx.pDmaBuff.address;
    const uint32_t tag = gfx.gifTagIdx != -1 ? full + (static_cast<uint32_t>(gfx.gifTagIdx) << 4) : full;
    const uint64_t eop = mem64(tag) | 0x8000u;
    const uint32_t buff0 = G(gpGfx_DmaBuff0).address;
#if !RC_VERSION_EU
    G(gLastTex).address = 0;
#endif
    mem64(tag) = eop;
    const bool wasBuff0 = full == buff0;
    gfx.pDmaBuff.address = wasBuff0 ? G(gpGfx_DmaBuff1).address : buff0;
#if RC_VERSION_EU
    G(gLastTex).address = 0;
#endif
    if (gfx.buffIdx >= 6)
    {
        guest_call(sym::FlushCache, 0);
        // Side tag: the kick sceDmaSendN makes is K1 (the 2D packet).
        const rc::gfx::SpanKicks k1(rc::gfx::KickLabel{rc::gfx::KickK1, full, static_cast<uint32_t>(gfx.buffIdx), 0u});
        guest_call(sym::sceDmaSendN, gfx.pDmaGif.address, wasBuff0 ? G(gpGfx_DmaBuff0).address : G(gpGfx_DmaBuff1).address, gfx.buffIdx);
    }
    gfx.buffIdx = 0;
    gfx.gifTagIdx = -1;
    rc::widescreen::packetSent();
}

namespace {
// GfxP2_LoadTex and GfxP2_LoadTexMip: one image (a mip level) uploaded to texAddr (bits 8-23: the GS block) through
// path 2 and waited for. The buffer width is the image's in 64-pixel units, at least 1.
void loadImage(const GuestFrame &frame, u32 texAddr, uint32_t tex, int32_t wd, int32_t ht, uint32_t bitmap, uint32_t levelFrame)
{
    // Side tag: the upload's kick.
    const rc::gfx::SpanKicks upload(rc::gfx::KickLabel{rc::gfx::KickTextureUpload, tex, (texAddr >> 8) & 0xffffu, levelFrame});
    const int32_t blocks = (wd >= 0 ? wd : wd + 63) >> 6;
    guest_call(sym::sceGsSetDefLoadImage, frame.local<uint8_t>(0).address, static_cast<int32_t>(texAddr << 8) >> 16,
               static_cast<int16_t>(blocks > 0 ? blocks : 1), mem<int16_t>(tex + 0x10u), 0, 0, static_cast<int16_t>(wd), static_cast<int16_t>(ht));
    guest_call(sym::FlushCache, 0);
    guest_call(sym::sceGsExecLoadImage, frame.local<uint8_t>(0).address, bitmap);
    guest_call(sym::sceGsSyncPath, 0, 0);
}
} // namespace

void GfxP2_LoadTex(u32 texAddr, ptr32<_sGfxTexture> pGfxTex)
{
    GuestFrame frame(0x80);
    const uint32_t tex = pGfxTex.address;
    // Side tag: the upload's kick.
    const rc::gfx::SpanKicks upload(rc::gfx::KickLabel{rc::gfx::KickTextureUpload, tex, (texAddr >> 8) & 0xffffu, 0u});
    // The height is read as a halfword, the width as a word.
    const int32_t wd = mem<int32_t>(tex + 8u);
    const int32_t blocks = (wd >= 0 ? wd : wd + 63) >> 6;
    guest_call(sym::sceGsSetDefLoadImage, frame.local<uint8_t>(0).address, static_cast<int32_t>(texAddr << 8) >> 16,
               static_cast<int16_t>(blocks > 0 ? blocks : 1), mem<int16_t>(tex + 0x10u), 0, 0, static_cast<int16_t>(wd), mem<int16_t>(tex + 0xcu));
    guest_call(sym::FlushCache, 0);
    guest_call(sym::sceGsExecLoadImage, frame.local<uint8_t>(0).address, mem<uint32_t>(tex + 0x48u));
    guest_call(sym::sceGsSyncPath, 0, 0);
}

// One mip level of one frame: the sizes shifted down by the level, the bitmap from the texture's list (seven levels a
// frame, from +0x48).
void GfxP2_LoadTexMip(u32 texAddr, ptr32<_sGfxTexture> pGfxTex, s32 level, s32 frame)
{
    GuestFrame stack(0xa0);
    const uint32_t tex = pGfxTex.address;
    const int32_t wd = mem<int32_t>(tex + 8u) >> (level & 31), ht = mem<int32_t>(tex + 0xcu) >> (level & 31);
    loadImage(stack, texAddr, tex, wd, ht, mem<uint32_t>(tex + 0x48u + (static_cast<uint32_t>(frame) * 7u + static_cast<uint32_t>(level)) * 4u),
              (static_cast<uint32_t>(level) & 0xffu) | static_cast<uint32_t>(frame) << 8);
}

} // namespace rc

RC_DECOMP_FN_BLOCKING(GfxP2_SwapBuffers)
// GS and DMA set-up and kicks: offline only, the hardware stores and the GS library's waits logged (display_fuzz.cpp).
RC_DECOMP_FN_BLOCKING_SETUP_HEAP(GfxP2_Init, rc::fuzz::gfxInit)
RC_DECOMP_FN_BLOCKING_SETUP(GfxP2_Term, rc::fuzz::gfxTerm)
RC_DECOMP_FN_BLOCKING_SETUP(GfxP2_RenderDmaSendActual, rc::fuzz::gfxDmaSend)
RC_DECOMP_FN_BLOCKING_SETUP(GfxP2_LoadTex, rc::fuzz::gfxLoadTex)
RC_DECOMP_FN_BLOCKING_SETUP(GfxP2_LoadTexMip, rc::fuzz::gfxLoadTex)
