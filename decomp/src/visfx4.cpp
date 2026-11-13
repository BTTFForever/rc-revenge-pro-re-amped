// Decompiled from VISFX.c, continued: more of the effects' handlers and renderers (heat ray aerial, explosion "kaboom",
// fireworks, the invincibility halo, the arm cannon, car fire, rockets, wheel dust...). They follow the layout of
// visfx.cpp: an effect is a `_sVfx_Object` whose own data starts at +0x40 with the ring header (mask, count, head, tail)
// and the particles from +0x44.

#include "ai_fuzz.h"
#include "ee_math.h"
#include "vifpk.h"
#include "visfx.h"

#include <algorithm>

namespace rc {

uint64_t VfxWheelPositionVal(ptr32<_sObject> pObj, int32 iWheel);   // visfx.cpp
uint32 VecNormalise(ptr32<SVECTOR> pOut, ptr32<SVECTOR> pVec);   // ai.cpp
ptr32<SVECTOR> VfxCarVelocityPoi(ptr32<_sObject> pObj);          // ai.cpp
ptr32<SVECTOR> VfxCarPositionPoi(ptr32<_sObject> pObj);          // ai.cpp
ptr32<SVECTOR> VfxWheelPositionPoi(ptr32<_sObject> pObj, int32 iWheel); // visfx.cpp
void VfxPutGT4vecs(ptr32<SVECTOR> svA, ptr32<SVECTOR> svB, ptr32<SVECTOR> svC, ptr32<SVECTOR> svD);
void VfxPutUV0(ee_float u, ee_float v);
void VfxPutUV1(ee_float u, ee_float v);
void VfxPutUV2(ee_float u, ee_float v);
void VfxPutUV3(ee_float u, ee_float v);
void PS2C_GteMatrixMulVec16(ptr32<_sVec16> pVec);                // psxdefs.cpp
void PS2C_SetGteMatrix(ptr32<_sMat> pMat);
void PS2C_GetVec16GteResult(ptr32<_sVec16> pOut);

namespace {

using vfx::at;
using vfx::sortPosFromCar;
using vfx::sortPosFromInts;

constexpr uint32_t kLifeSpan = 0x10;
constexpr uint32_t kRing = 0x40;      // mask, count, head, tail
constexpr uint32_t kParticles = 0x44; // the first particle

// The car's integer physics state (what the effects follow): car->pPhysics->pInteger.
uint32_t integerState(uint32_t car) { return at<uint32_t>(at<uint32_t>(car + 0x20) + 0x1e8); }

// A particle ring slot for a new particle: the ring's tail, which these effects load as an unsigned byte.
uint32_t tailParticle(uint32_t m, uint32_t stride)
{
    return m + kParticles + at<uint8_t>(m + kRing + 3) * stride;
}

// What the burst and the firework do after their first frame: every live particle loses a thirty-second of its
// velocity (+8, +0xa, +0xc) each frame, then the simple mover carries it on.
void decayAndMove(uint32_t m)
{
    uint32_t index = at<uint8_t>(m + kRing + 2);
    if (index != at<uint8_t>(m + kRing + 3))
    {
        do
        {
            const uint32_t p = m + kParticles + index * 0x14;
            for (uint32_t offset : {0xcu, 0x8u, 0xau})
            {
                const int16_t v = at<int16_t>(p + offset);
                at<int16_t>(p + offset) = static_cast<int16_t>(v - (v >> 5));
            }
            index = (index + 1) & at<uint8_t>(m + kRing);
        } while (index != at<uint8_t>(m + kRing + 3));
    }
    VfxSimpleParticleMover(ptr32<_sVfx_PartHeader>(m + kRing), ptr32<_VFX_SIMPLEPARTICLE>(m + kParticles));
}

// Fills the burst's 63 particles (0x14 bytes each, from +0x44 up to +0x530): a random unit direction, thrown out at a
// quarter of its length and a velocity of 1/(1<<velocityShift) of it, and a random life of 31 to 62 frames. The ring
// is then full (count and tail 63).
void startBurst(uint32_t m, int velocityShift)
{
    uint32_t p = m + kParticles;
    do
    {
        at<int16_t>(p) = static_cast<int16_t>((vfx::random15() & 0x7ff) - 0x400);
        at<int16_t>(p + 2) = static_cast<int16_t>((vfx::random15() & 0x7ff) - 0x400);
        at<int16_t>(p + 4) = static_cast<int16_t>((vfx::random15() & 0x7ff) - 0x400);
        VecNormalise(ptr32<SVECTOR>(p), ptr32<SVECTOR>(p));
        const int16_t x = at<int16_t>(p), y = at<int16_t>(p + 2), z = at<int16_t>(p + 4);
        const int16_t px = static_cast<int16_t>(x >> 2), py = static_cast<int16_t>(y >> 2), pz = static_cast<int16_t>(z >> 2);
        at<int16_t>(p + 8) = static_cast<int16_t>(px >> (velocityShift - 2));
        at<int16_t>(p + 0xa) = static_cast<int16_t>(py >> (velocityShift - 2));
        at<int16_t>(p + 0xc) = static_cast<int16_t>(pz >> (velocityShift - 2));
        at<int16_t>(p) = px;
        at<int16_t>(p + 2) = py;
        at<int16_t>(p + 4) = pz;
        at<int16_t>(p + 0x10) = static_cast<int16_t>((vfx::random15() & 0x1f) + 0x1f);
        p += 0x14;
    } while (static_cast<int32_t>(p) < static_cast<int32_t>(m + 0x530));
    at<uint8_t>(m + kRing + 1) = 0x3f;
    at<uint8_t>(m + kRing + 3) = 0x3f;
}

} // namespace

// The heat ray's aerial glow: while the car's ray has life (more than 11 frames) a particle is added to the ring each
// frame at a random spot, drifting with the car's own velocity (so it trails behind) and upwards. Sorted at the car's
// position. The life stays at 10 while the car's counter at +0x198 is zero.
void VfxHandler_CarHeatRayAerial(ptr32<_sVfx_Object> mem)
{
    const uint32_t m = mem.address;
    const uint32_t car = at<uint32_t>(m + 0x344);
    if (at<uint16_t>(m + kLifeSpan) >= 0xc && at<uint8_t>(m + kRing + 1) < at<uint8_t>(m + kRing))
    {
        const uint32_t state = integerState(car);
        const auto slot = [&] { return tailParticle(m, 0x18); };
        at<int16_t>(slot()) = static_cast<int16_t>((vfx::random15() & 0x1ff) - 0x100);
        at<int16_t>(slot() + 2) = static_cast<int16_t>((vfx::random15() & 0x1ff) - 0x100);
        at<int16_t>(slot() + 4) = static_cast<int16_t>((vfx::random15() & 0x1ff) - 0x100);
        at<int16_t>(slot() + 8) = static_cast<int16_t>((vfx::random15() & 0x7f) - ((at<int16_t>(state + 0x1c) >> 4) + 0x40));
        at<int16_t>(slot() + 0xa) = static_cast<int16_t>(0xe6 - (at<int16_t>(state + 0x1e) >> 4));
        at<int16_t>(slot() + 0xc) = static_cast<int16_t>((vfx::random15() & 0x7f) - ((at<int16_t>(state + 0x20) >> 4) + 0x40));
        at<int16_t>(slot() + 0x10) = static_cast<int16_t>(vfx::random15());
        at<int16_t>(slot() + 0x12) = static_cast<int16_t>((vfx::random15() & 0x7f) - 0x3f);
        at<int16_t>(slot() + 0x14) = static_cast<int16_t>((vfx::random15() & 7) + 4);
        at<uint8_t>(slot() + 0x16) = static_cast<uint8_t>((vfx::random15() & 3) + 0xc);
        at<uint8_t>(slot() + 0x17) = 0xff;
        vfx::ringPush(m);
    }
    VfxParticleMover(ptr32<_sVfx_PartHeader>(m + kRing), ptr32<_VFX_PARTICLE>(m + kParticles));
    const sVec32 &pos = at<sVec32>(integerState(car) + 4);
    sortPosFromInts(m, pos.x >> 8, pos.y >> 8, pos.z >> 8, 0x90);
    const uint16_t life = static_cast<uint16_t>(at<uint16_t>(m + kLifeSpan) - 1);
    at<uint16_t>(m + kLifeSpan) = life;
    if (at<int16_t>(car + 0x198) == 0)
        at<uint16_t>(m + kLifeSpan) = 10;
}

// The "kaboom" burst: on its first frame (life 64) 63 particles fly out in random directions; after that they slow by a
// thirty-second a frame. Sorted at the burst's centre (+0x544, three shorts).
void VfxHandler_Kaboom(ptr32<_sVfx_Object> mem)
{
    const uint32_t m = mem.address;
    if (at<uint16_t>(m + kLifeSpan) == 0x40)
        startBurst(m, 4);
    else
        decayAndMove(m);
    sortPosFromInts(m, at<int16_t>(m + 0x544), at<int16_t>(m + 0x546), at<int16_t>(m + 0x548), 0x80);
    at<uint16_t>(m + kLifeSpan) = static_cast<uint16_t>(at<uint16_t>(m + kLifeSpan) - 1);
}

// The firework: like the kaboom, but the particles are faster (an eighth) and on the first frame it also picks a
// random colour (three bytes at +0x54c, each 0x40 to 0x7f). The sort position is the burst's centre, by a tail call to
// VfxSortPos (so the stack frame of the stale lane is VfxSortPos's alone).
void VfxHandler_Firework(ptr32<_sVfx_Object> mem)
{
    const uint32_t m = mem.address;
    if (at<uint16_t>(m + kLifeSpan) == 0x40)
    {
        startBurst(m, 3);
        at<uint8_t>(m + 0x54c) = static_cast<uint8_t>((vfx::random15() & 0x3f) + 0x40);
        at<uint8_t>(m + 0x54d) = static_cast<uint8_t>((vfx::random15() & 0x3f) + 0x40);
        at<uint8_t>(m + 0x54e) = static_cast<uint8_t>((vfx::random15() & 0x3f) + 0x40);
    }
    else
        decayAndMove(m);
    at<uint16_t>(m + kLifeSpan) = static_cast<uint16_t>(at<uint16_t>(m + kLifeSpan) - 1);
    sortPosFromInts(m, at<int16_t>(m + 0x544), at<int16_t>(m + 0x546), at<int16_t>(m + 0x548), 0x20);
}

// The invincibility halo: on its first frame (life 0xffff) it lays 32 points on an ellipse around the car's shape (from
// its bounding box: half the width plus 30, at 24 units above the lowest point), at even angle steps; every frame it
// is sorted at the car and loses a frame of life. The points are the renderer's.
void VfxHandler_Invincible(ptr32<_sVfx_Object> mem)
{
    const uint32_t m = mem.address;
    const uint32_t car = at<uint32_t>(m + 0x250);
    if (at<uint16_t>(m + kLifeSpan) == 0xffff)
    {
        const sVec32 &pos = at<sVec32>(integerState(car) + 4);
        G(svVelTemp).vx = static_cast<int16>(pos.x >> 8);
        G(svVelTemp).vy = static_cast<int16>(pos.y >> 8);
        G(svVelTemp).vz = static_cast<int16>(pos.z >> 8);
        const uint32_t box = at<uint32_t>(at<uint32_t>(car + 0x18));
        const ee_float step = at<float>(0x327850u);
        for (int32_t k = 0; k < 32; ++k)
        {
            const uint32_t entry = m + 0x50 + 0x10 * static_cast<uint32_t>(k);
            const ee_float angle = ee_itof(k) * step;
            const int32_t halfX = (at<int16_t>(box + 0x6e) - at<int16_t>(box + 0x68)) >> 1;
            const int32_t halfZ = (at<int16_t>(box + 0x72) - at<int16_t>(box + 0x6c)) >> 1;
            at<float>(entry) = ee_sinf(angle) * (ee_itof(halfX) + ee_float(30.0f));
            at<float>(entry + 4) = ee_itof(at<int16_t>(box + 0x6a)) - ee_float(24.0f);
            at<float>(entry + 8) = ee_cosf(angle) * (ee_itof(halfZ) + ee_float(30.0f));
        }
    }
    const sVec32 &pos = at<sVec32>(integerState(car) + 4);
    sortPosFromInts(m, pos.x >> 8, pos.y >> 8, pos.z >> 8, 0xd0, 0x20);
    at<uint16_t>(m + kLifeSpan) = static_cast<uint16_t>(at<uint16_t>(m + kLifeSpan) - 1);
}

// The arm cannon's smoke: while the effect has life (more than 15 frames) a puff is added at the effect's origin with
// the hook's last movement (+0x18c..0x190) plus a small random spread, so the puffs keep the arm's speed. When the hook
// (+0x14, three shorts) has moved from where it was (+0x184), that movement is refreshed.
void VfxHandler_ArmCannon(ptr32<_sVfx_Object> mem)
{
    const uint32_t m = mem.address;
    if (at<uint16_t>(m + kLifeSpan) >= 0x10 && at<uint8_t>(m + kRing + 1) < 0xf)
    {
        const uint32_t p = m + kParticles + static_cast<uint32_t>(static_cast<int8_t>(at<uint8_t>(m + kRing + 3))) * 0x14u;
        at<uint16_t>(p) = 0;
        at<uint16_t>(p + 2) = 0;
        at<uint16_t>(p + 4) = 0;
        at<uint16_t>(p + 8) = static_cast<uint16_t>((vfx::random15() & 0xf) + at<uint16_t>(m + 0x18c) - 8);
        at<uint16_t>(p + 0xa) = static_cast<uint16_t>(at<uint16_t>(m + 0x18e) + (vfx::random15() & 0xf));
        at<uint16_t>(p + 0xc) = static_cast<uint16_t>((vfx::random15() & 0xf) + at<uint16_t>(m + 0x190) - 8);
        at<uint16_t>(p + 0x10) = 0xf;
        at<uint16_t>(p + 0x12) = static_cast<uint16_t>(vfx::random15());
        vfx::ringPush(m);
    }
    VfxSimpleParticleMover(ptr32<_sVfx_PartHeader>(m + kRing), ptr32<_VFX_SIMPLEPARTICLE>(m + kParticles));
    // The hook's last position is at +0x184, shorts x, y, z; it is compared as a word (x and y) and a half (z). If
    // the hook moved, the difference (old - new) is kept as the velocity and the hook's position becomes the old one.
    const bool same = at<uint32_t>(m + 0x184) == at<uint32_t>(m + 0x14) && at<int16_t>(m + 0x188) == at<int16_t>(m + 0x18);
    if (!same)
    {
        const uint16_t ox = at<uint16_t>(m + 0x184), oy = at<uint16_t>(m + 0x186), oz = at<uint16_t>(m + 0x188);
        const uint16_t nx = at<uint16_t>(m + 0x14), ny = at<uint16_t>(m + 0x16), nz = at<uint16_t>(m + 0x18);
        at<uint16_t>(m + 0x18c) = static_cast<uint16_t>(ox - nx);
        at<uint16_t>(m + 0x18e) = static_cast<uint16_t>(oy - ny);
        at<uint16_t>(m + 0x190) = static_cast<uint16_t>(oz - nz);
        at<uint16_t>(m + 0x184) = nx;
        at<uint16_t>(m + 0x186) = ny;
        at<uint16_t>(m + 0x188) = nz;
    }
    sortPosFromInts(m, at<int16_t>(m + 0x14), at<int16_t>(m + 0x16), at<int16_t>(m + 0x18), 0x50 + 0x20);
    at<uint16_t>(m + kLifeSpan) = static_cast<uint16_t>(at<uint16_t>(m + kLifeSpan) - 1);
}

// The car's fire: while the car is invincible the life is held at 11. With more than 11 frames left, up to two
// particles are added each frame (as long as the ring has room), each starting at a random spot near the car and
// carrying the car's velocity, rising. Sorted at the car by a tail call to VfxCarSortPos.
void VfxHandler_CarFire(ptr32<_sVfx_Object> mem)
{
    const uint32_t m = mem.address;
    const ptr32<_sObject> car(at<uint32_t>(m + 0x344));
    if (at<int8_t>(car.address + 0x19c) != 0)
        at<uint16_t>(m + kLifeSpan) = 0xb;
    const uint16_t life = at<uint16_t>(m + kLifeSpan);
    if (life >= 0xc)
    {
        for (int pass = 1; pass >= 0; --pass)
        {
            if (!(at<uint8_t>(m + kRing + 1) < at<uint8_t>(m + kRing)))
                continue;
            const uint32_t velocity = VfxCarVelocityPoi(car).address;
            const auto slot = [&] { return tailParticle(m, 0x18); };
            at<int16_t>(slot()) = static_cast<int16_t>((vfx::random15() & 0x3ff) - 0x200);
            at<int16_t>(slot() + 2) = static_cast<int16_t>((vfx::random15() & 0x3ff) - 0x200);
            at<int16_t>(slot() + 4) = static_cast<int16_t>((vfx::random15() & 0x3ff) - 0x200);
            at<int16_t>(slot() + 8) = static_cast<int16_t>((vfx::random15() & 0x7f) - ((at<int16_t>(velocity) >> 4) + 0x40));
            at<int16_t>(slot() + 0xa) = static_cast<int16_t>(0xf0 - (at<int16_t>(velocity + 2) >> 4));
            at<int16_t>(slot() + 0xc) = static_cast<int16_t>((vfx::random15() & 0x7f) - ((at<int16_t>(velocity + 4) >> 4) + 0x40));
            at<int16_t>(slot() + 0x10) = static_cast<int16_t>(vfx::random15());
            at<int16_t>(slot() + 0x12) = static_cast<int16_t>((vfx::random15() & 0x7f) - 0x3f);
            at<int16_t>(slot() + 0x14) = static_cast<int16_t>((vfx::random15() & 7) + 4);
            at<uint8_t>(slot() + 0x16) = static_cast<uint8_t>((vfx::random15() & 7) + 0x1a);
            at<uint8_t>(slot() + 0x17) = 0xfe;
            vfx::ringPush(m);
        }
    }
    at<uint16_t>(m + kLifeSpan) = static_cast<uint16_t>(at<uint16_t>(m + kLifeSpan) - 1);
    VfxParticleMover(ptr32<_sVfx_PartHeader>(m + kRing), ptr32<_VFX_PARTICLE>(m + kParticles));
    vfx::sortPosFromCar(m, car, 0x20);
}


// The car fire's flames, as sprites around the car's position: each particle is a sprite whose colour goes from white
// through orange to red (the colour channels fall off at different rates as its life runs out); now and then (one in
// eight) a second, darker red sprite of its own size is drawn over it.
void VfxRenderer_CarFire(ptr32<_sVfx_Object> mem)
{
    GuestFrame frame(0x90);
    const uint32_t m = mem.address;
    const uint32_t pos = VfxCarPositionPoi(ptr32<_sObject>(at<uint32_t>(m + 0x344))).address;
    at<uint8_t>(vfx::sprite() + vfx::spr::kFlipX) = 0;
    at<uint8_t>(vfx::sprite() + vfx::spr::kCode) = 2;
    at<uint32_t>(vfx::sprite() + vfx::spr::kTexture) = vfx::vfxTexture(0);
    for (uint32_t index = at<uint8_t>(m + kRing + 2); index != at<uint8_t>(m + kRing + 3);)
    {
        const uint32_t p = m + kParticles + index * 0x18;
        const uint32_t sp = vfx::sprite();
        at<float>(sp + vfx::spr::kPos) = ee_itof(at<int16_t>(pos) + (at<int16_t>(p) >> 5));
        const int32_t fade = 0x30 - static_cast<int32_t>(at<uint16_t>(p + 0x14)) * 4;
        int32_t green = 100 - fade * 2, blue = 100 - fade * 4;
        blue = blue > -1 ? blue : 0;
        green = green > -1 ? green : 0;
        at<float>(sp + vfx::spr::kPos + 4) = ee_itof(at<int16_t>(pos + 2) + (at<int16_t>(p + 2) >> 5));
        at<float>(sp + vfx::spr::kPos + 8) = ee_itof(at<int16_t>(pos + 4) + (at<int16_t>(p + 4) >> 5));
        at<uint16_t>(sp + vfx::spr::kAngle) = at<uint16_t>(p + 0x10);
        at<uint32_t>(sp + vfx::spr::kRgb) = static_cast<uint32_t>((100 - fade) + (green << 8) + (blue << 16));
        at<float>(sp + vfx::spr::kScale) = ee_itof(at<uint8_t>(p + 0x16) + 0xc);
        VfxPS2SpriteRotated3D(ee_float(-70.0f));
        if ((vfx::random15() & 7) == 0)
        {
            const uint32_t s2 = vfx::sprite();
            at<uint32_t>(s2 + vfx::spr::kTexture) = vfx::vfxTexture(1);
            at<uint32_t>(s2 + vfx::spr::kRgb) = 0x400000;
            at<float>(s2 + vfx::spr::kScale) = ee_itof(at<uint8_t>(p + 0x16));
            VfxPS2SpriteRotated3D(ee_float(-70.0f));
            at<uint32_t>(vfx::sprite() + vfx::spr::kTexture) = vfx::vfxTexture(0);
        }
        index = (index + 1) & at<uint8_t>(m + kRing);
    }
}

// The arm cannon's puffs as sprites at the cannon's hook position (+0x14), each fading and shrinking as its life
// runs down, then the cannon-ball shape itself at the hook with an identity rotation.
void VfxRenderer_ArmCannon(ptr32<_sVfx_Object> mem)
{
    GuestFrame frame(0xc0);
    const uint32_t m = mem.address;
    at<uint8_t>(vfx::sprite() + vfx::spr::kCode) = 4;
    at<uint32_t>(vfx::sprite() + vfx::spr::kTexture) = vfx::vfxTexture(5);
    for (int32_t index = vfx::ringHead(m); index != at<uint8_t>(m + kRing + 3); index = vfx::ringNext(m, index))
    {
        const uint32_t p = m + kParticles + static_cast<uint32_t>(index) * 0x14u;
        const uint32_t sp = vfx::sprite();
        const int32_t life = static_cast<int32_t>(at<uint16_t>(p + 0x10)) * 2 + 0x10;
        at<float>(sp + vfx::spr::kPos) = ee_itof(at<int16_t>(m + 0x14) + (at<int16_t>(p) >> 1));
        at<float>(sp + vfx::spr::kPos + 4) = ee_itof(at<int16_t>(m + 0x16) + (at<int16_t>(p + 2) >> 1));
        at<float>(sp + vfx::spr::kPos + 8) = ee_itof(at<int16_t>(m + 0x18) + (at<int16_t>(p + 4) >> 1));
        at<uint8_t>(sp + vfx::spr::kFlipX) = 0;
        at<uint16_t>(sp + vfx::spr::kAngle) = at<uint16_t>(p + 0x12);
        at<uint32_t>(sp + vfx::spr::kRgb) = static_cast<uint32_t>(life * 0x10101);
        at<float>(sp + vfx::spr::kScale) = ee_itof(0x82 - life * 2);
        VfxPS2SpriteRotated3D(ee_float(0.0f));
    }
    const ptr32<float> mat = frame.local<float>(0);
    guest_call(0x1c7ac8u, mat); // sceVu0UnitMatrix
    const uint32_t shape = at<uint32_t>(G(visFxVarG).pVfxRes.address + 0x84);
    const float *rows = &*mat;
    at<float>(shape + 0x8) = rows[0];
    at<float>(shape + 0xc) = rows[1];
    at<float>(shape + 0x10) = rows[2];
    at<float>(shape + 0x14) = rows[4];
    at<float>(shape + 0x18) = rows[5];
    at<float>(shape + 0x1c) = rows[6];
    at<float>(shape + 0x20) = rows[8];
    at<float>(shape + 0x24) = rows[9];
    at<float>(shape + 0x28) = rows[10];
    at<float>(shape + 0x2c) = ee_itof(at<int16_t>(m + 0x14));
    at<float>(shape + 0x30) = ee_itof(at<int16_t>(m + 0x16));
    at<float>(shape + 0x34) = ee_itof(at<int16_t>(m + 0x18));
    const float version = at<float>(shape);
    if (version == 2.0f)
        guest_call(0x1a5230u, ptr32<uint8>(shape), ptr32<uint8>(0x1f40b8u), 0); // Rdr_VUShape(shape, &rdrCameraG, 0)
    else if (version == 3.0f)
        guest_call(0x1a55b8u, ptr32<uint8>(shape), ptr32<uint8>(0x1f40b8u), 0); // TRdr_VUShape
}

// The barge's oil slick: drawn only when it is within 7,500 units of the camera on each axis. Four layers at its
// position: a dark disc, then three coloured ones spinning at different rates (the angle at +0x44, the size at +0x46).
// The last sprite is a tail call to VfxPS2SpriteRotated3D (the stack frame is already gone).
void VfxRenderer_BargeOil(ptr32<_sVfx_Object> mem)
{
    const uint32_t m = mem.address;
    const int32_t cam[3] = {G(visFxVarG).vCamPos.x >> 8, G(visFxVarG).vCamPos.y >> 8, G(visFxVarG).vCamPos.z >> 8};
    for (uint32_t axis = 0; axis < 3; ++axis)
    {
        const int32_t hook = at<int16_t>(m + 0x14 + 2 * axis);
        int32_t distance = hook - cam[axis];
        if (distance < 1)
            distance = cam[axis] - hook;
        if (!(distance < 0x1d4c))
            return;
    }
    {
        GuestFrame frame(0x50);
        const uint32_t sp = vfx::sprite();
        at<uint8_t>(sp + vfx::spr::kFlipX) = 0;
        at<uint8_t>(sp + vfx::spr::kCode) = 2;
        at<float>(sp + vfx::spr::kPos) = ee_itof(at<int16_t>(m + 0x14));
        at<float>(sp + vfx::spr::kPos + 4) = ee_itof(at<int16_t>(m + 0x16));
        at<uint32_t>(sp + vfx::spr::kRgb) = 0x6e6e6e;
        at<float>(sp + vfx::spr::kPos + 8) = ee_itof(at<int16_t>(m + 0x18));
        at<uint8_t>(sp + vfx::spr::kCode) = 4;
        at<uint16_t>(sp + vfx::spr::kAngle) = 0xfe00;
        at<float>(sp + vfx::spr::kScale) = ee_itof(at<int16_t>(m + 0x46));
        at<uint32_t>(vfx::sprite() + vfx::spr::kTexture) = vfx::vfxTexture(6);
        VfxPS2SpriteRotated3D(ee_float(-30.0f));
        const uint32_t s2 = vfx::sprite();
        at<uint32_t>(s2 + vfx::spr::kTexture) = vfx::vfxTexture(7);
        at<uint16_t>(s2 + vfx::spr::kAngle) = at<uint16_t>(m + 0x44);
        at<uint8_t>(s2 + vfx::spr::kCode) = 2;
        at<uint32_t>(s2 + vfx::spr::kRgb) = 0x46;
        at<float>(s2 + vfx::spr::kScale) = ee_itof(at<int16_t>(m + 0x46) + 5);
        VfxPS2SpriteRotated3D(ee_float(-30.0f));
        const uint32_t s3 = vfx::sprite();
        at<uint32_t>(s3 + vfx::spr::kRgb) = 0x4600;
        at<uint16_t>(s3 + vfx::spr::kAngle) = static_cast<uint16_t>(at<int16_t>(m + 0x44) >> 1);
        VfxPS2SpriteRotated3D(ee_float(-30.0f));
    }
    const uint32_t s4 = vfx::sprite();
    at<uint32_t>(s4 + vfx::spr::kRgb) = 0x460000;
    at<uint16_t>(s4 + vfx::spr::kAngle) = static_cast<uint16_t>(-static_cast<int32_t>(at<uint16_t>(m + 0x44)));
    VfxPS2SpriteRotated3D(ee_float(-30.0f));
}

// The explosion's burst as sprites: each particle is a 32-unit sprite at the burst's centre (+0x544) plus its offset,
// fading from white through yellow to red as its life runs out, at a random spin; then one large flash sprite that
// shrinks and dims over the first frames (a tail call to VfxPS2SpriteRotated3D).
void VfxRenderer_Kaboom(ptr32<_sVfx_Object> mem)
{
    const uint32_t m = mem.address;
    {
        GuestFrame frame(0x80);
        at<uint8_t>(vfx::sprite() + vfx::spr::kFlipX) = 0;
        at<uint32_t>(vfx::sprite() + vfx::spr::kTexture) = vfx::vfxTexture(1);
        at<uint8_t>(vfx::sprite() + vfx::spr::kCode) = 2;
        for (int32_t index = vfx::ringHead(m); index != at<uint8_t>(m + kRing + 3); index = vfx::ringNext(m, index))
        {
            const uint32_t p = m + kParticles + static_cast<uint32_t>(index) * 0x14u;
            const uint32_t sp = vfx::sprite();
            int32_t red = 0x78 - static_cast<int32_t>(at<uint16_t>(p + 0x10)) * 4;
            red = std::max(red, 0);
            at<float>(sp + vfx::spr::kPos) = ee_itof(at<int16_t>(m + 0x544) + (at<int16_t>(p) >> 5));
            int32_t green = 0x78 - red * 2, blue = 0x78 - red * 4;
            blue = blue > -1 ? blue : 0;
            green = green > -1 ? green : 0;
            at<float>(sp + vfx::spr::kPos + 4) = ee_itof(at<int16_t>(m + 0x546) + (at<int16_t>(p + 2) >> 5));
            at<float>(sp + vfx::spr::kScale) = 32.0f;
            at<uint32_t>(sp + vfx::spr::kRgb) = static_cast<uint32_t>((0x78 - red) + (green << 8) + (blue << 16));
            at<float>(sp + vfx::spr::kPos + 8) = ee_itof(at<int16_t>(m + 0x548) + (at<int16_t>(p + 4) >> 5));
            at<uint16_t>(vfx::sprite() + vfx::spr::kAngle) = static_cast<uint16_t>(vfx::random15());
            VfxPS2SpriteRotated3D(ee_float(-120.0f));
        }
    }
    const uint32_t sp = vfx::sprite();
    at<uint16_t>(sp + vfx::spr::kAngle) = 0;
    at<uint32_t>(sp + vfx::spr::kTexture) = vfx::vfxTexture(0xf);
    const int32_t flash = std::max((static_cast<int32_t>(at<uint16_t>(m + kLifeSpan)) - 0x21) * 2, 0);
    at<float>(sp + vfx::spr::kPos) = ee_itof(at<int16_t>(m + 0x544));
    at<float>(sp + vfx::spr::kPos + 4) = ee_itof(at<int16_t>(m + 0x546));
    at<uint32_t>(sp + vfx::spr::kRgb) = static_cast<uint32_t>(flash) * 0x10104u;
    at<float>(sp + vfx::spr::kScale) = ee_itof(flash * -4 + 0x136);
    at<float>(sp + vfx::spr::kPos + 8) = ee_itof(at<int16_t>(m + 0x548));
    VfxPS2SpriteRotated3D(ee_float(-120.0f));
}


// The dust a wheel kicks up, as sprites around the wheel's position: each puff is tinted with the dust colour of the
// material under the wheel (three bytes of the material's iDustRGB), darker as the puff's life runs out.
void VfxRenderer_WheelDust(ptr32<_sVfx_Object> mem)
{
    GuestFrame frame(0x90);
    const uint32_t m = mem.address;
    const uint64_t wheel = VfxWheelPositionVal(ptr32<_sObject>(at<uint32_t>(m + 0x1c4)), at<uint16_t>(m + 0x1c8));
    at<uint8_t>(vfx::sprite() + vfx::spr::kFlipX) = 0;
    at<uint32_t>(vfx::sprite() + vfx::spr::kTexture) = vfx::vfxTexture(0xd);
    at<uint8_t>(vfx::sprite() + vfx::spr::kCode) = 2;
    const int16_t wx = static_cast<int16_t>(wheel), wy = static_cast<int16_t>(wheel >> 16), wz = static_cast<int16_t>(wheel >> 32);
    for (uint32_t index = at<uint8_t>(m + kRing + 2); index != at<uint8_t>(m + kRing + 3);)
    {
        const uint32_t p = m + kParticles + index * 0x18;
        const uint32_t sp = vfx::sprite();
        const uint32_t dust = 0x1edce8u + static_cast<uint32_t>(at<uint16_t>(m + 0x1ca)) * 0x18 + 0xc; // mtlG.mtl[material].iDustRGB
        at<float>(sp + vfx::spr::kPos) = ee_itof(wx + (at<int16_t>(p) >> 4));
        at<float>(sp + vfx::spr::kPos + 4) = ee_itof(wy + (at<int16_t>(p + 2) >> 4));
        at<float>(sp + vfx::spr::kPos + 8) = ee_itof(wz + (at<int16_t>(p + 4) >> 4));
        const int32_t fade = 0x3c - static_cast<int32_t>(at<uint16_t>(p + 0x14)) * 4;
        const auto channel = [&](uint32_t offset) { return std::min(std::max(static_cast<int32_t>(at<uint8_t>(dust + offset)) - fade, 0), 0x10); };
        const int32_t green = channel(1), blue = channel(2), red = channel(0);
        at<uint16_t>(sp + vfx::spr::kAngle) = at<uint16_t>(p + 0x10);
        at<float>(sp + vfx::spr::kScale) = ee_itof(at<uint8_t>(p + 0x16));
        at<uint32_t>(sp + vfx::spr::kRgb) = static_cast<uint32_t>(red + green * 0x100 + blue * 0x10000);
        VfxPS2SpriteRotated3D(ee_float(0.0f));
        index = (index + 1) & at<uint8_t>(m + kRing);
    }
}

// The heat ray's damage sparks around the car it burns (the car at +0x344), drawn when the car is near enough to be
// seen: each particle is a sprite fading from white to orange-red as its life runs out, and while its extra count
// (+0x16) lasts a small blue-grey glow sprite is added on top.
void VfxRenderer_HeatRayDamage(ptr32<_sVfx_Object> mem)
{
    GuestFrame frame(0x90);
    const uint32_t m = mem.address;
    const uint32_t car = at<uint32_t>(m + 0x344);
    if (!(VfxHasCarBeenRendered(ptr32<_sObject>(car)) < 0x157c))
        return;
    const sVec32 &pos = at<sVec32>(integerState(car) + 4);
    G(svVelTemp).vz = static_cast<int16>(static_cast<uint32_t>(pos.z) >> 8);
    G(svVelTemp).vx = static_cast<int16>(static_cast<uint32_t>(pos.x) >> 8);
    G(svVelTemp).vy = static_cast<int16>(static_cast<uint32_t>(pos.y) >> 8);
    at<uint8_t>(vfx::sprite() + vfx::spr::kFlipX) = 0;
    at<uint8_t>(vfx::sprite() + vfx::spr::kCode) = 2;
    at<uint32_t>(vfx::sprite() + vfx::spr::kTexture) = vfx::vfxTexture(0);
    for (uint32_t index = at<uint8_t>(m + kRing + 2); index != at<uint8_t>(m + kRing + 3);)
    {
        const uint32_t p = m + kParticles + index * 0x18;
        const uint32_t sp = vfx::sprite();
        const int32_t fade = 0x3c - static_cast<int32_t>(at<uint16_t>(p + 0x14)) * 4;
        at<float>(sp + vfx::spr::kPos) = ee_itof(G(svVelTemp).vx + (at<int16_t>(p) >> 5));
        int32_t green = 0x87 - fade * 2, blue = 0x87 - fade * 4;
        blue = blue > -1 ? blue : 0;
        green = green > -1 ? green : 0;
        at<float>(sp + vfx::spr::kPos + 4) = ee_itof(G(svVelTemp).vy + (at<int16_t>(p + 2) >> 5));
        at<float>(sp + vfx::spr::kScale) = ee_itof(((0x42 - fade) >> 1) + 0xc);
        at<uint16_t>(sp + vfx::spr::kAngle) = at<uint16_t>(p + 0x10);
        at<uint32_t>(sp + vfx::spr::kRgb) = static_cast<uint32_t>((0x87 - fade) + (green << 8) + (blue << 16));
        at<float>(sp + vfx::spr::kPos + 8) = ee_itof(G(svVelTemp).vz + (at<int16_t>(p + 4) >> 5));
        VfxPS2SpriteRotated3D(ee_float(0.0f));
        if (at<uint8_t>(p + 0x16) != 0)
        {
            at<uint8_t>(p + 0x16) = static_cast<uint8_t>(at<uint8_t>(p + 0x16) - 1);
            at<uint32_t>(sp + vfx::spr::kRgb) = 0x204060;
            at<uint32_t>(sp + vfx::spr::kTexture) = vfx::vfxTexture(1);
            at<float>(sp + vfx::spr::kScale) = 25.0f;
            VfxPS2SpriteRotated3D(ee_float(0.0f));
            at<uint32_t>(vfx::sprite() + vfx::spr::kTexture) = vfx::vfxTexture(0);
        }
        index = (index + 1) & at<uint8_t>(m + kRing);
    }
}

// The firework's burst as sprites: each particle is a 48-unit sprite tinted with the firework's own colour (+0x54c, three
// bytes), fading as its life runs out, at a random spin; then a big white flash that shrinks over the first frames (a
// tail call to VfxPS2SpriteRotated3D).
void VfxRenderer_Firework(ptr32<_sVfx_Object> mem)
{
    const uint32_t m = mem.address;
    {
        GuestFrame frame(0x70);
        at<uint8_t>(vfx::sprite() + vfx::spr::kFlipX) = 0;
        at<uint32_t>(vfx::sprite() + vfx::spr::kTexture) = vfx::vfxTexture(1);
        at<uint8_t>(vfx::sprite() + vfx::spr::kCode) = 2;
        for (int32_t index = vfx::ringHead(m); index != at<uint8_t>(m + kRing + 3); index = vfx::ringNext(m, index))
        {
            const uint32_t p = m + kParticles + static_cast<uint32_t>(index) * 0x14u;
            const uint32_t sp = vfx::sprite();
            at<float>(sp + vfx::spr::kPos) = ee_itof(at<int16_t>(m + 0x544) + (at<int16_t>(p) >> 5));
            int32_t fade = 0x80 - static_cast<int32_t>(at<uint16_t>(p + 0x10)) * 4;
            fade = std::max(fade, 0);
            at<float>(sp + vfx::spr::kPos + 4) = ee_itof(at<int16_t>(m + 0x546) + (at<int16_t>(p + 2) >> 5));
            at<float>(sp + vfx::spr::kPos + 8) = ee_itof(at<int16_t>(m + 0x548) + (at<int16_t>(p + 4) >> 5));
            const int32_t red = std::max(static_cast<int32_t>(at<uint8_t>(m + 0x54c)) - fade, 0);
            const int32_t green = std::max(static_cast<int32_t>(at<uint8_t>(m + 0x54d)) - fade * 2, 0);
            const int32_t blue = std::max(static_cast<int32_t>(at<uint8_t>(m + 0x54e)) - fade * 4, 0);
            at<float>(sp + vfx::spr::kScale) = 48.0f;
            at<uint32_t>(sp + vfx::spr::kRgb) = static_cast<uint32_t>(red + green * 0x100 + blue * 0x10000);
            at<uint16_t>(vfx::sprite() + vfx::spr::kAngle) = static_cast<uint16_t>(vfx::random15());
            VfxPS2SpriteRotated3D(ee_float(0.0f));
        }
    }
    const uint32_t sp = vfx::sprite();
    at<uint32_t>(sp + vfx::spr::kTexture) = vfx::vfxTexture(0xf);
    at<uint16_t>(sp + vfx::spr::kAngle) = 0;
    const int32_t flash = std::max((static_cast<int32_t>(at<uint16_t>(m + kLifeSpan)) - 0x20) * 2, 0);
    at<float>(sp + vfx::spr::kPos) = ee_itof(at<int16_t>(m + 0x544));
    at<float>(sp + vfx::spr::kPos + 4) = ee_itof(at<int16_t>(m + 0x546));
    at<uint32_t>(sp + vfx::spr::kRgb) = static_cast<uint32_t>(flash) * 0x10101u;
    at<float>(sp + vfx::spr::kScale) = ee_itof(flash * -8 + 600);
    at<float>(sp + vfx::spr::kPos + 8) = ee_itof(at<int16_t>(m + 0x548));
    VfxPS2SpriteRotated3D(ee_float(0.0f));
}

// The car rocket (a rocket carried by the car, at +0x350): the smoke puffs trailing behind it as sprites (each with its
// own position offset, a grey set by its own intensity), then the rocket's shape at the car's aerial position,
// rotated like the car and scaled up threefold. The shape is drawn through TRdr_VUShape after resetting the render
// state, which is why the stack holds a matrix and a state block.
void VfxRenderer_CarRocket(ptr32<_sVfx_Object> mem)
{
    GuestFrame frame(0x180);
    const uint32_t m = mem.address;
    const ptr32<_sObject> car(at<uint32_t>(m + 0x350));
    const uint64_t aerial = VfxCarAerialVal(car, 5);
    const ptr32<ee_float> carMatrix = VfxPSXToVUMatrix(ptr32<MATRIX>(integerState(car.address) + 0x28));
    if (!(VfxHasCarBeenRendered(car) < 0x157c))
        return;
    const uint32_t pos = frame.local<float>(0x40).address;
    at<float>(pos) = ee_itof(static_cast<int16_t>(aerial));
    at<float>(pos + 4) = ee_itof(static_cast<int16_t>(aerial >> 16));
    at<float>(pos + 8) = ee_itof(static_cast<int16_t>(aerial >> 32));
    const uint32_t sp = vfx::sprite();
    at<uint8_t>(sp + vfx::spr::kFlipX) = 0;
    at<uint8_t>(sp + vfx::spr::kCode) = 2;
    at<uint32_t>(sp + vfx::spr::kTexture) = vfx::vfxTexture(0x1c);
    at<uint16_t>(vfx::sprite() + vfx::spr::kAngle) = static_cast<uint16_t>(vfx::random15());
    at<float>(vfx::sprite() + vfx::spr::kScale) = 6.0f;
    for (uint32_t index = at<uint8_t>(m + kRing + 2); index != at<uint8_t>(m + kRing + 3);)
    {
        const uint32_t p = m + 0x50 + index * 0x30;
        const uint32_t s = vfx::sprite();
        at<uint32_t>(s + vfx::spr::kRgb) = at<uint32_t>(p + 0x24) * 0x810;
        sceVu0CopyVector(ptr32<ee_float>(s + vfx::spr::kPos), ptr32<ee_float>(pos));
        vfx::vu0Add(s + vfx::spr::kPos, s + vfx::spr::kPos, p);
        VfxPS2SpriteRotated3D(ee_float(-10.0f));
        index = (index + 1) & at<uint8_t>(m + kRing);
    }
    const uint32_t s = vfx::sprite();
    sceVu0CopyVector(ptr32<ee_float>(s + vfx::spr::kPos), ptr32<ee_float>(pos));
    const ptr32<float> matrix = frame.local<float>(0x50);
    guest_call(0x1c7a60u, matrix, carMatrix); // sceVu0CopyMatrix
    const ptr32<uint8> block = frame.local<uint8>(0xa0);
    guest_call(0x1a1550u, block); // RdrState_SetStateBlockToDefaults
    guest_call(0x1a1058u, block); // RdrState_ForceApplyStateBlock
    const uint32_t shape = at<uint32_t>(G(visFxVarG).pVfxRes.address + 0x88);
    const uint32_t rows[3] = {0x50, 0x60, 0x70};
    for (uint32_t row = 0; row < 3; ++row)
        for (uint32_t col = 0; col < 3; ++col)
            at<float>(shape + 8 + row * 12 + col * 4) = (ee_float(at<float>(frame.local<float>(rows[row] + col * 4).address)) * ee_float(3.0f));
    at<float>(shape + 0x2c) = at<float>(s + vfx::spr::kPos);
    at<float>(shape + 0x30) = at<float>(s + vfx::spr::kPos + 4);
    at<float>(shape + 0x34) = at<float>(s + vfx::spr::kPos + 8);
    guest_call(0x1a55b8u, ptr32<uint8>(shape), ptr32<uint8>(0x1f40b8u), 0); // TRdr_VUShape(shape, &rdrCameraG, 0)
}

// ---------------------------------------------------------------------------------------------------------------------
// The fifth batch: the bomb, the dust and speed-up puffs that follow the wheels, the heat ray's damage, the oil dump, the
// car respawn and change-over effects, Sarge's lights, the skid marks and the quake.

namespace {

// svVelTemp (a shared short vector): the car's position in whole units, which the renderers keep their particles
// relative to. The original loads the car's integer state from the physics record each time.
void followCar(uint32_t car)
{
    const sVec32 &pos = at<sVec32>(integerState(car) + 4);
    G(svVelTemp).vz = static_cast<int16>(static_cast<uint32_t>(pos.z) >> 8);
    G(svVelTemp).vx = static_cast<int16>(static_cast<uint32_t>(pos.x) >> 8);
    G(svVelTemp).vy = static_cast<int16>(static_cast<uint32_t>(pos.y) >> 8);
}

// What the renderers ask of a car (the inlined VfxHasCarBeenRendered): how long since it was drawn.
bool carNear(uint32_t car, uint32_t limit) { return VfxHasCarBeenRendered(ptr32<_sObject>(car)) < limit; }

// A new particle of a car effect that follows the wheels: 0x18 bytes at the ring's tail, starting at the wheel's offset
// from the car (scaled up by 1 << offsetShift), thrown out against the car's own speed. Its first eight bytes are the
// original's eight-byte copy of the local offset vector, whose fourth short (at +6) was never written: it is a word of
// the stack (frame `frameSize`, the local at sp+0).
struct WheelPuff
{
    uint32_t offsetShift;
    uint32_t spreadX, spreadZ;   // the masks of the random sideways speeds
    int32_t baseX, baseZ;        // and the constants they are biased by (before the car's own speed)
    uint32_t velShift;
    bool risesFromCar;           // the up speed is the car's, negated (else a constant less the car's)
    int32_t upConstant;
    uint32_t lifeBase;
    uint8_t size;
};

void spawnWheelPuff(uint32_t m, uint32_t car, uint32_t wheel, const WheelPuff &puff, uint32_t frameSize)
{
    const ptr32<SVECTOR> pos = VfxWheelPositionPoi(ptr32<_sObject>(car), static_cast<int32_t>(wheel));
    const SVECTOR &vel = G(svVelTemp);
    const uint32_t offsetX = (static_cast<uint32_t>(static_cast<uint16_t>(pos->vx)) - static_cast<uint16_t>(vel.vx)) << puff.offsetShift;
    const uint32_t offsetY = (static_cast<uint32_t>(static_cast<uint16_t>(pos->vy)) - static_cast<uint16_t>(vel.vy)) << puff.offsetShift;
    const uint32_t offsetZ = (static_cast<uint32_t>(static_cast<uint16_t>(pos->vz)) - static_cast<uint16_t>(vel.vz)) << puff.offsetShift;
    const uint16_t pad = stale_stack<uint16_t>(frameSize, 6);
    if (!(at<uint8_t>(m + kRing + 1) < at<uint8_t>(m + kRing)))
        return;
    const uint32_t p = m + kParticles + at<uint8_t>(m + kRing + 3) * 0x18u;
    const uint32_t state = integerState(car);
    const sVec16 &carVel = at<sVec16>(state + 0x1c);
    at<uint16_t>(p) = static_cast<uint16_t>(offsetX);
    at<uint16_t>(p + 2) = static_cast<uint16_t>(offsetY);
    at<uint16_t>(p + 4) = static_cast<uint16_t>(offsetZ);
    at<uint16_t>(p + 6) = pad;
    at<uint16_t>(p + 8) = static_cast<uint16_t>(((vfx::random15()) & puff.spreadX) - ((carVel.x >> puff.velShift) + puff.baseX));
    at<uint16_t>(p + 0xa) = static_cast<uint16_t>(puff.risesFromCar ? -(carVel.y >> puff.velShift) : puff.upConstant - (carVel.y >> puff.velShift));
    at<uint16_t>(p + 0xc) = static_cast<uint16_t>(((vfx::random15()) & puff.spreadZ) - ((carVel.z >> puff.velShift) + puff.baseZ));
    at<uint16_t>(p + 0x10) = static_cast<uint16_t>(vfx::random15());
    at<uint16_t>(p + 0x12) = static_cast<uint16_t>((vfx::random15() & 0x7f) - 0x3f);
    at<uint16_t>(p + 0x14) = static_cast<uint16_t>((vfx::random15() & 7) + puff.lifeBase);
    at<uint8_t>(p + 0x16) = puff.size;
    at<uint8_t>(p + 0x17) = 0;
    vfx::ringPush(m);
}

// The two skid particle trails of a car (the original's pVfx_SkidLeft/Right, a byte flag at +0x18b in each) have a
// "keep drawing" bit the speed-ups and the heat ray set and clear. Nothing is done when there is no left one.
void setSkidFlag(uint32_t car, uint8_t bits, bool set)
{
    const uint32_t left = at<uint32_t>(car + 0x178);
    if (left == 0)
        return;
    const uint32_t right = at<uint32_t>(car + 0x17c);
    for (uint32_t trail : {left, right})
    {
        uint8_t &flags = at<uint8_t>(trail + 0x18b);
        flags = set ? static_cast<uint8_t>(flags | bits) : static_cast<uint8_t>(flags & ~bits);
    }
}

// The speed-up and the boost speed-up (the same effect, the boost's puffs live a little longer): while the effect has 12
// or more frames to live each wheel throws a puff and the car's skid trails are told to draw; below that the trails are
// let go when it ends. Only the plain one then holds its last frame at 1.
void speedUpHandler(uint32_t m, uint32_t lifeBase, bool holdLast)
{
    const uint32_t car = at<uint32_t>(m + 0x644);
    at<uint16_t>(m + kLifeSpan) = static_cast<uint16_t>(at<uint16_t>(m + kLifeSpan) - 1);
    VfxParticleMover(ptr32<_sVfx_PartHeader>(m + kRing), ptr32<_VFX_PARTICLE>(m + kParticles));
    followCar(car);
    const uint16_t life = at<uint16_t>(m + kLifeSpan);
    if (life < 0xc)
    {
        if (life == 0)
        {
            setSkidFlag(car, 2, false);
            if (holdLast)
                at<uint16_t>(m + kLifeSpan) = 1;
        }
    }
    else
    {
        setSkidFlag(car, 2, true);
        static const WheelPuff base{3, 0x3f, 0x3f, 0x20, 0x20, 6, true, 0, 8, 1};
        WheelPuff puff = base;
        puff.lifeBase = lifeBase;
        for (uint32_t wheel = 0; wheel < 4; ++wheel)
            spawnWheelPuff(m, car, wheel, puff, 0xc0);
    }
    sortPosFromCar(m, ptr32<_sObject>(car), 0xc0, 0x10);
}

} // namespace

// The bomb: on its first frame (life 0xffff) it scatters 31 shards: each a random unit direction at an eighth of its length
// (the bomb's own at +0x44, the others in pairs of 0x14-byte particles from +0x58), and a random angle, with the
// ring marked full. Afterwards a flash counter (+0x2d0) counts down by 0x10 while its size (+0x2c6) shrinks by 0x2d,
// never below 0x78.
void VfxHandler_Bomb(ptr32<_sVfx_Object> mem)
{
    const uint32_t m = mem.address;
    const auto direction = [](uint32_t v)
    {
        at<uint16_t>(v) = static_cast<uint16_t>((vfx::random15() & 0xfff) - 0x800);
        at<uint16_t>(v + 2) = static_cast<uint16_t>((vfx::random15() & 0xfff) - 0x800);
        at<uint16_t>(v + 4) = static_cast<uint16_t>((vfx::random15() & 0xfff) - 0x800);
        VecNormalise(ptr32<SVECTOR>(v), ptr32<SVECTOR>(v));
        for (uint32_t axis = 0; axis < 6; axis += 2)
            at<int16_t>(v + axis) = static_cast<int16_t>(at<int16_t>(v + axis) >> 3);
    };
    if (at<uint16_t>(m + kLifeSpan) == 0xffff)
    {
        direction(m + kParticles);
        for (uint32_t pair = m + 0x58; pair < m + 0x2b0; pair += 0x28)
        {
            direction(pair);
            direction(pair + 0x14);
        }
        at<uint8_t>(m + kRing + 1) = 0x1f;
        at<uint8_t>(m + kRing + 3) = 0x1f;
        at<uint16_t>(m + 0x2c6) = 0x78;
        at<uint16_t>(m + 0x2c4) = static_cast<uint16_t>(vfx::random15() & 0xfff);
        std::memcpy(&at<uint8_t>(m + 0x2c8), &at<uint8_t>(m + 0x14), 8);
        at<int32_t>(m + 0x2d0) = 0xff;
    }
    else
    {
        const int32_t flash = at<int32_t>(m + 0x2d0);
        if (flash == 0xff)
            at<uint16_t>(m + 0x2c6) = 900;
        const uint16_t size = static_cast<uint16_t>(at<uint16_t>(m + 0x2c6) - 0x2d);
        at<int32_t>(m + 0x2d0) = flash - 0x10;
        at<uint16_t>(m + 0x2c6) = size;
        if (flash - 0x10 < 1)
            at<int32_t>(m + 0x2d0) = 0;
        if (static_cast<int16_t>(at<uint16_t>(m + 0x2c6)) < 0x78)
            at<uint16_t>(m + 0x2c6) = 0x78;
    }
    vfx::sortPosFromInts(m, at<int16_t>(m + 0x14), at<int16_t>(m + 0x16), at<int16_t>(m + 0x18), 0xa0, 0);
    at<uint16_t>(m + kLifeSpan) = static_cast<uint16_t>(at<uint16_t>(m + kLifeSpan) - 1);
}

// The dust a wheel kicks up (the car at +0x1c4, the wheel at +0x1c8): while the wheel is touching the ground (a car's
// wheel flag, or a boat's sphere being in water), and the effect has 16 frames to live, a puff starts at the wheel,
// thrown against the car's velocity.
void VfxHandler_WheelDust(ptr32<_sVfx_Object> mem)
{
    const uint32_t m = mem.address;
    const uint32_t car = at<uint32_t>(m + 0x1c4);
    const uint32_t physics = at<uint32_t>(car + 0x20);
    const uint32_t wheel = at<uint16_t>(m + 0x1c8);
    bool touching;
    if (at<uint32_t>(at<uint32_t>(car + 0xc) + 4) < 0x14)
        touching = (at<uint16_t>(physics + 0xe8 + wheel * 0x48) & 1) != 0;
    else
        touching = ee_float(0.0f) < ee_float(at<float>(at<uint32_t>(physics + 0x1e0) + wheel * 0x190 + 0x78));
    const uint32_t state = at<uint32_t>(physics + 0x1e8);
    if (touching)
    {
        // The car's velocity (four shorts at +0x1c of its integer state) is what the puff is thrown against.
        const sVec16 carVel = at<sVec16>(state + 0x1c);
        if (!(at<uint16_t>(m + kLifeSpan) < 0x10) && at<uint8_t>(m + kRing + 1) < at<uint8_t>(m + kRing))
        {
            const uint32_t p = m + kParticles + at<uint8_t>(m + kRing + 3) * 0x18u;
            at<uint16_t>(p) = 0;
            at<uint16_t>(p + 2) = 0;
            at<uint16_t>(p + 4) = 0;
            at<uint16_t>(p + 8) = static_cast<uint16_t>((vfx::random15() & 0x7f) - ((carVel.x >> 5) + 0x40));
            at<uint16_t>(p + 0xa) = static_cast<uint16_t>((vfx::random15() & 0x3f) - ((carVel.y >> 5) + 0x14));
            at<uint16_t>(p + 0xc) = static_cast<uint16_t>((vfx::random15() & 0x7f) - ((carVel.z >> 5) + 0x3f));
            at<uint16_t>(p + 0x10) = static_cast<uint16_t>(vfx::random15());
            at<uint16_t>(p + 0x12) = static_cast<uint16_t>((vfx::random15() & 0x1ff) - 0x100);
            at<uint16_t>(p + 0x14) = static_cast<uint16_t>((vfx::random15() & 3) + 0xc);
            at<uint8_t>(p + 0x16) = static_cast<uint8_t>((vfx::random15() & 7) + 1);
            at<uint8_t>(p + 0x17) = 2;
            vfx::ringPush(m);
        }
    }
    vfx::sortPosFromInts(m, at<int32_t>(state + 4) >> 8, at<int32_t>(state + 8) >> 8, at<int32_t>(state + 0xc) >> 8, 0x90, 0x20);
    VfxParticleMover(ptr32<_sVfx_PartHeader>(m + kRing), ptr32<_VFX_PARTICLE>(m + kParticles));
    at<uint16_t>(m + kLifeSpan) = static_cast<uint16_t>(at<uint16_t>(m + kLifeSpan) - 1);
}

void VfxHandler_SpeedUp(ptr32<_sVfx_Object> mem) { speedUpHandler(mem.address, 8, true); }
void VfxHandler_BoostSpeedUp(ptr32<_sVfx_Object> mem) { speedUpHandler(mem.address, 10, false); }

// The heat ray's damage on a car (+0x344): the movers run, and while the effect has 12 or more frames to live each
// wheel throws a puff up and out (the car's skid trails get their bit 0); when it ends (or the car turns invincible) the
// car's stored angle and weapon effect are cleared, through the weapon effect remover.
void VfxHandler_HeatRayDamage(ptr32<_sVfx_Object> mem)
{
    const uint32_t m = mem.address;
    const uint32_t car = at<uint32_t>(m + 0x344);
    sortPosFromCar(m, ptr32<_sObject>(car), 0xb0, 0x10);
    at<uint16_t>(m + kLifeSpan) = static_cast<uint16_t>(at<uint16_t>(m + kLifeSpan) - 1);
    VfxParticleMover(ptr32<_sVfx_PartHeader>(m + kRing), ptr32<_VFX_PARTICLE>(m + kParticles));
    followCar(car);
    if (static_cast<int8_t>(at<uint8_t>(car + 0x19c)) != 0)
        at<uint16_t>(m + kLifeSpan) = 0;
    const uint16_t life = at<uint16_t>(m + kLifeSpan);
    if (life < 0xc)
    {
        if (life == 0)
        {
            at<uint16_t>(car + 0x19a) = 0;
            {
                GuestFrame frame(0xb0);
                guest_call(0x137800u, ptr32<_sObject>(car)); // ObjectWeaponAffectRemove
            }
            at<uint16_t>(car + 0x18c) = 0;
            setSkidFlag(car, 1, false);
        }
        return;
    }
    setSkidFlag(car, 1, true);
    static const WheelPuff puff{5, 0x7f, 0x7f, 0x40, 0x40, 4, false, 0xf0, 4, 4};
    for (uint32_t wheel = 0; wheel < 4; ++wheel)
        spawnWheelPuff(m, car, wheel, puff, 0xb0);
}

// The oil dump's drops: each live particle (life at +0x14) is drawn as a dark drop (code 4), then a coloured gleam (a
// random brightness, halved half the time) half the drop's size.
void VfxRenderer_OilDump(ptr32<_sVfx_Object> mem)
{
    GuestFrame frame(0xa0);
    const uint32_t m = mem.address;
    const uint32_t car = at<uint32_t>(m + 0x1c4);
    if (!carNear(car, 0x157c))
        return;
    followCar(car);
    at<uint8_t>(vfx::sprite() + vfx::spr::kFlipX) = 0;
    uint32_t index = at<uint8_t>(m + kRing + 2);
    for (uint32_t tail = at<uint8_t>(m + kRing + 3); index != tail;)
    {
        const uint32_t p = m + kParticles + index * 0x18;
        const uint32_t sp = vfx::sprite();
        const uint32_t life = at<uint16_t>(p + 0x14);
        if (life != 0)
        {
            at<float>(sp + vfx::spr::kPos) = ee_itof(G(svVelTemp).vx + (at<int16_t>(p) >> 4));
            at<float>(sp + vfx::spr::kPos + 4) = ee_itof(G(svVelTemp).vy + (at<int16_t>(p + 2) >> 4));
            at<float>(sp + vfx::spr::kPos + 8) = ee_itof(G(svVelTemp).vz + (at<int16_t>(p + 4) >> 4));
            at<uint8_t>(sp + vfx::spr::kCode) = 4;
            const int32_t shade = life > 3 ? 0x40 : static_cast<int32_t>(life << 4);
            at<float>(sp + vfx::spr::kScale) = ee_itof(at<uint8_t>(p + 0x16));
            at<uint32_t>(sp + vfx::spr::kTexture) = vfx::vfxTexture(3);
            at<uint32_t>(sp + vfx::spr::kRgb) = static_cast<uint32_t>(shade * 0x10101);
            VfxPS2SpriteRotated3D(ee_float(-15.0f));
            int32_t red = (vfx::random15() & 0x1f) + 0x20;
            int32_t green = (vfx::random15() & 0x1f) + 0x20;
            int32_t blue = (vfx::random15() & 0x1f) + 0x20;
            if (vfx::random15() & 1)
            {
                red >>= 1;
                green >>= 1;
                blue >>= 1;
            }
            at<uint8_t>(sp + vfx::spr::kCode) = 2;
            at<uint32_t>(sp + vfx::spr::kRgb) = static_cast<uint32_t>(red + green * 0x100 + blue * 0x10000);
            at<uint32_t>(sp + vfx::spr::kTexture) = vfx::vfxTexture(0x1a);
            at<float>(sp + vfx::spr::kScale) = ee_itof(at<uint8_t>(p + 0x16) >> 1);
            VfxPS2SpriteRotated3D(ee_float(-15.0f));
        }
        index = (index + 1) & at<uint8_t>(m + kRing);
    }
}

// The speed-up's sprites: a glow for each particle (a bright core that cools to blue, shrinking with its age), then a
// ring of dim smoke puffs (texture 0xe) scattered over the same spots by a random offset of up to 20 units.
void VfxRenderer_SpeedUp(ptr32<_sVfx_Object> mem)
{
    GuestFrame frame(0xb0);
    const uint32_t m = mem.address;
    const uint32_t car = at<uint32_t>(m + 0x644);
    if (!carNear(car, 0x1d4c))
        return;
    followCar(car);
    const uint32_t sp = vfx::sprite();
    at<uint8_t>(sp + vfx::spr::kFlipX) = 0;
    at<uint8_t>(sp + vfx::spr::kCode) = 2;
    at<uint32_t>(sp + vfx::spr::kTexture) = vfx::vfxTexture(0);
    for (uint32_t index = at<uint8_t>(m + kRing + 2); index != at<uint8_t>(m + kRing + 3);)
    {
        const uint32_t p = m + kParticles + index * 0x18;
        const int32_t age = static_cast<int32_t>(at<uint16_t>(p + 0x14)) * -4 + 100;
        at<float>(sp + vfx::spr::kPos) = ee_itof(G(svVelTemp).vx + (at<int16_t>(p) >> 3));
        const int32_t green = std::max(age * -2 + 0xb9, 0), red = std::max(age * -4 + 0xb9, 0);
        at<float>(sp + vfx::spr::kPos + 4) = ee_itof(G(svVelTemp).vy + (at<int16_t>(p + 2) >> 3));
        at<float>(sp + vfx::spr::kScale) = ee_itof(((0x69 - age) >> 1) + 8);
        at<uint16_t>(sp + vfx::spr::kAngle) = at<uint16_t>(p + 0x10);
        at<uint32_t>(sp + vfx::spr::kRgb) = static_cast<uint32_t>(red + green * 0x100 + (0xb9 - age) * 0x10000);
        at<float>(sp + vfx::spr::kPos + 8) = ee_itof(G(svVelTemp).vz + (at<int16_t>(p + 4) >> 3));
        VfxPS2SpriteRotated3D(ee_float(0.0f));
        index = (index + 1) & at<uint8_t>(m + kRing);
    }
    at<uint32_t>(sp + vfx::spr::kRgb) = 0x505050;
    at<float>(sp + vfx::spr::kScale) = 8.0f;
    at<uint32_t>(sp + vfx::spr::kTexture) = vfx::vfxTexture(0xe);
    const ee_float unit(float_from_bits(0x32a00000u)), twenty(20.0f);
    for (uint32_t index = at<uint8_t>(m + kRing + 2); index != at<uint8_t>(m + kRing + 3);)
    {
        const uint32_t p = m + kParticles + index * 0x18;
        const ee_float dx = ee_itof(rand()) * unit - twenty;
        at<float>(sp + vfx::spr::kPos) = dx + ee_itof(G(svVelTemp).vx + (at<int16_t>(p) >> 3));
        const ee_float dy = ee_itof(rand()) * unit - twenty;
        at<float>(sp + vfx::spr::kPos + 4) = dy + ee_itof(G(svVelTemp).vy + (at<int16_t>(p + 2) >> 3));
        const ee_float dz = ee_itof(rand()) * unit - twenty;
        at<float>(sp + vfx::spr::kPos + 8) = dz + ee_itof(G(svVelTemp).vz + (at<int16_t>(p + 4) >> 3));
        at<uint16_t>(sp + vfx::spr::kAngle) = static_cast<uint16_t>(vfx::random15());
        VfxPS2SpriteRotated3D(ee_float(0.0f));
        index = (index + 1) & at<uint8_t>(m + kRing);
    }
}

// A car coming back into the race: a spray of bright streaks from the car's position (each particle a ribbon from the
// car to the particle's offset, fading with its life and a random flicker), then three spinning glows over the car that
// breathe in and out with the effect's life.
void VfxRenderer_CarRespawn(ptr32<_sVfx_Object> mem)
{
    GuestFrame frame(0xd0);
    const uint32_t m = mem.address;
    const uint32_t car = at<uint32_t>(m + 0x2c4);
    if (!carNear(car, 0x1d4c) || at<int16_t>(car + 0x172) != 0)
        return;
    const ptr32<SVECTOR> where = VfxCarPositionPoi(ptr32<_sObject>(car));
    const ptr32<SVECTOR> sv = frame.local<SVECTOR>(0), sv1 = frame.local<SVECTOR>(8);
    const int16_t angle = at<int16_t>(m + 0x2c8);
    // The original sets these before it overwrites them with the car's position.
    sv->vx = static_cast<int16_t>(-angle >> 3);
    sv->vy = static_cast<int16_t>(angle >> 3);
    sv->vz = static_cast<int16_t>(angle >> 2);
    sv->vx = where->vx;
    sv->vy = where->vy;
    sv->vz = where->vz;
    for (int32_t index = at<uint8_t>(m + kRing + 2); index != at<uint8_t>(m + kRing + 3); index = (index + 1) & at<uint8_t>(m + kRing))
    {
        const uint32_t p = m + kParticles + static_cast<uint32_t>(index) * 0x14u;
        sv1->vx = static_cast<int16_t>(sv->vx + (at<int16_t>(p) >> 4));
        sv1->vy = static_cast<int16_t>(sv->vy + (at<int16_t>(p + 2) >> 4));
        sv1->vz = static_cast<int16_t>(sv->vz + (at<int16_t>(p + 4) >> 4));
        int32_t glow = static_cast<int32_t>(at<uint16_t>(m + kLifeSpan)) * 8 - (vfx::random15() & 0xff);
        glow = glow > 7 ? glow : 8;
        glow = glow < 0xdd ? glow : 0xdc;
        VfxPutGT4rgba0(static_cast<uint32_t>(glow) << 16);
        VfxPutGT4rgba1(static_cast<uint32_t>(glow) << 16);
        VfxPutGT4rgba2(0);
        VfxPutGT4rgba3(0);
        VfxPutGT4FacingPoly(sv, sv1, ptr32<_sGfxTexture>(vfx::vfxTexture(0x19)), 2, ee_float(8.0f), ee_float(-10.0f));
    }
    const int32_t phase = static_cast<int32_t>(at<uint16_t>(m + kLifeSpan)) - 0x21;
    int32_t bulk = phase * 2;
    if (bulk < 0)
        bulk = phase * -4;
    bulk = -bulk;
    const uint32_t colour = static_cast<uint32_t>(bulk + 0x96);
    const uint32_t sp = vfx::sprite();
    at<uint8_t>(sp + vfx::spr::kCode) = 2;
    at<uint32_t>(sp + vfx::spr::kTexture) = vfx::vfxTexture(0x14);
    const int16_t spin = at<int16_t>(m + 0x2c8);
    at<uint16_t>(sp + vfx::spr::kAngle) = static_cast<uint16_t>(-static_cast<int32_t>(spin) >> 1);
    at<float>(sp + vfx::spr::kPos) = ee_itof(where->vx);
    at<float>(sp + vfx::spr::kPos + 4) = ee_itof(where->vy);
    at<float>(sp + vfx::spr::kScale) = ee_itof(bulk + 0xbe);
    at<uint8_t>(sp + vfx::spr::kFlipX) = 0;
    at<uint32_t>(sp + vfx::spr::kRgb) = colour;
    at<float>(sp + vfx::spr::kPos + 8) = ee_itof(where->vz);
    VfxPS2SpriteRotated3D(ee_float(-80.0f));
    at<float>(sp + vfx::spr::kScale) = ee_itof(bulk + 0xb4);
    at<uint32_t>(sp + vfx::spr::kRgb) = colour * 0x100;
    at<uint16_t>(sp + vfx::spr::kAngle) = static_cast<uint16_t>(spin >> 1);
    at<uint8_t>(sp + vfx::spr::kFlipX) = 1;
    VfxPS2SpriteRotated3D(ee_float(-81.0f));
    at<uint8_t>(sp + vfx::spr::kFlipX) = 1;
    at<float>(sp + vfx::spr::kScale) = ee_itof(bulk + 0xd2);
    at<uint32_t>(sp + vfx::spr::kRgb) = colour * 0x10000;
    at<uint16_t>(sp + vfx::spr::kAngle) = static_cast<uint16_t>(0x400 - (spin >> 2));
    VfxPS2SpriteRotated3D(ee_float(-82.0f));
}

// A car changing over (the car at +0x2c4, a spin at +0x2cc): streaks from the car's position as the respawn has them (in
// white), then three glows (texture 0x11) that shrink in over the last frames.
void VfxRenderer_CarChangeOver(ptr32<_sVfx_Object> mem)
{
    GuestFrame frame(0xe0);
    const uint32_t m = mem.address;
    const uint32_t car = at<uint32_t>(m + 0x2c4);
    if (!carNear(car, 0x1d4c))
        return;
    followCar(car);
    const int16_t spin = at<int16_t>(m + 0x2cc);
    const ptr32<SVECTOR> sv = frame.local<SVECTOR>(0), sv1 = frame.local<SVECTOR>(8);
    sv->vz = static_cast<int16_t>(spin >> 2);
    sv->vx = static_cast<int16_t>(-static_cast<int32_t>(spin) >> 3);
    sv->vy = static_cast<int16_t>(spin >> 3);
    for (uint32_t index = at<uint8_t>(m + kRing + 2); index != at<uint8_t>(m + kRing + 3); index = (index + 1) & at<uint8_t>(m + kRing))
    {
        const uint32_t p = m + kParticles + index * 0x14u;
        sv->vx = G(svVelTemp).vx;
        sv->vy = G(svVelTemp).vy;
        sv1->vx = static_cast<int16_t>((at<int16_t>(p) >> 4) + G(svVelTemp).vx);
        sv1->vy = static_cast<int16_t>((at<int16_t>(p + 2) >> 4) + G(svVelTemp).vy);
        sv1->vz = static_cast<int16_t>((at<int16_t>(p + 4) >> 4) + G(svVelTemp).vz);
        sv->vz = G(svVelTemp).vz;
        int32_t glow = static_cast<int32_t>(at<uint16_t>(m + kLifeSpan)) * 8 - (vfx::random15() & 0xff);
        glow = glow > 7 ? glow : 8;
        glow = glow < 0xdd ? glow : 0xdc;
        VfxPutGT4rgba2(0);
        VfxPutGT4rgba3(0);
        VfxPutGT4rgba0(static_cast<uint32_t>(glow * 0x10101));
        VfxPutGT4rgba1(static_cast<uint32_t>(glow * 0x10101));
        VfxPutGT4FacingPoly(sv, sv1, ptr32<_sGfxTexture>(vfx::vfxTexture(0x19)), 2, ee_float(8.0f), ee_float(-10.0f));
    }
    const uint32_t sp = vfx::sprite();
    const uint16_t life = at<uint16_t>(m + kLifeSpan);
    at<uint8_t>(sp + vfx::spr::kCode) = 2;
    at<uint32_t>(sp + vfx::spr::kTexture) = vfx::vfxTexture(0x11);
    int32_t phase = static_cast<int32_t>(life) - 0x41;
    phase = phase < -0x19 ? -0x19 : phase;
    at<uint16_t>(sp + vfx::spr::kAngle) = static_cast<uint16_t>(-static_cast<int32_t>(spin) >> 1);
    int32_t bulk = phase * 5;
    if (bulk < 0)
        bulk = phase * -5;
    bulk = -bulk;
    const int32_t green = bulk + 0x7d;
    at<float>(sp + vfx::spr::kPos) = ee_itof(G(svVelTemp).vx);
    at<float>(sp + vfx::spr::kPos + 4) = ee_itof(G(svVelTemp).vy);
    at<float>(sp + vfx::spr::kScale) = ee_itof(bulk + 0x91);
    at<uint32_t>(sp + vfx::spr::kRgb) = static_cast<uint32_t>(green * 0x100);
    at<uint8_t>(sp + vfx::spr::kFlipX) = 0;
    at<float>(sp + vfx::spr::kPos + 8) = ee_itof(G(svVelTemp).vz);
    VfxPS2SpriteRotated3D(ee_float(-70.0f));
    at<uint32_t>(sp + vfx::spr::kRgb) = static_cast<uint32_t>(green * 2);
    at<float>(sp + vfx::spr::kScale) = ee_itof(bulk + 0x9b);
    at<uint16_t>(sp + vfx::spr::kAngle) = static_cast<uint16_t>(spin >> 1);
    at<uint8_t>(sp + vfx::spr::kFlipX) = 1;
    VfxPS2SpriteRotated3D(ee_float(-70.0f));
    at<uint8_t>(sp + vfx::spr::kFlipX) = 1;
    at<float>(sp + vfx::spr::kScale) = ee_itof(bulk + 0xa5);
    at<uint32_t>(sp + vfx::spr::kRgb) = static_cast<uint32_t>(green * 0x20000);
    at<uint16_t>(sp + vfx::spr::kAngle) = static_cast<uint16_t>(0x400 - (spin >> 2));
    VfxPS2SpriteRotated3D(ee_float(-70.0f));
}

// Sarge's lights (the headlights of the army car, at +0x650): two glows at fixed points of the car's body (a different pair
// for the type 0x16 car), turned by the car's matrix. The left one flashes with the effect's counter's low byte, the
// right with the same counter 0x46 later and in another colour. Not drawn while the car's render object is hidden
// (its brightness word is 0xff000000).
void VfxRenderer_SargeLights(ptr32<_sVfx_Object> mem)
{
    GuestFrame frame(0xc0);
    const uint32_t m = mem.address;
    const uint32_t car = at<uint32_t>(m + 0x650);
    if (!carNear(car, 0x157c))
        return;
    if (at<uint32_t>(at<uint32_t>(at<uint32_t>(car + 0x18)) + 0x64) == 0xff000000u)
        return;
    const uint32_t state = integerState(car);
    const int16_t px = static_cast<int16_t>(at<int32_t>(state + 4) >> 8), py = static_cast<int16_t>(at<int32_t>(state + 8) >> 8),
                  pz = static_cast<int16_t>(at<int32_t>(state + 0xc) >> 8);
    G(svVelTemp).vx = px;
    G(svVelTemp).vy = py;
    G(svVelTemp).vz = pz;
    int32_t offsetX = 0xd, offsetY = 0x19, offsetZ = -5;
    if (at<uint32_t>(at<uint32_t>(car + 0xc) + 4) == 0x16)
    {
        offsetX = 0xf;
        offsetY = 0x26;
        offsetZ = 0xb;
    }
    const uint32_t offset = frame.local<uint8>(0x10).address; // svOffset
    const auto turn = [&](int32_t x)
    {
        at<int16_t>(offset) = static_cast<int16_t>(x);
        at<int16_t>(offset + 2) = static_cast<int16_t>(offsetY);
        at<int16_t>(offset + 4) = static_cast<int16_t>(offsetZ);
        PS2C_SetGteMatrix(ptr32<_sMat>(state + 0x28));
        PS2C_GteMatrixMulVec16(ptr32<_sVec16>(offset));
        PS2C_GetVec16GteResult(ptr32<_sVec16>(offset));
    };
    const auto place = [&](uint32_t s)
    {
        at<float>(s + vfx::spr::kPos) = ee_itof(px + at<int16_t>(offset));
        at<float>(s + vfx::spr::kPos + 4) = ee_itof(py + at<int16_t>(offset + 2));
        at<float>(s + vfx::spr::kPos + 8) = ee_itof(pz + at<int16_t>(offset + 4));
    };
    turn(offsetX);
    int32_t level = at<uint8_t>(m + 0x654);
    if (level > 0x80)
        level = 0x100 - level;
    uint32_t sp = vfx::sprite();
    at<uint8_t>(sp + vfx::spr::kFlipX) = 0;
    if (level > 0x6e)
        level += 0x70;
    at<uint8_t>(vfx::sprite() + vfx::spr::kCode) = 2;
    sp = vfx::sprite();
    at<uint32_t>(sp + vfx::spr::kRgb) = static_cast<uint32_t>(level);
    at<float>(sp + vfx::spr::kScale) = 22.0f;
    at<uint32_t>(sp + vfx::spr::kTexture) = vfx::vfxTexture(1);
    at<uint16_t>(sp + vfx::spr::kAngle) = 0;
    place(sp);
    VfxPS2SpriteRotated3D(ee_float(-20.0f));
    turn(-offsetX);
    sp = vfx::sprite();
    place(sp);
    int32_t second = static_cast<int32_t>((at<uint32_t>(m + 0x654) + 0x46) & 0xff);
    if (second > 0x80)
        second = 0x100 - second;
    if (second > 0x6e)
        second += 0x7d;
    at<uint32_t>(sp + vfx::spr::kRgb) = static_cast<uint32_t>((second >> 1) * 0x100 + second * 0x10000);
    VfxPS2SpriteRotated3D(ee_float(-20.0f));
}

namespace {

// The colour of a skid mark's end: the skid colour of its material (three bytes of the material table), darkened by how
// young the mark is: 0xff less 0x20 a frame of the particle's life, none below zero.
uint32_t skidColour(uint32_t particle)
{
    const uint32_t material = at<uint8_t>(particle + 0x13);
    const uint32_t colour = 0x1edcf0u + material * 0x18;
    const int32_t fade = std::max(static_cast<int32_t>(at<uint16_t>(particle + 0x10)) * -0x20 + 0xff, 0);
    const int32_t green = std::max(static_cast<int32_t>(at<uint8_t>(colour + 1)) - fade, 0);
    const int32_t red = std::max(static_cast<int32_t>(at<uint8_t>(colour)) - fade, 0);
    const int32_t blue = std::max(static_cast<int32_t>(at<uint8_t>(colour + 2)) - fade, 0);
    return static_cast<uint32_t>(red + green * 0x100 + blue * 0x10000);
}

} // namespace

// The skid marks a car leaves (a ring of 0x14-byte marks, each two points of a ribbon, active when its flag at +0x12
// is set): every active mark but the oldest is drawn as a quad from the previous mark's two points to its own, in the
// material's skid colour (faded by age) and black at the old end. The tyre-track textures (0xc for the two surfaces
// that leave tracks 0x14 and 0x23, else 4) differ by the mark's material.
void VfxRenderer_CarSkidMark(ptr32<_sVfx_Object> mem)
{
    GuestFrame frame(0xb0);
    const uint32_t m = mem.address;
    bool previousActive = false;
    G(visFxVarG).pGT4->zOffset = -35.0f;
    if (!(1 < at<uint8_t>(m + kRing + 1)))
        return;
    uint32_t tail = at<uint8_t>(m + kRing + 3);
    uint32_t index = at<uint8_t>(m + kRing + 2);
    if (index == tail)
        return;
    const uint32_t previous = frame.local<uint8>(0).address; // sv[0] and sv[1]: the last mark's two points
    do
    {
        const uint32_t p = m + kParticles + index * 0x14u;
        if (at<uint8_t>(p + 0x12) != 0 && index != at<uint8_t>(m + kRing + 2))
        {
            VfxPutGT4vecs(ptr32<SVECTOR>(p), ptr32<SVECTOR>(p + 8), ptr32<SVECTOR>(previous), ptr32<SVECTOR>(previous + 8));
            const uint8_t material = at<uint8_t>(p + 0x13);
            if (material == 0x14 || material == 0x23)
                RdrPolyTexture(0, ptr32<_sGfxTexture>(vfx::vfxTexture(0xc)), 2);
            else
                RdrPolyTexture(0, ptr32<_sGfxTexture>(vfx::vfxTexture(4)), 4);
            const uint32_t next = (index + 1) & at<uint8_t>(m + kRing);
            const uint32_t nextParticle = m + kParticles + next * 0x14u;
            uint32_t colour = 0;
            if (!(next == tail || at<uint8_t>(nextParticle + 0x12) == 0))
                colour = skidColour(nextParticle);
            VfxPutGT4rgba0(colour);
            VfxPutGT4rgba1(colour);
            colour = previousActive ? skidColour(p) : 0;
            VfxPutGT4rgba2(colour);
            VfxPutGT4rgba3(colour);
            RdrPoly(0, G(visFxVarG).pGT4);
            tail = at<uint8_t>(m + kRing + 3);
        }
        std::memcpy(&at<uint8_t>(previous), &at<uint8_t>(p), 8);
        previousActive = at<uint8_t>(p + 0x12) != 0;
        std::memcpy(&at<uint8_t>(previous + 8), &at<uint8_t>(p + 8), 8);
        index = (index + 1) & at<uint8_t>(m + kRing);
    } while (index != tail);
}

// The quake: a ring of points (floats from +0x50, 0x30 bytes each) round the centre (shorts at +0x654), drawn as a band
// of quads between each point and the next (wrapping round by the ring's mask, as a remainder): a fan of three quads
// per point at two radii, one shifted 220 units up and one 460 units further down, with the texture sliding along
// with the effect's life. The colour (red falling to nothing) fades with the life.
void VfxRenderer_Quake(ptr32<_sVfx_Object> mem)
{
    GuestFrame frame(0xe0);
    const uint32_t m = mem.address;
    const uint32_t centre = frame.local<float>(0).address; // vuCentre; its fourth lane is a word of the stack nobody wrote
    const uint32_t life = at<uint16_t>(m + kLifeSpan);
    int32_t fade = static_cast<int32_t>(life * static_cast<uint32_t>(-4) + 0x78);
    at<float>(centre) = ee_itof(at<int16_t>(m + 0x654));
    at<float>(centre + 4) = ee_itof(at<int16_t>(m + 0x656));
    fade = fade < 0 ? 0 : fade;
    at<float>(centre + 8) = ee_itof(at<int16_t>(m + 0x658));
    const int32_t green = std::max(fade * -2 + 0x78, 0), blue = std::max(fade * -4 + 0x78, 0);
    const uint32_t colour = static_cast<uint32_t>((0x78 - (fade >> 1)) + green * 0x100 + blue * 0x10000);
    ee_float slide = ee_itof(static_cast<int32_t>(life)) * ee_float(at<float>(0x327834u));
    const uint8_t head = at<uint8_t>(m + kRing + 2);
    const uint32_t quad = G(visFxVarG).pGT4.address;
    RdrPolyTexture(0, ptr32<_sGfxTexture>(vfx::vfxTexture(0x10)), 0x12);
    const auto scale = [](float factor, uint32_t dst, uint32_t src)
    { guest_call(0x1c8338u, ee_float(factor), ptr32<ee_float>(dst), ptr32<ee_float>(src)); }; // sceVu0ScaleVectorXYZ
    if (head != at<uint8_t>(m + kRing + 3))
    {
        const ee_float one(1.0f), band(460.0f);
        uint32_t index = head;
        do
        {
            const uint32_t from = m + 0x50 + index * 0x30u;
            const float inner = at<float>(0x327838u);
            vfx::vu0Add(quad, centre, from);
            const uint32_t mask = at<uint8_t>(m + kRing);
            // The original divides by the ring's mask and traps on a zero one; the inputs never have one.
            const uint32_t to = m + 0x50 + ((index + 1) % mask) * 0x30u;
            vfx::vu0Add(quad + 0x10, centre, to);
            scale(inner, quad + 0x20, from);
            vfx::vu0Add(quad + 0x20, centre, quad + 0x20);
            const float outer = at<float>(0x32783cu);
            scale(inner, quad + 0x30, to);
            vfx::vu0Add(quad + 0x30, centre, quad + 0x30);
            VfxPutUV0(slide, ee_float(0.0f));
            const ee_float next = slide + ee_float(at<float>(0x327840u));
            VfxPutUV1(next, ee_float(0.0f));
            VfxPutUV2(slide, one);
            VfxPutUV3(next, one);
            VfxPutGT4rgba0(colour);
            VfxPutGT4rgba1(colour);
            VfxPutGT4rgba2(0);
            VfxPutGT4rgba3(0);
            RdrPoly(0, ptr32<_sRdrPoly>(quad));
            scale(outer, quad + 0x20, from);
            vfx::vu0Add(quad + 0x20, centre, quad + 0x20);
            scale(outer, quad + 0x30, to);
            vfx::vu0Add(quad + 0x30, centre, quad + 0x30);
            const ee_float lowerRight = at<float>(quad + 0x34);
            at<float>(quad + 0x24) = ee_float(at<float>(quad + 0x24)) + ee_float(220.0f);
            at<float>(quad + 0x34) = lowerRight + ee_float(220.0f);
            RdrPoly(0, ptr32<_sRdrPoly>(quad));
            const ee_float lowerRight2 = at<float>(quad + 0x34);
            at<float>(quad + 0x24) = ee_float(at<float>(quad + 0x24)) - band;
            at<float>(quad + 0x34) = lowerRight2 - band;
            RdrPoly(0, ptr32<_sRdrPoly>(quad));
            index = at<uint8_t>(m + kRing) & (index + 1);
            slide = next;
        } while (index != at<uint8_t>(m + kRing + 3));
    }
    VfxPutUV0(ee_float(1.0f), ee_float(0.0f));
    VfxPutUV1(ee_float(0.0f), ee_float(0.0f));
    VfxPutUV2(ee_float(1.0f), ee_float(1.0f));
    VfxPutUV3(ee_float(0.0f), ee_float(1.0f));
}

// A bolt of lightning from v0 to v1, as a ribbon of 16 short segments each wandering off the straight line by a random
// offset (scaled by offScale) at its end. The ribbon's colour is white one time in sixteen, otherwise a random blue-green.
// With a count of 1, one segment in sixteen throws off a fork (a second, shorter bolt, drawn after the first, from the
// point where it left to a random point 0x7f units away). The ribbon's ends fade out.
void VfxDrawLightningBetween(ptr32<ee_float> v0, ptr32<ee_float> v1, int32 count, ee_float size, ee_float offScale)
{
    GuestFrame frame(0x130);
    const ptr32<ee_float> line = frame.local<ee_float>(0), offset = frame.local<ee_float>(0x10), random = frame.local<ee_float>(0x20),
                          old = frame.local<ee_float>(0x30), forkFrom = frame.local<ee_float>(0x40), forkTo = frame.local<ee_float>(0x50);
    uint32_t colour;
    if ((vfx::random15() & 0xf) == 0)
        colour = 0xc0c0c0;
    else
    {
        const int32_t green = vfx::random15(), blue = vfx::random15();
        colour = static_cast<uint32_t>(((green & 0x7f) + 3) * 0x100 + ((blue & 0x7f) + 0x50) * 0x10000);
    }
    const ee_float step(0.0625f);
    vfx::vu0Sub(line.address, v1.address, v0.address);
    ee_float along = step;
    const auto scaleXYZ = [](ee_float factor, ptr32<ee_float> dst, ptr32<ee_float> src) { guest_call(0x1c8338u, factor, dst, src); };
    do
    {
        scaleXYZ(along, offset, line);
        random[0] = ee_itof((vfx::random15() & 0x7f) - 0x3f);
        random[1] = ee_itof((vfx::random15() & 0x7f) - 0x3f);
        vfx::vu0Add(offset.address, offset.address, v0.address);
        random[2] = ee_itof((vfx::random15() & 0x7f) - 0x3f);
        scaleXYZ(offScale, random, random);
        vfx::vu0Add(offset.address, offset.address, random.address);
        _sRdrPoly &quad = *G(visFxVarG).pGT4;
        if (along == step)
        {
            const ptr32<_sGfxTexture> texture(vfx::vfxTexture(0x19));
            quad.rgba[3] = colour;
            quad.rgba[0] = 0;
            quad.rgba[1] = 0;
            quad.rgba[2] = colour;
            VfxPutGT4FacingPolyVU(v0, offset, texture, 2, size, ee_float(-10.0f));
        }
        else
        {
            quad.rgba[3] = colour;
            quad.rgba[0] = colour;
            quad.rgba[1] = colour;
            quad.rgba[2] = colour;
            VfxPutGT4FacingContinueVU(old, offset);
        }
        guest_call(0x1c82e8u, old, offset); // sceVu0CopyVectorXYZ
        if (count == 1 && (vfx::random15() & 0xf) == 0)
        {
            count = -1;
            random[0] = ee_itof((vfx::random15() & 0xff) - 0x7f);
            random[1] = ee_itof((vfx::random15() & 0xff) - 0x7f);
            random[2] = ee_itof((vfx::random15() & 0xff) - 0x7f);
            vfx::vu0Add(forkTo.address, offset.address, random.address);
            sceVu0CopyVector(forkFrom, offset);
        }
        along = along + step;
    } while (along < ee_float(1.0f));
    _sRdrPoly &quad = *G(visFxVarG).pGT4;
    quad.rgba[1] = colour;
    quad.rgba[3] = 0;
    quad.rgba[0] = colour;
    quad.rgba[2] = 0;
    VfxPutGT4FacingContinueVU(old, v1);
    if (count == -1)
        VfxDrawLightningBetween(forkFrom, forkTo, 0, ee_float(6.0f), ee_float(at<float>(0x327874u)));
}

} // namespace rc

