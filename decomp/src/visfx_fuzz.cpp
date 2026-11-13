// Inputs for the random test of the small VISFX functions (RC_SHADOW_FUZZ). They use one of the race's real cars,
// copied into the scratch area by the route-node setup, and build effect objects and wheel indices around it.

#include "ai_fuzz.h"
#include "ee_math.h"
#include "visfx.h"

namespace rc {
uint32_t &randState(); // ai.cpp: newlib's rand state
}

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

void scrambleStack(R5900Context *ctx, uint64_t &state);
void seedRand(uint64_t &state);

// Every setup sets its first argument last-ish, so this is also where the stack is scrambled (see scrambleStack) for the
// setups that do not do it themselves. The words come from a generator of their own: only the original and the
// decompiled function have to agree on them, and they both see what is written here.
void setArg(R5900Context *ctx, int index, uint32_t value)
{
    static uint64_t stackState = 0x5eed5eed12345678ull;
    if (index == 0)
    {
        scrambleStack(ctx, stackState);
        seedRand(stackState);
    }
    SET_GPR_S32(ctx, 4 + index, static_cast<int32_t>(value));
}
void setFloatArg(R5900Context *ctx, int index, uint32_t bits) { std::memcpy(&ctx->f[12 + index], &bits, 4); }

// Fills the stack below the stack pointer (where the function under test builds its frame) with random words. A few
// functions store a local vector whose fourth lane they never wrote (sceVu0ITOF0Vector's, a short's pad), so the result
// holds a word of the stack that an earlier call left behind: with a stack that is all zeroes or all the same, a wrong
// frame size or offset gives the right answer by luck. The original and the decompiled function both start from this.
void scrambleStack(R5900Context *ctx, uint64_t &state)
{
    const uint32_t sp = getRegU32(ctx, 29);
    for (uint32_t offset = 0x400; offset >= 4; offset -= 4)
        *static_cast<uint32_t *>(guest_host(sp - offset)) = nextFuzz(state);
}

// Gives the game's rand a random state. A whole-RAM input (RC_DECOMP_FN_SETUP_HEAP) starts from RAM as the step began,
// rand's state included, so without this every input of such a function would draw the same random numbers.
void seedRand(uint64_t &state) { randState() = nextFuzz(state); }

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
Claim<uint32_t> g_camera, g_levelRecord; // cameraVarG.pCamera, pLvlG
Claim<uint8_t> g_level;                   // mainVarG.level
Claim<uint8_t> g_split;                   // mainVarG.splitScreen
Claim<uint32_t> g_screen;                 // mainVarG.iWhichSplitScreen
struct GlintBlock { uint8_t bytes[0x1800]; }; // visFxVarG.glint: 128 glints of 0x30 bytes
struct FourFloats { float v[4]; };
struct EightFloats { float v[8]; };
Claim<GlintBlock> g_glints;
Claim<FourFloats> g_cameraVector; // visFxVarG.ps2CameraTrans
Claim<uint32_t> g_playerState;    // one player's u8PlayerStates word (sNew, sOld)
Claim<EightFloats> g_lastSpots;   // where the glints last started, a vector per screen

