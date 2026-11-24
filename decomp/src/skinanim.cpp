// Decompiled from VERTANIM.c, SKINANIM.c and SVF.c: the loaders' construct daemons for vertex-animated shapes, skinned
// (matrix-animated) models and the SVF normal tables, and the matrix animation player (MxAnim).
//
// Never called by the game: SVFConvertNormal, SvfRender, SvfRenderPoints (and VertAnimFrameUpdate/VertAnimRender are
// empty). VAIConstruct, VertAnimConstruct, RSNConstructDaemon, RSRConstructDaemon and SvfConstructDaemon are reached only
// through the loader's daemon table.

#include "display_fuzz.h"

#include <cstring>

#include "rc_decomp.h"

// Europe variant: SvfConstructDaemon differs only in register use and instruction order (the same body;
// tools/asm_listing.py NAME --diff).
RC_VERSION_VARIANT(SvfConstructDaemon)

namespace rc {

void ResTypeSetConstructDaemon(ptr32<char> pType, uint32 constructDaemon, int16 options); // res.cpp
void ResTypeUnSetConstructDaemon(ptr32<char> pType);
void Rdr_FixVUShape(ptr32<sRdrVUShape> pShape); // shape.cpp
void Rdr_FixVUSkin(ptr32<sRdrVUSkin> pSkin);
void Rdr_VUSkin(ptr32<sRdrVUSkin> pSkin, ptr32<sAnimOutput> pAnimOut, ptr32<_sRdrCamera> pCamera);
angle ArcTan(int32 x, int32 y); // maths.cpp

namespace {

constexpr uint32_t kResAlloc = sym::ResAlloc;
// The resource type names ("VTX", "VAL", "VAI"; "RSO", "RSA", "RSK", "RSN", "RSH", "RSR"; "SVF") and their daemons.
constexpr uint32_t kVertAnimTypes[3] = {(sym::sec_sdata + 0x5c0u), (sym::sec_sdata + 0x5c8u), (sym::sec_sdata + 0x5d0u)};
constexpr uint32_t kSkinTypes[6] = {(sym::sec_sdata + 0x740u), (sym::sec_sdata + 0x748u), (sym::sec_sdata + 0x750u), (sym::sec_sdata + 0x758u), (sym::sec_sdata + 0x760u), (sym::sec_sdata + 0x768u)};
constexpr uint32_t kSvfType = (sym::sec_sdata + 0x550u);
constexpr uint32_t kVertAnimConstruct = sym::VertAnimConstruct, kVAIConstruct = sym::VAIConstruct, kRSNConstructDaemon = sym::RSNConstructDaemon,
                   kRSRConstructDaemon = sym::RSRConstructDaemon, kSvfConstructDaemon = sym::SvfConstructDaemon;

template <typename T>
T &mem(uint32_t address) { return *static_cast<T *>(guest_host(address)); }

} // namespace

// VERTANIM.c

void VertAnimTerm()
{
    for (const uint32_t type : kVertAnimTypes)
        ResTypeUnSetConstructDaemon(ptr32<char>(type));
}

void VertAnimConstruct(ptr32<memHandle> pHandle)
{
    const uint32_t shape = mem<uint32_t>(pHandle.address);
    G(vaVarsG).active = 1;
    Rdr_FixVUShape(ptr32<sRdrVUShape>(shape));
}

// A vertex animation instance: its flags go to its shape (+0x7c), with 0x40 added when 0x100 is set.
void VAIConstruct(ptr32<memHandle> pHandle)
{
    const uint32_t instance = mem<uint32_t>(pHandle.address);
    uint32_t flags = mem<uint32_t>(instance + 4u);
    if ((flags & 0x100u) != 0)
    {
        flags |= 0x40u;
        mem<uint32_t>(instance + 4u) = flags;
    }
    mem<uint32_t>(mem<uint32_t>(instance) + 0x7cu) = flags;
}

void VertAnimInit()
{
    ResTypeSetConstructDaemon(ptr32<char>(kVertAnimTypes[0]), kVertAnimConstruct, 6);
    ResTypeSetConstructDaemon(ptr32<char>(kVertAnimTypes[1]), 0, 6);
    ResTypeSetConstructDaemon(ptr32<char>(kVertAnimTypes[2]), kVAIConstruct, 6);
    G(vaVarsG).active = 0;
}

void VertAnimFrameUpdate(uint32) {}
void VertAnimRender() {}

// SKINANIM.c

void MxAnim_SetRate(ptr32<sMxAnimCtrl> pCtrl, ee_float rate)
{
    sMxAnimCtrl &ctrl = *pCtrl;
    const ptr32<sMxAnim> anim = ctrl.pAnim;
    const ee_float frameLen = ee_float(1000.0f) / rate;
    ctrl.frameRate = rate;
    const uint32_t last = anim->numFrames - 1u;
    ctrl.frameLen = frameLen;
    ee_float frames;
    if (static_cast<int32_t>(last) >= 0)
        frames = ee_itof(static_cast<int32_t>(last));
    else
    {
        const ee_float half = ee_itof(static_cast<int32_t>((last >> 1) | (last & 1u))); // unsigned to float
        frames = half + half;
    }
    const ee_float length = frames * ctrl.frameLen;
    ctrl.endTime = static_cast<s32>(static_cast<uint32_t>(ctrl.startTime) + static_cast<uint32_t>(ee_ftoi(length)));
}

void MxAnim_Construct(ptr32<sMxAnimCtrl> pCtrl, ptr32<sMxAnim> pAnim)
{
    sMxAnimCtrl &ctrl = *pCtrl;
    ctrl.nextFrame = 1;
    ctrl.pAnim = pAnim;
    ctrl.startTime = 0;
    ctrl.currFrame = 1;
    mem<uint32_t>(pCtrl.address + 0x14u) = 0; // tween
    MxAnim_SetRate(pCtrl, ee_float(30.0f));
}

// The current frame's node matrices, each placed by the object's matrix and position: out = node * tsoMat, and the
// position node.W * tsoMat + tsoPos. The object's matrix is read once per node for the position and for the first
// output entry, and again for each other entry.
void MxAnim_OutputFlatFrame(ptr32<sAnimOutput> pOutput, ptr32<sMxAnimCtrl> pCtrl, s32, ptr32<_sVec3> pTsoPos,
                            ptr32<_sMat33> pTsoMat)
{
    const ptr32<sMxAnim> anim = pCtrl->pAnim;
    const uint32_t frames = anim->pFrames.address;
    pOutput->numNodes = static_cast<s32>(anim->numNodes);
    const uint32_t frame = frames + static_cast<uint32_t>(pCtrl->currFrame) * anim->numNodes * 0x30u;
    if (anim->numNodes == 0)
        return;
    const uint32_t aPos = pOutput->aPos.address, aMat = pOutput->aMat.address;
    const auto f = [](uint32_t address) -> ee_float & { return mem<ee_float>(address); };
    const uint32_t m = pTsoMat.address;
    uint32_t i = 0;
    do
    {
        const ee_float yx = f(m + 0xcu), xx = f(m + 0x0u), zx = f(m + 0x18u), zy = f(m + 0x1cu), xz = f(m + 0x8u);
        const uint32_t out = mem<uint32_t>(aMat + i * 4u);
        const uint32_t node = frame + i * 0x30u, wRow = node + 0x24u;
        const ee_float xy = f(m + 0x4u), yy = f(m + 0x10u), yz = f(m + 0x14u), zz = f(m + 0x20u);
        ++i;
        const ee_float tx = f(wRow) * xx + f(wRow + 4u) * yx + f(wRow + 8u) * zx;
        const ee_float ty = f(wRow) * xy + f(wRow + 4u) * yy + f(wRow + 8u) * zy;
        const ee_float tz = f(wRow) * xz + f(wRow + 4u) * yz + f(wRow + 8u) * zz;
        f(out + 0x0u) = f(node + 0x0u) * xx + f(node + 0x4u) * yx + f(node + 0x8u) * zx;
        for (uint32_t k = 1; k < 9; ++k)
        {
            const uint32_t row = node + (k / 3u) * 0xcu, column = m + (k % 3u) * 4u;
            const ee_float a = f(row) * f(column);
            const ee_float b = f(row + 4u) * f(column + 0xcu);
            const ee_float c = f(row + 8u) * f(column + 0x18u);
            if (k == 8)
            {
                const uint32_t pos = mem<uint32_t>(aPos + (i - 1u) * 4u);
                const uint32_t count = anim->numNodes;
                f(out + 0x20u) = a + b + c;
                f(pos + 0x0u) = tx + pTsoPos->x;
                f(pos + 0x4u) = ty + pTsoPos->y;
                f(pos + 0x8u) = tz + pTsoPos->z;
                if (i >= count)
                    return;
            }
            else
                f(out + k * 4u) = a + b + c;
        }
    } while (true);
}

void SkinAnimTerm()
{
    for (const uint32_t type : kSkinTypes)
        ResTypeUnSetConstructDaemon(ptr32<char>(type));
}

void SkinAnimRdr(ptr32<_sRSRef> pAnimRef, int32)
{
    Rdr_VUSkin(pAnimRef->anim->pVUSkin, pAnimRef->pAnimOutput, ptr32<_sRdrCamera>(guest_addr(&G(rdrCameraG))));
}

void RSNConstructDaemon(ptr32<memHandle> pHandle)
{
    Rdr_FixVUSkin(ptr32<sRdrVUSkin>(mem<uint32_t>(pHandle.address)));
}

// A skinned model instance: its animation control and its output arrays (a position, matrix and rotation per node for
// each node), taken from the resource area, then the animation started at the instance's rate.
void RSRConstructDaemon(ptr32<memHandle> pHandle)
{
    const uint32_t ref = mem<uint32_t>(pHandle.address);
    _sRSRef &r = *ptr32<_sRSRef>(ref);
    const int32_t nodes = static_cast<int32_t>(r.anim->pSkinAnim->numNodes);
    const auto alloc = [](uint32_t size) { return guest_call<uint32_t>(kResAlloc, size); };
    r.pAnimCtrl = ptr32<sMxAnimCtrl>(alloc(0x34));
    r.pAnimOutput = ptr32<sAnimOutput>(alloc(0x14));
    const uint32_t listSize = static_cast<uint32_t>(nodes) << 2;
    uint32_t block = alloc(listSize);
    r.pAnimOutput->aPos = ptr32<ptr32<_sVec3>>(block);
    block = alloc(listSize);
    r.pAnimOutput->aMat = ptr32<ptr32<_sMat33>>(block);
    block = alloc(listSize);
    r.pAnimOutput->aQtn = ptr32<ptr32<_sQtn>>(block);
    for (int32_t i = 0; i < nodes; ++i)
    {
        block = alloc(static_cast<uint32_t>(nodes) * 0xcu);
        mem<uint32_t>(r.pAnimOutput->aPos.address + static_cast<uint32_t>(i) * 4u) = block;
        block = alloc(static_cast<uint32_t>(nodes) * 0x24u);
        mem<uint32_t>(r.pAnimOutput->aMat.address + static_cast<uint32_t>(i) * 4u) = block;
        block = alloc(static_cast<uint32_t>(nodes) << 4);
        mem<uint32_t>(r.pAnimOutput->aQtn.address + static_cast<uint32_t>(i) * 4u) = block;
    }
    MxAnim_Construct(r.pAnimCtrl, r.anim->pSkinAnim);
    MxAnim_SetRate(r.pAnimCtrl, ee_float(r.frameRate));
    const uint32_t skin = r.anim->pVUSkin.address;
    sMxAnimCtrl &ctrl = *r.pAnimCtrl;
    ctrl.currFrame = 1;
    mem<uint32_t>(ref + 0x30u) = 0; // frameRate
    uint8_t offset[12];
    std::memcpy(offset, guest_host(skin + 0x2cu), sizeof(offset)); // the skin's position
    std::memcpy(&ctrl.offset, offset, sizeof(offset));
    if ((r.flags & 0x100u) != 0)
        r.flags = r.flags | 0x40u;
}

void SkinAnimInit()
{
    ResTypeSetConstructDaemon(ptr32<char>(kSkinTypes[0]), 0, 4);
    ResTypeSetConstructDaemon(ptr32<char>(kSkinTypes[1]), 0, 6);
    ResTypeSetConstructDaemon(ptr32<char>(kSkinTypes[2]), 0, 4);
    ResTypeSetConstructDaemon(ptr32<char>(kSkinTypes[3]), kRSNConstructDaemon, 6);
    ResTypeSetConstructDaemon(ptr32<char>(kSkinTypes[4]), 0, 4);
    ResTypeSetConstructDaemon(ptr32<char>(kSkinTypes[5]), kRSRConstructDaemon, 6);
    G(pTestTOB) = nullptr;
}

// SVF.c

void SvfInit() { ResTypeSetConstructDaemon(ptr32<char>(kSvfType), kSvfConstructDaemon, 2); }

// The index of the table vector nearest a normal: first the slice whose height (y) is nearest, then, unless the slice
// holds one vector, the vector round it at the normal's heading. With no slices the slice is whatever the caller left
// in $s0 (the original never sets it).
uint32 SVFConvertNormal(ptr32<anon_949e6798> pNormal)
{
    const uint32_t header = G(svfVarG).pSvf.address;
    uint32_t best = getRegU32(&g_runtime->eeScheduler().runningContext(), 16);
    int32_t nearest = 1000000;
    const uint32_t vectors = mem<uint32_t>(header + 8u);
    if (mem<uint16_t>(header + 2u) != 0)
    {
        const int32_t y = mem<int16_t>(pNormal.address + 2u);
        uint32_t slice = header + 0xcu;
        int32_t i = 0;
        do
        {
            const int32_t sliceY = mem<int16_t>(vectors + static_cast<uint32_t>(mem<uint16_t>(slice + 2u)) * 8u + 2u);
            int32_t distance = static_cast<int32_t>(static_cast<uint32_t>(sliceY) - static_cast<uint32_t>(y));
            if (distance <= 0)
                distance = static_cast<int32_t>(static_cast<uint32_t>(y) - static_cast<uint32_t>(sliceY));
            if (distance < nearest)
            {
                nearest = distance;
                best = slice;
            }
            ++i;
            slice += 4u;
        } while (i < static_cast<int32_t>(mem<uint16_t>(header + 2u)));
    }
    const uint32_t count = mem<uint16_t>(best);
    if (count == 1)
        return mem<uint16_t>(best + 2u);
    const uint32_t heading = static_cast<uint16_t>(ArcTan(mem<int16_t>(pNormal.address + 4u), mem<int16_t>(pNormal.address + 0u)));
    const int32_t step = ee_div(0x10000, static_cast<int32_t>(mem<uint16_t>(best)));
    const int32_t centred = static_cast<int32_t>((heading - static_cast<uint32_t>(step >> 1)) << 14);
    const int32_t index = static_cast<int32_t>(static_cast<uint32_t>(ee_div(centred, step)) + 0x2000u) >> 14;
    return static_cast<uint32_t>(mem<uint16_t>(best + 2u)) + static_cast<uint32_t>(index);
}

void SvfRender(ptr32<anon_3eaf0e53>, ptr32<vec32_>) {}
void SvfRenderPoints() {}

// The table's vectors are stored at four times their scale.
void SvfConstructDaemon(ptr32<memHandle> pHandle)
{
    const uint32_t header = mem<uint32_t>(pHandle.address);
    const uint16_t count = mem<uint16_t>(header);
    G(svfVarG).pSvf = ptr32<svfHeader>(header);
    uint32_t vector = mem<uint32_t>(header + 8u);
    if (count == 0)
        return;
    uint32_t i = 0;
    do
    {
        const int16_t x = mem<int16_t>(vector), y = mem<int16_t>(vector + 2u), z = mem<int16_t>(vector + 4u);
        ++i;
        mem<int16_t>(vector) = static_cast<int16_t>(x >> 2);
        mem<int16_t>(vector + 2u) = static_cast<int16_t>(y >> 2);
        mem<int16_t>(vector + 4u) = static_cast<int16_t>(z >> 2);
        vector += 8u;
    } while (i < mem<uint16_t>(header));
}

} // namespace rc

