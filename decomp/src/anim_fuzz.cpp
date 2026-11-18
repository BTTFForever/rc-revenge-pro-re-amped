// Builds the random-test inputs of ANIM.c and TSO.c. See anim_fuzz.h.

#include "anim_fuzz.h"
#include "sfx_fuzz.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace rc::fuzz {

namespace {

constexpr uint32_t kAnimVars = sym::animVarG, kTsoVars = sym::tsoVarG, kRdrCamera = sym::rdrCameraG;
constexpr uint32_t kStride[4] = {0x64u, 0x50u, 0x5cu, 0x50u};
constexpr uint32_t kBoneTags[5] = {0x10000365u, 0x10000364u, 0x10000368u, 0x10000367u, 0x10000366u};

template <typename T>
T &mem(uint32_t address) { return *static_cast<T *>(guest_host(address)); }

uint32_t rnd(uint64_t &state, uint32_t n) { return n == 0 ? 0u : nextFuzz(state) % n; }
bool chance(uint64_t &state, uint32_t percent) { return rnd(state, 100) < percent; }
float frnd(uint64_t &state, float scale) { return (static_cast<float>(static_cast<int32_t>(nextFuzz(state) % 20001u) - 10000) / 10000.0f) * scale; }
void setArg(R5900Context *ctx, int index, uint32_t value) { SET_GPR_S32(ctx, 4 + index, static_cast<int32_t>(value)); }
bool inRam(uint32_t address) { return address >= 0x100000u && address < 0x2000000u && (address & 3u) == 0; }

struct Entry
{
    uint32_t address;
    int group;
};

std::vector<Entry> entries()
{
    std::vector<Entry> list;
    const uint32_t data = mem<uint32_t>(kAnimVars);
    if (!inRam(data))
        return list;
    uint32_t at = data + 8u;
    for (int g = 0; g < 4; ++g)
    {
        const int32_t count = mem<int16_t>(data + 2u * static_cast<uint32_t>(g));
        if (count < 0 || count > 200)
            return {};
        for (int32_t i = 0; i < count; ++i, at += kStride[g])
            list.push_back({at, g});
    }
    return list;
}

// The model chain AnimSetOcolPosition follows for types 0 and 4: the TSO's model, its skeleton and its bones 0-7.
bool bonesSound(uint32_t entry)
{
    const uint32_t tso = mem<uint32_t>(entry);
    if (!inRam(tso))
        return false;
    const uint32_t model = mem<uint32_t>(tso + 0xcu);
    if (!inRam(model) || !inRam(mem<uint32_t>(model + 0x3cu)))
        return false;
    const uint32_t bones = mem<uint32_t>(mem<uint32_t>(model + 0x3cu) + 8u);
    if (!inRam(bones))
        return false;
    for (uint32_t i = 0; i < 8; ++i)
        if (!inRam(mem<uint32_t>(bones + i * 4u)))
            return false;
    return true;
}

// One entry varied (its object's animation kind, 0x40 or 0x80, is left alone: it says what the object points at): its flags, frame counters, offset and type, its collision object dropped now and then (so that a
// new one is made), its tag made one of the bone tags.
void varyEntry(uint64_t &state, const Entry &e)
{
    const uint32_t a = e.address;
    if (chance(state, 50))
        mem<uint16_t>(a + 8u) = static_cast<uint16_t>(nextFuzz(state));
    if (chance(state, 30))
        mem<uint16_t>(a + 6u) = static_cast<uint16_t>(rnd(state, 0x200));
    if (chance(state, 20))
        mem<uint16_t>(a + 4u) = chance(state, 50) ? 0 : static_cast<uint16_t>(rnd(state, 0x200));
    if (chance(state, 40))
        for (uint32_t i = 0; i < 3; ++i)
            mem<float>(a + 0x2cu + i * 4u) = frnd(state, 2000.0f);
    if (chance(state, 25))
        mem<uint32_t>(a + 0x40u) = 0;
    const bool sound = bonesSound(a);
    if (chance(state, 30))
    {
        static const int16_t kTypes[] = {1, 2, 3, 5, 6, -1, 0, 4};
        int16_t type = kTypes[rnd(state, 8)];
        if ((type == 0 || type == 4) && !sound)
            type = 1;
        mem<int16_t>(a + 0xau) = type;
    }
    const uint32_t tso = mem<uint32_t>(a);
    if (sound && inRam(tso) && chance(state, 40))
        mem<uint32_t>(tso) = chance(state, 85) ? kBoneTags[rnd(state, 5)] : 0x10000300u + rnd(state, 0x100);
    if (inRam(tso) && chance(state, 30))
        mem<uint16_t>(tso + 0x22u) = static_cast<uint16_t>(mem<uint16_t>(tso + 0x22u) ^ (rnd(state, 2) ? 4u << rnd(state, 2) : 0x0cu));
    if (inRam(tso) && chance(state, 30))
        for (uint32_t i = 0; i < 3; ++i)
            mem<int32_t>(tso + 0x10u + i * 4u) = static_cast<int32_t>(nextFuzz(state)) >> rnd(state, 8);
}

// $s1 as a caller may leave it: a bone number (AnimSetOcolPosition reads it for an unknown tag).
void staleBone(R5900Context *ctx, uint64_t &state) { SET_GPR_S32(ctx, 17, static_cast<int32_t>(rnd(state, 8))); }

} // namespace

