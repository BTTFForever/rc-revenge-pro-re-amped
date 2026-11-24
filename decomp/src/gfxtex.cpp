// Decompiled from GFX_TEX.c and TEXTURE.c: placing textures in GS memory. Each texture (and each of its mipmap
// levels) takes the next free block from nextTexAddr upwards and is uploaded through GFX_P2; palettes follow their
// texture. Animated textures (more than one frame) are re-uploaded every frame with the current frame's images.
// The uploads themselves still go through the SDK's GS library (guest_call).

#include "ee_math.h"
#include "rc_cars.h"
#include "rc_decomp.h"

// Europe variants: GfxTex_ManageTexture and GfxTex_Update store the counters and nextTexAddr in another order (the
// same body).
RC_VERSION_VARIANT(GfxTex_ManageTexture)
RC_VERSION_VARIANT(GfxTex_Update)

namespace rc {

void ResTypeSetConstructDaemon(ptr32<char> pType, uint32 constructDaemon, int16 options);

namespace {

const uint32_t kPrintf = sym::printf, kLoadTexMip = sym::GfxP2_LoadTexMip, kSetDefLoadImage = sym::sceGsSetDefLoadImage, kExecLoadImage = sym::sceGsExecLoadImage;
const uint32_t kResReadAlloc = sym::ResReadAlloc, kResReAlloc = sym::ResReAlloc;
const uint32_t kFirstTexAddr = 0x1e0000, kLastTexAddr = 0x400000;

// The GS memory a level of the given (padded) size takes, in units of 256 bytes, by format.
uint32_t levelSize(uint32_t fmt, int32_t w, int32_t h)
{
    const auto roundedUp = [](int32_t value, int32_t add, int shift) {
        const int32_t a = value + add;
        return ((a < 0 ? value + 2 * add : a) >> shift) * 0x100;
    };
    if (fmt < 2) // 32-bit
    {
        const int32_t size = roundedUp(w * h, 0x3f, 6);
        ++G(tex32bitCount);
        G(tex32bitSize) += static_cast<u32>(size);
        return static_cast<uint32_t>(size);
    }
    if (fmt == 0x13) // 8-bit palette
    {
        const int32_t size = roundedUp(w * h, 0xff, 8);
        ++G(tex8bitCount);
        G(tex8bitSize) += static_cast<u32>(size);
        return static_cast<uint32_t>(size);
    }
    if (fmt == 0x14) // 4-bit palette
    {
        const int32_t size = roundedUp((w * h) >> 1, 0xff, 8);
        ++G(tex4bitCount);
        G(tex4bitSize) += static_cast<u32>(size);
        return static_cast<uint32_t>(size);
    }
    const int32_t size = roundedUp(w * h, 0x7f, 7); // 16-bit
    ++G(tex16bitCount);
    G(tex16bitSize) += static_cast<u32>(size);
    return static_cast<uint32_t>(size);
}

uint64_t mipBase(const _sGfxTexture &tex, int first)
{
    const auto idx = [&](int i) { return static_cast<uint64_t>(static_cast<int64_t>(tex.idxList[i])); };
    return idx(first) | 0x40000000004000ull | (idx(first + 2) << 40) | 0x400000000ull | (idx(first + 1) << 20);
}

} // namespace

void GfxTex_Init()
{
    cars::texturesReset(); // enhancement: the cars' texture banks (rc_cars.h) go with the memory
    G(gNumAnimTextures) = 0;
    G(nextTexAddr) = kFirstTexAddr;
    G(framecount) = 0;
}

void GfxTex_Reset()
{
    GfxTex_Init();
    G(tex4bitCount) = 0;
    G(tex8bitCount) = 0;
    G(tex16bitCount) = 0;
    G(tex32bitCount) = 0;
    G(tex4bitSize) = 0;
    G(tex8bitSize) = 0;
    G(tex16bitSize) = 0;
    G(tex32bitSize) = 0;
}

void GfxTex_Term() {}

// Gives a texture its place in GS memory and uploads it: each mipmap level (as many as the flags ask for, while
// the smaller side stays above 4), padded to sizes the GS accepts, then its palette.
void GfxTex_ManageTexture(ptr32<_sGfxTexture> pTexture)
{
    GuestFrame frame(0x150);
    cars::bankTexture(pTexture.address);
    _sGfxTexture &tex = *pTexture;
    uint32_t levels = (tex.flags & 0x1c) >> 2;
    int32_t w = tex.wd, h = tex.ht;
    if (levels != 0)
    {
        // No more levels than halvings of the smaller side down to 4.
        int32_t side = h < w ? h : w;
        uint32_t possible = 0;
        bool cap = true;
        if (side >= 5)
        {
            do
            {
                side >>= 1;
                ++possible;
            } while (4 < side);
            cap = possible < levels;
        }
        if (cap)
            levels = possible;
        tex.flags = (tex.flags & 0xffffffe3u) | (levels << 2);
    }
    for (uint32_t level = 0;; ++level)
    {
        h >>= level & 31;
        w >>= level & 31;
        if (w < 0x10)
            w = 0x10;
        int32_t doubleW = w << 1;
        if (h < 8)
            h = 8;
        // Widen or heighten until the sides are equal or one is twice the other.
        if (doubleW != h && (w >> 1) != h && w != h)
        {
            bool narrower = w < h;
            do
            {
                if (narrower)
                {
                    w = doubleW;
                    doubleW <<= 1;
                }
                else
                    h <<= 1;
            } while (doubleW != h && (w >> 1) != h && (narrower = w < h, w != h));
        }
        if (w == 0x10 && h == 0x20)
            w = 0x20;
        else if (w == 0x20 && h == 0x40 && tex.fmt == 2)
        {
            if (levels == 0)
                tex.fmt = 10;
            else
            {
                guest_call(kPrintf, (sym::sec_rodata + 0x22b8u), level); // "Storing MIPMAP Level %d, 32x64 as 64x64\n"
                w = 0x40;
            }
        }
        else if (static_cast<uint32_t>(tex.fmt) - 0x13u < 2u && w < h)
            w = h;
        // Enhancement: a texture bank's level that would pass the end of GS memory is not uploaded (rc_cars.h).
        const uint32_t size = levelSize(static_cast<uint32_t>(tex.fmt), w, h);
        if (G(nextTexAddr) <= kLastTexAddr && cars::bankUpload(G(nextTexAddr), size))
            guest_call(kLoadTexMip, G(nextTexAddr), pTexture, level, 0);
        tex.idxList[level] = static_cast<s32>(G(nextTexAddr) >> 8);
        G(nextTexAddr) += size;
        if (levels < level + 1)
            break;
        w = tex.wd;
        h = tex.ht;
    }
    if (levels != 0)
    {
        guest_call(kPrintf, (sym::sec_rodata + 0x22e8u), levels); // "MIP Levels %d\n"
        const uint64_t mip1 = mipBase(tex, 1);
        tex.miptbp1L = static_cast<u32>(mip1);
        tex.miptbp1H = static_cast<u32>(mip1 >> 32);
        if (3 < levels)
        {
            const uint64_t mip2 = mipBase(tex, 4);
            tex.miptbp2L = static_cast<u32>(mip2);
            tex.miptbp2H = static_cast<u32>(mip2 >> 32);
        }
    }
    // The palette: 16x16 entries for 8-bit textures, 8x2 for 4-bit.
    if (tex.fmt == 0x13 || tex.fmt == 0x14)
    {
        const bool eightBit = tex.fmt == 0x13;
        const ptr32<void> image = frame.local<void>(eightBit ? 0 : 0x60);
        if (G(nextTexAddr) <= kLastTexAddr && cars::bankUpload(G(nextTexAddr), eightBit ? 0x400u : 0x100u))
        {
            guest_call(kSetDefLoadImage, image, static_cast<int32_t>(G(nextTexAddr) << 8) >> 16, 1, 0, 0, 0, eightBit ? 0x10 : 8, eightBit ? 0x10 : 2);
            guest_call(kExecLoadImage, image, tex.pClut);
        }
        tex.clutVramAddr = static_cast<s32>(G(nextTexAddr) >> 8);
        G(nextTexAddr) += eightBit ? 0x400 : 0x100;
    }
    if (1 < tex.frames)
    {
        const int32_t index = G(gNumAnimTextures);
        G(gNumAnimTextures) = index + 1;
        *static_cast<ptr32<_sGfxTexture> *>(guest_host(guest_addr(&G(gpAnimTextures)) + static_cast<uint32_t>(index) * 4u)) = pTexture;
    }
}

// Re-uploads every animated texture's levels with the frame for this point in time (a new frame every 3).
void GfxTex_Update()
{
    GuestFrame frame(0x80);
    // Side tag: the animated textures' uploads (each also labelled by GfxP2_LoadTexMip).
    const rc::gfx::SpanKicks uploads(rc::gfx::KickLabel{rc::gfx::KickAnimatedTexture, 0u, static_cast<uint32_t>(G(gNumAnimTextures)), 0u});
    G(framecount) = G(framecount) + 1;
    for (int32_t i = 0; i < G(gNumAnimTextures); ++i)
    {
        const int32_t step = static_cast<int32_t>(G(framecount)) / 3;
        const ptr32<_sGfxTexture> tex = G(gpAnimTextures)[i];
        const int32_t frames = static_cast<int32_t>(tex->frames);
        const uint32_t flags = tex->flags;
        const int32_t current = frames == 0 ? step : (step == INT32_MIN && frames == -1 ? 0 : step % frames);
        for (int32_t level = 0; level <= static_cast<int32_t>(flags & 0x1c) >> 2; ++level)
            guest_call(kLoadTexMip, static_cast<uint32_t>(tex->idxList[level]) << 8, tex, level, current);
    }
}

// TEXTURE.c

void TexReset()
{
    GfxTex_Reset();
}

void TexInit()
{
    GfxTex_Init();
    ResTypeSetConstructDaemon(ptr32<char>((sym::sec_sdata + 0x560u)), sym::TextureConstructDaemon, 2);
}

// Places a loaded texture in GS memory. One that is a single frame and not flagged otherwise is then moved: its
// 0x50-byte header is copied to the end of the loader's read area (just below what it has read so far), so the
// bitmap data that follows the header can be released when the loader trims the area.
void TextureConstructDaemon(ptr32<memHandle> pHandle)
{
    GuestFrame frame(0x30);
    if (!pHandle)
        return;
    const ptr32<_sGfxTexture> texture(pHandle->address);
    GfxTex_ManageTexture(texture);
    if (texture->frames == 1 && (texture->flags & 0x8000) == 0)
    {
        const uint32_t end = guest_call<uint32_t>(kResReadAlloc);
        const uint32_t moved = end - 0x50u;
        std::memcpy(guest_host(moved), texture.get(), 0x50);
        guest_call(kResReAlloc, moved);
        *pHandle = ptr32<void>(moved);
    }
}

} // namespace rc

RC_DECOMP_FN(GfxTex_Init)
RC_DECOMP_FN(GfxTex_Reset)
RC_DECOMP_FN(GfxTex_Term)
RC_DECOMP_FN_NOFUZZ(GfxTex_ManageTexture) // Uploads to the GS.
RC_DECOMP_FN_NOFUZZ(GfxTex_Update)
RC_DECOMP_FN(TexReset)
RC_DECOMP_FN_NOFUZZ(TexInit)
RC_DECOMP_FN_NOFUZZ(TextureConstructDaemon) // Uploads to the GS.
