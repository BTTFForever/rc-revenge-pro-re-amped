// Decompiled from SPRITE.c: 2D sprites for the HUD, menus and fonts, drawn straight to the GS through GFX_P2's
// packet buffer. A sprite is two triangles, six vertices of GIF "packed" data (texture coordinates, Q, colour,
// position); every third vertex kicks a triangle.

#include "ee_math.h"

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

uint64_t depth(ee_float otz)
{
    const ee_float scale = *static_cast<ee_float *>(guest_host(0x327974u));
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
    const uint8 width = pSprite->width, height = pSprite->height;
    GfxP2_TexExt(pSprite->pTextureHandle, 0, 0);
    const int32_t left = x + 0x6c0;
    const uint64_t z = depth(otz);
    const uint64_t x0 = gsCoord(left);
    const uint64_t yTop = gsCoord(y + 0x780), yBottom = gsCoord(y + 0x780 + height);
    const uint64_t x1 = gsCoord(left + width);
    SpriteStrip strip;
    strip.vertex(st(0.0f, 1.0f), kWhite, kWhiteBA, xy(x0, yTop), z);
    strip.vertex(st(0.0f, 0.0f), kWhite, kWhiteBA, xy(x0, yBottom), z);
    strip.vertex(st(1.0f, 0.0f), kWhite, kWhiteBA, xy(x1, yBottom), z);
    strip.vertex(st(0.0f, 1.0f), kWhite, kWhiteBA, xy(x0, yTop), z);
    strip.vertex(st(1.0f, 1.0f), kWhite, kWhiteBA, xy(gsCoord(left + pSprite->width), yTop), z);
    strip.vertex(st(1.0f, 0.0f), kWhite, kWhiteBA, xy(x1, yBottom), depth(otz));
}

void SpriteInit()
{
    ResTypeSetConstructDaemon(ptr32<char>(0x328178u), 0x1897a0u, 4);
}

} // namespace rc

RC_DECOMP_FN(SpriteConstructDaemon, 0x1897a0)
RC_DECOMP_FN(SpriteSetTextureHandle, 0x1897c8)
RC_DECOMP_FN(SpriteRenderSprite, 0x1897d0)
RC_DECOMP_FN(SpriteRenderFast, 0x185830)
RC_DECOMP_FN_NOFUZZ(SpriteInit, 0x185818)
