// Inputs for the random test of the small VISFX functions (RC_SHADOW_FUZZ). They use one of the race's real cars,
// copied into the scratch area by the route-node setup, and build effect objects and wheel indices around it.

#include "ai_fuzz.h"
#include "ee_math.h"
#include "visfx.h"

namespace rc::fuzz {

namespace {

constexpr uint32_t kEffect = 0x4000u;   // an effect object placed after the car's records (0x0..0x2300)
constexpr uint32_t kEffectSize = 0x700u;
constexpr uint32_t kQuad = 0x4700u;     // the quad the sprites draw, clear of the effect (its UVs would overwrite the ring header)
constexpr uint32_t kDef = 0x3800u;      // a copy of the car's object definition
constexpr uint32_t kAuxA = 0x4800u, kAuxB = 0x5000u, kAuxC = 0x5800u; // small records the effects point at (0x200 each)
constexpr uint32_t kTobInst = 0x6000u, kTob = 0x6200u, kShape = 0x6400u; // a scenery instance, its object and shape
constexpr uint32_t kRing = 0x6800u, kRingParticles = 0x6900u;            // a particle ring header and its particles
constexpr uint32_t kVectors = 0x7000u, kTexture = 0x7400u, kSprite = 0x7800u, kNormal = 0x7c00u;

void setArg(R5900Context *ctx, int index, uint32_t value) { SET_GPR_S32(ctx, 4 + index, static_cast<int32_t>(value)); }
void setFloatArg(R5900Context *ctx, int index, uint32_t bits) { std::memcpy(&ctx->f[12 + index], &bits, 4); }

// Globals the setups point at their scratch data. The harness keeps what a setup leaves in the globals until the end of
// its step (the next input starts from it, and so does every function tested after this one in the step), so each setup
// puts back what the earlier ones changed before it builds its inputs. A value is only put back if it still holds what
// the setup wrote: after the step's own revert (or a change by the game) it is left alone. What the last setup of a step
// changes stays until the step ends.
template <typename T>
struct Claim
{
    uint32_t address = 0;
    bool active = false;
    T original{}, written{};

