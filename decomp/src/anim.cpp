// Decompiled from ANIM.c: the level's animated scenery. The level's animation file (animVarG.pAnimData, from the level
// record) is a header of four counts and then four groups of entries: linear moves (0x64 bytes each), spins (0x50),
// swings (0x5c) and bezier paths (0x50). Each entry starts with an animHead: its scenery object (TSO), frame counters,
// flags, the collision object it may have (pOcol) and where that sits.
//
// Written from the instructions. AnimSetOcolPosition places a type 4 entry's collision on one bone of its model, the bone
// chosen by the TSO's tag; for an unknown tag it uses whatever its caller left in $s1 (AnimReinit's spin cursor or
// entry), so the native callers put the original's $s1 there for their calls (HeldS1).

#include "anim_fuzz.h"

namespace rc {

void MemBlockFree(ptr32<memHandle> pHandle);                                                         // mem.cpp
bool_ MemBlockAllocLo(ptr32<memHandle> pHandle, uint32 size);                                        // mem.cpp
void OColInitObj(ptr32<_sOColObj> pObj, ptr32<_sColCellVecPSX> pPos, ptr32<ee_float> pRad, ptr32<void> pData); // ocol.cpp
void OColDeleteObject(ptr32<_sOColObj> pObj);                                                        // ocol.cpp
void Vec32MulMatPsx(ptr32<vec32_> p, ptr32<_matPsx> mp, ptr32<vec32_> r);                            // maths.cpp
void TSO_UpdateMatrix(ptr32<_sTSO> pTSO);                                                            // tso.cpp
void TSOReInit();                                                                                    // tso.cpp
void AnimTerm();

namespace {

constexpr uint32_t kAnimVars = 0x1e99d0u, kLevelPtr = 0x327e80u;
constexpr uint32_t kBoneYOffset = 0x327c9cu; // 200.0: a type 0 entry's collision sits that far above its first bone
constexpr uint32_t kBoneIds = 0x327ca0u;     // the bones of the tags 0x10000365, 364, 368, 367, 366
constexpr uint32_t kStride[4] = {0x64u, 0x50u, 0x5cu, 0x50u};

template <typename T>
T &mem(uint32_t address) { return *static_cast<T *>(guest_host(address)); }

// A word read as the recompiled code reads it: an address outside RAM (a wild bone number) goes to the runtime's
// hardware path (0 for nothing mapped) instead of wrapping into RAM.
uint32_t loadWord(uint32_t address)
{
    if (PS2Runtime::isSpecialAddress(address))
        return g_runtime->Load32(g_rdram, &g_runtime->eeScheduler().runningContext(), address);
    return mem<uint32_t>(address);
}
ee_float loadFloat(uint32_t address)
{
    const uint32_t word = loadWord(address);
    ee_float f;
    std::memcpy(&f, &word, 4);
    return f;
}

uint32_t animData() { return mem<uint32_t>(kAnimVars); }
int32_t groupCount(uint32_t data, int group) { return mem<int16>(data + 2u * static_cast<uint32_t>(group)); }

// Where each group starts, from the counts as they were on entry.
void groupStarts(uint32_t data, uint32_t (&start)[4])
{
    start[0] = data + 8u;
    for (int g = 1; g < 4; ++g)
        start[g] = start[g - 1] + static_cast<uint32_t>(groupCount(data, g - 1) * static_cast<int32_t>(kStride[g - 1]));
}

// $s1 as the original has it for the calls made while this lives (see the file comment).
class HeldS1
{
public:
    explicit HeldS1(uint32_t value) : m_ctx(g_runtime->eeScheduler().runningContext()), m_saved(m_ctx.r[17])
    {
        SET_GPR_S32((&m_ctx), 17, static_cast<int32_t>(value));
    }
    ~HeldS1() { m_ctx.r[17] = m_saved; }
    HeldS1(const HeldS1 &) = delete;
    HeldS1 &operator=(const HeldS1 &) = delete;

private:
    R5900Context &m_ctx;
    std::remove_reference_t<decltype(std::declval<R5900Context &>().r[0])> m_saved;
};

} // namespace

void AnimSetOcolPosition(ptr32<animHead> pAnimHead)
{
    const uint32_t staleBone = getRegU32(&g_runtime->eeScheduler().runningContext(), 17);
    GuestFrame frame(0x50);
    const uint32_t head = pAnimHead.address;
    // The offset (floats) as integers at sp+0, turned by the object's matrix into sp+0x10.
    const ptr32<int32_t> offset = frame.local<int32_t>(0), turned = frame.local<int32_t>(0x10);
    offset[0] = ee_ftoi(mem<ee_float>(head + 0x2cu));
    offset[1] = ee_ftoi(mem<ee_float>(head + 0x30u));
    offset[2] = ee_ftoi(mem<ee_float>(head + 0x34u));
    Vec32MulMatPsx(offset.cast<vec32_>(), ptr32<_matPsx>(mem<uint32_t>(head) + 0x24u), turned.cast<vec32_>());
    const uint32_t tso = mem<uint32_t>(head);
    const int32_t t[3] = {turned[0], turned[1], turned[2]};
    for (uint32_t i = 0; i < 3; ++i)
    {
        const int32_t p = mem<int32_t>(tso + 0x10u + i * 4u);
        mem<ee_float>(head + 0xcu + i * 4u) = ee_itof(((p >= 0 ? p : p + 255) >> 8) + t[i]);
    }
    const int32_t type = mem<int16>(head + 0xau);
    if (type == 0)
    {
        const uint32_t model = mem<uint32_t>(tso + 0xcu);
        const uint32_t bone = mem<uint32_t>(mem<uint32_t>(mem<uint32_t>(model + 0x3cu) + 8u) + 8u);
        mem<ee_float>(head + 0xcu) = mem<ee_float>(bone);
        mem<ee_float>(head + 0x10u) = mem<ee_float>(bone + 4u) + mem<ee_float>(kBoneYOffset);
        mem<ee_float>(head + 0x14u) = mem<ee_float>(bone + 8u);
    }
    else if (type == 4)
    {
        const uint32_t tag = mem<uint32_t>(tso);
        const uint32_t model = mem<uint32_t>(tso + 0xcu);
        uint32_t boneId;
        switch (tag)
        {
        case 0x10000365u: boneId = mem<uint32_t>(kBoneIds); break;
        case 0x10000364u: boneId = mem<uint32_t>(kBoneIds + 4u); break;
        case 0x10000368u: boneId = mem<uint32_t>(kBoneIds + 8u); break;
        case 0x10000367u: boneId = mem<uint32_t>(kBoneIds + 12u); break;
        case 0x10000366u: boneId = mem<uint32_t>(kBoneIds + 16u); break;
        default: boneId = staleBone; break;
        }
        const uint32_t bones = mem<uint32_t>(mem<uint32_t>(model + 0x3cu) + 8u);
        const uint32_t bone = loadWord(bones + boneId * 4u);
        for (uint32_t i = 0; i < 3; ++i)
            mem<ee_float>(head + 0xcu + i * 4u) = loadFloat(bone + i * 4u) + ee_itof(t[i]);
    }
}

void AnimInit() {}

void AnimCreateOcolObj(ptr32<animHead> pAnimHead)
{
    const uint32_t head = pAnimHead.address;
    if (mem<uint32_t>(head + 0x40u) != 0)
        return;
    const int32_t type = mem<int16>(head + 0xau);
    if (type >= 6 || type < 0)
        return;
    MemBlockAllocLo(ptr32<memHandle>(head + 0x40u), 0x6c);
    AnimSetOcolPosition(pAnimHead);
    std::memcpy(guest_host(head + 0x1cu), guest_host(head + 0xcu), 16);
    mem<uint8>(mem<uint32_t>(head + 0x40u) + 0x20u) = 4; // OCOL_TYPE: animated scenery
    OColInitObj(ptr32<_sOColObj>(mem<uint32_t>(head + 0x40u)), ptr32<_sColCellVecPSX>(head + 0xcu), ptr32<ee_float>(head + 0x3cu),
                ptr32<void>(head));
}

void AnimUpdateOcolPos16(ptr32<animHead> pAnimHead)
{
    const uint32_t head = pAnimHead.address;
    if (mem<uint32_t>(head + 0x40u) == 0)
        return;
    std::memcpy(guest_host(head + 0x1cu), guest_host(head + 0xcu), 16);
    AnimSetOcolPosition(pAnimHead);
    mem<int32_t>(mem<uint32_t>(head + 0x40u) + 0x14u) = 1; // updated
}

void AnimDeleteOcolObj(ptr32<animHead> pAnimHead)
{
    const uint32_t head = pAnimHead.address;
    const uint32_t ocol = mem<uint32_t>(head + 0x40u);
    if (ocol == 0)
        return;
    OColDeleteObject(ptr32<_sOColObj>(ocol));
    MemBlockFree(ptr32<memHandle>(head + 0x40u));
    mem<uint32_t>(head + 0x40u) = 0;
}

void AnimSetupClass()
{
    mem<uint32_t>(kAnimVars) = mem<uint32_t>(mem<uint32_t>(kLevelPtr) + 8u);
}

void AnimLvlTerm() { AnimTerm(); }

// Drops every entry's collision object. The counts are read again as the loops go.
void AnimTerm()
{
    const uint32_t data = animData();
    if (data == 0)
        return;
    uint32_t start[4];
    groupStarts(data, start);
    for (int g = 0; g < 4; ++g)
        for (int32_t i = 0; i < groupCount(data, g); ++i)
        {
            const uint32_t entry = start[g] + static_cast<uint32_t>(i) * kStride[g];
            if (mem<uint32_t>(entry + 0x40u) != 0)
                AnimDeleteOcolObj(ptr32<animHead>(entry));
        }
}

// Puts every animation back to its start and gives it its collision object, then the scenery its matrices.
void AnimReinit()
{
    const uint32_t data = animData();
    if (data == 0)
        return;
    uint32_t start[4];
    groupStarts(data, start);
    const auto restart = [](uint32_t entry)
    {
        mem<uint16>(entry + 6u) = 0;                                                    // lastFrame
        mem<uint16>(entry + 8u) = static_cast<uint16>(mem<uint16>(entry + 8u) & 0xff2du); // flags
    };
    const auto running = [](uint32_t entry)
    {
        const uint16 flags = mem<uint16>(entry + 8u);
        if ((flags & 0x100) != 0)
            mem<uint16>(entry + 8u) = static_cast<uint16>(flags | 0x80);
    };
    // Linear moves: the object back at its start (unless it is hidden), the velocity cleared for 0x20.
    {
        HeldS1 s1(start[1]);
        for (int32_t i = 0; i < groupCount(data, 0); ++i)
        {
            const uint32_t entry = start[0] + static_cast<uint32_t>(i) * 0x64u;
            restart(entry);
            const uint32_t tso = mem<uint32_t>(entry);
            if ((mem<uint32_t>(tso + 0x20u) & 0xc00000u) == 0)
                std::memcpy(guest_host(tso + 0x10u), guest_host(entry + 0x4cu), 12);
            const uint16 flags = mem<uint16>(entry + 8u);
            if ((flags & 0x20) != 0)
            {
                mem<int16>(entry + 0x58u) = 0;
                mem<int16>(entry + 0x5au) = 0;
                mem<int16>(entry + 0x5cu) = 0;
            }
            if ((flags & 0x100) != 0)
                mem<uint16>(entry + 8u) = static_cast<uint16>(flags | 0x80);
            AnimCreateOcolObj(ptr32<animHead>(entry));
        }
    }
    // Spins and swings: the object's angle back to its start (the first halfword after the head).
    uint32_t spinCursor = start[1];
    for (int32_t i = 0; i < groupCount(data, 1); ++i, spinCursor += 0x50u)
    {
        const uint32_t entry = spinCursor;
        HeldS1 s1(entry);
        restart(entry);
        const uint32_t tso = mem<uint32_t>(entry);
        mem<uint16>(tso + 0x22u) = static_cast<uint16>(mem<uint16>(tso + 0x22u) | 2);
        mem<uint16>(tso + 0x36u) = mem<uint16>(entry + 0x4cu);
        running(entry);
        TSO_UpdateMatrix(ptr32<_sTSO>(mem<uint32_t>(entry)));
        AnimCreateOcolObj(ptr32<animHead>(entry));
    }
    HeldS1 s1(spinCursor);
    uint32_t swingCursor = start[2];
    for (int32_t i = 0; i < groupCount(data, 2); ++i, swingCursor += 0x5cu)
    {
        const uint32_t entry = swingCursor;
        restart(entry);
        const uint32_t tso = mem<uint32_t>(entry);
        mem<uint16>(tso + 0x22u) = static_cast<uint16>(mem<uint16>(tso + 0x22u) | 2);
        mem<uint16>(tso + 0x36u) = mem<uint16>(entry + 0x4cu);
        running(entry);
        TSO_UpdateMatrix(ptr32<_sTSO>(mem<uint32_t>(entry)));
        AnimCreateOcolObj(ptr32<animHead>(entry));
    }
    // Bezier paths: the frame count from the spline when it has none. The angle is read through the swing cursor (as
    // the original does: the first bezier entry's spline pointer when the counts are sound), not the entry.
    for (int32_t i = 0; i < groupCount(data, 3); ++i)
    {
        const uint32_t entry = start[3] + static_cast<uint32_t>(i) * 0x50u;
        restart(entry);
        const uint32_t tso = mem<uint32_t>(entry);
        mem<uint16>(tso + 0x22u) = static_cast<uint16>(mem<uint16>(tso + 0x22u) | 0x102);
        mem<uint16>(tso + 0x36u) = mem<uint16>(swingCursor + 0x4cu);
        if (mem<uint16>(entry + 4u) == 0)
            mem<uint16>(entry + 4u) = mem<uint16>(mem<uint32_t>(entry + 0x4cu) + 4u);
        running(entry);
        AnimCreateOcolObj(ptr32<animHead>(entry));
    }
    TSOReInit();
}

ptr32<animHead> AnimFindByTSOTag(uint32 tag)
{
    const uint32_t data = animData();
    if (data == 0)
        return ptr32<animHead>(nullptr);
    uint32_t start[4];
    groupStarts(data, start);
    for (int g = 0; g < 4; ++g)
    {
        const int32_t count = groupCount(data, g);
        for (int32_t i = 0; i < count; ++i)
        {
            const uint32_t entry = start[g] + static_cast<uint32_t>(i) * kStride[g];
            if (mem<uint32_t>(mem<uint32_t>(entry)) == tag)
                return ptr32<animHead>(entry);
        }
    }
    return ptr32<animHead>(nullptr);
}

} // namespace rc

using rc::fuzz::animEntry;
using rc::fuzz::animLevel;
using rc::fuzz::animTag;

// The level's animation entries and their collision objects (on the heap): whole-RAM inputs.
RC_DECOMP_FN_SETUP_HEAP(AnimLvlTerm, 0x10f128, animLevel)
RC_DECOMP_FN_SETUP_HEAP(AnimTerm, 0x10f130, animLevel)
RC_DECOMP_FN_SETUP_HEAP(AnimReinit, 0x10f4f8, animLevel)
RC_DECOMP_FN_SETUP(AnimFindByTSOTag, 0x10f960, animTag)
RC_DECOMP_FN_SETUP_HEAP(AnimSetOcolPosition, 0x1106f8, animEntry)
RC_DECOMP_FN(AnimInit, 0x110908)
RC_DECOMP_FN_SETUP_HEAP(AnimCreateOcolObj, 0x110910, animEntry)
RC_DECOMP_FN_SETUP_HEAP(AnimUpdateOcolPos16, 0x1109a8, animEntry)
RC_DECOMP_FN_SETUP_HEAP(AnimDeleteOcolObj, 0x110a08, animEntry)
RC_DECOMP_FN_SETUP(AnimSetupClass, 0x110a48, animLevel)
