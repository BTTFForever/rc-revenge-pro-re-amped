// Decompiled from VISFX.c, VISFX2.c and VISFX3.c, continued (seventh batch): scenery handlers (pirate lava, lava spit,
// gulls, butterflies, light glow and flicker), car effect handlers and renderers (change-over, water ball, water
// covered, oil slick, rocket car, take terror, bomb, aerial and damage lightning, boat wake, shadow foam), the start
// lights and the car shadows. See visfx2.cpp for the scenery effect layout and visfx5.cpp for the renderers' conventions.

#include "ai_fuzz.h"
#include "ee_math.h"
#include "vifpk.h"
#include "visfx.h"

#include <emmintrin.h>

namespace rc {

void VfxSortPos(ptr32<SVECTOR> sv, ptr32<_sVfx_Object> fx); // visfx.cpp

namespace {

using vfx::at;
using vfx::ringHead;
using vfx::ringNext;
using vfx::sortAtScenery;

constexpr uint32_t kRing = 0x40;      // the ring header: mask, count, head, tail
constexpr uint32_t kParticles = 0x44; // the first particle
constexpr uint32_t kVisFx = sym::visFxVarG; // visFxVarG

constexpr uint32_t kAtan2f = sym::atan2f, kUnitMatrix = sym::sceVu0UnitMatrix, kRotMatrix = sym::sceVu0RotMatrix;

// The ring's bytes as these handlers read them: all with lbu, so the indexes are unsigned.
inline uint8_t &ringMask(uint32_t m) { return at<uint8_t>(m + kRing); }
inline uint8_t &ringCount(uint32_t m) { return at<uint8_t>(m + kRing + 1); }
inline uint8_t &ringHeadU(uint32_t m) { return at<uint8_t>(m + kRing + 2); }
inline uint8_t &ringTail(uint32_t m) { return at<uint8_t>(m + kRing + 3); }

inline void pushTail(uint32_t m)
{
    ringTail(m) = static_cast<uint8_t>(ringMask(m) & (ringTail(m) + 1));
    ringCount(m) = static_cast<uint8_t>(ringCount(m) + 1);
}

// Drops the particle at `index`: the head particle moves into its place and the head moves on.
inline void dropParticle(uint32_t m, uint32_t index, uint32_t stride)
{
    const uint32_t head = ringHeadU(m);
    if (index != head)
        std::memmove(&at<uint8_t>(m + kParticles + index * stride), &at<uint8_t>(m + kParticles + head * stride), stride);
    ringCount(m) = static_cast<uint8_t>(ringCount(m) - 1);
    ringHeadU(m) = static_cast<uint8_t>(ringMask(m) & (ringHeadU(m) + 1));
}

// The distance from the camera (visFxVarG.svCamPos, three shorts at +0x108) to a scenery instance, as the lights measure
// it (the same as visfx7.cpp's): the largest of the three whole-unit differences plus half of each of the others, roughly.
int32_t sceneryDistance(uint32_t tob)
{
    int16_t diff[3];
    for (uint32_t axis = 0; axis < 3; ++axis)
    {
        const int32_t where = at<int32_t>(tob + 4 + 4 * axis) >> 8;
        const int32_t camera = at<int16_t>(kVisFx + 0x108 + 2 * axis);
        diff[axis] = static_cast<int16_t>(where - camera <= 0 ? camera - where : where - camera);
    }
    int32_t d = diff[0] < diff[1] ? diff[1] + (diff[0] >> 1) : diff[0] + (diff[1] >> 1);
    d = d < diff[2] ? diff[2] + (d >> 1) : d + (diff[2] >> 1);
    return d;
}

int32_t atLeastZero(int32_t value) { return value > -1 ? value : 0; }
int32_t clampByte(int32_t value) { return value < 0x100 ? value : 0xff; }

// The word at +0x4c of a light as an unsigned number, converted to float as the compiler does it.
ee_float unsignedToFloat(uint32_t count)
{
    if (static_cast<int32_t>(count) >= 0)
        return ee_itof(static_cast<int32_t>(count));
    const ee_float half = ee_itof(static_cast<int32_t>((count >> 1) | (count & 1)));
    return half + half;
}

// The common part of the glow and flicker lights: the colour bytes at +0x48 (a zero byte stays zero, the others get
// `boost`), less the distance fade, kept in 0..255; the flare texture, no flip.
uint32_t lightColour(uint32_t m, int32_t distance, int32_t boost)
{
    int32_t c[3];
    for (uint32_t i = 0; i < 3; ++i)
        c[i] = at<uint8_t>(m + 0x48 + i) != 0 ? at<uint8_t>(m + 0x48 + i) + boost : 0;
    const int32_t fade = atLeastZero((0x400 - distance) >> 4) + atLeastZero((distance - 0x1b58) >> 3);
    for (int32_t &v : c)
        v = clampByte(atLeastZero(v - fade));
    return static_cast<uint32_t>(c[0] + (c[1] << 8) + (c[2] << 16));
}

inline void spritePosAtScenery(uint32_t tob)
{
    const uint32_t s = vfx::sprite();
    at<float>(s + vfx::spr::kPos + 8) = ee_itof(at<int32_t>(tob + 0xc) >> 8);
    at<float>(s + vfx::spr::kPos) = ee_itof(at<int32_t>(tob + 4) >> 8);
    at<float>(s + vfx::spr::kPos + 4) = ee_itof(at<int32_t>(tob + 8) >> 8);
}

inline void add16(uint32_t address, int32_t value) { at<uint16_t>(address) = static_cast<uint16_t>(at<uint16_t>(address) + value); }
inline uint16_t rnd16(uint32_t mask, int32_t offset) { return static_cast<uint16_t>((vfx::random15() & static_cast<int32_t>(mask)) + offset); }

} // namespace

// ---------------------------------------------------------------------------------------------------------------------
// Scenery handlers

// The pirate ship's lava: every 16th frame (a counter at +0x644) a new blob of 0x18 bytes (position, velocity from +8, a
// spin angle at +0x10 and its speed at +0x12, a life at +0x14, a size byte at +0x16) leaves the scenery position upwards
// with a random spread, smaller than the size at +0x64b by up to 63. Blobs fall (two units a frame), and a big one (size
// above 100) now and then throws off a smaller one. A blob dies when its life runs out or it has fallen far below.
void Vfx2Handler_PirateLava(ptr32<_sVfx_TSFX> mem)
{
    const uint32_t m = mem.address;
    const uint16_t counter = static_cast<uint16_t>(at<uint16_t>(m + 0x644) + 1);
    at<uint16_t>(m + 0x644) = counter;
    if ((counter & 0xf) == 0 && ringCount(m) < 0x3f)
    {
        const uint32_t p = m + kParticles + ringTail(m) * 0x18u;
        const uint16_t x = rnd16(0x7ff, -0x400);
        at<uint16_t>(p + 2) = 0;
        at<uint16_t>(p) = x;
        at<uint16_t>(p + 4) = rnd16(0x7ff, -0x400);
        at<uint16_t>(p + 8) = rnd16(0xff, -0x80);
        at<uint16_t>(p + 0xa) = rnd16(0x7f, 0x5a);
        at<uint16_t>(p + 0xc) = rnd16(0xff, -0x80);
        at<uint16_t>(p + 0x14) = rnd16(0x3f, 0x7f);
        at<uint16_t>(p + 0x10) = static_cast<uint16_t>(vfx::random15());
        at<uint16_t>(p + 0x12) = rnd16(0x3ff, -0x200);
        at<uint8_t>(p + 0x16) = static_cast<uint8_t>(at<uint8_t>(m + 0x64b) - (vfx::random15() & 0x3f));
        pushTail(m);
    }
    uint32_t index = ringHeadU(m);
    while (index != ringTail(m))
    {
        const uint32_t p = m + kParticles + index * 0x18;
        add16(p, at<uint16_t>(p + 8));
        add16(p + 2, at<uint16_t>(p + 0xa));
        add16(p + 4, at<uint16_t>(p + 0xc));
        add16(p + 0xa, -2);
        add16(p + 0x10, at<uint16_t>(p + 0x12));
        add16(p + 0x14, -1);
        if ((vfx::random15() & 0xf) == 0 && ringCount(m) < 0x3f && at<uint8_t>(p + 0x16) > 100)
        {
            const uint32_t c = m + kParticles + ringTail(m) * 0x18u;
            at<uint16_t>(c) = at<uint16_t>(p);
            at<uint16_t>(c + 2) = at<uint16_t>(p + 2);
            at<uint16_t>(c + 4) = at<uint16_t>(p + 4);
            at<uint16_t>(c + 8) = rnd16(0x1ff, -0x100);
            at<uint16_t>(c + 0xa) = rnd16(0x1ff, -0x100);
            const uint16_t vz = rnd16(0x1ff, -0x100);
            at<uint16_t>(c + 0x14) = 0x18;
            at<uint16_t>(c + 0xc) = vz;
            at<uint16_t>(c + 0x10) = static_cast<uint16_t>(vfx::random15());
            at<uint8_t>(c + 0x16) = static_cast<uint8_t>((vfx::random15() & 0x3f) + 0x20);
            at<uint16_t>(c + 0x12) = rnd16(0x3ff, -0x200);
            pushTail(m);
        }
        if (at<uint16_t>(p + 0x14) == 0 || at<int16_t>(p + 2) < -30000)
            dropParticle(m, index, 0x18);
        index = (index + 1) & ringMask(m);
    }
    sortAtScenery(m, 0xa0);
}

// The volcano's lava spit: as the pirate lava (counter at +0x344, at most 31 blobs, the size byte copied from +0x34b),
// falling one unit a frame. Now and then, on every eighth frame, a blob splits: both halves get new small random velocities,
// the new one lives 32 frames longer, and both take the old size scaled by a constant.
void Vfx2Handler_LavaSpit(ptr32<_sVfx_TSFX> mem)
{
    const uint32_t m = mem.address;
    const uint16_t counter = static_cast<uint16_t>(at<uint16_t>(m + 0x344) + 1);
    at<uint16_t>(m + 0x344) = counter;
    if ((counter & 0xf) == 0 && ringCount(m) < 0x1f)
    {
        const uint32_t p = m + kParticles + ringTail(m) * 0x18u;
        const uint16_t x = rnd16(0x3ff, -0x200);
        at<uint16_t>(p + 2) = 0;
        at<uint16_t>(p) = x;
        at<uint16_t>(p + 4) = rnd16(0x3ff, -0x200);
        at<uint16_t>(p + 8) = rnd16(0xff, -0x80);
        at<uint16_t>(p + 0xa) = rnd16(0x7f, 0x50);
        at<uint16_t>(p + 0xc) = rnd16(0xff, -0x80);
        at<uint16_t>(p + 0x14) = rnd16(0x3f, 0x7f);
        at<uint16_t>(p + 0x10) = static_cast<uint16_t>(vfx::random15());
        at<uint16_t>(p + 0x12) = rnd16(0x3ff, -0x200);
        at<uint8_t>(p + 0x16) = at<uint8_t>(m + 0x34b);
        pushTail(m);
    }
    uint32_t index = ringHeadU(m);
    while (index != ringTail(m))
    {
        const uint32_t p = m + kParticles + index * 0x18;
        add16(p, at<uint16_t>(p + 8));
        add16(p + 2, at<uint16_t>(p + 0xa));
        add16(p + 4, at<uint16_t>(p + 0xc));
        add16(p + 0xa, -1);
        add16(p + 0x10, at<uint16_t>(p + 0x12));
        add16(p + 0x14, -1);
        if ((vfx::random15() & 0xf) == 0 && (at<uint16_t>(m + 0x344) & 7) == 0 && ringCount(m) < 0x1f)
        {
            const uint32_t c = m + kParticles + ringTail(m) * 0x18u;
            at<uint16_t>(c) = at<uint16_t>(p);
            at<uint16_t>(c + 2) = at<uint16_t>(p + 2);
            at<uint16_t>(c + 4) = at<uint16_t>(p + 4);
            at<uint16_t>(c + 8) = rnd16(0x3f, -0x20);
            at<uint16_t>(c + 0xa) = rnd16(0x3f, -0x20);
            at<uint16_t>(c + 0xc) = rnd16(0x3f, -0x20);
            at<uint16_t>(p + 8) = rnd16(0x3f, -0x20);
            at<uint16_t>(p + 0xa) = rnd16(0x3f, -0x20);
            const uint16_t vz = rnd16(0x3f, -0x20);
            at<uint16_t>(c + 0x14) = static_cast<uint16_t>(at<uint16_t>(p + 0x14) + 0x20);
            at<uint16_t>(p + 0xc) = vz;
            at<uint16_t>(c + 0x10) = static_cast<uint16_t>(vfx::random15());
            const uint8_t size = static_cast<uint8_t>(ee_ftoi(ee_itof(at<uint8_t>(p + 0x16)) * ee_float(at<float>(sym::lit4_3278f0))));
            at<uint8_t>(p + 0x16) = size;
            at<uint8_t>(c + 0x16) = size;
            at<uint16_t>(c + 0x12) = rnd16(0x3ff, -0x200);
            pushTail(m);
        }
        if (at<uint16_t>(m + kParticles + index * 0x18 + 0x14) == 0)
            dropParticle(m, index, 0x18);
        index = (index + 1) & ringMask(m);
    }
    sortAtScenery(m, 0xa0);
}

// The gulls: on the first call (the halfword at +0x184 still zero) fifteen gulls of 0x14 bytes start at random places
// round the scenery with no velocity, a random heading and a life near the maximum, and the ring is full. Each frame they
// move, and each velocity lane is pulled back towards the centre by one.
void Vfx2Handler_Gulls(ptr32<_sVfx_TSFX> mem)
{
    const uint32_t m = mem.address;
    if (at<int16_t>(m + 0x184) == 0)
    {
        at<uint16_t>(m + 0x184) = 1;
        for (uint32_t p = m + kParticles; p < m + 0x170; p += 0x14) // fifteen
        {
            at<uint16_t>(p) = rnd16(0x1fff, -0x1000);
            at<uint16_t>(p + 2) = rnd16(0xfff, -0x800);
            const uint16_t z = rnd16(0x1fff, -0x1000);
            at<uint16_t>(p + 8) = 0;
            at<uint16_t>(p + 0xa) = 0;
            at<uint16_t>(p + 0xc) = 0;
            at<uint16_t>(p + 4) = z;
            at<uint16_t>(p + 0x12) = static_cast<uint16_t>(vfx::random15());
            at<uint16_t>(p + 0x10) = rnd16(0xfff, -0x15a0);
        }
        ringCount(m) = 0xf;
        ringTail(m) = 0xf;
    }
    VfxSimpleParticleMover(ptr32<_sVfx_PartHeader>(m + kRing), ptr32<_VFX_SIMPLEPARTICLE>(m + kParticles));
    for (int32_t index = ringHead(m); !vfx::ringAtTail(m, index); index = ringNext(m, index))
    {
        const uint32_t p = m + kParticles + static_cast<uint32_t>(index * 0x14);
        for (uint32_t axis = 0; axis < 3; ++axis)
            add16(p + 8 + 2 * axis, at<int16_t>(p + 2 * axis) < 0 ? 1 : -1);
    }
}

// The butterflies (an `_sVfx_Object` with sixteen 0x70-byte particles from +0x50: position, velocity from +8, a flap phase
// at +0x10, a timer at +0x12, a 4x4 matrix at +0x20 facing the way it flies, two colours at +0x60 and +0x64). On the first
// call (life 1) fifteen of them start at random. A butterfly whose timer is zero flies (rising and falling at random, its
// flap phase running down) until it sinks below the centre, where it rests for 16 to 527 frames; at a timer of one it
// takes a new random velocity, turned back inwards if it is more than 4000 units out, and faces it.
void Vfx2Handler_Butterflies(ptr32<_sVfx_Object> mem)
{
    GuestFrame frame(0xd0);
    const uint32_t m = mem.address;
    const ptr32<float> rot = frame.local<float>(0x10);
    const auto face = [&](uint32_t matrix, int16_t vx, int16_t vz)
    {
        rot[0] = 0.0f;
        const ee_float angle = guest_call<ee_float>(kAtan2f, ee_itof(vx), ee_itof(vz));
        rot[1] = angle;
        rot[2] = 0.0f;
        guest_call(kUnitMatrix, ptr32<float>(matrix));
        guest_call(kRotMatrix, ptr32<float>(matrix), ptr32<float>(matrix), rot);
    };
    if (at<uint16_t>(m + 0x10) == 1)
    {
        at<uint16_t>(m + 0x10) = 2;
        for (uint32_t i = 0; i < 15; ++i)
        {
            const uint32_t p = m + 0x50 + i * 0x70;
            const uint32_t tail = ringTail(m);
            at<uint16_t>(p) = rnd16(0x1001, -0x800);
            at<uint16_t>(p + 2) = rnd16(0x1001, -0x800);
            at<uint16_t>(p + 4) = rnd16(0x1001, -0x800);
            at<uint16_t>(p + 8) = rnd16(0x1f, -0x10);
            at<uint16_t>(p + 0xa) = rnd16(0x3f, 8);
            at<uint16_t>(p + 0xc) = rnd16(0x1f, -0x10);
            const int32_t phase = vfx::random15();
            at<uint16_t>(p + 0x12) = 0;
            at<uint16_t>(p + 0x10) = static_cast<uint16_t>(~(phase & 0x3ff));
            // It faces the way the ring's tail particle flies, not its own (as the original).
            const uint32_t t = m + 0x50 + tail * 0x70;
            face(p + 0x20, at<int16_t>(t + 8), at<int16_t>(t + 0xc));
            const int32_t r1 = vfx::random15(), r2 = vfx::random15(), r3 = vfx::random15();
            at<uint32_t>(p + 0x60) = static_cast<uint32_t>((r1 & 0x7f) + (((r2 & 0x7f) + 0x7f) << 16) + (((r3 & 0x7f) + 0x7f) << 8) + 0x7f);
            const int32_t s1 = rand(), s2 = rand(), s3 = rand();
            at<uint32_t>(p + 0x64) = static_cast<uint32_t>(((s1 >> 15) & 0x3f) + ((s2 >> 7) & 0x3f00) + (((s3 >> 15) & 0x3f) << 16));
        }
        ringCount(m) = 0xf;
        ringTail(m) = 0xf;
    }
    for (uint32_t index = ringHeadU(m); index != ringTail(m); index = (index + 1) & ringMask(m))
    {
        const uint32_t p = m + 0x50 + index * 0x70;
        const uint16_t timer = at<uint16_t>(p + 0x12);
        if (timer == 0)
        {
            if (at<int16_t>(p + 2) < 0)
            {
                at<uint16_t>(p + 2) = 0;
                at<uint16_t>(p + 0x12) = rnd16(0x1ff, 0x10);
            }
            else
            {
                const int32_t lift = vfx::random15() & 0x40;
                const uint16_t vy = at<uint16_t>(p + 0xa);
                at<uint16_t>(p + 2) = static_cast<uint16_t>(at<uint16_t>(p + 2) - 0x19 + lift + vy);
                add16(p, at<uint16_t>(p + 8));
                add16(p + 4, at<uint16_t>(p + 0xc));
                at<uint16_t>(p + 0xa) = static_cast<uint16_t>(vy - 1);
                add16(p + 0x10, -((vfx::random15() & 1) + 3));
            }
        }
        else if (timer == 1)
        {
            at<uint16_t>(p + 8) = rnd16(0x1f, -0x10);
            at<uint16_t>(p + 0xa) = rnd16(0x3f, 8);
            at<uint16_t>(p + 0xc) = rnd16(0x1f, -0x10);
            const auto magnitude = [](int16_t v) { return static_cast<int16_t>(v < 0 ? -v : v); };
            const int16_t x = at<int16_t>(p);
            if (x > 4000)
                at<int16_t>(p + 8) = static_cast<int16_t>(-magnitude(at<int16_t>(p + 8)));
            if (x < -4000)
                at<int16_t>(p + 8) = magnitude(at<int16_t>(p + 8));
            const int16_t z = at<int16_t>(p + 4);
            if (z > 4000)
                at<int16_t>(p + 0xc) = static_cast<int16_t>(-magnitude(at<int16_t>(p + 0xc)));
            if (z < -4000)
                at<int16_t>(p + 0xc) = magnitude(at<int16_t>(p + 0xc));
            face(p + 0x20, at<int16_t>(p + 8), at<int16_t>(p + 0xc));
            add16(p + 0x12, -1);
        }
        else
        {
            add16(p + 0x10, -(vfx::random15() & 1));
            add16(p + 0x12, -1);
        }
    }
    VfxSortPos(ptr32<SVECTOR>(m + 0x750), mem);
}

// A glowing light (a street lamp's halo): drawn while the camera is within 9000 units, pulsing with the sine of a phase
// byte at +0x4b that steps each frame (adding up to +-8 to each colour byte that is not zero), dimmed close up and far
// away, and sized by the count at +0x4c times the distance.
void Vfx2Renderer_LightGlow(ptr32<_sVfx_TSFX> mem)
{
    GuestFrame frame(0x50);
    const uint32_t m = mem.address;
    const uint32_t tob = at<uint32_t>(m + 0x10);
    const int32_t distance = sceneryDistance(tob);
    if (!(static_cast<uint32_t>(distance - 1) < 0x2327u))
        return;
    spritePosAtScenery(tob);
    const uint8_t phase = at<uint8_t>(m + 0x4b);
    const ee_float turn = ee_itof(static_cast<int32_t>((phase << 7) & 0xf80));
    at<uint8_t>(m + 0x4b) = static_cast<uint8_t>(phase + 1);
    const ee_float sine = ee_sinf((turn + turn) * ee_float(at<float>(sym::lit4_32789c)));
    const int32_t pulse = ee_ftoi(sine * ee_float(4096.0f)) >> 9;
    const uint32_t rgb = lightColour(m, distance, pulse);
    const uint32_t s = vfx::sprite();
    at<uint8_t>(s + vfx::spr::kCode) = 2;
    at<uint32_t>(s + vfx::spr::kTexture) = vfx::vfxTexture(1);
    at<uint8_t>(s + vfx::spr::kFlipX) = 0;
    at<uint32_t>(s + vfx::spr::kRgb) = rgb;
    at<float>(s + vfx::spr::kScale) = (unsignedToFloat(at<uint32_t>(m + 0x4c)) * ee_itof(distance)) * ee_float(at<float>(sym::lit4_3278a0));
    VfxPS2SpriteRotated3D(ee_float(-100.0f));
}

// A flickering light (a fire or a faulty lamp): as the glow, but each colour byte that is not zero gets a random 0 to 31
// each frame, and the sprite spins at random.
void Vfx2Renderer_LightFlicker(ptr32<_sVfx_TSFX> mem)
{
    GuestFrame frame(0x80);
    const uint32_t m = mem.address;
    const uint32_t tob = at<uint32_t>(m + 0x10);
    const int32_t distance = sceneryDistance(tob);
    if (!(static_cast<uint32_t>(distance - 1) < 0x2327u))
        return;
    spritePosAtScenery(tob);
    const int32_t flicker = vfx::random15() & 0x1f;
    const uint32_t rgb = lightColour(m, distance, flicker);
    const uint32_t s = vfx::sprite();
    at<uint8_t>(s + vfx::spr::kCode) = 2;
    at<uint32_t>(s + vfx::spr::kTexture) = vfx::vfxTexture(1);
    at<uint8_t>(s + vfx::spr::kFlipX) = 0;
    at<uint16_t>(vfx::sprite() + vfx::spr::kAngle) = static_cast<uint16_t>(vfx::random15());
    at<uint32_t>(s + vfx::spr::kRgb) = rgb;
    at<float>(s + vfx::spr::kScale) = (unsignedToFloat(at<uint32_t>(m + 0x4c)) * ee_itof(distance)) * ee_float(at<float>(sym::lit4_3278a4));
    VfxPS2SpriteRotated3D(ee_float(-100.0f));
}

// ---------------------------------------------------------------------------------------------------------------------
// Car effects

bool_ AColDownCastInt(ptr32<SVECTOR> pPosI, ptr32<SVECTOR> pPoiI, ptr32<SVECTOR> pNormalI, ptr32<uint8> pMatId); // collision.cpp

// The oil slick a car drops: on its first frame (life 200) 63 casts down from random points round the drop point (shorts at
// +0x644, 20 units up) place a blob (0x18 bytes from +0x44: the ground point and normal as two SVECTORs, a spin at +0x10,
// a colour pair at +0x12, a life of 128 to 191 at +0x14, a size at +0x16 and a shade at +0x17) wherever they hit. Each
// frame the blobs age, and one whose life reaches zero is dropped (a life already zero stays).
void VfxHandler_OilSlick(ptr32<_sVfx_Object> mem)
{
    const uint32_t m = mem.address;
    {
        GuestFrame frame(0x100);
        if (at<uint16_t>(m + 0x10) == 200)
        {
            const ptr32<SVECTOR> from = frame.local<SVECTOR>(0), hit = frame.local<SVECTOR>(0x10), normal = frame.local<SVECTOR>(0x20);
            for (int32_t i = 0; i < 63; ++i)
            {
                const int32_t rx = vfx::random15();
                at<uint16_t>(from.address) = static_cast<uint16_t>(at<uint16_t>(m + 0x644) + (rx & 0xff) - 0x80);
                at<uint16_t>(from.address + 2) = static_cast<uint16_t>(at<uint16_t>(m + 0x646) + 0x14);
                const int32_t rz = vfx::random15();
                const uint32_t tail = ringTail(m);
                at<uint16_t>(from.address + 4) = static_cast<uint16_t>(at<uint16_t>(m + 0x648) + (rz & 0xff) - 0x80);
                if (!AColDownCastInt(from, hit, normal, frame.local<uint8>(0x30)))
                    continue;
                const uint32_t p = m + kParticles + tail * 0x18;
                std::memcpy(&at<uint8_t>(p), &at<uint8_t>(hit.address), 8);
                std::memcpy(&at<uint8_t>(p + 8), &at<uint8_t>(normal.address), 8);
                at<uint16_t>(p + 0x10) = static_cast<uint16_t>(vfx::random15());
                at<uint16_t>(p + 0x14) = rnd16(0x3f, 0x80);
                at<uint8_t>(p + 0x16) = static_cast<uint8_t>((vfx::random15() & 0x1f) + 0x10);
                const int32_t c1 = rand(), c2 = rand();
                at<uint16_t>(p + 0x12) = static_cast<uint16_t>(((c1 >> 15) & 0x3f) + ((c2 >> 7) & 0x3f00));
                at<uint8_t>(p + 0x17) = static_cast<uint8_t>(vfx::random15() & 0x3f);
                pushTail(m);
            }
        }
        for (uint32_t index = ringHeadU(m); index != ringTail(m); index = (index + 1) & ringMask(m))
        {
            const uint32_t p = m + kParticles + index * 0x18;
            const uint16_t life = at<uint16_t>(p + 0x14);
            if (life == 0)
                continue;
            at<uint16_t>(p + 0x14) = static_cast<uint16_t>(life - 1);
            if (life == 1)
                dropParticle(m, index, 0x18);
        }
        at<uint16_t>(m + 0x10) = static_cast<uint16_t>(at<uint16_t>(m + 0x10) - 1);
    }
    vfx::sortPosFromInts(m, at<int16_t>(m + 0x644), at<int16_t>(m + 0x646), at<int16_t>(m + 0x648), 0x100, 0x40);
}

uint32 VecNormalise(ptr32<SVECTOR> pOut, ptr32<SVECTOR> pVec); // ai.cpp
bool_ AColDownCast(ptr32<_sColCellVecPSX> pPos, ptr32<_sColCellVecPSX> pPoi, ptr32<_sColNormalPSX> pNormal, ptr32<uint8> pMatId); // collision.cpp

namespace {
constexpr uint32_t kObjectCarChangeOver = sym::ObjectCarChangeOver;

// A spark flying out in a random direction: a random vector normalised and cut to a sixteenth (as its position and its
// velocity), with a life of 40.
void changeOverSpark(uint32_t p)
{
    at<uint16_t>(p) = rnd16(0xff, -0x80);
    at<uint16_t>(p + 2) = rnd16(0xff, -0x80);
    at<uint16_t>(p + 4) = rnd16(0xff, -0x80);
    VecNormalise(ptr32<SVECTOR>(p), ptr32<SVECTOR>(p));
    for (uint32_t axis = 0; axis < 3; ++axis)
    {
        const uint16_t v = static_cast<uint16_t>(at<int16_t>(p + 2 * axis) >> 4);
        at<uint16_t>(p + 8 + 2 * axis) = v;
        at<uint16_t>(p + 2 * axis) = v;
    }
    at<uint16_t>(p + 0x10) = 0x28;
}
} // namespace

// The car turning into a boat (or back): the car is at +0x2c4 and what it turns into at +0x2c8. On the first frame (life
// 80) 31 sparks burst out of the car and a spin (+0x2cc, speed +0x2ce from 200) starts; the sparks move and the spin
// slows each frame. Fifteen frames in, the car really changes (ObjectCarChangeOver) and the sparks turn back at half
// speed; at the end the car's change-over flag (+0x176) is cleared. Sorted at the car's position.
void VfxHandler_CarChangeOver(ptr32<_sVfx_Object> mem)
{
    const uint32_t m = mem.address;
    const uint32_t car = at<uint32_t>(m + 0x2c4);
    {
        GuestFrame frame(0xb0);
        if (at<uint16_t>(m + 0x10) == 0x50)
        {
            const int32_t spin = vfx::random15();
            at<uint16_t>(m + 0x2ce) = 200;
            at<uint16_t>(m + 0x2cc) = static_cast<uint16_t>(spin & 0xfff);
            for (uint32_t p = m + kParticles; p < m + 0x2b0; p += 0x14) // 31 sparks
                changeOverSpark(p);
            ringCount(m) = 0x1f;
            ringTail(m) = 0x1f;
        }
        else
        {
            VfxSimpleParticleMover(ptr32<_sVfx_PartHeader>(m + kRing), ptr32<_VFX_SIMPLEPARTICLE>(m + kParticles));
            const uint16_t speed = at<uint16_t>(m + 0x2ce);
            at<uint16_t>(m + 0x2cc) = static_cast<uint16_t>(at<uint16_t>(m + 0x2cc) + speed);
            at<uint16_t>(m + 0x2ce) = static_cast<uint16_t>(speed - (static_cast<int16_t>(speed) >> 5));
        }
        const uint16_t life = static_cast<uint16_t>(at<uint16_t>(m + 0x10) - 1);
        at<uint16_t>(m + 0x10) = life;
        if (life == 0x41)
        {
            guest_call(kObjectCarChangeOver, at<uint32_t>(m + 0x2c4), at<uint32_t>(m + 0x2c8));
            for (uint32_t index = ringHeadU(m); index != ringTail(m); index = (index + 1) & ringMask(m))
            {
                const uint32_t p = m + kParticles + index * 0x14;
                for (uint32_t axis = 0; axis < 3; ++axis)
                    at<int16_t>(p + 8 + 2 * axis) = static_cast<int16_t>(-at<int16_t>(p + 8 + 2 * axis) >> 1);
            }
        }
        else if (life == 0)
            at<uint8_t>(at<uint32_t>(m + 0x2c4) + 0x176) = 0;
    }
    vfx::sortPosFromCar(m, ptr32<_sObject>(car), 0xb0);
}

namespace {
// The drops of the car water effects (0x30 bytes from +0x50: a float position, a velocity from +0x10, the ground height
// under the drop at +0x20, a life at +0x24). A new one starts at a random point of a cube round `centre` (rand() times
// `scale`, less `half`), and a cast down finds the ground under it (-1000 if none). The three casts' records are the
// original's locals at sp+0x10, +0x20 and +0x50 (the caller's GuestFrame).
uint32_t startDrop(uint32_t m, const GuestFrame &frame, const int32_t centre[3], ee_float scale, ee_float half)
{
    const uint32_t p = m + 0x50 + ringTail(m) * 0x30u;
    for (uint32_t axis = 0; axis < 3; ++axis)
        at<float>(p + 4 * axis) = (ee_itof(rand()) * scale - half) + ee_itof(centre[axis]);
    const ptr32<_sColCellVecPSX> hit = frame.local<_sColCellVecPSX>(0x10);
    if (AColDownCast(ptr32<_sColCellVecPSX>(p), hit, frame.local<_sColNormalPSX>(0x20), frame.local<uint8>(0x50)))
        at<uint32_t>(p + 0x20) = at<uint32_t>(hit.address + 4);
    else
        at<float>(p + 0x20) = -1000.0f;
    at<int32_t>(p + 0x24) = 0x20;
    return p;
}

// Moves the drops: each falls `fall` faster a frame until it is within 10 units of its ground, where it stops with five
// frames to live (unless it had fewer than six left). A drop whose life reaches zero is dropped from the ring.
void moveDrops(uint32_t m, ee_float fall)
{
    for (uint32_t index = ringHeadU(m); index != ringTail(m); index = (index + 1) & ringMask(m))
    {
        const uint32_t p = m + 0x50 + index * 0x30;
        const ee_float y = ee_float(at<float>(p + 4)) + ee_float(at<float>(p + 0x14));
        const ee_float ground = ee_float(at<float>(p + 0x20)) + ee_float(10.0f);
        at<float>(p) = ee_float(at<float>(p)) + ee_float(at<float>(p + 0x10));
        at<float>(p + 8) = ee_float(at<float>(p + 8)) + ee_float(at<float>(p + 0x18));
        at<float>(p + 4) = y;
        const uint32_t life = at<uint32_t>(p + 0x24) - 1;
        at<uint32_t>(p + 0x24) = life;
        if (!(y <= ground) || life < 6)
            at<float>(p + 0x14) = ee_float(at<float>(p + 0x14)) - fall;
        else
        {
            at<float>(p + 0x14) = 0.0f;
            at<int32_t>(p + 0x24) = 5;
        }
        if (at<int32_t>(p + 0x24) == 0)
        {
            const uint32_t head = ringHeadU(m);
            if (index != head)
                std::memmove(&at<uint8_t>(p), &at<uint8_t>(m + 0x50 + head * 0x30), 0x30);
            ringCount(m) = static_cast<uint8_t>(ringCount(m) - 1);
            ringHeadU(m) = static_cast<uint8_t>(ringMask(m) & (ringHeadU(m) + 1));
        }
    }
}
} // namespace

// The water a car throws up when it drives through deep water: the car is at +0x650, a phase at +0x654 steps by 55. Up
// to three drops a frame start within 25 units of the car's aerial, moving out sideways at random, and fall half a
// unit a frame faster (moveDrops). The effect ends when the car no longer holds its weapon.
void VfxHandler_CarWaterBall(ptr32<_sVfx_Object> mem)
{
    const uint32_t m = mem.address;
    const uint32_t car = at<uint32_t>(m + 0x650);
    const uint64_t aerial = VfxCarAerialVal(ptr32<_sObject>(car), 5);
    vfx::sortPosFromCar(m, ptr32<_sObject>(car), 0x120, 0x40);
    GuestFrame frame(0x120);
    const int32_t centre[3] = {static_cast<int16_t>(aerial), static_cast<int16_t>(aerial >> 16), static_cast<int16_t>(aerial >> 32)};
    at<uint32_t>(m + 0x654) = at<uint32_t>(m + 0x654) + 0x37;
    const ee_float spread(at<float>(sym::lit4_3277e0));
    for (int32_t i = 0; i < 3; ++i)
    {
        if (!(ringCount(m) < ringMask(m)))
            continue;
        const uint32_t p = startDrop(m, frame, centre, float_from_bits(0x32c80000u), ee_float(25.0f));
        const int32_t rx = vfx::random15();
        at<float>(p + 0x14) = 0.0f;
        at<float>(p + 0x10) = ee_itof((rx & 0x3f) - 0x1f) * spread;
        at<float>(p + 0x18) = ee_itof((vfx::random15() & 0x3f) - 0x1f) * spread;
        pushTail(m);
    }
    moveDrops(m, ee_float(0.5f));
    at<uint16_t>(m + 0x10) = static_cast<uint16_t>(at<uint16_t>(m + 0x10) - 1);
    if (at<int8_t>(car + 0x194) == 0)
        at<uint16_t>(m + 0x10) = 0;
}

// A car that has been in deep water drips: its whole-unit position goes to svVelTemp, and while more than 16 frames
// are left one drop a frame starts within 60 units of it, splashing up and out. While the effect lasts it keeps
// setting a counter of the car's (+0xb4 of the record at +0x40) to 300.
void VfxHandler_CarWaterCovered(ptr32<_sVfx_Object> mem)
{
    const uint32_t m = mem.address;
    const uint32_t car = at<uint32_t>(m + 0x650);
    const uint32_t integer = at<uint32_t>(at<uint32_t>(car + 0x20) + 0x1e8);
    const int32_t centre[3] = {static_cast<int16_t>(at<int32_t>(integer + 4) >> 8), static_cast<int16_t>(at<int32_t>(integer + 8) >> 8),
                               static_cast<int16_t>(at<int32_t>(integer + 0xc) >> 8)};
    for (uint32_t axis = 0; axis < 3; ++axis)
        at<int16_t>(sym::svVelTemp + 2 * axis) = static_cast<int16_t>(centre[axis]); // svVelTemp
    vfx::sortPosFromCar(m, ptr32<_sObject>(car), 0xf0, 0x40);
    GuestFrame frame(0xf0);
    at<uint32_t>(m + 0x654) = at<uint32_t>(m + 0x654) + 0x37;
    if (ringCount(m) < ringMask(m) && at<uint16_t>(m + 0x10) >= 0x10)
    {
        const uint32_t p = startDrop(m, frame, centre, float_from_bits(0x33700000u), ee_float(60.0f));
        const ee_float spread(at<float>(sym::lit4_327814));
        at<float>(p + 0x10) = ee_itof((vfx::random15() & 0x3f) - 0x1f) * spread;
        at<float>(p + 0x14) = ee_itof(vfx::random15() & 0x3f) * ee_float(at<float>(sym::lit4_327818));
        at<float>(p + 0x18) = ee_itof((vfx::random15() & 0x3f) - 0x1f) * spread;
        pushTail(m);
    }
    moveDrops(m, ee_float(at<float>(sym::lit4_32781c)));
    const uint16_t life = static_cast<uint16_t>(at<uint16_t>(m + 0x10) - 1);
    at<uint16_t>(m + 0x10) = life;
    if (life >= 0x10)
        at<int32_t>(at<uint32_t>(car + 0x40) + 0xb4) = 300;
}

// ---------------------------------------------------------------------------------------------------------------------
// The start lights

namespace {
constexpr uint32_t kSfxPlay = sym::SfxPlay, kRevSfxReadyGoFade = sym::RevSfxReadyGoFade, kRCMusicSetVol = sym::RCMusicSetVol;
constexpr uint32_t kMainVar = sym::mainVarG; // mainVarG
}

// The "ready, steady, go" lights: on the first frame (life 63) they are placed 250 units above and a quarter of the view
// direction in front of the camera (the split-screen camera when the screen is split), drifting up 6 units a frame, and
// the start boost is turned off. While more than 23 frames are left a spark (0x14 bytes from +0x48) starts each frame,
// and the lights' size (+0x1a0) grows by a speed (+0x1a2) that falls by 16 each frame. The count of this light (the word
// at +0x1a4: 0 ready, 1 steady, 2 steady, 3 go) picks its sound at 22 frames from the end; "go" also fades the countdown
// sound, turns the start boost on and brings the music up. At 16 frames the boost goes off again.
void VfxHandler_ReadySteadyGo(ptr32<_sVfx_Object> mem)
{
    const uint32_t m = mem.address;
    if (at<uint16_t>(m + 0x10) == 0x3f)
    {
        at<uint16_t>(m + 0x1a0) = static_cast<uint16_t>(-0x334);
        at<uint16_t>(m + 0x19a) = 6;
        at<uint16_t>(m + 0x1a2) = 0x280;
        at<uint16_t>(m + 0x198) = 0;
        at<uint16_t>(m + 0x19c) = 0;
        const bool split = at<uint8_t>(kMainVar + 0x368) != 0;
        std::memcpy(&at<uint8_t>(m + 0x188), &at<uint8_t>(kVisFx + (split ? 0x110 : 0x108)), 8); // svCamPos(Split)
        const uint32_t rot = kVisFx + (split ? 0xdc : 0xbc);                                         // mCamInverseRot(Split)
        add16(m + 0x188, at<int16_t>(rot + 4) >> 2);
        add16(m + 0x18a, 0xfa + (at<int16_t>(rot + 0xa) >> 2));
        add16(m + 0x18c, at<int16_t>(rot + 0x10) >> 2);
        at<uint8_t>(kMainVar + 0x36d) = 0; // startBoostOn
        at<uint8_t>(kMainVar + 0x36c) = 0;
    }
    add16(m + 0x188, at<uint16_t>(m + 0x198));
    add16(m + 0x18a, at<uint16_t>(m + 0x19a));
    add16(m + 0x18c, at<uint16_t>(m + 0x19c));
    vfx::sortPosFromInts(m, at<int16_t>(m + 0x188), at<int16_t>(m + 0x18a), at<int16_t>(m + 0x18c), 0x60);
    GuestFrame frame(0x60);
    if (at<uint16_t>(m + 0x10) < 0x18)
        at<uint16_t>(m + 0x1a0) = 0;
    else
    {
        const auto particle = [&]() { return m + 0x48 + ringTail(m) * 0x14u; };
        at<uint16_t>(particle()) = rnd16(0x7f, -0x40);
        at<uint16_t>(particle() + 2) = rnd16(0x7f, -0x40);
        at<uint16_t>(particle() + 4) = rnd16(0x7f, -0x40);
        at<uint16_t>(particle() + 8) = rnd16(0x7f, -0x3f);
        at<uint16_t>(particle() + 0xa) = rnd16(0x7f, -0x3f);
        at<uint16_t>(particle() + 0xc) = rnd16(0x7f, -0x3f);
        at<uint16_t>(particle() + 0x10) = 0xf;
        pushTail(m);
        const uint16_t speed = at<uint16_t>(m + 0x1a2);
        add16(m + 0x1a0, speed);
        if (static_cast<int16_t>(speed) < 0)
            at<uint16_t>(m + 0x1a2) = 0;
    }
    const auto sound = [](int32_t id) { guest_call(kSfxPlay, id, 0xff, 0xf, 0xf, 0x100, 0, 0); };
    const int32_t light = at<int32_t>(m + 0x1a4);
    const uint16_t life = at<uint16_t>(m + 0x10);
    if (life == 0x16)
    {
        if (light == 1)
            sound(0x3c);
        else if (light == 0)
            sound(0x3d);
        else if (light == 2)
            sound(0x3b);
        else if (light == 3)
        {
            sound(0x3e);
            guest_call(kRevSfxReadyGoFade, 10, 0x1e);
            at<uint8_t>(kMainVar + 0x36d) = 1;
            at<uint8_t>(kMainVar + 0x36c) = 1;
        }
    }
    else if (life == 0x10)
    {
        at<uint8_t>(kMainVar + 0x36d) = 0;
        at<uint8_t>(kMainVar + 0x36c) = 0;
    }
    if (at<uint16_t>(m + 0x10) == 6 && at<int32_t>(m + 0x1a4) == 3)
        guest_call(kRCMusicSetVol, 1);
    if (at<uint16_t>(m + 0x10) == 2 && at<int32_t>(m + 0x1a4) == 3)
        guest_call(kRCMusicSetVol, 2);
    add16(m + 0x1a2, -0x10);
    VfxSimpleParticleMover(ptr32<_sVfx_PartHeader>(m + kRing), ptr32<_VFX_SIMPLEPARTICLE>(m + 0x48));
    add16(m + 0x10, -1);
}

// ---------------------------------------------------------------------------------------------------------------------
// More car effects

ptr32<MATRIX> VfxCarMatrixPoi(ptr32<_sObject> pObj); // ai.cpp
int32 VfxAreWeInCar(ptr32<_sObject> pObj);           // ai.cpp
void VfxCarSortPos(ptr32<_sObject> pObj, ptr32<_sVfx_Object> fx);

// The rocket car's exhaust (VISFX3): the car is at +0x350. Except on level 'b', a puff (0x30 bytes from +0x50: a float
// position, a velocity from +0x10, a life of 15 at +0x24) starts each frame at the exhaust (behind and above the car's
// centre, a different place in the cockpit view), carried back against the car's heading by its speed with a little
// random spread. Puffs move and die when their life runs out.
void Vfx3Handler_RocketCar(ptr32<_sVfx_Object> mem)
{
    GuestFrame frame(0xd0);
    const uint32_t m = mem.address;
    const ptr32<_sObject> car(at<uint32_t>(m + 0x350));
    (void)VfxCarAerialVal(car, 5); // read and not used
    VfxCarSortPos(car, mem);
    if (at<uint8_t>(kMainVar + 0x34a) != 'b') // mainVarG.level
    {
        const ptr32<MATRIX> matrix = VfxCarMatrixPoi(car);
        const ptr32<ee_float> vuMatrix = VfxPSXToVUMatrix(matrix);
        if (ringCount(m) < ringMask(m))
        {
            const uint32_t p = m + 0x50 + ringTail(m) * 0x30u;
            const bool inCar = VfxAreWeInCar(car) != 0;
            at<float>(p + 8) = inCar ? -75.0f : -44.0f;
            at<float>(p + 4) = inCar ? 11.0f : -15.0f;
            at<uint32_t>(p) = 0;
            const ee_float spread(at<float>(sym::lit4_32790c)), drag(at<float>(sym::lit4_327910));
            sceVu0ApplyMatrix(ptr32<ee_float>(p), vuMatrix, ptr32<ee_float>(p));
            const uint32_t physics = at<uint32_t>(car.address + 0x20);
            for (uint32_t axis = 0; axis < 3; ++axis)
            {
                const int32_t r = (vfx::random15() & 0x3f) - 0x1f;
                const ee_float heading = ee_itof(at<int16_t>(matrix.address + 4 + 6 * axis)) * ee_float(at<float>(physics + 0x38));
                if (axis == 2)
                    at<int32_t>(p + 0x24) = 0xf;
                at<float>(p + 0x10 + 4 * axis) = ee_itof(r) * spread - heading * drag;
            }
            pushTail(m);
        }
    }
    for (uint32_t index = ringHeadU(m); index != ringTail(m); index = (index + 1) & ringMask(m))
    {
        const uint32_t p = m + 0x50 + index * 0x30;
        for (uint32_t axis = 0; axis < 3; ++axis)
            at<float>(p + 4 * axis) = ee_float(at<float>(p + 4 * axis)) + ee_float(at<float>(p + 0x10 + 4 * axis));
        const int32_t life = at<int32_t>(p + 0x24) - 1;
        at<int32_t>(p + 0x24) = life;
        if (life == 0)
        {
            const uint32_t head = ringHeadU(m);
            if (index != head)
                std::memmove(&at<uint8_t>(p), &at<uint8_t>(m + 0x50 + head * 0x30), 0x30);
            ringCount(m) = static_cast<uint8_t>(ringCount(m) - 1);
            ringHeadU(m) = static_cast<uint8_t>(ringMask(m) & (ringHeadU(m) + 1));
        }
    }
}

} // namespace rc