    T &ref() const { return *static_cast<T *>(guest_host(address)); }
    void set(uint32_t where, const T &value)
    {
        address = where;
        if (!(active && std::memcmp(&ref(), &written, sizeof(T)) == 0))
        {
            original = ref();
            active = true;
        }
        ref() = value;
        written = value;
    }
    void restore()
    {
        if (active && std::memcmp(&ref(), &written, sizeof(T)) == 0)
            ref() = original;
        active = false;
    }
};

struct Table { uint32_t words[17]; };

Claim<uint32_t> g_quad, g_sprite, g_trackHints, g_res, g_time;
Claim<Table> g_rain, g_glint;

void restoreGlobals()
{
    g_quad.restore();
    g_sprite.restore();
    g_trackHints.restore();
    g_res.restore();
    g_time.restore();
    g_rain.restore();
    g_glint.restore();
}

uint32_t rnd(uint64_t &state, uint32_t n) { return nextFuzz(state) % n; }

// A float in a range that keeps products and sums finite.
float frnd(uint64_t &state, float scale = 1000.0f) { return (static_cast<float>(static_cast<int32_t>(nextFuzz(state) % 20001u) - 10000) / 10000.0f) * scale; }

bool levelHasCars()
{
    for (uint32_t i = 0; i < G(objectVarG).noObjects; ++i)
        if (G(objectVarG).list[i])
            return true;
    return false;
}

// A car at scratch+0 if a race is loaded (see aiCar); the route-node setup also needs nodes, which every race has.
bool buildCar(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    restoreGlobals();
    aiCar(ctx, state, scratch);
    return levelHasCars();
}

} // namespace

// a0 car, a1 a wheel number: 0 to 3, and now and then a larger or negative one that reads somewhere else.
void vfxWheel(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    if (!buildCar(ctx, state, scratch))
        return;
    _sObject &car = *static_cast<_sObject *>(guest_host(scratch));
    // Half the time make the car a boat, so the water-depth path runs too.
    if (nextFuzz(state) % 2u)
    {
        _sObjectDef &def = *static_cast<_sObjectDef *>(guest_host(scratch + kDef));
        def = *car.pDef;
        def.type = static_cast<eObjectType>(OBJECT_TYPE_BADDRC_BOAT + nextFuzz(state) % 4u);
        car.pDef = ptr32<_sObjectDef>(scratch + kDef);
    }
    else
    {
        _sObjectDef &def = *static_cast<_sObjectDef *>(guest_host(scratch + kDef));
        def = *car.pDef;
        def.type = static_cast<eObjectType>(nextFuzz(state) % 8u);
        car.pDef = ptr32<_sObjectDef>(scratch + kDef);
    }
    _sFPhysLibCar &physics = *car.pPhysics;
    for (_sFPhysLibWheel &w : physics.wheel)
    {
        w.flags = static_cast<uint16>(nextFuzz(state));
        w.drivingOnMtl = static_cast<int16>(nextFuzz(state) % 3u ? nextFuzz(state) % 0x60u : nextFuzz(state));
    }
    setArg(ctx, 1, nextFuzz(state) % 8u ? nextFuzz(state) % 4u : nextFuzz(state) % 64u);
}

// a0 car, with the car's draw distances set at random. The race-over and demo bits of the main state are left as the
// game has them (a demo race has the demo bit), so the path with both clear is only reached by a real race's live calls.
void vfxRendered(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    if (!buildCar(ctx, state, scratch))
        return;
    _sObject &car = *static_cast<_sObject *>(guest_host(scratch));
    car.sDontRenderMe = static_cast<int16>(nextFuzz(state) % 2u ? 0 : nextFuzz(state));
    car.zDrawnDist = static_cast<uint16>(nextFuzz(state) % 3u ? nextFuzz(state) % 0x2000u : nextFuzz(state));
    car.zShortestDrawnDist = static_cast<uint16>(nextFuzz(state));
}

// a0 car, a1 an effect object (16-byte aligned, after the car's records).
void vfxCarAndEffect(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    if (!buildCar(ctx, state, scratch))
        return;
    setArg(ctx, 1, scratch + kEffect);
}

// a0 an effect that follows the car at scratch+0: its car pointer at +0x40 and at +0x650, with random life and counters.
void vfxEffectOnCar(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    if (!buildCar(ctx, state, scratch))
        return;
    uint8_t *effect = static_cast<uint8_t *>(guest_host(scratch + kEffect));
    for (uint32_t i = 0; i < kEffectSize; i += 4)
        *reinterpret_cast<uint32_t *>(effect + i) = nextFuzz(state);
    *reinterpret_cast<uint32_t *>(effect + 0x40) = scratch;
    *reinterpret_cast<uint32_t *>(effect + 0x650) = scratch;
    setArg(ctx, 0, scratch + kEffect);
}

// Arguments for the quad setters: the current quad (pGT4) is a record in scratch, then four random words.
void vfxQuad(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    restoreGlobals();
    g_quad.set(0x1f1030u, scratch + kEffect); // visFxVarG.pGT4
    for (int i = 0; i < 4; ++i)
        setArg(ctx, i, nextFuzz(state));
    for (int i = 0; i < 2; ++i)
    {
        uint32_t bits = nextFuzz(state);
        if (nextFuzz(state) % 4u == 0u)
            bits &= 0x807fffffu; // a denormal or zero
        setFloatArg(ctx, i, bits);
    }
}


// ---------------------------------------------------------------------------------------------------------------------
// The second batch: the effect handlers that follow a car or a scenery instance, the particle rings, the ribbon and
// sprite drawing, and the rain and glint tables.

namespace {

template <typename T>
T &mem(uint32_t address) { return *static_cast<T *>(guest_host(address)); }

// A scenery instance (a `_sTOBInst`) with its object and shape, at the given scratch offsets: the effect's tob pointer
// at +0x10 leads to the instance, whose first word leads to the object, whose word at +0x38 leads to the shape, whose
// word at +0x18 is the animation frame the sci-fi and animal effects look at. Positions are fixed-point (24.8).
void buildScenery(uint64_t &state, uint32_t scratch, uint32_t effect, uint32_t frameMax)
{
    mem<uint32_t>(effect + 0x10) = scratch + kTobInst;
    mem<uint32_t>(scratch + kTobInst) = scratch + kTob;
    mem<uint32_t>(scratch + kTob + 0x38) = scratch + kShape;
    mem<uint32_t>(scratch + kShape + 0x18) = rnd(state, frameMax);
    for (uint32_t offset : {4u, 8u, 12u})
        mem<int32_t>(scratch + kTobInst + offset) = static_cast<int32_t>(nextFuzz(state) % 4000000u) - 2000000;
    for (uint32_t offset : {0x1au, 0x20u, 0x26u}) // the matrix row the spout and steam read
        mem<int16_t>(scratch + kTobInst + offset) = static_cast<int16_t>(static_cast<int32_t>(nextFuzz(state) % 8001u) - 4000);
}

// A particle ring in an effect: mask, count, head and tail at +0x40, consistent with each other (now and then not), and a
// short life in each particle so some end every frame. lifeBytes is 2 or 4.
void buildRing(uint64_t &state, uint32_t effect, uint32_t mask, uint32_t particles, uint32_t stride, uint32_t lifeOffset, uint32_t lifeBytes)
{
    const uint32_t head = rnd(state, mask + 1), count = rnd(state, mask + 1);
    uint32_t counter = count;
    if (rnd(state, 8) == 0)
        counter = rnd(state, mask + 2);
    mem<uint8_t>(effect + 0x40) = static_cast<uint8_t>(mask);
    mem<uint8_t>(effect + 0x41) = static_cast<uint8_t>(counter);
    mem<uint8_t>(effect + 0x42) = static_cast<uint8_t>(head);
    mem<uint8_t>(effect + 0x43) = static_cast<uint8_t>((head + count) & mask);
    for (uint32_t i = 0; i <= mask; ++i)
    {
        const uint32_t life = rnd(state, 2) ? rnd(state, 4) : rnd(state, 0x50);
        if (lifeBytes == 2)
            mem<uint16_t>(particles + i * stride + lifeOffset) = static_cast<uint16_t>(life);
        else
            mem<uint32_t>(particles + i * stride + lifeOffset) = life;
    }
}

} // namespace

// a0 a scenery effect: the instance at +0x10 with a random position.
void vfxLights(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    restoreGlobals();
    buildScenery(state, scratch, scratch + kEffect, 0x40);
    setArg(ctx, 0, scratch + kEffect);
}

// a0 an effect following a car; the route finder's switch (track direction hints) is sometimes on.
void vfxRouteFinder(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    if (!buildCar(ctx, state, scratch))
        return;
    mem<uint32_t>(scratch + kEffect + 0x40) = scratch;
    g_trackHints.set(0x1ecbf0u, rnd(state, 2) ? 1u : 0u); // frontEndVarG.gameStateData.gameConfig.trackDirHints
    setArg(ctx, 0, scratch + kEffect);
}

// a0 the UFO burner: its car at +0x650, whose render object leads to a head with a brightness word that sometimes is
// the finished value.
void vfxUfoBurner(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    if (!buildCar(ctx, state, scratch))
        return;
    _sObject &car = *static_cast<_sObject *>(guest_host(scratch));
    mem<uint32_t>(scratch + kAuxA) = scratch + kAuxB; // the render object's head
    car.pRendObj = ptr32<_sRendObj>(scratch + kAuxA);
    mem<uint32_t>(scratch + kAuxB + 0x64) = rnd(state, 2) ? 0xFF000000u : nextFuzz(state);
    mem<uint32_t>(scratch + kEffect + 0x650) = scratch;
    mem<int32_t>(scratch + kEffect + 0x654) = static_cast<int32_t>(nextFuzz(state));
    mem<uint16_t>(scratch + kEffect + 0x10) = static_cast<uint16_t>(nextFuzz(state));
    setArg(ctx, 0, scratch + kEffect);
}

// a0 oil on a car: the car at +0x40 with a record at +0x40 of its own (the aerial effect) to twang, and counters in the
// range where they turn round.
void vfxOilCovered(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    if (!buildCar(ctx, state, scratch))
        return;
    _sObject &car = *static_cast<_sObject *>(guest_host(scratch));
    car.pVfx_Aerial = ptr32<_sVfx_Aerial>(scratch + kAuxA);
    const uint32_t effect = scratch + kEffect;
    mem<uint32_t>(effect + 0x40) = scratch;
    mem<uint16_t>(effect + 0x44) = static_cast<uint16_t>(nextFuzz(state));
    mem<int16_t>(effect + 0x46) = static_cast<int16_t>(0x30 + rnd(state, 0x30));
    mem<int16_t>(effect + 0x48) = static_cast<int16_t>(static_cast<int32_t>(rnd(state, 11)) - 5);
    mem<uint16_t>(effect + 0x10) = static_cast<uint16_t>(rnd(state, 4) ? rnd(state, 100) : nextFuzz(state));
    setArg(ctx, 0, effect);
}

// a0 oil on a barge: counters as above and a position at +0x14.
void vfxBargeOil(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    if (!buildCar(ctx, state, scratch)) // (the car only gives the stack its realistic state; the effect needs none)
        return;
    const uint32_t effect = scratch + kEffect;
    mem<uint16_t>(effect + 0x44) = static_cast<uint16_t>(nextFuzz(state));
    mem<int16_t>(effect + 0x46) = static_cast<int16_t>(0x30 + rnd(state, 0x30));
    mem<int16_t>(effect + 0x48) = static_cast<int16_t>(static_cast<int32_t>(rnd(state, 11)) - 5);
    mem<uint16_t>(effect + 0x10) = static_cast<uint16_t>(nextFuzz(state));
    setArg(ctx, 0, effect);
}

// a0 a finishing position, mostly the ones the text depends on; a1 a buffer for the text.
void vfxFinishText(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    restoreGlobals();
    static constexpr int kPositions[] = {0, 1, 2, 3, 4, 8, 21, 22, -1};
    const int pos = rnd(state, 4) ? kPositions[rnd(state, 9)] : static_cast<int16_t>(nextFuzz(state));
    setArg(ctx, 0, static_cast<uint32_t>(pos));
    setArg(ctx, 1, scratch + kAuxA + 16 * rnd(state, 8));
}

// a0 a scenery effect with a ring of particles, 0x14 bytes each from +0x44, the frame word at 0x11..0x27 half the time.
void vfxGargoyle(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    restoreGlobals();
    const uint32_t effect = scratch + kEffect;
    buildScenery(state, scratch, effect, 0x40);
    buildRing(state, effect, 15, effect + 0x44, 0x14, 0x10, 2);
    mem<uint16_t>(effect + 0x184) = static_cast<uint16_t>(rnd(state, 4) ? rnd(state, 0x34) : 0x33 + rnd(state, 4));
    setArg(ctx, 0, effect);
}

// The fountain and Moby Dick: a ring of 32 particles of 0x18 bytes, a 32-bit life at +0x10, and a tick counter at +0x347.
void vfxFountain(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    restoreGlobals();
    const uint32_t effect = scratch + kEffect;
    buildScenery(state, scratch, effect, 0x40);
    buildRing(state, effect, 31, effect + 0x44, 0x18, 0x10, 4);
    mem<uint8_t>(effect + 0x347) = static_cast<uint8_t>(nextFuzz(state));
    setArg(ctx, 0, effect);
}

void vfxMobyDick(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    restoreGlobals();
    const uint32_t effect = scratch + kEffect;
    buildScenery(state, scratch, effect, 0x100);
    buildRing(state, effect, rnd(state, 2) ? 31 : 15, effect + 0x44, 0x18, 0x10, 4);
    mem<uint8_t>(effect + 0x347) = static_cast<uint8_t>(nextFuzz(state));
    setArg(ctx, 0, effect);
}

// The sci-fi steam: a ring of 16 particles of 0x14 bytes, the vent's countdown at +0x186 (often about to hit zero) and
// its on/off count at +0x184.
void vfxSciFiSteam(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    restoreGlobals();
    const uint32_t effect = scratch + kEffect;
    buildScenery(state, scratch, effect, 0x40);
    buildRing(state, effect, 15, effect + 0x44, 0x14, 0x10, 2);
    mem<uint16_t>(effect + 0x184) = static_cast<uint16_t>(nextFuzz(state));
    mem<uint16_t>(effect + 0x186) = static_cast<uint16_t>(rnd(state, 3) ? 1 : rnd(state, 40));
    setArg(ctx, 0, effect);
}

// a0 the rain table's owner is a global: 16 slots (some taken) and the word after. a1 the instance to add.
namespace {

void tableSetup(R5900Context *ctx, uint64_t &state, uint32_t scratch, Claim<Table> &claim, uint32_t address)
{
    restoreGlobals();
    Table table{};
    const uint32_t kind = rnd(state, 4);
    for (uint32_t i = 0; i < 17; ++i)
        table.words[i] = (kind == 0) ? 0u : (kind == 1) ? nextFuzz(state) | 1u : (rnd(state, 3) ? nextFuzz(state) | 1u : 0u);
    if (kind == 3)
    {
        const uint32_t used = rnd(state, 17);
        for (uint32_t i = 0; i < 17; ++i)
            table.words[i] = i < used ? (nextFuzz(state) | 1u) : 0u;
    }
    claim.set(address, table);
    setArg(ctx, 0, scratch + kTobInst + 16 * rnd(state, 8));
}

} // namespace

void vfxRainTable(R5900Context *ctx, uint64_t &state, uint32_t scratch) { tableSetup(ctx, state, scratch, g_rain, 0x1f1e64u); }
void vfxGlintTable(R5900Context *ctx, uint64_t &state, uint32_t scratch) { tableSetup(ctx, state, scratch, g_glint, 0x1f36b0u); }

// a0 a milk float: the car at +0x40 whose two wheel objects (flag bytes at +0x18b) and player number it flags.
void vfxMilk(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    if (!buildCar(ctx, state, scratch))
        return;
    const uint32_t effect = scratch + kEffect;
    mem<uint32_t>(scratch + 0x178) = scratch + kAuxA;
    mem<uint32_t>(scratch + 0x17c) = rnd(state, 5) ? scratch + kAuxB : scratch + kAuxA;
    mem<uint8_t>(scratch + 0x45) = static_cast<uint8_t>(rnd(state, 8));
    mem<uint32_t>(effect + 0x40) = scratch;
    mem<uint16_t>(effect + 0x10) = static_cast<uint16_t>(rnd(state, 3) ? 1 + rnd(state, 3) : nextFuzz(state));
    setArg(ctx, 0, effect);
}

// a0 a lightning effect following the car at +0x78, with random history in the position slots.
void vfxLightning(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    if (!buildCar(ctx, state, scratch))
        return;
    const uint32_t effect = scratch + kEffect;
    mem<uint32_t>(effect + 0x78) = scratch;
    mem<uint16_t>(effect + 0x10) = static_cast<uint16_t>(nextFuzz(state));
    setArg(ctx, 0, effect);
}

// a0 the moon rover's blast: the car at +0x40 with a render object (halfwords at +0x1c, +0x22, +0x28), a definition
// that is sometimes type 0x25 (the rover) and physics that lead to the two floats it reads. Life is 0xffff on the first
// frame.
void vfxBlast(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    if (!buildCar(ctx, state, scratch))
        return;
    _sObject &car = *static_cast<_sObject *>(guest_host(scratch));
    _sObjectDef &def = *static_cast<_sObjectDef *>(guest_host(scratch + kDef));
    def = *car.pDef;
    def.type = static_cast<eObjectType>(rnd(state, 2) ? 0x25 : rnd(state, 0x30));
    car.pDef = ptr32<_sObjectDef>(scratch + kDef);
    car.pRendObj = ptr32<_sRendObj>(scratch + kAuxA);
    for (uint32_t offset : {0x1cu, 0x22u, 0x28u})
        mem<int16_t>(scratch + kAuxA + offset) = static_cast<int16_t>(static_cast<int32_t>(nextFuzz(state) % 8001u) - 4000);
    // physics + 0x1e0 leads to a record with floats at +0xa4 and +0x528
    mem<uint32_t>(car.pPhysics.address + 0x1e0) = scratch + kAuxB;
    mem<ee_float>(scratch + kAuxB + 0xa4) = ee_float(frnd(state, 5000.0f));
    mem<ee_float>(scratch + kAuxB + 0x528) = ee_float(rnd(state, 2) ? frnd(state) : 0.0f);
    const uint32_t effect = scratch + kEffect;
    mem<uint32_t>(effect + 0x40) = scratch;
    mem<uint16_t>(effect + 0x10) = static_cast<uint16_t>(rnd(state, 2) ? 0xffff : nextFuzz(state));
    for (uint32_t offset : {0x50u, 0x54u, 0x58u, 0x60u, 0x64u, 0x68u, 0x70u, 0x74u, 0x78u})
        mem<ee_float>(effect + offset) = ee_float(frnd(state));
    setArg(ctx, 0, effect);
}

// a0 a spark effect (ring of 0x14-byte particles from +0x48, mask 15 or 31 at +0x40, the car at +0x44), a1 the point.
void vfxSparks(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    if (!buildCar(ctx, state, scratch))
        return;
    _sObject &car = *static_cast<_sObject *>(guest_host(scratch));
    car.sDontRenderMe = static_cast<int16>(rnd(state, 2) ? 0 : nextFuzz(state));
    car.zDrawnDist = static_cast<uint16>(rnd(state, 4) ? rnd(state, 0x1800) : nextFuzz(state));
    const uint32_t effect = scratch + kEffect;
    const uint32_t mask = rnd(state, 2) ? 15 : 31;
    mem<uint32_t>(effect + 0x44) = scratch;
    mem<uint8_t>(effect + 0x40) = static_cast<uint8_t>(mask);
    mem<uint8_t>(effect + 0x41) = static_cast<uint8_t>(rnd(state, mask + 2));
    mem<uint8_t>(effect + 0x42) = static_cast<uint8_t>(rnd(state, mask + 1));
    mem<uint8_t>(effect + 0x43) = static_cast<uint8_t>(rnd(state, mask + 1));
    for (uint32_t axis = 0; axis < 3; ++axis)
        mem<int32_t>(scratch + kAuxA + 4 * axis) = static_cast<int32_t>(nextFuzz(state) % 2000000u) - 1000000;
    setArg(ctx, 0, effect);
    setArg(ctx, 1, scratch + kAuxA);
}

// a0 a ring header, a1 its particles: 0x18 bytes each (the simple kind is 0x14), the life near zero.
void vfxMover(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    restoreGlobals();
    const uint32_t mask = rnd(state, 3) == 0 ? 7 : rnd(state, 2) ? 15 : 31;
    const uint32_t head = rnd(state, mask + 1), count = rnd(state, mask + 1);
    mem<uint8_t>(scratch + kRing) = static_cast<uint8_t>(mask);
    mem<uint8_t>(scratch + kRing + 1) = static_cast<uint8_t>(count);
    mem<uint8_t>(scratch + kRing + 2) = static_cast<uint8_t>(head);
    mem<uint8_t>(scratch + kRing + 3) = static_cast<uint8_t>((head + count) & mask);
    // The particle layout is the same in the first 0x10 bytes; the full one has its life at +0x14, the simple one at +0x10.
    for (uint32_t i = 0; i <= mask; ++i)
    {
        const uint32_t life = rnd(state, 2) ? rnd(state, 4) : rnd(state, 0x50);
        mem<uint16_t>(scratch + kRingParticles + i * 0x18 + 0x14) = static_cast<uint16_t>(life);
        mem<uint16_t>(scratch + kRingParticles + 0x400 + i * 0x14 + 0x10) = static_cast<uint16_t>(life);
    }
    setArg(ctx, 0, scratch + kRing);
    setArg(ctx, 1, scratch + kRingParticles);
}

void vfxSimpleMover(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    vfxMover(ctx, state, scratch);
    setArg(ctx, 1, scratch + kRingParticles + 0x400);
}

namespace {

// Two ends as float vectors that are also (in their first eight bytes) three shorts, the texture record and a style.
void buildEnds(uint64_t &state, uint32_t scratch)
{
    for (uint32_t i = 0; i < 4; ++i)
    {
        for (uint32_t axis = 0; axis < 3; ++axis)
            mem<float>(scratch + kVectors + 16 * i + 4 * axis) = frnd(state);
        mem<float>(scratch + kVectors + 16 * i + 12) = 1.0f;
    }
    for (uint32_t i = 0; i < 4; ++i) // the same as shorts, in the second half
        for (uint32_t axis = 0; axis < 3; ++axis)
            mem<int16_t>(scratch + kVectors + 0x80 + 8 * i + 2 * axis) = static_cast<int16_t>(static_cast<int32_t>(nextFuzz(state) % 6001u) - 3000);
}

} // namespace

// The ribbon functions: a0 and a1 the ends, a2 a texture, a3 a style, f12 the width and f13 the depth offset. The quad is
// the effect's (pGT4); the vectors the first call left in visFxVarG are whatever the game has.
void vfxRibbon(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    restoreGlobals();
    g_quad.set(0x1f1030u, scratch + kEffect);
    buildEnds(state, scratch);
    const uint32_t a = rnd(state, 4), b = rnd(state, 4);
    setArg(ctx, 0, scratch + kVectors + 16 * a);
    setArg(ctx, 1, scratch + kVectors + 16 * b);
    setArg(ctx, 2, scratch + kTexture + 16 * rnd(state, 8));
    static constexpr uint32_t kStyles[] = {0, 2, 4, 0x10, 0x20, 0x12, 0x22, 0x24};
    setArg(ctx, 3, kStyles[rnd(state, 8)]);
    setFloatArg(ctx, 0, float_bits(frnd(state, 50.0f)));
    setFloatArg(ctx, 1, float_bits(frnd(state, 4.0f)));
}

// As above where the ends are shorts: the second half of the vector area.
void vfxRibbonShorts(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    vfxRibbon(ctx, state, scratch);
    setArg(ctx, 0, scratch + kVectors + 0x80 + 8 * rnd(state, 4));
    setArg(ctx, 1, scratch + kVectors + 0x80 + 8 * rnd(state, 4));
}

// VfxPutGT4vecs: four ends as shorts.
void vfxFourEnds(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    restoreGlobals();
    g_quad.set(0x1f1030u, scratch + kEffect);
    buildEnds(state, scratch);
    for (int i = 0; i < 4; ++i)
        setArg(ctx, i, scratch + kVectors + 0x80 + 8 * rnd(state, 4));
}

// The camera-facing sprite: the current sprite is a record in scratch (scale, position, texture, flags, angle, colour).
void buildSprite(uint64_t &state, uint32_t scratch)
{
    g_quad.set(0x1f1030u, scratch + kQuad);
    g_sprite.set(0x1f3720u, scratch + kSprite); // visFxVarG.sprite
    const uint32_t sprite = scratch + kSprite;
    mem<float>(sprite) = rnd(state, 8) ? frnd(state, 40.0f) : 0.0f;
    for (uint32_t axis = 0; axis < 3; ++axis)
        mem<float>(sprite + 0x10 + 4 * axis) = frnd(state, 500.0f);
    mem<uint32_t>(sprite + 0x20) = scratch + kTexture + 16 * rnd(state, 8);
    mem<uint8_t>(sprite + 0x24) = static_cast<uint8_t>(rnd(state, 2) ? rnd(state, 4) * 2 : nextFuzz(state));
    mem<uint8_t>(sprite + 0x25) = static_cast<uint8_t>(rnd(state, 2));
    mem<int16_t>(sprite + 0x26) = static_cast<int16_t>(nextFuzz(state));
    mem<uint32_t>(sprite + 0x28) = nextFuzz(state);
}

// f12 the depth offset.
void vfxSprite(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    restoreGlobals();
    buildSprite(state, scratch);
    setFloatArg(ctx, 0, float_bits(frnd(state, 4.0f)));
}

// a0 a surface normal (shorts), f12 the depth offset.
void vfxGroundSprite(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    restoreGlobals();
    buildSprite(state, scratch);
    for (uint32_t axis = 0; axis < 3; ++axis)
        mem<int16_t>(scratch + kNormal + 2 * axis) = static_cast<int16_t>(static_cast<int32_t>(nextFuzz(state) % 8193u) - 4096 + (axis == 1 ? 4096 : 0));
    setArg(ctx, 0, scratch + kNormal);
    setFloatArg(ctx, 0, float_bits(frnd(state, 4.0f)));
}

// ---------------------------------------------------------------------------------------------------------------------
// The third batch: effect lists on the heap, the handlers and renderers of the smoke, steam and fire effects, and the
// level set-up and tear-down.

namespace {

constexpr uint32_t kRes = 0x7e00u;    // a stand-in for the level's effects resource: word i is vfxTex[i]
constexpr uint32_t kHandle = 0x7f00u; // a handle word for heap allocations

// The renderers' state: the current quad and sprite (see buildSprite), and the effects resource, whose textures all point
// at texture records in scratch.
void renderState(uint64_t &state, uint32_t scratch)
{
    buildSprite(state, scratch);
    g_res.set(0x1f106cu, scratch + kRes);
    for (uint32_t i = 0; i < 0x1c; ++i)
        mem<uint32_t>(scratch + kRes + 4 * i) = scratch + kTexture + 16 * rnd(state, 8);
}

// A scenery effect with a ring of particles: the instance behind it (buildScenery) and the ring (buildRing).
uint32_t sceneryEffect(uint64_t &state, uint32_t scratch, uint32_t mask, uint32_t stride, uint32_t lifeOffset, uint32_t frameMax = 0x40)
{
    restoreGlobals();
    const uint32_t effect = scratch + kEffect;
    buildScenery(state, scratch, effect, frameMax);
    buildRing(state, effect, mask, effect + 0x44, stride, lifeOffset, 2);
    return effect;
}

// The car's draw distance, mostly near enough to be drawn.
void nearCar(uint64_t &state, uint32_t scratch)
{
    _sObject &car = *static_cast<_sObject *>(guest_host(scratch));
    car.sDontRenderMe = static_cast<int16>(rnd(state, 2) ? 0 : nextFuzz(state));
    car.zDrawnDist = static_cast<uint16>(rnd(state, 5) ? rnd(state, 0x1800) : nextFuzz(state));
    car.zShortestDrawnDist = static_cast<uint16>(rnd(state, 5) ? rnd(state, 0x1800) : nextFuzz(state));
}

// Makes the car at scratch+0 one with an aerial: a node, a render object and a chain of sections, all in scratch.
void aerialCar(uint64_t &state, uint32_t scratch)
{
    _sObject &car = *static_cast<_sObject *>(guest_host(scratch));
    car.pAerialNode = ptr32<_sNode>(scratch + kAuxA);
    car.pRendObj = ptr32<_sRendObj>(scratch + kAuxB);
    car.pVfx_Aerial = ptr32<_sVfx_Aerial>(scratch + kAuxC);
    for (uint32_t offset = 0x40; offset < 0xc0; offset += 4)
        mem<float>(scratch + kAuxC + offset) = frnd(state, 800.0f);
    for (uint32_t axis = 0; axis < 3; ++axis)
        mem<int32_t>(scratch + kAuxB + 4 + 4 * axis) = static_cast<int32_t>(nextFuzz(state) % 400000u) - 200000;
}

// A block from the game's heap, with its address returned (0 if the heap is full).
uint32_t heapBlock(uint32_t scratch, uint32_t size)
{
    mem<uint32_t>(scratch + kHandle) = 0;
    if (!MemBlockAllocLo(ptr32<memHandle>(scratch + kHandle), size))
        return 0;
    return mem<uint32_t>(scratch + kHandle);
}

// A list of `count` effects of `size` bytes on the heap, linked as the game links them (Next at +0, Prev at +4), with the
// handler at +8, the life at +0x10 and the type at +0x12 given by the callbacks. The head and footer are returned.
struct FxList
{
    uint32_t head = 0, foot = 0;
    uint32_t items[6] = {};
    uint32_t count = 0;
};

FxList buildFxList(uint64_t &state, uint32_t scratch, uint32_t count, uint32_t size, uint32_t handler, bool types)
{
    FxList list;
    for (uint32_t i = 0; i < count; ++i)
    {
        const uint32_t block = heapBlock(scratch, size);
        if (block == 0)
            break;
        for (uint32_t offset = 0; offset < 0x40; offset += 4)
            mem<uint32_t>(block + offset) = nextFuzz(state);
        mem<uint32_t>(block) = 0;
        mem<uint32_t>(block + 4) = list.foot;
        mem<uint32_t>(block + 8) = handler;
        if (types)
            mem<uint16_t>(block + 0x12) = static_cast<uint16_t>(rnd(state, 3) == 0 ? (rnd(state, 2) ? 0x23 : 0x29) : rnd(state, 0x40));
        if (list.foot)
            mem<uint32_t>(list.foot) = block;
        else
            list.head = block;
        list.foot = block;
        list.items[list.count++] = block;
    }
    return list;
}

} // namespace

// a0..a3 a position and a type: only the types that make an effect from the position alone (butterflies, the explosion,
// the oil slick), now and then one that makes nothing; the effect list holds nothing, or one or two effects.
void vfxAddNewPlace(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    static constexpr int32_t kTypes[] = {0x29, 9, 0xa, 0x29, 0x31, 0x7f};
    const FxList list = buildFxList(state, scratch, rnd(state, 3), 0x60, 0, false);
    visFxClass &fx = G(visFxVarG);
    fx.vfxHeader = ptr32<_sVfx_Object>(list.head);
    fx.vfxFooter = ptr32<_sVfx_Object>(list.foot);
    setArg(ctx, 0, static_cast<uint32_t>(static_cast<int32_t>(rnd(state, 6000)) - 3000));
    setArg(ctx, 1, static_cast<uint32_t>(static_cast<int32_t>(rnd(state, 6000)) - 3000));
    setArg(ctx, 2, static_cast<uint32_t>(static_cast<int32_t>(rnd(state, 6000)) - 3000));
    setArg(ctx, 3, static_cast<uint32_t>(kTypes[rnd(state, 6)]));
}

// a0 a car with an aerial, a1 a section of its chain (0..6, now and then beyond).
void vfxAerial(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    if (!buildCar(ctx, state, scratch))
        return;
    aerialCar(state, scratch);
    setArg(ctx, 1, rnd(state, 8) ? rnd(state, 7) : rnd(state, 9));
}

// a0 a spark effect: its car at +0x44, a ring from +0x48 with a count (often zero), particles of 0x14 bytes.
void vfxSparkHandler(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    if (!buildCar(ctx, state, scratch))
        return;
    const uint32_t effect = scratch + kEffect;
    buildRing(state, effect, rnd(state, 2) ? 15 : 31, effect + 0x48, 0x14, 0x10, 2);
    mem<uint8_t>(effect + 0x41) = static_cast<uint8_t>(rnd(state, 3) ? mem<uint8_t>(effect + 0x41) : 0);
    mem<uint32_t>(effect + 0x44) = scratch;
    setArg(ctx, 0, effect);
}

// The rocket smoke: a ring of 32 particles of 0x14 bytes, a hook position at +0x14, and a life mostly above 0x1f.
void vfxRocketSmoke(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    restoreGlobals();
    const uint32_t effect = scratch + kEffect;
    buildRing(state, effect, 31, effect + 0x44, 0x14, 0x10, 2);
    mem<uint8_t>(effect + 0x41) = static_cast<uint8_t>(rnd(state, 6) == 0 ? 0x1f + rnd(state, 3) : rnd(state, 0x20));
    mem<uint16_t>(effect + 0x10) = static_cast<uint16_t>(rnd(state, 4) ? 0x20 + rnd(state, 100) : rnd(state, 0x22));
    setArg(ctx, 0, effect);
}

// The explosion: a ring of 16 particles of 0x18 bytes, a life that is 200 (the first frame) a third of the time, and the
// centre as three shorts at +0x1c4.
void vfxExplosion(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    restoreGlobals();
    const uint32_t effect = scratch + kEffect;
    buildRing(state, effect, 15, effect + 0x44, 0x18, 0x14, 2);
    mem<uint16_t>(effect + 0x10) = static_cast<uint16_t>(rnd(state, 3) == 0 ? 200 : rnd(state, 2) ? rnd(state, 200) : nextFuzz(state));
    for (uint32_t axis = 0; axis < 3; ++axis)
        mem<int16_t>(effect + 0x1c4 + 2 * axis) = static_cast<int16_t>(static_cast<int32_t>(nextFuzz(state) % 8001u) - 4000);
    setArg(ctx, 0, effect);
}

void vfxExplosionRenderer(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    vfxExplosion(ctx, state, scratch);
    const uint32_t effect = scratch + kEffect;
    renderState(state, scratch);
    for (uint32_t i = 0; i < 16; ++i)
        mem<uint16_t>(effect + 0x44 + i * 0x18 + 0x14) = static_cast<uint16_t>(rnd(state, 8) ? rnd(state, 80) : nextFuzz(state));
    setArg(ctx, 0, effect);
}

// The car smoke: the car at +0x1c4, a ring of 16 particles of 0x18 bytes, a life that is often a multiple of four above 0x20.
void vfxCarSmoke(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    if (!buildCar(ctx, state, scratch))
        return;
    const uint32_t effect = scratch + kEffect;
    buildRing(state, effect, 15, effect + 0x44, 0x18, 0x14, 2);
    mem<uint32_t>(effect + 0x1c4) = scratch;
    mem<uint16_t>(effect + 0x10) = static_cast<uint16_t>(rnd(state, 2) ? 0x21 + 4 * rnd(state, 30) : rnd(state, 3) ? rnd(state, 0x30) : nextFuzz(state));
    setArg(ctx, 0, effect);
}

void vfxCarSmokeRenderer(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    vfxCarSmoke(ctx, state, scratch);
    if (!levelHasCars())
        return;
    nearCar(state, scratch);
    const uint32_t effect = scratch + kEffect;
    renderState(state, scratch);
    for (uint32_t i = 0; i < 16; ++i)
        mem<uint16_t>(effect + 0x44 + i * 0x18 + 0x14) = static_cast<uint16_t>(rnd(state, 8) ? rnd(state, 80) : nextFuzz(state));
}

// The spark renderer: the car at +0x44, a ring from +0x48 with a count that is rarely zero.
void vfxSparksRenderer(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    vfxSparkHandler(ctx, state, scratch);
    if (!levelHasCars())
        return;
    nearCar(state, scratch);
    const uint32_t effect = scratch + kEffect;
    renderState(state, scratch);
    if (mem<uint8_t>(effect + 0x41) == 0 && rnd(state, 4))
        mem<uint8_t>(effect + 0x41) = 1;
    for (uint32_t i = 0; i < 32; ++i)
        mem<uint16_t>(effect + 0x48 + i * 0x14 + 0x10) = static_cast<uint16_t>(rnd(state, 8) ? rnd(state, 80) : nextFuzz(state));
}

// The take-terror effect: a ring of 16 or 32 particles of 0x14 bytes, a count often below the mask (so particles start),
// the car at +0x2cc and a counter at +0x2d0, and a life mostly in the range where it starts particles.
void vfxTakeTerror(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    if (!buildCar(ctx, state, scratch))
        return;
    const uint32_t effect = scratch + kEffect;
    const uint32_t mask = rnd(state, 2) ? 15 : 31;
    buildRing(state, effect, mask, effect + 0x44, 0x14, 0x10, 2);
    mem<uint8_t>(effect + 0x41) = static_cast<uint8_t>(rnd(state, 4) ? rnd(state, mask) : mask + rnd(state, 2));
    mem<uint32_t>(effect + 0x2cc) = scratch;
    mem<uint16_t>(effect + 0x10) = static_cast<uint16_t>(rnd(state, 4) ? 0xd + rnd(state, 0x28) : nextFuzz(state));
    setArg(ctx, 0, effect);
}

// The moon rover's beam: the car at +0x40 (its integer physics hold the rotation), the vectors at +0x50, +0x60 and +0x70,
// and a life that is 0xffff now and then.
void vfxBlastMoonroverRenderer(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    if (!buildCar(ctx, state, scratch))
        return;
    const uint32_t effect = scratch + kEffect;
    renderState(state, scratch);
    mem<uint32_t>(effect + 0x40) = scratch;
    mem<uint16_t>(effect + 0x10) = static_cast<uint16_t>(rnd(state, 8) == 0 ? 0xffff : nextFuzz(state));
    for (uint32_t offset : {0x50u, 0x54u, 0x58u, 0x60u, 0x64u, 0x68u, 0x70u, 0x74u, 0x78u})
        mem<float>(effect + offset) = frnd(state, 400.0f);
    setArg(ctx, 0, effect);
}

// The blast lightning: two cars (both the one in scratch, now and then far from the camera), each with an aerial.
void vfxBlastLightningRenderer(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    if (!buildCar(ctx, state, scratch))
        return;
    aerialCar(state, scratch);
    nearCar(state, scratch);
    const uint32_t effect = scratch + kEffect;
    mem<uint32_t>(effect + 0x40) = scratch;
    mem<uint32_t>(effect + 0x44) = scratch;
    setArg(ctx, 0, effect);
}

// --- VISFX2 and VISFX3 -------------------------------------------------------------------------------------------------

void vfxSteamingHandler(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    const uint32_t effect = sceneryEffect(state, scratch, 15, 0x14, 0x10);
    mem<uint16_t>(effect + 0x184) = static_cast<uint16_t>(rnd(state, 0x100));
    setArg(ctx, 0, effect);
}

void vfxSteamingRenderer(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    const uint32_t effect = sceneryEffect(state, scratch, 15, 0x14, 0x10);
    renderState(state, scratch);
    for (uint32_t i = 0; i < 16; ++i)
        mem<uint16_t>(effect + 0x44 + i * 0x14 + 0x10) = static_cast<uint16_t>(rnd(state, 8) ? rnd(state, 0x50) : nextFuzz(state));
    setArg(ctx, 0, effect);
}

// The pipe steam: the countdown at +0x186 is 1 (it flips) a third of the time, the on/off count at +0x184 random.
void vfxPipeSteam(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    const uint32_t effect = sceneryEffect(state, scratch, 15, 0x14, 0x10);
    mem<uint16_t>(effect + 0x184) = static_cast<uint16_t>(nextFuzz(state));
    mem<uint16_t>(effect + 0x186) = static_cast<uint16_t>(rnd(state, 3) == 0 ? 1 : rnd(state, 40));
    setArg(ctx, 0, effect);
}

void vfxMonkeySmoke(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    const uint32_t effect = sceneryEffect(state, scratch, 15, 0x14, 0x10);
    setArg(ctx, 0, effect);
}

// The volcano and smoking scenery: a tick at +0x184 that starts a particle every eighth frame, and the scenery's level
// byte at +0x18b either side of 0x50.
void vfxBigVolcano(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    const uint32_t effect = sceneryEffect(state, scratch, 15, 0x14, 0x10);
    mem<uint16_t>(effect + 0x184) = static_cast<uint16_t>(rnd(state, 2) ? 7 + 8 * rnd(state, 100) : nextFuzz(state));
    mem<uint8_t>(effect + 0x18b) = static_cast<uint8_t>(rnd(state, 2) ? 0x4c + rnd(state, 8) : nextFuzz(state));
    setArg(ctx, 0, effect);
}

void vfxSmoking(R5900Context *ctx, uint64_t &state, uint32_t scratch) { vfxBigVolcano(ctx, state, scratch); }

// The jungle fire: a ring of 32 particles of 0x18 bytes (the life at +0x14), a tick at +0x344 and a colour byte at +0x34b.
void vfxJungleFire(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    const uint32_t effect = sceneryEffect(state, scratch, 31, 0x18, 0x14);
    mem<uint16_t>(effect + 0x344) = static_cast<uint16_t>(nextFuzz(state));
    mem<uint8_t>(effect + 0x34b) = static_cast<uint8_t>(nextFuzz(state));
    setArg(ctx, 0, effect);
}

void vfxScifiRocket(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    const uint32_t effect = sceneryEffect(state, scratch, 15, 0x18, 0x14);
    mem<uint8_t>(effect + 0x34b) = static_cast<uint8_t>(nextFuzz(state));
    setArg(ctx, 0, effect);
}

void vfxFeathers(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    const uint32_t effect = sceneryEffect(state, scratch, 15, 0x14, 0x10);
    renderState(state, scratch);
    setArg(ctx, 0, effect);
}

void vfxBees(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    const uint32_t effect = sceneryEffect(state, scratch, 63, 0x14, 0x10);
    renderState(state, scratch);
    setArg(ctx, 0, effect);
}

void vfxFlies(R5900Context *ctx, uint64_t &state, uint32_t scratch) { vfxBees(ctx, state, scratch); }

// The gulls: drawn only when bit 0 of the instance's flags (+0x16) is set.
void vfxGulls(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    const uint32_t effect = sceneryEffect(state, scratch, 15, 0x14, 0x10);
    renderState(state, scratch);
    mem<uint16_t>(scratch + kTobInst + 0x16) = static_cast<uint16_t>(rnd(state, 4) ? 1 | (nextFuzz(state) & 0xfffe) : nextFuzz(state) & 0xfffe);
    setArg(ctx, 0, effect);
}

void vfxTumbleWeedRenderer(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    const uint32_t effect = sceneryEffect(state, scratch, 15, 0x14, 0x10);
    renderState(state, scratch);
    setArg(ctx, 0, effect);
}

void vfxCloudCarousel(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    if (!buildCar(ctx, state, scratch))
        return;
    renderState(state, scratch);
    // The car's render object, a record of our own: the game's would be rewritten under the test once the race is over.
    _sObject &car = *static_cast<_sObject *>(guest_host(scratch));
    car.pRendObj = ptr32<_sRendObj>(scratch + kAuxB);
    for (uint32_t axis = 0; axis < 3; ++axis)
        mem<int32_t>(scratch + kAuxB + 4 + 4 * axis) = static_cast<int32_t>(nextFuzz(state) % 400000u) - 200000;
    g_time.set(guest_addr(&G(mainVarG).time), nextFuzz(state) >> rnd(state, 12));
    mem<uint32_t>(scratch + 0x34) = scratch + kTexture + 16 * rnd(state, 8); // the car's shadow texture
}

void vfxTargetCar(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    vfxCloudCarousel(ctx, state, scratch);
}

// The skull's fart: the car at +0x1c4, a ring of 16 particles of 0x18 bytes, a life mostly above 0x1f.
void vfxSkullFartHandler(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    if (!buildCar(ctx, state, scratch))
        return;
    const uint32_t effect = scratch + kEffect;
    buildRing(state, effect, 15, effect + 0x44, 0x18, 0x14, 2);
    mem<uint32_t>(effect + 0x1c4) = scratch;
    mem<uint16_t>(effect + 0x10) = static_cast<uint16_t>(rnd(state, 4) ? 0x20 + rnd(state, 100) : rnd(state, 0x22));
    setArg(ctx, 0, effect);
}

void vfxSkullFartRenderer(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    vfxSkullFartHandler(ctx, state, scratch);
    if (!levelHasCars())
        return;
    nearCar(state, scratch);
    renderState(state, scratch);
}

// The right-the-car effect: life 0x10 (the first frame) half the time, else 1 or 2 (about to end) or random.
void vfxCarRight(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    vfxSkullFartHandler(ctx, state, scratch);
    const uint32_t effect = scratch + kEffect;
    mem<uint16_t>(effect + 0x10) = static_cast<uint16_t>(rnd(state, 2) ? 0x10 : rnd(state, 3) == 0 ? 1 + rnd(state, 2) : nextFuzz(state));
}

// The oil on a car: the car at +0x40 and two counters.
void vfxOilCoveredRenderer(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    if (!buildCar(ctx, state, scratch))
        return;
    nearCar(state, scratch);
    renderState(state, scratch);
    const uint32_t effect = scratch + kEffect;
    mem<uint32_t>(effect + 0x40) = scratch;
    mem<uint16_t>(effect + 0x44) = static_cast<uint16_t>(nextFuzz(state));
    mem<int16_t>(effect + 0x46) = static_cast<int16_t>(0x30 + rnd(state, 0x30));
    setArg(ctx, 0, effect);
}

// The spray: the car at +0x44, half the time a boat (so the wheel and sphere paths both run), a ring from +0x48 with
// particles of 0x1c bytes (the life at +0x18) and a count that is rarely zero.
void vfxSprayRenderer(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    if (!buildCar(ctx, state, scratch))
        return;
    nearCar(state, scratch);
    renderState(state, scratch);
    _sObject &car = *static_cast<_sObject *>(guest_host(scratch));
    _sObjectDef &def = *static_cast<_sObjectDef *>(guest_host(scratch + kDef));
    def = *car.pDef;
    def.type = static_cast<eObjectType>(rnd(state, 2) ? OBJECT_TYPE_BADDRC_BOAT + rnd(state, 4) : rnd(state, 8));
    car.pDef = ptr32<_sObjectDef>(scratch + kDef);
    const uint32_t effect = scratch + kEffect;
    buildRing(state, effect, 15, effect + 0x48, 0x1c, 0x18, 2);
    if (mem<uint8_t>(effect + 0x41) == 0 && rnd(state, 4))
        mem<uint8_t>(effect + 0x41) = 1;
    for (uint32_t i = 0; i < 16; ++i)
        mem<uint16_t>(effect + 0x48 + i * 0x1c + 0x18) = static_cast<uint16_t>(rnd(state, 8) ? rnd(state, 0x60) : nextFuzz(state));
    mem<uint32_t>(effect + 0x44) = scratch;
    setArg(ctx, 0, effect);
}

// --- the scenery effect list and the level's set-up and tear-down ----------------------------------------------------

// a0 a scenery instance, a1 its flags, a2 a size, a3 a type: mostly the 29 kinds, now and then beyond. The list is empty or
// holds one to three effects.
void vfxTSFXAdd(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    restoreGlobals();
    const FxList list = buildFxList(state, scratch, rnd(state, 4), 0x60, 0, false);
    visFxClass &fx = G(visFxVarG);
    fx.vfxTSFXHeader = ptr32<_sVfx_TSFX>(list.head);
    fx.vfxTSFXFooter = ptr32<_sVfx_TSFX>(list.foot);
    setArg(ctx, 0, scratch + kTobInst);
    setArg(ctx, 1, nextFuzz(state) & 0xffffff);
    setArg(ctx, 2, nextFuzz(state) & 0xff);
    setArg(ctx, 3, rnd(state, 8) ? rnd(state, 0x1d) : rnd(state, 0x24));
}

// The set-up and tear-down of the lists: the effect list or the scenery list holds some effects on the heap (the tear-down
// frees them all).
void vfxLvlTerm(R5900Context *, uint64_t &state, uint32_t scratch)
{
    restoreGlobals();
    const FxList list = buildFxList(state, scratch, rnd(state, 5), 0x60, 0, false);
    G(visFxVarG).vfxHeader = ptr32<_sVfx_Object>(list.head);
    G(visFxVarG).vfxFooter = ptr32<_sVfx_Object>(list.foot);
}

void vfxTSFXLvlTerm(R5900Context *, uint64_t &state, uint32_t scratch)
{
    restoreGlobals();
    const FxList list = buildFxList(state, scratch, rnd(state, 5), 0x60, 0, false);
    G(visFxVarG).vfxTSFXHeader = ptr32<_sVfx_TSFX>(list.head);
    G(visFxVarG).vfxTSFXFooter = ptr32<_sVfx_TSFX>(list.foot);
}

// The level start: the level number is the front end's (0x62) half the time, with the front end active or not.
void vfxLvlInit(R5900Context *, uint64_t &state, uint32_t scratch)
{
    restoreGlobals();
    const FxList list = buildFxList(state, scratch, rnd(state, 3), 0x60, 0, false);
    G(visFxVarG).vfxHeader = ptr32<_sVfx_Object>(list.head);
    G(visFxVarG).vfxFooter = ptr32<_sVfx_Object>(list.foot);
    G(mainVarG).level = rnd(state, 2) ? 0x62 : rnd(state, 30);
    G(frontEndVarG).active = static_cast<uint8_t>(rnd(state, 2) ? 5 : rnd(state, 8));
    G(mainVarG).status = rnd(state, 2) ? MAIN_RACE_RESULT : static_cast<eMain>(rnd(state, 0x80));
    G(visFxVarG).iResultTrigger = static_cast<int32>(nextFuzz(state));
}

void vfxTSFXLvlInit(R5900Context *, uint64_t &state, uint32_t scratch) { vfxTSFXLvlTerm(nullptr, state, scratch); }

// The scenery update: a list of one to three effects whose handler is the tumbleweed's (a call that needs no scenery
// instance) or none, and a time.
void vfxTSFXUpdate(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    restoreGlobals();
    const uint32_t handler = rnd(state, 3) ? 0x179020u : 0u;
    const FxList list = buildFxList(state, scratch, 1 + rnd(state, 3), 0x60, handler, false);
    G(visFxVarG).vfxTSFXHeader = ptr32<_sVfx_TSFX>(list.head);
    G(visFxVarG).vfxTSFXFooter = ptr32<_sVfx_TSFX>(list.foot);
    for (uint32_t i = 0; i < list.count; ++i)
        if (rnd(state, 4) == 0)
            mem<uint32_t>(list.items[i] + 8) = 0;
    setArg(ctx, 1, nextFuzz(state));
}

// a0 an effect from a list of two to five on the heap (VfxDelete); the head or the footer of it now and then.
void vfxDelete(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    restoreGlobals();
    const FxList list = buildFxList(state, scratch, 1 + rnd(state, 5), 0x60, 0, true);
    G(visFxVarG).vfxHeader = ptr32<_sVfx_Object>(list.head);
    G(visFxVarG).vfxFooter = ptr32<_sVfx_Object>(list.foot);
    setArg(ctx, 0, list.items[rnd(state, list.count ? list.count : 1)]);
}

// VfxHandOverFXDelete: a list of up to five effects of random types, some of them 0x23 and 0x29, which are kept.
void vfxHandOver(R5900Context *, uint64_t &state, uint32_t scratch)
{
    restoreGlobals();
    const FxList list = buildFxList(state, scratch, rnd(state, 6), 0x60, 0, true);
    G(visFxVarG).vfxHeader = ptr32<_sVfx_Object>(list.head);
    G(visFxVarG).vfxFooter = ptr32<_sVfx_Object>(list.foot);
    G(carListVarG).alternativeRendObj = nextFuzz(state);
}

} // namespace rc::fuzz
