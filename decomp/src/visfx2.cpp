// Decompiled from VISFX2.c and VISFX3.c: the handlers of the scenery effects (fountains, steam, bats, tumbleweed...)
// and the oil effects, the finish-line text, and the rain and glint tables.
//
// A scenery effect is a `_sVfx_TSFX`: like `_sVfx_Object` but following a scenery object instance (a `_sTOBInst`, at
// +0x10), with its own data from +0x40: a ring of particles whose header is four bytes (mask, count, head, tail).
// Handlers move the particles, drop dead ones by moving the head into their place, start new ones at the tail, and
// finally sort the effect at its scenery object's position. Ring indexes are signed bytes where the original loads
// them with lb, which only matters for a ring of 128 or more.

#include "ai_fuzz.h"
#include "ee_math.h"
#include "vifpk.h"
#include "visfx.h"

namespace rc {

using vfx::at;
using vfx::sortPosFromCar;
using vfx::sortPosFromInts;
using vfx::ringAtTail;
using vfx::ringHead;
using vfx::ringNext;
using vfx::ringPush;
using vfx::sortAtScenery;

namespace {

constexpr uint32_t kLifeSpan = 0x10;
constexpr uint32_t kCar = 0x40;
constexpr uint32_t kRing = 0x40; // the ring header: mask, count, head, tail
constexpr uint32_t kParticles = 0x44;

// Removes the particle at `index` by moving the head particle into its place, then moves the head on.
void ringRemove(uint32_t mem, int32_t index, uint32_t stride)
{
    const uint8_t head = at<uint8_t>(mem + kRing + 2);
    if (static_cast<int32_t>(index) != static_cast<int32_t>(head))
    {
        uint8_t tmp[0x18];
        std::memcpy(tmp, &at<uint8_t>(mem + kParticles + head * stride), stride);
        std::memcpy(&at<uint8_t>(mem + kParticles + static_cast<uint32_t>(index) * stride), tmp, stride);
    }
    at<uint8_t>(mem + kRing + 1) = static_cast<uint8_t>(at<uint8_t>(mem + kRing + 1) - 1);
    at<uint8_t>(mem + kRing + 2) = static_cast<uint8_t>(at<uint8_t>(mem + kRing) & (at<uint8_t>(mem + kRing + 2) + 1));
}

// The fountain's and Moby Dick's spout: particles of 0x18 bytes (position, velocity, a 32-bit life, a spin angle and
// spin). Each frame a particle moves by its velocity (which falls by `gravity`), spins, and loses a life; at the end of its
// life the head particle replaces it.
void spoutParticles(uint32_t mem, int16_t gravity)
{
    int32_t index = ringHead(mem);
    if (ringAtTail(mem, index))
        return;
    do
    {
        const uint32_t p = mem + kParticles + static_cast<uint32_t>(index) * 0x18u;
        const uint16_t velY = at<uint16_t>(p + 0xa);
        const uint16_t y = static_cast<uint16_t>(at<uint16_t>(p + 2) + velY);
        const uint16_t x = static_cast<uint16_t>(at<uint16_t>(p) + at<uint16_t>(p + 8));
        const uint16_t z = static_cast<uint16_t>(at<uint16_t>(p + 4) + at<uint16_t>(p + 0xc));
        const uint16_t angle = static_cast<uint16_t>(at<uint16_t>(p + 0x14) + at<uint16_t>(p + 0x16));
        const int32_t life = at<int32_t>(p + 0x10) - 1;
        at<uint16_t>(p) = x;
        at<uint16_t>(p + 2) = y;
        at<uint16_t>(p + 4) = z;
        at<uint16_t>(p + 0xa) = static_cast<uint16_t>(velY + gravity);
        at<uint16_t>(p + 0x14) = angle;
        at<int32_t>(p + 0x10) = life;
        if (life == 0)
            ringRemove(mem, index, 0x18);
        index = ringNext(mem, index);
    } while (!ringAtTail(mem, index));
}

// Starts a spout particle at the scenery object's position, with a random velocity. The velocity is around the
// object's up vector (the second row of its matrix), `base` is added to the vertical part.
void spoutSpawn(uint32_t mem, int32_t baseY)
{
    const uint32_t tob = at<uint32_t>(mem + 0x10) + 0x18; // the instance's matrix
    const uint32_t p = mem + kParticles + static_cast<uint32_t>(static_cast<int8_t>(at<uint8_t>(mem + kRing + 3))) * 0x18u;
    at<uint16_t>(p) = 0;
    at<uint16_t>(p + 2) = 0;
    at<uint16_t>(p + 4) = 0;
    int32 r = rand();
    at<uint16_t>(p + 8) = static_cast<uint16_t>(((r >> 15) & 0x1f) + (static_cast<int16_t>(at<uint16_t>(tob + 2)) >> 6) - 0x10);
    r = rand();
    at<uint16_t>(p + 0xa) = static_cast<uint16_t>(((r >> 15) & 0x1f) + (static_cast<int16_t>(at<uint16_t>(tob + 8)) >> 6) + baseY);
    r = rand();
    at<int32_t>(p + 0x10) = 0x3e;
    at<uint16_t>(p + 0xc) = static_cast<uint16_t>(((r >> 15) & 0x1f) + (static_cast<int16_t>(at<uint16_t>(tob + 0xe)) >> 6) - 0x10);
    r = rand();
    at<uint16_t>(p + 0x14) = static_cast<uint16_t>(r >> 15);
    r = rand();
    at<uint16_t>(p + 0x16) = static_cast<uint16_t>(((r >> 15) & 0xff) - 0x80);
    ringPush(mem);
}

} // namespace

// The effects that need no code.
void Vfx3PolyWheel(int32, ptr32<MATRIX>, ptr32<_sObject>, uint32) {}
void Vfx2Renderer_Bats(ptr32<_sVfx_TSFX>) {}

// Both of these end in a jump to VfxCarSortPos / VfxSortPos, so the sort position's stack word is in a frame of 0x20
// (plus the caller's own where it has one).
void Vfx2Handler_RouteFinder(ptr32<_sVfx_Object> mem)
{
    if (G(frontEndVarG).gameStateData.gameConfig.trackDirHints != 0)
        sortPosFromCar(mem.address, ptr32<_sObject>(at<uint32_t>(mem.address + kCar)), 0x20);
}

// The UFO's burner: its counter advances by 8, it ends when its car's render object is fully bright, and it is sorted at
// the car.
void Vfx2Handler_UFOBurner(ptr32<_sVfx_Object> mem)
{
    const uint32_t m = mem.address;
    const uint32_t car = at<uint32_t>(m + 0x650);
    const uint32_t render = at<uint32_t>(car + 0x18);
    at<int32_t>(m + 0x654) = at<int32_t>(m + 0x654) + 8;
    const uint32_t head = at<uint32_t>(render);
    if (at<uint32_t>(head + 0x64) == 0xFF000000u)
        at<uint16_t>(m + kLifeSpan) = 0;
    sortPosFromCar(m, ptr32<_sObject>(car), 0x20);
}

void VfxSortPosTS(ptr32<SVECTOR> sv, ptr32<_sVfx_TSFX> fx)
{
    sortPosFromInts(fx.address, sv->vx, sv->vy, sv->vz, 0x20);
}

void VfxVecSortPosTS(ptr32<vec32> v, ptr32<_sVfx_TSFX> fx)
{
    sortPosFromInts(fx.address, v->x >> 8, v->y >> 8, v->z >> 8, 0x20);
}

void Vfx2Handler_Lights(ptr32<_sVfx_TSFX> mem)
{
    sortAtScenery(mem.address, 0x20);
}

// Oil on a barge or car: a pair of counters runs up and down. At +0x44 an angle advances by 0x37; at +0x46 a level moves by
// the step at +0x48, which turns to -1 above 0x4b and to +1 below 0x42 (a 32-bit store: it also clears the next halfword).
void VfxHandler_BargeOil(ptr32<_sVfx_Object> mem)
{
    const uint32_t m = mem.address;
    const int16_t level = static_cast<int16_t>(at<uint16_t>(m + 0x46));
    at<uint16_t>(m + 0x44) = static_cast<uint16_t>(at<uint16_t>(m + 0x44) + 0x37);
    if (level < 0x41)
        at<uint16_t>(m + 0x46) = static_cast<uint16_t>(level + 5);
    else
    {
        at<uint16_t>(m + 0x46) = static_cast<uint16_t>(level + at<uint16_t>(m + 0x48));
        if (static_cast<int16_t>(at<uint16_t>(m + 0x46)) >= 0x4c)
            at<int32_t>(m + 0x48) = -1;
        if (static_cast<int16_t>(at<uint16_t>(m + 0x46)) < 0x42)
            at<int32_t>(m + 0x48) = 1;
    }
    sortPosFromInts(m, at<int16_t>(m + 0x14), at<int16_t>(m + 0x16), at<int16_t>(m + 0x18), 0x40); // VfxSortPos on +0x14
    at<uint16_t>(m + kLifeSpan) = static_cast<uint16_t>(at<uint16_t>(m + kLifeSpan) - 1);
}

// Oil on a car: the same counters (always running), and the car's aerial effect is made to twang.
void VfxHandler_OilCovered(ptr32<_sVfx_Object> mem)
{
    const uint32_t m = mem.address;
    const uint16_t level = at<uint16_t>(m + 0x46);
    const uint16_t step = at<uint16_t>(m + 0x48);
    const uint32_t car = at<uint32_t>(m + kCar);
    at<uint16_t>(m + 0x44) = static_cast<uint16_t>(at<uint16_t>(m + 0x44) + 0x37);
    at<uint16_t>(m + 0x46) = static_cast<uint16_t>(level + step);
    if (static_cast<int16_t>(static_cast<uint16_t>(level + step)) >= 0x4c)
        at<int32_t>(m + 0x48) = -1;
    if (static_cast<int16_t>(at<uint16_t>(m + 0x46)) < 0x42)
        at<int32_t>(m + 0x48) = 1;
    at<int32_t>(at<uint32_t>(car + 0x40) + 0xb4) = 300;
    sortPosFromCar(m, ptr32<_sObject>(car), 0x40);
    at<uint16_t>(m + kLifeSpan) = static_cast<uint16_t>(at<uint16_t>(m + kLifeSpan) - 1);
}

// The tumbleweed: position (+0x40 x, +0x42 y, +0x44 z) moves by its velocity (+0x48, +0x4a, +0x4c); gravity pulls the
// vertical velocity down 2 a frame; when it falls below the ground it is put back at 0 with a new random bounce and
// its spin (+0x52) restarts.
void Vfx2Handler_TumbleWeed(ptr32<_sVfx_TSFX> mem)
{
    const uint32_t m = mem.address;
    const uint16_t velY = at<uint16_t>(m + 0x4a), y = at<uint16_t>(m + 0x42), x = at<uint16_t>(m + 0x40), z = at<uint16_t>(m + 0x44);
    const uint16_t a = at<uint16_t>(m + 0x50), b = at<uint16_t>(m + 0x52), velX = at<uint16_t>(m + 0x48), velZ = at<uint16_t>(m + 0x4c);
    const uint16_t newY = static_cast<uint16_t>(y + velY);
    at<uint16_t>(m + 0x4a) = static_cast<uint16_t>(velY - 2);
    at<uint16_t>(m + 0x40) = static_cast<uint16_t>(x + velX);
    at<uint16_t>(m + 0x44) = static_cast<uint16_t>(z + velZ);
    at<uint16_t>(m + 0x50) = static_cast<uint16_t>(a - b);
    at<uint16_t>(m + 0x42) = newY;
    if (static_cast<int16_t>(newY) < 0)
    {
        at<uint16_t>(m + 0x42) = 0;
        const int32 r = rand();
        at<uint16_t>(m + 0x52) = 0xf0;
        at<uint16_t>(m + 0x4a) = static_cast<uint16_t>(((r >> 15) & 0x1f) + 0x1e);
    }
    else
    {
        const uint16_t shrunk = static_cast<uint16_t>(b - 4);
        at<uint16_t>(m + 0x52) = shrunk;
        if (static_cast<int16_t>(shrunk) < 10)
            at<uint16_t>(m + 0x52) = 10;
    }
}

// The text a finished race shows: by finishing position, or the time trial's record message.
void Vfx2FinishText(int16 pos, ptr32<char> t)
{
    struct Message
    {
        uint32_t address;
        uint32_t length; // bytes copied, including the terminator and the padding after it
    };
    static constexpr Message kExcellent{0x3245b0u, 11}, kCongratulations{0x3245c0u, 17}, kWellDone{0x3245d8u, 11},
        kRecord{0x3245e8u, 19}, kBetterLuck{0x324600u, 23};
    int32_t place = 0x15;
    if (G(mainVarG).timeTrialBeaten == 0)
        place = pos;
    const int32_t index = place - 1;
    const Message &message = index == 1 ? kCongratulations
                           : index == 0 ? kExcellent
                           : index == 2 ? kWellDone
                           : index == 0x14 ? kRecord
                                           : kBetterLuck;
    uint8_t copy[24];
    std::memcpy(copy, guest_host(message.address), message.length);
    std::memcpy(t.get(), copy, message.length);
}

// The gargoyle's dribble: a ring of 0x14-byte particles started while the scenery object's animation frame is in
// the range 0x11 to 0x27, moved with gravity of 3.
void Vfx2Handler_Gargoyle(ptr32<_sVfx_TSFX> mem)
{
    const uint32_t m = mem.address;
    const int16_t tick = static_cast<int16_t>(at<uint16_t>(m + 0x184) + 1);
    const uint32_t frameWord = at<uint32_t>(at<uint32_t>(at<uint32_t>(at<uint32_t>(m + 0x10)) + 0x38) + 0x18);
    at<uint16_t>(m + 0x184) = static_cast<uint16_t>(tick);
    if (tick >= 0x34)
        at<uint16_t>(m + 0x184) = 0;
    if (frameWord - 0x11u < 0x17u && at<uint8_t>(m + kRing + 1) < 0xf)
    {
        const uint32_t p = m + kParticles + static_cast<uint32_t>(static_cast<int8_t>(at<uint8_t>(m + kRing + 3))) * 0x14u;
        at<uint16_t>(p) = 0;
        at<uint16_t>(p + 2) = 0;
        at<uint16_t>(p + 4) = 0;
        int32 r = rand();
        at<uint16_t>(p + 8) = static_cast<uint16_t>(((r >> 15) & 0xf) + 0x20);
        r = rand();
        at<uint16_t>(p + 0xa) = static_cast<uint16_t>(((r >> 15) & 0xf) - 0x20);
        r = rand();
        at<uint16_t>(p + 0x10) = 0x28;
        at<uint16_t>(p + 0xc) = static_cast<uint16_t>(((r >> 15) & 0xf) - 8);
        r = rand();
        at<uint16_t>(p + 0x12) = static_cast<uint16_t>(r >> 15);
        ringPush(m);
    }
    int32_t index = ringHead(m);
    if (!ringAtTail(m, index))
    {
        do
        {
            const uint32_t p = m + kParticles + static_cast<uint32_t>(index) * 0x14u;
            const uint16_t velY = at<uint16_t>(p + 0xa);
            const uint16_t life = static_cast<uint16_t>(at<uint16_t>(p + 0x10) - 1);
            at<uint16_t>(p + 2) = static_cast<uint16_t>(at<uint16_t>(p + 2) + velY);
            at<uint16_t>(p) = static_cast<uint16_t>(at<uint16_t>(p) + at<uint16_t>(p + 8));
            at<uint16_t>(p + 4) = static_cast<uint16_t>(at<uint16_t>(p + 4) + at<uint16_t>(p + 0xc));
            at<uint16_t>(p + 0xa) = static_cast<uint16_t>(velY + 3);
            at<uint16_t>(p + 0x10) = life;
            if (life == 0)
                ringRemove(m, index, 0x14);
            index = ringNext(m, index);
        } while (!ringAtTail(m, index));
    }
    sortAtScenery(m, 0x60);
}

// The fountain: every other frame (a counter at +0x347) a particle starts if the ring has room; the up velocity starts at
// 10 and falls by 4 a frame.
void Vfx2Handler_Fountain(ptr32<_sVfx_TSFX> mem)
{
    const uint32_t m = mem.address;
    const uint8_t tick = static_cast<uint8_t>(at<uint8_t>(m + 0x347) + 1);
    at<uint8_t>(m + 0x347) = tick;
    if ((tick & 1) != 0 && at<uint8_t>(m + kRing + 1) < 0x1f)
        spoutSpawn(m, 10);
    spoutParticles(m, -4);
    sortAtScenery(m, 0x70);
}

// Moby Dick's spout: as the fountain, but it only runs while the scenery object's animation frame is 0x8d or more, the
// start velocity is 0x1e and gravity 3. The counter runs whatever the frame.
void Vfx2Handler_MobyDick(ptr32<_sVfx_TSFX> mem)
{
    const uint32_t m = mem.address;
    const uint32_t tob = at<uint32_t>(m + 0x10);
    const uint8_t tick = static_cast<uint8_t>(at<uint8_t>(m + 0x347) + 1);
    at<uint8_t>(m + 0x347) = tick;
    const int32_t frameWord = at<int32_t>(at<uint32_t>(at<uint32_t>(tob) + 0x38) + 0x18);
    if ((tick & 1) != 0 && at<uint8_t>(m + kRing + 1) < at<uint8_t>(m + kRing) && !(frameWord < 0x8d))
        spoutSpawn(m, 0x1e);
    spoutParticles(m, -3);
    sortAtScenery(m, 0x70);
}

// Steam from a sci-fi vent: a second counter (+0x186) counts down and flips the vent on and off (+0x184, low bit) with a
// random delay; while on, a particle of 0x14 bytes starts each frame the ring has room. Particles slow by an eighth of
// their velocity each frame and rise with gravity 3.
void Vfx2Handler_SciFiSteam(ptr32<_sVfx_TSFX> mem)
{
    const uint32_t m = mem.address;
    const uint16_t countdown = static_cast<uint16_t>(at<uint16_t>(m + 0x186) - 1);
    at<uint16_t>(m + 0x186) = countdown;
    if (countdown == 0)
    {
        at<uint16_t>(m + 0x184) = static_cast<uint16_t>(at<uint16_t>(m + 0x184) + 1);
        const int32 r = rand();
        at<uint16_t>(m + 0x186) = static_cast<uint16_t>(((r >> 15) & 0xf) + 0x10);
    }
    if ((at<uint16_t>(m + 0x184) & 1) != 0 && at<uint8_t>(m + kRing + 1) < 0xf)
    {
        const uint32_t tob = at<uint32_t>(m + 0x10) + 0x18;
        const uint32_t p = m + kParticles + static_cast<uint32_t>(static_cast<int8_t>(at<uint8_t>(m + kRing + 3))) * 0x14u;
        at<uint16_t>(p) = 0;
        at<uint16_t>(p + 2) = 0;
        at<uint16_t>(p + 4) = 0;
        int32 r = rand();
        at<uint16_t>(p + 8) = static_cast<uint16_t>(((r >> 15) & 0x1f) + (static_cast<int16_t>(at<uint16_t>(tob + 2)) >> 5) - 0x10);
        r = rand();
        at<uint16_t>(p + 0xa) = static_cast<uint16_t>(((r >> 15) & 0x1f) + (static_cast<int16_t>(at<uint16_t>(tob + 8)) >> 5) - 0x10);
        r = rand();
        at<uint16_t>(p + 0x10) = 0x28;
        at<uint16_t>(p + 0xc) = static_cast<uint16_t>(((r >> 15) & 0x1f) + (static_cast<int16_t>(at<uint16_t>(tob + 0xe)) >> 5) - 0x10);
        r = rand();
        at<uint16_t>(p + 0x12) = static_cast<uint16_t>(r >> 15);
        ringPush(m);
    }
    int32_t index = ringHead(m);
    if (!ringAtTail(m, index))
    {
        do
        {
            const uint32_t p = m + kParticles + static_cast<uint32_t>(index) * 0x14u;
            const uint16_t velY = at<uint16_t>(p + 0xa), velX = at<uint16_t>(p + 8), velZ = at<uint16_t>(p + 0xc);
            const uint16_t life = static_cast<uint16_t>(at<uint16_t>(p + 0x10) - 1);
            at<uint16_t>(p) = static_cast<uint16_t>(at<uint16_t>(p) + velX);
            at<uint16_t>(p + 2) = static_cast<uint16_t>(at<uint16_t>(p + 2) + velY);
            at<uint16_t>(p + 4) = static_cast<uint16_t>(at<uint16_t>(p + 4) + velZ);
            at<uint16_t>(p + 8) = static_cast<uint16_t>(velX - (static_cast<int16_t>(velX) >> 3));
            at<uint16_t>(p + 0xc) = static_cast<uint16_t>(velZ - (static_cast<int16_t>(velZ) >> 3));
            at<uint16_t>(p + 0xa) = static_cast<uint16_t>(velY - (static_cast<int16_t>(velY) >> 3) + 3);
            at<uint16_t>(p + 0x10) = life;
            if (life == 0)
                ringRemove(m, index, 0x14);
            index = ringNext(m, index);
        } while (!ringAtTail(m, index));
    }
    sortAtScenery(m, 0x70);
}

// VISFX3: the rain and glint tables.

namespace {

// Puts a scenery instance in the first free slot of a table of 16 and returns. A full table is written one past its
// end, as the original does (for the rain table that is padding, for the glint table the first camera vector).
void addToTable(uint32_t table, ptr32<_sTOBInst> pTOB)
{
    uint32_t i = 0;
    while (i < 16 && at<uint32_t>(table + 4 * i) != 0)
        ++i;
    at<uint32_t>(table + 4 * i) = pTOB.address;
}

constexpr uint32_t kRainTable = 0x1f1e64u, kGlintTable = 0x1f36b0u;
constexpr uint32_t kCamTrans = 0x1f1160u, kCamMatrix = 0x1f1120u;

void clearParticles(uint32_t first, uint32_t count)
{
    for (uint32_t i = 0; i < count; ++i)
        at<uint16_t>(first + i * 0x30u) = 0; // iLife (+0x10 of each 0x30-byte particle)
}

void copyVector(uint32_t dst, uint32_t src) { std::memcpy(guest_host(dst & ~0xFu), guest_host(src & ~0xFu), 16); }

} // namespace

void Vfx3AddRain(ptr32<_sTOBInst> pTOB) { addToTable(kRainTable, pTOB); }
void Vfx3AddGlint(ptr32<_sTOBInst> pTOB) { addToTable(kGlintTable, pTOB); }

// Empties the rain and glint particle tables and remembers the camera they were last drawn from.
void Vfx3RainInit()
{
    clearParticles(0x1f1000u + 0x1c0u + 0x10u, 64);
    for (uint32_t i = 0; i < 16; ++i)
        at<uint32_t>(kRainTable + 4 * i) = 0;
    copyVector(0x1f1dc0u, kCamTrans);
    std::memcpy(guest_host(0x1f1de0u), guest_host(kCamMatrix), 64);
    copyVector(0x1f1dd0u, kCamTrans);
    std::memcpy(guest_host(0x1f1e20u), guest_host(kCamMatrix), 64);
    clearParticles(0x1f1000u + 0xeb0u + 0x10u, 128);
    for (uint32_t i = 0; i < 16; ++i)
        at<uint32_t>(kGlintTable + 4 * i) = 0;
    copyVector(0x1f36f0u, kCamTrans);
    copyVector(0x1f3700u, kCamTrans);
}


} // namespace rc

