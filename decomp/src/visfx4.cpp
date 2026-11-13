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

namespace {

using vfx::at;
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
