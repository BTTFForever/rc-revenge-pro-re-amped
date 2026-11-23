// Decompiled from SPRITE.c: 2D sprites for the HUD, menus and fonts, drawn straight to the GS through GFX_P2's
// packet buffer. A sprite is two triangles, six vertices of GIF "packed" data (texture coordinates, Q, colour,
// position); every third vertex kicks a triangle.

#include "rc_settings.h" // enhanced recomp: widescreen
#include "rc_widescreen.h"
#include "ee_math.h"

#include <utility>

namespace rc {

void ResTypeSetConstructDaemon(ptr32<char> pType, uint32 constructDaemon, int16 options);
void GfxP2_TexExt(ptr32<_sGfxTexture> pTex, s32 attrib, s32 alphaType);

namespace {

const uint64_t kWhite = 0x7f0000007fu, kWhiteBA = 0x800000007fu; // RGBA 0x7f,0x7f,0x7f,0x80 as packed data
const uint64_t kOne = 0x3f800000u;                               // Q = 1.0
const uint64_t kAdc = 0x800000000000u;                           // XYZ2's "don't draw" bit

// Appends vertices to the open GIF tag of the GS packet buffer, opening one (ST, RGBAQ, XYZ2) if there is none.
class SpriteStrip
{
public:
    SpriteStrip() : m_gfx(G(gGfxP2)), m_buffer(m_gfx.pDmaBuff), m_tag(m_gfx.gifTagIdx) {}

    void vertex(uint64_t st, uint64_t rgbaLow, uint64_t rgbaHigh, uint64_t xy, uint64_t z)
    {
        if (m_tag == -1)
        {
            uint64_t *tag = quadword(m_gfx.buffIdx);
            tag[0] = 0x3000000000000000ull;
            tag[1] = 0x512;
            m_tag = m_gfx.gifTagIdx = m_gfx.buffIdx;
            m_gfx.buffIdx = m_gfx.buffIdx + 1;
        }
        uint16_t &count = *reinterpret_cast<uint16_t *>(quadword(m_tag));
        const uint16_t n = static_cast<uint16_t>(count + 1);
        count = n;
        uint64_t *q = quadword(m_gfx.buffIdx);
        q[0] = st;
        q[1] = kOne;
        q[2] = rgbaLow;
        q[3] = rgbaHigh;
        q[4] = xy;
        q[5] = n % 3u != 0 ? z | kAdc : z;
        m_gfx.buffIdx = m_gfx.buffIdx + 3;
    }

private:
    uint64_t *quadword(int32_t index) { return reinterpret_cast<uint64_t *>((m_buffer + index).get()); }

