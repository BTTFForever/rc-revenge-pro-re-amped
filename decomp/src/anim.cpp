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

constexpr uint32_t kAnimVars = sym::animVarG, kLevelPtr = sym::pLvlG;
constexpr uint32_t kBoneYOffset = sym::dolphinOffsetYG; // 200.0: a type 0 entry's collision sits that far above its first bone
constexpr uint32_t kBoneIds = sym::boneId1G;     // the bones of the tags 0x10000365, 364, 368, 367, 366
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

void RevSfxAnim(ptr32<animHead> pAnimHead, int32 flags); // revsfx.cpp
ptr32<sVec3> returnPoint(ptr32<sVec3> out, ptr32<void> spl, int32 curve, int32 dir, ee_float u);

namespace {

// An entry's frame counter at the end of a pass: the last frame ends the animation (or bounces it with 8, or loops it
// with 4) and marks it done (2) if it was to run once (1); a one-frame start with 0x200 resets. `clear` is the mask a
// one-frame 0x200 start applies (0xfd7f for the linear moves, 0xfd3f for the others). Returns the new flags; the counter
// is stored. The forms differ by group (the original's own code for each).
int32_t lastFrameLinear(uint32_t entry, int32_t flags, uint32_t clear)
{
    uint32_t frame = mem<uint16>(entry + 6u) + 1u;
    if (frame == mem<uint16>(entry + 4u))
    {
        if ((flags & 1) != 0)
            flags |= 2;
        if ((flags & 8) != 0)
            flags ^= 0x10;
        frame = 0;
        if ((flags & 0x14) == 0)
            flags &= static_cast<int32_t>(0xffffffbfu);
        if ((flags & 0xc) == 4)
            flags |= 0x80;
    }
    else if (frame == 1 && (flags & 0x200) != 0)
    {
        frame = 0;
        flags &= static_cast<int32_t>(clear);
    }
    mem<uint16>(entry + 6u) = static_cast<uint16>(frame);
    return flags;
}

} // namespace

