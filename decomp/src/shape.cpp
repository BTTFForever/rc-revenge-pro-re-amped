// Decompiled from RDR_SHAPE.c: the CPU side of drawing a shape with the VU1 programs (the track, cars, scenery,
// Track Editor pieces). For each shape it fills a packet with the camera seen from the shape's own space, which the
// microprogram uses to transform, light and clip the shape's prebuilt triangle list, then adds the packet and the
// list to the frame's DMA chain. Packets come from a ring of 360 in gRdr.pShapeData (384 for the Track Editor).

#include "ee_math.h"

namespace rc {

bool_ MemBlockAllocLo(ptr32<memHandle> pHandle, uint32 size);
void MemBlockFree(ptr32<memHandle> pHandle);
void Rdr_AddDmaCall(ptr32<void> pPacket);
void Rdr_UploadProg(ptr32<u32> pProg);
void RdrState_SetStateBlockToDefaults(ptr32<sRdrStateBlock> block);
void RdrState_SetZTestEnable(ptr32<sRdrStateBlock> block, u32 enable, u32 func);
void RdrState_SetZBufferWriteEnable(ptr32<sRdrStateBlock> block, u32 enable);
void RdrState_SetPixelAlphaBlendControlEnable(ptr32<sRdrStateBlock> block, u32 enable);
void RdrState_SetAlphaBlending(ptr32<sRdrStateBlock> block, u32 A, u32 B, u32 C, u32 D, u8 fixedAlpha);
void RdrState_ApplyStateBlock(ptr32<sRdrStateBlock> block);
void RdrState_ApplyDefaultStateBlock();
uint64_t gsTex0(const _sGfxTexture *tex); // rdr.cpp

namespace {

const uint32_t kShapeTemplate = sym::gTRdr_ShapeData__1;  // The packet every shape ring entry starts as.
const uint32_t kTrackEdTemplate = sym::gTERdr_ShapeData; // The same for the Track Editor ring (Ghidra shows both as _dma_1).
const uint32_t kShapeProgram = sym::TRdr_VUShapeProg;   // _dma_48: track and shape renderer.
const uint32_t kEnvProgram = sym::EnvRdr_VUShapeProg;     // gTRdr_EndShapeData: the shiny (environment map) pass.
const uint32_t kVUShapeProgram = sym::Rdr_VUShapeProg; // _dma_47: Rdr_VUShape's renderer.
const uint32_t kTrackEdProgram = sym::TERdr_VUShapeProg; // The Track Editor's renderer (Ghidra shows it as _dma_48 too).
const uint32_t kVUShapePacket = sym::gRdr_ShapeData;  // Rdr_VUShape's single packet; the gRdr_* globals are inside it.
const uint32_t kRdrAlphaAddTriangles = sym::RdrAlphaAddTriangles;
const int32_t kShapePackets = 0x168, kTrackEdPackets = 0x180;

ee_float dot(ee_float ax, ee_float ay, ee_float az, ee_float bx, ee_float by, ee_float bz)
{
    return ax * bx + ay * by + az * bz;
}

struct Row
{
    ee_float x, y, z;
};
Row rowX(const sMat33 &m) { return {m.Xx, m.Xy, m.Xz}; }
Row rowY(const sMat33 &m) { return {m.Yx, m.Yy, m.Yz}; }
Row rowZ(const sMat33 &m) { return {m.Zx, m.Zy, m.Zz}; }

void putf(uint8_t *p, uint32_t offset, ee_float value) { std::memcpy(p + offset, &value, 4); }
void putw(uint8_t *p, uint32_t offset, uint32_t value) { std::memcpy(p + offset, &value, 4); }

// The part of the packet every shape renderer shares, at offsets relative to the program's layout (base is 0xf0 for
// the track renderer, 0xb0 for the Track Editor's): the camera's view direction and position in the shape's space,
// the projection scale, the shape-to-camera rotation and the shape's position seen from the camera.
void writeShapeCamera(uint8_t *p, uint32_t base, const sRdrVUShape &shape, const _sRdrCamera &camera)
{
    const sMat33 &s = shape.mat, &c = camera.mat;
    const Row shapeRows[3] = {rowX(s), rowY(s), rowZ(s)};
    const Row cameraRows[3] = {rowX(c), rowY(c), rowZ(c)};
    for (int i = 0; i < 3; ++i)
        putf(p, base + i * 4u, dot(c.Zx, c.Zy, c.Zz, shapeRows[i].x, shapeRows[i].y, shapeRows[i].z));
    putw(p, base + 0xc, 0);
    const ee_float dx = camera.pos.x - shape.pos.x, dy = camera.pos.y - shape.pos.y, dz = camera.pos.z - shape.pos.z;
    for (int i = 0; i < 3; ++i)
        putf(p, base + 0x10 + i * 4u, dot(dx, dy, dz, shapeRows[i].x, shapeRows[i].y, shapeRows[i].z));
    putw(p, base + 0x1c, 0);
    putf(p, base + 0x30, camera.dValX);
    putf(p, base + 0x34, -camera.dValY);
    for (int r = 0; r < 3; ++r)
        for (int col = 0; col < 3; ++col)
            putf(p, base + 0x40 + r * 0x10u + col * 4u,
                 dot(shapeRows[r].x, shapeRows[r].y, shapeRows[r].z, cameraRows[col].x, cameraRows[col].y, cameraRows[col].z));
    const ee_float ex = shape.pos.x - camera.pos.x, ey = shape.pos.y - camera.pos.y, ez = shape.pos.z - camera.pos.z;
    for (int col = 0; col < 3; ++col)
        putf(p, base + 0x70 + col * 4u, dot(ex, ey, ez, cameraRows[col].x, cameraRows[col].y, cameraRows[col].z));
    putw(p, base + 0x7c, 0x3f800000); // 1.0
    putf(p, base + 0x24, camera.vertDrawOffset + ee_float(2048.0f));
}

// The next packet of a ring.
uint8_t *nextPacket(s32 &current, int32_t count, ptr32<u8> data, s32 size, uint32_t *address = nullptr)
{
    const int32_t index = current;
    current = current + 1;
    if (count - 1 < current)
        current = 0;
    const ptr32<u8> packet = data + index * size;
    if (address)
        *address = packet.address;
    return packet.get();
}

// The track renderer's tint: the top byte is how strongly the RGB colour replaces the lighting.
void writeTint(uint8_t *p, u32 tint, ee_float scale)
{
    const ee_float keep = ee_float(1.0f) - ee_itof(static_cast<int32_t>(tint >> 24)) * scale;
    const ee_float amount = ee_float(1.0f) - keep;
    putf(p, 0xcc, keep);
    putf(p, 0xc8, ee_itof(static_cast<int32_t>((tint >> 8) & 0xff)) * amount);
    putf(p, 0xc0, ee_itof(static_cast<int32_t>(tint & 0xff)) * amount);
    putf(p, 0xc4, ee_itof(static_cast<int32_t>((tint >> 16) & 0xff)) * amount);
}

// The camera's rotation for the environment map pass.
void writeCameraRows(uint8_t *p, const _sRdrCamera &camera)
{
    const sMat33 &c = camera.mat;
    const Row rows[3] = {rowX(c), rowY(c), rowZ(c)};
    for (int r = 0; r < 3; ++r)
    {
        putf(p, 0x80 + r * 0x10u, rows[r].x);
        putf(p, 0x84 + r * 0x10u, rows[r].y);
        putw(p, 0x8c + r * 0x10u, 0);
        putf(p, 0x88 + r * 0x10u, rows[r].z);
    }
}

// The blend for the shiny pass, rebuilt every time: the original's "already set up" flag is never set.
ptr32<sRdrStateBlock> envState(uint32_t blockAddress)
{
    const ptr32<sRdrStateBlock> block(blockAddress);
    RdrState_SetStateBlockToDefaults(block);
    RdrState_SetZTestEnable(block, 1, 2);
    RdrState_SetPixelAlphaBlendControlEnable(block, 0);
    RdrState_SetAlphaBlending(block, 1, 2, 2, 0, 0x80);
    return block;
}

void addAlphaTriangles(ptr32<sRdrVUShape> pShape)
{
    if (pShape->pAlpha && pShape->pAlpha->numPolys != 0)
        guest_call(kRdrAlphaAddTriangles, pShape);
}

// Rdr_VUShape's packet: the same camera data at fixed addresses, with the camera's rotation transposed.
void writeVUShapePacket(const sRdrVUShape &shape, const _sRdrCamera &camera)
{
    uint8_t *p = static_cast<uint8_t *>(guest_host(kVUShapePacket));
    const sMat33 &s = shape.mat, &c = camera.mat;
    const Row shapeRows[3] = {rowX(s), rowY(s), rowZ(s)};
    for (int i = 0; i < 3; ++i)
        putf(p, 0x130 + i * 4u, dot(c.Zx, c.Zy, c.Zz, shapeRows[i].x, shapeRows[i].y, shapeRows[i].z));
    putw(p, 0x13c, 0);
    const ee_float dx = camera.pos.x - shape.pos.x, dy = camera.pos.y - shape.pos.y, dz = camera.pos.z - shape.pos.z;
    for (int i = 0; i < 3; ++i)
        putf(p, 0x140 + i * 4u, dot(dx, dy, dz, shapeRows[i].x, shapeRows[i].y, shapeRows[i].z));
    putw(p, 0x14c, 0);
    putf(p, 0x160, camera.dValX);
    putf(p, 0x164, -camera.dValY);
    for (int r = 0; r < 3; ++r)
    {
        putf(p, 0x170 + r * 0x10u, shapeRows[r].x);
        putf(p, 0x174 + r * 0x10u, shapeRows[r].y);
        putf(p, 0x178 + r * 0x10u, shapeRows[r].z);
    }
    putf(p, 0x1a8, shape.pos.z - camera.pos.z);
    putf(p, 0x1a0, shape.pos.x - camera.pos.x);
    putf(p, 0x1a4, shape.pos.y - camera.pos.y);
    putw(p, 0x1ac, 0x3f800000);
    const Row columns[3] = {{c.Xx, c.Yx, c.Zx}, {c.Xy, c.Yy, c.Zy}, {c.Xz, c.Yz, c.Zz}};
    for (int r = 0; r < 3; ++r)
    {
        putf(p, 0x1b0 + r * 0x10u, columns[r].x);
        putf(p, 0x1b4 + r * 0x10u, columns[r].y);
        putf(p, 0x1b8 + r * 0x10u, columns[r].z);
    }
    std::memset(p + 0x1e0, 0, 12);
    putw(p, 0x1ec, 0x3f800000);
    putf(p, 0x154, camera.vertDrawOffset + ee_float(2048.0f));
}

} // namespace

// The shape file holds the vertices and triangles right after its header.
void Rdr_ShapeConstruct(ptr32<_sRdrShape> pShape)
{
    pShape->pVert = ptr32<_sRdrVertex>(pShape.address + 0x4c);
    pShape->pTri = ptr32<_sRdrTri>(pShape.address + 0x4c + static_cast<uint32_t>(pShape->numVert) * 0x18u);
}

// Turns texture indices into pointers (-1: none).
void Rdr_FixupShapeTex(ptr32<_sRdrShape> pShape, ptr32<ptr32<_sGfxTexture>> ppTex)
{
    const int32_t count = pShape->numTri;
    for (int32_t i = 0; i < count; ++i)
    {
        _sRdrTri &tri = pShape->pTri[i];
        const int32_t index = static_cast<int32_t>(tri.apTexture[0].address);
        tri.apTexture[0] = index == -1 ? ptr32<_sGfxTexture>(nullptr)
                                       : *ptr32<ptr32<_sGfxTexture>>(ppTex.address + static_cast<uint32_t>(index) * 4u);
    }
}

namespace {

// A prebuilt DMA list ends with an "end" tag. It becomes a "ret", so the frame's chain can call it, and the list's
// last VIF code a FLUSHA.
void endListWithReturn(uint32_t tag)
{
    while (static_cast<uint8_t *>(guest_host(tag))[3] != 0x70)
        tag += (static_cast<uint32_t>(*static_cast<uint16_t *>(guest_host(tag))) + 1u) * 16u;
    const uint32_t qwc = *static_cast<uint16_t *>(guest_host(tag));
    static_cast<uint8_t *>(guest_host(tag))[3] = 0x60;
    static_cast<uint8_t *>(guest_host(tag + qwc * 16u))[0xf] = 0x13;
}

} // namespace

void Rdr_FixVUGridDma(ptr32<sDMAtag> pDmaTag)
{
    endListWithReturn(pDmaTag.address);
}

void Rdr_FixVUShapeDma(ptr32<sRdrVUShape> pShape)
{
    if (pShape->pTri)
        endListWithReturn(pShape->pTri.address);
}

// Fills in each texture's GS registers in the shape's triangle list (TEX0, and on first use TEX1, then the two
// MIPTBP registers), then makes the list callable.
void Rdr_FixVUShape(ptr32<sRdrVUShape> pShape)
{
    if (!pShape)
        return;
    uint32_t list = pShape->pTexFixList.address;
    const ptr32<ptr32<_sGfxTexture>> textures = pShape->pTex;
    for (uint32_t remaining = pShape->numFixups; remaining != 0; --remaining, list += 4)
    {
        const uint32_t at = *static_cast<uint32_t *>(guest_host(list));
        const auto w = [](uint32_t address) -> uint32_t & { return *static_cast<uint32_t *>(guest_host(address)); };
        const ptr32<_sGfxTexture> texPtr = *ptr32<ptr32<_sGfxTexture>>(textures.address + w(at) * 4u);
        const _sGfxTexture &tex = *texPtr;
        const uint64_t tex0 = gsTex0(texPtr ? &tex : nullptr);
        w(at) = static_cast<uint32_t>(tex0);
        w(at + 4) = static_cast<uint32_t>(tex0 >> 32);
        if (w(at + 0x10) == 0)
        {
            w(at + 0x10) = (tex.flags & 0x1c) | 0x160;
            w(at + 0x14) = 0xf60;
            w(at + 0x18) = 0x14;
            w(at + 0x1c) = 0;
        }
        uint32_t mip = at + 0x20;
        if (w(mip) != 0)
            mip += 4;
        w(mip) = tex.miptbp1L;
        w(mip + 4) = tex.miptbp1H;
        w(mip + 8) = 0x34;
        w(mip + 12) = 0;
        uint32_t mip2 = mip + 0x10;
        if (w(mip2) != 0)
            mip2 += 4;
        w(mip2) = tex.miptbp2L;
        w(mip2 + 4) = tex.miptbp2H;
        w(mip2 + 8) = 0x36;
        w(mip2 + 12) = 0;
    }
    Rdr_FixVUShapeDma(pShape);
}

void Rdr_VUShapeExecute(ptr32<u32> pData)
{
    Rdr_AddDmaCall(pData.cast<void>());
}

void Rdr_ShapeTerm()
{
    MemBlockFree(ptr32<memHandle>(guest_addr(&G(gRdr).pShapeData)));
    MemBlockFree(ptr32<memHandle>(guest_addr(&G(gRdr).pTrackEdData)));
}

// Allocates the two packet rings and starts every packet as a copy of the template.
void Rdr_ShapeInit()
{
    GuestFrame frame(0x80); // The two handles are locals of the original, at 0 and 4.
    sRdr &rdr = G(gRdr);
    const ptr32<memHandle> handle1 = frame.local<memHandle>(0), handle2 = frame.local<memHandle>(4);
    *handle1 = nullptr;
    *handle2 = nullptr;
    MemBlockAllocLo(handle1, 0x15f900);
    rdr.pShapeData = handle1->cast<u8>();
    for (int32_t i = 0; i < kShapePackets; ++i)
        std::memcpy((rdr.pShapeData + i * 4000).get(), guest_host(kShapeTemplate), 4000);
    rdr.shapeDataSz = 4000;
    rdr.currShapeData = 0;
    MemBlockAllocLo(handle2, 0x178800);
    rdr.pTrackEdData = handle2->cast<u8>();
    for (int32_t i = 0; i < kTrackEdPackets; ++i)
        std::memcpy((rdr.pTrackEdData + i * 0xfb0).get(), guest_host(kTrackEdTemplate), 0xfb0);
    rdr.trackEdDataSz = 0xfb0;
    rdr.currTrackEdData = 0;
}

// The general shape renderer (version 2 shapes: not used by any level, see NOTES).
void Rdr_VUShape(ptr32<sRdrVUShape> pShape, ptr32<_sRdrCamera> pCamera, u32 tint)
{
    if (!pShape || !pShape->pTri || !pCamera)
        return;
    writeVUShapePacket(*pShape, *pCamera);
    Rdr_UploadProg(ptr32<u32>(kVUShapeProgram));
    Rdr_AddDmaCall(ptr32<void>(kVUShapePacket));
    Rdr_AddDmaCall(pShape->pTri.cast<void>());
}

// As Rdr_VUShape, without drawing a shape.
void Rdr_VUShapeSetup(ptr32<sRdrVUShape> pShape, ptr32<_sRdrCamera> pCamera)
{
    if (!pShape || !pCamera)
        return;
    writeVUShapePacket(*pShape, *pCamera);
    Rdr_UploadProg(ptr32<u32>(kVUShapeProgram));
    Rdr_AddDmaCall(ptr32<void>(kVUShapePacket));
}

namespace {

// Fills the track renderer's packet for a shape, and with an environment map a copy for the shiny pass. Returns the
// main packet's address; envAddress gets the copy's.
uint32_t fillTrackPacket(const sRdrVUShape &shape, const _sRdrCamera &camera, u32 tint, ee_float tintScale, bool rampOnShield,
                         uint32_t &envAddress)
{
    sRdr &rdr = G(gRdr);
    uint32_t mainAddress = 0;
    uint8_t *packet = nextPacket(rdr.currShapeData, kShapePackets, rdr.pShapeData, rdr.shapeDataSz, &mainAddress);
    uint8_t *envPacket = nullptr;
    if (G(gEnvMap) != 0)
        envPacket = nextPacket(rdr.currShapeData, kShapePackets, rdr.pShapeData, rdr.shapeDataSz, &envAddress);
    writeTint(packet, tint, tintScale);
    writeShapeCamera(packet, 0xf0, shape, camera);
    writeCameraRows(packet, camera);
    putw(packet, 0xb8, 0);
    if (G(gEnvMap) != 0)
    {
        std::memcpy(envPacket, packet, static_cast<size_t>(rdr.shapeDataSz));
        putw(packet, 0xb8, 1);
        const u64 envMap = G(gEnvMap);
        std::memcpy(packet + 0xb0, &envMap, 8);
        if (rampOnShield)
        {
            ee_float ramp = G(gEnvRamp);
            if (G(gEnvShield) != 0)
                ramp = ramp * ee_float(10.0f);
            putf(packet, 0xbc, ramp);
        }
    }
    return mainAddress;
}

} // namespace

// The track renderer, for cars and scenery. With an environment map it draws twice: the shiny pass first, then the
// shape blended over it (the shield effect draws the shiny pass at a fixed strength; gEnvFirstPassOnly skips the
// second pass).
void TRdr_VUShape(ptr32<sRdrVUShape> pShape, ptr32<_sRdrCamera> pCamera, u32 tint)
{
    GuestFrame frame(0xc0);
    if (!pShape || !pCamera)
        return;
    uint32_t envAddress = 0;
    const uint32_t mainAddress = fillTrackPacket(*pShape, *pCamera, tint, *static_cast<ee_float *>(guest_host(sym::lit4_327bec)), true, envAddress);
    const ptr32<void> mainPacket(mainAddress), envPacket(envAddress), tris = pShape->pTri.cast<void>();
    if (tris)
    {
        if (G(gEnvMap) == 0)
        {
            Rdr_UploadProg(ptr32<u32>(kShapeProgram));
            Rdr_AddDmaCall(mainPacket);
            Rdr_AddDmaCall(tris);
        }
        else
        {
            const ptr32<sRdrStateBlock> state = envState(sym::envRdrState_454);
            const bool shield = G(gEnvShield) != 0;
            if (shield)
            {
                uint8_t *env = static_cast<uint8_t *>(guest_host(envAddress));
                std::memset(env + 0xc0, 0, 12);
                putf(env, 0xcc, ee_float(1.0f) - G(gEnvRamp));
            }
            Rdr_UploadProg(ptr32<u32>(kEnvProgram));
            Rdr_AddDmaCall(envPacket);
            Rdr_AddDmaCall(tris);
            if (shield || G(gEnvFirstPassOnly) == 0)
            {
                RdrState_ApplyStateBlock(state);
                Rdr_AddDmaCall(mainPacket);
                Rdr_UploadProg(ptr32<u32>(kEnvProgram));
                Rdr_AddDmaCall(tris);
                RdrState_ApplyDefaultStateBlock();
            }
        }
    }
    addAlphaTriangles(pShape);
}

// The same for one cell of the track, whose triangle list is passed separately.
void TRdr_VUShapeCell(ptr32<sRdrVUShape> pShape, ptr32<_sRdrCamera> pCamera, u32 tint, ptr32<u32> pDMAtag)
{
    GuestFrame frame(0xb0);
    if (!pShape || !pCamera)
        return;
    uint32_t envAddress = 0;
    const uint32_t mainAddress = fillTrackPacket(*pShape, *pCamera, tint, *static_cast<ee_float *>(guest_host(sym::lit4_327bf0)), false, envAddress);
    if (pShape->pTri)
    {
        if (G(gEnvMap) == 0)
        {
            Rdr_UploadProg(ptr32<u32>(kShapeProgram));
            Rdr_AddDmaCall(ptr32<void>(mainAddress));
            Rdr_AddDmaCall(pDMAtag.cast<void>());
        }
        else
        {
            Rdr_UploadProg(ptr32<u32>(kEnvProgram));
            Rdr_AddDmaCall(ptr32<void>(envAddress));
            const ptr32<sRdrStateBlock> state = envState(sym::envRdrState_459);
            Rdr_AddDmaCall(pShape->pTri.cast<void>());
            RdrState_ApplyStateBlock(state);
            Rdr_AddDmaCall(ptr32<void>(mainAddress));
            Rdr_UploadProg(ptr32<u32>(kEnvProgram));
            Rdr_AddDmaCall(pShape->pTri.cast<void>());
            RdrState_ApplyDefaultStateBlock();
        }
    }
    addAlphaTriangles(pShape);
}

// The Track Editor's renderer: the tint scales the lighting, and four directional lights come with the packet.
void TERdr_VUShape(ptr32<sRdrVUShape> pShape, ptr32<_sRdrCamera> pCamera, u32 tint)
{
    if (!pShape || !pCamera)
        return;
    sRdr &rdr = G(gRdr);
    uint32_t address = 0;
    uint8_t *p = nextPacket(rdr.currTrackEdData, kTrackEdPackets, rdr.pTrackEdData, rdr.trackEdDataSz, &address);
    const ee_float brightness = ee_itof(static_cast<int32_t>(tint >> 24)) * ee_float(0.00390625f) + ee_float(0.5f);
    const auto channel = [&](uint32_t value) { return (ee_itof(static_cast<int32_t>(value & 0xff)) * ee_float(0.001953125f) + ee_float(0.5f)) * brightness; };
    putf(p, 0x88, channel(tint >> 8));
    putf(p, 0x80, channel(tint));
    putf(p, 0x84, channel(tint >> 16));
    writeShapeCamera(p, 0xb0, *pShape, *pCamera);
    const auto &vecs = G(rdr_TELightVecsG);
    const auto &vals = G(rdr_TELightValsG);
    for (int axis = 0; axis < 3; ++axis)
        for (int light = 0; light < 4; ++light)
            putf(p, 0x130 + axis * 0x10u + light * 4u, vecs[light][axis]);
    for (int light = 0; light < 4; ++light)
    {
        for (int k = 0; k < 3; ++k)
            putf(p, 0x160 + light * 0x10u + k * 4u, vals[light][k]);
        putw(p, 0x16c + light * 0x10u, 0);
    }
    Rdr_UploadProg(ptr32<u32>(kTrackEdProgram));
    Rdr_AddDmaCall(ptr32<void>(address));
    Rdr_AddDmaCall(pShape->pTri.cast<void>());
}

// RDR_SKIN.c: animated models (the drivers). The packet carries the camera like a shape's, then each bone's
// matrix and position for the skin microprogram.

namespace {
const uint32_t kSkinTemplate = sym::gRdr_SkinShapeData, kSkinProgram = sym::Rdr_VUSkinProg;
const int32_t kSkinPackets = 0x180;
const uint32_t kWaterTemplate = sym::gRdr_WaterShapeData, kWaterProgram = sym::gRdr_WaterEndShapeData;
const int32_t kWaterPackets = 0x20;

void putRows(uint8_t *p, uint32_t offset, const sMat33 &m)
{
    const Row rows[3] = {rowX(m), rowY(m), rowZ(m)};
    for (int r = 0; r < 3; ++r)
    {
        putf(p, offset + r * 0x10u, rows[r].x);
        putf(p, offset + r * 0x10u + 4, rows[r].y);
        putf(p, offset + r * 0x10u + 8, rows[r].z);
    }
}
} // namespace

void Rdr_FixVUSkinDma(ptr32<sRdrVUSkin> pSkin)
{
    endListWithReturn(pSkin->pTri.address);
}

void Rdr_SkinTerm()
{
    MemBlockFree(ptr32<memHandle>(guest_addr(&G(gRdr).pSkinData)));
}

// Fills in each texture's TEX0 register in the skin's triangle list, then makes the list callable.
void Rdr_FixVUSkin(ptr32<sRdrVUSkin> pSkin)
{
    if (pSkin)
    {
        uint32_t list = pSkin->pTexFixList.address;
        for (uint32_t remaining = pSkin->numFixups; remaining != 0; --remaining, list += 4)
        {
            const uint32_t at = *static_cast<uint32_t *>(guest_host(list));
            uint32_t *words = static_cast<uint32_t *>(guest_host(at));
            const ptr32<_sGfxTexture> texPtr = *ptr32<ptr32<_sGfxTexture>>(pSkin->pTex.address + words[0] * 4u);
            const uint64_t tex0 = gsTex0(texPtr ? texPtr.get() : nullptr);
            words[1] = static_cast<uint32_t>(tex0 >> 32);
            words[0] = static_cast<uint32_t>(tex0);
        }
    }
    Rdr_FixVUSkinDma(pSkin);
}

void Rdr_SkinInit()
{
    GuestFrame frame(0x60); // The handle is a local of the original, at 0.
    const ptr32<memHandle> handle = frame.local<memHandle>(0);
    *handle = nullptr;
    MemBlockAllocLo(handle, 0x2f7000);
    sRdr &rdr = G(gRdr);
    rdr.pSkinData = handle->cast<u8>();
    for (int32_t i = 0; i < kSkinPackets; ++i)
        std::memcpy((rdr.pSkinData + i * 0x1fa0).get(), guest_host(kSkinTemplate), 0x1fa0);
    rdr.skinDataSz = 0x1fa0;
    rdr.currSkinData = 0;
}

// Draws a skinned model in the pose pAnimOut holds; the root bone places the model.
void Rdr_VUSkin(ptr32<sRdrVUSkin> pSkin, ptr32<sAnimOutput> pAnimOut, ptr32<_sRdrCamera> pCamera)
{
    if (!pSkin || !pCamera)
        return;
    sRdr &rdr = G(gRdr);
    uint32_t address = 0;
    uint8_t *p = nextPacket(rdr.currSkinData, kSkinPackets, rdr.pSkinData, rdr.skinDataSz, &address);
    sRdrVUSkin &skin = *pSkin;
    const sAnimOutput &anim = *pAnimOut;
    const _sRdrCamera &camera = *pCamera;
    skin.pos = *anim.aPos[0];
    skin.mat = *anim.aMat[0];
    const sMat33 &s = skin.mat, &c = camera.mat;
    const Row skinRows[3] = {rowX(s), rowY(s), rowZ(s)};
    for (int i = 0; i < 3; ++i)
        putf(p, 0xe0 + i * 4u, dot(c.Zx, c.Zy, c.Zz, skinRows[i].x, skinRows[i].y, skinRows[i].z));
    putw(p, 0xec, 0);
    const ee_float dx = camera.pos.x - skin.pos.x, dy = camera.pos.y - skin.pos.y, dz = camera.pos.z - skin.pos.z;
    for (int i = 0; i < 3; ++i)
        putf(p, 0xf0 + i * 4u, dot(dx, dy, dz, skinRows[i].x, skinRows[i].y, skinRows[i].z));
    putw(p, 0xfc, 0);
    putf(p, 0xd0, camera.pos.x);
    putf(p, 0xd4, camera.pos.y);
    putf(p, 0xd8, camera.pos.z);
    putw(p, 0xdc, 0);
    putf(p, 0x110, camera.dValX);
    putf(p, 0x114, -camera.dValY);
    putRows(p, 0x120, s);
    putf(p, 0x150, skin.pos.x - camera.pos.x);
    putf(p, 0x154, skin.pos.y - camera.pos.y);
    putf(p, 0x158, skin.pos.z - camera.pos.z);
    putw(p, 0x15c, 0x3f800000);
    const Row columns[3] = {{c.Xx, c.Yx, c.Zx}, {c.Xy, c.Yy, c.Zy}, {c.Xz, c.Yz, c.Zz}};
    for (int r = 0; r < 3; ++r)
    {
        putf(p, 0x160 + r * 0x10u, columns[r].x);
        putf(p, 0x164 + r * 0x10u, columns[r].y);
        putf(p, 0x168 + r * 0x10u, columns[r].z);
    }
    std::memset(p + 0x190, 0, 12);
    putw(p, 0x19c, 0x3f800000);
    // The bones, from offset 4000.
    for (int32_t bone = 0; bone < anim.numNodes; ++bone)
    {
        uint8_t *b = p + 4000 + bone * 0x40;
        putRows(b, 0, *anim.aMat[bone]);
        const _sVec3 &pos = *anim.aPos[bone];
        putf(b, 0x30, pos.x);
        putf(b, 0x34, pos.y);
        putf(b, 0x38, pos.z);
        putw(b, 0x3c, 0x3f800000);
    }
    putf(p, 0x104, camera.vertDrawOffset + ee_float(2048.0f));
    Rdr_UploadProg(ptr32<u32>(kSkinProgram));
    Rdr_AddDmaCall(ptr32<void>(address + 0xf90));
    Rdr_AddDmaCall(ptr32<void>(address));
    Rdr_AddDmaCall(pSkin->pTri.cast<void>());
}

// RDR_WATER.c

void Rdr_WaterTerm()
{
    MemBlockFree(ptr32<memHandle>(guest_addr(&G(gRdr).pWaterShapeData)));
}

void Rdr_WaterInit()
{
    sRdr &rdr = G(gRdr);
    MemBlockAllocLo(ptr32<memHandle>(guest_addr(&rdr.pWaterShapeData)), 0x20400);
    for (int32_t i = 0; i < kWaterPackets; ++i)
        std::memcpy((rdr.pWaterShapeData + i * 0x1020).get(), guest_host(kWaterTemplate), 0x1020);
    rdr.waterShapeDataSz = 0x1020;
    rdr.waterCurrShapeData = 0;
    const ptr32<sRdrStateBlock> state(guest_addr(&G(waterStateBlock)));
    RdrState_SetStateBlockToDefaults(state);
    RdrState_SetZBufferWriteEnable(state, 0);
}

void RdrState_ForceApplyStateBlock(ptr32<sRdrStateBlock> block);

// Water: the shape's bump texture (a pointer kept after the shape header) and a light above and ahead of the camera
// go into the water template's globals and the packet; drawn without depth writes.
void Rdr_Water(ptr32<sRdrVUShape> pShape, ptr32<_sRdrCamera> pCamera)
{
    if (!pShape || !pCamera)
        return;
    const auto global = [](uint32_t address) -> uint32_t & { return *static_cast<uint32_t *>(guest_host(address)); };
    const ptr32<_sGfxTexture> bump = *ptr32<ptr32<_sGfxTexture>>(pShape.address + 0x60);
    const uint64_t tex0 = gsTex0(bump ? bump.get() : nullptr);
    sRdr &rdr = G(gRdr);
    const int32_t index = rdr.waterCurrShapeData;
    global((sym::sec_vutext + 0xd4u)) = static_cast<uint32_t>(tex0 >> 32);
    rdr.waterCurrShapeData = rdr.waterCurrShapeData + 1;
    global(sym::gRdr_WaterBumpTex) = static_cast<uint32_t>(tex0);
    if (kWaterPackets - 1 < rdr.waterCurrShapeData)
        rdr.waterCurrShapeData = 0;
    const _sRdrCamera &camera = *pCamera;
    const ee_float lightX = camera.pos.x + camera.mat.Zx * ee_float(2000.0f);
    std::memcpy(guest_host(sym::gRdr_WaterLightPos), &lightX, 4);
    const uint32_t address = (rdr.pWaterShapeData + index * rdr.waterShapeDataSz).address;
    uint8_t *p = static_cast<uint8_t *>(guest_host(address));
    const ee_float lightY = camera.pos.y + ee_float(1000.0f);
    std::memcpy(guest_host((sym::sec_vutext + 0xc4u)), &lightY, 4);
    global((sym::sec_vutext + 0xccu)) = 0;
    const ee_float lightZ = camera.pos.z + camera.mat.Zz * ee_float(2000.0f);
    std::memcpy(guest_host((sym::sec_vutext + 0xc8u)), &lightZ, 4);
    writeShapeCamera(p, 0x1a0, *pShape, camera);
    putw(p, 0xa0, global(sym::gRdr_WaterBumpUVs));
    putw(p, 0xa4, global((sym::sec_vutext + 0xa4u)));
    for (uint32_t k = 0; k < 8; ++k)
        putw(p, 0xc0 + k * 4u, global(sym::gRdr_WaterLightPos + k * 4u));
    RdrState_ForceApplyStateBlock(ptr32<sRdrStateBlock>(guest_addr(&G(waterStateBlock))));
    Rdr_UploadProg(ptr32<u32>(kWaterProgram));
    Rdr_AddDmaCall(ptr32<void>(address));
    Rdr_AddDmaCall(pShape->pTri.cast<void>());
    RdrState_ApplyDefaultStateBlock();
}

// RDRALPHA.c: transparent triangles. Shapes hand theirs over as they are drawn (moved into world space unless the
// shape sits at the origin unrotated); once a frame they are sorted back to front and drawn in one DMA chain with
// depth writes off.

namespace {

const uint32_t kQsort = sym::qsort, kRdrAlphaSortFunc = sym::RdrAlphaSortFunc, kPrintf = sym::printf;

ee_float f(f32 value) { return ee_float(value); }

// The camera the alpha pass draws with, rebuilt from the game's 16-bit view matrix.
void alphaCamera(_sRdrCamera &cam)
{
    const view3dClass &view = G(view3dVarG);
    const _matPsx &t = *view.pPsxTViewMatrix;
    const vec32 &trans = view.pPsxViewMatrix->trans;
    const ee_float k = ee_float(0.00024414062f);
    const auto unit = [&](frac value) { return ee_itof(static_cast<int16_t>(value)) * k; };
    cam.mat.Xx = unit(t.mat.Xx);
    cam.mat.Xy = unit(t.mat.Xy);
    cam.mat.Xz = unit(t.mat.Xz);
    cam.mat.Yx = unit(t.mat.Yx);
    cam.mat.Yy = unit(t.mat.Yy);
    cam.mat.Yz = unit(t.mat.Yz);
    cam.mat.Zx = unit(t.mat.Zx);
    cam.mat.Zy = unit(t.mat.Zy);
    cam.pos.x = ee_itof(trans.x);
    cam.pos.y = ee_itof(trans.y);
    cam.pos.z = ee_itof(trans.z);
    cam.mat.Zz = unit(t.mat.Zz);
    const ee_float toAngle = *static_cast<ee_float *>(guest_host(sym::lit4_327bc4));
    const ee_float toRadians = *static_cast<ee_float *>(guest_host(sym::lit4_327bc8));
    const int32_t fov = ee_ftoi(G(lsVarG).fov * toAngle) & 0xffff;
    const ee_float half = ee_itof(ee_ftoi(ee_itof(fov) * ee_float(0.5f)) & 0xffff) * toRadians;
    const ee_float sine = ee_sinf(half), cosine = ee_cosf(half);
    const ee_float dValX = ee_float(1.0f) / (sine / cosine) * ee_itof(G(gGfxP2).wd) * ee_float(0.5f);
    cam.dValX = dValX;
    cam.dValY = dValX * *static_cast<ee_float *>(guest_host(sym::lit4_327bcc));
    const int32_t player = G(gRdrPolyPlayer);
    cam.vertDrawOffset = player == 1 ? -56.0f : player == 2 ? 56.0f : 0.0f;
}

void transformPoint(f32 *out, const f32 *in, const sMat33 &m, const sVec3 &pos)
{
    out[0] = in[0] * m.Xx + f(in[1]) * m.Yx + f(in[2]) * m.Zx + pos.x;
    out[1] = f(in[0]) * m.Xy + f(in[1]) * m.Yy + f(in[2]) * m.Zy + pos.y;
    out[2] = f(in[0]) * m.Xz + f(in[1]) * m.Yz + f(in[2]) * m.Zz + pos.z;
}

ee_float depth(const f32 *v, const _sRdrCamera &cam)
{
    return (f(v[0]) - cam.pos.x) * cam.mat.Zx + (f(v[1]) - cam.pos.y) * cam.mat.Zy + (f(v[2]) - cam.pos.z) * cam.mat.Zz;
}

} // namespace

void RdrAlphaCameraToRdrCamera(ptr32<_sRdrCamera> pCam)
{
    RC_DIVERT_IF_CHANGED(RdrAlphaCameraToRdrCamera, pCam);
    alphaCamera(*pCam);
}

// qsort's comparison: further first.
int32_t RdrAlphaSortFunc(ptr32<void> arg1, ptr32<void> arg2)
{
    const ee_float a = *ptr32<ee_float>(arg1.address + 4), b = *ptr32<ee_float>(arg2.address + 4);
    return ee_ftoi(b - a);
}

void RdrAlphaTerm()
{
    sRdrAlpha &alpha = G(gRdrAlpha);
    const auto release = [](auto &field) { MemBlockFree(ptr32<memHandle>(guest_addr(&field))); };
    for (int i = 0; i < 4; ++i)
    {
        release(alpha.pDmaTags[i]);
        release(alpha.pVifCommands[i]);
    }
    release(alpha.pTriSortList);
    release(alpha.pTempTris[0]);
    release(alpha.pTempTris[1]);
    release(G(pAlphaPtrListG));
    release(G(pAlphaTriListG));
}

void RdrAlphaInit()
{
    sRdrAlpha &alpha = G(gRdrAlpha);
    const auto allocate = [](auto &field, uint32 size) { MemBlockAllocLo(ptr32<memHandle>(guest_addr(&field)), size); };
    for (int i = 0; i < 4; ++i)
    {
        allocate(alpha.pDmaTags[i], 0x29f70);
        allocate(alpha.pVifCommands[i], 0x1800);
    }
    allocate(alpha.pTriSortList, 80000);
    allocate(alpha.pTempTris[0], 0xe0000);
    allocate(alpha.pTempTris[1], 0xe0000);
    alpha.numTriangles = 0;
    alpha.nextTempTri = static_cast<int32_t>((G(gRdr).frameNumber & 1) << 11);
    allocate(G(pAlphaPtrListG), 40000);
    allocate(G(pAlphaTriListG), 80000);
}

// Adds a shape's transparent triangles to this frame's list, keeping those with a corner in front of the camera.
void RdrAlphaAddTriangles(ptr32<sRdrVUShape> shape)
{
    sRdrAlpha &alpha = G(gRdrAlpha);
    const sRdrVUShape &sh = *shape;
    uint32_t count = static_cast<uint32_t>(sh.pAlpha->numPolys);
    ptr32<sVuTri> tris = ptr32<sVuTri>(sh.pAlpha.address + offsetof(sRdrAlphaData, alphaPolys));
    if (count + static_cast<uint32_t>(alpha.numTriangles) >= 10000)
        return;
    if (sh.pos.x.v != 0.0f || sh.pos.y.v != 0.0f || sh.pos.z.v != 0.0f || sh.mat.Xx.v != 1.0f || sh.mat.Yy.v != 1.0f || sh.mat.Zz.v != 1.0f)
    {
        if (0x7ffu < static_cast<uint32_t>(alpha.nextTempTri) + count)
            return;
        int32_t first = alpha.nextTempTri;
        if (G(gRdrPolyPlayer) == 2)
            first += 0x800;
        const ptr32<sVuTri> temp = alpha.pTempTris[G(gRdr).frameNumber & 1];
        for (uint32_t i = 0; i < count; ++i)
        {
            const sVuTri &in = tris[static_cast<int32_t>(i)];
            sVuTri &out = temp[first + static_cast<int32_t>(i)];
            out = in;
            transformPoint(out.vertex1, reinterpret_cast<const f32 *>(in.vertex1), sh.mat, sh.pos);
            transformPoint(out.vertex2, reinterpret_cast<const f32 *>(in.vertex2), sh.mat, sh.pos);
            transformPoint(out.vertex3, reinterpret_cast<const f32 *>(in.vertex3), sh.mat, sh.pos);
            if (out.backfaceDisable == 0)
            {
                const sMat33 &m = sh.mat;
                out.nx = f(in.nx) * m.Xx + f(in.ny) * m.Yx + f(in.nz) * m.Zx;
                out.ny = f(in.nx) * m.Xy + f(in.ny) * m.Yy + f(in.nz) * m.Zy;
                out.nz = f(in.nx) * m.Xz + f(in.ny) * m.Yz + f(in.nz) * m.Zz;
            }
            alpha.nextTempTri = alpha.nextTempTri + 1;
        }
        tris = temp + first;
    }
    _sRdrCamera camera;
    alphaCamera(camera);
    ptr32<sTriSorter> slot = alpha.pTriSortList + alpha.numTriangles;
    for (; count != 0; --count, tris = tris + 1)
    {
        const sVuTri &tri = *tris;
        const ee_float d1 = depth(reinterpret_cast<const f32 *>(tri.vertex1), camera);
        const ee_float d2 = depth(reinterpret_cast<const f32 *>(tri.vertex2), camera);
        const ee_float d3 = depth(reinterpret_cast<const f32 *>(tri.vertex3), camera);
        slot->distance = d1 + d2 + d3;
        if (0.0f <= d1.v || 0.0f <= d2.v || 0.0f <= d3.v)
        {
            slot->pTri = tris;
            alpha.numTriangles = alpha.numTriangles + 1;
            slot = slot + 1;
        }
    }
}

// Sorts the frame's transparent triangles and adds them to the frame as one chain: a camera packet, then for each
// triangle a DMA "ref" that unpacks it into VU1 memory, with a draw and flush whenever 0x180 quadwords fill up.
void RdrAlphaAddToDmaChain()
{
    GuestFrame frame(0x280);
    sRdrAlpha &alpha = G(gRdrAlpha);
    alpha.lastNumTriangles = alpha.numTriangles;
    if (alpha.numTriangles < 1)
    {
        alpha.nextTempTri = 0;
        return;
    }
    _sRdrCamera camera;
    alphaCamera(camera);
    const int32_t total = alpha.numTriangles;
    std::memcpy(G(pAlphaTriListG).get(), alpha.pTriSortList.get(), static_cast<size_t>(total) * 8u);
    for (int32_t i = 0; i < total; ++i)
        G(pAlphaPtrListG)[i] = G(pAlphaTriListG) + i;
    int32_t vifWords = 4;
    guest_call(kQsort, G(pAlphaPtrListG), total, 4, kRdrAlphaSortFunc);
    for (int32_t i = 0; i < alpha.numTriangles; ++i)
    {
        const sTriSorter &sorted = *G(pAlphaPtrListG)[i];
        sTriSorter &dst = alpha.pTriSortList[i];
        const ptr32<sVuTri> tri = sorted.pTri;
        dst.distance = sorted.distance;
        dst.pTri = tri;
    }

    // The camera packet, for an identity shape at the origin.
    sRdr &rdr = G(gRdr);
    uint32_t packetAddress = 0;
    uint8_t *packet = nextPacket(rdr.currShapeData, kShapePackets, rdr.pShapeData, rdr.shapeDataSz, &packetAddress);
    std::memset(packet + 0xc0, 0, 12);
    putw(packet, 0xcc, 0x3f800000);
    sRdrVUShape origin{};
    std::memcpy(&origin.mat, guest_host((sym::sec_rodata + 0x26c8u)), sizeof(sMat33));
    writeShapeCamera(packet, 0xf0, origin, camera);
    Rdr_UploadProg(ptr32<u32>(kShapeProgram));
    Rdr_AddDmaCall(ptr32<void>(packetAddress));

    const int32_t buffer = G(gRdrPolyPlayer) == 2 ? static_cast<int32_t>((rdr.frameNumber & 1) + 2) : static_cast<int32_t>(rdr.frameNumber & 1);
    uint32_t vif = alpha.pVifCommands[buffer].address;
    uint32_t tag = alpha.pDmaTags[buffer].address;
    const uint32_t vifEnd = vif + 0x600 * 4;
    const auto u8at = [](uint32_t a) -> uint8_t & { return *static_cast<uint8_t *>(guest_host(a)); };
    const auto u16at = [](uint32_t a) -> uint16_t & { return *static_cast<uint16_t *>(guest_host(a)); };
    const auto u32at = [](uint32_t a) -> uint32_t & { return *static_cast<uint32_t *>(guest_host(a)); };

    const ptr32<sRdrStateBlock> state(guest_addr(&G(block)));
    if (G(blockSetup) == 0)
    {
        RdrState_SetStateBlockToDefaults(state);
        RdrState_SetPixelAlphaBlendControlEnable(state, 0);
        RdrState_SetAlphaBlending(state, 0, 1, 0, 1, 0x80);
        RdrState_SetZBufferWriteEnable(state, 0);
        void RdrState_SetAlphaTestEnable(ptr32<sRdrStateBlock> block, u32 enable, u32 func, u32 fail, u8 ref);
        RdrState_SetAlphaTestEnable(state, 0, 0, 0, 0);
        G(blockSetup) = 1;
    }
    RdrState_ApplyStateBlock(state);

    // Unpacks a zero quadword at the VU address reached, then draws and flushes: the end of a batch.
    const auto endBatch = [&](uint32_t vuAddress)
    {
        u32at(tag + 8) = 0x1000404;
        u32at(tag + 12) = vuAddress | 0x6c018000u;
        u8at(tag + 3) = 0x30; // ref
        u8at(tag + 2) = 0;
        u32at(vif) = 0;
        u32at(tag + 4) = vif;
        u16at(tag) = 1;
        u32at(vif + 4) = 0;
        u16at(tag + 16) = 0;
        u32at(vif + 8) = 0;
        u32at(vif + 12) = 0;
    };
    int32_t drawn = 0;
    uint32_t vuAddress = 0;
    int32_t left = total;
    ptr32<sTriSorter> entry = alpha.pTriSortList;
    if (0 < left && 0 < G(maxAlphaPolys))
    {
        uint32_t reach = 0xf;
        for (;;)
        {
            uint32_t at = vuAddress;
            if (0x17f < reach)
            {
                endBatch(vuAddress);
                at = 0;
                vifWords += 4;
                vif += 16;
                u8at(tag + 0x13) = 0x30;
                u32at(tag + 0x18) = 0x14000000; // MSCAL
                u32at(tag + 0x1c) = 0x13000000; // FLUSHA
                u8at(tag + 0x12) = 0;
                u32at(tag + 0x14) = vif;
                tag += 32;
            }
            u8at(tag + 2) = 0;
            u8at(tag + 3) = 0x30;
            u16at(tag) = 0xe; // The triangle's 14 quadwords.
            --left;
            u32at(tag + 4) = entry->pTri.address;
            entry = entry + 1;
            u32at(tag + 8) = 0x1000404;
            ++drawn;
            u32at(tag + 12) = at | 0x6c0e8000u;
            vuAddress = at + 0xe;
            tag += 16;
            if (left <= 0 || drawn >= G(maxAlphaPolys))
                break;
            reach = at + 0x1d;
        }
    }
    endBatch(vuAddress);
    u8at(tag + 0x13) = 0x10; // cnt
    u32at(tag + 0x1c) = 0x13000000;
    u8at(tag + 0x12) = 0;
    u32at(tag + 0x18) = 0x14000000;
    u32at(tag + 0x14) = vif + 16;
    u8at(tag + 0x23) = 0x60; // ret
    u8at(tag + 0x22) = 0;
    u32at(tag + 0x2c) = 0;
    u16at(tag + 0x20) = 0;
    u32at(tag + 0x24) = 0;
    u32at(tag + 0x28) = 0;
    if (-1 < G(maxAlphaPolys) && 0 < drawn)
        Rdr_AddDmaCall(alpha.pDmaTags[buffer].cast<void>());
    RdrState_ApplyDefaultStateBlock();
    alpha.nextTempTri = 0;
    alpha.numTriangles = 0;
    if (vifEnd < vif + 16)
        guest_call(kPrintf, (sym::sec_rodata + 0x26f0u)); // "overflowed\n"
    if (0x600 < vifWords)
        guest_call(kPrintf, (sym::sec_rodata + 0x2700u)); // "ovecounted\n"
}

} // namespace rc

