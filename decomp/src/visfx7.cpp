// Decompiled from VISFX2.c and VISFX3.c, continued (sixth batch): the scenery handlers and renderers that were still
// recompiled (bats, sci-fi bubbles, light flares and candles, butterflies, car splash, pirate lava, cannon, glints). See
// visfx2.cpp for the layout of a scenery effect (`_sVfx_TSFX`) and visfx5.cpp for the renderers' conventions.

#include "ai_fuzz.h"
#include "ee_math.h"
#include "vifpk.h"
#include "visfx.h"

#include <algorithm>
#include <emmintrin.h>

namespace rc {

void VfxPutUV0(ee_float u, ee_float v);
void VfxPutUV1(ee_float u, ee_float v);
void VfxPutUV2(ee_float u, ee_float v);
void VfxPutUV3(ee_float u, ee_float v);
void VfxPS2SpriteGroundRotated3D(ptr32<SVECTOR> svNormal, ee_float zOffset);
bool_ AColDownCast(ptr32<_sColCellVecPSX> pPos, ptr32<_sColCellVecPSX> pPoi, ptr32<_sColNormalPSX> pNormal, ptr32<uint8> pMatId); // collision.cpp

namespace {

using vfx::at;
using vfx::ringHead;
using vfx::ringNext;
using vfx::ringPush;
using vfx::sortAtScenery;

constexpr uint32_t kRing = 0x40;      // the ring header: mask, count, head, tail
constexpr uint32_t kParticles = 0x44; // the first particle
constexpr uint32_t kVisFx = sym::visFxVarG; // visFxVarG

// The distance from the camera (visFxVarG.svCamPos, three shorts at +0x108) to a scenery instance, as the effects measure
// it: the three whole-unit differences (as 16 bits), then the largest plus half the next plus half the smallest, roughly.
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

} // namespace

// The bats: a ring of 0x14-byte particles (position, velocity from +8, a spin at +0x12). A counter at +0x2c6 runs down
// to zero, when the beat count at +0x2c4 goes up and the counter restarts at four to nineteen; on odd beats a new bat
// starts at the scenery's position (the instance's matrix's up vector gives its velocity, with a random scatter) with a
// life of 200. They flap (spin and rise a little) and wander sideways at random.
void Vfx2Handler_Bats(ptr32<_sVfx_TSFX> mem)
{
    const uint32_t m = mem.address;
    const uint16_t counter = static_cast<uint16_t>(at<uint16_t>(m + 0x2c6) - 1);
    at<uint16_t>(m + 0x2c6) = counter;
    if (counter == 0)
    {
        at<uint16_t>(m + 0x2c4) = static_cast<uint16_t>(at<uint16_t>(m + 0x2c4) + 1);
        at<uint16_t>(m + 0x2c6) = static_cast<uint16_t>((vfx::random15() & 0xf) + 4);
    }
    if ((at<uint16_t>(m + 0x2c4) & 1) != 0 && at<uint8_t>(m + kRing + 1) < at<uint8_t>(m + 0x40))
    {
        const uint32_t tob = at<uint32_t>(m + 0x10) + 0x18;
        const uint32_t p = m + kParticles + static_cast<uint32_t>(static_cast<int8_t>(at<uint8_t>(m + kRing + 3))) * 0x14;
        at<uint16_t>(p) = 0;
        at<uint16_t>(p + 2) = 0;
        at<uint16_t>(p + 4) = 0;
        at<uint16_t>(p + 8) = static_cast<uint16_t>((vfx::random15() & 0x1f) + (at<int16_t>(tob + 2) >> 5) - 0x10);
        at<uint16_t>(p + 0xa) = static_cast<uint16_t>((vfx::random15() & 0x1f) + (at<int16_t>(tob + 8) >> 5) - 0x10);
        at<uint16_t>(p + 0xc) = static_cast<uint16_t>((vfx::random15() & 0x1f) + (at<int16_t>(tob + 0xe) >> 5) - 0x10);
        at<uint16_t>(p + 0x10) = 200;
        at<uint16_t>(p + 0x12) = static_cast<uint16_t>(vfx::random15());
        ringPush(m);
    }
    VfxSimpleParticleMover(ptr32<_sVfx_PartHeader>(m + kRing), ptr32<_VFX_SIMPLEPARTICLE>(m + kParticles));
    for (int32_t index = ringHead(m); index != at<uint8_t>(m + kRing + 3); index = ringNext(m, index))
    {
        const uint32_t p = m + kParticles + static_cast<uint32_t>(index * 0x14);
        at<uint16_t>(p + 0x12) = static_cast<uint16_t>(at<uint16_t>(p + 0x12) + 1);
        at<uint16_t>(p + 0xa) = static_cast<uint16_t>(at<uint16_t>(p + 0xa) + 1);
        at<uint16_t>(p + 2) = static_cast<uint16_t>(at<uint16_t>(p + 2) - 0x3f + (vfx::random15() & 0x7f));
    }
}

// The sci-fi bubbles: as the bats' counters, but a new bubble every fourth beat (at most 15 at once), rising with a
// random wobble that decays (velocity by a sixteenth, rising by an eighth) and living 60 to 91 frames. The effect is
// sorted at the scenery position.
void Vfx2Handler_SciFiBubbles(ptr32<_sVfx_TSFX> mem)
{
    const uint32_t m = mem.address;
    const uint16_t counter = static_cast<uint16_t>(at<uint16_t>(m + 0x186) - 1);
    at<uint16_t>(m + 0x186) = counter;
    if (counter == 0)
    {
        at<uint16_t>(m + 0x184) = static_cast<uint16_t>(at<uint16_t>(m + 0x184) + 1);
        at<uint16_t>(m + 0x186) = static_cast<uint16_t>((vfx::random15() & 0xf) + 0x10);
    }
    if ((at<uint16_t>(m + 0x184) & 3) == 0 && at<uint8_t>(m + kRing + 1) < 0xf)
    {
        const uint32_t tob = at<uint32_t>(m + 0x10) + 0x18;
        const uint32_t p = m + kParticles + static_cast<uint32_t>(static_cast<int8_t>(at<uint8_t>(m + kRing + 3))) * 0x14;
        at<uint16_t>(p) = 0;
        at<uint16_t>(p + 2) = 0;
        at<uint16_t>(p + 4) = 0;
        at<uint16_t>(p + 8) = static_cast<uint16_t>((vfx::random15() & 0x1f) + (at<int16_t>(tob + 2) >> 6) - 0x10);
        at<uint16_t>(p + 0xa) = static_cast<uint16_t>((vfx::random15() & 0x1f) + (at<int16_t>(tob + 8) >> 6) - 0x10);
        at<uint16_t>(p + 0xc) = static_cast<uint16_t>((vfx::random15() & 0x1f) + (at<int16_t>(tob + 0xe) >> 6) - 0x10);
        at<uint16_t>(p + 0x10) = static_cast<uint16_t>((vfx::random15() & 0x1f) + 0x3c);
        at<uint16_t>(p + 0x12) = static_cast<uint16_t>(vfx::random15());
        ringPush(m);
    }
    int32_t index = ringHead(m);
    if (index != at<uint8_t>(m + kRing + 3))
    {
        do
        {
            const uint32_t p = m + kParticles + static_cast<uint32_t>(index * 0x14);
            at<uint16_t>(p) = static_cast<uint16_t>(at<uint16_t>(p) + at<uint16_t>(p + 8) + ((vfx::random15() & 0x1f) - 0x10));
            at<uint16_t>(p + 2) = static_cast<uint16_t>(at<uint16_t>(p + 2) + at<uint16_t>(p + 0xa) + ((vfx::random15() & 0x1f) - 0x10));
            const uint16_t vy = at<uint16_t>(p + 0xa), vz = at<uint16_t>(p + 0xc), vx = at<uint16_t>(p + 8);
            const uint16_t life = static_cast<uint16_t>(at<uint16_t>(p + 0x10) - 1);
            const int32_t z = at<uint16_t>(p + 4) + vz + ((vfx::random15() & 0x1f) - 0x10);
            at<uint16_t>(p + 4) = static_cast<uint16_t>(z);
            at<uint16_t>(p + 8) = static_cast<uint16_t>(vx - (static_cast<int16_t>(vx) >> 4));
            at<uint16_t>(p + 0xc) = static_cast<uint16_t>(vz - (static_cast<int16_t>(vz) >> 4));
            at<uint16_t>(p + 0xa) = static_cast<uint16_t>(vy - (static_cast<int16_t>(vy) >> 3) + 1);
            at<uint16_t>(p + 0x10) = life;
            if (life == 0)
            {
                const uint8_t head = at<uint8_t>(m + kRing + 2);
                if (index != head)
                    std::memcpy(&at<uint8_t>(p), &at<uint8_t>(m + kParticles + head * 0x14u), 0x14);
                at<uint8_t>(m + kRing + 1) = static_cast<uint8_t>(at<uint8_t>(m + kRing + 1) - 1);
                at<uint8_t>(m + kRing + 2) = static_cast<uint8_t>(at<uint8_t>(m + kRing) & (at<uint8_t>(m + kRing + 2) + 1));
            }
            index = ringNext(m, index);
        } while (index != at<uint8_t>(m + kRing + 3));
    }
    sortAtScenery(m, 0x90);
}

// The glow of a street lamp or window light: a sprite of the flare texture at the scenery position, fading with the
// distance from the camera (full up to 1024 units, then dimmer, and nothing past 9000), tinted by the light's colour (three
// bytes at +0x48) less the fade, its size growing with the distance (a float count at +0x4c times the distance).
void Vfx2Renderer_LightFlare(ptr32<_sVfx_TSFX> mem)
{
    GuestFrame frame(0x20);
    const uint32_t m = mem.address;
    const uint32_t tob = at<uint32_t>(m + 0x10);
    const int32_t distance = sceneryDistance(tob);
    if (!(static_cast<uint32_t>(distance - 1) < 0x2327u))
        return;
    const uint32_t sp = vfx::sprite();
    at<float>(sp + vfx::spr::kPos) = ee_itof(at<int32_t>(tob + 4) >> 8);
    at<float>(sp + vfx::spr::kPos + 4) = ee_itof(at<int32_t>(tob + 8) >> 8);
    at<float>(sp + vfx::spr::kPos + 8) = ee_itof(at<int32_t>(tob + 0xc) >> 8);
    const int32_t fade = atLeastZero((0x400 - distance) >> 4) + atLeastZero((distance - 0x1b58) >> 3);
    const int32_t red = atLeastZero(at<uint8_t>(m + 0x48) - fade), green = atLeastZero(at<uint8_t>(m + 0x49) - fade), blue = atLeastZero(at<uint8_t>(m + 0x4a) - fade);
    at<uint8_t>(sp + vfx::spr::kFlipX) = 0;
    const uint32_t s2 = vfx::sprite();
    at<uint32_t>(s2 + vfx::spr::kTexture) = vfx::vfxTexture(0x17);
    at<uint16_t>(s2 + vfx::spr::kAngle) = static_cast<uint16_t>(distance);
    at<uint32_t>(s2 + vfx::spr::kRgb) = static_cast<uint32_t>(red + green * 0x100 + blue * 0x10000);
    // The size is the 32-bit word at +0x4c as an unsigned number.
    const uint32_t count = at<uint32_t>(m + 0x4c);
    const ee_float size = static_cast<int32_t>(count) >= 0 ? ee_itof(static_cast<int32_t>(count))
                                                           : ee_itof(static_cast<int32_t>((count >> 1) | (count & 1))) + ee_itof(static_cast<int32_t>((count >> 1) | (count & 1)));
    at<float>(s2 + vfx::spr::kScale) = (size * ee_itof(distance)) * ee_float(at<float>(sym::lit4_3278a8));
    VfxPS2SpriteRotated3D(ee_float(-100.0f));
}

// The candle's light: the flare's distance fade, with a flicker (a random 0 to 63 added to each colour byte that is not
// zero), a random spin, and a size from the instance (the halfword at +0x2c of the scenery object).
void Vfx2Renderer_LightCandle(ptr32<_sVfx_TSFX> mem)
{
    GuestFrame frame(0x80);
    const uint32_t m = mem.address;
    const uint32_t tob = at<uint32_t>(m + 0x10);
    const int32_t distance = sceneryDistance(tob);
    if (!(static_cast<uint32_t>(distance - 1) < 0x2327u))
        return;
    const uint32_t sp = vfx::sprite();
    at<float>(sp + vfx::spr::kPos + 8) = ee_itof(at<int32_t>(tob + 0xc) >> 8);
    at<float>(sp + vfx::spr::kPos) = ee_itof(at<int32_t>(tob + 4) >> 8);
    at<float>(sp + vfx::spr::kPos + 4) = ee_itof(at<int32_t>(tob + 8) >> 8);
    const int32_t flicker = vfx::random15() & 0x3f;
    const int32_t red = at<uint8_t>(m + 0x48) != 0 ? at<uint8_t>(m + 0x48) + flicker : 0;
    const int32_t green = at<uint8_t>(m + 0x49) != 0 ? at<uint8_t>(m + 0x49) + flicker : 0;
    const int32_t blue = at<uint8_t>(m + 0x4a) != 0 ? at<uint8_t>(m + 0x4a) + flicker : 0;
    const int32_t fade = atLeastZero((0x400 - distance) >> 4) + atLeastZero((distance - 0x1b58) >> 3);
    const uint32_t s2 = vfx::sprite();
    at<uint8_t>(s2 + vfx::spr::kCode) = 2;
    at<uint32_t>(s2 + vfx::spr::kTexture) = vfx::vfxTexture(1);
    const int32_t r = atLeastZero(red - fade), g = atLeastZero(green - fade), b = atLeastZero(blue - fade);
    at<uint8_t>(s2 + vfx::spr::kFlipX) = 0;
    const int32_t spin = vfx::random15();
    const uint32_t s3 = vfx::sprite();
    at<uint16_t>(s3 + vfx::spr::kAngle) = static_cast<uint16_t>(spin);
    at<uint32_t>(s3 + vfx::spr::kRgb) = static_cast<uint32_t>(r + g * 0x100 + b * 0x10000);
    at<float>(s3 + vfx::spr::kScale) = ee_itof(at<uint16_t>(at<uint32_t>(m + 0x10) + 0x2c));
    VfxPS2SpriteRotated3D(ee_float(-100.0f));
}

// The butterflies: a ring of 0x70-byte particles round the centre (shorts at +0x750) while the camera is within 7500 units
// on each axis. Each is a pair of wings drawn as quads in the effect's own matrix (the particle's 4x4 at +0x20): the wings
// are six points flapping with the sine of the particle's phase (the low five bits of +0x10 scaled up), added to the particle's
// position (in 8ths of a unit) and the centre, with the colours at +0x60 and +0x64.
void Vfx2Renderer_Butterflies(ptr32<_sVfx_Object> mem)
{
    GuestFrame frame(0x180);
    const uint32_t m = mem.address;
    const auto near = [&](uint32_t centre, uint32_t camera)
    {
        const int32_t diff = at<int16_t>(centre) - at<int16_t>(camera);
        return diff > 0 ? diff < 0x1d4c : at<int16_t>(camera) - at<int16_t>(centre) < 0x1d4c;
    };
    if (!near(m + 0x750, kVisFx + 0x108) || !near(m + 0x752, kVisFx + 0x10a) || !near(m + 0x754, kVisFx + 0x10c))
        return;
    RdrPolyTexture(0, ptr32<_sGfxTexture>(vfx::vfxTexture(0x14)), 0x20);
    const uint32_t quad = G(visFxVarG).pGT4.address;
    at<float>(quad + 0x70) = -50.0f;
    const uint32_t wing = frame.local<float>(0x60).address, centre = frame.local<float>(0x50).address; // vu[6], vuPos
    int32_t index = static_cast<int8_t>(at<uint8_t>(m + kRing + 2));
    while (index != at<uint8_t>(m + kRing + 3))
    {
        const uint32_t p = m + 0x50 + static_cast<uint32_t>(index * 0x70);
        ++index;
        const int32_t x = at<int16_t>(m + 0x750) + (at<int16_t>(p) >> 3);
        const int32_t y = at<int16_t>(m + 0x752) + (at<int16_t>(p + 2) >> 3);
        const int32_t z = at<int16_t>(m + 0x754) + (at<int16_t>(p + 4) >> 3);
        const uint32_t phase = (static_cast<uint32_t>(at<uint16_t>(p + 0x10)) << 7) & 0xfff;
        const ee_float turn = ee_itof(static_cast<int32_t>(phase));
        at<float>(centre) = ee_itof(x);
        at<float>(centre + 4) = ee_itof(y);
        at<float>(centre + 8) = ee_itof(z);
        const ee_float sine = ee_sinf((turn + turn) * ee_float(at<float>(sym::lit4_3278cc)));
        const ee_float flap = ee_itof(ee_ftoi(sine * ee_float(4096.0f)) >> 7) + ee_float(35.0f);
        const float points[6][3] = {{0.0f, 10.0f, 28.0f}, {0.0f, 0.0f, -28.0f}, {32.0f, 0.0f, 28.0f}, {32.0f, 0.0f, -28.0f}, {-32.0f, 0.0f, 28.0f}, {-32.0f, 0.0f, -28.0f}};
        for (uint32_t i = 0; i < 6; ++i)
        {
            const uint32_t v = wing + 0x10 * i;
            at<float>(v) = points[i][0];
            at<float>(v + 4) = i >= 2 ? static_cast<float>(flap) : points[i][1];
            at<float>(v + 8) = points[i][2];
        }
        for (uint32_t i = 0; i < 6; ++i)
        {
            const uint32_t v = wing + 0x10 * i;
            sceVu0ApplyMatrix(ptr32<ee_float>(v), ptr32<ee_float>(p + 0x20), ptr32<ee_float>(v));
            vfx::vu0Add(v, v, centre);
        }
        sceVu0CopyVector(ptr32<ee_float>(quad + 0x10), ptr32<ee_float>(wing));
        sceVu0CopyVector(ptr32<ee_float>(quad), ptr32<ee_float>(wing + 0x10));
        sceVu0CopyVector(ptr32<ee_float>(quad + 0x30), ptr32<ee_float>(wing + 0x20));
        sceVu0CopyVector(ptr32<ee_float>(quad + 0x20), ptr32<ee_float>(wing + 0x30));
        VfxPutGT4rgba0(at<uint32_t>(p + 0x64) | 0x80000000u);
        VfxPutGT4rgba1(at<uint32_t>(p + 0x64) | 0x80000000u);
        VfxPutGT4rgba2(at<uint32_t>(p + 0x60) | 0x80000000u);
        VfxPutGT4rgba3(at<uint32_t>(p + 0x60) | 0x80000000u);
        RdrPoly(0, ptr32<_sRdrPoly>(quad));
        sceVu0CopyVector(ptr32<ee_float>(quad + 0x30), ptr32<ee_float>(wing + 0x40));
        sceVu0CopyVector(ptr32<ee_float>(quad + 0x20), ptr32<ee_float>(wing + 0x50));
        RdrPoly(0, ptr32<_sRdrPoly>(quad));
        index = static_cast<int8_t>(at<uint8_t>(m + kRing) & static_cast<uint8_t>(index));
    }
}

// The pirate ship's lava: a ring of 0x18-byte blobs round the scenery position (their position shorts are in quarter units)
// drawn as sprites of the lava texture, spun at random once for all, their colour (three bytes at +0x648) darkening
// with their age (the halfword at +0x14), flipped by the sign of their drift (+0x12); then the glowing ones (size byte
// above 100) as camera-facing streaks from the blob's position (an eighth of the offset) to the quarter-offset point.
void Vfx2Renderer_PirateLava(ptr32<_sVfx_TSFX> mem)
{
    GuestFrame frame(0xc0);
    const uint32_t m = mem.address;
    const uint32_t tob = at<uint32_t>(m + 0x10);
    const ptr32<int16_t> centre = frame.local<int16_t>(0);
    centre[0] = static_cast<int16_t>(at<int32_t>(tob + 4) >> 8);
    centre[1] = static_cast<int16_t>(at<int32_t>(tob + 8) >> 8);
    centre[2] = static_cast<int16_t>(at<int32_t>(tob + 0xc) >> 8);
    const uint32_t sp = vfx::sprite();
    at<uint8_t>(sp + vfx::spr::kCode) = 2;
    at<uint32_t>(sp + vfx::spr::kTexture) = vfx::vfxTexture(0x15);
    at<uint16_t>(vfx::sprite() + vfx::spr::kAngle) = static_cast<uint16_t>(vfx::random15());
    for (int32_t index = ringHead(m); index != at<uint8_t>(m + kRing + 3); index = ringNext(m, index))
    {
        const uint32_t p = m + kParticles + static_cast<uint32_t>(index * 0x18);
        const uint32_t s = vfx::sprite();
        at<float>(s + vfx::spr::kPos) = ee_itof(centre[0] + (at<int16_t>(p) >> 2));
        at<float>(s + vfx::spr::kPos + 4) = ee_itof(centre[1] + (at<int16_t>(p + 2) >> 2));
        at<float>(s + vfx::spr::kPos + 8) = ee_itof(centre[2] + (at<int16_t>(p + 4) >> 2));
        (void)rand(); // the original draws a number it does not use
        const int32_t dim = atLeastZero(0x10 - at<uint16_t>(p + 0x14)) * 8;
        const int32_t blue = atLeastZero(at<uint8_t>(m + 0x64a) - dim);
        const int32_t red = atLeastZero(at<uint8_t>(m + 0x648) - dim);
        const int32_t green = atLeastZero(at<uint8_t>(m + 0x649) - dim);
        at<uint8_t>(vfx::sprite() + vfx::spr::kFlipX) = at<int16_t>(p + 0x12) > 0 ? 0 : 1;
        const uint32_t s2 = vfx::sprite();
        at<uint32_t>(s2 + vfx::spr::kRgb) = static_cast<uint32_t>(red + (green << 8) + (blue << 16));
        at<uint16_t>(s2 + vfx::spr::kAngle) = at<uint16_t>(p + 0x10);
        at<float>(s2 + vfx::spr::kScale) = ee_itof(at<uint8_t>(p + 0x16)) + ee_itof(at<uint8_t>(p + 0x16));
        VfxPS2SpriteRotated3D(ee_float(-100.0f));
    }
    const uint32_t s3 = vfx::sprite();
    at<uint8_t>(s3 + vfx::spr::kCode) = 4;
    at<uint32_t>(s3 + vfx::spr::kTexture) = vfx::vfxTexture(5);
    const ptr32<float> from = frame.local<float>(0x10), to = frame.local<float>(0x20);
    for (int32_t index = ringHead(m); index != at<uint8_t>(m + kRing + 3); index = ringNext(m, index))
    {
        const uint32_t p = m + kParticles + static_cast<uint32_t>(index * 0x18);
        if (at<uint8_t>(p + 0x16) < 0x65)
            continue;
        const uint32_t glow = at<uint16_t>(p + 0x14);
        VfxPutGT4rgba0(0);
        VfxPutGT4rgba1(0);
        VfxPutGT4rgba2(glow << 1);
        VfxPutGT4rgba3(glow << 1);
        from[0] = ee_itof(centre[0] + (at<int16_t>(p) >> 3));
        from[1] = ee_itof(centre[1] + (at<int16_t>(p + 2) >> 3));
        from[2] = ee_itof(centre[2] + (at<int16_t>(p + 4) >> 3));
        to[0] = ee_itof(centre[0] + (at<int16_t>(p) >> 2));
        to[1] = ee_itof(centre[1] + (at<int16_t>(p + 2) >> 2));
        to[2] = ee_itof(centre[2] + (at<int16_t>(p + 4) >> 2));
        VfxPutGT4FacingPolyVU(ptr32<ee_float>(from.address), ptr32<ee_float>(to.address), ptr32<_sGfxTexture>(vfx::vfxTexture(0x19)), 2,
                              ee_float(256.0f), ee_float(-100.0f));
    }
}

// The splash round a car: the pirate lava's treatment of a ring of 0x14-byte drops (positions in 16ths of a unit round the
// centre at +0x548): a streak while the drop rises (flag zero) coloured by its height, a sprite once it falls, then the
// ring on the water under the splash (a ground sprite that grows, and a second, fainter and larger).
void VfxRenderer_CarSplash(ptr32<_sVfx_Object> mem)
{
    GuestFrame frame(0xd0);
    const uint32_t m = mem.address;
    const ptr32<int16_t> sv = frame.local<int16_t>(0x10);
    for (uint32_t index = at<uint8_t>(m + kRing + 2); index != at<uint8_t>(m + kRing + 3); index = (index + 1) & at<uint8_t>(m + kRing))
    {
        const uint32_t p = m + kParticles + index * 0x14;
        const int32_t y16 = static_cast<int16_t>(at<uint16_t>(p + 2));
        const int32_t height = y16 >> 4;
        const uint16_t x = static_cast<uint16_t>(at<uint16_t>(m + 0x548) + (at<int16_t>(p) >> 4));
        const uint16_t y = static_cast<uint16_t>(at<uint16_t>(m + 0x54a) + height);
        const uint16_t z = static_cast<uint16_t>(at<uint16_t>(m + 0x54c) + (at<int16_t>(p + 4) >> 4));
        sv[0] = static_cast<int16_t>(x);
        sv[1] = static_cast<int16_t>(y);
        sv[2] = static_cast<int16_t>(z);
        const uint16_t flag = at<uint16_t>(p + 0x12);
        if (flag == 0)
        {
            int32_t bright = y16 >> 6;
            bright = bright < 0x5b ? bright : 0x5a;
            bright = bright > 7 ? bright : 8;
            VfxPutGT4rgba2(static_cast<uint32_t>(bright) * 0x10101);
            VfxPutGT4rgba3(static_cast<uint32_t>(bright) * 0x10101);
            int32_t red = y16 >> 9;
            red = red > flag ? red : 1;
            red = red < 0x1b ? red : 0x1a;
            VfxPutGT4rgba0(static_cast<uint32_t>(red) << 16);
            VfxPutGT4rgba1(static_cast<uint32_t>(red) << 16);
            VfxPutGT4FacingPoly(ptr32<SVECTOR>(m + 0x548), ptr32<SVECTOR>(sv.address), ptr32<_sGfxTexture>(vfx::vfxTexture(10)), 2,
                                ee_itof(at<uint16_t>(m + 0x10) >> 1) + ee_float(32.0f), ee_float(-10.0f));
        }
        else
        {
            int32_t blue = height;
            blue = blue < 0x47 ? blue : 0x46;
            blue = blue > -1 ? blue : 0;
            const int32_t green = atLeastZero(blue - 0x28);
            const uint32_t sp = vfx::sprite();
            at<uint8_t>(sp + vfx::spr::kCode) = 2;
            at<float>(sp + vfx::spr::kPos) = ee_itof(sv[0]);
            at<float>(sp + vfx::spr::kPos + 4) = ee_itof(sv[1]);
            at<float>(sp + vfx::spr::kPos + 8) = ee_itof(sv[2]);
            at<uint16_t>(sp + vfx::spr::kAngle) = static_cast<uint16_t>(vfx::random15());
            const int32_t sizeBits = vfx::random15() & 0xf;
            const uint32_t s2 = vfx::sprite();
            at<uint32_t>(s2 + vfx::spr::kTexture) = vfx::vfxTexture(0x1c);
            at<uint32_t>(s2 + vfx::spr::kRgb) = static_cast<uint32_t>(green * 0x100 + (blue << 16));
            at<float>(s2 + vfx::spr::kScale) = ee_itof((sizeBits + at<uint16_t>(m + 0x10)) >> 1) + ee_float(8.0f);
            VfxPS2SpriteRotated3D(ee_float(-20.0f));
        }
    }
    const uint32_t life = at<uint16_t>(m + 0x10);
    if (life == 0x50)
        return;
    const uint32_t sp = vfx::sprite();
    at<uint8_t>(sp + vfx::spr::kFlipX) = 0;
    at<uint16_t>(sp + vfx::spr::kAngle) = 0;
    sv[1] = 0x1000;
    at<float>(sp + vfx::spr::kPos) = ee_itof(at<int16_t>(m + 0x548));
    sv[0] = 0;
    sv[2] = 0;
    at<float>(sp + vfx::spr::kPos + 4) = ee_itof(at<int16_t>(m + 0x54a));
    at<uint8_t>(sp + vfx::spr::kCode) = 4;
    at<float>(sp + vfx::spr::kScale) = ee_itof(0xf0 - static_cast<int32_t>(life) * 2);
    at<uint32_t>(sp + vfx::spr::kTexture) = vfx::vfxTexture(0xb);
    at<uint32_t>(sp + vfx::spr::kRgb) = life * 0x10101;
    at<float>(sp + vfx::spr::kPos + 8) = ee_itof(at<int16_t>(m + 0x54c));
    VfxPS2SpriteGroundRotated3D(ptr32<SVECTOR>(sv.address), ee_float(-20.0f));
    const int32_t shade = ee_ftoi(ee_itof(static_cast<int32_t>(life)) * ee_float(at<float>(sym::lit4_3278c0)));
    const uint32_t s2 = vfx::sprite();
    at<uint8_t>(s2 + vfx::spr::kCode) = 2;
    const ee_float grow = ee_itof(0x50 - static_cast<int32_t>(at<uint16_t>(m + 0x10))) * ee_float(at<float>(sym::lit4_3278c8));
    const int32_t grey = ee_ftoi(ee_itof(shade) * ee_float(at<float>(sym::lit4_3278c4)));
    at<uint32_t>(s2 + vfx::spr::kRgb) = static_cast<uint32_t>(grey) * 0x10101;
    at<float>(s2 + vfx::spr::kScale) = ee_float(at<float>(s2 + vfx::spr::kScale)) * (grow + ee_float(1.0f));
    VfxPS2SpriteGroundRotated3D(ptr32<SVECTOR>(sv.address), ee_float(-20.0f));
}

// The cannon's smoke (the renderer is Vfx2Renderer_Cannon): while the cannon's shape is in the early frames of its
// animation (the frame number at +0x18 of the object's shape is below 8) two puffs a frame start (while fewer than 15
// are in use) at the muzzle with a random puff of velocity, living 25 to 40 frames. They fall (their rise gains three a
// frame) and drift. The effect is sorted at the scenery position.
void Vfx2Handler_Cannon(ptr32<_sVfx_TSFX> mem)
{
    const uint32_t m = mem.address;
    const uint32_t shape = at<uint32_t>(at<uint32_t>(at<uint32_t>(m + 0x10)) + 0x38);
    at<uint16_t>(m + 0x184) = static_cast<uint16_t>(at<uint16_t>(m + 0x184) + 1);
    if (at<int32_t>(shape + 0x18) < 8)
    {
        for (int pass = 0; pass < 2 && at<uint8_t>(m + kRing + 1) < 0xf; ++pass)
        {
            const uint32_t p = m + kParticles + static_cast<uint32_t>(static_cast<int8_t>(at<uint8_t>(m + kRing + 3))) * 0x14;
            at<uint16_t>(p) = 0;
            at<uint16_t>(p + 2) = 0;
            at<uint16_t>(p + 4) = 0;
            at<uint16_t>(p + 8) = static_cast<uint16_t>((vfx::random15() & 0xf) - 7);
            at<uint16_t>(p + 0xa) = static_cast<uint16_t>((vfx::random15() & 0x1f) - 0xf);
            at<uint16_t>(p + 0xc) = static_cast<uint16_t>((vfx::random15() & 0x3f) - 0x46);
            at<uint16_t>(p + 0x10) = static_cast<uint16_t>((vfx::random15() & 0xf) + 0x19);
            at<uint16_t>(p + 0x12) = static_cast<uint16_t>(vfx::random15());
            ringPush(m);
        }
    }
    int32_t index = ringHead(m);
    if (index != at<uint8_t>(m + kRing + 3))
    {
        do
        {
            const uint32_t p = m + kParticles + static_cast<uint32_t>(index * 0x14);
            const uint16_t vy = at<uint16_t>(p + 0xa);
            const uint16_t life = static_cast<uint16_t>(at<uint16_t>(p + 0x10) - 1);
            at<uint16_t>(p + 2) = static_cast<uint16_t>(at<uint16_t>(p + 2) + vy);
            at<uint16_t>(p) = static_cast<uint16_t>(at<uint16_t>(p) + at<uint16_t>(p + 8));
            at<uint16_t>(p + 4) = static_cast<uint16_t>(at<uint16_t>(p + 4) + at<uint16_t>(p + 0xc));
            at<uint16_t>(p + 0xa) = static_cast<uint16_t>(vy + 3);
            at<uint16_t>(p + 0x10) = life;
            if (life == 0)
            {
                const uint8_t head = at<uint8_t>(m + kRing + 2);
                if (index != head)
                    std::memcpy(&at<uint8_t>(p), &at<uint8_t>(m + kParticles + head * 0x14u), 0x14);
                at<uint8_t>(m + kRing + 1) = static_cast<uint8_t>(at<uint8_t>(m + kRing + 1) - 1);
                at<uint8_t>(m + kRing + 2) = static_cast<uint8_t>(at<uint8_t>(m + kRing) & (at<uint8_t>(m + kRing + 2) + 1));
            }
            index = ringNext(m, index);
        } while (index != at<uint8_t>(m + kRing + 3));
    }
    sortAtScenery(m, 0x80);
}

// The glints (sparkles on the road, 128 slots of 0x30 bytes from visFxVarG+0xeb0: a position vector, a life at +0x10 and
// a brightness at +0x12). While the camera has not moved 200 units from where it last started glints (a vector per screen
// at visFxVarG+0x26f0), a dead slot is given a random spot ahead of the camera (sideways by a random amount, 500 up, 4000
// ahead, through the inverse camera matrix) dropped on to the road, and kept if the road there is a material of type 2
// (other than material 10); starting one moves the "last" spot to the camera. Live glints count up, fade in and out
// every 64 frames, dim with the distance from the camera, and die when more than the limit from it (sideways or ahead).
void Vfx3Glint()
{
    GuestFrame frame(0x130);
    constexpr uint32_t kGlints = kVisFx + 0xeb0, kCamera = kVisFx + 0x160, kLastSpot = kVisFx + 0x26f0;
    const uint32_t screen = at<uint32_t>((sym::mainVarG + 0x378u)) * 0x10;
    const ptr32<float> offset = frame.local<float>(0);
    const ptr32<float> normal = frame.local<float>(0x20);
    const ptr32<int16_t> sv = frame.local<int16_t>(0x70);
    const uint32_t material = frame.local<uint8_t>(0x80).address;
    vfx::vu0Sub(offset.address, kCamera, kLastSpot + screen);
    const ee_float near(200.0f);
    bool nearLast = !(near < ee_abs(at<float>(offset.address))) && !(near < ee_abs(at<float>(offset.address + 4))) && !(near < ee_abs(at<float>(offset.address + 8)));
    const uint32_t sp = vfx::sprite();
    at<uint8_t>(sp + vfx::spr::kCode) = 2;
    at<uint16_t>(vfx::sprite() + vfx::spr::kAngle) = 0;
    at<uint32_t>(vfx::sprite() + vfx::spr::kTexture) = vfx::vfxTexture(1);
    for (uint32_t i = 0; i < 0x80; ++i)
    {
        const uint32_t glint = kGlints + i * 0x30;
        if (at<int16_t>(glint + 0x10) == 0)
        {
            if (nearLast)
                continue;
            at<uint16_t>(glint + 0x12) = static_cast<uint16_t>((vfx::random15() & 0x3f) + 0x3c);
            const int32_t r = rand();
            at<float>(offset.address + 4) = 500.0f;
            at<float>(offset.address + 8) = 4000.0f;
            at<float>(offset.address) = ee_itof(r) * ee_float(at<float>(sym::lit4_3278fc)) - ee_float(at<float>(sym::lit4_327900));
            sceVu0ApplyMatrix(ptr32<ee_float>(offset.address), ptr32<ee_float>(kVisFx + 0x170), ptr32<ee_float>(offset.address));
            vfx::vu0Add(offset.address, offset.address, kCamera);
            const bool hit = AColDownCast(ptr32<_sColCellVecPSX>(offset.address), ptr32<_sColCellVecPSX>(glint), ptr32<_sColNormalPSX>(normal.address),
                                          ptr32<uint8>(material));
            nearLast = true;
            if (hit)
            {
                const uint32_t kind = at<uint32_t>((sym::mtlG + 0x4u) + at<uint8_t>(material) * 0x18u);
                if (kind != 2 || at<uint8_t>(material) == 0xa)
                    continue;
                at<int16_t>(glint + 0x10) = 1;
                sceVu0CopyVector(ptr32<ee_float>(kLastSpot + screen), ptr32<ee_float>(kCamera));
            }
            continue;
        }
        vfx::vu0Sub(offset.address, glint, kCamera);
        const ee_float limit(at<float>(sym::lit4_327904));
        const ee_float dx = ee_abs(at<float>(offset.address));
        if (limit < dx)
        {
            at<int16_t>(glint + 0x10) = 0;
            continue;
        }
        const ee_float dz = ee_abs(at<float>(offset.address + 8));
        if (limit < dz)
        {
            at<int16_t>(glint + 0x10) = 0;
            continue;
        }
        const ee_float distance = (dx + dz) * ee_float(at<float>(sym::lit4_327908));
        at<uint16_t>(glint + 0x10) = static_cast<uint16_t>(at<uint16_t>(glint + 0x10) + 1);
        int32_t away = ee_ftoi(distance);
        away = away < 0x100 ? away : 0xff;
        away = 0xff - away;
        sceVu0CopyVector(ptr32<ee_float>(vfx::sprite() + vfx::spr::kPos), ptr32<ee_float>(glint));
        int32_t phase = (at<uint16_t>(glint + 0x10) & 0x3f) - 0x20;
        phase = phase > -1 ? phase : 0;
        phase = phase < 0x11 ? phase << 3 : (0x20 - phase) << 3;
        phase -= away;
        const uint32_t s2 = vfx::sprite();
        at<uint8_t>(s2 + vfx::spr::kFlipX) = static_cast<uint8_t>(i & 1);
        phase = phase > -1 ? phase : 0;
        at<float>(s2 + vfx::spr::kScale) = 15.0f;
        at<uint32_t>(s2 + vfx::spr::kRgb) = static_cast<uint32_t>(phase) * 0x10101;
        sv[1] = 0x1000;
        sv[0] = 0;
        sv[2] = 0;
        VfxPS2SpriteGroundRotated3D(ptr32<SVECTOR>(sv.address), ee_float(-80.0f));
    }
}

} // namespace rc

