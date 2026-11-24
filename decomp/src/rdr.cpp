// Decompiled from the renderer's lowest layer: RDRSTATE.c (blend, test, scissor and depth state for the graphics
// chip), RDR.c (the frame's DMA chain), RENDER.c and TEXTURE.c (setup), and the trivial parts of GFX_P2.c.
//
// Drawing on the PS2 is a chain of DMA tags that VIF1 walks once a frame: each "call" tag points at a packet to run,
// such as a block of graphics chip register writes or a microprogram upload. gRdr.pDmaTag holds two chains of 1000
// tags, one per frame, swapped each frame.

#include "rc_settings.h" // enhanced recomp: widescreen
#include "../rc_frame_tick.h"
#include "rc_widescreen.h"
#include "display_fuzz.h"
#include "ee_math.h"
#include "vifpk.h"

#include <initializer_list>

namespace rc {

bool_ MemBlockAllocLo(ptr32<memHandle> pHandle, uint32 size);
void MemBlockFree(ptr32<memHandle> pHandle);
void ResTypeSetConstructDaemon(ptr32<char> pType, uint32 constructDaemon, int16 options);

namespace {

// Microprograms: the shape renderer's and the track renderer's. The state packets start whichever is loaded.
const uint32_t kShapeProgram = sym::TRdr_VUShapeProg;     // _dma_48
const uint32_t kTrackProgramEnd = sym::EnvRdr_VUShapeProg;  // gTRdr_EndShapeData
const uint32_t kStateBlocks = 0x100;         // Packets in the state packet ring.

// GS register numbers.
const uint64_t kGsAlpha1 = 0x42, kGsClamp1 = 0x08, kGsScissor1 = 0x40, kGsTest1 = 0x47, kGsZbuf1 = 0x4e, kGsPabe = 0x49;

uint64_t scissorValue(u32 x1, u32 y1, u32 x2, u32 y2)
{
    return static_cast<uint64_t>(x1 | (x2 << 16)) | (static_cast<uint64_t>(y1) << 32) | (static_cast<uint64_t>(y2) << 48);
}

} // namespace

// RDR_SHAPE.c

u32 Rdr_WhichProg()
{
    return G(rdrShapeCurrProg);
}

// RDR.c

// Adds a call to a packet to this frame's DMA chain.
void Rdr_AddDmaCall(ptr32<void> pPacket)
{
    sRdr &rdr = G(gRdr);
    sDmaTag &tag = rdr.pDmaTag[rdr.currDmaTag];
    rdr.currDmaTag = rdr.currDmaTag + 1;
    tag.id = 'P'; // call
    tag.next = pPacket;
    tag.qwc = 0;
    tag.mark = 0;
}

// Loads a microprogram into VU1 unless it is already there.
void Rdr_UploadProg(ptr32<u32> pProg)
{
    if (pProg.address != G(rdrShapeCurrProg))
    {
        Rdr_AddDmaCall(pProg.cast<void>());
        G(rdrShapeCurrProg) = pProg.address;
    }
}

void Rdr_UseCamera(ptr32<_sRdrCamera> pCamera)
{
    G(gRdr).pCamera = pCamera;
}

// Never written: the game draws no debug points, triangles or quads.
void Rdr_DrawPoint(ptr32<_sVec3>, u32) {}
void Rdr_DrawTri(ptr32<_sRdrTri>) {}
void Rdr_DrawQuad(ptr32<_sRdrTri>) {}

void Rdr_Term()
{
    MemBlockFree(ptr32<memHandle>(guest_addr(&G(gRdr).pDmaTag)));
}

void RdrState_Init();
void GfxP2_EndScene();
void GfxTex_Update(); // gfxtex.cpp

namespace {

constexpr uint32_t kAngleToRadians = sym::lit4_327bd0, kCameraMinZ = sym::lit4_327bd4, kCameraMaxZ = sym::lit4_327bd8, kAspect = sym::lit4_327bdc;
constexpr uint32_t kScreenLimit = sym::lit4_327be0, kLineDepth1 = sym::lit4_327be4, kLineDepth2 = sym::lit4_327be8;
constexpr uint32_t kGfxP2_SwapBuffers = sym::GfxP2_SwapBuffers, kSceGsSyncPath = sym::sceGsSyncPath, kFlushCache = sym::FlushCache;
// Hardware registers: VIF1's error mask, DMA channel 1 (VIF1) and the DMA controller's status.
constexpr uint32_t kVif1Err = 0x10003c20u, kD1Chcr = 0x10009000u, kD1Qwc = 0x10009020u, kD1Tadr = 0x10009030u,
                   kDStat = 0x1000e010u;

ee_float &fat(uint32_t address) { return *static_cast<ee_float *>(guest_host(address)); }
uint64_t &qat(uint32_t address) { return *static_cast<uint64_t *>(guest_host(address)); }

// Starts VIF1's DMA on a chain of tags (its error mask set, the channel's interrupt cleared), with the cache flushed.
void startVif1Chain(uint32_t tags)
{
    // Side tag: this is K2, the frame's VIF1 chain.
    const rc::gfx::SpanKicks k2(rc::gfx::KickLabel{rc::gfx::KickK2, tags, G(gRdr).frameNumber, 0u});
    hw_write32(kVif1Err, 2);
    hw_write32(kD1Qwc, 0);
    hw_write32(kD1Tadr, tags & 0x0fffffffu);
    hw_write32(kDStat, 2);
    guest_call(kFlushCache, 0);
    hw_write32(kD1Chcr, 0x145);
}

} // namespace

// The camera for a field of view (a 16-bit angle): the projection scales, the near and far planes, the normals of the
// four side planes (each made by rotating an axis through the half angle) and an identity orientation.
void Rdr_SetupCamera(ptr32<_sRdrCamera> pCamera, tAngle viewAngle)
{
    _sRdrCamera &cam = *pCamera;
    const ee_float one(1.0f), half(0.5f), zero(0.0f);
    const ee_float x = ee_itof(static_cast<int32_t>(static_cast<uint32_t>(viewAngle) >> 1)) * fat(kAngleToRadians);
    cam.viewAngle = viewAngle;
    ee_float d = ee_sinf(x);
    d = d / ee_cosf(x);
    cam.minZ = fat(kCameraMinZ);
    const ee_float width = ee_itof(G(gGfxP2).wd);
    cam.maxZ = fat(kCameraMaxZ);
    d = one / d;
    const ee_float aspect = width * fat(kAspect);
    const ee_float halfD = d * half;
    cam.normDVal = d;
    const ee_float scaledD = d * width;
    cam.dValY = halfD * aspect;
    cam.dValX = scaledD * half;
    cam.dVal = scaledD * half;
    // Widescreen (rc_settings): Hor+, the x projection scaled by (4/3)/aspect and the side planes widened.
    const ee_float sideHalf = rc::settings::widescreen() ? ee_float(rc::widescreen::widenHalfAngle(static_cast<float>(x))) : x;
    if (rc::settings::widescreen())
        cam.dValX = cam.dValX * ee_float(rc::settings::hscale());

    // Right and left: the X axis turned about Y (only two terms of the middle row are used).
    {
        const ee_float s = ee_sinf(sideHalf), c = ee_cosf(sideHalf);
        const ee_float m[9] = {c, zero, -s, zero, one, zero, s, zero, c};
        ee_float v[3] = {one, zero, zero};
        cam.normRight.x = v[0] * m[0] + v[1] * m[3] + v[2] * s;
        cam.normRight.y = v[0] * m[1] + v[1] * m[4];
        cam.normRight.z = v[0] * m[2] + v[1] * m[5] + v[2] * m[8];
        v[0] = -one;
        v[1] = v[2] = zero;
        cam.normLeft.x = -m[0];
        cam.normLeft.y = -m[3];
        cam.normLeft.z = v[0] * m[6] + v[1] * m[7] + v[2] * m[8];
    }
    // Bottom and top: the Y axis turned about X.
    {
        const ee_float s = ee_sinf(x), c = ee_cosf(x);
        const ee_float m[9] = {one, zero, zero, zero, c, s, zero, -s, c};
        ee_float v[3] = {zero, -one, zero};
        cam.normBottom.x = v[0];
        cam.normBottom.y = v[1] * c + v[2] * -s;
        cam.normBottom.z = v[0] * m[2] + v[1] * m[5] + v[2] * m[8];
        v[0] = zero;
        v[1] = one;
        v[2] = zero;
        cam.normTop.x = m[1];
        cam.normTop.y = m[4];
        cam.normTop.z = v[0] * m[6] + v[1] * m[7] + v[2] * m[8];
    }
    sMat33 &mat = cam.mat;
    mat.Xx = one;
    mat.Xy = zero;
    mat.Xz = zero;
    mat.Yx = zero;
    mat.Yy = one;
    mat.Yz = zero;
    mat.Zx = zero;
    mat.Zy = zero;
    mat.Zz = one;
    // The orientation as a quaternion (the usual conversion; the matrix is the identity, so only the first case runs).
    const uint32_t m = guest_addr(&mat), q = guest_addr(&cam.qtn);
    const ee_float xx = mat.Xx;
    const ee_float trace = xx + one + one;
    if (zero < trace)
    {
        const ee_float r = ee_sqrt(trace + one);
        const ee_float k = half / r;
        cam.qtn.w = r * half;
        cam.qtn.x = (mat.Yz - mat.Zy) * k;
        cam.qtn.y = (mat.Zx - mat.Xz) * k;
        cam.qtn.z = (mat.Xy - mat.Yx) * k;
    }
    else
    {
        uint32_t i = xx < one ? 1u : 0u;
        if (fat(m + i * 16u) < one)
            i = 2;
        const ee_float diagonal = fat(m + i * 16u);
        const uint32_t j = static_cast<uint32_t>(G(gaQtnNxt)[i]), k = static_cast<uint32_t>(G(gaQtnNxt)[j]);
        const auto at = [m](uint32_t row, uint32_t column) { return m + row * 12u + column * 4u; };
        const ee_float r = ee_sqrt(diagonal - (fat(m + j * 16u) + fat(m + k * 16u)) + one);
        fat(q + 4u + i * 4u) = r * half;
        const ee_float inv = half / r;
        fat(q) = (fat(at(j, k)) - fat(at(k, j))) * inv;
        fat(q + 4u + j * 4u) = (fat(at(i, j)) + fat(at(j, i))) * inv;
        fat(q + 4u + k * 4u) = (fat(at(i, k)) + fat(at(k, i))) * inv;
    }
    cam.pos.x = zero;
    cam.pos.z = zero;
    cam.pos.y = zero;
}

void Rdr_Init()
{
    sRdr &rdr = G(gRdr);
    const ptr32<_sRdrCamera> camera(guest_addr(&G(gRdrCamera)));
    Rdr_SetupCamera(camera, 0x2aaa);
    rdr.pCamera = camera;
    rdr.frameNumber = 0;
    rdr.pDmaTag = nullptr;
    MemBlockAllocLo(ptr32<memHandle>(guest_addr(&rdr.pDmaTag)), 32000);
    std::memset(guest_host(rdr.pDmaTag.address), 0, 32000);
    rdr.currDmaTag = 0;
    RdrState_Init();
}

// Ends the frame: closes the GS packet, swaps the buffers (waiting for the vertical blank), ends this frame's tag chain
// and starts VIF1 on it (the chains of even and odd frames are 1000 tags apart).
void Rdr_EndFrame()
{
    sRdr &rdr = G(gRdr);
    GfxP2_EndScene();
    guest_call(kGfxP2_SwapBuffers); // can block
    const int32_t n = rdr.currDmaTag;
    const uint32_t tags = rdr.pDmaTag.address;
    rdr.currDmaTag = n + 1;
    sDmaTag &end = *ptr32<sDmaTag>(tags + static_cast<uint32_t>(n) * 16u);
    end.id = 0x70; // end
    end.qwc = 0;
    end.mark = 0;
    end.next = nullptr;
    // Enhancement: between logic ticks (rc_frame_tick.h) the animated textures are uploaded again at the same frame
    // (framecount / 3 is their clock: stepping it every picture ran them 2.4 times fast at 144 Hz).
    if (!frametick::logicPass())
        G(framecount) = G(framecount) - 1;
    GfxTex_Update();
    guest_call(kSceGsSyncPath, 0, 0);
    startVif1Chain((rdr.frameNumber & 1u) == 0 ? rdr.pDmaTag.address : rdr.pDmaTag.address + 1000u * 16u);
    rdr.frameNumber = rdr.frameNumber + 1u;
}

// A debug line in screen space, clipped at the near plane and dropped if either end is off screen: two vertices of a
// RGBAQ/XYZ2 GIF tag in the GS packet (a new tag when there is none open or the open one is nearly full).
void Rdr_DrawLine(ptr32<_sVec3> p1, ptr32<_sVec3> p2, u32 colour)
{
    const uint32_t cam = G(gRdr).pCamera.address;
    const auto toView = [cam](ptr32<_sVec3> p, ee_float &x, ee_float &y, ee_float &z)
    {
        _sVec3 v;
        std::memcpy(&v, p.get(), sizeof(v));
        const ee_float dx = v.x - fat(cam + 0xbcu), dy = v.y - fat(cam + 0xc0u), dz = v.z - fat(cam + 0xc4u);
        x = dx * fat(cam + 0x98u) + dy * fat(cam + 0x9cu) + dz * fat(cam + 0xa0u);
        y = dx * fat(cam + 0xa4u) + dy * fat(cam + 0xa8u) + dz * fat(cam + 0xacu);
        z = dx * fat(cam + 0xb0u) + dy * fat(cam + 0xb4u) + dz * fat(cam + 0xb8u);
    };
    ee_float x1, y1, z1, x2, y2, z2;
    toView(p1, x1, y1, z1);
    toView(p2, x2, y2, z2);
    const ee_float minZ = fat(cam + 0x14u);
    if (z1 < minZ)
    {
        if (z2 < minZ)
            return;
        const ee_float t = (z2 - minZ) / (z2 - z1);
        x1 = x2 + t * (x1 - x2);
        y1 = y2 + t * (y1 - y2);
        z1 = fat(cam + 0x14u);
    }
    else if (z2 < minZ)
    {
        const ee_float t = (z1 - minZ) / (z1 - z2);
        y2 = y1 + t * (y2 - y1);
        x2 = x1 + t * (x2 - x1);
        z2 = fat(cam + 0x14u);
    }
    const ee_float one(1.0f), centre(2048.0f), zero(0.0f), sixteen(16.0f);
    const ee_float inv1 = one / z1;
    const ee_float dValX = fat(cam + 0xcu), dValY = fat(cam + 0x10u), limit = fat(kScreenLimit);
    const ee_float sx1 = x1 * inv1 * dValX + centre, sy1 = centre - y1 * inv1 * dValY;
    const ee_float depth1 = one - inv1;
    const ee_float inv2 = one / z2;
    const ee_float sy2 = centre - y2 * inv2 * dValY, sx2 = x2 * inv2 * dValX + centre;
    const ee_float depth2 = one - inv2;
    if (limit < sx1 || sx1 < zero || limit < sy1 || sy1 < zero || limit < sx2 || sx2 < zero || limit < sy2 || sy2 < zero)
        return;

    sGfxP2 &gfx = G(gGfxP2);
    const uint64_t rgba = ((colour >> 16) & 0xffu) | (static_cast<uint64_t>((colour >> 8) & 0xffu) << 32);
    const uint64_t blue = colour & 0xffu;
    const uint64_t kNewTag = 0x2000000000000000ull, kRegs = 0x51; // NREG 2: RGBAQ, XYZ2
    // The open tag, or a new one at the vertex's place (which then moves past it).
    const auto open = [&gfx, kNewTag, kRegs](int32_t &index, uint32_t base, int32_t tagIndex, uint32_t &at) -> uint32_t
    {
        at = base + static_cast<uint32_t>(index) * 16u;
        if (tagIndex != -1)
        {
            const uint32_t tag = base + static_cast<uint32_t>(tagIndex) * 16u;
            if ((qat(tag) & 0x7fffu) < 0x7ffdu)
                return tag;
        }
        const uint32_t tag = at;
        qat(at) = kNewTag;
        qat(at + 8u) = kRegs;
        gfx.gifTagIdx = index;
        gfx.buffIdx = index + 1;
        ++index;
        at += 16u;
        return tag;
    };
    const auto count = [](uint64_t tag) { return (tag & 0xffffffffffff0000ull) | ((tag & 0x7fffu) + 1u); };
    const auto xy = [](int32_t x, int32_t y)
    { return static_cast<uint64_t>(static_cast<uint32_t>(x) & 0xffffu) | (static_cast<uint64_t>(static_cast<uint32_t>(y) & 0xffffu) << 32); };

    // The globals are read where the original reads them, between the packet's stores.
    int32_t index = gfx.buffIdx;
    uint32_t base = gfx.pDmaBuff.address;
    int32_t tagIndex = gfx.gifTagIdx;
    uint32_t at = 0;
    uint32_t tag = open(index, base, tagIndex, at);
    qat(at) = rgba;
    qat(at + 8u) = blue;
    const int32_t y1i = ee_ftoi(sy1 * sixteen), x1i = ee_ftoi(sx1 * sixteen);
    const int32_t z1i = ee_ftoi((one - depth1) * fat(kLineDepth1));
    uint64_t value = qat(tag);
    gfx.buffIdx = index + 2;
    index = gfx.buffIdx;
    qat(tag) = count(value);
    base = gfx.pDmaBuff.address;
    qat(at + 0x18u) = static_cast<uint64_t>(static_cast<int64_t>(z1i));
    qat(at + 0x10u) = xy(x1i, y1i);
    tagIndex = gfx.gifTagIdx;

    tag = open(index, base, tagIndex, at);
    qat(at) = rgba;
    qat(at + 8u) = blue;
    const int32_t y2i = ee_ftoi(sy2 * sixteen), x2i = ee_ftoi(sx2 * sixteen);
    const int32_t z2i = ee_ftoi((one - depth2) * fat(kLineDepth2));
    value = qat(tag);
    qat(tag) = count(value);
    qat(at + 0x18u) = static_cast<uint64_t>(static_cast<int64_t>(z2i));
    gfx.buffIdx = index + 2;
    qat(at + 0x10u) = xy(x2i, y2i);
}

// Starts VIF1 on a DMA chain. Never called by the game.
void Rdr_Vif1Send(ptr32<u32> pData) { startVif1Chain(pData.address); }

// GFX_P2.c

void GfxP2_BeginScene()
{
    G(zWriteEnabled) = 0;
}

// Marks the frame's last GIF tag as the end of the packet, and forgets the last texture so the next frame sets it.
void GfxP2_EndScene()
{
    sGfxP2 &gfx = G(gGfxP2);
    const int32_t tag = gfx.gifTagIdx == -1 ? 0 : gfx.gifTagIdx;
    uint64_t &tagLow = *reinterpret_cast<uint64_t *>((gfx.pDmaBuff + tag).get());
    G(gLastTex) = nullptr;
    tagLow |= 0x8000; // EOP
}

// Starts this frame's DMA chain, and turns the camera's four side planes into world space for culling.
void Rdr_StartFrame()
{
    sRdr &rdr = G(gRdr);
    _sRdrCamera &camera = *rdr.pCamera;
    rdr.currDmaTag = (rdr.frameNumber & 1) == 0 ? 0 : 1000;
    const auto rotate = [&camera](const sVec3 &n, sVec3 &out)
    {
        const sMat33 &m = camera.mat;
        // Read before writing: the planes are separate fields, but keep the original's order of operations.
        const ee_float x = n.x;
        out.x = x * m.Xx + n.y * m.Yx + n.z * m.Zx;
        out.y = x * m.Xy + n.y * m.Yy + n.z * m.Zy;
        out.z = n.x * m.Xz + n.y * m.Yz + n.z * m.Zz;
    };
    rotate(camera.normRight, camera.rotRight);
    rotate(camera.normLeft, camera.rotLeft);
    rotate(camera.normBottom, camera.rotBottom);
    rotate(camera.normTop, camera.rotTop);
    GfxP2_BeginScene();
}

// GFX_P2.c: register writes appended to the GS packet buffer (gGfxP2.pDmaBuff), as GIF A+D data. Each ends the
// current GIF tag (gifTagIdx = -1), so the next primitive starts a new one.

void GfxP2_RenderDmaSend() {}

namespace {

uint64_t *gifQuadwords(sGfxP2 &gfx, int32_t count)
{
    uint64_t *at = reinterpret_cast<uint64_t *>((gfx.pDmaBuff + gfx.buffIdx).get());
    gfx.buffIdx = gfx.buffIdx + count;
    gfx.gifTagIdx = -1;
    return at;
}

} // namespace

// The default scissor and test registers.
void GfxP2_DefRenderState()
{
    uint64_t *q = gifQuadwords(G(gGfxP2), 4);
    q[0] = 0x1000000000000003ull; // 3 registers, A+D
    q[1] = 0xee;
    q[2] = 0xfd0003027d0003ull; // SCISSOR_1
    q[3] = 0x40;
    q[4] = 0x60; // PRMODECONT
    q[5] = 0x14;
    q[7] = 0x47; // TEST_1
    q[6] = 0x703f7;
}

void GfxP2_Scissor(int32_t x1, int32_t y1, int32_t x2, int32_t y2)
{
    uint64_t *q = gifQuadwords(G(gGfxP2), 4);
    q[0] = 0x1000000000000003ull;
    q[1] = 0xee;
    q[2] = static_cast<uint64_t>(static_cast<int64_t>(x1 | (x2 << 16))) | (static_cast<uint64_t>(static_cast<int64_t>(y1)) << 32) |
           (static_cast<uint64_t>(static_cast<int64_t>(y2)) << 48);
    q[3] = 0x40;
    q[4] = 0x60;
    q[5] = 0x14;
    q[7] = 0x47;
    q[6] = 0x70000;
}

void GfxP2_ZWriteEnable()
{
    uint64_t *q = gifQuadwords(G(gGfxP2), 2);
    q[0] = 0x1000000000000001ull;
    q[1] = 0xe;
    q[3] = 0x4e; // ZBUF_1
    q[2] = 0x10000a0;
    G(zWriteEnabled) = 1;
}

void GfxP2_ZWriteDisable()
{
    uint64_t *q = gifQuadwords(G(gGfxP2), 2);
    q[0] = 0x1000000000000001ull;
    q[1] = 0xe;
    q[3] = 0x4e;
    q[2] = 0x1010000a0ull; // ZMSK
    G(zWriteEnabled) = 0;
}

uint64_t gsTex0Of(const _sGfxTexture &texture);

namespace {

void gifRegisters(uint64_t *q, int32_t first, std::initializer_list<uint64_t> values)
{
    for (uint64_t value : values)
        q[first++] = value;
}

} // namespace

// Sets the GS for a sprite texture unless it is already set: TEX1 (no mipmaps), TEXA, then TEX0.
void GfxP2_Tex(ptr32<_sGfxTexture> pTex)
{
    RC_DIVERT_IF_CHANGED(GfxP2_Tex, pTex);
    if (pTex == G(gLastTex))
        return;
    sGfxP2 &gfx = G(gGfxP2);
    uint64_t *q = reinterpret_cast<uint64_t *>((gfx.pDmaBuff + gfx.buffIdx).get());
    gifRegisters(q, 0, {0x2000000000000001ull, 0xe0, 0x5b, 0, 0, 0x49, 0x2000000000000001ull, 0x6e, 9, 0x42});
    G(gLastTex) = pTex;
    q[10] = gsTex0Of(*pTex);
    q[11] = 0;
    gfx.buffIdx = gfx.buffIdx + 6;
    gfx.gifTagIdx = -1;
}

// As GfxP2_Tex, with filtering (attrib 1: bilinear, 2: blended) and the blend for an alpha type.
void GfxP2_TexExt(ptr32<_sGfxTexture> pTex, s32 attrib, s32 alphaType)
{
    RC_DIVERT_IF_CHANGED(GfxP2_TexExt, pTex, attrib, alphaType);
    if (pTex == G(gLastTex))
        return;
    sGfxP2 &gfx = G(gGfxP2);
    uint64_t *q = reinterpret_cast<uint64_t *>((gfx.pDmaBuff + gfx.buffIdx).get());
    const uint64_t blended = (attrib & 2) != 0 ? 1 : 0;
    gifRegisters(q, 0, {0x2000000000000001ull, 0xe0, (static_cast<uint64_t>(attrib & 1) << 3) | (blended << 6) | 0x13, 0, 0, 0x49,
                        0x3000000000000001ull, 0xe6e});
    G(gLastTex) = pTex;
    uint64_t alpha;
    if (blended == 0)
        alpha = 0x3f00000029ull;
    else if ((alphaType & 1) == 0)
        alpha = (alphaType & 2) == 0 ? 0x3f00000021ull : 0x7f000000a1ull;
    else
        alpha = 0x7f00000029ull;
    q[8] = alpha; // ALPHA_1
    q[9] = 0x42;
    q[10] = gsTex0Of(*pTex);
    q[11] = 0;
    q[13] = 0x47;
    q[12] = 0x703f7; // TEST_1
    gfx.buffIdx = gfx.buffIdx + 7;
    gfx.gifTagIdx = -1;
}

// As GfxP2_Tex for effects: clamping (attrib 0x10: repeat), alpha test (0x20), additive (2) or subtractive (4)
// blending.
void GfxP2_TexFX(ptr32<_sGfxTexture> pTex, u32 attrib)
{
    RC_DIVERT_IF_CHANGED(GfxP2_TexFX, pTex, attrib);
    if (pTex == G(gLastTex))
        return;
    sGfxP2 &gfx = G(gGfxP2);
    uint64_t *q = reinterpret_cast<uint64_t *>((gfx.pDmaBuff + gfx.buffIdx).get());
    G(gLastTex) = pTex;
    gifRegisters(q, 0, {0x4000000000000001ull, 0xeee0, 0x5b, 0, 0, 0x49, (attrib & 0x10) == 0 ? 5u : 0u, 8,
                        (attrib & 0x20) == 0 ? 0x703f7u : 0x73017u, 0x47, 0x2000000000000001ull, 0x6e});
    uint64_t blend;
    if ((attrib & 2) != 0)
        blend = 0x8000000029ull;
    else if ((attrib & 4) != 0)
        blend = 0x80000000a1ull;
    else
        blend = (attrib & 0x20) == 0 ? 9 : 1;
    q[12] = blend;
    q[13] = 0x42;
    q[14] = gsTex0Of(*pTex);
    q[15] = 0;
    gfx.buffIdx = gfx.buffIdx + 8;
    gfx.gifTagIdx = -1;
}

// RDRSTATE.c

void RdrState_SetAlphaBlending(ptr32<sRdrStateBlock> block, u32 A, u32 B, u32 C, u32 D, u8 fixedAlpha)
{
    block->alpha = static_cast<uint64_t>(A | (D << 6) | (C << 4) | (B << 2)) | (static_cast<uint64_t>(fixedAlpha) << 32);
}

void RdrState_SetAlphaTestEnable(ptr32<sRdrStateBlock> block, u32 enable, u32 func, u32 fail, u8 ref)
{
    const uint64_t kept = block->test & 0xffffffffffffc000ull;
    block->test = kept;
    if (enable != 0)
        block->test = kept | 1u | static_cast<uint64_t>(func << 1) | static_cast<uint64_t>(fail << 12) | (static_cast<uint64_t>(ref) << 4);
}

// With the test off, the depth test is set to always pass rather than disabled.
void RdrState_SetZTestEnable(ptr32<sRdrStateBlock> block, u32 enable, u32 func)
{
    const uint64_t kept = block->test & 0xfffffffffff8ffffull;
    block->test = kept;
    block->test = enable != 0 ? kept | 0x10000u | static_cast<uint64_t>(func << 17) : kept | 0x30000u;
}

void RdrState_SetTextureClamp(ptr32<sRdrStateBlock> block, u32 clampMode)
{
    block->clamp = clampMode;
}

// Without a block, sets the scissor every state packet uses.
void RdrState_SetScissor(ptr32<sRdrStateBlock> block, u32 x1, u32 y1, u32 x2, u32 y2)
{
    if (block)
        block->scissor = scissorValue(x1, y1, x2, y2);
    else
        G(gRdrStateScissorOverride) = scissorValue(x1, y1, x2, y2);
}

void RdrState_SetZBufferWriteEnable(ptr32<sRdrStateBlock> block, u32 enable)
{
    const uint64_t kept = block->zbuf & 0xfffffffeffffffffull;
    block->zbuf = kept;
    if (enable == 0)
        block->zbuf = kept | 0x100000000ull; // ZMSK
}

void RdrState_SetPixelAlphaBlendControlEnable(ptr32<sRdrStateBlock> block, u32 enable)
{
    block->pabe = enable;
}

void RdrState_SetStateBlockToDefaults(ptr32<sRdrStateBlock> block)
{
    RC_DIVERT_IF_CHANGED(RdrState_SetStateBlockToDefaults, block);
    block->alpha = 0x44;
    block->pabe = 1;
    block->clamp = 5;
    block->scissor = 0xed0003027d0003ull;
    block->test = 0x702f7;
    block->zbuf = 0x10000a0;
}

// Sends a state block: fills the next packet of the ring with its register writes, ends it by restarting whichever
// shape microprogram is loaded (loading the shape renderer's if neither is), and calls it from the DMA chain.
void RdrState_ForceApplyStateBlock(ptr32<sRdrStateBlock> block)
{
    const uint64_t scissor = G(gRdrStateScissorOverride);
    int32_t next = G(g_RdrStateCurrBlock) + 1;
    if (static_cast<int32_t>(kStateBlocks - 1) < next)
        next = 0;
    const ptr32<anon_9d38028c> packetPtr = G(g_RdrStateDmaBlocks) + G(g_RdrStateCurrBlock);
    G(g_RdrStateCurrBlock) = next;
    anon_9d38028c &packet = *packetPtr;
    packet.reg1_value = block->alpha;
    packet.reg1_register = kGsAlpha1;
    packet.reg2_register = kGsClamp1;
    packet.reg2_value = block->clamp;
    packet.reg3_value = scissor;
    packet.reg3_register = kGsScissor1;
    packet.reg4_register = kGsTest1;
    packet.reg4_value = block->test;
    packet.reg5_register = kGsZbuf1;
    packet.reg5_value = block->zbuf;
    packet.reg6_register = kGsPabe;
    packet.reg6_value = block->pabe;
    if (Rdr_WhichProg() == kShapeProgram)
        packet.vifMscal = (0x2050u >> 3) | 0x14000000u;
    else if (Rdr_WhichProg() == kTrackProgramEnd)
        packet.vifMscal = (0x2370u >> 3) | 0x14000000u;
    else
    {
        Rdr_UploadProg(ptr32<u32>(kShapeProgram));
        packet.vifMscal = 0x1400040a;
    }
    Rdr_AddDmaCall(packetPtr.cast<void>());
}

// The same: the game never skips state the hardware already has.
void RdrState_ApplyStateBlock(ptr32<sRdrStateBlock> block)
{
    RdrState_ForceApplyStateBlock(block);
}

void RdrState_ApplyDefaultStateBlock()
{
    RdrState_ForceApplyStateBlock(ptr32<sRdrStateBlock>(guest_addr(&G(g_RdrDefaultRenderState))));
}

// Allocates the ring of 256 state packets and fills in the parts that never change: the DMA, VIF and GIF headers.
void RdrState_Init()
{
    RC_DIVERT_IF_CHANGED(RdrState_Init);
    MemBlockAllocLo(ptr32<memHandle>(guest_addr(&G(g_RdrStateDmaBlocks))), 0x9000);
    sRdrStateBlock &hardware = G(g_RdrStateOfHardware);
    hardware.pabe = ~0ull;
    hardware.alpha = ~0ull;
    hardware.clamp = ~0ull;
    hardware.scissor = ~0ull;
    hardware.test = ~0ull;
    hardware.zbuf = ~0ull;
    const ptr32<anon_9d38028c> blocks = G(g_RdrStateDmaBlocks);
    for (uint32_t i = 0; i < kStateBlocks; ++i)
    {
        anon_9d38028c &packet = blocks[i];
        packet.dmaHeader = 0x60000008;           // ret, 8 quadwords
        packet.zeroPadding1 = 0;
        packet.gifHeaderL = 0x1000000000008006ull; // 6 register writes, A+D
        packet.gifHeaderH = 0xe;
        packet.vifStcol = 0x1000404;
        packet.vifUnpack = 0x6c078000;
        packet.vifMscal = 0x1400040a;
        packet.vifFlusha = 0x13000000;
    }
    RdrState_SetStateBlockToDefaults(ptr32<sRdrStateBlock>(guest_addr(&G(g_RdrDefaultRenderState))));
    G(gRdrStateScissorOverride) = 0xed0003027d0003ull;
}

// RENDER.c

// Picks this frame's half of the primitive buffer.
void RenderFrameInit(int32 flags)
{
    renderClass &render = G(renderVarG);
    render.gpuPrimitiveBufferPtr = G(displayVarG).viewBuffer != 0 ? render.gpuPrimitiveOddPtr : render.gpuPrimitiveEvenPtr;
    render.gpuPrimitiveEndPtr = render.gpuPrimitiveBufferPtr + static_cast<int32_t>(render.primitiveBufferSize - 0xc4);
    render.gpuStartPrimitiveBufferPtr = render.gpuPrimitiveBufferPtr;
}

// A leftover from the PlayStation version: nothing is allocated, and the "even" buffer pointer holds the size.
void RenderAllocateBuffers(uint32 size)
{
    renderClass &render = G(renderVarG);
    render.gpuPrimitiveOddPtr = nullptr;
    render.primitiveBufferSize = size;
    render.gpuPrimitiveEvenPtr = ptr32<uint8>(size);
}

// Sets aside the renderer's buffers and sets up the renderer.
void RenderInit(uint32 size)
{
    RenderAllocateBuffers(size);
    Rdr_Init();
}

void RenderFreeBuffers() {}
void RenderTerm() {}

// TEXTURE.c

void TexFreeIntermediateResData() {}

// RDRSTATE.c, for a packet being built: the six register writes as GIF A+D data inside an UNPACK.
void RdrState_PkForceApplyStateBlock(ptr32<sRdrStateBlock> block, ptr32<sceVif1Packet> packet)
{
    // Each value is read just before it is written, as in the original: the packet may overlap the block.
    sceVif1PkAddUpkData64(packet, 0);
    sceVif1PkAddUpkData64(packet, 0xfffffffffffffff9ull);
    sceVif1PkAddUpkData64(packet, 0x1000000000008006ull);
    sceVif1PkAddUpkData64(packet, 0xe);
    sceVif1PkAddUpkData64(packet, G(gRdrStateScissorOverride));
    sceVif1PkAddUpkData64(packet, kGsScissor1);
    sceVif1PkAddUpkData64(packet, block->alpha);
    sceVif1PkAddUpkData64(packet, kGsAlpha1);
    sceVif1PkAddUpkData64(packet, block->clamp);
    sceVif1PkAddUpkData64(packet, kGsClamp1);
    sceVif1PkAddUpkData64(packet, block->test);
    sceVif1PkAddUpkData64(packet, kGsTest1);
    sceVif1PkAddUpkData64(packet, block->zbuf);
    sceVif1PkAddUpkData64(packet, kGsZbuf1);
    sceVif1PkAddUpkData64(packet, block->pabe);
    sceVif1PkAddUpkData64(packet, kGsPabe);
}

// RDRPOLY.c: textured polygons for the HUD, menus and effects, batched per channel into a VIF packet that the
// shape microprogram draws. Channels 0 and 1 are the two players' views (0 means the current player), 2 and 3 the
// screen overlay for each.

uint64_t gsTex0(const _sGfxTexture *tex);

namespace {

const uint32_t kPolyOne = (sym::fConst_450 + 0x4u), kTriOne = (sym::fConst_454 + 0x4u); // Each holds the vertex w (1.0) the packets carry.

int32_t polyChannel(int32_t channel)
{
    const int32_t player = G(gRdrPolyPlayer);
    if (channel == 0)
        channel = player == 2 ? 1 : 0;
    if (channel == 2 && player == 2)
        channel = 3;
    return channel;
}

// The channel number is not checked; past the four channels the original reads and writes whatever follows.
sRdrPolyChannel &channelAt(int32_t channel)
{
    return *static_cast<sRdrPolyChannel *>(guest_host(guest_addr(&G(gRdrPolyClass)) + static_cast<uint32_t>(channel) * sizeof(sRdrPolyChannel)));
}

ptr32<sceVif1Packet> packetOf(sRdrPolyChannel &c) { return ptr32<sceVif1Packet>(guest_addr(&c.vifDataPkt)); }
ptr32<sRdrStateBlock> stateOf(sRdrPolyChannel &c) { return ptr32<sRdrStateBlock>(guest_addr(&c.renderState)); }

// log2 of a texture side for TEX0; 0 for anything that isn't a power of two up to 1024.
uint64_t sideLog2(int32_t side)
{
    for (uint64_t bits = 0; bits <= 10; ++bits)
        if (side == (1 << bits))
            return bits;
    return 0;
}

// Writes the count of polygons since the last texture into the packet, where that texture's GIF tag left room.
void closePolyCount(sRdrPolyChannel &c)
{
    if (c.pPolysToRender)
    {
        const u32 count = c.polysToRender;
        const ptr32<u32> at = c.pPolysToRender;
        c.pPolysToRender = nullptr;
        *at = count;
        c.polysToRender = 0;
    }
}

// Starts a polygon: closes the UNPACK if it is getting long, opens the packet and an UNPACK if needed, and emits the
// render state and texture when they changed. Returns false if the channel has no texture.
bool beginPolygon(int32_t channel, int32_t maxQuadwords)
{
    sRdrPolyChannel &c = channelAt(channel);
    if (!c.pCurrTexture)
        return false;
    const ptr32<sceVif1Packet> packet = packetOf(c);
    const int32_t sinceCode = static_cast<int32_t>(c.vifDataPkt.pCurrent.address + 0xf - c.vifDataPkt.pVifCode.address);
    const int32_t quadwords = (sinceCode < 0 ? static_cast<int32_t>(c.vifDataPkt.pCurrent.address + 0x1e - c.vifDataPkt.pVifCode.address) : sinceCode) >> 4;
    if (maxQuadwords < quadwords && c.unpacking != 0)
    {
        closePolyCount(c);
        pkAddUpkData128(c.vifDataPkt, 0, 0);
        sceVif1PkCloseUpkCode(packet);
        c.unpacking = 0;
        sceVif1PkAddCode(packet, 0x14000000); // MSCAL: draw what is there.
    }
    if (c.vifPktInUse == 0)
    {
        sceVif1PkInit(packet, c.pDmaBuffer[c.buffToUse]);
        sceVif1PkReset(packet);
        sceVif1PkRet(packet, 0);
        c.vifPktInUse = 1;
    }
    if (c.unpacking == 0)
    {
        sceVif1PkOpenUpkCode(packet, 0x8000, 0x6c, 4, 4);
        c.pPolysToRender = nullptr;
        c.reqTexture = 1;
        c.unpacking = 1;
        c.polysToRender = 0;
    }
    if (c.reqTexture != 0)
    {
        RdrState_PkForceApplyStateBlock(stateOf(c), packet);
        const uint64_t texture = gsTex0(c.pCurrTexture.get());
        closePolyCount(c);
        sceVif1PkAddUpkData64(packet, texture);
        c.pPolysToRender = c.vifDataPkt.pCurrent;
        sceVif1PkAddUpkData64(packet, 0);
        sceVif1PkAddUpkData64(packet, 0x60);
        sceVif1PkAddUpkData64(packet, 0x14);
        sceVif1PkAddUpkData64(packet, 0);
        sceVif1PkAddUpkData64(packet, 0x34);
        sceVif1PkAddUpkData64(packet, 0);
        sceVif1PkAddUpkData64(packet, 0x36);
        c.reqTexture = 0;
    }
    return true;
}

// One triangle of a polygon: a header, the three vertices, their texture coordinates and their colours.
void emitTriangle(ptr32<sceVif1Packet> packet, ptr32<_sRdrPoly> poly, const int (&v)[3], uint32_t one)
{
    const uint32_t base = poly.address;
    const auto words = [&](uint32_t address, u_int n) { sceVif1PkAddUpkData32N(packet, ptr32<u_int>(address), n); };
    const uint32_t zOffset = base + offsetof(_sRdrPoly, zOffset);
    sceVif1PkAddUpkData32(packet, 0);
    words(one, 1);
    sceVif1PkAddUpkData32(packet, 0);
    sceVif1PkAddUpkData32(packet, 1);
    for (int i : v)
    {
        words(base + offsetof(_sRdrPoly, vertex) + i * 16u, 3);
        words(one, 1);
    }
    for (int i : v)
    {
        words(base + offsetof(_sRdrPoly, u) + i * 4u, 1);
        words(base + offsetof(_sRdrPoly, v) + i * 4u, 1);
        words(one, 1);
        words(zOffset, 1);
    }
    const uint8_t *rgba = reinterpret_cast<const uint8_t *>(poly->rgba);
    for (int i : v)
        for (int k = 0; k < 4; ++k)
            sceVif1PkAddUpkData32(packet, rgba[i * 4 + k]);
}

} // namespace

// The GS TEX0 register for the channel's texture: buffer, width in 64-texel units, format, size, and for the
// palette formats (0x13, 0x14) the palette's address, loaded with the texture.
uint64_t gsTex0Of(const _sGfxTexture &texture);

uint64_t gsTex0(const _sGfxTexture *tex)
{
    return tex ? gsTex0Of(*tex) : 0;
}

uint64_t gsTex0Of(const _sGfxTexture &texture)
{
    const _sGfxTexture *tex = &texture;
    const int32_t wd = tex->wd;
    const int32_t blocks = (wd < 0 ? wd + 0x3f : wd) >> 6;
    const uint64_t bufferWidth = 0 < blocks ? static_cast<uint64_t>(blocks) : 1;
    uint64_t value = static_cast<uint64_t>(static_cast<int64_t>(tex->idxList[0])) | 0x400000000ull | (sideLog2(wd) << 26) |
                     (bufferWidth << 14) | (static_cast<uint64_t>(static_cast<int64_t>(tex->fmt)) << 20) | (sideLog2(tex->ht) << 30);
    if (static_cast<uint32_t>(tex->fmt) - 0x13u < 2u)
        value |= 0x2000000000000000ull | (static_cast<uint64_t>(static_cast<int64_t>(tex->clutVramAddr)) << 37);
    return value;
}

void RdrPolyCamera(int32_t channel, ptr32<sceVu0FMATRIX> pMatrix, ptr32<sceVu0FVECTOR> pPos, ee_float dValX, ee_float dValY)
{
    sRdrPolyChannel &c = channelAt(polyChannel(channel));
    std::memcpy(c.cameraMatrix, pMatrix.get(), sizeof(c.cameraMatrix));
    std::memcpy(c.cameraPos, pPos.get(), sizeof(c.cameraPos));
    c.cameraDValY = dValY;
    c.cameraDValX = dValX;
}

int32_t sceVif1UnpackSize(ptr32<sceVif1Packet> pPacket)
{
    const int32_t a = static_cast<int32_t>(pPacket->pCurrent.address + (0xf - pPacket->pVifCode.address));
    const int32_t b = static_cast<int32_t>(pPacket->pCurrent.address + (0x1e - pPacket->pVifCode.address));
    return (a < 0 ? b : a) >> 4;
}

// Sets the channel's texture and the state for a texture style: 0x10 repeat instead of clamp, 0x20 alpha test kept
// on failure for colour only, 2 additive, 4 subtractive, otherwise normal blending.
void RdrPolyTexture(int32_t channel, ptr32<_sGfxTexture> pTexture, u32 style)
{
    sRdrPolyChannel &c = channelAt(polyChannel(channel));
    const ptr32<sRdrStateBlock> state = stateOf(c);
    if (pTexture == c.pCurrTexture && style == c.currTexAttrib)
        return;
    RdrState_SetStateBlockToDefaults(state);
    c.pCurrTexture = pTexture;
    c.reqTexture = 1;
    c.currTexAttrib = style;
    RdrState_SetTextureClamp(state, (style & 0x10) == 0 ? 5 : 0);
    RdrState_SetAlphaTestEnable(state, 1, 3, (style & 0x20) == 0 ? 0 : 3, '?');
    if ((style & 2) == 0 && (style & 4) == 0 && (style & 0x20) == 0)
    {
        RdrState_SetAlphaBlending(state, 1, 0, 2, 0, 0x80);
        RdrState_SetPixelAlphaBlendControlEnable(state, 1);
    }
    else
    {
        u32 B = 0, C = 0, D = 0;
        if ((style & 2) != 0)
            B = 2, C = 2;
        else if ((style & 4) != 0)
            C = 2, D = 2;
        RdrState_SetAlphaBlending(state, 1, B, C, D, 0x80);
        RdrState_SetPixelAlphaBlendControlEnable(state, 0);
    }
    RdrState_SetZBufferWriteEnable(state, c.turnOffZeds != 0 ? 0 : 1);
}

void RdrPolyTerm()
{
    for (sRdrPolyChannel &c : G(gRdrPolyClass).channels)
    {
        MemBlockFree(ptr32<memHandle>(guest_addr(&c.pDmaBuffer[0])));
        MemBlockFree(ptr32<memHandle>(guest_addr(&c.pDmaBuffer[1])));
    }
}

void RdrZedsWriteOff(int32_t channel)
{
    sRdrPolyChannel &c = channelAt(polyChannel(channel));
    RdrState_SetZBufferWriteEnable(stateOf(c), 0);
    c.reqTexture = 1;
    c.turnOffZeds = 1;
}

// Sets the state's depth writes back on but leaves turnOffZeds set, as the original does.
void RdrZedsWriteOn(int32_t channel)
{
    sRdrPolyChannel &c = channelAt(polyChannel(channel));
    RdrState_SetZBufferWriteEnable(stateOf(c), 1);
    c.reqTexture = 1;
    c.turnOffZeds = 1;
}

void RdrPolyInit()
{
    sRdrPolyClass &polys = G(gRdrPolyClass);
    polys.dmaVIF1 = ptr32<anon_0db2ace9>(guest_call<uint32_t>(sym::sceDmaGetChan, 1)); // sceDmaGetChan(1)
    for (int32_t i = 0; i < 4; ++i)
    {
        sRdrPolyChannel &c = polys.channels[i];
        const uint32 size = static_cast<uint32>(G(channelSizes)[i]);
        MemBlockAllocLo(ptr32<memHandle>(guest_addr(&c.pDmaBuffer[0])), size);
        MemBlockAllocLo(ptr32<memHandle>(guest_addr(&c.pDmaBuffer[1])), size);
        c.buffToUse = 0;
        c.pCurrTexture = nullptr;
        c.currTexAttrib = 0xffffffffu;
        c.vifPktInUse = 0;
        c.unpacking = 0;
        c.reqTexture = 0;
        c.pPolysToRender = nullptr;
        c.polysToRender = 0;
        RdrState_SetStateBlockToDefaults(stateOf(c));
        c.turnOffZeds = 0;
    }
}

// Adds the packet that sets the shape microprogram's camera for a channel, from the shape packet ring.
void RdrPolyInitCamera(int32_t channel)
{
    RC_DIVERT_IF_CHANGED(RdrPolyInitCamera, channel);
    sRdr &rdr = G(gRdr);
    const int32_t index = rdr.currShapeData;
    rdr.currShapeData = rdr.currShapeData + 1;
    if (0x167 < rdr.currShapeData)
        rdr.currShapeData = 0;
    const ptr32<u8> packet = rdr.pShapeData + index * rdr.shapeDataSz;
    uint8_t *p = packet.get();
    const auto put = [p](uint32_t offset, uint32_t value) { std::memcpy(p + offset, &value, 4); };
    const auto putf = [p](uint32_t offset, ee_float value) { std::memcpy(p + offset, &value, 4); };
    const sRdrPolyChannel &c = channelAt(channel);
    put(0xcc, *static_cast<uint32_t *>(guest_host(sym::lit4_327bf4)));
    std::memset(p + 0xc0, 0, 12);
    putf(0xf0, c.cameraMatrix[0][2]);
    putf(0xf4, c.cameraMatrix[1][2]);
    put(0xfc, 0);
    putf(0xf8, c.cameraMatrix[2][2]);
    std::memcpy(p + 0x100, c.cameraPos, 16);
    put(0x10c, 0);
    putf(0x120, c.cameraDValX);
    putf(0x124, -c.cameraDValY);
    alignas(16) float position[4], negated[4], matrix[16], eye[4];
    std::memcpy(position, c.cameraPos, 16);
    _mm_storeu_ps(negated, PS2_VMUL(_mm_loadu_ps(position), _mm_set1_ps(-1.0f)));
    std::memcpy(matrix, c.cameraMatrix, 64);
    vu0ApplyMatrix(eye, matrix, negated);
    for (int row = 0; row < 3; ++row)
    {
        std::memcpy(p + 0x130 + row * 0x10, &c.cameraMatrix[row][0], 8);
        putf(0x138 + row * 0x10, c.cameraMatrix[row][2]);
    }
    std::memcpy(p + 0x160, eye, 8);
    putf(0x168, eye[2]);
    put(0x16c, 0x3f800000); // 1.0
    const int32_t player = G(gRdrPolyPlayer);
    put(0x114, player == 1 ? 0x44f90000u : player == 2 ? *static_cast<uint32_t *>(guest_host(sym::lit4_327bf8)) : 0x45000000u);
    Rdr_UploadProg(ptr32<u32>(kShapeProgram));
    Rdr_AddDmaCall(packet.cast<void>());
}

// A quad, as two triangles (0 1 2 and 1 2 3).
void RdrPoly(int32_t channel, ptr32<_sRdrPoly> poly)
{
    channel = polyChannel(channel);
    if (!beginPolygon(channel, 0x96))
        return;
    sRdrPolyChannel &c = channelAt(channel);
    const ptr32<sceVif1Packet> packet = packetOf(c);
    emitTriangle(packet, poly, {0, 1, 2}, kPolyOne);
    emitTriangle(packet, poly, {1, 2, 3}, kPolyOne);
    c.polysToRender = c.polysToRender + 2;
}

void RdrTri(int32_t channel, ptr32<_sRdrPoly> poly)
{
    channel = polyChannel(channel);
    if (!beginPolygon(channel, 200))
        return;
    sRdrPolyChannel &c = channelAt(channel);
    emitTriangle(packetOf(c), poly, {0, 1, 2}, kTriOne);
    c.polysToRender = c.polysToRender + 1;
}

// Ends the channel's packet, adds the camera packet and the polygon packet to the frame, and swaps buffers.
void RdrPolyFlush(int32_t channel)
{
    channel = polyChannel(channel);
    sRdrPolyChannel &c = channelAt(channel);
    if (c.vifPktInUse == 0)
        return;
    const ptr32<sceVif1Packet> packet = packetOf(c);
    if (c.unpacking != 0)
    {
        closePolyCount(c);
        pkAddUpkData128(c.vifDataPkt, 0, 0);
        sceVif1PkCloseUpkCode(packet);
        c.unpacking = 0;
    }
    sceVif1PkAddCode(packet, 0x14000000); // MSCAL
    sceVif1PkAddCode(packet, 0x13000000); // FLUSHA
    sceVif1PkTerminate(packet);
    RdrPolyInitCamera(channel);
    Rdr_AddDmaCall(c.pDmaBuffer[c.buffToUse].cast<void>());
    c.vifPktInUse = 0;
    c.buffToUse = (c.buffToUse + 1) & 1;
    RdrState_SetStateBlockToDefaults(stateOf(c));
    c.turnOffZeds = 0;
    c.pCurrTexture = nullptr;
    c.currTexAttrib = 0xffffffffu;
}

} // namespace rc