RC_DECOMP_FN(Rdr_ShapeConstruct)
RC_DECOMP_FN_NOFUZZ(Rdr_FixupShapeTex)
RC_DECOMP_FN_NOFUZZ(Rdr_FixVUGridDma) // Walks a DMA list to its end tag.
RC_DECOMP_FN_NOFUZZ(Rdr_FixVUShapeDma)
RC_DECOMP_FN_NOFUZZ(Rdr_FixVUShape)
RC_DECOMP_FN(Rdr_VUShapeExecute)
RC_DECOMP_FN_NOFUZZ(Rdr_ShapeTerm)
RC_DECOMP_FN_NOFUZZ(Rdr_ShapeInit)
RC_DECOMP_FN(Rdr_VUShape)
RC_DECOMP_FN(Rdr_VUShapeSetup)
RC_DECOMP_FN_NOFUZZ(TRdr_VUShape) // Random alpha counts would loop for ever.
RC_DECOMP_FN_NOFUZZ(TRdr_VUShapeCell)
RC_DECOMP_FN(TERdr_VUShape)
RC_DECOMP_FN_NOFUZZ(Rdr_FixVUSkinDma)
RC_DECOMP_FN_NOFUZZ(Rdr_SkinTerm)
RC_DECOMP_FN_NOFUZZ(Rdr_FixVUSkin)
RC_DECOMP_FN_NOFUZZ(Rdr_SkinInit)
RC_DECOMP_FN_NOFUZZ(Rdr_VUSkin)
RC_DECOMP_FN_NOFUZZ(Rdr_WaterTerm)
RC_DECOMP_FN_NOFUZZ(Rdr_WaterInit)
RC_DECOMP_FN_NOFUZZ(Rdr_Water)
RC_DECOMP_FN_NOFUZZ(RdrAlphaCameraToRdrCamera)
RC_DECOMP_FN_NOFUZZ(RdrAlphaSortFunc)
RC_DECOMP_FN_NOFUZZ(RdrAlphaTerm)
RC_DECOMP_FN_NOFUZZ(RdrAlphaInit)
RC_DECOMP_FN_NOFUZZ(RdrAlphaAddTriangles)
RC_DECOMP_FN_NOFUZZ(RdrAlphaAddToDmaChain)