// Forward declarations of the random-input setups (visfx_fuzz.cpp).
namespace rc::fuzz {
void vfxBatsHandler(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxSciFiBubblesHandler(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxLight(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxButterflies(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxPirateLava(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxCarSplash(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxCannonHandler(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxGlint(R5900Context *ctx, uint64_t &state, uint32_t scratch);
}

RC_DECOMP_FN_SETUP(Vfx2Handler_Bats, rc::fuzz::vfxBatsHandler)
RC_DECOMP_FN_SETUP(Vfx2Handler_SciFiBubbles, rc::fuzz::vfxSciFiBubblesHandler)
RC_DECOMP_FN_SETUP(Vfx2Renderer_LightFlare, rc::fuzz::vfxLight)
RC_DECOMP_FN_SETUP(Vfx2Renderer_LightCandle, rc::fuzz::vfxLight)
RC_DECOMP_FN_SETUP(Vfx2Renderer_Butterflies, rc::fuzz::vfxButterflies)
RC_DECOMP_FN_SETUP(Vfx2Renderer_PirateLava, rc::fuzz::vfxPirateLava)
RC_DECOMP_FN_SETUP(VfxRenderer_CarSplash, rc::fuzz::vfxCarSplash)
RC_DECOMP_FN_SETUP(Vfx2Handler_Cannon, rc::fuzz::vfxCannonHandler)
RC_DECOMP_FN_SETUP(Vfx3Glint, rc::fuzz::vfxGlint)
