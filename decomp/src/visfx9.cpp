// Decompiled from VISFX.c, continued (seventh batch, second file): the renderers of the car effects (water ball, take
// terror, bouncing bomb, aerial and damage lightning, boat wake and foam) and the car shadows. The handlers are in
// visfx8.cpp; see visfx5.cpp for the renderers' conventions.

#include "ai_fuzz.h"
#include "ee_math.h"
#include "vifpk.h"
#include "visfx.h"

#include <emmintrin.h>

namespace rc {

namespace {

using vfx::at;

constexpr uint32_t kRing = 0x40;       // the ring header: mask, count, head, tail
constexpr uint32_t kParticles = 0x44;  // the first particle
constexpr uint32_t kVisFx = 0x1f1000;  // visFxVarG
constexpr uint32_t kMainVar = 0x1ed900; // mainVarG
constexpr uint32_t kUnitMatrix = 0x1c7ac8, kRotMatrix = 0x1c7d60;

// The ring's bytes, all read with lbu (unsigned indexes), as in visfx8.cpp.
inline uint8_t &ringMask(uint32_t m) { return at<uint8_t>(m + kRing); }
inline uint8_t &ringCount(uint32_t m) { return at<uint8_t>(m + kRing + 1); }
inline uint8_t &ringHeadU(uint32_t m) { return at<uint8_t>(m + kRing + 2); }
inline uint8_t &ringTail(uint32_t m) { return at<uint8_t>(m + kRing + 3); }

inline void add16(uint32_t address, int32_t value) { at<uint16_t>(address) = static_cast<uint16_t>(at<uint16_t>(address) + value); }
int32_t atLeastZero(int32_t value) { return value > -1 ? value : 0; }

// An unsigned word converted to float as the compiler does it.
ee_float unsignedToFloat(uint32_t count)
{
    if (static_cast<int32_t>(count) >= 0)
        return ee_itof(static_cast<int32_t>(count));
    const ee_float half = ee_itof(static_cast<int32_t>((count >> 1) | (count & 1)));
    return half + half;
}

} // namespace

namespace {
// The car's draw distance as the renderers test it (as VfxHasCarBeenRendered): in a race that is over or a demo race, a
// car that is drawn counts as 100 units away.
uint16_t carDrawnDistance(uint32_t car)
{
    if ((at<uint32_t>(kMainVar) & 0x30) == 0)
        return at<uint16_t>(car + 0x174);
    return at<int16_t>(car + 0x172) != 0 ? at<uint16_t>(car + 0x174) : 100;
}

inline void spriteLook(uint32_t code, uint32_t texture, uint32_t rgb, ee_float scale, uint16_t angle)
{
    const uint32_t s = vfx::sprite();
    at<uint8_t>(s + vfx::spr::kCode) = static_cast<uint8_t>(code);
    at<uint32_t>(s + vfx::spr::kTexture) = texture;
    at<uint32_t>(s + vfx::spr::kRgb) = rgb;
    at<float>(s + vfx::spr::kScale) = scale;
    at<uint16_t>(s + vfx::spr::kAngle) = angle;
}

inline void spritePos(ee_float x, ee_float y, ee_float z)
{
    const uint32_t s = vfx::sprite();
    at<float>(s + vfx::spr::kPos) = x;
    at<float>(s + vfx::spr::kPos + 4) = y;
    at<float>(s + vfx::spr::kPos + 8) = z;
}
} // namespace

// The water ball weapon round a car (the handler above moves the drops): while the car is near enough to be drawn, one
// small water sprite per charge the car holds circles the car's aerial (each on its own sine and cosine of the effect's
// life), then the drops (smaller and fading to the splash texture in their last five frames), then the ball itself: two
// blue sprites spinning opposite ways and a grey halo.
void VfxRenderer_CarWaterBall(ptr32<_sVfx_Object> mem)
{
    GuestFrame frame(0xe0);
    const uint32_t m = mem.address;
    const uint32_t car = at<uint32_t>(m + 0x650);
    const uint64_t aerial = VfxCarAerialVal(ptr32<_sObject>(car), 5);
    if (!(carDrawnDistance(car) < 0x157c))
        return;
    const ee_float ax = ee_itof(static_cast<int16_t>(aerial)), ay = ee_itof(static_cast<int16_t>(aerial >> 16)),
                   az = ee_itof(static_cast<int16_t>(aerial >> 32));
    at<uint8_t>(vfx::sprite() + vfx::spr::kFlipX) = 0;
    spriteLook(2, vfx::vfxTexture(7), 0x787878, ee_float(15.0f), at<uint16_t>(m + 0x654));
    const ee_float turn(at<float>(0x3277e4u));
    const auto wave = [&](uint32_t angle) { const ee_float a = ee_itof(static_cast<int32_t>(angle & 0xfff)); return (a + a) * turn; };
    for (int32_t i = 0; i < at<int8_t>(car + 0x194); ++i)
    {
        const uint32_t life = at<uint16_t>(m + 0x10), step = static_cast<uint32_t>(i) << 8;
        const int32_t dx = ee_ftoi(ee_sinf(wave((life << ((i + 5) & 31)) + step)) * ee_float(4096.0f)) >> 9;
        const int32_t dy = ee_ftoi(ee_sinf(wave((at<uint16_t>(m + 0x10) << ((7 - i) & 31)) - step)) * ee_float(4096.0f)) >> 9;
        const int32_t dz = ee_ftoi(ee_cosf(wave((at<uint16_t>(m + 0x10) << ((8 - i) & 31)) + step)) * ee_float(4096.0f)) >> 9;
        at<uint16_t>(vfx::sprite() + vfx::spr::kAngle) = static_cast<uint16_t>(step - at<uint16_t>(m + 0x654));
        spritePos(ax + ee_itof(dx), ay + ee_itof(dy), az + ee_itof(dz));
        VfxPS2SpriteRotated3D(ee_float(0.0f));
    }
    at<uint8_t>(vfx::sprite() + vfx::spr::kCode) = 2;
    for (uint32_t index = ringHeadU(m); index != ringTail(m); index = (index + 1) & ringMask(m))
    {
        const uint32_t p = m + 0x50 + index * 0x30;
        sceVu0CopyVector(ptr32<ee_float>(vfx::sprite() + vfx::spr::kPos), ptr32<ee_float>(p));
        const uint32_t s = vfx::sprite();
        const uint32_t life = at<uint32_t>(p + 0x24);
        int32_t red = 0x5a, green = 0x78, blue = 0x78;
        if (life < 6)
        {
            const int32_t fade = static_cast<int32_t>(5 - life);
            blue = 0x78 - fade * 16;
            green = blue;
            red = 0x5a - fade * 16;
            at<uint32_t>(s + vfx::spr::kTexture) = vfx::vfxTexture(8);
            at<float>(s + vfx::spr::kScale) = unsignedToFloat(static_cast<uint32_t>(fade * 2 + 4));
        }
        else
        {
            at<float>(s + vfx::spr::kScale) = 4.0f;
            at<uint32_t>(s + vfx::spr::kTexture) = vfx::vfxTexture(7);
        }
        at<uint16_t>(s + vfx::spr::kAngle) = 0;
        at<uint32_t>(s + vfx::spr::kRgb) = static_cast<uint32_t>(red + (green << 8) + (blue << 16));
        VfxPS2SpriteRotated3D(ee_float(-30.0f));
    }
    spritePos(ax, ay, az);
    spriteLook(4, vfx::vfxTexture(6), 0x2d4696, ee_float(at<float>(0x3277e8u)), at<uint16_t>(m + 0x654));
    VfxPS2SpriteRotated3D(ee_float(-31.0f));
    at<uint16_t>(vfx::sprite() + vfx::spr::kAngle) = static_cast<uint16_t>(-at<uint16_t>(m + 0x654));
    VfxPS2SpriteRotated3D(ee_float(-31.0f));
    spriteLook(2, vfx::vfxTexture(7), 0x646464, ee_float(32.0f), 0);
    VfxPS2SpriteRotated3D(ee_float(-33.0f));
}

namespace {
constexpr uint32_t kScaleVectorXYZ = 0x1c8338, kCopyVectorXYZ = 0x1c82e8;
inline void scaleXYZ(ee_float factor, uint32_t dst, uint32_t src) { guest_call(kScaleVectorXYZ, factor, ptr32<ee_float>(dst), ptr32<ee_float>(src)); }
inline void copyXYZ(uint32_t dst, uint32_t src) { guest_call(kCopyVectorXYZ, ptr32<ee_float>(dst), ptr32<ee_float>(src)); }

// A lightning bolt from `from` to `to` (VU vectors): sixteen camera-facing segments of the given texture, each point at
// a sixteenth more of the way, shaken by up to 31 units at random, black at both ends and `colour` between. `diff`,
// `offset`, `shake` and `last` are the original's locals (the difference, the point, the random shake, the last point).
void lightningBolt(uint32_t from, uint32_t to, uint32_t colour, uint32_t texture, ee_float width, ee_float zOffset, uint32_t diff, uint32_t offset,
                   uint32_t shake, uint32_t last)
{
    vfx::vu0Sub(diff, to, from);
    const ee_float step(0.0625f);
    ee_float t = step;
    do
    {
        scaleXYZ(t, offset, diff);
        at<float>(shake) = ee_itof((vfx::random15() & 0x3f) - 0x1f);
        at<float>(shake + 4) = ee_itof((vfx::random15() & 0x3f) - 0x1f);
        vfx::vu0Add(offset, offset, from);
        at<float>(shake + 8) = ee_itof((vfx::random15() & 0x3f) - 0x1f);
        vfx::vu0Add(offset, offset, shake);
        if (t == step)
        {
            VfxPutGT4rgba0(0);
            VfxPutGT4rgba1(0);
            VfxPutGT4rgba2(colour);
            VfxPutGT4rgba3(colour);
            VfxPutGT4FacingPolyVU(ptr32<ee_float>(from), ptr32<ee_float>(offset), ptr32<_sGfxTexture>(texture), 2, width, zOffset);
        }
        else
        {
            VfxPutGT4rgba0(colour);
            VfxPutGT4rgba1(colour);
            VfxPutGT4rgba2(colour);
            VfxPutGT4rgba3(colour);
            VfxPutGT4FacingContinueVU(ptr32<ee_float>(last), ptr32<ee_float>(offset));
        }
        copyXYZ(last, offset);
        t = t + step;
    } while (t < ee_float(1.0f));
    VfxPutGT4rgba0(colour);
    VfxPutGT4rgba1(colour);
    VfxPutGT4rgba2(0);
    VfxPutGT4rgba3(0);
    VfxPutGT4FacingContinueVU(ptr32<ee_float>(last), ptr32<ee_float>(to));
}

// The lightning's colour: three random bytes from `base` up (none below zero), blue lowest.
uint32_t boltColour(int32_t base)
{
    int32_t red = base + (vfx::random15() & 0x3f);
    red = red > -1 ? red : 0;
    int32_t green = base + (vfx::random15() & 0x3f);
    green = green > -1 ? green : 0;
    return static_cast<uint32_t>((vfx::random15() & 0x3f) + (green << 8) + (red << 16));
}
} // namespace

// The take-terror pickup on a car (the car at +0x2cc): its sparks (0x14-byte particles from +0x44 with a life at +0x10)
// as sprites round the car's centre, bigger and brighter while young; then, in the last 48 frames and unless the car is
// invincible, a lightning bolt from the sky (2000 units above the aerial) down to the aerial, brightening as it ends.
void VfxRenderer_TakeTerror(ptr32<_sVfx_Object> mem)
{
    GuestFrame frame(0x140);
    const uint32_t m = mem.address;
    const uint32_t car = at<uint32_t>(m + 0x2cc);
    if (!(carDrawnDistance(car) < 0x157c))
        return;
    const uint64_t aerial = VfxCarAerialVal(ptr32<_sObject>(car), 5);
    (void)VfxCarAerialVal(ptr32<_sObject>(car), 5);
    const uint32_t ground = frame.local<float>(0x20).address, sky = frame.local<float>(0x30).address; // v[0], v[1]
    at<float>(ground) = ee_itof(static_cast<int16_t>(aerial));
    at<float>(ground + 4) = ee_itof(static_cast<int16_t>(aerial >> 16));
    at<float>(ground + 8) = ee_itof(static_cast<int16_t>(aerial >> 32));
    const uint64_t centre = VfxCarAerialVal(ptr32<_sObject>(car), 0);
    sceVu0CopyVector(ptr32<ee_float>(sky), ptr32<ee_float>(ground));
    at<float>(sky + 4) = ee_float(at<float>(sky + 4)) + ee_float(2000.0f);
    at<uint8_t>(vfx::sprite() + vfx::spr::kCode) = 2;
    at<uint32_t>(vfx::sprite() + vfx::spr::kTexture) = vfx::vfxTexture(0);
    for (uint32_t index = ringHeadU(m); index != ringTail(m); index = (index + 1) & ringMask(m))
    {
        const uint32_t p = m + kParticles + index * 0x14;
        const uint32_t life = at<uint16_t>(p + 0x10);
        if (life == 0)
            continue;
        const uint32_t s = vfx::sprite();
        at<uint16_t>(s + vfx::spr::kAngle) = static_cast<uint16_t>(index << 7);
        at<uint32_t>(s + vfx::spr::kRgb) = life + (life << 10) + (life << 19);
        at<float>(s + vfx::spr::kScale) = ee_itof(static_cast<int32_t>(life * 2)) + ee_float(5.0f);
        spritePos(ee_itof(static_cast<int16_t>(centre) + (at<int16_t>(p) >> 3)), ee_itof(static_cast<int16_t>(centre >> 16) + (at<int16_t>(p + 2) >> 3)),
                  ee_itof(static_cast<int16_t>(centre >> 32) + (at<int16_t>(p + 4) >> 3)));
        VfxPS2SpriteRotated3D(ee_float(-10.0f));
    }
    if (at<int8_t>(car + 0x19c) != 0 || !(at<uint16_t>(m + 0x10) < 0x31))
        return;
    int32_t base = (static_cast<int32_t>(at<uint16_t>(m + 0x10)) - 4) * 16;
    base = base < 0xc1 ? base : 0xc0;
    const uint32_t colour = boltColour(base);
    lightningBolt(ground, sky, colour, vfx::vfxTexture(0x19), ee_float(25.0f), ee_float(-10.0f), frame.local<float>(0x40).address, frame.local<float>(0x60).address,
                  frame.local<float>(0x50).address, frame.local<float>(0x70).address);
}

void RdrState_SetStateBlockToDefaults(ptr32<sRdrStateBlock> block); // rdr.cpp
void RdrState_ForceApplyStateBlock(ptr32<sRdrStateBlock> block);

// The bouncing bomb: drawn while the camera is within 7500 units on each axis (its position is the shorts at +0x14). The
// bomb's shape (the effects resource's pWEPbouncy) turns on three axes taken from one angle (+0x2c4, stepping by +0x2c6
// each frame) and is drawn there. While it has a target (the colour word at +0x2d0 is not zero) a lightning bolt runs from
// the bomb to the target (the shorts at +0x2c8), half the colour, often doubled into green, and a spark at the target.
void VfxRenderer_Bomb(ptr32<_sVfx_Object> mem)
{
    GuestFrame frame(0x1b0);
    const uint32_t m = mem.address;
    for (uint32_t axis = 0; axis < 3; ++axis)
    {
        const int32_t camera = at<int32_t>(kVisFx + 0xfc + 4 * axis) >> 8; // vCamPos
        const int32_t where = at<int16_t>(m + 0x14 + 2 * axis);
        const int32_t d = where - camera;
        if (!((d > 0 ? d : camera - where) < 0x1d4c))
            return;
    }
    const int32_t angle = at<int16_t>(m + 0x2c4);
    const ee_float turn(at<float>(0x32785cu));
    const uint32_t rot = frame.local<float>(0x20).address, mat = frame.local<float>(0x80).address;
    at<uint32_t>(rot + 0xc) = 0;
    at<float>(rot) = ee_itof((((-angle) >> 3) & 0xfff) - 0x800) * turn;
    at<float>(rot + 4) = ee_itof(((angle >> 2) & 0xfff) - 0x800) * turn;
    at<float>(rot + 8) = ee_itof(((angle >> 3) & 0xfff) - 0x800) * turn;
    guest_call(kUnitMatrix, ptr32<float>(mat));
    guest_call(kRotMatrix, ptr32<float>(mat), ptr32<float>(mat), ptr32<float>(rot));
    add16(m + 0x2c4, at<uint16_t>(m + 0x2c6));
    const ptr32<sRdrStateBlock> state = frame.local<sRdrStateBlock>(0xc0);
    RdrState_SetStateBlockToDefaults(state);
    RdrState_ForceApplyStateBlock(state);
    const uint32_t shape = at<uint32_t>(G(visFxVarG).pVfxRes.address + 0x80); // pWEPbouncy
    for (uint32_t row = 0; row < 3; ++row)
        for (uint32_t column = 0; column < 3; ++column)
            at<uint32_t>(shape + 8 + 12 * row + 4 * column) = at<uint32_t>(mat + 0x10 * row + 4 * column);
    for (uint32_t axis = 0; axis < 3; ++axis)
        at<float>(shape + 0x2c + 4 * axis) = ee_itof(at<int16_t>(m + 0x14 + 2 * axis));
    const ee_float version(at<float>(shape));
    if (version == ee_float(2.0f))
        guest_call(0x1a5230u, ptr32<uint8>(shape), ptr32<uint8>(0x1f40b8u), 0); // Rdr_VUShape(shape, &rdrCameraG, 0)
    else if (version == ee_float(3.0f))
        guest_call(0x1a55b8u, ptr32<uint8>(shape), ptr32<uint8>(0x1f40b8u), 0); // TRdr_VUShape(shape, &rdrCameraG, 0)
    const int32_t target = at<int32_t>(m + 0x2d0);
    if (target == 0)
        return;
    uint32_t colour = static_cast<uint32_t>(target >> 1);
    const uint32_t from = rot, to = frame.local<float>(0x30).address; // v0, v1
    for (uint32_t axis = 0; axis < 3; ++axis)
    {
        at<float>(from + 4 * axis) = ee_itof(at<int16_t>(m + 0x14 + 2 * axis));
        at<float>(to + 4 * axis) = ee_itof(at<int16_t>(m + 0x2c8 + 2 * axis));
    }
    if ((vfx::random15() & 1) == 0)
        colour = colour + (colour << 16);
    lightningBolt(from, to, colour, vfx::vfxTexture(0x19), ee_float(8.0f), ee_float(0.0f), frame.local<float>(0x40).address,
                  frame.local<float>(0x50).address, frame.local<float>(0x60).address, frame.local<float>(0x70).address);
    at<uint8_t>(vfx::sprite() + vfx::spr::kCode) = 2;
    at<uint32_t>(vfx::sprite() + vfx::spr::kTexture) = vfx::vfxTexture(0);
    const uint32_t s = vfx::sprite();
    spritePos(ee_itof(at<int16_t>(m + 0x2c8)), ee_itof(at<int16_t>(m + 0x2ca)), ee_itof(at<int16_t>(m + 0x2cc)));
    at<float>(s + vfx::spr::kScale) = 56.0f;
    at<uint32_t>(s + vfx::spr::kRgb) = colour;
    at<uint16_t>(vfx::sprite() + vfx::spr::kAngle) = static_cast<uint16_t>(vfx::random15() & 0xfff);
    VfxPS2SpriteRotated3D(ee_float(0.0f));
}

void VfxPS2SpriteGroundRotated3D(ptr32<SVECTOR> svNormal, ee_float zOffset);

namespace {
constexpr uint32_t kNodeCalcWorldPos = 0x1339d0;

// One spark at an aerial point: `phase` (0 to 7) picks the point (phase - 1) and the size (growing to the middle point,
// then shrinking); the sprite lies along the aerial there (the direction to the point before) and spins at random.
void aerialSpark(uint32_t points, const int32_t tip[3], uint32_t phase, uint32_t rgb, uint32_t svDir)
{
    if (static_cast<int32_t>(phase) <= 0)
        return;
    const int32_t n = static_cast<int32_t>(phase) - 1;
    const uint32_t s = vfx::sprite();
    at<uint32_t>(s + vfx::spr::kRgb) = rgb;
    at<float>(s + vfx::spr::kScale) = ee_itof((n - 3 > 0 ? 6 - n : n) * 4 + 0xc);
    const uint32_t q = points + static_cast<uint32_t>(n) * 0x10;
    at<float>(s + vfx::spr::kPos) = ee_float(at<float>(q)) + ee_itof(tip[0]);
    at<float>(s + vfx::spr::kPos + 4) = ee_float(at<float>(q + 4)) + ee_itof(tip[1]);
    at<float>(s + vfx::spr::kPos + 8) = ee_float(at<float>(q + 8)) + ee_itof(tip[2]);
    const int32_t spin = vfx::random15();
    for (uint32_t axis = 0; axis < 3; ++axis)
        at<int16_t>(svDir + 2 * axis) = static_cast<int16_t>(ee_ftoi(ee_float(at<float>(q + 4 * axis)) - ee_float(at<float>(q - 0x10 + 4 * axis))));
    at<uint16_t>(vfx::sprite() + vfx::spr::kAngle) = static_cast<uint16_t>(spin);
    VfxPS2SpriteGroundRotated3D(ptr32<SVECTOR>(svDir), ee_float(0.0f));
}
} // namespace

// Lightning along a car's aerial (the car at +0x78; the aerial's six points are vectors from +0x40 of the record at +0x40
// of the car, moved by the tip's place in the world, NodeCalcWorldPos): five grey segments, each point shaken by the
// effect's offsets (shorts at +0x40, eight bytes apart), and two sparks running along it, eight frames a cycle, three
// frames apart.
void VfxRenderer_AerialLightning(ptr32<_sVfx_Object> mem)
{
    GuestFrame frame(0xe0);
    const uint32_t m = mem.address;
    const uint32_t car = at<uint32_t>(m + 0x78);
    const uint32_t rendObj = at<uint32_t>(car + 0x18);
    const uint32_t position = frame.local<uint8>(0x30).address; // pvPosition
    guest_call(kNodeCalcWorldPos, ptr32<uint8>(at<uint32_t>(car + 0x3c)), ptr32<uint8>(rendObj + 0x18), ptr32<uint8>(rendObj + 4), ptr32<uint8>(position));
    const uint32_t points = at<uint32_t>(car + 0x40) + 0x40;
    if (!(carDrawnDistance(car) < 0x157c))
        return;
    const uint32_t a = frame.local<float>(0).address, b = frame.local<float>(0x10).address; // vu[0], vu[1]
    for (int32_t i = 5; i > 0; --i)
    {
        const uint32_t p = points + static_cast<uint32_t>(i) * 0x10;
        const ee_float tx = ee_itof(at<int32_t>(position) >> 8), ty = ee_itof(at<int32_t>(position + 4) >> 8), tz = ee_itof(at<int32_t>(position + 8) >> 8);
        const uint32_t shake = m + 0x40 + static_cast<uint32_t>(i) * 8, before = shake - 8;
        at<float>(a) = (ee_float(at<float>(p)) + ee_itof(at<int16_t>(shake))) + tx;
        at<float>(a + 4) = (ee_float(at<float>(p + 4)) + ee_itof(at<int16_t>(shake + 2))) + ty;
        at<float>(a + 8) = (ee_float(at<float>(p + 8)) + ee_itof(at<int16_t>(shake + 4))) + tz;
        at<float>(b) = (ee_float(at<float>(p - 0x10)) + ee_itof(at<int16_t>(before))) + tx;
        at<float>(b + 4) = (ee_float(at<float>(p - 0xc)) + ee_itof(at<int16_t>(before + 2))) + ty;
        at<float>(b + 8) = (ee_float(at<float>(p - 8)) + ee_itof(at<int16_t>(before + 4))) + tz;
        VfxPutGT4rgba0(0x606060);
        VfxPutGT4rgba1(0x606060);
        VfxPutGT4rgba2(0x606060);
        VfxPutGT4rgba3(0x606060);
        if (i == 5)
            VfxPutGT4FacingPolyVU(ptr32<ee_float>(a), ptr32<ee_float>(b), ptr32<_sGfxTexture>(vfx::vfxTexture(0x19)), 2, ee_float(10.0f),
                                  ee_float(at<float>(0x3280e4u)));
        else
            VfxPutGT4FacingContinueVU(ptr32<ee_float>(a), ptr32<ee_float>(b));
    }
    at<uint8_t>(vfx::sprite() + vfx::spr::kCode) = 2;
    at<uint32_t>(vfx::sprite() + vfx::spr::kTexture) = vfx::vfxTexture(0xb);
    const int32_t tip[3] = {at<int32_t>(position) >> 8, at<int32_t>(position + 4) >> 8, at<int32_t>(position + 8) >> 8};
    const uint32_t svDir = frame.local<uint8>(0x20).address;
    aerialSpark(points, tip, (0u - at<uint16_t>(m + 0x10)) & 7, 0x282828, svDir);
    aerialSpark(points, tip, (3u - at<uint16_t>(m + 0x10)) & 7, 0x2c2c00, svDir);
}

// The damage lightning on a car's aerial (the car at +0x78): bolts from the aerial's base (its first point moved by the
// tip's place in the world, kept in svVelTemp) to each of the aerial's six points (VfxCarAerialVal, shaken by the shorts
// at +0x40 of the effect, eight bytes apart), fading in from black at the base to yellow-green as the effect ages.
void VfxRenderer_DamageLightning(ptr32<_sVfx_Object> mem)
{
    GuestFrame frame(0x110);
    const uint32_t m = mem.address;
    const uint32_t car = at<uint32_t>(m + 0x78);
    const uint32_t rendObj = at<uint32_t>(car + 0x18);
    const uint32_t position = frame.local<uint8>(0x60).address; // pvPosition
    guest_call(kNodeCalcWorldPos, ptr32<uint8>(at<uint32_t>(car + 0x3c)), ptr32<uint8>(rendObj + 0x18), ptr32<uint8>(rendObj + 4), ptr32<uint8>(position));
    const uint32_t aerial = at<uint32_t>(car + 0x40);
    constexpr uint32_t kSvVelTemp = 0x3280f8;
    for (uint32_t axis = 0; axis < 3; ++axis)
    {
        const ee_float tip = ee_itof(at<int32_t>(position + 4 * axis)) * ee_float(0.00390625f);
        at<int16_t>(kSvVelTemp + 2 * axis) = static_cast<int16_t>(ee_ftoi(ee_float(at<float>(aerial + 0x40 + 4 * axis)) + tip));
    }
    if (!(carDrawnDistance(car) < 0x157c))
        return;
    const uint32_t sv = frame.local<uint8>(0).address; // sv[6], 8 bytes each
    for (int32_t i = 5; i >= 0; --i)
    {
        const uint64_t point = VfxCarAerialVal(ptr32<_sObject>(car), i);
        const uint32_t v = sv + static_cast<uint32_t>(i) * 8, shake = m + 0x40 + static_cast<uint32_t>(i) * 8;
        std::memcpy(&at<uint8_t>(v), &point, 8);
        for (uint32_t axis = 0; axis < 3; ++axis)
            add16(v + 2 * axis, at<uint16_t>(shake + 2 * axis));
    }
    int32_t fade = 0x84 - static_cast<int32_t>(at<uint16_t>(m + 0x10)) * 8;
    VfxPutGT4rgba2(0);
    VfxPutGT4rgba3(0);
    for (int32_t i = 5; i >= 0; --i)
    {
        fade = fade > -1 ? fade : 0;
        const int32_t green = atLeastZero(0xb4 - fade * 2), red = atLeastZero(200 - fade * 2);
        const uint32_t colour = static_cast<uint32_t>((green << 8) + (red << 16));
        VfxPutGT4rgba0(colour);
        VfxPutGT4rgba1(colour);
        VfxPutGT4FacingPoly(ptr32<SVECTOR>(kSvVelTemp), ptr32<SVECTOR>(sv + static_cast<uint32_t>(i) * 8), ptr32<_sGfxTexture>(vfx::vfxTexture(0x19)), 2,
                            ee_float(10.0f), ee_float(at<float>(0x3280e8u)));
    }
}

void VfxPutGT4vecs(ptr32<SVECTOR> svA, ptr32<SVECTOR> svB, ptr32<SVECTOR> svC, ptr32<SVECTOR> svD);

// A boat's wake: a ring of 0x1c-byte cross-sections (two SVECTORs, a flag byte at +0x18 for a section that is on the
// water, an age at +0x1a), drawn with at least two of them as quads between neighbouring sections that are both on the
// water, brighter with age (to 0x6e). First a pass of soft spray shifted towards the camera (at most 20 units sideways),
// then the foam on the water. The previous section and its flag carry over from the first pass into the second, as in
// the original.
void VfxRenderer_BoatWake(ptr32<_sVfx_Object> mem)
{
    GuestFrame frame(0xe0);
    const uint32_t m = mem.address;
    const uint32_t sv = frame.local<uint8>(0).address, shift = frame.local<float>(0x20).address, last = frame.local<uint32_t>(0x30).address;
    at<uint32_t>(last) = 0;
    if (ringCount(m) < 2)
        return;
    const uint32_t texture = vfx::vfxTexture(0xc);
    at<float>(G(visFxVarG).pGT4.address + 0x70) = -25.0f;
    RdrPolyTexture(0, ptr32<_sGfxTexture>(texture), 4);
    vfx::vu0Sub(shift, kVisFx + 0x160, m + 0x20);
    sceVu0ScaleVector(ptr32<ee_float>(shift), ptr32<ee_float>(shift), ee_float(at<float>(0x3277ecu)));
    const ee_float limit(20.0f), lowest(-20.0f);
    for (uint32_t lane : {0u, 8u})
    {
        if (limit < ee_float(at<float>(shift + lane)))
            at<float>(shift + lane) = limit;
        if (ee_float(at<float>(shift + lane)) < lowest)
            at<float>(shift + lane) = lowest;
        if (lane == 0)
            at<uint32_t>(shift + 4) = 0;
    }
    // The brightness of a corner from a section's age (shifted by `shift`), at most 0x6e; `halve` gives the spray's
    // colour (the green and blue at half).
    const auto shade = [](uint32_t age, uint32_t bits, bool halve)
    {
        uint32_t v = age << bits;
        v = v < 0x6f ? v : 0x6e;
        const uint32_t h = halve ? v >> 1 : v;
        return v + (h << 8) + (h << 16);
    };
    for (int32_t pass = 0; pass < 2; ++pass)
    {
        if (pass == 1)
            RdrPolyTexture(0, ptr32<_sGfxTexture>(vfx::vfxTexture(0xc)), 2);
        for (uint32_t index = ringHeadU(m); index != ringTail(m);)
        {
            const uint32_t p = kParticles + m + index * 0x1c;
            if (at<uint8_t>(p + 0x18) != 0 && index != ringHeadU(m))
            {
                VfxPutGT4vecs(ptr32<SVECTOR>(p), ptr32<SVECTOR>(p + 8), ptr32<SVECTOR>(sv), ptr32<SVECTOR>(sv + 8));
                if (pass == 0)
                    for (uint32_t corner = 0; corner < 4; ++corner)
                    {
                        const uint32_t v = G(visFxVarG).pGT4.address + 0x10 * corner;
                        vfx::vu0Add(v, v, shift);
                    }
                const uint32_t next = (index + 1) & ringMask(m);
                const uint32_t front = next == ringTail(m) || at<uint8_t>(m + kParticles + next * 0x1c + 0x18) == 0 ? 0 : shade(at<uint16_t>(p + 0x1a), 4, pass == 0);
                VfxPutGT4rgba0(front);
                VfxPutGT4rgba1(front);
                const uint32_t back = at<uint32_t>(last) == 0 ? 0 : shade(at<uint16_t>(p + 0x1a), 3, pass == 0);
                VfxPutGT4rgba2(back);
                VfxPutGT4rgba3(back);
                RdrPoly(0, ptr32<_sRdrPoly>(G(visFxVarG).pGT4.address));
            }
            index = (index + 1) & ringMask(m);
            std::memcpy(&at<uint8_t>(sv), &at<uint8_t>(p), 8);
            at<uint32_t>(last) = at<uint8_t>(p + 0x18);
            std::memcpy(&at<uint8_t>(sv + 8), &at<uint8_t>(p + 8), 8);
        }
    }
}

void PS2C_GteMatrixMulVec16(ptr32<_sVec16> pVec); // psxdefs.cpp
void PS2C_SetGteMatrix(ptr32<_sMat> pMat);
void PS2C_GetVec16GteResult(ptr32<_sVec16> pOut);

// The foam under a boat (the car at +0x40; not for object type 0x22): while it is within 4500 units and both its front
// and back are in the water (bit 0 of two wheel flags for the smaller types, else the two depth floats above zero), a
// quad under the hull: its four corners are the hull's bounds (shorts at +0x68..+0x72 of the shape header, 10 units in
// at the back) turned by the boat's matrix, scaled by the effect's four size bytes (+0x44) in 32nds round the boat's
// position, at the water heights of its four wheels (floats 0x48 bytes apart from +0xec of the physics). It fades as the
// boat rises (the integer state's +0x24), and is not drawn when a water height is missing or the boat has no wake.
void VfxRenderer_ShadowFoam(ptr32<_sVfx_Object> mem)
{
    GuestFrame frame(0xf0);
    const uint32_t m = mem.address;
    const uint32_t car = at<uint32_t>(m + 0x40);
    const uint32_t type = at<uint32_t>(at<uint32_t>(car + 0xc) + 4);
    if (type == 0x22)
        return;
    const uint16_t drawn = carDrawnDistance(car);
    if (!(drawn < 0x1194))
        return;
    const uint32_t physics = at<uint32_t>(car + 0x20);
    const auto inWater = [&](uint32_t flags, uint32_t depth)
    {
        if (type < 0x14)
            return (at<uint16_t>(physics + flags) & 1) != 0;
        return ee_float(0.0f) < ee_float(at<float>(at<uint32_t>(physics + 0x1e0) + depth));
    };
    if (!inWater(0xe8, 0x78) || !inWater(0x130, 0x208))
        return;
    const uint32_t integer = at<uint32_t>(physics + 0x1e8);
    int16_t position[3];
    for (uint32_t axis = 0; axis < 3; ++axis)
    {
        position[axis] = static_cast<int16_t>(at<int32_t>(integer + 4 + 4 * axis) >> 8);
        at<int16_t>(0x3280f8u + 2 * axis) = position[axis]; // svVelTemp
    }
    position[1] = static_cast<int16_t>(position[1] + 100);
    const uint32_t head = at<uint32_t>(at<uint32_t>(car + 0x18));
    const uint32_t matrix = at<uint32_t>(at<uint32_t>(car + 0x20) + 0x1e8) + 0x28;
    const uint32_t offset = frame.local<uint8>(0x10).address, sv = frame.local<uint8>(0x20).address;
    bool missing = false;
    for (uint32_t corner = 0; corner < 4; ++corner)
    {
        at<uint16_t>(offset + 2) = 0;
        static constexpr uint8_t kX[4] = {0x68, 0x6e, 0x68, 0x6e}, kZ[4] = {0x72, 0x72, 0x6c, 0x6c};
        at<uint16_t>(offset) = at<uint16_t>(head + kX[corner]);
        at<uint16_t>(offset + 4) = static_cast<uint16_t>(at<uint16_t>(head + kZ[corner]) - 10);
        PS2C_SetGteMatrix(ptr32<_sMat>(matrix));
        PS2C_GteMatrixMulVec16(ptr32<_sVec16>(offset));
        PS2C_GetVec16GteResult(ptr32<_sVec16>(offset));
        const uint32_t size = at<uint8_t>(m + 0x44 + corner);
        const ee_float height(at<float>(at<uint32_t>(car + 0x20) + corner * 0x48 + 0xec));
        const uint16_t x = static_cast<uint16_t>(((at<int16_t>(offset) * static_cast<int32_t>(size)) >> 5) + position[0]);
        at<uint16_t>(offset + 2) = static_cast<uint16_t>(at<uint16_t>(offset + 2) + position[1]);
        const uint16_t z = static_cast<uint16_t>(((at<int16_t>(offset + 4) * static_cast<int32_t>(size)) >> 5) + position[2]);
        at<uint16_t>(offset + 4) = z;
        at<uint16_t>(offset) = x;
        if (!(ee_float(-32768.0f) <= height))
        {
            missing = true;
            break;
        }
        at<uint16_t>(sv + corner * 8) = x;
        at<int16_t>(sv + corner * 8 + 2) = static_cast<int16_t>(ee_ftoi(height));
        at<uint16_t>(sv + corner * 8 + 4) = z;
    }
    if (at<uint32_t>(car + 0x1d8) == 0)
        missing = true;
    if (missing)
        return;
    const int32_t fade = 100 - (at<int32_t>(at<uint32_t>(at<uint32_t>(car + 0x20) + 0x1e8) + 0x24) >> 8);
    if (fade < 2)
        return;
    const uint32_t colour = static_cast<uint32_t>(fade + (fade << 8) + (fade << 16));
    VfxPutGT4vecs(ptr32<SVECTOR>(sv + 0x10), ptr32<SVECTOR>(sv + 0x18), ptr32<SVECTOR>(sv), ptr32<SVECTOR>(sv + 8));
    VfxPutGT4rgba2(colour);
    VfxPutGT4rgba3(colour);
    VfxPutGT4rgba0(colour);
    VfxPutGT4rgba1(colour);
    at<uint32_t>(G(visFxVarG).pGT4.address + 0x70) = 0;
    RdrPolyTexture(0, ptr32<_sGfxTexture>(at<uint32_t>(car + 0x34)), 2);
    RdrPoly(0, ptr32<_sRdrPoly>(G(visFxVarG).pGT4.address));
}

void VfxSetupCameraMatrix();                 // visfx6.cpp
void RdrZedsWriteOff(int32_t channel);       // rdr.cpp
void RdrZedsWriteOn(int32_t channel);
void RdrPolyFlush(int32_t channel);
void RdrState_ApplyDefaultStateBlock();

// The cars' shadows (not the boats'): for each car that is drawn and within 4500 units of the camera, a dark quad on the
// ground under it: the corners of its shape's bounds (shorts at +0x68..+0x72 of the shape header) widened by the car
// type's shadow allowance (visFxVarG.shadBodgeG), turned by the car's matrix and moved to its position, each at its
// wheel's shadow height plus 3 (floats 0x48 bytes apart from +0xec of the physics). A corner with no ground under it, or a
// car whose cast down failed, has no shadow. Drawn in polygon channel 2 without depth writes.
void VfxRenderShadows()
{
    GuestFrame frame(0x120);
    VfxSetupCameraMatrix();
    const uint32_t offset = frame.local<uint8>(0x10).address, sv = frame.local<uint8>(0x20).address;
    constexpr uint32_t kObjectVar = 0x1ee390; // objectVarG: the list, then noObjects at +0x170
    for (uint32_t i = 0; i < at<uint32_t>(kObjectVar + 0x170); ++i)
    {
        const uint32_t car = at<uint32_t>(kObjectVar + 4 * i);
        const uint32_t type = at<uint32_t>(at<uint32_t>(car + 0xc) + 4);
        if (!(type < 0x14))
            continue;
        RdrZedsWriteOff(2);
        if (at<int16_t>(car + 0x172) != 0)
            continue;
        const uint32_t integer = at<uint32_t>(at<uint32_t>(car + 0x20) + 0x1e8);
        int16_t position[3];
        int32_t d[3];
        for (uint32_t axis = 0; axis < 3; ++axis)
        {
            position[axis] = static_cast<int16_t>(at<int32_t>(integer + 4 + 4 * axis) >> 8);
            at<int16_t>(0x3280f8u + 2 * axis) = position[axis]; // svVelTemp
            const int32_t camera = at<int16_t>(kVisFx + 0x108 + 2 * axis);
            d[axis] = position[axis] - camera > 0 ? position[axis] - camera : camera - position[axis];
        }
        int32_t distance = d[0] < d[1] ? d[1] + (d[0] >> 1) : d[0] + (d[1] >> 1);
        distance = distance < d[2] ? d[2] + (distance >> 1) : distance + (d[2] >> 1);
        if (!(distance < 0x1194))
            continue;
        bool missing = false;
        const uint32_t bodge = kVisFx + 0x2734 + (type - 1) * 4; // shadBodgeG[type - 1]: x, then z
        const int32_t wideX = at<int16_t>(bodge), wideZ = at<int16_t>(bodge + 2);
        const uint32_t head = at<uint32_t>(at<uint32_t>(car + 0x18));
        const uint32_t matrix = at<uint32_t>(at<uint32_t>(car + 0x20) + 0x1e8) + 0x28;
        for (uint32_t corner = 0; corner < 4; ++corner)
        {
            at<uint16_t>(offset + 2) = 0;
            const bool right = corner == 1 || corner == 3, front = corner < 2;
            at<uint16_t>(offset) = static_cast<uint16_t>(right ? at<uint16_t>(head + 0x6e) + wideX : at<uint16_t>(head + 0x68) - wideX);
            at<uint16_t>(offset + 4) = static_cast<uint16_t>(front ? at<uint16_t>(head + 0x72) + wideZ : at<uint16_t>(head + 0x6c) - wideZ);
            PS2C_SetGteMatrix(ptr32<_sMat>(matrix));
            PS2C_GteMatrixMulVec16(ptr32<_sVec16>(offset));
            PS2C_GetVec16GteResult(ptr32<_sVec16>(offset));
            const uint16_t x = static_cast<uint16_t>(at<uint16_t>(offset) + position[0]);
            const uint16_t y = static_cast<uint16_t>(at<uint16_t>(offset + 2) + position[1]);
            const uint16_t z = static_cast<uint16_t>(at<uint16_t>(offset + 4) + position[2]);
            const ee_float height(at<float>(at<uint32_t>(car + 0x20) + corner * 0x48 + 0xec));
            at<uint16_t>(offset) = x;
            at<uint16_t>(offset + 2) = y;
            at<uint16_t>(offset + 4) = z;
            if (ee_float(-32768.0f) < height)
            {
                at<uint16_t>(sv + corner * 8) = x;
                at<int16_t>(sv + corner * 8 + 2) = static_cast<int16_t>(ee_ftoi(height + ee_float(3.0f)));
                at<uint16_t>(sv + corner * 8 + 4) = z;
            }
            else
                missing = true;
        }
        if (at<uint32_t>(car + 0x1d8) == 0)
            missing = true;
        if (missing)
            continue;
        RdrPolyTexture(2, ptr32<_sGfxTexture>(at<uint32_t>(car + 0x34)), 4);
        const uint32_t quad = G(visFxVarG).pGT4.address;
        static constexpr uint8_t kCorner[4] = {1, 3, 0, 2};
        for (uint32_t v = 0; v < 4; ++v)
            for (uint32_t axis = 0; axis < 3; ++axis)
                at<float>(quad + 0x10 * v + 4 * axis) = ee_itof(at<int16_t>(sv + kCorner[v] * 8 + 2 * axis));
        for (uint32_t c = 0; c < 4; ++c)
            at<uint32_t>(quad + 0x60 + 4 * c) = 0x242424;
        at<float>(quad + 0x70) = -10.0f;
        RdrPoly(2, ptr32<_sRdrPoly>(quad));
    }
    RdrZedsWriteOn(2);
    RdrPolyFlush(2);
    RdrState_ApplyDefaultStateBlock();
}

} // namespace rc