void restoreGlobals()
{
    g_quad.restore();
    g_sprite.restore();
    g_trackHints.restore();
    g_res.restore();
    g_time.restore();
    g_rain.restore();
    g_glint.restore();
    g_camera.restore();
    g_levelRecord.restore();
    g_level.restore();
    g_split.restore();
    g_screen.restore();
    g_glints.restore();
    g_cameraVector.restore();
    g_lastSpots.restore();
    g_playerState.restore();
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

// A car at scratch+0 (aiCar copies one of the race's cars there with its physics, as the next lines check). When no race is
// loaded (or the AI nodes are gone: the demo race has ended) aiCar builds nothing and the scratch area holds random words,
// whose pointers lead outside the 32 MB of RAM, where the original wraps the address and a native function does not (the
// race's object list also holds things that are not cars, which aiCar copies as if they were: a definition pointer that is
// null or not in RAM marks them). The setups then get a car of zeroes with sane pointers instead: type 0, zero physics and integer state (RC_FUZZ_FAKECAR=1
// makes them always do, to test that path while a race is running).
bool buildCar(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    restoreGlobals();
    aiCar(ctx, state, scratch);
    _sObject &car = *static_cast<_sObject *>(guest_host(scratch));
    static const bool forceFake = std::getenv("RC_FUZZ_FAKECAR") != nullptr; // test the zero car while a race is running
    const bool realCar = levelHasCars() && car.pPhysics.address == scratch + 0x1000u && car.pDef.address >= 0x100000u && car.pDef.address < 0x1ffff00u;
    if (!forceFake && realCar)
        return true;
    std::memset(guest_host(scratch), 0, 0x2300);
    std::memset(guest_host(scratch + kDef), 0, 0x200);
    car.pDef = ptr32<_sObjectDef>(scratch + kDef);
    car.pPhysics = ptr32<_sFPhysLibCar>(scratch + 0x1000u);
    static_cast<_sFPhysLibCar *>(guest_host(scratch + 0x1000u))->pInteger = ptr32<_sOPhysLibIntegerCarState>(scratch + 0x2000u);
    setArg(ctx, 0, scratch);
    return true;
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

// ---------------------------------------------------------------------------------------------------------------------
// The fourth batch: the rest of the effect handlers and renderers.

// The heat ray aerial and the car fire: a ring of 32 particles of 0x18 bytes, the car (a copy in scratch) at +0x344, the
// car's own counter at +0x198 now and then zero, and a life mostly above 11.
void vfxCarHeatRayAerial(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    if (!buildCar(ctx, state, scratch))
        return;
    const uint32_t effect = scratch + kEffect;
    buildRing(state, effect, 31, effect + 0x44, 0x18, 0x14, 2);
    mem<uint32_t>(effect + 0x344) = scratch;
    mem<int16_t>(scratch + 0x198) = static_cast<int16_t>(rnd(state, 2) ? 0 : nextFuzz(state));
    mem<int8_t>(scratch + 0x19c) = static_cast<int8_t>(rnd(state, 3) ? 0 : nextFuzz(state));
    mem<uint16_t>(effect + 0x10) = static_cast<uint16_t>(rnd(state, 4) ? 0xc + rnd(state, 40) : rnd(state, 0x10));
    mem<uint8_t>(effect + 0x41) = static_cast<uint8_t>(rnd(state, 4) ? rnd(state, 32) : 31);
    setArg(ctx, 0, effect);
}

// The kaboom and the firework: a ring of 64 particles of 0x14 bytes, life 64 (the first frame) a third of the time.
void vfxKaboom(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    restoreGlobals();
    const uint32_t effect = scratch + kEffect;
    buildRing(state, effect, 63, effect + 0x44, 0x14, 0x10, 2);
    mem<uint16_t>(effect + 0x10) = static_cast<uint16_t>(rnd(state, 3) == 0 ? 0x40 : rnd(state, 2) ? rnd(state, 0x40) : nextFuzz(state));
    for (uint32_t axis = 0; axis < 3; ++axis)
        mem<int16_t>(effect + 0x544 + 2 * axis) = static_cast<int16_t>(static_cast<int32_t>(nextFuzz(state) % 8001u) - 4000);
    for (uint32_t i = 0; i < 64; ++i)
        for (uint32_t offset : {8u, 10u, 12u})
            mem<int16_t>(effect + 0x44 + i * 0x14 + offset) = static_cast<int16_t>(nextFuzz(state));
    setArg(ctx, 0, effect);
}

// The invincibility halo: the car (a copy) at +0x250 with a render object and a shape in scratch (the shape's bounding
// box is shorts at +0x68..+0x72), life 0xffff a third of the time.
void vfxInvincible(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    if (!buildCar(ctx, state, scratch))
        return;
    _sObject &car = *static_cast<_sObject *>(guest_host(scratch));
    car.pRendObj = ptr32<_sRendObj>(scratch + kAuxB);
    mem<uint32_t>(scratch + kAuxB) = scratch + kAuxC;
    for (uint32_t offset = 0x68; offset <= 0x72; offset += 2)
        mem<int16_t>(scratch + kAuxC + offset) = static_cast<int16_t>(static_cast<int32_t>(nextFuzz(state) % 801u) - 400);
    const uint32_t effect = scratch + kEffect;
    mem<uint32_t>(effect + 0x250) = scratch;
    mem<uint16_t>(effect + 0x10) = static_cast<uint16_t>(rnd(state, 3) == 0 ? 0xffff : nextFuzz(state));
    setArg(ctx, 0, effect);
}

// The arm cannon smoke: a ring of 16 particles of 0x14 bytes, the hook (shorts at +0x14) and the last hook position
// (+0x184), equal about half the time, and a life around 0x10.
void vfxArmCannon(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    restoreGlobals();
    const uint32_t effect = scratch + kEffect;
    buildRing(state, effect, 15, effect + 0x44, 0x14, 0x10, 2);
    mem<uint8_t>(effect + 0x41) = static_cast<uint8_t>(rnd(state, 4) ? rnd(state, 16) : 0xf + rnd(state, 2));
    mem<uint16_t>(effect + 0x10) = static_cast<uint16_t>(rnd(state, 4) ? 0x10 + rnd(state, 20) : rnd(state, 0x12));
    for (uint32_t axis = 0; axis < 3; ++axis)
    {
        mem<int16_t>(effect + 0x14 + 2 * axis) = static_cast<int16_t>(static_cast<int32_t>(nextFuzz(state) % 8001u) - 4000);
        mem<int16_t>(effect + 0x184 + 2 * axis) = static_cast<int16_t>(rnd(state, 3) ? mem<int16_t>(effect + 0x14 + 2 * axis) : nextFuzz(state));
    }
    setArg(ctx, 0, effect);
}


// The car fire's renderer: the car (a copy) at +0x344 and a ring of 32 particles of 0x18 bytes with random lives.
void vfxCarFireRenderer(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    vfxCarHeatRayAerial(ctx, state, scratch);
    const uint32_t effect = scratch + kEffect;
    renderState(state, scratch);
    for (uint32_t i = 0; i < 32; ++i)
        mem<uint16_t>(effect + 0x44 + i * 0x18 + 0x14) = static_cast<uint16_t>(rnd(state, 8) ? rnd(state, 0x20) : nextFuzz(state));
}

// The arm cannon's renderer: the particle ring of vfxArmCannon, and the level's own cannon-ball shape if it has one (the
// renderer rewrites its matrix and position: the harness puts them back), else a made-up one whose version is not the
// 2.0 or 3.0 that draws it (the shape renderers are not run on made-up shapes).
void vfxArmCannonRenderer(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    vfxArmCannon(ctx, state, scratch);
    const uint32_t realRes = G(visFxVarG).pVfxRes.address;
    const uint32_t realShape = realRes ? mem<uint32_t>(realRes + 0x84) : 0;
    const uint32_t effect = scratch + kEffect;
    renderState(state, scratch);
    for (uint32_t i = 0; i < 16; ++i)
        mem<uint16_t>(effect + 0x44 + i * 0x14 + 0x10) = static_cast<uint16_t>(rnd(state, 8) ? rnd(state, 0x40) : nextFuzz(state));
    if (realShape)
    {
        mem<uint32_t>(scratch + kRes + 0x84) = realShape;
        return;
    }
    mem<uint32_t>(scratch + kRes + 0x84) = scratch + kAuxA;
    mem<float>(scratch + kAuxA) = rnd(state, 2) ? 0.0f : frnd(state, 5.0f);
    if (mem<float>(scratch + kAuxA) == 2.0f || mem<float>(scratch + kAuxA) == 3.0f)
        mem<float>(scratch + kAuxA) = 1.0f;
}

// The barge oil: the hook (shorts at +0x14) within a few thousand units of the camera, mostly on every axis.
void vfxBargeOilRenderer(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    restoreGlobals();
    const uint32_t effect = scratch + kEffect;
    renderState(state, scratch);
    const visFxClass &fx = G(visFxVarG);
    const int32_t cam[3] = {fx.vCamPos.x >> 8, fx.vCamPos.y >> 8, fx.vCamPos.z >> 8};
    for (uint32_t axis = 0; axis < 3; ++axis)
        mem<int16_t>(effect + 0x14 + 2 * axis) = static_cast<int16_t>(rnd(state, 6) ? cam[axis] + static_cast<int32_t>(rnd(state, 16000)) - 8000 : static_cast<int32_t>(nextFuzz(state)));
    mem<uint16_t>(effect + 0x44) = static_cast<uint16_t>(nextFuzz(state));
    mem<int16_t>(effect + 0x46) = static_cast<int16_t>(rnd(state, 2) ? rnd(state, 200) : nextFuzz(state));
    setArg(ctx, 0, effect);
}

// The kaboom renderer: the burst of vfxKaboom with particle lives from zero up to over 30.
void vfxKaboomRenderer(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    vfxKaboom(ctx, state, scratch);
    const uint32_t effect = scratch + kEffect;
    renderState(state, scratch);
    for (uint32_t i = 0; i < 64; ++i)
    {
        mem<uint16_t>(effect + 0x44 + i * 0x14 + 0x10) = static_cast<uint16_t>(rnd(state, 8) ? rnd(state, 0x40) : nextFuzz(state));
        for (uint32_t offset : {0u, 2u, 4u})
            mem<int16_t>(effect + 0x44 + i * 0x14 + offset) = static_cast<int16_t>(nextFuzz(state));
    }
}


// The wheel dust: the car (a copy) at +0x1c4 with a wheel number and a material index at +0x1c8, a ring of 16 particles of
// 0x18 bytes with lives that reach both ends of the fade.
void vfxWheelDust(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    if (!buildCar(ctx, state, scratch))
        return;
    renderState(state, scratch);
    const uint32_t effect = scratch + kEffect;
    buildRing(state, effect, 15, effect + 0x44, 0x18, 0x14, 2);
    mem<uint32_t>(effect + 0x1c4) = scratch;
    mem<uint16_t>(effect + 0x1c8) = static_cast<uint16_t>(rnd(state, 8) ? rnd(state, 4) : rnd(state, 64));
    mem<uint16_t>(effect + 0x1ca) = static_cast<uint16_t>(rnd(state, 8) ? rnd(state, 0x30) : nextFuzz(state));
    for (uint32_t i = 0; i < 16; ++i)
        mem<uint16_t>(effect + 0x44 + i * 0x18 + 0x14) = static_cast<uint16_t>(rnd(state, 8) ? rnd(state, 0x20) : nextFuzz(state));
    // The wheel position record is read from the car's physics: the wheel number set above is mostly 0 to 3.
    setArg(ctx, 0, effect);
}

// The heat ray damage renderer: the car at +0x344, a ring of 32 particles, lives from 0 to 0x20 and a glow count.
void vfxHeatRayDamage(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    vfxCarHeatRayAerial(ctx, state, scratch);
    nearCar(state, scratch);
    renderState(state, scratch);
    const uint32_t effect = scratch + kEffect;
    for (uint32_t i = 0; i < 32; ++i)
    {
        mem<uint16_t>(effect + 0x44 + i * 0x18 + 0x14) = static_cast<uint16_t>(rnd(state, 8) ? rnd(state, 0x20) : nextFuzz(state));
        mem<uint8_t>(effect + 0x44 + i * 0x18 + 0x16) = static_cast<uint8_t>(rnd(state, 3) ? 0 : rnd(state, 4));
    }
    setArg(ctx, 0, effect);
}

// The firework renderer: the burst of vfxKaboom, a colour at +0x54c and particle lives from zero up to over 32.
void vfxFireworkRenderer(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    vfxKaboomRenderer(ctx, state, scratch);
    const uint32_t effect = scratch + kEffect;
    for (uint32_t i = 0; i < 3; ++i)
        mem<uint8_t>(effect + 0x54c + i) = static_cast<uint8_t>(rnd(state, 4) ? 0x40 + rnd(state, 0x40) : nextFuzz(state));
}

// The car rocket: the car (a copy with an aerial) at +0x350, a ring of 16 particles of 0x30 bytes from +0x50, and the
// level's own rocket shape (the effects resource's, which the renderer rewrites: the harness puts it back).
void vfxCarRocket(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    if (!buildCar(ctx, state, scratch))
        return;
    const uint32_t realRes = G(visFxVarG).pVfxRes.address;
    const uint32_t shape = realRes ? mem<uint32_t>(realRes + 0x88) : 0;
    aerialCar(state, scratch);
    nearCar(state, scratch);
    renderState(state, scratch);
    const uint32_t effect = scratch + kEffect;
    mem<uint32_t>(scratch + kRes + 0x88) = shape;
    if (!shape)
    {
        _sObject &car = *static_cast<_sObject *>(guest_host(scratch));
        car.sDontRenderMe = 1;
        car.zDrawnDist = 0x7fff;
    }
    mem<uint32_t>(effect + 0x350) = scratch;
    buildRing(state, effect, 15, effect + 0x50, 0x30, 0x10, 2);
    for (uint32_t i = 0; i < 16; ++i)
    {
        for (uint32_t axis = 0; axis < 3; ++axis)
            mem<float>(effect + 0x50 + i * 0x30 + 4 * axis) = frnd(state, 60.0f);
        mem<float>(effect + 0x50 + i * 0x30 + 0xc) = 0.0f;
        mem<uint32_t>(effect + 0x50 + i * 0x30 + 0x24) = rnd(state, 4) ? rnd(state, 0x20) : nextFuzz(state);
    }
    setArg(ctx, 0, effect);
}


// --- the fifth batch: VISFX2 renderers ------------------------------------------------------------------------------

namespace {

// Gives each particle of a ring (count particles of `stride` bytes) a random position (shorts at +0, +2, +4: wide enough
// that sums and shifts wrap now and then) and a life of 0 to `lifeMax` (a 16-bit one at lifeOffset), now and then wild.
void scatterParticles(uint64_t &state, uint32_t first, uint32_t count, uint32_t stride, uint32_t lifeOffset, uint32_t lifeBytes, uint32_t lifeMax)
{
    for (uint32_t i = 0; i < count; ++i)
    {
        const uint32_t p = first + i * stride;
        for (uint32_t offset : {0u, 2u, 4u})
            mem<int16_t>(p + offset) = static_cast<int16_t>(rnd(state, 4) ? static_cast<int32_t>(rnd(state, 2000)) - 1000 : static_cast<int32_t>(nextFuzz(state)));
        const uint32_t life = rnd(state, 8) ? rnd(state, lifeMax) : nextFuzz(state);
        if (lifeBytes == 2)
            mem<uint16_t>(p + lifeOffset) = static_cast<uint16_t>(life);
        else
            mem<uint32_t>(p + lifeOffset) = life;
        mem<uint16_t>(p + 0x12) = static_cast<uint16_t>(nextFuzz(state));
        if (lifeOffset == 0x14) // the car-style particle: an angle at +0x10 and a size byte at +0x16
        {
            mem<uint16_t>(p + 0x10) = static_cast<uint16_t>(nextFuzz(state));
            mem<uint8_t>(p + 0x16) = static_cast<uint8_t>(nextFuzz(state));
        }
    }
}

// A scenery effect for a renderer: ring, scenery, the renderers' state, and three colour bytes at colourOffset.
uint32_t sceneryRenderer(uint64_t &state, uint32_t scratch, uint32_t mask, uint32_t stride, uint32_t lifeOffset, uint32_t lifeBytes,
                         uint32_t colourOffset, uint32_t lifeMax)
{
    const uint32_t effect = sceneryEffect(state, scratch, mask, stride, lifeOffset);
    if (lifeBytes == 4)
        buildRing(state, effect, mask, effect + 0x44, stride, lifeOffset, 4);
    renderState(state, scratch);
    for (uint32_t i = 0; i < 4; ++i)
        mem<uint8_t>(effect + colourOffset + i) = static_cast<uint8_t>(rnd(state, 4) ? rnd(state, 0x80) : nextFuzz(state));
    scatterParticles(state, effect + 0x44, mask + 1, stride, lifeOffset, lifeBytes, lifeMax);
    return effect;
}

} // namespace

void vfxMonkeySmokeRenderer(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    setArg(ctx, 0, sceneryRenderer(state, scratch, 15, 0x14, 0x10, 2, 0x188, 0x40));
}

void vfxBigVolcanoRenderer(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    setArg(ctx, 0, sceneryRenderer(state, scratch, 15, 0x14, 0x10, 2, 0x188, 0x80));
}

void vfxFountainRenderer(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    setArg(ctx, 0, sceneryRenderer(state, scratch, 31, 0x18, 0x10, 4, 0x344, 0x60));
}

void vfxMobyDickRenderer(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    setArg(ctx, 0, sceneryRenderer(state, scratch, rnd(state, 2) ? 31 : 15, 0x18, 0x10, 4, 0x344, 0x60));
}

void vfxPipeSteamRenderer(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    setArg(ctx, 0, sceneryRenderer(state, scratch, 15, 0x14, 0x10, 2, 0x188, 0x40));
}

void vfxSciFiBubblesRenderer(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    setArg(ctx, 0, sceneryRenderer(state, scratch, 15, 0x14, 0x10, 2, 0x188, 0x60));
}


void vfxSciFiSteamRenderer(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    setArg(ctx, 0, sceneryRenderer(state, scratch, 15, 0x14, 0x10, 2, 0x188, 0x40));
}

void vfxSmokingRenderer(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    setArg(ctx, 0, sceneryRenderer(state, scratch, 15, 0x14, 0x10, 2, 0x188, 0x80));
}

void vfxGargoyleRenderer(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    setArg(ctx, 0, sceneryRenderer(state, scratch, 15, 0x14, 0x10, 2, 0x188, 0x40));
}

// The right-the-car dust: the car (a copy) at +0x1c4 and a ring of 16 particles of 0x18 bytes (life at +0x14).
void vfxCarRightRenderer(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    if (!buildCar(ctx, state, scratch))
        return;
    renderState(state, scratch);
    const uint32_t effect = scratch + kEffect;
    buildRing(state, effect, 15, effect + 0x44, 0x18, 0x14, 2);
    mem<uint32_t>(effect + 0x1c4) = scratch;
    scatterParticles(state, effect + 0x44, 16, 0x18, 0x14, 2, 0x20);
    setArg(ctx, 0, effect);
}

// The sci-fi rocket: the scenery's object chain leads to a vector of floats (the rocket's place in the model), now and
// then so large that converting it to an integer saturates. The particles are 0x18 bytes (life at +0x14, size at +0x16).
void vfxScifiRocketRenderer(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    const uint32_t effect = sceneryRenderer(state, scratch, 15, 0x18, 0x14, 2, 0x344, 0x20);
    mem<uint32_t>(scratch + kTob + 0x3c) = scratch + kVectors;
    mem<uint32_t>(scratch + kVectors + 8) = scratch + kVectors + 0x10;
    mem<uint32_t>(scratch + kVectors + 0x10) = scratch + kVectors + 0x20;
    for (uint32_t axis = 0; axis < 3; ++axis)
        mem<float>(scratch + kVectors + 0x20 + 4 * axis) = rnd(state, 8) ? frnd(state, 40000.0f) : frnd(state, 1.0e11f);
    setArg(ctx, 0, effect);
}

void vfxScifiRocketOldRenderer(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    setArg(ctx, 0, sceneryRenderer(state, scratch, 15, 0x18, 0x14, 2, 0x344, 0x20));
}


// The cartoon train's smoke: a ring of 16 particles of 0x14 bytes, a counter at +0x194 (every fourth value starts a puff),
// the last position at +0x184 (the scenery's own one about half the time) and the last movement at +0x18c.
void vfxCartoonTrainHandler(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    const uint32_t effect = sceneryEffect(state, scratch, 15, 0x14, 0x10);
    mem<uint32_t>(effect + 0x194) = rnd(state, 2) ? 3 + 4 * rnd(state, 100) : nextFuzz(state);
    const uint32_t tob = scratch + kTobInst;
    for (uint32_t axis = 0; axis < 3; ++axis)
    {
        const int16_t here = static_cast<int16_t>(mem<int32_t>(tob + 4 + 4 * axis) >> 8);
        mem<int16_t>(effect + 0x184 + 2 * axis) = static_cast<int16_t>(rnd(state, 2) ? here : static_cast<int16_t>(nextFuzz(state)));
        mem<int16_t>(effect + 0x18c + 2 * axis) = static_cast<int16_t>(rnd(state, 2) ? rnd(state, 40) : nextFuzz(state));
    }
    setArg(ctx, 0, effect);
}

// The cartoon train's renderer: the scenery's matrix (in the instance at +0x18) is a real rotation matrix of shorts.
void vfxCartoonTrainRenderer(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    const uint32_t effect = sceneryRenderer(state, scratch, 15, 0x14, 0x10, 2, 0x188, 0x20);
    const uint32_t tob = scratch + kTobInst;
    for (uint32_t offset = 0x18; offset < 0x2a; offset += 2)
        mem<int16_t>(tob + offset) = static_cast<int16_t>(static_cast<int32_t>(rnd(state, 8193)) - 4096);
    setArg(ctx, 0, effect);
}


// The finish position puffs: the car (a copy) at +0x44, a ring of 16 particles of 0x14 bytes from +0x48 (mask 15), and a
// life that is the first frame (0x43f) a third of the time, and else around 0x418 where the speed changes over.
void vfxFinishPosition(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    if (!buildCar(ctx, state, scratch))
        return;
    const uint32_t effect = scratch + kEffect;
    buildRing(state, effect, 15, effect + 0x48, 0x14, 0x10, 2);
    mem<uint32_t>(effect + 0x44) = scratch;
    mem<uint16_t>(effect + 0x10) = static_cast<uint16_t>(rnd(state, 3) == 0 ? 0x43f : rnd(state, 2) ? 0x410 + rnd(state, 0x20) : nextFuzz(state));
    for (uint32_t offset : {0x188u, 0x18au, 0x18cu, 0x198u, 0x19au, 0x19cu, 0x1a0u})
        mem<int16_t>(effect + offset) = static_cast<int16_t>(rnd(state, 2) ? static_cast<int32_t>(rnd(state, 600)) - 300 : static_cast<int32_t>(nextFuzz(state)));
    mem<uint16_t>(effect + 0x1a2) = static_cast<uint16_t>(rnd(state, 3) ? rnd(state, 0x300) : nextFuzz(state));
    setArg(ctx, 0, effect);
}


// The skull wave: a ring of 16 points of 0x30 bytes (floats at +0) from +0x50, the centre (shorts at +0x654), and the
// life. The ring's mask is never zero (the original's divide would trap).
void vfxSkullWave(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    restoreGlobals();
    const uint32_t effect = scratch + kEffect;
    renderState(state, scratch);
    buildRing(state, effect, 15, effect + 0x50, 0x30, 0x10, 2);
    mem<uint16_t>(effect + 0x10) = static_cast<uint16_t>(rnd(state, 4) ? rnd(state, 0x40) : nextFuzz(state));
    for (uint32_t i = 0; i < 16; ++i)
        for (uint32_t lane = 0; lane < 4; ++lane)
            mem<float>(effect + 0x50 + i * 0x30 + 4 * lane) = frnd(state, 200.0f);
    for (uint32_t axis = 0; axis < 3; ++axis)
        mem<int16_t>(effect + 0x654 + 2 * axis) = static_cast<int16_t>(static_cast<int32_t>(rnd(state, 8001)) - 4000);
    setArg(ctx, 0, effect);
}

// --- the sixth batch: more VISFX2 handlers and renderers --------------------------------------------------------------

// The UFO burner's renderer: its car at +0x650 (near enough to be drawn), a counter at +0x654, and now and then the level
// that hides cars that ask not to be drawn.
void vfxUfoBurnerRenderer(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    if (!buildCar(ctx, state, scratch))
        return;
    nearCar(state, scratch);
    renderState(state, scratch);
    g_level.set(0x1edc4au, rnd(state, 2) ? 0x62 : 0x10);
    const uint32_t effect = scratch + kEffect;
    mem<uint32_t>(effect + 0x650) = scratch;
    mem<uint32_t>(effect + 0x654) = nextFuzz(state);
    setArg(ctx, 0, effect);
}

// The lava spit: a ring of 16 particles of 0x18 bytes (life at +0x14), the lava colour at +0x348.
void vfxLavaSpit(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    setArg(ctx, 0, sceneryRenderer(state, scratch, 15, 0x18, 0x14, 2, 0x348, 0x20));
}

// The feathers' handler: the scenery effect of the renderer, with a tick at +0x184 whose low bits come up now and then.
void vfxFeathersHandler(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    const uint32_t effect = sceneryEffect(state, scratch, 15, 0x14, 0x10);
    mem<uint16_t>(effect + 0x184) = static_cast<uint16_t>(rnd(state, 2) ? 3 + 4 * rnd(state, 100) : nextFuzz(state));
    scatterParticles(state, effect + 0x44, 16, 0x14, 0x10, 2, 0x40);
    for (uint32_t i = 0; i < 16; ++i)
        mem<int16_t>(effect + 0x44 + i * 0x14 + 2) = static_cast<int16_t>(rnd(state, 4) ? static_cast<int32_t>(rnd(state, 0x1200)) - 0x900 : static_cast<int32_t>(nextFuzz(state)));
    setArg(ctx, 0, effect);
}

// The route finder's renderer: the car at +0x40, the camera looking at it (most of the time), the hint switch on.
void vfxRouteFinderRenderer(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    if (!buildCar(ctx, state, scratch))
        return;
    renderState(state, scratch);
    g_trackHints.set(0x1ecbf0u, rnd(state, 8) ? 1u : 0u);
    g_camera.set(0x1eab20u, scratch + kAuxA);
    mem<uint32_t>(scratch + kAuxA + 0xac) = rnd(state, 8) ? scratch : scratch + 0x40;
    const uint32_t effect = scratch + kEffect;
    mem<uint32_t>(effect + 0x40) = scratch;
    mem<uint32_t>(effect + 0x48) = rnd(state, 4) ? rnd(state, 0x40) : nextFuzz(state);
    setArg(ctx, 0, effect);
}

// The clock's effect (handler and renderer): its car at +0x44, a position and speed (floats from +0xf0), a ring of 16
// particles of 0x14 bytes from +0x48, and a life that is often the first frame (0x80) or near the end.
void ttClock(uint64_t &state, uint32_t scratch)
{
    const uint32_t effect = scratch + kEffect;
    buildRing(state, effect, 15, effect + 0x48, 0x14, 0x10, 2);
    mem<uint32_t>(effect + 0x44) = scratch;
    const uint32_t kind = rnd(state, 4);
    mem<uint16_t>(effect + 0x10) = static_cast<uint16_t>(kind == 0 ? 0x80 : kind == 1 ? rnd(state, 0x50) : kind == 2 ? rnd(state, 0x100) : nextFuzz(state));
    for (uint32_t i = 0; i < 3; ++i)
    {
        mem<float>(effect + 0xf0 + 4 * i) = frnd(state, 800.0f);
        mem<float>(effect + 0x100 + 4 * i) = frnd(state, 2.0f);
    }
    mem<uint16_t>(effect + 0x110) = static_cast<uint16_t>(nextFuzz(state));
    mem<uint16_t>(effect + 0x112) = static_cast<uint16_t>(rnd(state, 2) ? rnd(state, 0x300) : nextFuzz(state));
    scatterParticles(state, effect + 0x48, 16, 0x14, 0x10, 2, 0x20);
}

void vfxTTclockHandler(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    if (!buildCar(ctx, state, scratch))
        return;
    g_camera.set(0x1eab20u, scratch + kAuxA);
    mem<uint32_t>(scratch + kAuxA + 0xac) = rnd(state, 2) ? scratch : scratch + 0x40;
    ttClock(state, scratch);
    setArg(ctx, 0, scratch + kEffect);
}

void vfxTTclockRenderer(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    if (!buildCar(ctx, state, scratch))
        return;
    nearCar(state, scratch);
    renderState(state, scratch);
    mem<uint8_t>(scratch + 0x44) = static_cast<uint8_t>(rnd(state, 4) ? rnd(state, 2) : 2); // playerType: 2 skips the clock
    g_levelRecord.set(0x327e80u, scratch + kAuxC);
    mem<uint32_t>(scratch + kAuxC + 0x40) = scratch + kAuxB;
    mem<uint32_t>(scratch + kAuxB + 0xd4) = scratch + kTexture + 16 * rnd(state, 8);
    ttClock(state, scratch);
    setArg(ctx, 0, scratch + kEffect);
}

void vfxCannon(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    setArg(ctx, 0, sceneryRenderer(state, scratch, 15, 0x14, 0x10, 2, 0x188, 0x50));
}

void vfxJungleFireRenderer(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    setArg(ctx, 0, sceneryRenderer(state, scratch, 31, 0x18, 0x14, 2, 0x344, 0x40));
}

// The swarm handlers (bees and flies): the first-frame flag at +0x544 is clear half the time.
void vfxBeesHandler(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    const uint32_t effect = sceneryEffect(state, scratch, 63, 0x14, 0x10);
    mem<uint16_t>(effect + 0x544) = static_cast<uint16_t>(rnd(state, 2) ? 0 : nextFuzz(state));
    scatterParticles(state, effect + 0x44, 64, 0x14, 0x10, 2, 0x40);
    setArg(ctx, 0, effect);
}

// The rocket car: the car-rocket effect of vfxCarRocket (a car at +0x350, ring of 0x30-byte particles), on the rocket
// level half the time.
void vfxRocketCar(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    if (!buildCar(ctx, state, scratch))
        return;
    nearCar(state, scratch);
    renderState(state, scratch);
    g_level.set(0x1edc4au, rnd(state, 2) ? 0x62 : 0x10);
    const uint32_t effect = scratch + kEffect;
    mem<uint32_t>(effect + 0x350) = scratch;
    buildRing(state, effect, 15, effect + 0x50, 0x30, 0x10, 2);
    if (rnd(state, 2))
        mem<uint8_t>(effect + 0x41) = static_cast<uint8_t>(rnd(state, 8));
    for (uint32_t i = 0; i < 16; ++i)
    {
        for (uint32_t axis = 0; axis < 3; ++axis)
            mem<float>(effect + 0x50 + i * 0x30 + 4 * axis) = frnd(state, 60.0f);
        mem<float>(effect + 0x50 + i * 0x30 + 0xc) = 0.0f;
        mem<uint32_t>(effect + 0x50 + i * 0x30 + 0x24) = rnd(state, 4) ? rnd(state, 0x20) : nextFuzz(state);
    }
    setArg(ctx, 0, effect);
}

// --- the sixth batch: more VISFX handlers and renderers ---------------------------------------------------------------

// The car's two skid trails (pointers at +0x178 and +0x17c, a flag byte at +0x18b in each) are records in scratch, or none.
void skidTrails(uint64_t &state, uint32_t scratch)
{
    mem<uint32_t>(scratch + 0x178) = rnd(state, 4) ? scratch + kAuxA : 0;
    mem<uint32_t>(scratch + 0x17c) = scratch + kAuxB;
    mem<uint8_t>(scratch + kAuxA + 0x18b) = static_cast<uint8_t>(nextFuzz(state));
    mem<uint8_t>(scratch + kAuxB + 0x18b) = static_cast<uint8_t>(nextFuzz(state));
}

// Makes the car at scratch+0 sometimes a boat, so the effects that look at the wheels' sphere records take both paths.
void maybeBoat(uint64_t &state, uint32_t scratch)
{
    _sObject &car = *static_cast<_sObject *>(guest_host(scratch));
    _sObjectDef &def = *static_cast<_sObjectDef *>(guest_host(scratch + kDef));
    def = *car.pDef;
    def.type = static_cast<eObjectType>(rnd(state, 2) ? OBJECT_TYPE_BADDRC_BOAT + rnd(state, 4) : rnd(state, 8));
    car.pDef = ptr32<_sObjectDef>(scratch + kDef);
    for (_sFPhysLibWheel &w : car.pPhysics->wheel)
        w.flags = static_cast<uint16>(nextFuzz(state));
}

void vfxBomb(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    restoreGlobals();
    const uint32_t effect = scratch + kEffect;
    mem<uint16_t>(effect + 0x10) = static_cast<uint16_t>(rnd(state, 2) ? 0xffff : nextFuzz(state));
    for (uint32_t axis = 0; axis < 4; ++axis)
        mem<int16_t>(effect + 0x14 + 2 * axis) = static_cast<int16_t>(static_cast<int32_t>(nextFuzz(state) % 20001u) - 10000);
    mem<int32_t>(effect + 0x2d0) = static_cast<int32_t>(rnd(state, 3) == 0 ? 0xff : rnd(state, 3) ? rnd(state, 0x120) : nextFuzz(state));
    mem<uint16_t>(effect + 0x2c6) = static_cast<uint16_t>(rnd(state, 2) ? rnd(state, 1000) : nextFuzz(state));
    setArg(ctx, 0, effect);
}

// The wheel dust handler: the renderer's effect (car at +0x1c4, wheel at +0x1c8, a ring of 16 particles) with a life that is
// often near the 16 that matters, and the car now and then a boat.
void vfxWheelDustHandler(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    if (!buildCar(ctx, state, scratch))
        return;
    const uint32_t effect = scratch + kEffect;
    buildRing(state, effect, 15, effect + 0x44, 0x18, 0x14, 2);
    maybeBoat(state, scratch);
    mem<uint32_t>(effect + 0x1c4) = scratch;
    mem<uint16_t>(effect + 0x1c8) = static_cast<uint16_t>(rnd(state, 8) ? rnd(state, 4) : rnd(state, 64));
    mem<uint16_t>(effect + 0x10) = static_cast<uint16_t>(rnd(state, 3) ? 0x8 + rnd(state, 0x20) : nextFuzz(state));
    mem<uint8_t>(effect + 0x41) = static_cast<uint8_t>(rnd(state, 3) ? rnd(state, 16) : 15);
    setArg(ctx, 0, effect);
}

// The speed-up handlers: the car at +0x644, a ring of 16 particles of 0x18 bytes and a life that crosses the 12 where the
// puffs stop and the 0 where the trails are let go.
void vfxSpeedUpHandler(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    if (!buildCar(ctx, state, scratch))
        return;
    const uint32_t effect = scratch + kEffect;
    buildRing(state, effect, 15, effect + 0x44, 0x18, 0x14, 2);
    skidTrails(state, scratch);
    mem<uint32_t>(effect + 0x644) = scratch;
    const uint32_t kind = rnd(state, 4);
    mem<uint16_t>(effect + 0x10) = static_cast<uint16_t>(kind == 0 ? 1 : kind == 1 ? 0xc + rnd(state, 3) : kind == 2 ? rnd(state, 0x20) : nextFuzz(state));
    mem<uint8_t>(effect + 0x41) = static_cast<uint8_t>(rnd(state, 3) ? rnd(state, 16) : 15);
    setArg(ctx, 0, effect);
}

// The heat ray damage handler: the heat ray effect (car at +0x344, ring of 32) with skid trails, and a life that crosses
// the 12 and the 0.
void vfxHeatRayDamageHandler(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    vfxCarHeatRayAerial(ctx, state, scratch);
    skidTrails(state, scratch);
    const uint32_t effect = scratch + kEffect;
    const uint32_t kind = rnd(state, 4);
    mem<uint16_t>(effect + 0x10) = static_cast<uint16_t>(kind == 0 ? 1 : kind == 1 ? 0xc + rnd(state, 3) : kind == 2 ? rnd(state, 0x20) : 0xc + rnd(state, 40));
    setArg(ctx, 0, effect);
}

// A renderer that follows a car (at carOffset) and draws a ring of particles of `stride` bytes with the life at lifeOffset.
uint32_t carRingRenderer(R5900Context *ctx, uint64_t &state, uint32_t scratch, uint32_t carOffset, uint32_t stride, uint32_t lifeOffset,
                         uint32_t lifeMax)
{
    if (!buildCar(ctx, state, scratch))
        return 0;
    nearCar(state, scratch);
    renderState(state, scratch);
    const uint32_t effect = scratch + kEffect;
    buildRing(state, effect, 15, effect + 0x44, stride, lifeOffset, 2);
    scatterParticles(state, effect + 0x44, 16, stride, lifeOffset, 2, lifeMax);
    mem<uint32_t>(effect + carOffset) = scratch;
    setArg(ctx, 0, effect);
    return effect;
}

void vfxOilDump(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    carRingRenderer(ctx, state, scratch, 0x1c4, 0x18, 0x14, 6);
}

void vfxSpeedUpRenderer(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    carRingRenderer(ctx, state, scratch, 0x644, 0x18, 0x14, 0x20);
}

// The car respawn and change-over: the car at +0x2c4, ring of 16 particles of 0x14 bytes (life +0x10), a spin at +0x2c8
// (and +0x2cc) and a life that sweeps the glow's range; the car is drawn (not hidden) mostly.
void vfxCarRespawn(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    const uint32_t effect = carRingRenderer(ctx, state, scratch, 0x2c4, 0x14, 0x10, 0x40);
    if (!effect)
        return;
    mem<int16_t>(effect + 0x2c8) = static_cast<int16_t>(rnd(state, 4) ? static_cast<int32_t>(rnd(state, 4000)) - 2000 : static_cast<int32_t>(nextFuzz(state)));
    mem<int16_t>(effect + 0x2cc) = static_cast<int16_t>(rnd(state, 4) ? static_cast<int32_t>(rnd(state, 4000)) - 2000 : static_cast<int32_t>(nextFuzz(state)));
    mem<uint16_t>(effect + 0x10) = static_cast<uint16_t>(rnd(state, 4) ? rnd(state, 0x90) : nextFuzz(state));
    if (rnd(state, 8))
        static_cast<_sObject *>(guest_host(scratch))->sDontRenderMe = 0;
}

// Sarge's lights: the car at +0x650 with a render object whose head has a brightness word at +0x64 that sometimes hides
// it, a definition whose type is sometimes 0x16, and a counter at +0x654.
void vfxSargeLights(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    if (!buildCar(ctx, state, scratch))
        return;
    nearCar(state, scratch);
    renderState(state, scratch);
    _sObject &car = *static_cast<_sObject *>(guest_host(scratch));
    _sObjectDef &def = *static_cast<_sObjectDef *>(guest_host(scratch + kDef));
    def = *car.pDef;
    def.type = static_cast<eObjectType>(rnd(state, 2) ? 0x16 : rnd(state, 0x30));
    car.pDef = ptr32<_sObjectDef>(scratch + kDef);
    mem<uint32_t>(scratch + kAuxA) = scratch + kAuxB;
    car.pRendObj = ptr32<_sRendObj>(scratch + kAuxA);
    mem<uint32_t>(scratch + kAuxB + 0x64) = rnd(state, 4) ? nextFuzz(state) : 0xFF000000u;
    const uint32_t effect = scratch + kEffect;
    mem<uint32_t>(effect + 0x650) = scratch;
    mem<uint32_t>(effect + 0x654) = nextFuzz(state);
    setArg(ctx, 0, effect);
}

// The skid marks: a ring of 16 or 32 marks of 0x14 bytes (two points as shorts, a life at +0x10, an active flag at +0x12
// and a material at +0x13), with at least two marks in use most of the time.
void vfxCarSkidMark(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    restoreGlobals();
    renderState(state, scratch);
    const uint32_t effect = scratch + kEffect;
    const uint32_t mask = rnd(state, 2) ? 15 : 31;
    buildRing(state, effect, mask, effect + 0x44, 0x14, 0x10, 2);
    if (rnd(state, 8))
        mem<uint8_t>(effect + 0x41) = static_cast<uint8_t>(2 + rnd(state, mask));
    scatterParticles(state, effect + 0x44, mask + 1, 0x14, 0x10, 2, 12);
    for (uint32_t i = 0; i <= mask; ++i)
    {
        const uint32_t p = effect + 0x44 + i * 0x14;
        mem<uint8_t>(p + 0x12) = static_cast<uint8_t>(rnd(state, 4) ? 1 : 0);
        mem<uint8_t>(p + 0x13) = static_cast<uint8_t>(rnd(state, 4) == 0 ? (rnd(state, 2) ? 0x14 : 0x23) : rnd(state, 4) ? rnd(state, 0x48) : nextFuzz(state));
        for (uint32_t offset : {8u, 0xau, 0xcu})
            mem<int16_t>(p + offset) = static_cast<int16_t>(static_cast<int32_t>(rnd(state, 2000)) - 1000);
    }
    setArg(ctx, 0, effect);
}

// The quake: a ring of 16 points of 0x30 bytes (floats at +0) from +0x50 (mask 15, so the remainder is never a divide by
// zero), the centre (shorts at +0x654) and a life from 0 to well past the fade.
void vfxQuake(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    restoreGlobals();
    const uint32_t effect = scratch + kEffect;
    renderState(state, scratch);
    buildRing(state, effect, 15, effect + 0x50, 0x30, 0x10, 2);
    mem<uint16_t>(effect + 0x10) = static_cast<uint16_t>(rnd(state, 4) ? rnd(state, 0x40) : nextFuzz(state));
    for (uint32_t i = 0; i < 16; ++i)
        for (uint32_t lane = 0; lane < 4; ++lane)
            mem<float>(effect + 0x50 + i * 0x30 + 4 * lane) = frnd(state, 200.0f);
    for (uint32_t axis = 0; axis < 3; ++axis)
        mem<int16_t>(effect + 0x654 + 2 * axis) = static_cast<int16_t>(static_cast<int32_t>(rnd(state, 8001)) - 4000);
    setArg(ctx, 0, effect);
}

// A bolt between two points: a0 and a1 the ends (float vectors), a2 a count (1 most of the time, which allows a fork),
// f12 the width and f13 the scatter.
void vfxLightningBetween(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    restoreGlobals();
    renderState(state, scratch);
    buildEnds(state, scratch);
    setArg(ctx, 0, scratch + kVectors + 16 * rnd(state, 4));
    setArg(ctx, 1, scratch + kVectors + 16 * rnd(state, 4));
    setArg(ctx, 2, rnd(state, 4) ? 1 : rnd(state, 3));
    setFloatArg(ctx, 0, float_bits(frnd(state, 40.0f)));
    setFloatArg(ctx, 1, float_bits(frnd(state, 4.0f)));
}


// --- the sixth batch: the camera set-up, aerial, quake, water, oil, pickup, rocket and countdown effects -----------------

namespace {

// A ring effect for a renderer: the render state, the ring of `mask + 1` particles of `stride` bytes from `first`, with
// positions, lives and the car-style fields scattered over them (lifeOffset as in scatterParticles).
uint32_t ringEffect(uint64_t &state, uint32_t scratch, uint32_t mask, uint32_t first, uint32_t stride, uint32_t lifeOffset, uint32_t lifeMax,
                    uint32_t lifeBytes = 2)
{
    restoreGlobals();
    renderState(state, scratch);
    const uint32_t effect = scratch + kEffect;
    buildRing(state, effect, mask, effect + first, stride, lifeOffset, lifeBytes);
    scatterParticles(state, effect + first, mask + 1, stride, lifeOffset, lifeBytes, lifeMax);
    return effect;
}

uint32_t someTexture(uint64_t &state, uint32_t scratch) { return scratch + kTexture + 16 * rnd(state, 8); }

// The HUD's resource record, reached through the level record (pLvlG->pHudRes at +0x40): a weapon sprite at +0x80 (a
// record whose first word is its texture) and the countdown's textures from +0x88.
void hudResource(uint64_t &state, uint32_t scratch)
{
    g_levelRecord.set(0x327e80u, scratch + kAuxC);
    mem<uint32_t>(scratch + kAuxC + 0x40) = scratch + kAuxB;
    mem<uint32_t>(scratch + kAuxB + 0x80) = scratch + kAuxA;
    mem<uint32_t>(scratch + kAuxA) = someTexture(state, scratch);
    for (uint32_t i = 0; i < 8; ++i)
        mem<uint32_t>(scratch + kAuxB + 0x88 + 4 * i) = someTexture(state, scratch);
}

} // namespace

// The camera set-up reads the game's own camera, view matrix and screen state; the split screen path needs the main state
// to say so now and then.
void vfxCameraMatrix(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    restoreGlobals();
    (void)ctx;
    (void)scratch;
    if (rnd(state, 2))
    {
        g_split.set(0x1edc68u, 1);
        g_screen.set(0x1edc78u, rnd(state, 4) ? 0 : 1);
    }
}

// The heat ray aerial's renderer: the fire renderer's car and ring (32 particles of 0x18 bytes, scattered), with an
// aerial on the car.
void vfxCarHeatRayAerialRenderer(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    vfxCarFireRenderer(ctx, state, scratch);
    aerialCar(state, scratch);
    nearCar(state, scratch);
    const uint32_t effect = scratch + kEffect;
    scatterParticles(state, effect + 0x44, 32, 0x18, 0x14, 2, 0x20);
}

// The water splash: a ring of 16, 32 or 64 particles of 0x14 bytes whose flag at +0x12 is zero half the time (the streaks), the
// centre (shorts at +0x544), the water's height (+0x54e), a life and the ground normal (shorts at +0x14).
void vfxWaterSploosh(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    const uint32_t mask = rnd(state, 3) == 0 ? 15 : rnd(state, 2) ? 31 : 63; // the game's own is 64 drops
    const uint32_t effect = ringEffect(state, scratch, mask, 0x44, 0x14, 0x10, 0x20);
    for (uint32_t i = 0; i <= mask; ++i)
        mem<uint16_t>(effect + 0x44 + i * 0x14 + 0x12) = static_cast<uint16_t>(rnd(state, 2) ? 0 : nextFuzz(state));
    for (uint32_t i = 0; i < 3; ++i)
        mem<int16_t>(effect + 0x544 + 2 * i) = static_cast<int16_t>(static_cast<int32_t>(rnd(state, 6000)) - 3000);
    mem<int16_t>(effect + 0x54e) = static_cast<int16_t>(static_cast<int32_t>(rnd(state, 6000)) - 3000);
    mem<uint16_t>(effect + 0x10) = static_cast<uint16_t>(rnd(state, 4) ? rnd(state, 0x60) : nextFuzz(state));
    for (uint32_t axis = 0; axis < 3; ++axis)
        mem<int16_t>(effect + 0x14 + 2 * axis) = static_cast<int16_t>(static_cast<int32_t>(nextFuzz(state) % 8193u) - 4096 + (axis == 1 ? 4096 : 0));
    setArg(ctx, 0, effect);
}

// The oil slick: a ring of 16 or 32 blobs of 0x18 bytes (a fade-in at +0x14, zero now and then), the ground normal at +8
// and the effect's age (past 0xd0 it fades).
void vfxOilSlick(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    const uint32_t mask = rnd(state, 2) ? 15 : 31;
    const uint32_t effect = ringEffect(state, scratch, mask, 0x44, 0x18, 0x14, 0x14);
    for (uint32_t i = 0; i <= mask; ++i)
    {
        const uint32_t p = effect + 0x44 + i * 0x18;
        for (uint32_t axis = 0; axis < 3; ++axis)
            mem<int16_t>(p + 8 + 2 * axis) = static_cast<int16_t>(static_cast<int32_t>(nextFuzz(state) % 8193u) - 4096 + (axis == 1 ? 4096 : 0));
        mem<uint8_t>(p + 0x17) = static_cast<uint8_t>(nextFuzz(state));
    }
    mem<uint16_t>(effect + 0x10) = static_cast<uint16_t>(rnd(state, 4) ? 0xa0 + rnd(state, 0x100) : nextFuzz(state));
    setArg(ctx, 0, effect);
}

// The car's aerial as a ribbon: the car (with an aerial) at +0xb0, six vectors from +0x40, and now and then the level that
// hides cars that ask not to be drawn.
void vfxAerialRenderer(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    if (!buildCar(ctx, state, scratch))
        return;
    aerialCar(state, scratch);
    nearCar(state, scratch);
    renderState(state, scratch);
    g_level.set(0x1edc4au, rnd(state, 2) ? 0x62 : 0x10);
    const uint32_t effect = scratch + kEffect;
    mem<uint32_t>(effect + 0xb0) = scratch;
    for (uint32_t i = 0; i < 24; ++i)
        mem<float>(effect + 0x40 + 4 * i) = frnd(state, 200.0f);
    mem<uint32_t>(scratch + 0x48) = nextFuzz(state);
    setArg(ctx, 0, effect);
}

// The spinning pickup: the car at +0x44 (its player type at +0x44, screen at +0x45), a ring of 8 sparkles from +0x48,
// the pickup's position (shorts at +0xe8) and spin (+0xf8), a life up to 0x40, on one screen or two.
void vfxSpinPickup(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    if (!buildCar(ctx, state, scratch))
        return;
    nearCar(state, scratch);
    const uint32_t effect = ringEffect(state, scratch, 7, 0x48, 0x14, 0x10, 0x20);
    mem<uint32_t>(effect + 0x44) = scratch;
    mem<uint8_t>(scratch + 0x44) = static_cast<uint8_t>(rnd(state, 4) ? rnd(state, 2) : 2);
    mem<uint8_t>(scratch + 0x45) = static_cast<uint8_t>(rnd(state, 2));
    g_split.set(0x1edc68u, static_cast<uint8_t>(rnd(state, 2)));
    g_screen.set(0x1edc78u, rnd(state, 2));
    hudResource(state, scratch);
    for (uint32_t i = 0; i < 3; ++i)
        mem<int16_t>(effect + 0xe8 + 2 * i) = static_cast<int16_t>(static_cast<int32_t>(rnd(state, 6000)) - 3000);
    mem<uint16_t>(effect + 0xf8) = static_cast<uint16_t>(nextFuzz(state));
    mem<uint16_t>(effect + 0x10) = static_cast<uint16_t>(rnd(state, 8) ? rnd(state, 0x44) : nextFuzz(state));
    setArg(ctx, 0, effect);
}

// The aerial's ribbon: the car (with an aerial) at +0x124, a ring of 8 particles of 0x18 bytes from +0x44 (life at +0x14,
// velocities at +8) and its three-byte trail offsets from +0x104.
void vfxCarAerialRibbon(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    if (!buildCar(ctx, state, scratch))
        return;
    aerialCar(state, scratch);
    restoreGlobals();
    const uint32_t effect = scratch + kEffect;
    buildRing(state, effect, 7, effect + 0x44, 0x18, 0x14, 2);
    scatterParticles(state, effect + 0x44, 8, 0x18, 0x14, 2, 6);
    for (uint32_t i = 0; i < 8; ++i)
        for (uint32_t offset : {8u, 0xau, 0xcu})
            mem<int16_t>(effect + 0x44 + i * 0x18 + offset) = static_cast<int16_t>(static_cast<int32_t>(rnd(state, 200)) - 100);
    for (uint32_t i = 0; i < 0x20; ++i)
        mem<uint8_t>(effect + 0x104 + i) = static_cast<uint8_t>(nextFuzz(state));
    mem<uint32_t>(effect + 0x124) = scratch;
    setArg(ctx, 0, effect);
}

// A drop ring of 32 particles of 0x30 bytes from +0x50 (positions, velocities and the ground height as floats; the life, a
// 32-bit word, at +0x24), the ball's position (shorts at +0x14) and its spin (+0x654).
void waterBall(uint64_t &state, uint32_t scratch, uint32_t effect, bool handler)
{
    buildRing(state, effect, 31, effect + 0x50, 0x30, 0x24, 4);
    const uint32_t mask = 31;
    for (uint32_t i = 0; i <= mask; ++i)
    {
        const uint32_t p = effect + 0x50 + i * 0x30;
        for (uint32_t offset : {0u, 4u, 8u, 0x10u, 0x14u, 0x18u, 0x20u})
            mem<float>(p + offset) = frnd(state, 100.0f);
        mem<uint32_t>(p + 0x24) = rnd(state, 8) ? rnd(state, 0x24) : nextFuzz(state);
    }
    const _sObject &car = *static_cast<_sObject *>(guest_host(scratch));
    const sVec32 &pos = *static_cast<sVec32 *>(guest_host(car.pPhysics.address ? mem<uint32_t>(car.pPhysics.address + 0x1e8) + 4 : scratch));
    const int32_t base[3] = {pos.x >> 8, pos.y >> 8, pos.z >> 8};
    for (uint32_t axis = 0; axis < 3; ++axis)
        mem<int16_t>(effect + 0x14 + 2 * axis) = static_cast<int16_t>(base[axis] + static_cast<int32_t>(rnd(state, 100)) - 50);
    mem<uint16_t>(effect + 0x10) = static_cast<uint16_t>(rnd(state, 8) ? rnd(state, 0x60) : nextFuzz(state));
    mem<uint32_t>(effect + 0x654) = nextFuzz(state);
    // The handler casts straight down on the level's collision grid: only with one that is really there.
    const uint32_t grid = G(acolG).pGrid.address;
    if (handler && (grid < 0x100000u || grid >= 0x1ffff00u))
        mem<uint8_t>(effect + 0x41) = static_cast<uint8_t>(mem<uint8_t>(effect + 0x40));
}

void vfxWaterBallHandler(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    if (!buildCar(ctx, state, scratch))
        return;
    waterBall(state, scratch, scratch + kEffect, true);
    setArg(ctx, 0, scratch + kEffect);
}

void vfxWaterBallRenderer(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    if (!buildCar(ctx, state, scratch))
        return;
    renderState(state, scratch);
    waterBall(state, scratch, scratch + kEffect, false);
    setArg(ctx, 0, scratch + kEffect);
}

// The countdown: its pictures from the HUD's resource, a ring of 16 sparkles of 0x14 bytes from +0x48, the picture's
// position (shorts at +0x188), spin (+0x1a0) and number (+0x1a4, 0 to 3), on one screen or two.
void vfxReadySteadyGo(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    const uint32_t effect = ringEffect(state, scratch, 15, 0x48, 0x14, 0x10, 0x20);
    g_split.set(0x1edc68u, static_cast<uint8_t>(rnd(state, 2)));
    g_screen.set(0x1edc78u, rnd(state, 2));
    hudResource(state, scratch);
    for (uint32_t i = 0; i < 3; ++i)
        mem<int16_t>(effect + 0x188 + 2 * i) = static_cast<int16_t>(static_cast<int32_t>(rnd(state, 600)) - 300);
    mem<uint16_t>(effect + 0x1a0) = static_cast<uint16_t>(nextFuzz(state));
    mem<uint32_t>(effect + 0x1a4) = rnd(state, 8) ? rnd(state, 4) : rnd(state, 8);
    mem<uint16_t>(effect + 0x10) = static_cast<uint16_t>(rnd(state, 4) ? rnd(state, 0x40) : nextFuzz(state));
    setArg(ctx, 0, effect);
}

// The rocket's smoke: a ring of 32 puffs of 0x14 bytes, the rocket's position (shorts at +0x14) and direction (+0x2cc), a
// spin counter (+0x2d4) and a life that is mostly above 0x20 (which draws the rocket itself with the level's own shape).
void vfxRocketSmokeRenderer(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    const uint32_t realRes = G(visFxVarG).pVfxRes.address;
    const uint32_t shape = realRes ? mem<uint32_t>(realRes + 0x88) : 0;
    const uint32_t effect = ringEffect(state, scratch, 31, 0x44, 0x14, 0x10, 0x20);
    mem<uint32_t>(scratch + kRes + 0x88) = shape;
    for (uint32_t i = 0; i < 3; ++i)
    {
        mem<int16_t>(effect + 0x14 + 2 * i) = static_cast<int16_t>(static_cast<int32_t>(rnd(state, 6000)) - 3000);
        mem<int16_t>(effect + 0x2cc + 2 * i) = static_cast<int16_t>(rnd(state, 16) ? static_cast<int32_t>(rnd(state, 8000)) - 4000 : 0);
    }
    mem<uint32_t>(effect + 0x2d4) = nextFuzz(state);
    mem<uint16_t>(effect + 0x10) = static_cast<uint16_t>(shape && rnd(state, 4) ? 0x20 + rnd(state, 0x40) : rnd(state, 0x20));
    setArg(ctx, 0, effect);
}

// The car rocket's handler: the car (with an aerial) at +0x350 whose weapon byte is zero now and then, a ring of 16
// particles of 0x30 bytes from +0x50 with floats and a 32-bit life at +0x24.
void vfxCarRocketHandler(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    if (!buildCar(ctx, state, scratch))
        return;
    aerialCar(state, scratch);
    const uint32_t effect = scratch + kEffect;
    mem<uint32_t>(effect + 0x350) = scratch;
    mem<int8_t>(scratch + 0x194) = static_cast<int8_t>(rnd(state, 4) ? 1 + rnd(state, 100) : 0);
    buildRing(state, effect, 15, effect + 0x50, 0x30, 0x24, 4);
    for (uint32_t i = 0; i < 16; ++i)
    {
        const uint32_t p = effect + 0x50 + i * 0x30;
        for (uint32_t offset : {0u, 4u, 8u, 0x10u, 0x14u, 0x18u})
            mem<float>(p + offset) = frnd(state, 60.0f);
        mem<uint32_t>(p + 0x24) = rnd(state, 4) ? rnd(state, 0x12) : nextFuzz(state);
    }
    setArg(ctx, 0, effect);
}


// A scenery instance's position, in whole units a short way from the camera (visFxVarG.svCamPos, as the game has it) most of
// the time, else anywhere: the effects that fade with the distance, or are drawn only near the camera.
void nearTheCamera(uint64_t &state, uint32_t scratch, uint32_t range)
{
    for (uint32_t axis = 0; axis < 3; ++axis)
    {
        const int32_t camera = mem<int16_t>(0x1f1108u + 2 * axis);
        const int32_t where = rnd(state, 8) ? camera + static_cast<int32_t>(rnd(state, 2 * range)) - static_cast<int32_t>(range)
                                            : static_cast<int32_t>(rnd(state, 16000)) - 8000;
        mem<int32_t>(scratch + kTobInst + 4 + 4 * axis) = where * 256 + static_cast<int32_t>(rnd(state, 256));
    }
}

void vfxBatsHandler(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    const uint32_t effect = sceneryEffect(state, scratch, 31, 0x14, 0x10);
    scatterParticles(state, effect + 0x44, 32, 0x14, 0x10, 2, 0x40);
    mem<uint16_t>(effect + 0x2c4) = static_cast<uint16_t>(rnd(state, 2) ? 1 + 2 * rnd(state, 50) : nextFuzz(state));
    mem<uint16_t>(effect + 0x2c6) = static_cast<uint16_t>(rnd(state, 2) ? 1 : rnd(state, 20));
    setArg(ctx, 0, effect);
}

void vfxSciFiBubblesHandler(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    const uint32_t effect = sceneryEffect(state, scratch, 15, 0x14, 0x10);
    scatterParticles(state, effect + 0x44, 16, 0x14, 0x10, 2, 0x40);
    mem<uint16_t>(effect + 0x184) = static_cast<uint16_t>(rnd(state, 2) ? 4 * rnd(state, 50) : nextFuzz(state));
    mem<uint16_t>(effect + 0x186) = static_cast<uint16_t>(rnd(state, 2) ? 1 : rnd(state, 20));
    setArg(ctx, 0, effect);
}

// The lights: the scenery instance near the camera, a colour (three bytes at +0x48, some zero) and the flare's size count at
// +0x4c (now and then a negative or huge one).
void vfxLight(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    restoreGlobals();
    renderState(state, scratch);
    const uint32_t effect = scratch + kEffect;
    buildScenery(state, scratch, effect, 0x40);
    nearTheCamera(state, scratch, 6000);
    for (uint32_t i = 0; i < 3; ++i)
        mem<uint8_t>(effect + 0x48 + i) = static_cast<uint8_t>(rnd(state, 4) ? rnd(state, 256) : 0);
    mem<uint32_t>(effect + 0x4c) = rnd(state, 8) ? rnd(state, 12) : nextFuzz(state);
    mem<uint16_t>(scratch + kTobInst + 0x2c) = static_cast<uint16_t>(rnd(state, 4) ? 10 + rnd(state, 200) : nextFuzz(state));
    setArg(ctx, 0, effect);
}

// The butterflies: a ring of 16 particles of 0x70 bytes from +0x50 (positions, a phase at +0x10, a 4x4 matrix from +0x20,
// colours at +0x60 and +0x64), the centre (shorts at +0x750) near the camera most of the time.
void vfxButterflies(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    restoreGlobals();
    renderState(state, scratch);
    const uint32_t effect = scratch + kEffect;
    buildRing(state, effect, 15, effect + 0x50, 0x70, 0x10, 2);
    for (uint32_t i = 0; i < 16; ++i)
    {
        const uint32_t p = effect + 0x50 + i * 0x70;
        for (uint32_t offset : {0u, 2u, 4u})
            mem<int16_t>(p + offset) = static_cast<int16_t>(static_cast<int32_t>(rnd(state, 4000)) - 2000);
        mem<uint16_t>(p + 0x10) = static_cast<uint16_t>(nextFuzz(state));
        for (uint32_t lane = 0; lane < 16; ++lane)
            mem<float>(p + 0x20 + 4 * lane) = frnd(state, 1.0f);
        mem<uint32_t>(p + 0x60) = nextFuzz(state);
        mem<uint32_t>(p + 0x64) = nextFuzz(state);
    }
    for (uint32_t axis = 0; axis < 3; ++axis)
    {
        const int32_t camera = mem<int16_t>(0x1f1108u + 2 * axis);
        const int32_t where = rnd(state, 8) ? camera + static_cast<int32_t>(rnd(state, 16000)) - 8000 : static_cast<int32_t>(nextFuzz(state));
        mem<int16_t>(effect + 0x750 + 2 * axis) = static_cast<int16_t>(where);
    }
    setArg(ctx, 0, effect);
}

// The pirate lava: a ring of 32 blobs of 0x18 bytes (an age at +0x14, a size byte at +0x16 mostly above 100) and the lava
// colour (three bytes at +0x648).
void vfxPirateLava(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    const uint32_t effect = sceneryRenderer(state, scratch, 31, 0x18, 0x14, 2, 0x648, 0x20);
    for (uint32_t i = 0; i < 32; ++i)
        mem<uint8_t>(effect + 0x44 + i * 0x18 + 0x16) = static_cast<uint8_t>(rnd(state, 3) ? 0x60 + rnd(state, 0x40) : nextFuzz(state));
    setArg(ctx, 0, effect);
}

// The car splash: a ring of 16, 32 or 64 drops of 0x14 bytes (a flag at +0x12 that is zero half the time), the centre (shorts at
// +0x548) and a life up to 0x58.
void vfxCarSplash(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    const uint32_t mask = rnd(state, 3) == 0 ? 15 : rnd(state, 2) ? 31 : 63; // the game's own is 64 drops
    const uint32_t effect = ringEffect(state, scratch, mask, 0x44, 0x14, 0x10, 0x20);
    for (uint32_t i = 0; i <= mask; ++i)
        mem<uint16_t>(effect + 0x44 + i * 0x14 + 0x12) = static_cast<uint16_t>(rnd(state, 2) ? 0 : nextFuzz(state));
    for (uint32_t i = 0; i < 3; ++i)
        mem<int16_t>(effect + 0x548 + 2 * i) = static_cast<int16_t>(static_cast<int32_t>(rnd(state, 6000)) - 3000);
    mem<uint16_t>(effect + 0x10) = static_cast<uint16_t>(rnd(state, 8) ? rnd(state, 0x58) : nextFuzz(state));
    setArg(ctx, 0, effect);
}

// The cannon's handler: the scenery's shape is in the first frames of its animation (below 8) most of the time.
void vfxCannonHandler(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    const uint32_t effect = sceneryEffect(state, scratch, 15, 0x14, 0x10, 12);
    scatterParticles(state, effect + 0x44, 16, 0x14, 0x10, 2, 0x30);
    setArg(ctx, 0, effect);
}

// The glints: 128 glints round the camera (a vector the setup chooses, near the real car when there is one), some alive
// with lives that wrap now and then, the "last started" spots for both screens, and the screen being drawn. Without a
// collision grid to cast on, the camera has not moved (no glint is started).
void vfxGlint(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    if (!buildCar(ctx, state, scratch))
        return;
    renderState(state, scratch);
    const _sObject &car = *static_cast<_sObject *>(guest_host(scratch));
    const sVec32 &pos = *static_cast<sVec32 *>(guest_host(mem<uint32_t>(car.pPhysics.address + 0x1e8) + 4));
    FourFloats camera;
    camera.v[0] = static_cast<float>(pos.x >> 8) + frnd(state, 50.0f);
    camera.v[1] = static_cast<float>(pos.y >> 8) + frnd(state, 50.0f);
    camera.v[2] = static_cast<float>(pos.z >> 8) + frnd(state, 50.0f);
    camera.v[3] = frnd(state, 10.0f);
    g_cameraVector.set(0x1f1160u, camera);
    g_screen.set(0x1edc78u, rnd(state, 4) ? 0 : 1);
    const uint32_t grid = G(acolG).pGrid.address;
    const bool haveGrid = grid >= 0x100000u && grid < 0x1ffff00u;
    EightFloats spots;
    for (uint32_t screen = 0; screen < 2; ++screen)
        for (uint32_t axis = 0; axis < 4; ++axis)
            spots.v[4 * screen + axis] = (!haveGrid || rnd(state, 2)) ? camera.v[axis] + frnd(state, 150.0f) : camera.v[axis] + frnd(state, 1000.0f);
    g_lastSpots.set(0x1f36f0u, spots);
    GlintBlock block;
    std::memset(&block, 0, sizeof(block));
    g_glints.set(0x1f1eb0u, block);
    for (uint32_t i = 0; i < 0x80; ++i)
    {
        const uint32_t glint = 0x1f1eb0u + i * 0x30;
        for (uint32_t axis = 0; axis < 4; ++axis)
            mem<float>(glint + 4 * axis) = camera.v[axis] + frnd(state, 3500.0f);
        const uint32_t kind = rnd(state, 4);
        mem<int16_t>(glint + 0x10) = static_cast<int16_t>(kind < 2 ? 0 : kind == 2 ? 1 + rnd(state, 200) : nextFuzz(state));
        mem<uint16_t>(glint + 0x12) = static_cast<uint16_t>(nextFuzz(state));
    }
}

// --- the seventh batch (visfx8.cpp) ------------------------------------------------------------------------------------

namespace {

bool haveCollisionGrid()
{
    const uint32_t grid = G(acolG).pGrid.address;
    return grid >= 0x100000u && grid < 0x1ffff00u;
}

// A lava ring (32 blobs of 0x18 bytes): the counter at counterOffset is mostly one short of a multiple of 16 (so a blob
// starts), the ring now and then full, lives low, some blobs far below (the pirate lava drops those).
uint32_t lavaHandler(uint64_t &state, uint32_t scratch, uint32_t counterOffset)
{
    const uint32_t effect = sceneryEffect(state, scratch, 31, 0x18, 0x14);
    scatterParticles(state, effect + 0x44, 32, 0x18, 0x14, 2, 0x10);
    for (uint32_t i = 0; i < 32; ++i)
    {
        const uint32_t p = effect + 0x44 + i * 0x18;
        for (uint32_t offset : {8u, 0xau, 0xcu})
            mem<int16_t>(p + offset) = static_cast<int16_t>(static_cast<int32_t>(rnd(state, 0x200)) - 0x100);
        if (rnd(state, 8) == 0)
            mem<int16_t>(p + 2) = static_cast<int16_t>(-29000 - static_cast<int32_t>(rnd(state, 3000)));
        mem<uint8_t>(p + 0x16) = static_cast<uint8_t>(rnd(state, 2) ? 0x60 + rnd(state, 0x40) : nextFuzz(state));
    }
    if (rnd(state, 4) == 0)
        mem<uint8_t>(effect + 0x41) = static_cast<uint8_t>(0x1f + rnd(state, 0x30));
    mem<uint16_t>(effect + counterOffset) = static_cast<uint16_t>(rnd(state, 2) ? 16 * rnd(state, 100) - 1 : nextFuzz(state));
    mem<uint8_t>(effect + counterOffset + 7) = static_cast<uint8_t>(nextFuzz(state));
    return effect;
}

// The car-weapon drops (32 of 0x30 bytes from +0x50, a 32-bit life at +0x24, the ground height at +0x20 near the drop's
// height), the car (with an aerial) at +0x650 holding 0 to 3 charges, and a phase at +0x654. Without a collision grid to
// cast on, the ring is full (no drop starts).
uint32_t waterDrops(uint64_t &state, uint32_t scratch)
{
    aerialCar(state, scratch);
    const uint32_t effect = scratch + kEffect;
    buildRing(state, effect, 31, effect + 0x50, 0x30, 0x24, 4);
    if (rnd(state, 3) == 0)
        mem<uint8_t>(effect + 0x41) = static_cast<uint8_t>(rnd(state, 2) ? 31 : 30);
    for (uint32_t i = 0; i < 32; ++i)
    {
        const uint32_t p = effect + 0x50 + i * 0x30;
        for (uint32_t offset : {0u, 4u, 8u, 0x10u, 0x14u, 0x18u})
            mem<float>(p + offset) = frnd(state, 100.0f);
        mem<float>(p + 0x20) = rnd(state, 8) ? mem<float>(p + 4) + frnd(state, 30.0f) : -1000.0f;
        mem<uint32_t>(p + 0x24) = rnd(state, 8) ? rnd(state, 0x24) : nextFuzz(state);
    }
    mem<uint32_t>(effect + 0x650) = scratch;
    mem<uint32_t>(effect + 0x654) = nextFuzz(state);
    mem<int8_t>(scratch + 0x194) = static_cast<int8_t>(rnd(state, 4) ? rnd(state, 4) : nextFuzz(state) % 9u);
    mem<uint16_t>(effect + 0x10) = static_cast<uint16_t>(rnd(state, 4) ? rnd(state, 0x40) : nextFuzz(state));
    if (!haveCollisionGrid())
        mem<uint8_t>(effect + 0x41) = 31;
    return effect;
}

} // namespace

void vfxPirateLavaHandler(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    const uint32_t effect = lavaHandler(state, scratch, 0x644);
    setArg(ctx, 0, effect);
}

void vfxLavaSpitHandler(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    const uint32_t effect = lavaHandler(state, scratch, 0x344);
    mem<uint8_t>(effect + 0x41) = static_cast<uint8_t>(rnd(state, 4) ? rnd(state, 0x1f) : mem<uint8_t>(effect + 0x41));
    if (rnd(state, 2)) // a counter on a multiple of 8, so blobs split
        mem<uint16_t>(effect + 0x344) = static_cast<uint16_t>(8 * rnd(state, 1000) + 7);
    setArg(ctx, 0, effect);
}

// The gulls: the first call (the flag at +0x184 zero) half the time.
void vfxGullsHandler(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    const uint32_t effect = sceneryEffect(state, scratch, 15, 0x14, 0x10);
    scatterParticles(state, effect + 0x44, 16, 0x14, 0x10, 2, 0x40);
    for (uint32_t i = 0; i < 16; ++i)
        for (uint32_t offset : {8u, 0xau, 0xcu})
            mem<int16_t>(effect + 0x44 + i * 0x14 + offset) = static_cast<int16_t>(static_cast<int32_t>(rnd(state, 0x100)) - 0x80);
    mem<uint16_t>(effect + 0x184) = static_cast<uint16_t>(rnd(state, 2) ? 0 : nextFuzz(state));
    setArg(ctx, 0, effect);
}

// The butterflies' handler: the renderer's ring, with the first call (life 1) a third of the time and the timers (+0x12) at
// 0, 1 or more.
void vfxButterfliesHandler(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    vfxButterflies(ctx, state, scratch);
    const uint32_t effect = scratch + kEffect;
    mem<uint16_t>(effect + 0x10) = static_cast<uint16_t>(rnd(state, 3) == 0 ? 1 : nextFuzz(state));
    for (uint32_t i = 0; i < 16; ++i)
    {
        const uint32_t p = effect + 0x50 + i * 0x70;
        const uint32_t kind = rnd(state, 4);
        mem<uint16_t>(p + 0x12) = static_cast<uint16_t>(kind == 0 ? 0 : kind == 1 ? 1 : kind == 2 ? 2 + rnd(state, 50) : nextFuzz(state));
        for (uint32_t offset : {8u, 0xau, 0xcu})
            mem<int16_t>(p + offset) = static_cast<int16_t>(static_cast<int32_t>(rnd(state, 0x40)) - 0x20);
        mem<int16_t>(p) = static_cast<int16_t>(static_cast<int32_t>(rnd(state, 10000)) - 5000);
        mem<int16_t>(p + 2) = static_cast<int16_t>(static_cast<int32_t>(rnd(state, 400)) - 100);
        mem<int16_t>(p + 4) = static_cast<int16_t>(static_cast<int32_t>(rnd(state, 10000)) - 5000);
    }
    setArg(ctx, 0, effect);
}

// The oil slick's handler: the drop point (shorts at +0x644) near the car, on its first frame (life 200) a third of the
// time when there is collision to cast on, and a ring of 16 or 32 blobs with lives near zero.
void vfxOilSlickHandler(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    if (!buildCar(ctx, state, scratch))
        return;
    const uint32_t mask = rnd(state, 2) ? 63 : 31;
    const uint32_t effect = scratch + kEffect;
    buildRing(state, effect, mask, effect + 0x44, 0x18, 0x14, 2);
    const _sObject &car = *static_cast<_sObject *>(guest_host(scratch));
    const sVec32 &pos = *static_cast<sVec32 *>(guest_host(mem<uint32_t>(car.pPhysics.address + 0x1e8) + 4));
    const int32_t base[3] = {pos.x >> 8, pos.y >> 8, pos.z >> 8};
    for (uint32_t axis = 0; axis < 3; ++axis)
        mem<int16_t>(effect + 0x644 + 2 * axis) = static_cast<int16_t>(base[axis] + static_cast<int32_t>(rnd(state, 200)) - 100);
    mem<uint16_t>(effect + 0x10) = static_cast<uint16_t>(haveCollisionGrid() && rnd(state, 3) == 0 ? 200 : rnd(state, 2) ? rnd(state, 200) : nextFuzz(state));
    setArg(ctx, 0, effect);
}

// The change-over: the car at +0x2c4 and a target kind at +0x2c8 for which ObjectCarChangeOver does nothing (3 to 7: it
// swaps models and plays sounds only for 0 to 2), 31 sparks, a life of 80 (the burst), 66 (the change) or 1 (the end)
// most of the time.
void vfxCarChangeOver(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    if (!buildCar(ctx, state, scratch))
        return;
    maybeBoat(state, scratch);
    const uint32_t effect = scratch + kEffect;
    buildRing(state, effect, 31, effect + 0x44, 0x14, 0x10, 2);
    scatterParticles(state, effect + 0x44, 32, 0x14, 0x10, 2, 0x40);
    for (uint32_t i = 0; i < 32; ++i)
        for (uint32_t offset : {8u, 0xau, 0xcu})
            mem<int16_t>(effect + 0x44 + i * 0x14 + offset) = static_cast<int16_t>(static_cast<int32_t>(rnd(state, 0x200)) - 0x100);
    mem<uint32_t>(effect + 0x2c4) = scratch;
    mem<uint32_t>(effect + 0x2c8) = 3 + rnd(state, 5);
    mem<uint16_t>(effect + 0x2cc) = static_cast<uint16_t>(nextFuzz(state));
    mem<uint16_t>(effect + 0x2ce) = static_cast<uint16_t>(rnd(state, 2) ? rnd(state, 200) : nextFuzz(state));
    static constexpr uint16_t kLives[] = {0x50, 0x42, 1, 0x30};
    mem<uint16_t>(effect + 0x10) = static_cast<uint16_t>(rnd(state, 4) ? kLives[rnd(state, 4)] : nextFuzz(state));
    setArg(ctx, 0, effect);
}

void vfxCarWaterBallHandler(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    if (!buildCar(ctx, state, scratch))
        return;
    restoreGlobals();
    setArg(ctx, 0, waterDrops(state, scratch));
}

// The water-covered car: as the water ball's drops, the car's record at +0x40 (its aerial, in scratch) taking the 300.
void vfxCarWaterCovered(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    if (!buildCar(ctx, state, scratch))
        return;
    restoreGlobals();
    const uint32_t effect = waterDrops(state, scratch);
    mem<uint16_t>(effect + 0x10) = static_cast<uint16_t>(rnd(state, 4) ? 0xc + rnd(state, 0x10) : nextFuzz(state));
    setArg(ctx, 0, effect);
}

void vfxCarWaterBallRenderer(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    if (!buildCar(ctx, state, scratch))
        return;
    restoreGlobals();
    nearCar(state, scratch);
    renderState(state, scratch);
    const uint32_t effect = waterDrops(state, scratch);
    for (uint32_t i = 0; i < 32; ++i)
        if (rnd(state, 2))
            mem<uint32_t>(effect + 0x50 + i * 0x30 + 0x24) = rnd(state, 7);
    setArg(ctx, 0, effect);
}

// The start lights' handler: the countdown's ring and position, its life anywhere from 0 to 63 (the sounds play at 22,
// the music at 6 and 2), its number 0 to 3 (now and then another).
void vfxReadySteadyGoHandler(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    const uint32_t effect = ringEffect(state, scratch, 15, 0x48, 0x14, 0x10, 0x20);
    g_split.set(0x1edc68u, static_cast<uint8_t>(rnd(state, 2)));
    for (uint32_t offset = 0x188; offset < 0x1a4; offset += 2)
        mem<uint16_t>(effect + offset) = static_cast<uint16_t>(rnd(state, 2) ? rnd(state, 0x400) : nextFuzz(state));
    mem<int32_t>(effect + 0x1a4) = static_cast<int32_t>(rnd(state, 8) ? rnd(state, 4) : nextFuzz(state));
    static constexpr uint16_t kLives[] = {0x3f, 0x16, 0x10, 6, 2};
    mem<uint16_t>(effect + 0x10) = static_cast<uint16_t>(rnd(state, 2) ? kLives[rnd(state, 5)] : rnd(state, 0x40));
    setArg(ctx, 0, effect);
}

// The rocket car's handler: the renderer's effect (vfxRocketCar) with the car's aerial in scratch and the ring sometimes full.
void vfxRocketCarHandler(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    vfxRocketCar(ctx, state, scratch);
    aerialCar(state, scratch);
    const uint32_t effect = scratch + kEffect;
    if (rnd(state, 4) == 0)
        mem<uint8_t>(effect + 0x41) = 15;
    setArg(ctx, 0, effect);
}

// The take-terror renderer: the handler's effect with an aerial car, near the camera, invincible now and then.
void vfxTakeTerrorRenderer(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    vfxTakeTerror(ctx, state, scratch);
    aerialCar(state, scratch);
    nearCar(state, scratch);
    renderState(state, scratch);
    mem<int8_t>(scratch + 0x19c) = static_cast<int8_t>(rnd(state, 4) ? 0 : nextFuzz(state));
    const uint32_t effect = scratch + kEffect;
    mem<uint16_t>(effect + 0x10) = static_cast<uint16_t>(rnd(state, 4) ? rnd(state, 0x38) : nextFuzz(state));
    setArg(ctx, 0, effect);
}

// The bomb's renderer: the handler's effect near the camera (vCamPos, 24.8) most of the time, a target (shorts at +0x2c8).
void vfxBombRenderer(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    vfxBomb(ctx, state, scratch);
    const uint32_t realRes = G(visFxVarG).pVfxRes.address;
    const uint32_t realShape = realRes ? mem<uint32_t>(realRes + 0x80) : 0; // pWEPbouncy: the level's real bomb shape
    renderState(state, scratch);
    if (realShape)
        mem<uint32_t>(scratch + kRes + 0x80) = realShape;
    else
    {
        mem<uint32_t>(scratch + kRes + 0x80) = scratch + kAuxA; // a shape that is not drawn (version not 2 or 3)
        mem<float>(scratch + kAuxA) = 1.0f;
    }
    const uint32_t effect = scratch + kEffect;
    for (uint32_t axis = 0; axis < 3; ++axis)
    {
        const int32_t camera = mem<int32_t>(0x1f10fcu + 4 * axis) >> 8;
        if (rnd(state, 4))
            mem<int16_t>(effect + 0x14 + 2 * axis) = static_cast<int16_t>(camera + static_cast<int32_t>(rnd(state, 16000)) - 8000);
        mem<int16_t>(effect + 0x2c8 + 2 * axis) = static_cast<int16_t>(mem<int16_t>(effect + 0x14 + 2 * axis) + static_cast<int32_t>(rnd(state, 2000)) - 1000);
    }
    mem<uint16_t>(effect + 0x2c4) = static_cast<uint16_t>(nextFuzz(state));
    setArg(ctx, 0, effect);
}

// The aerial lightning and damage lightning: the car (with an aerial) at +0x78, the shake offsets (shorts from +0x40).
void vfxAerialLightning(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    if (!buildCar(ctx, state, scratch))
        return;
    aerialCar(state, scratch);
    nearCar(state, scratch);
    renderState(state, scratch);
    const uint32_t effect = scratch + kEffect;
    mem<uint32_t>(effect + 0x78) = scratch;
    for (uint32_t offset = 0x40; offset < 0x78; offset += 2)
        mem<int16_t>(effect + offset) = static_cast<int16_t>(rnd(state, 4) ? static_cast<int32_t>(rnd(state, 64)) - 32 : static_cast<int32_t>(nextFuzz(state)));
    mem<uint16_t>(effect + 0x10) = static_cast<uint16_t>(rnd(state, 4) ? rnd(state, 0x20) : nextFuzz(state));
    setArg(ctx, 0, effect);
}

// The boat's wake: a ring of 16 cross-sections of 0x1c bytes (two SVECTORs, an on-water flag at +0x18, an age at +0x1a),
// the effect's sort position near the camera.
void vfxBoatWake(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    const uint32_t effect = ringEffect(state, scratch, 15, 0x44, 0x1c, 0x1a, 0x20);
    for (uint32_t i = 0; i < 16; ++i)
    {
        const uint32_t p = effect + 0x44 + i * 0x1c;
        for (uint32_t offset = 0; offset < 0x10; offset += 2)
            mem<int16_t>(p + offset) = static_cast<int16_t>(static_cast<int32_t>(rnd(state, 4000)) - 2000);
        mem<uint8_t>(p + 0x18) = static_cast<uint8_t>(rnd(state, 4) ? 1 : 0);
    }
    for (uint32_t axis = 0; axis < 4; ++axis)
        mem<float>(effect + 0x20 + 4 * axis) = mem<float>(0x1f1160u + 4 * axis) + frnd(state, 3000.0f);
    setArg(ctx, 0, effect);
}

// The boat's foam: the car (now and then of type 0x22, or not a boat) at +0x40 with its two water tests passing most of
// the time (the wheel flags for the smaller types, the depth floats of a record at +0x1e0 of the physics), the water
// heights of the wheels (now and then missing), its shape header's bounds, the cast-down flag and a shadow texture.
void vfxShadowFoam(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    if (!buildCar(ctx, state, scratch))
        return;
    restoreGlobals();
    renderState(state, scratch);
    nearCar(state, scratch);
    _sObject &car = *static_cast<_sObject *>(guest_host(scratch));
    _sObjectDef &def = *static_cast<_sObjectDef *>(guest_host(scratch + kDef));
    def = *car.pDef;
    const uint32_t kind = rnd(state, 8);
    def.type = static_cast<eObjectType>(kind == 0 ? 0x22 : kind < 4 ? rnd(state, 0x14) : 0x14 + rnd(state, 8));
    car.pDef = ptr32<_sObjectDef>(scratch + kDef);
    const uint32_t physics = car.pPhysics.address;
    mem<uint16_t>(physics + 0xe8) = static_cast<uint16_t>(rnd(state, 4) ? 1 : nextFuzz(state));
    mem<uint16_t>(physics + 0x130) = static_cast<uint16_t>(rnd(state, 4) ? 1 : nextFuzz(state));
    mem<uint32_t>(physics + 0x1e0) = scratch + kVectors;
    mem<float>(scratch + kVectors + 0x78) = rnd(state, 4) ? 1.0f + frnd(state, 1.0f) * frnd(state, 1.0f) : frnd(state, 5.0f);
    mem<float>(scratch + kVectors + 0x208) = rnd(state, 4) ? 2.0f : frnd(state, 5.0f);
    for (uint32_t w = 0; w < 4; ++w)
        mem<float>(physics + w * 0x48 + 0xec) = rnd(state, 10) ? frnd(state, 3000.0f) : -40000.0f;
    mem<uint32_t>(scratch + 0x18) = scratch + kAuxA; // the render object, whose first word is the shape header
    mem<uint32_t>(scratch + kAuxA) = scratch + kAuxB;
    for (uint32_t offset = 0x68; offset < 0x74; offset += 2)
        mem<int16_t>(scratch + kAuxB + offset) = static_cast<int16_t>(static_cast<int32_t>(rnd(state, 400)) - 200);
    mem<uint32_t>(scratch + 0x1d8) = rnd(state, 6) ? 1 : 0;
    mem<uint32_t>(scratch + 0x34) = someTexture(state, scratch);
    mem<int32_t>(mem<uint32_t>(physics + 0x1e8) + 0x24) = static_cast<int32_t>(rnd(state, 4) ? rnd(state, 0x6000) : nextFuzz(state));
    const uint32_t effect = scratch + kEffect;
    mem<uint32_t>(effect + 0x40) = scratch;
    for (uint32_t i = 0; i < 4; ++i)
        mem<uint8_t>(effect + 0x44 + i) = static_cast<uint8_t>(nextFuzz(state));
    setArg(ctx, 0, effect);
}


// --- the eighth batch: handlers whose renderers are native, and five renderers ----------------------------------------

namespace {

constexpr uint32_t kBoatRecord = 0x2400u;  // a boat's float records (physics +0x1e0): 4 of 0x190 bytes
constexpr uint32_t kCameraRecord = 0x2a00u; // a camera record (cameraVarG.pCamera)

uint32_t carState(uint32_t scratch) { return mem<uint32_t>(mem<uint32_t>(scratch + 0x20) + 0x1e8); }

// The car's player number 0 to 3 and that player's state word (sNew, sOld), random.
void playerState(uint64_t &state, uint32_t scratch)
{
    const uint32_t player = rnd(state, 4);
    mem<uint8_t>(scratch + 0x45) = static_cast<uint8_t>(player);
    g_playerState.set(0x1f37e0u + 4 * player, nextFuzz(state));
}

// A car or boat (types below 0x14, now and then 0xf, 0x11 or 0x1a, or a boat from 0x14), its wheel flags random and its
// boat records (physics +0x1e0) in scratch with depths often above zero.
void carOrBoat(uint64_t &state, uint32_t scratch)
{
    _sObject &car = *static_cast<_sObject *>(guest_host(scratch));
    _sObjectDef &def = *static_cast<_sObjectDef *>(guest_host(scratch + kDef));
    def = *car.pDef;
    static constexpr uint32_t kTypes[] = {0xf, 0x11, 0x1a, 0x24};
    const uint32_t kind = rnd(state, 8);
    def.type = static_cast<eObjectType>(kind < 3 ? rnd(state, 0x14) : kind < 5 ? 0x14 + rnd(state, 6) : kind < 7 ? kTypes[rnd(state, 4)] : nextFuzz(state) % 0x30u);
    car.pDef = ptr32<_sObjectDef>(scratch + kDef);
    const uint32_t physics = mem<uint32_t>(scratch + 0x20);
    for (uint32_t w = 0; w < 4; ++w)
        mem<uint16_t>(physics + 0xe8 + w * 0x48) = static_cast<uint16_t>(nextFuzz(state));
    mem<uint32_t>(physics + 0x1e0) = scratch + kBoatRecord;
    for (uint32_t offset = 0; offset < 0x640; offset += 4)
        mem<float>(scratch + kBoatRecord + offset) = rnd(state, 3) ? frnd(state, 50.0f) : 0.0f;
}

// The quake handlers' effect: the wave's ring (31 particles of 0x30 bytes from +0x50, word lives near zero), the
// object (the car) at +0x650, the centre near the car (shorts at +0x654) and its velocity (+0x65c); the first frame
// (life 32) a third of the time when there is collision to cast on.
uint32_t quakeHandler(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    if (!buildCar(ctx, state, scratch))
        return 0;
    const uint32_t effect = scratch + kEffect;
    buildRing(state, effect, 31, effect + 0x50, 0x30, 0x20, 4);
    for (uint32_t i = 0; i < 32; ++i)
        for (uint32_t lane = 0; lane < 0x20; lane += 4)
            mem<float>(effect + 0x50 + i * 0x30 + lane) = frnd(state, 200.0f);
    mem<uint32_t>(effect + 0x650) = scratch;
    const uint32_t car = carState(scratch);
    for (uint32_t axis = 0; axis < 3; ++axis)
    {
        mem<int16_t>(effect + 0x654 + 2 * axis) = static_cast<int16_t>((mem<int32_t>(car + 4 + 4 * axis) >> 8) + static_cast<int32_t>(rnd(state, 400)) - 200);
        mem<int16_t>(effect + 0x65c + 2 * axis) = static_cast<int16_t>(rnd(state, 4) ? static_cast<int32_t>(rnd(state, 200)) - 100 : static_cast<int32_t>(nextFuzz(state)));
    }
    mem<uint16_t>(effect + 0x10) = static_cast<uint16_t>(haveCollisionGrid() && rnd(state, 3) == 0 ? 0x20 : rnd(state, 2) ? rnd(state, 0x20) : nextFuzz(state));
    return effect;
}

} // namespace

// Both quake handlers.
void vfxQuakeHandler(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    if (const uint32_t effect = quakeHandler(ctx, state, scratch))
        setArg(ctx, 0, effect);
}

// The oil dump's handler: blobs of 0x18 bytes from +0x44 (lives at +0x14 near the 4 that marks a landed blob, big or
// small at +0x16), the car at +0x1c4, the landing flags (+0x1d0, +0x1d4) and the ground's height (+0x1ca) near the car;
// the life at 200 (twelve blobs), 185 to 199 (one) or below.
void vfxOilDumpHandler(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    if (!buildCar(ctx, state, scratch))
        return;
    const uint32_t mask = rnd(state, 2) ? 15 : 31;
    const uint32_t effect = scratch + kEffect;
    buildRing(state, effect, mask, effect + 0x44, 0x18, 0x14, 2);
    scatterParticles(state, effect + 0x44, mask + 1, 0x18, 0x14, 2, 8);
    for (uint32_t i = 0; i <= mask; ++i)
    {
        const uint32_t p = effect + 0x44 + i * 0x18;
        for (uint32_t offset : {8u, 0xau, 0xcu})
            mem<int16_t>(p + offset) = static_cast<int16_t>(static_cast<int32_t>(rnd(state, 0x400)) - 0x200);
        mem<uint8_t>(p + 0x16) = static_cast<uint8_t>(rnd(state, 3) ? (rnd(state, 2) ? 0xf : 1) : nextFuzz(state));
        if (rnd(state, 3) == 0)
            mem<uint16_t>(p + 0x14) = static_cast<uint16_t>(rnd(state, 2) ? 0x400 : 8);
    }
    mem<uint32_t>(effect + 0x1c4) = scratch;
    mem<uint32_t>(effect + 0x1d0) = rnd(state, 3) == 0 ? 1 : 0;
    mem<uint32_t>(effect + 0x1d4) = rnd(state, 3);
    const uint32_t car = carState(scratch);
    for (uint32_t axis = 0; axis < 3; ++axis)
        mem<int16_t>(effect + 0x1c8 + 2 * axis) = static_cast<int16_t>((mem<int32_t>(car + 4 + 4 * axis) >> 8) + static_cast<int32_t>(rnd(state, 200)) - 100);
    const uint32_t kind = rnd(state, 4);
    mem<uint16_t>(effect + 0x10) = static_cast<uint16_t>(kind == 0 ? 200 : kind == 1 ? 0xb9 + rnd(state, 15) : kind == 2 ? rnd(state, 0xb9) : nextFuzz(state));
    setArg(ctx, 0, effect);
}

// The skid marks' handler: a car or boat (the material under each wheel random, now and then deep water 0x23), the
// wheel (+0x188), the chain flag, the frame counter and the forced material (+0x189 to +0x18b), the player's state, the
// speed for the dust; a ring of 16 marks.
void vfxCarSkidMarkHandler(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    if (!buildCar(ctx, state, scratch))
        return;
    carOrBoat(state, scratch);
    nearCar(state, scratch);
    playerState(state, scratch);
    const uint32_t physics = mem<uint32_t>(scratch + 0x20);
    for (uint32_t w = 0; w < 4; ++w)
        mem<uint8_t>(physics + 0xea + w * 0x48) = static_cast<uint8_t>(rnd(state, 4) == 0 ? 0x23 : rnd(state, 4) ? rnd(state, 0x48) : nextFuzz(state));
    mem<float>(physics + 0x38) = rnd(state, 2) ? 4000.0f + frnd(state, 2000.0f) : frnd(state, 10000.0f);
    const uint32_t effect = scratch + kEffect;
    buildRing(state, effect, 15, effect + 0x44, 0x14, 0x10, 2);
    mem<uint32_t>(effect + 0x184) = scratch;
    mem<uint8_t>(effect + 0x188) = static_cast<uint8_t>(rnd(state, 4));
    mem<uint8_t>(effect + 0x189) = static_cast<uint8_t>(rnd(state, 2));
    mem<uint8_t>(effect + 0x18a) = static_cast<uint8_t>(nextFuzz(state));
    static constexpr uint8_t kForced[] = {0, 0, 1, 2, 4, 3};
    mem<uint8_t>(effect + 0x18b) = kForced[rnd(state, 6)];
    setArg(ctx, 0, effect);
}

// The water splash's handler: as its renderer's (a ring of 64), with the first frame (life 50) a third of the time near
// the car (when there is collision to cast on).
void vfxWaterSplooshHandler(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    if (!buildCar(ctx, state, scratch))
        return;
    vfxWaterSploosh(ctx, state, scratch);
    const uint32_t effect = scratch + kEffect;
    const uint32_t car = carState(scratch);
    for (uint32_t axis = 0; axis < 4; ++axis)
        mem<int16_t>(effect + 0x544 + 2 * axis) =
            static_cast<int16_t>(axis < 3 ? (mem<int32_t>(car + 4 + 4 * axis) >> 8) + static_cast<int32_t>(rnd(state, 400)) - 200 : static_cast<int32_t>(nextFuzz(state)));
    mem<uint16_t>(effect + 0x10) = static_cast<uint16_t>(haveCollisionGrid() && rnd(state, 3) == 0 ? 0x32 : rnd(state, 0x40));
    mem<uint8_t>(effect + 0x40) = 63; // the game's own ring: the first frame fills 63 drops whatever the mask, so a smaller one never ends
    setArg(ctx, 0, effect);
}

// The car splash's handler: the renderer's ring (16, 32 or 64 drops), the car at +0x544 and its player's state, the
// first frame (life 80) a third of the time.
void vfxCarSplashHandler(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    if (!buildCar(ctx, state, scratch))
        return;
    vfxCarSplash(ctx, state, scratch);
    playerState(state, scratch);
    const uint32_t effect = scratch + kEffect;
    mem<uint32_t>(effect + 0x544) = scratch;
    for (uint32_t axis = 0; axis < 3; ++axis)
        mem<int16_t>(effect + 0x14 + 2 * axis) = static_cast<int16_t>(static_cast<int32_t>(nextFuzz(state) % 8193u) - 4096 + (axis == 1 ? 4096 : 0));
    mem<int16_t>(effect + 0x54a) = static_cast<int16_t>((mem<int32_t>(carState(scratch) + 8) >> 8) + static_cast<int32_t>(rnd(state, 400)) - 200);
    mem<uint16_t>(effect + 0x10) = static_cast<uint16_t>(haveCollisionGrid() && rnd(state, 3) == 0 ? 0x50 : rnd(state, 0x58));
    setArg(ctx, 0, effect);
}

// The boat wake's handler: a car or boat that passes the water tests often (moving: the speed word at +0x24 of the
// integer state; low over the water: physics +0x18 near the float at +0x1f0; the back wheels in it), its player's state,
// now and then falling fast, the frame counter (+0x20a) and joined flag (+0x209); 16 sections of 0x1c bytes.
void vfxBoatWakeHandler(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    if (!buildCar(ctx, state, scratch))
        return;
    carOrBoat(state, scratch);
    nearCar(state, scratch);
    playerState(state, scratch);
    mem<uint8_t>(scratch + 0x44) = static_cast<uint8_t>(rnd(state, 4));
    const uint32_t physics = mem<uint32_t>(scratch + 0x20), car = carState(scratch);
    mem<int32_t>(car + 0x24) = static_cast<int32_t>(rnd(state, 4) ? rnd(state, 0x1000) : nextFuzz(state));
    mem<int16_t>(car + 0x1e) = static_cast<int16_t>(rnd(state, 3) == 0 ? -0xdad - static_cast<int32_t>(rnd(state, 100)) : static_cast<int32_t>(nextFuzz(state)));
    mem<float>(physics + 0x18) = (mem<float>(scratch + 0x1f0) + (rnd(state, 4) ? frnd(state, 150.0f) : frnd(state, 1000.0f))) * 256.0f;
    const uint32_t effect = scratch + kEffect;
    buildRing(state, effect, 15, effect + 0x44, 0x1c, 0x1a, 2);
    for (uint32_t i = 0; i < 16; ++i)
    {
        const uint32_t p = effect + 0x44 + i * 0x1c;
        for (uint32_t offset = 0; offset < 0x18; offset += 2)
            mem<int16_t>(p + offset) = static_cast<int16_t>(static_cast<int32_t>(rnd(state, 4000)) - 2000);
        mem<uint8_t>(p + 0x19) = static_cast<uint8_t>(rnd(state, 2));
    }
    mem<uint32_t>(effect + 0x204) = scratch;
    mem<uint8_t>(effect + 0x209) = static_cast<uint8_t>(rnd(state, 2));
    mem<uint16_t>(effect + 0x20a) = static_cast<uint16_t>(nextFuzz(state));
    setArg(ctx, 0, effect);
}

// The aerial's handler: an aerial car (the effect at +0xb0 is the car), the reset flag (+0xb4) a quarter of the time, the
// bend and its speed (+0xc0, +0xd0) random, the life around 10.
void vfxAerialHandler(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    if (!buildCar(ctx, state, scratch))
        return;
    aerialCar(state, scratch);
    const uint32_t effect = scratch + kEffect;
    for (uint32_t offset = 0x40; offset < 0xe0; offset += 4)
        mem<float>(effect + offset) = frnd(state, offset >= 0xd0 ? 400.0f : 300.0f);
    mem<uint32_t>(effect + 0xb0) = scratch;
    mem<uint32_t>(effect + 0xb4) = rnd(state, 4) == 0 ? 1 : 0;
    mem<uint16_t>(effect + 0x10) = static_cast<uint16_t>(rnd(state, 4) ? rnd(state, 0x14) : nextFuzz(state));
    setArg(ctx, 0, effect);
}

// The spin pickup's handler: the renderer's effect, the first frame (63) or the last (1) often, a weapon kind of 0 to
// 20 (now and then any byte) at +0xfc, the car firing or not.
void vfxSpinPickupHandler(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    vfxSpinPickup(ctx, state, scratch);
    const uint32_t effect = scratch + kEffect;
    if (mem<uint32_t>(effect + 0x44) != scratch)
        return;
    const uint32_t kind = rnd(state, 4);
    mem<uint16_t>(effect + 0x10) = static_cast<uint16_t>(kind == 0 ? 0x3f : kind == 1 ? 1 : kind == 2 ? rnd(state, 0x40) : nextFuzz(state));
    mem<uint8_t>(effect + 0xfc) = static_cast<uint8_t>(rnd(state, 8) ? rnd(state, 21) : nextFuzz(state));
    mem<int8_t>(scratch + 0x1a3) = static_cast<int8_t>(rnd(state, 2) ? 0 : 1);
    _sObject &car = *static_cast<_sObject *>(guest_host(scratch));
    if (car.pDef.address != scratch + kDef)
    {
        _sObjectDef &def = *static_cast<_sObjectDef *>(guest_host(scratch + kDef));
        def = *car.pDef;
        car.pDef = ptr32<_sObjectDef>(scratch + kDef);
    }
    static_cast<_sObjectDef *>(guest_host(scratch + kDef))->type = static_cast<eObjectType>(rnd(state, 2) ? 0x24 : rnd(state, 0x30));
    setArg(ctx, 0, effect);
}

// The respawn's handler: the renderer's effect with the life at 100 (the burst), 34 or 33 (the car put back), 1 or 0,
// or anything.
void vfxCarRespawnHandler(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    vfxCarRespawn(ctx, state, scratch);
    const uint32_t effect = scratch + kEffect;
    if (mem<uint32_t>(effect + 0x2c4) != scratch)
        return;
    static constexpr uint16_t kLives[] = {100, 0x22, 0x21, 1, 0x40};
    mem<uint16_t>(effect + 0x10) = static_cast<uint16_t>(rnd(state, 4) ? kLives[rnd(state, 5)] : nextFuzz(state));
    mem<uint16_t>(effect + 0x2ca) = static_cast<uint16_t>(nextFuzz(state));
    mem<int16_t>(scratch + 0x172) = static_cast<int16_t>(rnd(state, 2) ? 0 : nextFuzz(state));
    setArg(ctx, 0, effect);
}

// The spray's handler: the renderer's effect, a car or boat passing the spray's tests often (near, fast, low over the
// water, the back floats in it), its player's state.
void vfxSprayHandler(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    vfxSprayRenderer(ctx, state, scratch);
    const uint32_t effect = scratch + kEffect;
    if (mem<uint32_t>(effect + 0x44) != scratch)
        return;
    carOrBoat(state, scratch);
    playerState(state, scratch);
    const uint32_t physics = mem<uint32_t>(scratch + 0x20);
    mem<float>(physics + 0x38) = rnd(state, 4) ? 2000.0f + frnd(state, 1.0f) * frnd(state, 30000.0f) : frnd(state, 3000.0f);
    if (mem<float>(physics + 0x38) < 0.0f)
        mem<float>(physics + 0x38) = -mem<float>(physics + 0x38);
    mem<float>(physics + 0x18) = (mem<float>(scratch + 0x1f0) + (rnd(state, 4) ? frnd(state, 150.0f) : frnd(state, 1000.0f))) * 256.0f;
    for (uint32_t i = 0; i < 16; ++i)
    {
        const uint32_t p = effect + 0x48 + i * 0x1c;
        for (uint32_t offset = 0; offset < 0x18; offset += 2)
            if (offset != 0x12 || rnd(state, 2))
                mem<int16_t>(p + offset) = static_cast<int16_t>(static_cast<int32_t>(rnd(state, 200)) - 100);
    }
    setArg(ctx, 0, effect);
}

// The give-terror handler: a ring of 32 sparks, the car at +0x2cc, the life at 96 (the burst and the take-terror effects
// for the race's other cars) or 48 (their weapons removed), or anything.
void vfxGiveTerrorHandler(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    if (!buildCar(ctx, state, scratch))
        return;
    const uint32_t effect = scratch + kEffect;
    buildRing(state, effect, 31, effect + 0x44, 0x14, 0x10, 2);
    scatterParticles(state, effect + 0x44, 32, 0x14, 0x10, 2, 0x40);
    mem<uint32_t>(effect + 0x2cc) = scratch;
    mem<uint16_t>(effect + 0x2d0) = static_cast<uint16_t>(nextFuzz(state));
    mem<uint8_t>(scratch + 0x45) = static_cast<uint8_t>(rnd(state, 8));
    const uint32_t kind = rnd(state, 3);
    mem<uint16_t>(effect + 0x10) = static_cast<uint16_t>(kind == 0 ? 0x60 : kind == 1 ? 0x30 : rnd(state, 2) ? rnd(state, 0x60) : nextFuzz(state));
    setArg(ctx, 0, effect);
}

// The give-terror renderer: the handler's effect with an aerial car near the camera, the life anywhere up to 0x70.
void vfxGiveTerrorRenderer(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    vfxGiveTerrorHandler(ctx, state, scratch);
    const uint32_t effect = scratch + kEffect;
    if (mem<uint32_t>(effect + 0x2cc) != scratch)
        return;
    aerialCar(state, scratch);
    nearCar(state, scratch);
    renderState(state, scratch);
    mem<uint16_t>(effect + 0x10) = static_cast<uint16_t>(rnd(state, 4) ? rnd(state, 0x70) : nextFuzz(state));
    setArg(ctx, 0, effect);
}

// The heat ray blast: an aerial car (the car at +0x40; its type picks the aim) whose render object holds a position and
// a matrix, a camera near it or far, the counter (+0x48) and the life.
void vfxCarHeatRayBlast(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    if (!buildCar(ctx, state, scratch))
        return;
    carOrBoat(state, scratch);
    aerialCar(state, scratch);
    nearCar(state, scratch);
    renderState(state, scratch);
    const uint32_t rendObj = scratch + kAuxB;
    for (uint32_t offset = 0x18; offset < 0x2a; offset += 2)
        mem<int16_t>(rendObj + offset) = static_cast<int16_t>(static_cast<int32_t>(rnd(state, 0x2001)) - 0x1000);
    g_camera.set(0x1eab20u, scratch + kCameraRecord);
    for (uint32_t axis = 0; axis < 3; ++axis)
        mem<int32_t>(scratch + kCameraRecord + 0xc + 4 * axis) =
            mem<int32_t>(rendObj + 4 + 4 * axis) + (rnd(state, 2) ? static_cast<int32_t>(rnd(state, 0x100000)) - 0x80000 : static_cast<int32_t>(nextFuzz(state) >> 4));
    const uint32_t effect = scratch + kEffect;
    mem<uint32_t>(effect + 0x40) = scratch;
    mem<uint16_t>(effect + 0x48) = static_cast<uint16_t>(nextFuzz(state));
    mem<uint16_t>(effect + 0x10) = static_cast<uint16_t>(nextFuzz(state));
    setArg(ctx, 0, effect);
}

// The water-covered renderer: the water ball's drops on an aerial car near the camera, its shape's bounds (+0x6c, +0x72
// of the header) and the life low (the glint's and the sheet's fades).
void vfxCarWaterCoveredRenderer(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    if (!buildCar(ctx, state, scratch))
        return;
    restoreGlobals();
    nearCar(state, scratch);
    renderState(state, scratch);
    const uint32_t effect = waterDrops(state, scratch);
    mem<uint32_t>(scratch + kAuxB) = scratch + kNormal; // the render object's shape header
    for (uint32_t offset = 0x68; offset <= 0x72; offset += 2)
        mem<int16_t>(scratch + kNormal + offset) = static_cast<int16_t>(static_cast<int32_t>(rnd(state, 801)) - 400);
    for (uint32_t i = 0; i < 32; ++i)
        if (rnd(state, 2))
            mem<uint32_t>(effect + 0x50 + i * 0x30 + 0x24) = rnd(state, 7);
    mem<uint16_t>(effect + 0x10) = static_cast<uint16_t>(rnd(state, 4) ? rnd(state, 0x14) : nextFuzz(state));
    setArg(ctx, 0, effect);
}

// The invincibility renderer: the handler's car and shape, near the camera, its ring of 32 points (+0x50), the colour
// flag (+0x254) and the life (0xffff a third of the time in the handler's setup; here low or anything).
void vfxInvincibleRenderer(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    vfxInvincible(ctx, state, scratch);
    const uint32_t effect = scratch + kEffect;
    if (mem<uint32_t>(effect + 0x250) != scratch)
        return;
    restoreGlobals();
    nearCar(state, scratch);
    renderState(state, scratch);
    for (uint32_t i = 0; i < 32; ++i)
        for (uint32_t lane = 0; lane < 16; lane += 4)
            mem<float>(effect + 0x50 + i * 0x10 + lane) = frnd(state, 300.0f);
    mem<uint32_t>(effect + 0x254) = rnd(state, 2) ? 0 : nextFuzz(state);
    const uint32_t kind = rnd(state, 4);
    mem<uint16_t>(effect + 0x10) = static_cast<uint16_t>(kind == 0 ? 0xffff : kind == 1 ? rnd(state, 0x100) : nextFuzz(state));
    setArg(ctx, 0, effect);
}

// The finish position's renderer: the handler's effect near the camera, the level record's table of position textures
// (pLvlG +0x40, records from +0x20), the position 0 to 4 (now and then -1), the camera's glide flag (+0xac).
void vfxFinishPositionRenderer(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    vfxFinishPosition(ctx, state, scratch);
    const uint32_t effect = scratch + kEffect;
    if (mem<uint32_t>(effect + 0x44) != scratch)
        return;
    restoreGlobals();
    nearCar(state, scratch);
    renderState(state, scratch);
    g_levelRecord.set(0x327e80u, scratch + kAuxC);
    mem<uint32_t>(scratch + kAuxC + 0x40) = scratch + kAuxB;
    for (uint32_t i = 0; i < 6; ++i)
        mem<uint32_t>(scratch + kAuxB + 0x20 + 4 * i) = scratch + kAuxA + 0x10 * i;
    for (uint32_t i = 0; i < 6; ++i)
        mem<uint32_t>(scratch + kAuxA + 0x10 * i) = someTexture(state, scratch);
    g_camera.set(0x1eab20u, scratch + kCameraRecord);
    mem<uint32_t>(scratch + kCameraRecord + 0xac) = rnd(state, 2) ? 0 : 1 + rnd(state, 4);
    mem<int32_t>(effect + 0x1a4) = rnd(state, 8) ? static_cast<int32_t>(rnd(state, 5)) : -1;
    setArg(ctx, 0, effect);
}


// ---------------------------------------------------------------------------------------------------------------------
// The last batch (visfx12.cpp): the effect constructor, the effects task, the draw lists, the scenery tags, the podium,
// the sky and the rain. All of them compare and restore all of RAM (RC_DECOMP_FN_SETUP_HEAP); several run on the
// dump's real effect lists, cars and level.

namespace {

constexpr uint32_t kFxHeaderAddr = 0x1f3710u, kTsfxHeaderAddr = 0x1f3718u;
constexpr uint32_t kSkyTerrorAddr = 0x1f3728u, kResultTriggerAddr = 0x1f3730u;

// Walks a real effect list (Next at +0) and gives about one effect in `oneIn` a life of zero (VfxUpdate frees those,
// the renderers skip them). At most 400 effects are visited, in case a list is damaged.
void killSome(uint64_t &state, uint32_t header, uint32_t oneIn)
{
    uint32_t visited = 0;
    for (uint32_t fx = mem<uint32_t>(header); fx && visited < 400; fx = mem<uint32_t>(fx), ++visited)
        if (fx < 0x2000000u && rnd(state, oneIn) == 0)
            mem<uint16_t>(fx + 0x10) = 0;
}

// Moves the camera position the effects are sorted from by up to `range` units now and then (more culled or kept).
void nudgeCamera(uint64_t &state, float range)
{
    if (rnd(state, 2))
        return;
    for (uint32_t axis = 0; axis < 3; ++axis)
        mem<float>(0x1f1160u + 4 * axis) += frnd(state, range);
}

// A random real object slot (0 to the object count, now and then one past it).
uint32_t objectSlot(uint64_t &state)
{
    const uint32_t count = G(objectVarG).noObjects;
    return count == 0 ? 0u : rnd(state, count + 1);
}

} // namespace

// a0 a car (a copy in scratch), a1/a2 parameters (a1 for the spin pickup a record with its position at +0x78), a3 a type:
// mostly the 0x38 kinds, now and then 0x2e or beyond, and now and then a placed effect (+0x400, a0..a2 a position) of a
// kind that does not reach into a car. The effect list holds nothing or one or two effects.
void vfxAddNew(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    if (!buildCar(ctx, state, scratch))
        return;
    const FxList list = buildFxList(state, scratch, rnd(state, 3), 0x60, 0, false);
    G(visFxVarG).vfxHeader = ptr32<_sVfx_Object>(list.head);
    G(visFxVarG).vfxFooter = ptr32<_sVfx_Object>(list.foot);
    const uint32_t pickup = scratch + kAuxA;
    for (uint32_t offset = 0x78; offset <= 0x80; offset += 4)
        mem<float>(pickup + offset) = frnd(state, 3000.0f);
    uint32_t type = rnd(state, 10) ? rnd(state, 0x38) : rnd(state, 0x60);
    const uint32_t y = type == 0x15 ? pickup : (rnd(state, 2) ? rnd(state, 8) : nextFuzz(state));
    const uint32_t z = rnd(state, 2) ? rnd(state, 0x40) : nextFuzz(state);
    const bool reachesCar = type == 3 || type == 0x15 || type == 0x1b || type == 0x24 || type == 0x30;
    if (!reachesCar && rnd(state, 4) == 0)
    {
        setArg(ctx, 0, static_cast<uint32_t>(static_cast<int32_t>(rnd(state, 0x10000)) - 0x8000));
        setArg(ctx, 1, static_cast<uint32_t>(static_cast<int32_t>(rnd(state, 0x10000)) - 0x8000));
        setArg(ctx, 2, static_cast<uint32_t>(static_cast<int32_t>(rnd(state, 0x10000)) - 0x8000));
        setArg(ctx, 3, type + 0x400);
        return;
    }
    setArg(ctx, 0, scratch);
    setArg(ctx, 1, y);
    setArg(ctx, 2, z);
    setArg(ctx, 3, type);
}

// The effects task on the dump's real state: some cars lose their attachments (they are made again), the car viewer's
// mode now and then, some effects' lives end (they are freed), the players' old states and the sky terror vary, and a1 is
// a time.
void vfxUpdate(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    restoreGlobals();
    (void)scratch;
    const bool reattach = rnd(state, 2) == 0; // otherwise the list's footer is an old effect, which may be freed
    for (uint32_t i = 0; i < G(objectVarG).noObjects && i < 0x5c; ++i)
    {
        const uint32_t obj = mem<uint32_t>(0x1ee390u + 4 * i);
        if (reattach && obj && obj < 0x2000000u && rnd(state, 3) == 0)
            mem<uint8_t>(obj + 0x177) = 0;
        mem<int16_t>(0x1f37e2u + 4 * i) = static_cast<int16_t>(rnd(state, 2) ? 0 : nextFuzz(state));
    }
    if (rnd(state, 4) == 0)
        G(frontEndVarG).active = 5;
    killSome(state, kFxHeaderAddr, 4);
    const uint32_t terror = rnd(state, 3);
    mem<int32_t>(kSkyTerrorAddr) = terror == 0 ? 0 : terror == 1 ? 0x7f : static_cast<int32_t>(rnd(state, 0x80));
    setArg(ctx, 0, nextFuzz(state));
    setArg(ctx, 1, nextFuzz(state) >> rnd(state, 16));
}

// The effect draw list on the dump's real effects: some lives ended, the camera now and then moved.
void vfxCallRenderers(R5900Context *, uint64_t &state, uint32_t)
{
    restoreGlobals();
    seedRand(state);
    killSome(state, kFxHeaderAddr, 5);
    nudgeCamera(state, 3000.0f);
}

// The scenery effect draw list on the dump's real scenery effects, the camera now and then moved.
void vfxRenderTSFX(R5900Context *, uint64_t &state, uint32_t)
{
    restoreGlobals();
    seedRand(state);
    nudgeCamera(state, 3000.0f);
}

// a0 a scenery instance, a1 a tag: mostly one of the tags that start an effect (or a rain area, or two effects) or a
// neighbour of one, now and then anything. The scenery list holds nothing or up to three effects.
void vfxCallBack(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    static constexpr uint32_t kTags[] = {0x3e8, 0x3e9, 0x3ea, 0x3eb, 0x3ec, 0x3ed, 0x3ee, 0x3ef, 0x3f0, 0x3f1, 0x3f2, 0x1000023a,
                                         0x10000244, 0x10000245, 0x1000025e, 0x10000264, 0x10000265, 0x10000269, 0x1000026a,
                                         0x1000026b, 0x1000026c, 0x1000026d, 0x1000026e, 0x1000026f, 0x10000270, 0x10000271,
                                         0x10000272, 0x10000273, 0x10000274, 0x10000275, 0x10000276, 0x10000277, 0x10000278,
                                         0x10000279, 0x1000027a, 0x1000027b, 0x1000027c, 0x1000027d, 0x1000027e, 0x1000027f,
                                         0x10000280, 0x10000281, 0x1000034e, 0x1000035c, 0x1000036f, 0x10000374, 0x10000428,
                                         0x10000457, 0x10000476, 0x1000047b, 0x10000480, 0x100004bd, 0x100004d6, 0x100004d8,
                                         0x100004d9, 0x100004ef, 0x100004f2, 0x10000501, 0x10000540, 0x100005d4, 0x100005f1,
                                         0x100005f3, 0x100005f4, 0x10000623, 0x10000649, 0x10000678, 0x10000679};
    restoreGlobals();
    const FxList list = buildFxList(state, scratch, rnd(state, 4), 0x60, 0, false);
    G(visFxVarG).vfxTSFXHeader = ptr32<_sVfx_TSFX>(list.head);
    G(visFxVarG).vfxTSFXFooter = ptr32<_sVfx_TSFX>(list.foot);
    uint32_t tag = kTags[rnd(state, std::size(kTags))];
    const uint32_t kind = rnd(state, 8);
    if (kind == 0)
        tag += 1;
    else if (kind == 1)
        tag -= 1;
    else if (kind == 2)
        tag = rnd(state, 2) ? nextFuzz(state) : 0x10000000u + rnd(state, 0x800);
    setArg(ctx, 0, scratch + kTobInst);
    setArg(ctx, 1, tag);
}

// The podium: the result count near each of its moments (the car releases, the firework rates, the menu set-up), the
// podium's car slots (0x13 to 0x17) a car copy in scratch with its hold flag set or not, or empty, and the crackle count.
// The count never reaches 0x100 here (that hands over to the front end).
void vfxPodiumPlay(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    if (!buildCar(ctx, state, scratch))
        return;
    seedRand(state);
    static constexpr int32_t kMoments[] = {0, 1, 0xf97, 0xf6f, 0xf27, 0xeeb, 0xe76, 0xe75, 0xdac, 0xdad, 0x102, 0x100, 0x50, 0x1000};
    const uint32_t kind = rnd(state, 3);
    const int32_t trigger = kind == 0 ? static_cast<int32_t>(0x102 + rnd(state, 0xf00)) : kMoments[rnd(state, std::size(kMoments))];
    mem<int32_t>(kResultTriggerAddr) = trigger;
    for (uint32_t slot = 0x13; slot <= 0x17; ++slot)
        mem<uint32_t>(0x1ee390u + 4 * slot) = rnd(state, 3) ? scratch : 0u;
    mem<uint8_t>(scratch + 0x1000 + 0x1cd) = static_cast<uint8_t>(nextFuzz(state));
    mem<int32_t>(0x328108u) = rnd(state, 2) ? static_cast<int32_t>(rnd(state, 0x22)) : static_cast<int32_t>(nextFuzz(state));
}

// The rain: up to three rain areas (scenery instances near the camera with a radius) or none, every drop in some state
// (waiting, falling, just landed), split screen 0 or 1, and the renderers' sprite, quad and textures in scratch.
void vfxRain(R5900Context *, uint64_t &state, uint32_t scratch)
{
    restoreGlobals();
    seedRand(state);
    renderState(state, scratch);
    for (uint32_t i = 0x1c; i < 0x20; ++i)
        mem<uint32_t>(scratch + kRes + 4 * i) = scratch + kTexture + 16 * rnd(state, 8);
    const uint32_t areas = rnd(state, 5) ? 1 + rnd(state, 3) : 0;
    const int32_t cam[3] = {mem<int16_t>(0x1f1108u), mem<int16_t>(0x1f110au), mem<int16_t>(0x1f110cu)};
    for (uint32_t i = 0; i < 17; ++i)
    {
        const uint32_t tob = scratch + kTobInst + 0x40 * i;
        mem<uint32_t>(0x1f1e64u + 4 * i) = i < areas ? tob : 0u;
        if (i >= areas)
            continue;
        for (uint32_t axis = 0; axis < 3; ++axis)
            mem<int32_t>(tob + 4 + 4 * axis) = (cam[axis] + static_cast<int32_t>(rnd(state, 6001)) - 3000) * 256;
        mem<uint16_t>(tob + 0x2c) = static_cast<uint16_t>(rnd(state, 4000));
    }
    mem<int32_t>(0x1edc78u) = rnd(state, 4) ? 0 : 1;
    const float camera[3] = {mem<float>(0x1f1160u), mem<float>(0x1f1164u), mem<float>(0x1f1168u)};
    for (uint32_t i = 0; i < 64; ++i)
    {
        const uint32_t p = 0x1f11c0u + 0x30 * i;
        const uint32_t life = rnd(state, 4);
        mem<int16_t>(p + 0x10) = static_cast<int16_t>(life == 0 ? 0 : life == 1 ? 1 + rnd(state, 12) : rnd(state, 3) ? rnd(state, 60) : nextFuzz(state));
        mem<int16_t>(p + 0x12) = static_cast<int16_t>(rnd(state, 0x60));
        for (uint32_t axis = 0; axis < 3; ++axis)
            mem<float>(p + 4 * axis) = camera[axis] + frnd(state, 2000.0f);
        mem<float>(p + 0x14) = 30.0f + frnd(state, 170.0f);
        mem<float>(p + 0x18) = camera[1] + frnd(state, 1000.0f);
        for (uint32_t axis = 0; axis < 3; ++axis)
            mem<int16_t>(p + 0x1c + 2 * axis) = static_cast<int16_t>(static_cast<int32_t>(rnd(state, 8193)) - 4096 + (axis == 1 ? 4096 : 0));
    }
}

// The sky terror's merged sky: its weight (0 to 0x7f), the time, and the quad in scratch.
void vfxSkyMerge(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    restoreGlobals();
    renderState(state, scratch);
    mem<int32_t>(kSkyTerrorAddr) = static_cast<int32_t>(rnd(state, 0x80));
    g_time.set(guest_addr(&G(mainVarG).time), nextFuzz(state) >> rnd(state, 12));
    setArg(ctx, 0, nextFuzz(state));
}

// The sky and the podium's texts: the sky terror (mostly off), the result count (off, or in any of its ranges), the sky
// flag, the players' places (0 to 5) and their podium cars (real object slots). The front end's level data is a record
// in scratch whose font is the one now loaded, so the texts print with a real font (no text if none is loaded).
void vfxSkyChange(R5900Context *, uint64_t &state, uint32_t scratch)
{
    restoreGlobals();
    seedRand(state);
    renderState(state, scratch);
    mem<int32_t>(kSkyTerrorAddr) = rnd(state, 3) ? 0 : static_cast<int32_t>(rnd(state, 2) ? rnd(state, 0x80) : nextFuzz(state));
    const uint32_t font = G(fontVarG).pFont.address;
    const uint32_t kind = rnd(state, 4);
    int32_t trigger = kind == 0 ? 0 : kind == 1 ? static_cast<int32_t>(rnd(state, 0x100)) : static_cast<int32_t>(0x100 + rnd(state, 0xf00));
    if (font == 0 && trigger >= 0x100)
        trigger = 0x80;
    mem<int32_t>(kResultTriggerAddr) = trigger;
    mem<uint16_t>(0x1f48e4u) = static_cast<uint16_t>(rnd(state, 3) ? 1 : 0);
    mem<uint32_t>(0x1eca58u) = scratch + kAuxB;
    mem<uint32_t>(scratch + kAuxB + 0x1c) = font;
    mem<uint16_t>(0x1e9464u) = static_cast<uint16_t>(rnd(state, 6));
    mem<uint16_t>(0x1e9466u) = static_cast<uint16_t>(rnd(state, 2) ? 0 : rnd(state, 6));
    mem<uint16_t>(0x1e9468u) = static_cast<uint16_t>(objectSlot(state));
    mem<uint16_t>(0x1e946au) = static_cast<uint16_t>(objectSlot(state));
}

} // namespace rc::fuzz
