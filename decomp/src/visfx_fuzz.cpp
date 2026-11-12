// Inputs for the random test of the small VISFX functions (RC_SHADOW_FUZZ). They use one of the race's real cars,
// copied into the scratch area by the route-node setup, and build effect objects and wheel indices around it.

#include "ai_fuzz.h"
#include "ee_math.h"

namespace rc::fuzz {

namespace {

constexpr uint32_t kEffect = 0x4000u;   // an effect object placed after the car's records (0x0..0x2300)
constexpr uint32_t kEffectSize = 0x700u;
constexpr uint32_t kDef = 0x3800u;      // a copy of the car's object definition

void setArg(R5900Context *ctx, int index, uint32_t value) { SET_GPR_S32(ctx, 4 + index, static_cast<int32_t>(value)); }
void setFloatArg(R5900Context *ctx, int index, uint32_t bits) { std::memcpy(&ctx->f[12 + index], &bits, 4); }

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

// a0 car, with the race-over/demo bits of the main state and the car's draw distances set at random.
void vfxRendered(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    if (!buildCar(ctx, state, scratch))
        return;
    _sObject &car = *static_cast<_sObject *>(guest_host(scratch));
    car.sDontRenderMe = static_cast<int16>(nextFuzz(state) % 2u ? 0 : nextFuzz(state));
    car.zDrawnDist = static_cast<uint16>(nextFuzz(state));
    car.zShortestDrawnDist = static_cast<uint16>(nextFuzz(state));
    G(mainVarG).status = static_cast<eMain>((static_cast<int32_t>(G(mainVarG).status) & ~0x30) | static_cast<int32_t>((nextFuzz(state) % 4u) << 4));
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
    G(visFxVarG).pGT4 = ptr32<_sRdrPoly>(scratch + kEffect);
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

} // namespace rc::fuzz