// Forward declarations of the random-input setups (visfx_fuzz.cpp).
namespace rc::fuzz {
void vfxCarWaterBallRenderer(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxTakeTerrorRenderer(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxBombRenderer(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxAerialLightning(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxBoatWake(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxShadowFoam(R5900Context *ctx, uint64_t &state, uint32_t scratch);
}

RC_DECOMP_FN_SETUP(VfxRenderer_CarWaterBall, 0x15c930, rc::fuzz::vfxCarWaterBallRenderer)
RC_DECOMP_FN_SETUP(VfxRenderer_TakeTerror, 0x160950, rc::fuzz::vfxTakeTerrorRenderer)
RC_DECOMP_FN_SETUP(VfxRenderer_Bomb, 0x166a28, rc::fuzz::vfxBombRenderer)
RC_DECOMP_FN_SETUP(VfxRenderer_AerialLightning, 0x15f210, rc::fuzz::vfxAerialLightning)
RC_DECOMP_FN_SETUP(VfxRenderer_DamageLightning, 0x15f868, rc::fuzz::vfxAerialLightning)
RC_DECOMP_FN_SETUP(VfxRenderer_BoatWake, 0x15dc20, rc::fuzz::vfxBoatWake)
RC_DECOMP_FN_SETUP(VfxRenderer_ShadowFoam, 0x15e040, rc::fuzz::vfxShadowFoam)
// Draws every car's shadow from the real object list and takes no arguments: compared on the game's own calls.
RC_DECOMP_FN_NOFUZZ(VfxRenderShadows, 0x1697e8)