// Moves the level's animated scenery on one frame. Each entry's flags: 1 done, 2 finished, 4 loop, 8 bounce, 0x10
// backwards, 0x20 with velocity, 0x40 running, 0x80 start (the collision object made), 0x200 reset. Linear moves add
// (or with 0x10 take) their step, with 0x20 a velocity growing by an acceleration; spins add their angle step; swings
// swing with a damped amplitude; bezier paths follow a spline (two points a little apart give the direction, and the
// level's per-path matrix turns the frame built from it). Hidden objects only keep their collision where it is. The
// collision calls (AnimSetOcolPosition) see the entry's flags in $s1 (HeldS1), as an unknown tag makes them use it.
void AnimUpdateAnims()
{
    GuestFrame frame(0x190);
    const uint32_t data = animData();
    if (data == 0)
        return;
    uint32_t start[4];
    groupStarts(data, start);
    const auto create = [](uint32_t entry, int32_t flags)
    {
        HeldS1 s1(static_cast<uint32_t>(flags));
        AnimCreateOcolObj(ptr32<animHead>(entry));
    };
    const auto update = [](uint32_t entry, int32_t flags)
    {
        HeldS1 s1(static_cast<uint32_t>(flags));
        AnimUpdateOcolPos16(ptr32<animHead>(entry));
    };
    const auto stop = [](uint32_t entry, int32_t flags)
    {
        HeldS1 s1(static_cast<uint32_t>(flags));
        AnimDeleteOcolObj(ptr32<animHead>(entry));
        mem<int16>(entry + 8u) = static_cast<int16>(flags);
    };

    // Linear moves (0x64 bytes): the object's position by its step (+0x58), which grows by +0x5e with 0x20.
    uint32_t entry = start[0];
    for (int32_t i = 0; i < groupCount(data, 0); ++i, entry += 0x64u)
    {
        int32_t flags = mem<int16>(entry + 8u);
        RevSfxAnim(ptr32<animHead>(entry), 1);
        if ((flags & 1) != 0)
            continue;
        const uint32_t tso = mem<uint32_t>(entry);
        if ((mem<uint32_t>(tso + 0x20u) & 0xc00000u) != 0)
        {
            if (mem<uint32_t>(entry + 0x40u) != 0)
                update(entry, flags);
            continue;
        }
        if ((flags & 0x80) != 0)
        {
            std::memcpy(guest_host(tso + 0x10u), guest_host(entry + 0x4cu), 12); // back to the start
            flags = (flags & static_cast<int32_t>(0xffffff6fu)) | 0x40;
            if ((flags & 0x20) != 0)
            {
                mem<int16>(entry + 0x58u) = 0;
                mem<int16>(entry + 0x5au) = 0;
                mem<int16>(entry + 0x5cu) = 0;
            }
            create(entry, flags);
        }
        if ((flags & 0x40) == 0)
        {
            stop(entry, flags);
            continue;
        }
        if ((flags & 0x20) != 0)
            for (uint32_t k = 0; k < 3; ++k)
                mem<uint16>(entry + 0x58u + 2u * k) = static_cast<uint16>(mem<uint16>(entry + 0x58u + 2u * k) + mem<uint16>(entry + 0x5eu + 2u * k));
        const uint32_t obj = mem<uint32_t>(entry);
        for (uint32_t k = 0; k < 3; ++k)
        {
            const uint32_t step = static_cast<uint32_t>(static_cast<int32_t>(mem<int16>(entry + 0x58u + 2u * k)));
            uint32_t &at = mem<uint32_t>(obj + 0x10u + 4u * k);
            at = (flags & 0x10) != 0 ? at - step : at + step;
        }
        flags = lastFrameLinear(entry, flags, 0xfffffd7fu);
        update(entry, flags);
        mem<int16>(entry + 8u) = static_cast<int16>(flags);
    }

    // Spins (0x50 bytes): the object's angle by its step (+0x4e).
    entry = start[1];
    for (int32_t i = 0; i < groupCount(data, 1); ++i, entry += 0x50u)
    {
        int32_t flags = mem<int16>(entry + 8u);
        RevSfxAnim(ptr32<animHead>(entry), 2);
        if ((flags & 1) != 0)
            continue;
        if ((flags & 0x80) != 0)
        {
            flags = (flags & static_cast<int32_t>(0xffffff7fu)) | 0x40;
            mem<uint16>(mem<uint32_t>(entry) + 0x36u) = mem<uint16>(entry + 0x4cu);
            create(entry, flags);
        }
        if ((flags & 0x40) == 0)
        {
            stop(entry, flags);
            continue;
        }
        const uint32_t tso = mem<uint32_t>(entry);
        mem<uint16>(tso + 0x36u) = static_cast<uint16>(mem<uint16>(tso + 0x36u) + mem<uint16>(entry + 0x4eu));
        uint32_t count = mem<uint16>(entry + 6u) + 1u;
        if (count == mem<uint16>(entry + 4u))
        {
            if ((flags & 1) != 0)
                flags |= 2;
            if ((flags & 4) == 0)
                flags &= static_cast<int32_t>(0xffffffbfu);
            count = 0;
            if ((flags & 0x200) != 0)
                flags = (flags & static_cast<int32_t>(0xffffffbfu)) | 0x80;
        }
        else if (count == 1 && (flags & 0x200) != 0)
        {
            count = 0;
            flags &= static_cast<int32_t>(0xfffffd3fu);
        }
        mem<uint16>(entry + 6u) = static_cast<uint16>(count);
        TSO_UpdateMatrix(ptr32<_sTSO>(mem<uint32_t>(entry)));
        update(entry, flags);
        mem<int16>(entry + 8u) = static_cast<int16>(flags);
    }

    // Swings (0x5c bytes): the angle (+0x52) moves by the swing's speed (+0x5a) scaled by its damping (+0x54); the
    // object's angle is that times the amplitude (+0x58), which falls by +0x56 a frame until it is spent.
    entry = start[2];
    for (int32_t i = 0; i < groupCount(data, 2); ++i, entry += 0x5cu)
    {
        int32_t flags = mem<int16>(entry + 8u);
        RevSfxAnim(ptr32<animHead>(entry), 0);
        if ((flags & 1) != 0)
            continue;
        if ((flags & 0x80) != 0)
        {
            flags = (flags & static_cast<int32_t>(0xffffff7fu)) | 0x40;
            const uint16 from = mem<uint16>(entry + 0x50u), speed = mem<uint16>(entry + 0x4cu);
            mem<uint16>(entry + 0x52u) = from;
            mem<uint16>(entry + 0x58u) = 0x8000;
            mem<uint16>(entry + 0x5au) = speed;
            create(entry, flags);
        }
        if ((flags & 0x40) == 0)
        {
            stop(entry, flags);
            continue;
        }
        const int32_t velocity = mem<int16>(entry + 0x5au);
        const int32_t magnitude = static_cast<int16_t>(velocity < 0 ? -velocity : velocity);
        const int32_t damped = static_cast<int32_t>(static_cast<uint32_t>(magnitude) * mem<uint16>(entry + 0x54u) << 16) >> 24;
        const uint16 oldVelocity = mem<uint16>(entry + 0x5au);
        if (velocity >= 0)
            mem<uint16>(entry + 0x52u) = static_cast<uint16>(damped + mem<uint16>(entry + 0x52u));
        else
            mem<uint16>(entry + 0x52u) = static_cast<uint16>(mem<uint16>(entry + 0x52u) - damped);
        const uint32_t newVelocity = oldVelocity + static_cast<uint32_t>(mem<uint16>(entry + 0x52u));
        const uint32_t amplitude = mem<uint16>(entry + 0x58u);
        const uint32_t tso = mem<uint32_t>(entry);
        mem<uint16>(entry + 0x5au) = static_cast<uint16>(newVelocity);
        mem<uint16>(tso + 0x36u) = static_cast<uint16>(static_cast<int32_t>(static_cast<int16_t>(newVelocity) * static_cast<int32_t>(amplitude)) >> 15);
        int32_t left = static_cast<int32_t>(mem<uint16>(entry + 0x58u)) - static_cast<int32_t>(mem<uint16>(entry + 0x56u));
        uint32_t count;
        if (left >= 0)
            count = mem<uint16>(entry + 6u);
        else
        {
            left = 0;
            mem<uint16>(entry + 6u) = static_cast<uint16>(mem<uint16>(entry + 4u) - 1u);
            count = mem<uint16>(entry + 6u);
        }
        ++count;
        mem<uint16>(entry + 0x58u) = static_cast<uint16>(left);
        if (count == mem<uint16>(entry + 4u))
        {
            if ((flags & 1) != 0)
                flags |= 2;
            count = 0;
            if ((flags & 4) == 0)
                flags &= static_cast<int32_t>(0xffffffbfu);
            else if (mem<uint16>(entry + 0x56u) != 0)
                flags |= 0x80;
        }
        else if (count == 1 && (flags & 0x200) != 0)
        {
            count = 0;
            flags &= static_cast<int32_t>(0xfffffd3fu);
        }
        mem<uint16>(entry + 6u) = static_cast<uint16>(count);
        TSO_UpdateMatrix(ptr32<_sTSO>(mem<uint32_t>(entry)));
        update(entry, flags);
        mem<int16>(entry + 8u) = static_cast<int16>(flags);
    }

    // Bezier paths (0x50 bytes): the spline (+0x4c, its point count first) sampled at the frame's place and a little
    // ahead; the object goes to the first point (in 1/350 units) and its matrix is the level's per-path matrix (index i)
    // times the frame the difference of the two points makes with the up axis.
    const uint32_t pos = frame.local<uint8_t>(0).address, dir = frame.local<uint8_t>(0x10).address;
    const uint32_t xAxis = frame.local<uint8_t>(0x50).address, yAxis = frame.local<uint8_t>(0x60).address, zAxis = frame.local<uint8_t>(0x70).address;
    const uint32_t vuMat = frame.local<uint8_t>(0x80).address;
    const ee_float ahead = mem<ee_float>(sym::lit4_3275d0);
    entry = start[3];
    for (int32_t i = 0; i < groupCount(data, 3); ++i, entry += 0x50u)
    {
        int32_t flags = mem<int16>(entry + 8u);
        RevSfxAnim(ptr32<animHead>(entry), 3);
        if ((flags & 1) != 0)
            continue;
        if ((flags & 0x80) != 0)
        {
            flags = (flags & static_cast<int32_t>(0xffffff6fu)) | 0x40;
            create(entry, flags);
        }
        if ((flags & 0x40) == 0)
        {
            stop(entry, flags);
            continue;
        }
        const uint32_t frameNo = mem<uint16>(entry + 6u) + 1u;
        const uint32_t spline = mem<uint32_t>(entry + 0x4cu);
        const int32_t points = mem<int32_t>(spline);
        const ee_float perFrame = ee_itof(points) * ee_float(1024.0f) / ee_itof(mem<uint16>(entry + 4u));
        const ee_float along = ee_itof(static_cast<int32_t>((flags & 0x10) != 0 ? static_cast<uint32_t>(points) - frameNo : frameNo)) * perFrame;
        int32_t curve = ee_ftoi(along);
        const ee_float fraction = ee_itof(curve & 0x3ff) * ee_float(1.0f / 1024.0f);
        curve >>= 10;
        if (static_cast<int32_t>(static_cast<uint32_t>(points) - 1u) < curve)
            curve = 0;
        returnPoint(ptr32<sVec3>(pos), ptr32<void>(spline), curve, 1, fraction + ahead);
        returnPoint(ptr32<sVec3>(dir), ptr32<void>(mem<uint32_t>(entry + 0x4cu)), curve, 1, fraction);
        const ee_float scale = 89600.0f;
        const ee_float p0 = mem<ee_float>(pos), p1 = mem<ee_float>(pos + 4u), p2 = mem<ee_float>(pos + 8u);
        const uint32_t obj = mem<uint32_t>(entry);
        mem<ee_float>(zAxis) = mem<ee_float>(dir) - p0;
        mem<int32_t>(obj + 0x18u) = ee_ftoi(p2 * scale);
        mem<ee_float>(zAxis + 4u) = mem<ee_float>(dir + 4u) - p1;
        mem<int32_t>(obj + 0x10u) = ee_ftoi(p0 * scale);
        mem<ee_float>(zAxis + 8u) = mem<ee_float>(dir + 8u) - p2;
        mem<int32_t>(obj + 0x14u) = ee_ftoi(p1 * scale);
        guest_call(sym::sceVu0Normalize, zAxis, zAxis);
        const ee_float zx = mem<ee_float>(zAxis), zz = mem<ee_float>(zAxis + 8u);
        mem<uint32_t>(xAxis + 4u) = 0;
        mem<ee_float>(xAxis) = zz;
        mem<ee_float>(xAxis + 8u) = -zx;
        guest_call(sym::sceVu0Normalize, xAxis, xAxis);
        guest_call(sym::sceVu0OuterProduct, yAxis, xAxis, zAxis);
        // The three axes as the rows' first three lanes (their w lanes and the fourth row keep what was there).
        const uint32_t pathMatrix = mem<uint32_t>(mem<uint32_t>(sym::pLvlG) + 0x5cu) + static_cast<uint32_t>(i) * 0x40u;
        for (uint32_t r = 0; r < 3; ++r)
        {
            mem<ee_float>(vuMat + 0x10u * r) = mem<ee_float>(xAxis + 4u * r);
            mem<ee_float>(vuMat + 0x10u * r + 4u) = mem<ee_float>(yAxis + 4u * r);
            mem<ee_float>(vuMat + 0x10u * r + 8u) = mem<ee_float>(zAxis + 4u * r);
        }
        guest_call(sym::sceVu0MulMatrix, vuMat, pathMatrix, vuMat);
        const uint32_t target = mem<uint32_t>(entry);
        static const uint32_t kPlace[9] = {0x24u, 0x2au, 0x30u, 0x26u, 0x2cu, 0x32u, 0x28u, 0x2eu, 0x34u}; // row r, lane l -> matrix
        for (uint32_t r = 0; r < 3; ++r)
            for (uint32_t l = 0; l < 3; ++l)
                mem<int16>(target + kPlace[r * 3u + l]) = static_cast<int16>(ee_ftoi(mem<ee_float>(vuMat + 0x10u * r + 4u * l) * ee_float(4096.0f)));
        flags = lastFrameLinear(entry, flags, 0xfffffd3fu);
        update(entry, flags);
        mem<int16>(entry + 8u) = static_cast<int16>(flags);
    }
}

} // namespace rc

