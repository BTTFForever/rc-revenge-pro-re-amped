// Inputs for the random test of the small VISFX functions (RC_SHADOW_FUZZ). They use one of the race's real cars,
// copied into the scratch area by the route-node setup, and build effect objects and wheel indices around it.

#include "ai_fuzz.h"
#include "ee_math.h"

namespace rc::fuzz {

namespace {

constexpr uint32_t kEffect = 0x4000u;   // an effect object placed after the car's records (0x0..0x2300)
constexpr uint32_t kEffectSize = 0x700u;
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

Claim<uint32_t> g_quad, g_sprite, g_trackHints;
Claim<Table> g_rain, g_glint;

void restoreGlobals()
{
    g_quad.restore();
    g_sprite.restore();
    g_trackHints.restore();
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
    g_quad.set(0x1f1030u, scratch + kEffect);
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

} // namespace rc::fuzz