void animLevel(R5900Context *ctx, uint64_t &state, uint32_t)
{
    const std::vector<Entry> list = entries();
    for (const Entry &e : list)
        if (chance(state, 30))
            varyEntry(state, e);
    // Now and then every linear entry without its collision object (as after AnimTerm) and of type 4 with a tag that
    // has no bone of its own, so that AnimReinit's caller-$s1 bone is used.
    if (chance(state, 30))
        for (const Entry &e : list)
            if (e.group == 0 && bonesSound(e.address))
            {
                mem<uint32_t>(e.address + 0x40u) = 0;
                mem<int16_t>(e.address + 0xau) = 4;
                mem<uint32_t>(mem<uint32_t>(e.address)) = 0x10000300u + rnd(state, 0x60);
            }
    staleBone(ctx, state);
}

void animEntry(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    const std::vector<Entry> list = entries();
    staleBone(ctx, state);
    if (list.empty())
    {
        setArg(ctx, 0, scratch); // a zeroed entry: no collision, type 0 with no model is not made
        std::memset(guest_host(scratch), 0, 0x64);
        mem<int16_t>(scratch + 0xau) = 1;
        return;
    }
    const Entry &e = list[rnd(state, static_cast<uint32_t>(list.size()))];
    varyEntry(state, e);
    setArg(ctx, 0, e.address);
}

void animTag(R5900Context *ctx, uint64_t &state, uint32_t)
{
    const std::vector<Entry> list = entries();
    if (list.empty() || chance(state, 20))
    {
        setArg(ctx, 0, chance(state, 50) ? nextFuzz(state) : 0x10000000u + rnd(state, 0x800));
        return;
    }
    const Entry &e = list[rnd(state, static_cast<uint32_t>(list.size()))];
    setArg(ctx, 0, mem<uint32_t>(mem<uint32_t>(e.address)));
}

namespace {

uint32_t tsoCount()
{
    const uint32_t header = mem<uint32_t>(kTsoVars);
    return inRam(header) ? std::min<uint32_t>(mem<uint32_t>(header), 400u) : 0u;
}
uint32_t tsoAt(uint32_t i) { return mem<uint32_t>(kTsoVars) + 4u + i * 0x3cu; }

// An object's flags and angles varied; the animation kind (0x40, 0x80) is left as the level has it, since it says what
// its +0xc points at.
void varyTso(uint64_t &state, uint32_t tso)
{
    if (chance(state, 50))
        mem<uint16_t>(tso + 0x22u) = static_cast<uint16_t>((mem<uint16_t>(tso + 0x22u) & 0xc0u) | (nextFuzz(state) & 0xff3fu));
    if (chance(state, 50))
        for (uint32_t i = 0; i < 4; ++i)
            mem<int16_t>(tso + 0x1cu + i * 2u) = static_cast<int16_t>(nextFuzz(state));
    if (chance(state, 50))
        mem<int16_t>(tso + 0x36u) = static_cast<int16_t>(nextFuzz(state));
    if (chance(state, 30))
        for (uint32_t i = 0; i < 9; ++i)
            mem<int16_t>(tso + 0x24u + i * 2u) = static_cast<int16_t>(static_cast<int32_t>(rnd(state, 0x2001)) - 0x1000);
    if (chance(state, 30))
        for (uint32_t i = 0; i < 3; ++i)
            mem<int32_t>(tso + 0x10u + i * 4u) = static_cast<int32_t>(nextFuzz(state)) >> rnd(state, 8);
}

} // namespace

