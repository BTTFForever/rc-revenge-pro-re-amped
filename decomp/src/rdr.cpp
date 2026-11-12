// Decompiled from the renderer's lowest layer: RDRSTATE.c (blend, test, scissor and depth state for the graphics
// chip), RDR.c (the frame's DMA chain), RENDER.c and TEXTURE.c (setup), and the trivial parts of GFX_P2.c.
//
// Drawing on the PS2 is a chain of DMA tags that VIF1 walks once a frame: each "call" tag points at a packet to run,
// such as a block of graphics chip register writes or a microprogram upload. gRdr.pDmaTag holds two chains of 1000
// tags, one per frame, swapped each frame.

#include "ee_math.h"

namespace rc {

bool_ MemBlockAllocLo(ptr32<memHandle> pHandle, uint32 size);
void MemBlockFree(ptr32<memHandle> pHandle);
void ResTypeSetConstructDaemon(ptr32<char> pType, uint32 constructDaemon, int16 options);

namespace {

// Microprograms: the shape renderer's and the track renderer's. The state packets start whichever is loaded.
const uint32_t kShapeProgram = 0x1d9840;     // _dma_48
const uint32_t kTrackProgramEnd = 0x1dc8a0;  // gTRdr_EndShapeData
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

void RenderFreeBuffers() {}
void RenderTerm() {}

// TEXTURE.c

void TexFreeIntermediateResData() {}

} // namespace rc

RC_DECOMP_FN(Rdr_WhichProg, 0x1a73a8)
RC_DECOMP_FN(Rdr_AddDmaCall, 0x1a4498)
RC_DECOMP_FN(Rdr_UploadProg, 0x1a74d0)
RC_DECOMP_FN(Rdr_UseCamera, 0x1a4400)
RC_DECOMP_FN(Rdr_DrawPoint, 0x1a4410)
RC_DECOMP_FN(Rdr_DrawTri, 0x1a4418)
RC_DECOMP_FN(Rdr_DrawQuad, 0x1a4420)
RC_DECOMP_FN_NOFUZZ(Rdr_Term, 0x1a3628)
RC_DECOMP_FN(GfxP2_BeginScene, 0x18fc88)
RC_DECOMP_FN(GfxP2_EndScene, 0x18fc90)
RC_DECOMP_FN(Rdr_StartFrame, 0x1a3638)
RC_DECOMP_FN(RdrState_SetAlphaBlending, 0x1a13b8)
RC_DECOMP_FN(RdrState_SetAlphaTestEnable, 0x1a13f0)
RC_DECOMP_FN(RdrState_SetZTestEnable, 0x1a1448)
RC_DECOMP_FN(RdrState_SetTextureClamp, 0x1a1498)
RC_DECOMP_FN(RdrState_SetScissor, 0x1a14a8)
RC_DECOMP_FN(RdrState_SetZBufferWriteEnable, 0x1a1500)
RC_DECOMP_FN(RdrState_SetPixelAlphaBlendControlEnable, 0x1a1540)
RC_DECOMP_FN(RdrState_SetStateBlockToDefaults, 0x1a1550)
RC_DECOMP_FN(RdrState_ForceApplyStateBlock, 0x1a1058)
RC_DECOMP_FN(RdrState_ApplyStateBlock, 0x1a1050)
RC_DECOMP_FN(RdrState_ApplyDefaultStateBlock, 0x1a1040)
RC_DECOMP_FN_NOFUZZ(RdrState_Init, 0x1a1268)
RC_DECOMP_FN(RenderFrameInit, 0x183c38)
RC_DECOMP_FN(RenderAllocateBuffers, 0x183c80)
RC_DECOMP_FN(RenderFreeBuffers, 0x183c98)
RC_DECOMP_FN(RenderTerm, 0x183c30)
RC_DECOMP_FN(TexFreeIntermediateResData, 0x183ee8)