RC_DECOMP_FN(Rdr_WhichProg)
RC_DECOMP_FN(Rdr_AddDmaCall)
RC_DECOMP_FN(Rdr_UploadProg)
RC_DECOMP_FN(Rdr_UseCamera)
RC_DECOMP_FN(Rdr_DrawPoint)
RC_DECOMP_FN(Rdr_DrawTri)
RC_DECOMP_FN(Rdr_DrawQuad)
RC_DECOMP_FN_NOFUZZ(Rdr_Term)
RC_DECOMP_FN_SETUP_HEAP(Rdr_Init, rc::fuzz::rdrInit)
RC_DECOMP_FN_BLOCKING_SETUP(Rdr_EndFrame, rc::fuzz::rdrEndFrame)
RC_DECOMP_FN_SETUP(Rdr_SetupCamera, rc::fuzz::rdrCamera)
RC_DECOMP_FN_SETUP(Rdr_DrawLine, rc::fuzz::rdrLine)
RC_DECOMP_FN_BLOCKING_SETUP(Rdr_Vif1Send, rc::fuzz::hardwareArg)
RC_DECOMP_FN(GfxP2_BeginScene)
RC_DECOMP_FN(GfxP2_EndScene)
RC_DECOMP_FN(GfxP2_RenderDmaSend)
RC_DECOMP_FN(GfxP2_Tex)
RC_DECOMP_FN(GfxP2_TexExt)
RC_DECOMP_FN(GfxP2_TexFX)
RC_DECOMP_FN(GfxP2_DefRenderState)
RC_DECOMP_FN(GfxP2_Scissor)
RC_DECOMP_FN(GfxP2_ZWriteEnable)
RC_DECOMP_FN(GfxP2_ZWriteDisable)
RC_DECOMP_FN(Rdr_StartFrame)
RC_DECOMP_FN(RdrState_SetAlphaBlending)
RC_DECOMP_FN(RdrState_SetAlphaTestEnable)
RC_DECOMP_FN(RdrState_SetZTestEnable)
RC_DECOMP_FN(RdrState_SetTextureClamp)
RC_DECOMP_FN(RdrState_SetScissor)
RC_DECOMP_FN(RdrState_SetZBufferWriteEnable)
RC_DECOMP_FN(RdrState_SetPixelAlphaBlendControlEnable)
RC_DECOMP_FN(RdrState_SetStateBlockToDefaults)
RC_DECOMP_FN(RdrState_ForceApplyStateBlock)
RC_DECOMP_FN(RdrState_ApplyStateBlock)
RC_DECOMP_FN(RdrState_ApplyDefaultStateBlock)
RC_DECOMP_FN_NOFUZZ(RdrState_Init)
RC_DECOMP_FN(RenderFrameInit)
RC_DECOMP_FN_BLOCKING(RenderInit) // Once, at start-up: only a whole-game check.
RC_DECOMP_FN(RenderAllocateBuffers)
RC_DECOMP_FN(RenderFreeBuffers)
RC_DECOMP_FN(RenderTerm)
RC_DECOMP_FN(TexFreeIntermediateResData)
RC_DECOMP_FN(RdrState_PkForceApplyStateBlock)
RC_DECOMP_FN(RdrPolyCamera)
RC_DECOMP_FN(sceVif1UnpackSize)
RC_DECOMP_FN(RdrPolyTexture)
RC_DECOMP_FN_NOFUZZ(RdrPolyTerm)
RC_DECOMP_FN(RdrZedsWriteOff)
RC_DECOMP_FN(RdrZedsWriteOn)
RC_DECOMP_FN_NOFUZZ(RdrPolyInit)
RC_DECOMP_FN_NOFUZZ(RdrPolyInitCamera)
RC_DECOMP_FN_NOFUZZ(RdrPoly)
RC_DECOMP_FN_NOFUZZ(RdrTri)
RC_DECOMP_FN_NOFUZZ(RdrPolyFlush)

namespace {

// What the polygon builder writes: the four channels and their packet buffers.
void polygonRegions(std::vector<std::pair<uint32_t, uint32_t>> &ranges)
{
    using namespace rc;
    const uint32_t base = guest_addr(&G(gRdrPolyClass));
    ranges.emplace_back(base, base + static_cast<uint32_t>(sizeof(sRdrPolyClass)));
    for (int c = 0; c < 4; ++c)
        for (int b = 0; b < 2; ++b)
        {
            const uint32_t at = G(gRdrPolyClass).channels[c].pDmaBuffer[b].address & PS2_RAM_MASK;
            const uint32_t size = static_cast<uint32_t>(G(channelSizes)[c]);
            if (at != 0 && at + size <= PS2_RAM_SIZE)
                ranges.emplace_back(at, at + size);
        }
}

const bool polygonRegionsRegistered = []
{
    for (const char *name : {"RdrPoly", "RdrTri", "RdrPolyTexture", "RdrZedsWriteOff", "RdrZedsWriteOn", "RdrPolyCamera"})
        rc::registerRegions(name, polygonRegions);
    return true;
}();

} // namespace