RC_DECOMP_FN_SETUP_HEAP(VertAnimTerm, rc::fuzz::resDaemons)
RC_DECOMP_FN_SETUP(VertAnimConstruct, rc::fuzz::vuShape)
RC_DECOMP_FN_SETUP(VAIConstruct, rc::fuzz::vaInstance)
RC_DECOMP_FN_SETUP_HEAP(VertAnimInit, rc::fuzz::resDaemons)
RC_DECOMP_FN(VertAnimFrameUpdate)
RC_DECOMP_FN(VertAnimRender)
RC_DECOMP_FN_SETUP(MxAnim_SetRate, rc::fuzz::mxCtrl)
RC_DECOMP_FN_SETUP(MxAnim_Construct, rc::fuzz::mxCtrl)
RC_DECOMP_FN_SETUP(MxAnim_OutputFlatFrame, rc::fuzz::mxOutput)
RC_DECOMP_FN_SETUP_HEAP(SkinAnimTerm, rc::fuzz::resDaemons)
RC_DECOMP_FN_SETUP_HEAP(SkinAnimRdr, rc::fuzz::skinRef)
RC_DECOMP_FN_SETUP_HEAP(RSNConstructDaemon, rc::fuzz::vuSkin)
RC_DECOMP_FN_SETUP_HEAP(RSRConstructDaemon, rc::fuzz::skinInstance)
RC_DECOMP_FN_SETUP_HEAP(SkinAnimInit, rc::fuzz::resDaemons)
RC_DECOMP_FN_SETUP_HEAP(SvfInit, rc::fuzz::resDaemons)
RC_DECOMP_FN_SETUP(SVFConvertNormal, rc::fuzz::svfNormal)
RC_DECOMP_FN(SvfRender)
RC_DECOMP_FN(SvfRenderPoints)
RC_DECOMP_FN_SETUP(SvfConstructDaemon, rc::fuzz::svfTable)
