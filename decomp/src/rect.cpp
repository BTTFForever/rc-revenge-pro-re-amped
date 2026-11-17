// Decompiled from RECT.c: filled rectangles for the 2D screen (menus, the HUD), drawn through GFX_P2's packet buffer
// as two triangles each, with one colour (RectRender) or one per corner (RectRenderGouraud). The first packet sets
// the primitive type and the blending, then each corner is a colour and a position under one GIF tag.

#include "ee_math.h"

namespace rc {

namespace {

uint64_t *quadword(int32_t index) { return reinterpret_cast<uint64_t *>((G(gGfxP2).pDmaBuff + index).get()); }

// The state block: no texture, then PRIM (a triangle, with the given flags), PABE off and ALPHA.
void rectState(uint64_t prim, uint64_t alpha)
{
    sGfxP2 &gfx = G(gGfxP2);
    uint64_t *q = quadword(gfx.buffIdx);
    q[0] = 0x2000000000000001ull;
    q[1] = 0xe0;
    gfx.buffIdx = gfx.buffIdx + 3;
    q[2] = prim;
    q[3] = 0;
    q[4] = 0;
    q[5] = 0x49;
    q[6] = 0x1000000000000001ull;
    q[7] = 0xe;
    q[8] = alpha;
    q[9] = 0x42;
    gfx.buffIdx = gfx.buffIdx + 2;
    gfx.gifTagIdx = -1;
}

// One corner (RGBAQ, XYZ2) under the open GIF tag, or under a new tag if there is none or it is full.
void corner(uint64_t colourLow, uint64_t colourHigh, int32_t x, int32_t y, ee_float otz, uint32_t scaleAddress)
{
    sGfxP2 &gfx = G(gGfxP2);
    int32_t at = gfx.buffIdx;
    int32_t tag = gfx.gifTagIdx;
    if (tag == -1 || 0x7ffc < (*quadword(tag) & 0x7fff))
    {
        uint64_t *t = quadword(at);
        t[0] = 0x2000000000000000ull;
        t[1] = 0x51; // RGBAQ, XYZ2
        tag = gfx.gifTagIdx = at;
        ++at;
    }
    uint64_t *q = quadword(at);
    q[0] = colourLow;
    q[1] = colourHigh;
    const ee_float scale = *static_cast<ee_float *>(guest_host(scaleAddress));
    uint64_t &header = *quadword(tag);
    header = (header & 0xffffffffffff0000ull) | ((header & 0x7fff) + 1);
    gfx.buffIdx = at + 2;
    q[3] = static_cast<uint64_t>(static_cast<int64_t>(ee_ftoi((ee_float(1.0f) - otz) * scale)));
    const auto screen = [](int32_t v) { return static_cast<uint64_t>(ee_ftoi(ee_itof(v) * ee_float(16.0f))) & 0xffffu; };
    q[2] = screen(static_cast<int16_t>(x) + 0x6c0) | (screen(static_cast<int16_t>(y) + 0x780) << 32);
}

} // namespace

void RectRender(ptr32<_sRect> pRect, f32 otz)
{
    _sRect &rect = *pRect;
    const uint32_t flags = rect.flags;
    uint64_t blend = 0;
    if ((flags & 2) != 0)
        blend = 1;
    else if ((flags & 4) != 0)
        blend = 2;
    else if ((flags & 8) != 0)
        blend = 4;
    else if ((flags & 0x10) != 0)
        blend = static_cast<uint64_t>(static_cast<int32_t>(rect.fix * 0x100u + 0x20u));
    const uint64_t blended = (flags & 1) != 0;
    uint64_t alpha;
    if (!blended)
        alpha = 0x8000000029ull;
    else if ((blend & 0x20) != 0)
        alpha = ((blend & 0xff00) << 0x18) | 0x21;
    else if ((blend & 1) != 0)
        alpha = 0x8000000029ull;
    else if ((blend & 2) != 0)
        alpha = 0x80000000a1ull;
    else
        alpha = 0x4000000021ull;
    rectState((blended << 6) | 3, alpha);
    int32_t x1 = rect.x1;
    if (0x280 < static_cast<int16_t>(rect.x1))
    {
        x1 = 0x280;
        rect.x1 = 0x280;
    }
    int32_t x2 = rect.x2;
    if (0x280 < static_cast<int16_t>(rect.x2))
    {
        x2 = 0x280;
        rect.x2 = 0x280;
    }
    const int32_t x0 = rect.x0, y0 = rect.y0;
    G(gLastTex) = ptr32<_sGfxTexture>(0xffffffffu);
    const uint64_t colour = rect.red | (static_cast<uint64_t>(rect.green) << 32), blue = rect.blue;
    corner(colour, blue, x0, y0, otz, sym::lit4_327930);
    corner(colour, blue, rect.x3, rect.y3, otz, sym::lit4_327934);
    const int32_t y2 = rect.y2;
    corner(colour, blue, x2, y2, otz, sym::lit4_327938);
    corner(colour, blue, x0, y0, otz, sym::lit4_32793c);
    corner(colour, blue, x1, rect.y1, otz, sym::lit4_327940);
    corner(colour, blue, x2, y2, otz, sym::lit4_327944);
}

void RectRenderGouraud(ptr32<_sGRect> pGRect, f32 otz)
{
    _sGRect &rect = *pGRect;
    const uint32_t flags = rect.flags;
    bool testAlpha = false, add = false; // Ghidra's bVar8 (flag 2) and bVar9 (flag 4, without flag 2)
    uint64_t blend = 0;
    if ((flags & 2) != 0)
        testAlpha = true;
    else if ((flags & 4) != 0)
        add = true;
    else
        blend = (flags >> 1) & 4;
    const uint64_t blended = flags & 1;
    uint64_t alpha;
    if (blended == 0 || testAlpha)
        alpha = 0x8000000029ull;
    else if (add)
        alpha = 0x80000000a1ull;
    else
        alpha = 0x4000000021ull;
    (void)blend;
    rectState((blended << 6) | 0xb, alpha);
    int32_t x1 = rect.x1;
    if (0x280 < static_cast<int16_t>(rect.x1))
    {
        x1 = 0x280;
        rect.x1 = 0x280;
    }
    int32_t x2 = rect.x2;
    if (0x280 < static_cast<int16_t>(rect.x2))
    {
        x2 = 0x280;
        rect.x2 = 0x280;
    }
    const int32_t x0 = rect.x0, y0 = rect.y0;
    G(gLastTex) = ptr32<_sGfxTexture>(0xffffffffu);
    const auto low = [](uint8 r, uint8 g) { return static_cast<uint64_t>(r) | (static_cast<uint64_t>(g) << 32); };
    corner(low(rect.r0, rect.g0), rect.b0, x0, y0, otz, sym::lit4_327948);
    corner(low(rect.r3, rect.g3), rect.b3, rect.x3, rect.y3, otz, sym::lit4_32794c);
    const int32_t y2 = rect.y2;
    corner(low(rect.r2, rect.g2), rect.b2, x2, y2, otz, sym::lit4_327950);
    corner(low(rect.r0, rect.g0), rect.b0, x0, y0, otz, sym::lit4_327954);
    corner(low(rect.r1, rect.g1), rect.b1, x1, rect.y1, otz, sym::lit4_327958);
    corner(low(rect.r2, rect.g2), rect.b2, x2, y2, otz, sym::lit4_32795c);
}

// The clipped rectangle is not drawn.
void RectClip(ptr32<_sRectClip> pRectClip, uint32 otz) {}

} // namespace rc

// These write the packet buffer, which is on the heap.
RC_DECOMP_FN_SETUP_HEAP(RectRender, ::rc::Thunk<&::rc::RectRender>::randomise)
RC_DECOMP_FN_SETUP_HEAP(RectRenderGouraud, ::rc::Thunk<&::rc::RectRenderGouraud>::randomise)
RC_DECOMP_FN(RectClip)