// Forward declarations of the random-input setups (visfx_fuzz.cpp).
namespace rc::fuzz {
void vfxCarHeatRayAerial(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxKaboom(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxInvincible(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxArmCannon(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxCarFireRenderer(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxArmCannonRenderer(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxBargeOilRenderer(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxKaboomRenderer(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxWheelDust(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxHeatRayDamage(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxFireworkRenderer(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxCarRocket(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxBomb(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxWheelDustHandler(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxSpeedUpHandler(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxHeatRayDamageHandler(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxOilDump(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxSpeedUpRenderer(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxCarRespawn(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxSargeLights(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxCarSkidMark(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxQuake(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxLightningBetween(R5900Context *ctx, uint64_t &state, uint32_t scratch);
}

RC_DECOMP_FN_SETUP(VfxHandler_CarHeatRayAerial, 0x15bf68, rc::fuzz::vfxCarHeatRayAerial)
RC_DECOMP_FN_SETUP(VfxHandler_Kaboom, 0x161790, rc::fuzz::vfxKaboom)
RC_DECOMP_FN_SETUP(VfxHandler_Firework, 0x16ba00, rc::fuzz::vfxKaboom)
RC_DECOMP_FN_SETUP(VfxHandler_Invincible, 0x165cd8, rc::fuzz::vfxInvincible)
RC_DECOMP_FN_SETUP(VfxHandler_ArmCannon, 0x174e28, rc::fuzz::vfxArmCannon)
RC_DECOMP_FN_SETUP(VfxHandler_CarFire, 0x1714c8, rc::fuzz::vfxCarHeatRayAerial)
RC_DECOMP_FN_SETUP(VfxRenderer_CarFire, 0x171730, rc::fuzz::vfxCarFireRenderer)
RC_DECOMP_FN_SETUP(VfxRenderer_ArmCannon, 0x174fb8, rc::fuzz::vfxArmCannonRenderer)
RC_DECOMP_FN_SETUP(VfxRenderer_BargeOil, 0x174a58, rc::fuzz::vfxBargeOilRenderer)
RC_DECOMP_FN_SETUP(VfxRenderer_Kaboom, 0x161980, rc::fuzz::vfxKaboomRenderer)
RC_DECOMP_FN_SETUP(VfxRenderer_WheelDust, 0x15e760, rc::fuzz::vfxWheelDust)
RC_DECOMP_FN_SETUP(VfxRenderer_HeatRayDamage, 0x1597c0, rc::fuzz::vfxHeatRayDamage)
RC_DECOMP_FN_SETUP(VfxRenderer_Firework, 0x16bc00, rc::fuzz::vfxFireworkRenderer)
RC_DECOMP_FN_SETUP(VfxRenderer_CarRocket, 0x169508, rc::fuzz::vfxCarRocket)
RC_DECOMP_FN_SETUP(VfxHandler_Bomb, 0x166750, rc::fuzz::vfxBomb)
RC_DECOMP_FN_SETUP(VfxHandler_WheelDust, 0x15e438, rc::fuzz::vfxWheelDustHandler)
RC_DECOMP_FN_SETUP(VfxHandler_SpeedUp, 0x163360, rc::fuzz::vfxSpeedUpHandler)
RC_DECOMP_FN_SETUP(VfxHandler_BoostSpeedUp, 0x163680, rc::fuzz::vfxSpeedUpHandler)
RC_DECOMP_FN_SETUP(VfxHandler_HeatRayDamage, 0x159498, rc::fuzz::vfxHeatRayDamageHandler)
RC_DECOMP_FN_SETUP(VfxRenderer_OilDump, 0x15a738, rc::fuzz::vfxOilDump)
RC_DECOMP_FN_SETUP(VfxRenderer_SpeedUp, 0x163990, rc::fuzz::vfxSpeedUpRenderer)
RC_DECOMP_FN_SETUP(VfxRenderer_CarRespawn, 0x16c318, rc::fuzz::vfxCarRespawn)
RC_DECOMP_FN_SETUP(VfxRenderer_CarChangeOver, 0x164060, rc::fuzz::vfxCarRespawn)
RC_DECOMP_FN_SETUP(VfxRenderer_SargeLights, 0x162ff0, rc::fuzz::vfxSargeLights)
RC_DECOMP_FN_SETUP(VfxRenderer_CarSkidMark, 0x15b5e0, rc::fuzz::vfxCarSkidMark)
RC_DECOMP_FN_SETUP(VfxRenderer_Quake, 0x162c68, rc::fuzz::vfxQuake)
RC_DECOMP_FN_SETUP(VfxDrawLightningBetween, 0x1681f0, rc::fuzz::vfxLightningBetween)