void tsoLevel(R5900Context *, uint64_t &state, uint32_t)
{
    const uint32_t n = tsoCount();
    for (uint32_t i = 0; i < n; ++i)
        if (chance(state, 20))
        {
            const uint32_t tso = tsoAt(i);
            const uint16_t kind = mem<uint16_t>(tso + 0x22u) & 0xc0u;
            if (chance(state, 50))
                mem<uint16_t>(tso + 0x22u) = static_cast<uint16_t>(kind | (nextFuzz(state) & 0xff3fu));
            if (chance(state, 50))
                for (uint32_t k = 0; k < 9; ++k)
                    mem<int16_t>(tso + 0x24u + k * 2u) = static_cast<int16_t>(static_cast<int32_t>(rnd(state, 0x2001)) - 0x1000);
            if (chance(state, 50))
                for (uint32_t k = 0; k < 3; ++k)
                    mem<int32_t>(tso + 0x10u + k * 4u) = static_cast<int32_t>(nextFuzz(state)) >> rnd(state, 8);
            if (chance(state, 5))
                mem<uint32_t>(tso + 0xcu) = 0;
        }
    if (n && chance(state, 5))
        mem<uint32_t>(mem<uint32_t>(kTsoVars)) = 0;
}

void tsoObject(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    const uint32_t n = tsoCount();
    uint32_t tso = scratch;
    if (n && chance(state, 90))
        tso = tsoAt(rnd(state, n));
    varyTso(state, tso);
    setArg(ctx, 0, tso);
}

void tsoCallback(R5900Context *ctx, uint64_t &state, uint32_t)
{
    const uint32_t n = tsoCount();
    for (uint32_t i = 0; i < n; ++i)
        if (chance(state, 10))
            mem<uint32_t>(tsoAt(i)) = chance(state, 50) ? 0u : nextFuzz(state);
    setArg(ctx, 0, sym::TSOSetSFXCallBack); // TSOSetSFXCallBack: TSOCallBack ends as the last object's instance
}

// --- The animations moved on and the scenery drawn, with its sounds (RevSfxAnim, RevSfxTSO with the IOP scripted). ---

// In the track editor (mainVarG.level 0x63) no race runs: the scenery and animation pointers are a freed level's.
bool inTrackEditor() { return mem<uint8_t>(sym::mainVarG + 0x34au) == 0x63u; }

void animUpdate(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    soundScripted(state);
    if (inTrackEditor())
        mem<uint32_t>(kAnimVars) = 0;
    animLevel(ctx, state, scratch);
    // Counters near their ends, the states that start, reset or loop. (An object's word at +0x20 holds its animation
    // kind, 0x40 or 0x80 of the flags at +0x22, in bits 0xc00000: left alone, it says what the object points at.)
    for (const Entry &e : entries())
    {
        if (chance(state, 20))
            mem<uint16_t>(e.address + 6u) = static_cast<uint16_t>(mem<uint16_t>(e.address + 4u) - rnd(state, 2));
        if (e.group == 3)
        {
            // A path's frame count is never 0 here (AnimReinit takes it from the spline) and its counter stays below it:
            // past it, running backwards, the spline would be read from before its start, beyond RAM.
            if (mem<uint16_t>(e.address + 4u) == 0)
                mem<uint16_t>(e.address + 4u) = static_cast<uint16_t>(1u + rnd(state, 0x100));
            if (mem<uint16_t>(e.address + 6u) >= mem<uint16_t>(e.address + 4u))
                mem<uint16_t>(e.address + 6u) = static_cast<uint16_t>(rnd(state, mem<uint16_t>(e.address + 4u)));
        }
        if (chance(state, 15))
        {
            static const uint16_t kFlags[] = {0x40, 0x80, 0x240, 0x244, 0x248, 0x250, 0x260, 0x264, 0x41, 0x2c0, 0x58, 0x4c, 0x0};
            mem<uint16_t>(e.address + 8u) = kFlags[rnd(state, 13)];
        }
    }
}