// Forward declarations of the random-input setups (visfx_fuzz.cpp).
namespace rc::fuzz {
void vfxPirateLavaHandler(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxLavaSpitHandler(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxGullsHandler(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxButterfliesHandler(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxLight(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxOilSlickHandler(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxCarChangeOver(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxCarWaterBallHandler(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxCarWaterCovered(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxReadySteadyGoHandler(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxRocketCarHandler(R5900Context *ctx, uint64_t &state, uint32_t scratch);
}

RC_DECOMP_FN_SETUP(Vfx2Handler_PirateLava, rc::fuzz::vfxPirateLavaHandler)
RC_DECOMP_FN_SETUP(Vfx2Handler_LavaSpit, rc::fuzz::vfxLavaSpitHandler)
RC_DECOMP_FN_SETUP(Vfx2Handler_Gulls, rc::fuzz::vfxGullsHandler)
RC_DECOMP_FN_SETUP(Vfx2Handler_Butterflies, rc::fuzz::vfxButterfliesHandler)
RC_DECOMP_FN_SETUP(Vfx2Renderer_LightGlow, rc::fuzz::vfxLight)
RC_DECOMP_FN_SETUP(Vfx2Renderer_LightFlicker, rc::fuzz::vfxLight)
RC_DECOMP_FN_SETUP(VfxHandler_OilSlick, rc::fuzz::vfxOilSlickHandler)
RC_DECOMP_FN_SETUP(VfxHandler_CarChangeOver, rc::fuzz::vfxCarChangeOver)
RC_DECOMP_FN_SETUP(VfxHandler_CarWaterBall, rc::fuzz::vfxCarWaterBallHandler)
RC_DECOMP_FN_SETUP(VfxHandler_CarWaterCovered, rc::fuzz::vfxCarWaterCovered)
RC_DECOMP_FN_SETUP(VfxHandler_ReadySteadyGo, rc::fuzz::vfxReadySteadyGoHandler)
RC_DECOMP_FN_SETUP(Vfx3Handler_RocketCar, rc::fuzz::vfxRocketCarHandler)