using rc::fuzz::animEntry;
using rc::fuzz::animLevel;
using rc::fuzz::animTag;

// The level's animation entries and their collision objects (on the heap): whole-RAM inputs.
RC_DECOMP_FN_SETUP_HEAP(AnimLvlTerm, animLevel)
RC_DECOMP_FN_SETUP_HEAP(AnimTerm, animLevel)
RC_DECOMP_FN_SETUP_HEAP(AnimReinit, animLevel)
RC_DECOMP_FN_SETUP(AnimFindByTSOTag, animTag)
RC_DECOMP_FN_SETUP_HEAP(AnimSetOcolPosition, animEntry)
RC_DECOMP_FN(AnimInit)
RC_DECOMP_FN_SETUP_HEAP(AnimCreateOcolObj, animEntry)
RC_DECOMP_FN_SETUP_HEAP(AnimUpdateOcolPos16, animEntry)
RC_DECOMP_FN_SETUP_HEAP(AnimDeleteOcolObj, animEntry)
RC_DECOMP_FN_SETUP(AnimSetupClass, animLevel)
// The level's animations moved on, their sounds (RevSfxAnim, with the IOP's answers scripted offline) and collision.
RC_DECOMP_FN_SETUP_HEAP(AnimUpdateAnims, rc::fuzz::animUpdate)