namespace {

// The sound fields a scenery object's sound pair uses: a sound (an ambient sound's id, now and then with 0x200 or 0x400
// or none), a frame and a state (0, a frame, or one of the state bits with a frame).
int32_t sceneSound(uint64_t &state)
{
    if (chance(state, 15))
        return -1;
    const int32_t count = std::max(1, ambientCount());
    return static_cast<int32_t>(rnd(state, static_cast<uint32_t>(std::min(count, 0x7f))));
}

uint16_t sceneState(uint64_t &state)
{
    static const uint16_t kBits[] = {0, 0, 0, 0x200, 0x400, 0x800, 0x4000};
    const uint16_t bits = kBits[rnd(state, 7)];
    return static_cast<uint16_t>(bits | (chance(state, 50) ? 0 : rnd(state, 0x40)));
}

void callBackIsTheGames() { mem<uint32_t>(sym::TSOCallBack) = sym::RevSfxTSO; }

} // namespace

// a0 a boned object's instance in scratch (its control's frame near the sounds' frames), a1 a place near the camera.
void tsoSfxBoned(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    soundScripted(state);
    callBackIsTheGames();
    const uint32_t ref = scratch + 0x100u, ctrl = scratch + 0x200u, pos = scratch + 0x300u;
    for (uint32_t i = 0; i < 0x50u; i += 4)
        mem<uint32_t>(ref + i) = nextFuzz(state);
    mem<uint32_t>(ref + 0x38u) = ctrl;
    mem<int32_t>(ctrl + 0x18u) = static_cast<int32_t>(chance(state, 90) ? rnd(state, 0x60) : nextFuzz(state));
    for (const uint32_t at : {0x44u, 0x48u})
    {
        mem<int8_t>(ref + at) = static_cast<int8_t>(sceneSound(state));
        mem<uint8_t>(ref + at + 1u) = static_cast<uint8_t>(chance(state, 80) ? rnd(state, 0x60) : nextFuzz(state));
        mem<uint16_t>(ref + at + 2u) = sceneState(state);
    }
    if (chance(state, 25))
        mem<uint16_t>(ref + 0x4au) = 0x4000;
    for (uint32_t axis = 0; axis < 3; ++axis)
        mem<int32_t>(pos + 4u * axis) = static_cast<int32_t>(nextFuzz(state)) >> rnd(state, 12);
    setArg(ctx, 0, ref);
    setArg(ctx, 1, pos);
}

// a0 a vertex animation's instance in scratch (its shape running forwards or backwards, its float frame near the
// sounds' frames, now and then 0), a1 a place.
void tsoSfxVert(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    soundScripted(state);
    callBackIsTheGames();
    const uint32_t ref = scratch + 0x100u, shape = scratch + 0x200u, pos = scratch + 0x300u;
    for (uint32_t i = 0; i < 0x20u; i += 4)
        mem<uint32_t>(ref + i) = nextFuzz(state);
    mem<uint32_t>(ref) = shape;
    mem<uint32_t>(shape + 0x7cu) = nextFuzz(state);
    mem<float>(ref + 8u) = chance(state, 10) ? 0.0f : chance(state, 85) ? frnd(state, 50.0f) + 50.0f : frnd(state, 1e6f);
    for (const uint32_t at : {0x18u, 0x1cu})
    {
        mem<int8_t>(ref + at) = static_cast<int8_t>(sceneSound(state));
        mem<int8_t>(ref + at + 1u) = static_cast<int8_t>(chance(state, 80) ? rnd(state, 0x60) : nextFuzz(state));
        mem<uint16_t>(ref + at + 2u) = static_cast<uint16_t>(chance(state, 50) ? 0 : chance(state, 80) ? rnd(state, 0x100) : nextFuzz(state));
    }
    for (uint32_t axis = 0; axis < 3; ++axis)
        mem<int32_t>(pos + 4u * axis) = static_cast<int32_t>(nextFuzz(state)) >> rnd(state, 12);
    setArg(ctx, 0, ref);
    setArg(ctx, 1, pos);
}

