// Decompiled from VISFX2.c, continued: the renderers (and two handlers) of the scenery effects that were still
// recompiled: smoke, steam, volcano, fountain, bubbles and the sci-fi and cartoon-train effects. See visfx2.cpp for the
// layout of a scenery effect (`_sVfx_TSFX`) and visfx3.cpp for the ring of particles the renderers walk.

#include "ai_fuzz.h"
#include "ee_math.h"
#include "vifpk.h"
#include "visfx.h"

#include <algorithm>
#include <emmintrin.h>

namespace rc {

ptr32<SVECTOR> VfxCarPositionPoi(ptr32<_sObject> pObj); // ai.cpp
ptr32<MATRIX> VfxCarMatrixPoi(ptr32<_sObject> pObj);      // ai.cpp
uint64_t VfxCarPositionVal(ptr32<_sObject> pObj);        // visfx.cpp
void VfxPutUV0(ee_float u, ee_float v);                   // visfx.cpp
void VfxPutUV1(ee_float u, ee_float v);
void VfxPutUV2(ee_float u, ee_float v);
void VfxPutUV3(ee_float u, ee_float v);
void PS2C_GteMatrixMulVec16(ptr32<_sVec16> pVec);        // psxdefs.cpp
void PS2C_SetGteMatrix(ptr32<_sMat> pMat);
void PS2C_GetVec16GteResult(ptr32<_sVec16> pOut);

namespace {

using vfx::at;
using vfx::ringHead;
using vfx::ringNext;

constexpr uint32_t kLifeSpan = 0x10;
constexpr uint32_t kRing = 0x40;      // the ring header: mask, count, head, tail
constexpr uint32_t kParticles = 0x44; // the first particle

// The scenery instance's position in whole units, as shorts.
struct SceneryPos
{
    int16_t x, y, z;
};

SceneryPos sceneryPosition(uint32_t mem)
{
    const uint32_t tob = at<uint32_t>(mem + 0x10);
    return {static_cast<int16_t>(at<int32_t>(tob + 4) >> 8), static_cast<int16_t>(at<int32_t>(tob + 8) >> 8),
            static_cast<int16_t>(at<int32_t>(tob + 0xc) >> 8)};
}

int32_t atLeastZero(int32_t value) { return value > -1 ? value : 0; }

// The colour the scenery effects fade a particle to: three bytes (red, green, blue) minus the fade, none below zero.
uint32_t fadedColour(int32_t red, int32_t green, int32_t blue)
{
    return static_cast<uint32_t>(atLeastZero(red) + atLeastZero(green) * 0x100 + atLeastZero(blue) * 0x10000);
}

} // namespace

// The monkey smoke: each particle is a grey sprite (colour 0x80808080, texture 21, code 0) at the particle's own
// position, shrinking as its life runs out. (The original is unrolled by two; the effect is the same as one loop.)
void Vfx2Renderer_MonkeySmoke(ptr32<_sVfx_TSFX> mem)
{
    GuestFrame frame(0x80);
    const uint32_t m = mem.address;
    at<uint8_t>(vfx::sprite() + vfx::spr::kCode) = 0;
    at<uint32_t>(vfx::sprite() + vfx::spr::kTexture) = vfx::vfxTexture(0x15);
    for (int32_t index = ringHead(m); index != at<uint8_t>(m + kRing + 3); index = ringNext(m, index))
    {
        const uint32_t p = m + kParticles + static_cast<uint32_t>(index) * 0x14u;
        const uint32_t sp = vfx::sprite();
        at<float>(sp + vfx::spr::kPos) = ee_itof(at<int16_t>(p));
        at<float>(sp + vfx::spr::kPos + 4) = ee_itof(at<int16_t>(p + 2));
        at<uint8_t>(sp + vfx::spr::kFlipX) = 0;
        at<float>(sp + vfx::spr::kPos + 8) = ee_itof(at<int16_t>(p + 4));
        const uint32_t s2 = vfx::sprite();
        at<uint32_t>(s2 + vfx::spr::kRgb) = 0x80808080;
        at<uint16_t>(s2 + vfx::spr::kAngle) = at<uint16_t>(p + 0x12);
        at<float>(s2 + vfx::spr::kScale) = ee_itof(static_cast<int32_t>(0xa0 - (static_cast<uint32_t>(at<uint16_t>(p + 0x10)) * 4 + 0x10)));
        VfxPS2SpriteRotated3D(ee_float(0.0f));
    }
}

// The volcano's smoke as sprites around the scenery: each particle starts in the volcano's colour (three bytes at
// +0x188, the fourth is the base size) and fades as it ages; it grows eight units for each unit of age.
void Vfx2Renderer_BigVolcano(ptr32<_sVfx_TSFX> mem)
{
    GuestFrame frame(0x80);
    const uint32_t m = mem.address;
    const SceneryPos where = sceneryPosition(m);
    at<uint8_t>(vfx::sprite() + vfx::spr::kCode) = 4;
    at<uint32_t>(vfx::sprite() + vfx::spr::kTexture) = vfx::vfxTexture(5);
    for (int32_t index = ringHead(m); index != at<uint8_t>(m + kRing + 3); index = ringNext(m, index))
    {
        const uint32_t p = m + kParticles + static_cast<uint32_t>(index) * 0x14u;
        const uint32_t sp = vfx::sprite();
        const int32_t age = 0x7f - static_cast<int32_t>(at<uint16_t>(p + 0x10));
        at<float>(sp + vfx::spr::kPos) = ee_itof(where.x + at<int16_t>(p));
        at<float>(sp + vfx::spr::kPos + 4) = ee_itof(where.y + at<int16_t>(p + 2));
        at<float>(sp + vfx::spr::kPos + 8) = ee_itof(where.z + at<int16_t>(p + 4));
        const int32_t green = at<uint8_t>(m + 0x189) - age, red = at<uint8_t>(m + 0x188) - age, blue = at<uint8_t>(m + 0x18a) - age;
        at<uint8_t>(sp + vfx::spr::kFlipX) = 0;
        const uint32_t s2 = vfx::sprite();
        at<float>(sp + vfx::spr::kScale) = ee_itof(at<uint8_t>(m + 0x18b) + age * 8);
        at<uint16_t>(s2 + vfx::spr::kAngle) = at<uint16_t>(p + 0x12);
        at<uint32_t>(s2 + vfx::spr::kRgb) = fadedColour(red, green, blue);
        VfxPS2SpriteRotated3D(ee_float(0.0f));
    }
}

// Draws a ring of 0x18-byte particles as the fountain does: the spray around the scenery position (offset by (dx, dy, 0)
// in whole units), each drop in the fountain's colour (three bytes at +0x344) fading with its 32-bit age and growing.
void fountainDrops(uint32_t m, int32_t dx, int32_t dy)
{
    const SceneryPos where = sceneryPosition(m);
    const int16_t x = static_cast<int16_t>(where.x + dx), y = static_cast<int16_t>(where.y + dy), z = where.z;
    at<uint8_t>(vfx::sprite() + vfx::spr::kCode) = 2;
    at<uint32_t>(vfx::sprite() + vfx::spr::kTexture) = vfx::vfxTexture(0xd);
    for (int32_t index = ringHead(m); index != at<uint8_t>(m + kRing + 3); index = ringNext(m, index))
    {
        const uint32_t p = m + kParticles + static_cast<uint32_t>(index) * 0x18u;
        const uint32_t sp = vfx::sprite();
        const int32_t age = 0x50 - at<int32_t>(p + 0x10);
        at<float>(sp + vfx::spr::kPos + 4) = ee_itof(y + (at<int16_t>(p + 2) >> 1));
        at<float>(sp + vfx::spr::kPos) = ee_itof(x + (at<int16_t>(p) >> 1));
        at<float>(sp + vfx::spr::kPos + 8) = ee_itof(z + (at<int16_t>(p + 4) >> 1));
        const int32_t green = at<uint8_t>(m + 0x345) - age, red = at<uint8_t>(m + 0x344) - age * 2, blue = at<uint8_t>(m + 0x346) - age;
        at<uint8_t>(sp + vfx::spr::kFlipX) = 0;
        const uint32_t s2 = vfx::sprite();
        at<float>(s2 + vfx::spr::kScale) = ee_itof(age * 2 + 0x50);
        at<uint16_t>(s2 + vfx::spr::kAngle) = at<uint16_t>(p + 0x14);
        at<uint32_t>(s2 + vfx::spr::kRgb) = fadedColour(red, green, blue);
        VfxPS2SpriteRotated3D(ee_float(0.0f));
    }
}

void Vfx2Renderer_Fountain(ptr32<_sVfx_TSFX> mem)
{
    GuestFrame frame(0x80);
    fountainDrops(mem.address, 0, 0);
}

// Moby Dick's spout is the fountain's drawing, placed 500 units to one side of the scenery and 0x898 above it (as
// shorts: the sums wrap at 16 bits like the original's).
void Vfx2Renderer_MobyDick(ptr32<_sVfx_TSFX> mem)
{
    GuestFrame frame(0x80);
    fountainDrops(mem.address, -500, 0x898);
}

// The pipe steam as sprites around the scenery: tinted with the pipe's colour (three bytes at +0x188), fading and
// growing as it ages (the age is twice the particle's life).
void Vfx2Renderer_PipeSteam(ptr32<_sVfx_TSFX> mem)
{
    GuestFrame frame(0x80);
    const uint32_t m = mem.address;
    const SceneryPos where = sceneryPosition(m);
    at<uint8_t>(vfx::sprite() + vfx::spr::kCode) = 2;
    at<uint32_t>(vfx::sprite() + vfx::spr::kTexture) = vfx::vfxTexture(0xd);
    for (int32_t index = ringHead(m); index != at<uint8_t>(m + kRing + 3); index = ringNext(m, index))
    {
        const uint32_t p = m + kParticles + static_cast<uint32_t>(index) * 0x14u;
        const uint32_t sp = vfx::sprite();
        at<float>(sp + vfx::spr::kPos) = ee_itof(where.x + (at<int16_t>(p) >> 1));
        const int32_t age = static_cast<int32_t>(at<uint16_t>(p + 0x10)) * -2 + 0x50;
        at<float>(sp + vfx::spr::kPos + 4) = ee_itof(where.y + (at<int16_t>(p + 2) >> 1));
        at<float>(sp + vfx::spr::kPos + 8) = ee_itof(where.z + (at<int16_t>(p + 4) >> 1));
        const int32_t green = at<uint8_t>(m + 0x189) - age, red = at<uint8_t>(m + 0x188) - age, blue = at<uint8_t>(m + 0x18a) - age;
        at<uint8_t>(sp + vfx::spr::kFlipX) = 0;
        const uint32_t s2 = vfx::sprite();
        at<uint16_t>(s2 + vfx::spr::kAngle) = at<uint16_t>(p + 0x12);
        at<uint32_t>(s2 + vfx::spr::kRgb) = fadedColour(red, green, blue);
        at<float>(s2 + vfx::spr::kScale) = ee_itof(age * 2 + 0x14);
        VfxPS2SpriteRotated3D(ee_float(0.0f));
    }
}

// The sci-fi bubbles: bubbles in the scenery's colour (+0x188), the red reduced further as they age, drifting up and
// wobbling in size (a random 8 to 23 added to half the age). Drawn 80 units towards the camera.
void Vfx2Renderer_SciFiBubbles(ptr32<_sVfx_TSFX> mem)
{
    GuestFrame frame(0xa0);
    const uint32_t m = mem.address;
    const SceneryPos where = sceneryPosition(m);
    at<uint8_t>(vfx::sprite() + vfx::spr::kCode) = 2;
    at<uint32_t>(vfx::sprite() + vfx::spr::kTexture) = vfx::vfxTexture(7);
    for (int32_t index = ringHead(m); index != at<uint8_t>(m + kRing + 3); index = ringNext(m, index))
    {
        const uint32_t p = m + kParticles + static_cast<uint32_t>(index) * 0x14u;
        const uint32_t sp = vfx::sprite();
        const int32_t age = 0x50 - static_cast<int32_t>(at<uint16_t>(p + 0x10));
        at<float>(sp + vfx::spr::kPos + 4) = ee_itof(where.y + (at<int16_t>(p + 2) >> 2));
        at<float>(sp + vfx::spr::kPos + 8) = ee_itof(where.z + (at<int16_t>(p + 4) >> 2));
        at<float>(sp + vfx::spr::kPos) = ee_itof(where.x + (at<int16_t>(p) >> 2));
        const int32_t green = at<uint8_t>(m + 0x189) - age, red = (at<uint8_t>(m + 0x188) - age) - (age >> 1), blue = at<uint8_t>(m + 0x18a) - age;
        at<uint8_t>(sp + vfx::spr::kFlipX) = 0;
        at<uint32_t>(vfx::sprite() + vfx::spr::kRgb) = fadedColour(red, green, blue);
        const int32_t wobble = vfx::random15() & 0xf;
        const uint32_t s2 = vfx::sprite();
        at<uint16_t>(s2 + vfx::spr::kAngle) = 0;
        at<float>(s2 + vfx::spr::kScale) = ee_itof((age >> 1) + wobble + 8);
        VfxPS2SpriteRotated3D(ee_float(-80.0f));
    }
}


// The sci-fi steam: bubbles of vapour tinted by the scenery's colour (+0x188), the red falling away faster than the
// others as it ages, growing four units for each unit of age.
void Vfx2Renderer_SciFiSteam(ptr32<_sVfx_TSFX> mem)
{
    GuestFrame frame(0x90);
    const uint32_t m = mem.address;
    const SceneryPos where = sceneryPosition(m);
    at<uint8_t>(vfx::sprite() + vfx::spr::kCode) = 2;
    at<uint32_t>(vfx::sprite() + vfx::spr::kTexture) = vfx::vfxTexture(0xd);
    for (int32_t index = ringHead(m); index != at<uint8_t>(m + kRing + 3); index = ringNext(m, index))
    {
        const uint32_t p = m + kParticles + static_cast<uint32_t>(index) * 0x14u;
        const uint32_t sp = vfx::sprite();
        at<float>(sp + vfx::spr::kPos) = ee_itof(where.x + (at<int16_t>(p) >> 1));
        const int32_t age = static_cast<int32_t>(at<uint16_t>(p + 0x10)) * -2 + 0x50;
        at<float>(sp + vfx::spr::kPos + 4) = ee_itof(where.y + (at<int16_t>(p + 2) >> 1));
        at<float>(sp + vfx::spr::kPos + 8) = ee_itof(where.z + (at<int16_t>(p + 4) >> 1));
        const int32_t green = at<uint8_t>(m + 0x189) - age, red = (at<uint8_t>(m + 0x188) - age) - (age >> 1), blue = at<uint8_t>(m + 0x18a) - age;
        at<uint8_t>(sp + vfx::spr::kFlipX) = 0;
        const uint32_t s2 = vfx::sprite();
        at<uint16_t>(s2 + vfx::spr::kAngle) = at<uint16_t>(p + 0x12);
        at<uint32_t>(s2 + vfx::spr::kRgb) = fadedColour(red, green, blue);
        at<float>(s2 + vfx::spr::kScale) = ee_itof(age * 4 + 0x28);
        VfxPS2SpriteRotated3D(ee_float(0.0f));
    }
}

// The smoking scenery's puffs: like the volcano's smoke (texture 5, in the scenery's colour at +0x188), but growing
// two units for each unit of age from a base size (+0x18b), and spreading a quarter as far as the particle's position.
void Vfx2Renderer_Smoking(ptr32<_sVfx_TSFX> mem)
{
    GuestFrame frame(0x80);
    const uint32_t m = mem.address;
    const SceneryPos where = sceneryPosition(m);
    at<uint8_t>(vfx::sprite() + vfx::spr::kCode) = 4;
    at<uint32_t>(vfx::sprite() + vfx::spr::kTexture) = vfx::vfxTexture(5);
    for (int32_t index = ringHead(m); index != at<uint8_t>(m + kRing + 3); index = ringNext(m, index))
    {
        const uint32_t p = m + kParticles + static_cast<uint32_t>(index) * 0x14u;
        const uint32_t sp = vfx::sprite();
        const int32_t age = 0x7f - static_cast<int32_t>(at<uint16_t>(p + 0x10));
        at<float>(sp + vfx::spr::kPos) = ee_itof(where.x + (at<int16_t>(p) >> 2));
        at<float>(sp + vfx::spr::kPos + 4) = ee_itof(where.y + (at<int16_t>(p + 2) >> 2));
        at<float>(sp + vfx::spr::kPos + 8) = ee_itof(where.z + (at<int16_t>(p + 4) >> 2));
        const int32_t green = at<uint8_t>(m + 0x189) - age, red = at<uint8_t>(m + 0x188) - age, blue = at<uint8_t>(m + 0x18a) - age;
        at<uint8_t>(sp + vfx::spr::kFlipX) = 0;
        const uint32_t s2 = vfx::sprite();
        at<float>(sp + vfx::spr::kScale) = ee_itof(at<uint8_t>(m + 0x18b) + age * 2);
        at<uint16_t>(s2 + vfx::spr::kAngle) = at<uint16_t>(p + 0x12);
        at<uint32_t>(s2 + vfx::spr::kRgb) = fadedColour(red, green, blue);
        VfxPS2SpriteRotated3D(ee_float(0.0f));
    }
}

// The gargoyle's water: sprites that fade from white to blue (green falls twice as fast, red four times) as they age,
// at the spout 0x226 units to one side of the scenery (as a short, so it wraps like the original's), each a size of
// 28 plus four times its remaining life.
void Vfx2Renderer_Gargoyle(ptr32<_sVfx_TSFX> mem)
{
    GuestFrame frame(0x80);
    const uint32_t m = mem.address;
    const SceneryPos where = sceneryPosition(m);
    const int16_t x = static_cast<int16_t>(where.x + 0x226);
    at<uint8_t>(vfx::sprite() + vfx::spr::kCode) = 2;
    at<uint32_t>(vfx::sprite() + vfx::spr::kTexture) = vfx::vfxTexture(0);
    for (int32_t index = ringHead(m); index != at<uint8_t>(m + kRing + 3); index = ringNext(m, index))
    {
        const uint32_t p = m + kParticles + static_cast<uint32_t>(index) * 0x14u;
        const uint32_t sp = vfx::sprite();
        const uint32_t life = at<uint16_t>(p + 0x10);
        const int32_t fade = atLeastZero(static_cast<int32_t>(life) * -2 + 100);
        const int32_t green = atLeastZero(fade * -2 + 100), blue = atLeastZero(fade * -4 + 100);
        at<uint8_t>(sp + vfx::spr::kFlipX) = 0;
        at<float>(sp + vfx::spr::kPos) = ee_itof(x + (at<int16_t>(p) >> 1));
        at<float>(sp + vfx::spr::kPos + 4) = ee_itof(where.y + (at<int16_t>(p + 2) >> 1));
        at<float>(sp + vfx::spr::kPos + 8) = ee_itof(where.z + (at<int16_t>(p + 4) >> 1));
        const uint32_t s2 = vfx::sprite();
        at<uint16_t>(s2 + vfx::spr::kAngle) = at<uint16_t>(p + 0x12);
        at<uint32_t>(s2 + vfx::spr::kRgb) = static_cast<uint32_t>((100 - fade) + (green << 8) + (blue << 16));
        at<float>(s2 + vfx::spr::kScale) = ee_itof((static_cast<int32_t>(life) * -2 + 0x50) * 2) + ee_float(28.0f);
        VfxPS2SpriteRotated3D(ee_float(0.0f));
    }
}

// The right-the-car effect's dust: sprites around the car's position (shifted a thirty-second), pale as they age; one in
// four also gets a small dark glint over it.
void Vfx2Renderer_CarRight(ptr32<_sVfx_Object> mem)
{
    GuestFrame frame(0x80);
    const uint32_t m = mem.address;
    const uint32_t pos = VfxCarPositionPoi(ptr32<_sObject>(at<uint32_t>(m + 0x1c4))).address;
    at<uint8_t>(vfx::sprite() + vfx::spr::kFlipX) = 0;
    at<uint8_t>(vfx::sprite() + vfx::spr::kCode) = 2;
    at<uint32_t>(vfx::sprite() + vfx::spr::kTexture) = vfx::vfxTexture(0xd);
    for (uint32_t index = at<uint8_t>(m + kRing + 2); index != at<uint8_t>(m + kRing + 3);)
    {
        const uint32_t p = m + kParticles + index * 0x18;
        const uint32_t sp = vfx::sprite();
        at<float>(sp + vfx::spr::kPos) = ee_itof(at<int16_t>(pos) + (at<int16_t>(p) >> 5));
        const int32_t fade = 0x3c - static_cast<int32_t>(at<uint16_t>(p + 0x14)) * 4;
        const int32_t red = atLeastZero(0x53 - fade * 2), blue = atLeastZero(0x49 - fade);
        at<float>(sp + vfx::spr::kPos + 4) = ee_itof(at<int16_t>(pos + 2) + (at<int16_t>(p + 2) >> 5));
        at<float>(sp + vfx::spr::kPos + 8) = ee_itof(at<int16_t>(pos + 4) + (at<int16_t>(p + 4) >> 5));
        at<uint16_t>(sp + vfx::spr::kAngle) = at<uint16_t>(p + 0x10);
        at<uint32_t>(sp + vfx::spr::kRgb) = static_cast<uint32_t>(red + (red << 8) + (blue << 16));
        at<float>(sp + vfx::spr::kScale) = ee_itof(at<uint8_t>(p + 0x16) + 2);
        VfxPS2SpriteRotated3D(ee_float(0.0f));
        if ((vfx::random15() & 3) == 0)
        {
            const uint32_t s2 = vfx::sprite();
            at<uint32_t>(s2 + vfx::spr::kTexture) = vfx::vfxTexture(0xe);
            at<uint32_t>(s2 + vfx::spr::kRgb) = 0x40;
            at<float>(s2 + vfx::spr::kScale) = ee_itof(at<uint8_t>(p + 0x16) >> 1);
            VfxPS2SpriteRotated3D(ee_float(0.0f));
            at<uint32_t>(vfx::sprite() + vfx::spr::kTexture) = vfx::vfxTexture(0xd);
        }
        index = (index + 1) & at<uint8_t>(m + kRing);
    }
}

// The sci-fi rocket's exhaust, shared by the two versions: each flame particle is two sprites, first the flame (colour
// fading from white through yellow to red as it ages, as big as eight times the particle's size byte) and, once the
// particle is 12 or more old, a grey glow over it. The first sprite is drawn `depth` units back. `position` is the rocket's
// place (three shorts) and `dropY` how far below that the exhaust starts (0xe10 and 0xed8 for the two versions).
void rocketExhaust(uint32_t m, const SceneryPos &position, int32_t dropY, float depth)
{
    at<uint8_t>(vfx::sprite() + vfx::spr::kCode) = 2;
    at<uint32_t>(vfx::sprite() + vfx::spr::kTexture) = vfx::vfxTexture(0);
    for (int32_t index = ringHead(m); index != at<uint8_t>(m + kRing + 3); index = ringNext(m, index))
    {
        const uint32_t p = m + kParticles + static_cast<uint32_t>(index) * 0x18u;
        const uint32_t sp = vfx::sprite();
        const int32_t age = static_cast<int32_t>(at<uint16_t>(p + 0x14)) * -8 + 0x80;
        at<float>(sp + vfx::spr::kPos) = ee_itof(position.x + at<int16_t>(p));
        const int32_t green = atLeastZero(0x87 - (age + (age >> 2)));
        const int32_t blue = atLeastZero(age * -2 + 0x87);
        at<float>(sp + vfx::spr::kPos + 4) = ee_itof(position.y + at<int16_t>(p + 2) - dropY);
        at<float>(sp + vfx::spr::kPos + 8) = ee_itof(position.z + at<int16_t>(p + 4));
        at<uint8_t>(sp + vfx::spr::kFlipX) = 0;
        const uint32_t s2 = vfx::sprite();
        at<uint32_t>(s2 + vfx::spr::kRgb) = static_cast<uint32_t>((0x87 - age) + (green << 8) + (blue << 16));
        at<uint16_t>(s2 + vfx::spr::kAngle) = at<uint16_t>(p + 0x10);
        at<float>(sp + vfx::spr::kScale) = ee_itof(static_cast<int32_t>(at<uint8_t>(p + 0x16)) << 3);
        VfxPS2SpriteRotated3D(ee_float(depth));
        if (!(at<uint16_t>(p + 0x14) < 0xc))
        {
            const uint32_t s3 = vfx::sprite();
            at<uint32_t>(s3 + vfx::spr::kRgb) = static_cast<uint32_t>(blue * 0x10101);
            at<uint32_t>(s3 + vfx::spr::kTexture) = vfx::vfxTexture(1);
            VfxPS2SpriteRotated3D(ee_float(0.0f));
            at<uint32_t>(vfx::sprite() + vfx::spr::kTexture) = vfx::vfxTexture(0);
        }
    }
}

// The sci-fi rocket: the exhaust starts at the rocket's own position, which comes from the shape's data: the scenery's
// object has a shape, whose third vector's first entry is a vector of floats (a point in the model).
void Vfx2Renderer_ScifiRocket(ptr32<_sVfx_TSFX> mem)
{
    GuestFrame frame(0xb0);
    const uint32_t m = mem.address;
    const uint32_t tob = at<uint32_t>(at<uint32_t>(m + 0x10));
    const uint32_t vector = at<uint32_t>(at<uint32_t>(at<uint32_t>(tob + 0x3c) + 8));
    at<uint32_t>(vfx::sprite() + vfx::spr::kTexture) = vfx::vfxTexture(0);
    const SceneryPos position = {static_cast<int16_t>(ee_ftoi(at<float>(vector))), static_cast<int16_t>(ee_ftoi(at<float>(vector + 4))),
                                 static_cast<int16_t>(ee_ftoi(at<float>(vector + 8)))};
    rocketExhaust(m, position, 0xe10, 0.0f);
}

// The older version of the same effect: placed at the scenery's own position and drawn 20 units back.
void Vfx2Renderer_ScifiRocketOld(ptr32<_sVfx_TSFX> mem)
{
    GuestFrame frame(0xb0);
    const uint32_t m = mem.address;
    rocketExhaust(m, sceneryPosition(m), 0xed8, 20.0f);
}


// The cartoon train's chimney smoke: a counter at +0x194 goes up each frame and every fourth frame (while the ring has
// room) a puff starts at the chimney with the train's own movement (+0x18c, three shorts: how far the scenery moved
// since last time, so the smoke trails behind a moving train) doubled, plus a random spread. The scenery's last position
// (+0x184) is brought up to date when it has moved; the sort position is the scenery's.
void Vfx2Handler_CartoonTrainSmoke(ptr32<_sVfx_TSFX> mem)
{
    const uint32_t m = mem.address;
    const SceneryPos where = sceneryPosition(m);
    const uint32_t count = at<uint32_t>(m + 0x194) + 1;
    at<uint32_t>(m + 0x194) = count;
    if ((count & 4) != 0 && at<uint8_t>(m + kRing + 1) < 0xf)
    {
        const uint32_t p = m + kParticles + static_cast<uint32_t>(static_cast<int8_t>(at<uint8_t>(m + kRing + 3))) * 0x14u;
        at<uint16_t>(p) = 0;
        at<uint16_t>(p + 2) = 0;
        at<uint16_t>(p + 4) = 0;
        at<uint16_t>(p + 8) = static_cast<uint16_t>((vfx::random15() & 0x1f) + at<uint16_t>(m + 0x18c) * 2 - 0x10);
        at<uint16_t>(p + 0xa) = static_cast<uint16_t>((vfx::random15() & 0x1f) + at<uint16_t>(m + 0x18e) * 2 + 0x28);
        at<uint16_t>(p + 0x10) = 0x1f;
        at<uint16_t>(p + 0xc) = static_cast<uint16_t>((vfx::random15() & 0x1f) + at<uint16_t>(m + 0x190) * 2 - 0x10);
        at<uint16_t>(p + 0x12) = static_cast<uint16_t>(vfx::random15());
        vfx::ringPush(m);
    }
    VfxSimpleParticleMover(ptr32<_sVfx_PartHeader>(m + kRing), ptr32<_VFX_SIMPLEPARTICLE>(m + kParticles));
    if (at<int16_t>(m + 0x184) != where.x || at<int16_t>(m + 0x186) != where.y || at<int16_t>(m + 0x188) != where.z)
    {
        const uint16_t ox = at<uint16_t>(m + 0x184), oy = at<uint16_t>(m + 0x186), oz = at<uint16_t>(m + 0x188);
        at<uint16_t>(m + 0x18c) = static_cast<uint16_t>(ox - static_cast<uint16_t>(where.x));
        at<uint16_t>(m + 0x18e) = static_cast<uint16_t>(oy - static_cast<uint16_t>(where.y));
        at<uint16_t>(m + 0x190) = static_cast<uint16_t>(oz - static_cast<uint16_t>(where.z));
        at<int16_t>(m + 0x184) = where.x;
        at<int16_t>(m + 0x186) = where.y;
        at<int16_t>(m + 0x188) = where.z;
    }
    vfx::sortPosFromInts(m, where.x, where.y, where.z, 0x70, 0x10);
}

// The chimney smoke as sprites at the chimney: the scenery's position plus a point 0x442 up and 0x8ca forward in the
// scenery's own frame (turned by its matrix), then each puff offset from that, greying as it ages and shrinking.
void Vfx2Renderer_CartoonTrainSmoke(ptr32<_sVfx_TSFX> mem)
{
    GuestFrame frame(0x90);
    const uint32_t m = mem.address;
    const uint32_t tob = at<uint32_t>(m + 0x10);
    const SceneryPos where = sceneryPosition(m);
    const uint32_t offset = frame.local<uint8>(0x10).address; // svOffset
    at<int16_t>(offset) = 0;
    at<int16_t>(offset + 2) = 0x442;
    at<int16_t>(offset + 4) = 0x8ca;
    PS2C_SetGteMatrix(ptr32<_sMat>(tob + 0x18));
    PS2C_GteMatrixMulVec16(ptr32<_sVec16>(offset));
    PS2C_GetVec16GteResult(ptr32<_sVec16>(offset));
    const int16_t x = static_cast<int16_t>(where.x + at<int16_t>(offset));
    const int16_t y = static_cast<int16_t>(where.y + at<int16_t>(offset + 2));
    const int16_t z = static_cast<int16_t>(where.z + at<int16_t>(offset + 4));
    at<uint8_t>(vfx::sprite() + vfx::spr::kCode) = 2;
    at<uint32_t>(vfx::sprite() + vfx::spr::kTexture) = vfx::vfxTexture(0xd);
    for (int32_t index = ringHead(m); index != at<uint8_t>(m + kRing + 3); index = ringNext(m, index))
    {
        const uint32_t p = m + kParticles + static_cast<uint32_t>(index) * 0x14u;
        const uint32_t sp = vfx::sprite();
        const uint32_t life = at<uint16_t>(p + 0x10);
        at<uint8_t>(sp + vfx::spr::kFlipX) = 0;
        at<float>(sp + vfx::spr::kPos) = ee_itof(x + (at<int16_t>(p) >> 1));
        at<float>(sp + vfx::spr::kPos + 4) = ee_itof(y + (at<int16_t>(p + 2) >> 1));
        at<float>(sp + vfx::spr::kPos + 8) = ee_itof(z + (at<int16_t>(p + 4) >> 1));
        at<uint16_t>(sp + vfx::spr::kAngle) = at<uint16_t>(p + 0x12);
        at<uint32_t>(sp + vfx::spr::kRgb) = life * 0x40004 + life * 0x400;
        at<float>(sp + vfx::spr::kScale) = ee_itof(static_cast<int32_t>(life * static_cast<uint32_t>(-8) + 0x1c2));
        VfxPS2SpriteRotated3D(ee_float(0.0f));
    }
}


// The "finish position" text effect's puffs of dust (a number rising from the car after the race): on its first frame
// (life 0x43f) it takes its position from the car, nudged half a matrix column up, with an upward speed of 0x280 that
// it keeps adding to the position, losing 0x10 a frame (and holding position for the first 0x27 frames, where the speed
// is not applied). Each frame it adds a puff with random small velocity, runs the particles and sorts at the car.
void Vfx2Handler_FinishPosition(ptr32<_sVfx_Object> mem)
{
    const uint32_t m = mem.address;
    const ptr32<_sObject> car(at<uint32_t>(m + 0x44));
    if (at<uint16_t>(m + kLifeSpan) == 0x43f)
    {
        at<uint16_t>(m + 0x198) = 0;
        at<uint16_t>(m + 0x19a) = 0;
        at<uint16_t>(m + 0x19c) = 0;
        at<uint16_t>(m + 0x1a0) = 0xfccc;
        at<uint16_t>(m + 0x1a2) = 0x280;
        at<uint64_t>(m + 0x188) = VfxCarPositionVal(car); // x, y, z and the shared vector's fourth short
        const uint32_t matrix = VfxCarMatrixPoi(car).address;
        at<uint16_t>(m + 0x188) = static_cast<uint16_t>(at<uint16_t>(m + 0x188) + (at<int16_t>(matrix + 4) >> 1));
        at<uint16_t>(m + 0x18a) = static_cast<uint16_t>(at<uint16_t>(m + 0x18a) + 0x15e + (at<int16_t>(matrix + 0xa) >> 1));
        at<uint16_t>(m + 0x18c) = static_cast<uint16_t>(at<uint16_t>(m + 0x18c) + (at<int16_t>(matrix + 0x10) >> 1));
    }
    at<uint16_t>(m + 0x188) = static_cast<uint16_t>(at<uint16_t>(m + 0x188) + at<uint16_t>(m + 0x198));
    at<uint16_t>(m + 0x18a) = static_cast<uint16_t>(at<uint16_t>(m + 0x18a) + at<uint16_t>(m + 0x19a));
    at<uint16_t>(m + 0x18c) = static_cast<uint16_t>(at<uint16_t>(m + 0x18c) + at<uint16_t>(m + 0x19c));
    vfx::sortPosFromInts(m, at<int16_t>(m + 0x188), at<int16_t>(m + 0x18a), at<int16_t>(m + 0x18c), 0x70 + 0x20);
    const uint32_t p = m + 0x48 + at<uint8_t>(m + kRing + 3) * 0x14u;
    at<uint16_t>(p) = static_cast<uint16_t>((vfx::random15() & 0x7f) - 0x40);
    at<uint16_t>(p + 2) = static_cast<uint16_t>((vfx::random15() & 0x7f) - 0x40);
    at<uint16_t>(p + 4) = static_cast<uint16_t>((vfx::random15() & 0x7f) - 0x40);
    at<uint16_t>(p + 8) = static_cast<uint16_t>((vfx::random15() & 0x7f) - 0x3f);
    at<uint16_t>(p + 0xa) = static_cast<uint16_t>((vfx::random15() & 0x7f) - 0x3f);
    at<uint16_t>(p + 0xc) = static_cast<uint16_t>((vfx::random15() & 0x7f) - 0x3f);
    at<uint16_t>(p + 0x10) = 0xf;
    vfx::ringPush(m);
    uint16_t speed;
    if (at<uint16_t>(m + kLifeSpan) < 0x418)
    {
        at<uint16_t>(m + 0x1a0) = 0;
        speed = at<uint16_t>(m + 0x1a2);
    }
    else
    {
        at<uint16_t>(m + 0x1a0) = static_cast<uint16_t>(at<uint16_t>(m + 0x1a0) + at<uint16_t>(m + 0x1a2));
        if (at<int16_t>(m + 0x1a2) < 0)
        {
            at<uint16_t>(m + 0x1a2) = 0;
            speed = 0;
        }
        else
            speed = at<uint16_t>(m + 0x1a2);
    }
    at<uint16_t>(m + 0x1a2) = static_cast<uint16_t>(speed - 0x10);
    VfxSimpleParticleMover(ptr32<_sVfx_PartHeader>(m + kRing), ptr32<_VFX_SIMPLEPARTICLE>(m + 0x48));
    vfx::sortPosFromCar(m, car, 0x70 + 0x20);
    at<uint16_t>(m + kLifeSpan) = static_cast<uint16_t>(at<uint16_t>(m + kLifeSpan) - 1);
}


namespace {

// sceneVu0ScaleVectorXYZ: the first three lanes of src times a scalar, the fourth lane copied (the VU0 multiplies only
// x, y and z of the register it loaded from src and stores all four lanes).
void scaleVectorXYZ(uint32_t dst, uint32_t src, ee_float scale)
{
    const __m128 v = _mm_loadu_ps(&at<float>(src & ~0xFu));
    const __m128 product = PS2_VMUL(v, _mm_set1_ps(scale.v));
    const __m128 xyzFromProduct = _mm_castsi128_ps(_mm_set_epi32(0, -1, -1, -1));
    _mm_storeu_ps(&at<float>(dst & ~0xFu), _mm_or_ps(_mm_and_ps(xyzFromProduct, product), _mm_andnot_ps(xyzFromProduct, v)));
}

} // namespace

// The skull wave: a ring of 0x30-byte points around a centre (the three shorts at +0x654), drawn as a band of quads
// between neighbouring points: first a quad from the point (and its neighbour) out to a scaled-up copy, faded from the
// life-dependent colour at the inside to black at the outside, then two more quads the same width stepped 220 units
// up and 460 units down, which make the wave's sides. The texture scrolls along the band by the effect's life.
// The neighbour is the next point modulo the ring's mask (a division, as in the original: a mask of zero traps there).
void Vfx2Renderer_SkullWave(ptr32<_sVfx_Object> mem)
{
    GuestFrame frame(0xe0);
    const uint32_t m = mem.address;
    const uint32_t centre = frame.local<float>(0).address; // vuCentre; its fourth lane is a word of the stack nobody wrote
    at<float>(centre) = ee_itof(at<int16_t>(m + 0x654));
    at<float>(centre + 4) = ee_itof(at<int16_t>(m + 0x656));
    at<float>(centre + 8) = ee_itof(at<int16_t>(m + 0x658));
    const uint32_t life = at<uint16_t>(m + kLifeSpan);
    const uint32_t colour = life * 4 + (life * 4 << 16);
    ee_float u = ee_itof(static_cast<int32_t>(life)) * ee_float(at<float>(0x3278acu));
    RdrPolyTexture(0, ptr32<_sGfxTexture>(vfx::vfxTexture(0x10)), 0x12);
    uint32_t index = at<uint8_t>(m + kRing + 2);
    if (index != at<uint8_t>(m + kRing + 3))
    {
        const ee_float outer = at<float>(0x3278b0u), inner = at<float>(0x3278b4u), step = at<float>(0x3278b8u);
        do
        {
            const uint32_t quad = G(visFxVarG).pGT4.address;
            const uint32_t here = m + 0x50 + index * 0x30;
            const uint32_t mask = at<uint8_t>(m + kRing);
            ++index;
            vfx::vu0Add(G(visFxVarG).pGT4.address, centre, here);
            if (mask == 0)
                __builtin_trap(); // the original's divide traps
            const uint32_t next = m + 0x50 + (index % mask) * 0x30;
            vfx::vu0Add(quad + 0x10, centre, next);
            scaleVectorXYZ(quad + 0x20, here, outer);
            vfx::vu0Add(quad + 0x20, centre, quad + 0x20);
            scaleVectorXYZ(quad + 0x30, next, outer);
            vfx::vu0Add(quad + 0x30, centre, quad + 0x30);
            const ee_float uEnd = u + step;
            VfxPutUV0(u, 0.0f);
            VfxPutUV1(uEnd, 0.0f);
            VfxPutUV2(u, 1.0f);
            VfxPutUV3(uEnd, 1.0f);
            u = uEnd;
            VfxPutGT4rgba0(colour);
            VfxPutGT4rgba1(colour);
            VfxPutGT4rgba2(0);
            VfxPutGT4rgba3(0);
            RdrPoly(0, G(visFxVarG).pGT4);
            scaleVectorXYZ(quad + 0x20, here, inner);
            vfx::vu0Add(quad + 0x20, centre, quad + 0x20);
            scaleVectorXYZ(quad + 0x30, next, inner);
            vfx::vu0Add(quad + 0x30, centre, quad + 0x30);
            at<float>(quad + 0x24) = ee_float(at<float>(quad + 0x24)) + ee_float(220.0f);
            at<float>(quad + 0x34) = ee_float(at<float>(quad + 0x34)) + ee_float(220.0f);
            RdrPoly(0, G(visFxVarG).pGT4);
            at<float>(quad + 0x24) = ee_float(at<float>(quad + 0x24)) - ee_float(460.0f);
            at<float>(quad + 0x34) = ee_float(at<float>(quad + 0x34)) - ee_float(460.0f);
            RdrPoly(0, G(visFxVarG).pGT4);
            index &= at<uint8_t>(m + kRing);
        } while (index != at<uint8_t>(m + kRing + 3));
    }
    VfxPutUV0(1.0f, 0.0f);
    VfxPutUV1(0.0f, 0.0f);
    VfxPutUV2(1.0f, 1.0f);
    VfxPutUV3(0.0f, 1.0f);
}

} // namespace rc