namespace rc::fuzz { // visfx_fuzz.cpp
void vfxTSFXOnly(R5900Context *ctx, uint64_t &state, uint32_t scratch);

void vfxLights(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxRouteFinder(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxUfoBurner(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxOilCovered(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxBargeOil(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxFinishText(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxGargoyle(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxFountain(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxMobyDick(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxSciFiSteam(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxRainTable(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxGlintTable(R5900Context *ctx, uint64_t &state, uint32_t scratch);
}

RC_DECOMP_FN(Vfx3PolyWheel, 0x178cf8)
RC_DECOMP_FN(Vfx2Renderer_Bats, 0x179018)
RC_DECOMP_FN_SETUP(Vfx2Handler_RouteFinder, 0x176768, rc::fuzz::vfxRouteFinder)
RC_DECOMP_FN_SETUP(Vfx2Handler_UFOBurner, 0x171940, rc::fuzz::vfxUfoBurner)
RC_DECOMP_FN(VfxSortPosTS, 0x1790d8)
RC_DECOMP_FN(VfxVecSortPosTS, 0x179118)
RC_DECOMP_FN_SETUP(Vfx2Handler_Lights, 0x178fc8, rc::fuzz::vfxLights)
RC_DECOMP_FN_SETUP(VfxHandler_OilCovered, 0x178d98, rc::fuzz::vfxOilCovered)
RC_DECOMP_FN_SETUP(VfxHandler_BargeOil, 0x178d00, rc::fuzz::vfxBargeOil)
RC_DECOMP_FN(Vfx2Handler_TumbleWeed, 0x179020)
RC_DECOMP_FN_SETUP(Vfx2FinishText, 0x178e28, rc::fuzz::vfxFinishText)
RC_DECOMP_FN_SETUP(Vfx2Handler_Gargoyle, 0x175820, rc::fuzz::vfxGargoyle)
RC_DECOMP_FN_SETUP(Vfx2Handler_Fountain, 0x172df8, rc::fuzz::vfxFountain)
RC_DECOMP_FN_SETUP(Vfx2Handler_MobyDick, 0x178878, rc::fuzz::vfxMobyDick)
RC_DECOMP_FN_SETUP(Vfx2Handler_SciFiSteam, 0x1725d8, rc::fuzz::vfxSciFiSteam)
RC_DECOMP_FN_SETUP(Vfx3AddRain, 0x17a4b8, rc::fuzz::vfxRainTable)
RC_DECOMP_FN_SETUP(Vfx3AddGlint, 0x17a5b8, rc::fuzz::vfxGlintTable)
RC_DECOMP_FN(Vfx3RainInit, 0x1791e0)