// a0 a time (now or a little later); the level's objects varied: matrices rebuilt or not, hidden, skipped
// (0x20), and their animations' frames, directions, ends and clocks.
void tsoRender(R5900Context *ctx, uint64_t &state, uint32_t)
{
    soundScripted(state);
    if (inTrackEditor())
        mem<uint32_t>(kTsoVars) = 0;
    callBackIsTheGames();
    const uint32_t n = tsoCount();
    const uint32_t time = mem<uint32_t>(sym::mainVarG + 0x1cu);
    for (uint32_t i = 0; i < n; ++i)
    {
        const uint32_t tso = tsoAt(i);
        const uint16_t kind = mem<uint16_t>(tso + 0x22u) & 0x2c0u;
        if (chance(state, 25))
            mem<uint16_t>(tso + 0x22u) = static_cast<uint16_t>(kind | (nextFuzz(state) & 0x3fu & ~(chance(state, 80) ? 0x20u : 0u)));
        const uint32_t ref = mem<uint32_t>(tso + 0xcu);
        if (!inRam(ref) || !chance(state, 40))
            continue;
        if ((kind & 0x40u) != 0 && inRam(mem<uint32_t>(ref)))
        {
            const uint32_t shape = mem<uint32_t>(ref);
            // Within the key frames as the game keeps it (the blend reads the key after the frame's): from 0 to two
            // before the last, often at the ends, and steps of a few frames at most.
            const uint32_t frames = mem<uint32_t>(shape + 0x74u);
            const float span = frames > 2u ? static_cast<float>(frames - 2u) : 0.0f;
            const uint32_t where = rnd(state, 4);
            mem<float>(shape + 0x78u) = where == 0 ? 0.0f : where == 1 ? span * 0.999f : (static_cast<float>(rnd(state, 10000)) / 10000.0f) * span;
            static const uint32_t kBits[] = {0x40, 0x50, 0x44, 0x54, 0x48, 0x4c, 0x5c, 0x440, 0xc0, 0x0};
            mem<uint32_t>(shape + 0x7cu) = (mem<uint32_t>(shape + 0x7cu) & ~0x4dcu) | kBits[rnd(state, 10)];
            mem<uint32_t>(ref + 0x10u) = time - rnd(state, chance(state, 80) ? 0x40 : 0x400);
            if (chance(state, 20))
                mem<uint32_t>(ref + 0xcu) = 16u + rnd(state, 0x40);
        }
        else if ((kind & 0x80u) != 0 && inRam(mem<uint32_t>(ref + 0x38u)))
        {
            const uint32_t ctrl = mem<uint32_t>(ref + 0x38u);
            static const uint32_t kBits[] = {0x40, 0x50, 0x44, 0x54, 0x48, 0x4c, 0x5c, 0xc0, 0x0};
            mem<uint32_t>(ref + 0x40u) = (mem<uint32_t>(ref + 0x40u) & ~0xdcu) | kBits[rnd(state, 9)];
            mem<float>(ref + 0x30u) = frnd(state, 40.0f) + 20.0f;
            mem<uint32_t>(ctrl + 4u) = chance(state, 20) ? time : time - rnd(state, chance(state, 80) ? 0x40 : 0x400);
            if (chance(state, 30))
                mem<uint32_t>(ctrl + 0x18u) = rnd(state, 0x40);
        }
    }
    setArg(ctx, 0, time + rnd(state, 0x40));
}

void tsoViewCone(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    const uint32_t cam = kRdrCamera;
    const float scale = chance(state, 50) ? 500.0f : 20000.0f;
    for (uint32_t i = 0; i < 3; ++i)
        mem<float>(scratch + i * 4u) = mem<float>(cam + 0xbcu + i * 4u) + frnd(state, scale);
    setArg(ctx, 0, scratch);
    ctx->f[12] = chance(state, 10) ? 0.0f : std::fabs(frnd(state, chance(state, 50) ? 100.0f : 3000.0f));
}

} // namespace rc::fuzz