namespace rc::fuzz {
void vfxMonkeySmokeRenderer(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxBigVolcanoRenderer(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxFountainRenderer(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxMobyDickRenderer(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxPipeSteamRenderer(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxSciFiBubblesRenderer(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxSciFiSteamRenderer(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxSmokingRenderer(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxGargoyleRenderer(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxCarRightRenderer(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxScifiRocketRenderer(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxScifiRocketOldRenderer(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxCartoonTrainHandler(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxCartoonTrainRenderer(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxFinishPosition(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxSkullWave(R5900Context *ctx, uint64_t &state, uint32_t scratch);
}

RC_DECOMP_FN_SETUP(Vfx2Renderer_MonkeySmoke, 0x176278, rc::fuzz::vfxMonkeySmokeRenderer)
RC_DECOMP_FN_SETUP(Vfx2Renderer_BigVolcano, 0x1765a0, rc::fuzz::vfxBigVolcanoRenderer)
RC_DECOMP_FN_SETUP(Vfx2Renderer_Fountain, 0x173078, rc::fuzz::vfxFountainRenderer)
RC_DECOMP_FN_SETUP(Vfx2Renderer_MobyDick, 0x178b10, rc::fuzz::vfxMobyDickRenderer)
RC_DECOMP_FN_SETUP(Vfx2Renderer_PipeSteam, 0x16f038, rc::fuzz::vfxPipeSteamRenderer)
RC_DECOMP_FN_SETUP(Vfx2Renderer_SciFiBubbles, 0x175f68, rc::fuzz::vfxSciFiBubblesRenderer)
RC_DECOMP_FN_SETUP(Vfx2Renderer_SciFiSteam, 0x172890, rc::fuzz::vfxSciFiSteamRenderer)
RC_DECOMP_FN_SETUP(Vfx2Renderer_Smoking, 0x16fb48, rc::fuzz::vfxSmokingRenderer)
RC_DECOMP_FN_SETUP(Vfx2Renderer_Gargoyle, 0x175a68, rc::fuzz::vfxGargoyleRenderer)
RC_DECOMP_FN_SETUP(Vfx2Renderer_CarRight, 0x173a10, rc::fuzz::vfxCarRightRenderer)
RC_DECOMP_FN_SETUP(Vfx2Renderer_ScifiRocket, 0x1733c0, rc::fuzz::vfxScifiRocketRenderer)
RC_DECOMP_FN_SETUP(Vfx2Renderer_ScifiRocketOld, 0x173618, rc::fuzz::vfxScifiRocketOldRenderer)
RC_DECOMP_FN_SETUP(Vfx2Handler_CartoonTrainSmoke, 0x16dd30, rc::fuzz::vfxCartoonTrainHandler)
RC_DECOMP_FN_SETUP(Vfx2Renderer_CartoonTrainSmoke, 0x16df28, rc::fuzz::vfxCartoonTrainRenderer)
RC_DECOMP_FN_SETUP(Vfx2Handler_FinishPosition, 0x173c20, rc::fuzz::vfxFinishPosition)
RC_DECOMP_FN_SETUP(Vfx2Renderer_SkullWave, 0x170640, rc::fuzz::vfxSkullWave)
