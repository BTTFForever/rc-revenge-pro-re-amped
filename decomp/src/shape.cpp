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
void RdrState_SetPixelAlphaBlendControlEnable(ptr32<sRdrStateBlock> block, u32 enable);
void RdrState_SetAlphaBlending(ptr32<sRdrStateBlock> block, u32 A, u32 B, u32 C, u32 D, u8 fixedAlpha);
void RdrState_ApplyStateBlock(ptr32<sRdrStateBlock> block);
void RdrState_ApplyDefaultStateBlock();
uint64_t gsTex0(const _sGfxTexture *tex); // rdr.cpp

namespace {

const uint32_t kShapeTemplate = 0x1d8880;  // The packet every shape ring entry starts as.
const uint32_t kTrackEdTemplate = 0x1dec80; // The same for the Track Editor ring (Ghidra shows both as _dma_1).
const uint32_t kShapeProgram = 0x1d9840;   // _dma_48: track and shape renderer.
const uint32_t kEnvProgram = 0x1dc8a0;     // gTRdr_EndShapeData: the shiny (environment map) pass.
const uint32_t kVUShapeProgram = 0x1e70e0; // _dma_47: Rdr_VUShape's renderer.
const uint32_t kVUShapePacket = 0x1e60f0;  // Rdr_VUShape's single packet; the gRdr_* globals are inside it.
const uint32_t kRdrAlphaAddTriangles = 0x1a17b0;
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
    const uint32_t mainAddress = fillTrackPacket(*pShape, *pCamera, tint, *static_cast<ee_float *>(guest_host(0x327becu)), true, envAddress);
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
            const ptr32<sRdrStateBlock> state = envState(0x4b9718);
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
    const uint32_t mainAddress = fillTrackPacket(*pShape, *pCamera, tint, *static_cast<ee_float *>(guest_host(0x327bf0u)), false, envAddress);
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
            const ptr32<sRdrStateBlock> state = envState(0x4b9748);
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
    Rdr_UploadProg(ptr32<u32>(kShapeProgram));
    Rdr_AddDmaCall(ptr32<void>(address));
    Rdr_AddDmaCall(pShape->pTri.cast<void>());
}

} // namespace rc

RC_DECOMP_FN(Rdr_ShapeConstruct, 0x1a7210)
RC_DECOMP_FN_NOFUZZ(Rdr_FixupShapeTex, 0x1a7230)
RC_DECOMP_FN_NOFUZZ(Rdr_FixVUGridDma, 0x1a7290) // Walks a DMA list to its end tag.
RC_DECOMP_FN_NOFUZZ(Rdr_FixVUShapeDma, 0x1a73b0)
RC_DECOMP_FN_NOFUZZ(Rdr_FixVUShape, 0x1a4c28)
RC_DECOMP_FN(Rdr_VUShapeExecute, 0x1a7208)
RC_DECOMP_FN_NOFUZZ(Rdr_ShapeTerm, 0x1a4bf8)
RC_DECOMP_FN_NOFUZZ(Rdr_ShapeInit, 0x1a4ad0)
RC_DECOMP_FN_NOFUZZ(Rdr_VUShape, 0x1a5230)
RC_DECOMP_FN_NOFUZZ(Rdr_VUShapeSetup, 0x1a6e98)
RC_DECOMP_FN_NOFUZZ(TRdr_VUShape, 0x1a55b8)
RC_DECOMP_FN_NOFUZZ(TRdr_VUShapeCell, 0x1a5ec8)
RC_DECOMP_FN_NOFUZZ(TERdr_VUShape, 0x1a66f8)