    sGfxP2 &m_gfx;
    ptr32<u128> m_buffer;
    int32_t m_tag;
};

// GS coordinates are 12.4 fixed point with the screen at (0x6c0, 0x780).
uint64_t gsCoord(int32_t value)
{
    return static_cast<uint64_t>(ee_ftoi(ee_itof(value) * ee_float(16.0f))) & 0xffffu;
}

uint64_t xy(uint64_t x, uint64_t y) { return x | (y << 32); }

// The depth for an ordering value: each sprite function has its own scale constant.
uint64_t depth(ee_float otz, uint32_t scaleAddress)
{
    const ee_float scale = *static_cast<ee_float *>(guest_host(scaleAddress));
    return static_cast<uint64_t>(static_cast<int64_t>(ee_ftoi((ee_float(1.0f) - otz) * scale)));
}

uint64_t st(float s, float t)
{
    uint32_t sb, tb;
    std::memcpy(&sb, &s, 4);
    std::memcpy(&tb, &t, 4);
    return sb | (static_cast<uint64_t>(tb) << 32);
}

} // namespace

// A sprite file points at its texture; its size comes from the texture.
void SpriteConstructDaemon(ptr32<memHandle> pHandle)
{
    _sSprite &sprite = *pHandle->cast<_sSprite>();
    const uint8_t *tex = static_cast<const uint8_t *>(guest_host(sprite.pTextureHandle.address));
    // In the original's order: the texture may overlap the sprite.
    sprite.width = tex[8];
    const uint8 height = *static_cast<const uint8_t *>(guest_host(sprite.pTextureHandle.address + 0xc));
    sprite.yOffset = 0;
    sprite.height = height;
    sprite.xOffset = 0;
}

void SpriteSetTextureHandle(ptr32<_sSprite>, uint32, uint32) {}
void SpriteRenderSprite(ptr32<_sSprite>, ptr32<_sSpriteParams>, uint32) {}

// A whole sprite at (x, y), white, unscaled.
void SpriteRenderFast(ptr32<_sSprite> pSprite, int16 x, int16 y, f32 otz)
{
    const rc::widescreen::AnchorScope uiScope(rc::gfx::AnchorCentre, rc::widescreen::AnchorScope::Ui); // widescreen: menus and 2D pillarboxed
    const uint8 width = pSprite->width, height = pSprite->height;
    GfxP2_TexExt(pSprite->pTextureHandle, 0, 0);
    const int32_t left = x + 0x6c0;
    const uint64_t z = depth(otz, sym::lit4_327974);
    const uint64_t x0 = gsCoord(left);
    const uint64_t yTop = gsCoord(y + 0x780), yBottom = gsCoord(y + 0x780 + height);
    const uint64_t x1 = gsCoord(left + width);
    SpriteStrip strip;
    strip.vertex(st(0.0f, 1.0f), kWhite, kWhiteBA, xy(x0, yTop), z);
    strip.vertex(st(0.0f, 0.0f), kWhite, kWhiteBA, xy(x0, yBottom), z);
    strip.vertex(st(1.0f, 0.0f), kWhite, kWhiteBA, xy(x1, yBottom), z);
    strip.vertex(st(0.0f, 1.0f), kWhite, kWhiteBA, xy(x0, yTop), z);
    strip.vertex(st(1.0f, 1.0f), kWhite, kWhiteBA, xy(gsCoord(left + pSprite->width), yTop), z);
    strip.vertex(st(1.0f, 0.0f), kWhite, kWhiteBA, xy(x1, yBottom), depth(otz, sym::lit4_327974));
}

namespace {

// The texture blend for a sprite's flags: 1 turns on blending, then 0x40, 0x80 or 0x100 pick its kind.
s32 spriteAlphaType(uint32 flags)
{
    if ((flags & 1) == 0)
        return 0;
    if ((flags & 0x40) != 0)
        return 1;
    if ((flags & 0x80) != 0)
        return 2;
    if ((flags & 0x100) != 0)
        return 4;
    return 0;
}

// Corners of a sprite on screen and the texel rectangle it shows, after its rotation flags (2, 4, 8 turn it by a
// quarter, half and three quarters) and mirror flags (0x10 horizontal, 0x20 vertical). Texel bounds are bytes, so
// they wrap like the original's.
struct SpriteQuad
{
    int16_t x[4], y[4];
    uint8_t s0, s1, t0, t1;
};

SpriteQuad spriteQuad(const _sSprite &sprite, const _sSpriteParams &params, uint8_t w, uint8_t h, int16_t screenW, int16_t screenH)
{
    SpriteQuad q;
    const uint32 flags = params.flags;
    uint8_t u, v, uEnd, vEnd;
    const int16_t px = params.x, py = params.y;
    if ((flags & 0xe) == 0)
    {
        u = static_cast<uint8_t>(sprite.xOffset + static_cast<uint8_t>(params.xOffset));
        v = static_cast<uint8_t>(sprite.yOffset + static_cast<uint8_t>(params.yOffset));
        q.x[0] = px; q.y[0] = py;
        q.x[1] = static_cast<int16_t>(px + screenW); q.y[1] = py;
        q.x[2] = px; q.y[2] = static_cast<int16_t>(py + screenH);
        q.x[3] = q.x[1]; q.y[3] = q.y[2];
        uEnd = static_cast<uint8_t>(u + w);
        vEnd = static_cast<uint8_t>(v + h);
    }
    else if ((flags & 2) != 0)
    {
        u = static_cast<uint8_t>(sprite.xOffset + static_cast<uint8_t>(params.yOffset));
        v = static_cast<uint8_t>(sprite.yOffset + static_cast<uint8_t>(params.xOffset));
        q.x[2] = px; q.y[0] = py;
        q.x[0] = static_cast<int16_t>(px + screenW); q.y[1] = static_cast<int16_t>(py + screenH);
        q.x[1] = q.x[0]; q.y[2] = q.y[0];
        q.x[3] = q.x[2]; q.y[3] = q.y[1];
        uEnd = static_cast<uint8_t>(u + h);
        vEnd = static_cast<uint8_t>(v + w);
    }
    else if ((flags & 4) == 0)
    {
        q.x[0] = px; q.y[1] = py;
        q.x[2] = static_cast<int16_t>(px + screenW); q.y[0] = static_cast<int16_t>(py + screenH);
        u = static_cast<uint8_t>(sprite.xOffset + static_cast<uint8_t>(params.yOffset));
        v = static_cast<uint8_t>(sprite.yOffset + static_cast<uint8_t>(params.xOffset));
        q.x[1] = q.x[0]; q.y[2] = q.y[0];
        q.x[3] = q.x[2]; q.y[3] = q.y[1];
        uEnd = static_cast<uint8_t>(u + h);
        vEnd = static_cast<uint8_t>(v + w);
    }
    else
    {
        u = static_cast<uint8_t>(sprite.xOffset + static_cast<uint8_t>(params.xOffset));
        v = static_cast<uint8_t>(sprite.yOffset + static_cast<uint8_t>(params.yOffset));
        q.x[1] = px; q.y[2] = py;
        q.x[0] = static_cast<int16_t>(px + screenW); q.y[0] = static_cast<int16_t>(py + screenH);
        q.y[1] = q.y[0]; q.x[2] = q.x[0];
        q.x[3] = q.x[1]; q.y[3] = q.y[2];
        uEnd = static_cast<uint8_t>(u + w);
        vEnd = static_cast<uint8_t>(v + h);
    }
    q.s0 = u; q.s1 = uEnd;
    if ((flags & 0x10) != 0)
        std::swap(q.s0, q.s1);
    q.t0 = v; q.t1 = vEnd;
    if ((flags & 0x20) != 0)
        std::swap(q.t0, q.t1);
    return q;
}

uint64_t corner(const SpriteQuad &q, int i) { return xy(gsCoord(q.x[i] + 0x6c0), gsCoord(q.y[i] + 0x780)); }

} // namespace

namespace {

enum class SpriteSize { Normal, Half, Scaled };

// SpriteRender and its variants: the same quad, with the screen size halved or scaled, a different texture flag and
// depth constant.
void renderSprite(ptr32<_sSprite> pSprite, ptr32<_sSpriteParams> pSpriteParams, f32 otz, SpriteSize size, bool filter, uint32_t depthScale)
{
    const rc::widescreen::AnchorScope uiScope(rc::gfx::AnchorCentre, rc::widescreen::AnchorScope::Ui); // widescreen: menus and 2D pillarboxed
    const _sSpriteParams &params = *pSpriteParams;
    const uint32 flags = params.flags;
    const uint8 spriteWidth = pSprite->width, spriteHeight = pSprite->height;
    const uint8_t w8 = static_cast<uint8_t>(params.width), h8 = static_cast<uint8_t>(params.height);
    int16_t screenW = w8, screenH = h8;
    if (size == SpriteSize::Half)
        screenH = static_cast<int16_t>(h8 >> 1);
    else if (size == SpriteSize::Scaled)
    {
        screenW = static_cast<int16_t>((static_cast<uint32_t>(w8) * params.xScale) >> 8);
        screenH = static_cast<int16_t>((static_cast<uint32_t>(h8) * params.yScale) >> 8);
    }
    const SpriteQuad q = spriteQuad(*pSprite, params, w8, h8, screenW, screenH);
    const uint8 blue = params.blue, red = params.red, green = params.green;
    GfxP2_TexExt(pSprite->pTextureHandle, filter ? static_cast<s32>((flags & 1) << 1) : 0, spriteAlphaType(flags));
    const ee_float w = ee_itof(spriteWidth), h = ee_itof(spriteHeight);
    const auto texel = [&](uint8_t s, uint8_t t) {
        const ee_float fs = ee_itof(s) / w, ft = ee_itof(t) / h;
        uint32_t sb, tb;
        std::memcpy(&sb, &fs, 4);
        std::memcpy(&tb, &ft, 4);
        return sb | (static_cast<uint64_t>(tb) << 32);
    };
    const uint64_t rgbaLow = red | (static_cast<uint64_t>(green) << 32), rgbaHigh = blue | 0x8000000000ull;
    const uint64_t z = depth(otz, depthScale);
    SpriteStrip strip;
    strip.vertex(texel(q.s0, q.t0), rgbaLow, rgbaHigh, corner(q, 0), z);
    strip.vertex(texel(q.s0, q.t1), rgbaLow, rgbaHigh, corner(q, 2), z);
    strip.vertex(texel(q.s1, q.t1), rgbaLow, rgbaHigh, corner(q, 3), z);
    strip.vertex(texel(q.s0, q.t0), rgbaLow, rgbaHigh, corner(q, 0), z);
    strip.vertex(texel(q.s1, q.t0), rgbaLow, rgbaHigh, corner(q, 1), z);
    strip.vertex(texel(q.s1, q.t1), rgbaLow, rgbaHigh, corner(q, 3), depth(otz, depthScale));
}

} // namespace

// A sprite with a colour, a texel rectangle, rotation and mirroring.
void SpriteRender(ptr32<_sSprite> pSprite, ptr32<_sSpriteParams> pSpriteParams, f32 otz)
{
    renderSprite(pSprite, pSpriteParams, otz, SpriteSize::Normal, true, sym::lit4_327978);
}

// At half height (interlaced screens), always unfiltered.
void SpriteRenderHalf(ptr32<_sSprite> pSprite, ptr32<_sSpriteParams> pSpriteParams, f32 otz)
{
    renderSprite(pSprite, pSpriteParams, otz, SpriteSize::Half, false, sym::lit4_32797c);
}

// Scaled by xScale and yScale (8.8 fixed point).
void SpriteRenderScaled(ptr32<_sSprite> pSprite, ptr32<_sSpriteParams> pSpriteParams, f32 otz)
{
    renderSprite(pSprite, pSpriteParams, otz, SpriteSize::Scaled, true, sym::lit4_327980);
}

// A sprite at double width (wide-screen menus), with its texel corners chosen by the rotation flags and swapped by
// the mirror flags (here 0x10 swaps vertically and 0x20 horizontally). Each vertex has its own depth constant.
void SpriteRenderNew(ptr32<_sSprite> pSprite, ptr32<_sSpriteParams> pSpriteParams, uint32 otz)
{
    const rc::widescreen::AnchorScope uiScope(rc::gfx::AnchorCentre, rc::widescreen::AnchorScope::Ui); // widescreen: menus and 2D pillarboxed
    const _sSpriteParams &params = *pSpriteParams;
    const uint32 flags = params.flags;
    const uint8 spriteWidth = pSprite->width, spriteHeight = pSprite->height;
    uint8_t s0 = static_cast<uint8_t>(static_cast<uint8_t>(params.xOffset) + pSprite->xOffset);
    uint8_t t0 = static_cast<uint8_t>(static_cast<uint8_t>(params.yOffset) + pSprite->yOffset);
    const int32_t px = params.x, py = params.y;
    int32_t w = params.width, h = params.height;
    if ((flags & 6) != 0)
        std::swap(w, h);
    const int16_t bottom = static_cast<int16_t>(py + static_cast<int16_t>(h));
    const int16_t right = static_cast<int16_t>(px + static_cast<int16_t>(w) * 2);
    uint8_t s1 = static_cast<uint8_t>((w & 0xffff) + s0);
    uint8_t t1 = static_cast<uint8_t>((h & 0xffff) + t0);
    if ((flags & 0x10) != 0)
        std::swap(t0, t1);
    if ((flags & 0x20) != 0)
        std::swap(s0, s1);
    // Texel corners for screen top-left, top-right, bottom-left, bottom-right.
    uint8_t u[4], v[4];
    const auto set = [&](uint8_t a0, uint8_t b0, uint8_t a1, uint8_t b1, uint8_t a2, uint8_t b2, uint8_t a3, uint8_t b3) {
        u[0] = a0; v[0] = b0; u[1] = a1; v[1] = b1; u[2] = a2; v[2] = b2; u[3] = a3; v[3] = b3;
    };
    if ((flags & 0xe) == 0)
        set(s0, t0, s1, t0, s0, t1, s1, t1);
    else if ((flags & 2) != 0)
        set(s0, t1, s0, t0, s1, t1, s1, t0);
    else if ((flags & 4) != 0)
        set(s1, t1, s0, t1, s1, t0, s0, t0);
    else
        set(s1, t0, s1, t1, s0, t0, s0, t1);
    const uint8 blue = params.blue, red = params.red, green = params.green;
    GfxP2_TexExt(pSprite->pTextureHandle, static_cast<s32>((flags & 1) << 1), spriteAlphaType(flags));
    const ee_float fw = ee_itof(spriteWidth), fh = ee_itof(spriteHeight);
    const auto texel = [&](int i) {
        const ee_float fs = ee_itof(u[i]) / fw, ft = ee_itof(v[i]) / fh;
        uint32_t sb, tb;
        std::memcpy(&sb, &fs, 4);
        std::memcpy(&tb, &ft, 4);
        return sb | (static_cast<uint64_t>(tb) << 32);
    };
    const ee_float order = ee_itof(static_cast<int32_t>(otz));
    const uint64_t rgbaLow = red | (static_cast<uint64_t>(green) << 32), rgbaHigh = blue | 0x8000000000ull;
    const uint64_t x0 = gsCoord(static_cast<int16_t>(px) + 0x6c0), x1 = gsCoord(right + 0x6c0);
    const uint64_t y0 = gsCoord(static_cast<int16_t>(py) + 0x780), y1 = gsCoord(bottom + 0x780);
    SpriteStrip strip;
    strip.vertex(texel(0), rgbaLow, rgbaHigh, xy(x0, y0), depth(order, sym::lit4_327984));
    strip.vertex(texel(2), rgbaLow, rgbaHigh, xy(x0, y1), depth(order, sym::lit4_327988));
    strip.vertex(texel(3), rgbaLow, rgbaHigh, xy(x1, y1), depth(order, sym::lit4_32798c));
    strip.vertex(texel(0), rgbaLow, rgbaHigh, xy(x0, y0), depth(order, sym::lit4_327990));
    strip.vertex(texel(1), rgbaLow, rgbaHigh, xy(x1, y0), depth(order, sym::lit4_327994));
    strip.vertex(texel(3), rgbaLow, rgbaHigh, xy(x1, y1), depth(order, sym::lit4_327998));
}

// A font character: half height, at the farthest depth, coloured, through the effects texture setter.
void SpriteRenderFont(ptr32<_sSprite> pSprite, ptr32<_sSpriteParams> pSpriteParams, f32)
{
    const rc::widescreen::AnchorScope uiScope(rc::gfx::AnchorCentre, rc::widescreen::AnchorScope::Ui); // widescreen: menus and 2D pillarboxed
    const _sSpriteParams &params = *pSpriteParams;
    const uint8_t s0 = static_cast<uint8_t>(pSprite->xOffset + static_cast<uint8_t>(params.xOffset));
    const uint8_t w = static_cast<uint8_t>(params.width), h = static_cast<uint8_t>(params.height);
    const uint8_t t0 = static_cast<uint8_t>(pSprite->yOffset + static_cast<uint8_t>(params.yOffset));
    const int16_t px = params.x, py = params.y;
    const uint8_t s1 = static_cast<uint8_t>(s0 + w);
    const uint8 blue = params.blue;
    const int16_t right = static_cast<int16_t>(px + w);
    const uint8 red = params.red, green = params.green;
    const uint8_t t1 = static_cast<uint8_t>(t0 + h);
    const int16_t bottom = static_cast<int16_t>(py + (h >> 1));
    const uint8 spriteWidth = pSprite->width, spriteHeight = pSprite->height;
    void GfxP2_TexFX(ptr32<_sGfxTexture> pTex, u32 attrib);
    GfxP2_TexFX(pSprite->pTextureHandle, 0);
    const ee_float fw = ee_itof(spriteWidth), fh = ee_itof(spriteHeight);
    const auto texel = [&](uint8_t a, uint8_t b) {
        const ee_float fs = ee_itof(a) / fw, ft = ee_itof(b) / fh;
        uint32_t sb, tb;
        std::memcpy(&sb, &fs, 4);
        std::memcpy(&tb, &ft, 4);
        return sb | (static_cast<uint64_t>(tb) << 32);
    };
    const uint64_t rgbaLow = red | (static_cast<uint64_t>(green) << 32), rgbaHigh = blue | 0x8000000000ull;
    const uint64_t x0 = gsCoord(px + 0x6c0), x1 = gsCoord(right + 0x6c0), y0 = gsCoord(py + 0x780), y1 = gsCoord(bottom + 0x780);
    const uint64_t far = 0xffffff;
    SpriteStrip strip;
    strip.vertex(texel(s0, t0), rgbaLow, rgbaHigh, xy(x0, y0), far);
    strip.vertex(texel(s0, t1), rgbaLow, rgbaHigh, xy(x0, y1), far);
    strip.vertex(texel(s1, t1), rgbaLow, rgbaHigh, xy(x1, y1), far);
    strip.vertex(texel(s0, t0), rgbaLow, rgbaHigh, xy(x0, y0), far);
    strip.vertex(texel(s1, t0), rgbaLow, rgbaHigh, xy(x1, y0), far);
    strip.vertex(texel(s1, t1), rgbaLow, rgbaHigh, xy(x1, y1), far);
}

void SpriteInit()
{
    ResTypeSetConstructDaemon(ptr32<char>((sym::sec_sdata + 0x578u)), sym::SpriteConstructDaemon, 4);
}

} // namespace rc

RC_DECOMP_FN(SpriteConstructDaemon)
RC_DECOMP_FN(SpriteSetTextureHandle)
RC_DECOMP_FN(SpriteRenderSprite)
RC_DECOMP_FN(SpriteRenderFast)
RC_DECOMP_FN(SpriteRender)
RC_DECOMP_FN(SpriteRenderHalf)
RC_DECOMP_FN(SpriteRenderScaled)
RC_DECOMP_FN(SpriteRenderNew)
RC_DECOMP_FN(SpriteRenderFont)
RC_DECOMP_FN_NOFUZZ(SpriteInit)
