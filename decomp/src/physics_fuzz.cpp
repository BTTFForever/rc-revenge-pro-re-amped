// Builds the random-test inputs of PHYSICS, GAMETASK, TBOX and LEVEL. See physics_fuzz.h.
//
// The whole-RAM setups change the dump freely: RAM is put back after every input. Guest calls made here reach the
// originals (a setup runs while comparing).

#include "physics_fuzz.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace rc {
ptr32<_sNode> NodeFindByTag(ptr32<_sNodeHead> pNodeHead, uint32 tag);                            // misc.cpp
ptr32<_sNode> NodeFindNextByTag(ptr32<_sNode> pCurrNode, ptr32<_sNodeHead> pNodeHead, uint32 tag); // misc.cpp
} // namespace rc

namespace rc::fuzz {

namespace {

constexpr uint32_t kObjectList = sym::objectVarG, kMainVars = sym::mainVarG, kSfxInitOk = sym::sfxVarG, kHelpOutScale = sym::_dbgHelpOutScale_534;
constexpr uint32_t kReadySteadyGo = (sym::hudVarG + 0x70u), kResultTrigger = (sym::visFxVarG + 0x2730u), kDoApplyAIBoost = sym::doApplyAIBoost_534;
constexpr uint32_t kGameTask = sym::GameTaskGlobals, kTaskDelete = sym::TaskDelete, kCameras = sym::cameraVarG, kCameraSize = 0x5b8u;
constexpr uint32_t kTBoxVars = sym::tBoxVarG, kLsParent = sym::lsVarG, kRevSfx = sym::revSfxVarG, kLevelPtr = sym::pLvlG;
constexpr uint32_t kScratchPadG = sym::scratchPadG, kRaceOrder = (sym::aiVarG + 0x4u);
constexpr uint32_t kWheelTags = sym::wheelTag_562, kBodySphereTag = 0x10000235u;

template <typename T>
T &mem(uint32_t address) { return *static_cast<T *>(guest_host(address)); }

uint32_t rnd(uint64_t &state, uint32_t n) { return n == 0 ? 0u : nextFuzz(state) % n; }
bool chance(uint64_t &state, uint32_t percent) { return rnd(state, 100) < percent; }
float frnd(uint64_t &state, float scale) { return (static_cast<float>(static_cast<int32_t>(nextFuzz(state) % 20001u) - 10000) / 10000.0f) * scale; }
void setArg(R5900Context *ctx, int index, uint32_t value) { SET_GPR_S32(ctx, 4 + index, static_cast<int32_t>(value)); }
bool inRam(uint32_t address) { return address != 0 && address < 0x2000000u; }

// A float of any size: ordinary, tiny, huge (past what a word holds), negative zero, now and then a NaN.
float anyFloat(uint64_t &state, float scale)
{
    switch (rnd(state, 10))
    {
    case 0: return std::ldexp(frnd(state, 1.0f), -static_cast<int>(rnd(state, 140)));
    case 1: return frnd(state, 1.0f) * 3.0e10f;
    case 2: return -0.0f;
    case 3: return rnd(state, 4) ? 0.0f : std::numeric_limits<float>::quiet_NaN();
    default: return frnd(state, scale);
    }
}

uint32_t noObjects() { return mem<uint32_t>(kObjectList + 0x170u); }
uint32_t objectAt(uint32_t n) { return mem<uint32_t>(kObjectList + 4u * n); }

bool usableCar(uint32_t car)
{
    if (!inRam(car))
        return false;
    const uint32_t phys = mem<uint32_t>(car + 0x20u), def = mem<uint32_t>(car + 0xcu), carCol = mem<uint32_t>(car + 0x24u);
    return inRam(phys) && inRam(def) && inRam(carCol) && inRam(mem<uint32_t>(def + 0x28u)) && inRam(mem<uint32_t>(def + 0x2cu)) &&
           inRam(mem<uint32_t>(phys + 0x1e0u)) && inRam(mem<uint32_t>(phys + 0x1e4u)) && inRam(mem<uint32_t>(car + 0x2cu)) &&
           inRam(mem<uint32_t>(carCol + 4u)) && inRam(mem<uint32_t>(mem<uint32_t>(carCol + 4u) + 0x28u));
}

uint32_t pickCar(uint64_t &state)
{
    const uint32_t count = std::min<uint32_t>(noObjects(), 46);
    for (int tries = 0; tries < 30 && count; ++tries)
        if (const uint32_t car = objectAt(rnd(state, count)); usableCar(car))
            return car;
    return objectAt(0);
}

void soundOff() { mem<uint8_t>(kSfxInitOk) = 0; }

int32_t timeNow() { return mem<int32_t>(kMainVars + 0x1cu); }

// Stand-in polys (only their material byte, +0x53, is read) for contacts made up below.
uint32_t fakePoly(uint64_t &state, uint32_t scratch)
{
    const uint32_t poly = scratch + 0x7000u + 0x60u * rnd(state, 8);
    mem<uint8_t>(poly + 0x53u) = static_cast<uint8_t>(chance(state, 30) ? 0x46 : rnd(state, 0x40));
    return poly;
}

// A car's wheel spheres' contacts (counts, types, normals, depths), its wheels' materials, its body spheres' contact
// counters, its metamorphosis and respawn state.
void varyCar(uint64_t &state, uint32_t car, uint32_t scratch)
{
    const uint32_t phys = mem<uint32_t>(car + 0x20u);
    const uint32_t spheres = mem<uint32_t>(phys + 0x1e0u);
    for (uint32_t w = 0; w < 4; ++w)
    {
        const uint32_t s = spheres + w * 0x190u;
        if (chance(state, 60))
        {
            // The car and other contacts (the first numCarCarCols + numOtherCols) are kept: the car's flags say
            // which exist, and an "other" contact is never shallower than 0 in the game. Track contacts are made up.
            const uint32_t first = static_cast<uint32_t>(std::max(0, mem<int8_t>(s + 0x64u) + mem<int8_t>(s + 0x65u)));
            if (first >= 12)
                continue;
            mem<int8_t>(s + 0x67u) = static_cast<int8_t>(first + rnd(state, 13 - first));
            for (uint32_t k = first; k < 12; ++k)
            {
                mem<int8_t>(s + 0x94u + k) = static_cast<int8_t>(rnd(state, 4));
                mem<float>(s + 0xa4u + 16u * k) = frnd(state, 20.0f);
                mem<float>(s + 0xacu + 16u * k) = frnd(state, 300.0f);
                if (!inRam(mem<uint32_t>(s + 0x160u + 4u * k)) || chance(state, 30))
                    mem<uint32_t>(s + 0x160u + 4u * k) = fakePoly(state, scratch);
            }
        }
        if (chance(state, 40))
            mem<int16_t>(phys + 0xacu + 0x48u * w + 0x3eu) = static_cast<int16_t>(chance(state, 60) ? 0x46 : rnd(state, 0x40));
        if (chance(state, 20))
            mem<float>(s + 0x58u) = frnd(state, chance(state, 50) ? 3000.0f : 60000.0f);
    }
    for (uint32_t b = 5; b < 9; ++b)
        if (chance(state, 25))
            mem<int16_t>(spheres + b * 0x190u + 0x60u) = static_cast<int16_t>(rnd(state, 10));
    if (chance(state, 40))
    {
        mem<int16_t>(phys + 0u) = static_cast<int16_t>(nextFuzz(state));
        mem<int16_t>(phys + 2u) = static_cast<int16_t>(nextFuzz(state));
        mem<int16_t>(phys + 4u) = static_cast<int16_t>(nextFuzz(state));
    }
    if (chance(state, 30)) // the contact bits (0x40 cars, 0x80 other) kept
        mem<uint8_t>(phys + 0x1cdu) = static_cast<uint8_t>((nextFuzz(state) & 0x3fu) | (mem<uint8_t>(phys + 0x1cdu) & 0xc0u));
    if (chance(state, 40))
    {
        mem<uint8_t>(car + 0x176u) = static_cast<uint8_t>(rnd(state, 2));
        static const int32_t kKinds[] = {0, -1, 1, 2, 3};
        const int32_t kind = kKinds[rnd(state, 5)];
        // Kind 3: started 398 to 401 ms before now (the task's time is often now: the 400 ms boundary).
        mem<int32_t>(car + 0x1d0u) = kind <= 0                 ? kind
                                     : kind == 1               ? timeNow() - static_cast<int32_t>(rnd(state, 600))
                                     : kind == 3               ? timeNow() - 398 - static_cast<int32_t>(rnd(state, 4))
                                                               : static_cast<int32_t>(nextFuzz(state));
        mem<int32_t>(car + 0x1d4u) = static_cast<int32_t>(rnd(state, 4000));
    }
    if (chance(state, 30))
        mem<int8_t>(car + 0x18au) = static_cast<int8_t>(rnd(state, 3));
    if (chance(state, 50))
    {
        mem<uint16_t>(car + 0x170u) = static_cast<uint16_t>(rnd(state, 0x4000)); // zShortestDrawnDist
        mem<int16_t>(car + 0x172u) = static_cast<int16_t>(rnd(state, 2));        // sDontRenderMe
    }
    if (chance(state, 30))
        mem<int8_t>(car + 0x21cu) = static_cast<int8_t>(chance(state, 80) ? rnd(state, 5) : nextFuzz(state));
    if (chance(state, 10))
    {
        // Off the world.
        mem<float>(phys + 0x8u) = frnd(state, 1.0e8f);
        mem<float>(phys + 0xcu) = frnd(state, 1.0e8f);
    }
}

void varyRace(uint64_t &state, uint32_t scratch)
{
    soundOff();
    for (uint32_t i = 0; i < std::min<uint32_t>(noObjects(), 46); ++i)
        if (const uint32_t car = objectAt(i); usableCar(car) && chance(state, 50))
            varyCar(state, car, scratch);
    if (chance(state, 40))
        mem<int32_t>(kReadySteadyGo) = static_cast<int32_t>(rnd(state, 5));
    if (chance(state, 30))
        mem<int32_t>(kResultTrigger) = static_cast<int32_t>(rnd(state, 2));
    mem<int32_t>(kDoApplyAIBoost) = static_cast<int32_t>(rnd(state, 2));
    if (chance(state, 40))
        mem<uint8_t>(kMainVars + 0x369u) = static_cast<uint8_t>(rnd(state, 3)); // noHuman
    if (chance(state, 40) || mem<uint8_t>(kMainVars + 0x36au) == 0)
        mem<uint8_t>(kMainVars + 0x36au) = static_cast<uint8_t>(1 + rnd(state, 8)); // noAI
    if (chance(state, 30))
        mem<int32_t>(kMainVars + 8u) = static_cast<int32_t>(chance(state, 50) ? 1 : rnd(state, 4)); // currentDo
    if (chance(state, 30))
        mem<int32_t>(kHelpOutScale) = static_cast<int32_t>(nextFuzz(state) % 64u) - 16;
    if (chance(state, 20))
        mem<int32_t>(kCameras) = static_cast<int32_t>(rnd(state, 3)); // camera 0's mode (1: front end)
    if (chance(state, 50)) // not a demo race nor over: the cars' drawn distances count
        mem<uint32_t>(kMainVars) &= ~0x30u;
}

void unlinkGameTask()
{
    if (mem<uint32_t>(kGameTask + 0x10u) != 0 || mem<uint32_t>(kGameTask + 0x14u) != 0)
        guest_call(kTaskDelete, kGameTask);
}

// A copy of a real car's physics at scratch+at, its spheres after it (pSphere repointed), varied.
uint32_t carCopy(uint64_t &state, uint32_t scratch, uint32_t at)
{
    const uint32_t from = mem<uint32_t>(pickCar(state) + 0x20u);
    const uint32_t car = scratch + at;
    std::memcpy(guest_host(car), guest_host(from), 0x23c);
    const uint32_t spheres = scratch + at + 0x400u;
    if (inRam(mem<uint32_t>(from + 0x1e0u)))
        std::memcpy(guest_host(spheres), guest_host(mem<uint32_t>(from + 0x1e0u)), 0x190u * 9u);
    mem<uint32_t>(car + 0x1e0u) = spheres;
    mem<uint32_t>(car + 0x1e4u) = scratch + 0x7f00u;
    for (uint32_t off = 0x8u; off < 0x94u; off += 4u)
        if (chance(state, 15))
            mem<float>(car + off) = anyFloat(state, 40000.0f);
    if (chance(state, 30))
        mem<int16_t>(car + 0x1d0u) = static_cast<int16_t>(nextFuzz(state));
    for (uint32_t b = 5; b < 9; ++b)
        if (chance(state, 30))
            mem<int16_t>(spheres + b * 0x190u + 0x60u) = static_cast<int16_t>(chance(state, 70) ? rnd(state, 10) : nextFuzz(state));
    if (chance(state, 50))
    {
        mem<int16_t>(car + 0u) = static_cast<int16_t>(nextFuzz(state));
        mem<int16_t>(car + 2u) = static_cast<int16_t>(nextFuzz(state));
    }
    return car;
}

uint32_t floatVector(uint64_t &state, uint32_t address)
{
    for (uint32_t k = 0; k < 3; ++k)
        mem<float>(address + 4u * k) = chance(state, 80) ? frnd(state, chance(state, 50) ? 5000.0f : 50.0f) : anyFloat(state, 1.0e5f);
    return address;
}

// A real box of the level's grid (index), or 0.
uint32_t realBox(uint64_t &state)
{
    const uint32_t grid = mem<uint32_t>(kTBoxVars);
    if (!inRam(grid) || mem<uint32_t>(grid) == 0 || !inRam(mem<uint32_t>(grid + 8u)))
        return 0;
    return mem<uint32_t>(grid + 8u) + 0x18u * rnd(state, std::min<uint32_t>(mem<uint32_t>(grid), 4000));
}

// A point (whole units * 256) inside or near a box, or anywhere near one of the cars.
void pointNear(uint64_t &state, uint32_t box, int32_t out[3])
{
    if (box == 0 || chance(state, 15))
    {
        const uint32_t phys = mem<uint32_t>(pickCar(state) + 0x20u);
        for (uint32_t k = 0; k < 3; ++k)
            out[k] = static_cast<int32_t>(mem<float>(phys + 8u + 4u * k)) + static_cast<int32_t>(rnd(state, 0x8000)) - 0x4000;
        return;
    }
    for (uint32_t k = 0; k < 3; ++k)
    {
        const int32_t centre = mem<int16_t>(box + 8u + 2u * k);
        const int32_t half = std::max<int32_t>(1, std::abs(static_cast<int32_t>(mem<int16_t>(box + 0xeu + 2u * k))));
        const int32_t at = chance(state, 20) ? (chance(state, 50) ? half : -half) + static_cast<int32_t>(rnd(state, 3)) - 1 // on an edge
                                             : static_cast<int32_t>(rnd(state, static_cast<uint32_t>(half) * 3u)) - half * 3 / 2;
        out[k] = (centre + at) * 256 + static_cast<int32_t>(rnd(state, 256));
    }
}

// Whether TBoxUpdate finds a position (1/256 units) inside the trigger grid. It does not check: off the grid it reads a
// "cell" from past the grid's table, follows it to a list of garbage box numbers and calls through the garbage type's
// callback slot (a jump to nowhere, 0x800e on EU level 20 with seed 4243, which ended the oracle's child). Cars and
// cameras are always over the level in the game, so the setups keep the points they move on the grid.
bool onGrid(const int32_t p[3])
{
    const uint32_t grid = mem<uint32_t>(kTBoxVars), parent = mem<uint32_t>(kLsParent);
    if (!inRam(grid) || !inRam(parent))
        return true; // TBoxUpdate returns at once without a grid; a broken parent is not the setup's doing
    const uint32_t shift = static_cast<uint32_t>(mem<int16_t>(grid + 4u)) & 31u, widthShift = static_cast<uint32_t>(mem<int16_t>(grid + 6u)) & 31u;
    const int32_t col = (static_cast<int16_t>(p[0] >> 8) - mem<int32_t>(parent + 0x58u)) >> shift;
    const int32_t row = (static_cast<int16_t>(p[2] >> 8) - mem<int32_t>(parent + 0x5cu)) >> shift;
    if (col < 0 || row < 0 || row >= (1 << widthShift))
        return false;
    // The table of cells ends where the first list starts: the smallest non-zero cell offset.
    const uint32_t index = (static_cast<uint32_t>(col) << widthShift) + static_cast<uint32_t>(row);
    uint32_t end = 0x10000u;
    for (uint32_t i = 0; 0xcu + 2u * i < end; ++i)
        if (const uint16_t cell = mem<uint16_t>(grid + 0xcu + 2u * i); cell != 0 && cell < end)
            end = cell;
    return 0xcu + 2u * index + 2u <= end;
}

// pointNear, on the grid: a few tries, else camera 0's position (the dump's, over the level).
void pointOnGrid(uint64_t &state, uint32_t box, int32_t out[3])
{
    for (int tries = 0; tries < 8; ++tries)
    {
        pointNear(state, box, out);
        if (onGrid(out))
            return;
    }
    for (uint32_t k = 0; k < 3; ++k)
        out[k] = mem<int32_t>(kCameras + 0xcu + 4u * k);
}

// Varies a few of the level's boxes: angles, sizes, masks.
void varyBoxes(uint64_t &state)
{
    for (int i = 0; i < 8; ++i)
    {
        const uint32_t box = realBox(state);
        if (box == 0)
            return;
        if (chance(state, 40))
        {
            mem<int16_t>(box + 4u) = static_cast<int16_t>(chance(state, 50) ? 0 : nextFuzz(state));
            mem<int16_t>(box + 6u) = static_cast<int16_t>(chance(state, 50) ? 0 : nextFuzz(state));
        }
        if (chance(state, 30))
            mem<uint32_t>(box) = chance(state, 50) ? 0xff000000u | nextFuzz(state) : nextFuzz(state);
        if (chance(state, 15))
            mem<uint16_t>(box + 0x14u) = 2; // a sound box (its data's halfwords become a trigger)
    }
}

void varyTBoxRace(uint64_t &state)
{
    if (chance(state, 30))
        mem<uint8_t>(kMainVars + 0x368u) = static_cast<uint8_t>(rnd(state, 2)); // splitScreen
    if (chance(state, 30))
        mem<int32_t>(kReadySteadyGo) = chance(state, 70) ? 0 : static_cast<int32_t>(rnd(state, 4));
    if (chance(state, 20))
        mem<uint16_t>(kMainVars + 0x366u) = static_cast<uint16_t>(rnd(state, 2)); // pauseMenu
    if (chance(state, 30))
        mem<int32_t>(kMainVars + 0x174u) = static_cast<int32_t>(rnd(state, 2)); // gameEndPos
    if (chance(state, 30))
        mem<int32_t>(kRevSfx + 0x1140u) = static_cast<int32_t>(rnd(state, 2)); // rvbLevel
    // The second camera, a copy of the first (its target valid).
    if (mem<uint8_t>(kMainVars + 0x368u) != 0 || chance(state, 30))
        std::memcpy(guest_host(kCameras + kCameraSize), guest_host(kCameras), kCameraSize);
    for (uint32_t c = 0; c < 3; ++c) // inTunnel
        mem<int32_t>(kCameras + c * kCameraSize + 0x57cu) = static_cast<int32_t>(rnd(state, 2));
}

} // namespace

void physicsRace(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    varyRace(state, scratch);
    setArg(ctx, 0, chance(state, 80) ? rnd(state, 2) : nextFuzz(state)); // GameTaskGetShadowMaterialInfo's fastMethod
    // GameTaskLvlInit inserts the task: take it out of the list first, now and then.
    if (chance(state, 50))
        unlinkGameTask();
}

void physicsTask(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    varyRace(state, scratch);
    unlinkGameTask();
    const int32_t time = timeNow() + (chance(state, 50) ? 0 : static_cast<int32_t>(rnd(state, 0x40)));
    setArg(ctx, 0, static_cast<uint32_t>(time));
    setArg(ctx, 1, static_cast<uint32_t>(time));
}

void physicsObject(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    varyRace(state, scratch);
    const uint32_t car = pickCar(state);
    varyCar(state, car, scratch);
    setArg(ctx, 0, car);
}

// The car-car contacts of a car's spheres, so that every dump reaches both FCollisionResponseCarCar paths: none at
// depth 0 or more (its best sphere stays -1: the normal is a stale stack word and the other car a stale $t6, both made
// up here too), or new and varied ones (contacts are inserted before the sphere's others, which keep their order).
static void varyCarContacts(R5900Context *ctx, uint64_t &state, uint32_t phys)
{
    const uint32_t spheres = mem<uint32_t>(phys + 0x1e0u);
    const int32_t numSpheres = mem<int8_t>(phys + 0x1ccu);
    const auto otherCar = [&]
    {
        const uint32_t car = pickCar(state);
        return chance(state, 10) ? phys : mem<uint32_t>(car + 0x20u);
    };
    switch (rnd(state, 3))
    {
    case 0: // as the dump left them
        break;
    case 1: // none at depth 0 or more
        for (int32_t i = 0; i < numSpheres; ++i)
        {
            const uint32_t s = spheres + static_cast<uint32_t>(i) * 0x190u;
            for (int32_t c = 0; c < std::min<int32_t>(mem<int8_t>(s + 0x64u), 12); ++c)
                mem<float>(s + 0xacu + 16u * c) = -1.0f - std::fabs(frnd(state, 300.0f));
        }
        break;
    default: // contacts on one to three spheres
        for (uint32_t n = 1 + rnd(state, 3); n > 0 && numSpheres > 0; --n)
        {
            const uint32_t s = spheres + rnd(state, static_cast<uint32_t>(numSpheres)) * 0x190u;
            const int32_t total = mem<int8_t>(s + 0x67u), cars = mem<int8_t>(s + 0x64u);
            if (total < 0 || total > 12 || cars < 0 || cars > total)
                continue;
            const int32_t add = std::min<int32_t>(static_cast<int32_t>(rnd(state, 3)), 12 - total);
            if (add > 0)
            {
                std::memmove(guest_host(s + 0x94u + add), guest_host(s + 0x94u), static_cast<size_t>(total));
                std::memmove(guest_host(s + 0xa0u + 16u * add), guest_host(s + 0xa0u), 16u * total);
                std::memmove(guest_host(s + 0x160u + 4u * add), guest_host(s + 0x160u), 4u * total);
                mem<int8_t>(s + 0x64u) = static_cast<int8_t>(cars + add);
                mem<int8_t>(s + 0x67u) = static_cast<int8_t>(total + add);
            }
            for (int32_t c = 0; c < cars + add; ++c)
            {
                if (c >= add && chance(state, 50))
                    continue; // a contact from the dump kept
                mem<int8_t>(s + 0x94u + c) = 0;
                for (uint32_t k = 0; k < 3; ++k)
                    mem<float>(s + 0xa0u + 16u * c + 4u * k) = frnd(state, 4096.0f);
                mem<float>(s + 0xacu + 16u * c) = chance(state, 15) ? -std::fabs(frnd(state, 300.0f)) : std::fabs(frnd(state, 300.0f));
                mem<uint32_t>(s + 0x160u + 4u * c) = otherCar();
            }
        }
        break;
    }
    // The original's uninitialised normal (sp+0 of its 0x50 frame) and other car ($t6).
    if (chance(state, 50))
    {
        const uint32_t frame = getRegU32(ctx, 29) - 0x50u;
        for (uint32_t k = 0; k < 3; ++k)
            mem<float>(frame + 4u * k) = chance(state, 20) ? 0.0f : frnd(state, 4096.0f);
    }
    if (chance(state, 50))
    {
        const uint32_t pick = rnd(state, 3);
        SET_GPR_S32(ctx, 14, static_cast<int32_t>(pick == 0 ? 0u : pick == 1 ? otherCar() : (nextFuzz(state) % 0x1fff000u) & ~3u));
    }
}

void physicsResponse(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    varyRace(state, scratch);
    const uint32_t car = pickCar(state);
    varyCar(state, car, scratch);
    const uint32_t phys = mem<uint32_t>(car + 0x20u);
    varyCarContacts(ctx, state, phys);
    setArg(ctx, 0, phys);
}

void physicsAddCar(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    soundOff();
    const uint32_t car = pickCar(state);
    uint32_t def = mem<uint32_t>(car + 0xcu);
    if (chance(state, 60))
    {
        const uint32_t copy = scratch + 0x1000u;
        std::memcpy(guest_host(copy), guest_host(def), 0x58);
        if (chance(state, 25))
            mem<uint32_t>(copy + 4u) = 0xfu; // a UFO
        if (chance(state, 50))
            for (uint32_t k = 0; k < 3; ++k)
                mem<int32_t>(copy + 8u + 4u * k) += static_cast<int32_t>(rnd(state, 4000)) - 2000;
        if (chance(state, 50))
            mem<int16_t>(copy + 0x16u) = static_cast<int16_t>(nextFuzz(state));
        def = copy;
    }
    if (chance(state, 20))
        mem<uint32_t>(mem<uint32_t>(car + 0x2cu) + 0x10u) = 0; // no wheel models
    if (chance(state, 30))
        mem<uint8_t>(kMainVars + 0x34au) = static_cast<uint8_t>(chance(state, 50) ? 21 : rnd(state, 0x60)); // level
    setArg(ctx, 0, car);
    setArg(ctx, 1, def);
}

void physicsCarDef(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    soundOff();
    const uint32_t car = pickCar(state);
    const uint32_t phys = mem<uint32_t>(car + 0x20u);
    uint32_t def = mem<uint32_t>(car + 0xcu);
    if (chance(state, 40))
    {
        // A definition whose physics data is a copy with random words.
        const uint32_t copy = scratch + 0x1000u, data = scratch + 0x1100u;
        std::memcpy(guest_host(copy), guest_host(def), 0x58);
        std::memcpy(guest_host(data), guest_host(mem<uint32_t>(def + 0x2cu)), 0xf8);
        for (uint32_t off = 4u; off < 0xf8u; off += 4u)
            if (chance(state, 30))
                mem<uint32_t>(data + off) = chance(state, 50) ? nextFuzz(state) : nextFuzz(state) % 20000u;
        mem<uint32_t>(copy + 0x2cu) = data;
        def = copy;
    }
    mem<int8_t>(phys + 0x1ceu) = static_cast<int8_t>(chance(state, 45) ? 1 : chance(state, 80) ? 2 : rnd(state, 4)); // mode
    if (chance(state, 20))
        mem<uint32_t>(phys + 0x1e0u) = 0; // no spheres yet
    if (chance(state, 30))
        mem<uint8_t>(kMainVars + 0x34au) = static_cast<uint8_t>(chance(state, 50) ? 21 + 24 * rnd(state, 2) : rnd(state, 0x60));
    // The sphere nodes' radii (a boat's second radius).
    const ptr32<_sNodeHead> head(mem<uint32_t>(def + 0x28u));
    for (uint32_t node = NodeFindByTag(head, kBodySphereTag).address; inRam(node); node = NodeFindNextByTag(ptr32<_sNode>(node), head, kBodySphereTag).address)
    {
        if (chance(state, 30))
            mem<int16_t>(node + 8u) = static_cast<int16_t>(rnd(state, 200));
        if (chance(state, 40))
            mem<int16_t>(node + 0xau) = static_cast<int16_t>(chance(state, 50) ? 0 : rnd(state, 200));
        if (chance(state, 20))
            mem<int32_t>(node + 0x20u + 4u * rnd(state, 3)) = static_cast<int32_t>(rnd(state, 400)) - 200;
    }
    for (uint32_t w = 0; w < 4; ++w)
        if (const uint32_t node = NodeFindByTag(head, mem<uint32_t>(kWheelTags + 4u * w)).address; inRam(node) && chance(state, 20))
            mem<int16_t>(node + 8u) = static_cast<int16_t>(rnd(state, 100));
    setArg(ctx, 0, phys);
    setArg(ctx, 1, def);
}

void physicsBoatSwap(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    soundOff();
    const uint32_t car = pickCar(state);
    const uint32_t phys = mem<uint32_t>(car + 0x20u);
    mem<int8_t>(phys + 0x1ceu) = static_cast<int8_t>(chance(state, 80) ? 1 + rnd(state, 2) : rnd(state, 4));
    if (chance(state, 30))
        varyCar(state, car, scratch);
    setArg(ctx, 0, phys);
    setArg(ctx, 1, car);
}

void physicsForce(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    const uint32_t car = carCopy(state, scratch, 0);
    setArg(ctx, 0, car);
    setArg(ctx, 1, chance(state, 5) ? 0u : floatVector(state, scratch + 0x1800u));
    setArg(ctx, 2, chance(state, 25) ? 0u : floatVector(state, scratch + 0x1810u));
    setArg(ctx, 3, chance(state, 80) ? rnd(state, 4) : static_cast<uint32_t>(static_cast<int16_t>(nextFuzz(state))));
}

void physicsIntForce(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    const uint32_t car = carCopy(state, scratch, 0);
    for (uint32_t k = 0; k < 6; ++k)
        mem<int16_t>(scratch + 0x1800u + 2u * k) = static_cast<int16_t>(chance(state, 70) ? static_cast<int32_t>(rnd(state, 8000)) - 4000 : static_cast<int32_t>(nextFuzz(state)));
    setArg(ctx, 0, car);
    setArg(ctx, 1, chance(state, 20) ? 0u : scratch + 0x1800u);
    setArg(ctx, 2, chance(state, 25) ? 0u : scratch + 0x1806u);
    setArg(ctx, 3, chance(state, 80) ? rnd(state, 4) : static_cast<uint32_t>(static_cast<int16_t>(nextFuzz(state))));
}

void physicsCarFlag(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    // The help-out scale is a global: put back the dump's value, or a random one.
    static const int32_t dumpScale = mem<int32_t>(kHelpOutScale);
    mem<int32_t>(kHelpOutScale) = chance(state, 50) ? dumpScale : static_cast<int32_t>(nextFuzz(state) % 64u) - 16;
    const uint32_t car = carCopy(state, scratch, 0);
    setArg(ctx, 0, car);
    setArg(ctx, 1, chance(state, 80) ? 1u : chance(state, 50) ? 0u : nextFuzz(state));
}

void tboxRace(R5900Context *, uint64_t &state, uint32_t scratch)
{
    soundOff();
    varyTBoxRace(state);
    varyBoxes(state);
    // Cameras and cars moved into (or near) boxes.
    for (uint32_t c = 0; c < 2; ++c)
        if (chance(state, 60))
        {
            int32_t p[3];
            pointOnGrid(state, realBox(state), p);
            for (uint32_t k = 0; k < 3; ++k)
                mem<int32_t>(kCameras + c * kCameraSize + 0xcu + 4u * k) = p[k];
        }
    for (uint32_t i = 0; i < std::min<uint32_t>(noObjects(), 46); ++i)
    {
        const uint32_t car = mem<uint32_t>(kRaceOrder + 12u * i);
        if (!usableCar(car) || chance(state, 40))
            continue;
        int32_t p[3];
        pointOnGrid(state, realBox(state), p);
        const uint32_t phys = mem<uint32_t>(car + 0x20u);
        for (uint32_t k = 0; k < 3; ++k)
            mem<float>(phys + 8u + 4u * k) = static_cast<float>(p[k]) + frnd(state, 0.5f);
        if (chance(state, 30))
            mem<uint32_t>(car) = nextFuzz(state); // tBoxMask
        if (chance(state, 20))
            mem<uint32_t>(car) &= ~8u;
    }
    if (chance(state, 10))
        mem<uint32_t>(kTBoxVars) = 0;
    (void)scratch;
}

void tboxUpdate(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    soundOff();
    varyTBoxRace(state);
    varyBoxes(state);
    const uint32_t box = realBox(state);
    int32_t p[3];
    pointOnGrid(state, box, p);
    for (uint32_t k = 0; k < 3; ++k)
        mem<int32_t>(scratch + 4u * k) = p[k];
    const uint32_t car = pickCar(state);
    uint32_t mask;
    switch (rnd(state, 4))
    {
    case 0: mask = 0xff000001u + rnd(state, 2); break;
    case 1: mask = mem<uint32_t>(car) | (0x10000u << rnd(state, 8)); break;
    case 2: mask = box ? mem<uint32_t>(box) | (nextFuzz(state) & 0xffffffu) : nextFuzz(state); break;
    default: mask = nextFuzz(state); break;
    }
    if (chance(state, 10))
        mem<uint32_t>(kTBoxVars) = 0;
    setArg(ctx, 0, scratch);
    setArg(ctx, 1, mask);
    setArg(ctx, 2, chance(state, 20) ? kCameras : car);
}

namespace {

// A real box of a given data type, or 0.
uint32_t realBoxOfType(uint64_t &state, uint16_t type)
{
    for (int tries = 0; tries < 60; ++tries)
        if (const uint32_t box = realBox(state); box != 0 && mem<uint16_t>(box + 0x14u) == type)
            return box;
    return 0;
}

// a0 a box of the given type (a real one of the level, or one made in scratch with data laid out for its callback),
// a1 a car.
void tboxCallbackOf(R5900Context *ctx, uint64_t &state, uint32_t scratch, uint16_t type)
{
    soundOff();
    varyTBoxRace(state);
    const uint32_t car = pickCar(state);
    if (chance(state, 25))
        mem<uint32_t>(car) ^= 8u; // the other camera
    for (uint32_t k = 8; k < 12; ++k)
        if (chance(state, 30))
            mem<uint8_t>(car + k) = static_cast<uint8_t>(chance(state, 50) ? nextFuzz(state) : (nextFuzz(state) & 1u) * 0xffu);
    mem<int32_t>(kScratchPadG + 0x18u) = static_cast<int32_t>(rnd(state, 4000)) - 2000; // the reverb's z offset
    uint32_t box = realBoxOfType(state, type);
    if (box == 0 || chance(state, 60))
    {
        box = scratch;
        const uint32_t data = scratch + 0x100u;
        mem<uint16_t>(box + 0x14u) = type;
        mem<uint16_t>(box + 0x16u) = static_cast<uint16_t>(data - box - 0x16u);
        mem<int16_t>(box + 0x12u) = static_cast<int16_t>(1 + rnd(state, 2000)); // maxZ, never 0 (the reverb divides)
        switch (type)
        {
        case 5:
        {
            // An animation: flags at +0x22, state words at +0xc -> +0x40 or +0xc -> * -> +0x7c.
            const uint32_t anim = scratch + 0x200u, inner = scratch + 0x300u;
            mem<uint32_t>(data) = chance(state, 10) ? 0u : anim;
            mem<uint16_t>(data + 4u) = static_cast<uint16_t>(rnd(state, 6));
            mem<uint16_t>(data + 6u) = static_cast<uint16_t>(chance(state, 70) ? 1 : rnd(state, 3));
            mem<uint32_t>(anim) = chance(state, 30) ? 0u : nextFuzz(state);
            mem<uint16_t>(anim + 0x22u) = static_cast<uint16_t>(rnd(state, 3) == 0 ? 0x80 : rnd(state, 2) ? 0x40 : 0);
            mem<uint32_t>(anim + 0xcu) = inner;
            mem<uint32_t>(inner) = inner + 0x10u;
            break;
        }
        case 10:
            mem<int32_t>(data) = static_cast<int32_t>(chance(state, 90) ? 1 + rnd(state, 8) : rnd(state, 2) * 9); // a trigger number
            break;
        case 4:
            // Camera data: a point, flags, angles.
            for (uint32_t k = 0; k < 3; ++k)
                mem<int16_t>(data + 2u * k) = static_cast<int16_t>(rnd(state, 4000)) - 2000;
            mem<uint16_t>(data + 6u) = static_cast<uint16_t>(nextFuzz(state) & 0xfu);
            break;
        case 2: // a sound trigger's number
            mem<int16_t>(data + 2u) = static_cast<int16_t>(rnd(state, std::max<uint32_t>(1u, mem<uint32_t>(kRevSfx + 0x1184u))));
            break;
        default:
            // Small signed amounts (tint), reverb levels, environment words.
            for (uint32_t k = 0; k < 4; ++k)
                mem<int16_t>(data + 2u * k) = static_cast<int16_t>(rnd(state, 600)) - 300;
            break;
        }
    }
    else if (mem<int16_t>(box + 0x12u) == 0)
        mem<int16_t>(box + 0x12u) = 1;
    setArg(ctx, 0, box);
    setArg(ctx, 1, car);
}

} // namespace

void tboxReverb(R5900Context *ctx, uint64_t &state, uint32_t scratch) { tboxCallbackOf(ctx, state, scratch, 0); }
void tboxEnv(R5900Context *ctx, uint64_t &state, uint32_t scratch) { tboxCallbackOf(ctx, state, scratch, 1); }
void tboxSfx(R5900Context *ctx, uint64_t &state, uint32_t scratch) { tboxCallbackOf(ctx, state, scratch, 2); }
void tboxCam(R5900Context *ctx, uint64_t &state, uint32_t scratch) { tboxCallbackOf(ctx, state, scratch, 4); }
void tboxAnim(R5900Context *ctx, uint64_t &state, uint32_t scratch) { tboxCallbackOf(ctx, state, scratch, 5); }
void tboxTint(R5900Context *ctx, uint64_t &state, uint32_t scratch) { tboxCallbackOf(ctx, state, scratch, 6); }
void tboxTunnel(R5900Context *ctx, uint64_t &state, uint32_t scratch) { tboxCallbackOf(ctx, state, scratch, 9); }
void tboxCol(R5900Context *ctx, uint64_t &state, uint32_t scratch) { tboxCallbackOf(ctx, state, scratch, 10); }

void levelData(R5900Context *, uint64_t &state, uint32_t scratch)
{
    // Put back the dump's level pointer (a setup's globals persist), then now and then use one made in scratch.
    static const uint32_t dumpLevel = mem<uint32_t>(kLevelPtr);
    mem<uint32_t>(kLevelPtr) = dumpLevel;
    if (inRam(dumpLevel) && chance(state, 50))
        return;
    const uint32_t lvl = scratch;
    for (uint32_t off = 0; off < 0x60u; off += 4u)
        mem<uint32_t>(lvl + off) = scratch + 0x400u + 0x100u * rnd(state, 8);
    mem<uint32_t>(kLevelPtr) = lvl;
}

void levelInitTerm(R5900Context *, uint64_t &, uint32_t)
{
    soundOff();
}

} // namespace rc::fuzz
