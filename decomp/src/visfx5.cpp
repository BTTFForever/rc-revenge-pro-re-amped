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
uint64_t VfxCarVelocityVal(ptr32<_sObject> pObj);         // visfx.cpp
void VfxCarSortPos(ptr32<_sObject> pObj, ptr32<_sVfx_Object> fx);
void AINodeRouteFinder(ptr32<_sObject> pObj, int32 iDistance, int32 count); // ai.cpp
void PS2C_GteMatrixMulVec16(ptr32<_sVec16> pVec);        // psxdefs.cpp
void PS2C_SetGteMatrix(ptr32<_sMat> pMat);
void PS2C_GetVec16GteResult(ptr32<_sVec16> pOut);

namespace {

using vfx::at;
using vfx::ringHead;
using vfx::ringNext;
using vfx::ringPush;
using vfx::sortAtScenery;

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


// ---------------------------------------------------------------------------------------------------------------------
// The fifth batch: the burner, lava, route finder, clock, cannon, fire, feather and swarm effects of the scenery and the
// cars.

// Four glowing sprites at fixed offsets from a UFO's burner: the offsets are turned by the car's matrix (through the GTE
// stand-ins) and added to its position; the brightness swings with the effect's counter (0 to 0x40 and back) plus a
// random flicker.
void Vfx2Renderer_UFOBurner(ptr32<_sVfx_Object> mem)
{
    GuestFrame frame(0xa0);
    const uint32_t m = mem.address;
    const ptr32<_sObject> car(at<uint32_t>(m + 0x650));
    if (car->sDontRenderMe != 0 && G(mainVarG).level == 0x62)
        return;
    if (!(VfxHasCarBeenRendered(car) < 0x157c))
        return;
    const uint64_t position = VfxCarPositionVal(car);
    const ptr32<MATRIX> matrix = VfxCarMatrixPoi(car);
    const int16_t px = static_cast<int16_t>(position), py = static_cast<int16_t>(position >> 16), pz = static_cast<int16_t>(position >> 32);
    uint32_t level = at<uint32_t>(m + 0x654) & 0x7f;
    if (level > 0x40)
        level = 0x80 - level;
    const int32_t flicker = ((vfx::random15() + 0x28) & 0x3f);
    const uint32_t sp = vfx::sprite();
    const uint32_t shade = level + flicker;
    at<uint8_t>(sp + vfx::spr::kFlipX) = 0;
    at<uint8_t>(sp + vfx::spr::kCode) = 2;
    at<uint16_t>(sp + vfx::spr::kAngle) = 0;
    at<float>(sp + vfx::spr::kScale) = 25.0f;
    at<uint32_t>(sp + vfx::spr::kRgb) = shade * 0x10101u;
    at<uint32_t>(sp + vfx::spr::kTexture) = vfx::vfxTexture(1);
    const uint32_t offset = frame.local<uint8>(0x10).address; // svOffset
    const uint32_t result = frame.local<uint8>(0x20).address; // svRes
    // The four burner points: x, y, z in the car's frame.
    static const int16_t kOffsets[4][3] = {{0xd, 0xc, -0x36}, {-0xd, 0xc, -0x36}, {0x23, 8, -0x3d}, {-0x23, 8, -0x3d}};
    for (const auto &point : kOffsets)
    {
        at<int16_t>(offset) = point[0];
        at<int16_t>(offset + 2) = point[1];
        at<int16_t>(offset + 4) = point[2];
        PS2C_SetGteMatrix(ptr32<_sMat>(matrix.address));
        PS2C_GteMatrixMulVec16(ptr32<_sVec16>(offset));
        PS2C_GetVec16GteResult(ptr32<_sVec16>(result));
        const uint32_t s = vfx::sprite();
        at<float>(s + vfx::spr::kPos) = ee_itof(px + at<int16_t>(result));
        at<float>(s + vfx::spr::kPos + 4) = ee_itof(py + at<int16_t>(result + 2));
        at<float>(s + vfx::spr::kPos + 8) = ee_itof(pz + at<int16_t>(result + 4));
        VfxPS2SpriteRotated3D(ee_float(-10.0f));
    }
}

// Sparks of lava thrown up from a lava pool (0x18-byte particles): each is a sprite at the pool's position plus the
// particle's own, fading from the lava colour (three bytes at +0x348) as it ages, flipped by the side it flies to and
// spinning with its own angle. The original draws a random number for each and throws it away.
void Vfx2Renderer_LavaSpit(ptr32<_sVfx_TSFX> mem)
{
    GuestFrame frame(0xa0);
    const uint32_t m = mem.address;
    const SceneryPos where = sceneryPosition(m);
    const uint32_t sp = vfx::sprite();
    at<uint8_t>(sp + vfx::spr::kCode) = 2;
    at<uint32_t>(sp + vfx::spr::kTexture) = vfx::vfxTexture(0x15);
    at<uint16_t>(sp + vfx::spr::kAngle) = static_cast<uint16_t>(vfx::random15());
    for (int32_t index = ringHead(m); index != at<uint8_t>(m + kRing + 3); index = ringNext(m, index))
    {
        const uint32_t p = m + kParticles + static_cast<uint32_t>(index) * 0x18u;
        at<float>(sp + vfx::spr::kPos) = ee_itof(where.x + (at<int16_t>(p) >> 2));
        at<float>(sp + vfx::spr::kPos + 4) = ee_itof(where.y + (at<int16_t>(p + 2) >> 2));
        at<float>(sp + vfx::spr::kPos + 8) = ee_itof(where.z + (at<int16_t>(p + 4) >> 2));
        rand();
        const int32_t age = std::max(0x10 - static_cast<int32_t>(at<uint16_t>(p + 0x14)), 0);
        const int32_t blue = atLeastZero(at<uint8_t>(m + 0x34a) - age * 8);
        const int32_t red = atLeastZero(at<uint8_t>(m + 0x348) - age * 8);
        const int32_t green = atLeastZero(at<uint8_t>(m + 0x349) - age * 8);
        at<uint8_t>(sp + vfx::spr::kFlipX) = at<int16_t>(p + 0x12) < 1 ? 1 : 0;
        at<uint32_t>(sp + vfx::spr::kRgb) = static_cast<uint32_t>(red + green * 0x100 + blue * 0x10000);
        at<uint16_t>(sp + vfx::spr::kAngle) = at<uint16_t>(p + 0x10);
        at<float>(sp + vfx::spr::kScale) = ee_itof(at<uint8_t>(p + 0x16));
        VfxPS2SpriteRotated3D(ee_float(0.0f));
    }
}

// Feathers: a new one every fourth frame while the ring has room, thrown to a random side with a slow fall; the movers
// run, and each feather's sideways speeds are then nudged towards zero as it flutters (and its sideways speed and
// height cleared once it has fallen far enough: -0x8fc).
void Vfx2Handler_Feathers(ptr32<_sVfx_TSFX> mem)
{
    const uint32_t m = mem.address;
    const uint16_t tick = static_cast<uint16_t>(at<uint16_t>(m + 0x184) + 1);
    at<uint16_t>(m + 0x184) = tick;
    if ((tick & 3) == 0 && at<uint8_t>(m + kRing + 1) < 0xf)
    {
        const uint32_t p = m + kParticles + static_cast<uint32_t>(static_cast<int8_t>(at<uint8_t>(m + kRing + 3))) * 0x14u;
        at<uint16_t>(p + 2) = 0;
        at<uint16_t>(p) = static_cast<uint16_t>((vfx::random15() & 0xfff) - 0x800);
        at<uint16_t>(p + 8) = 0;
        at<uint16_t>(p + 4) = static_cast<uint16_t>((vfx::random15() & 0xfff) - 0x800);
        at<uint16_t>(p + 0xc) = 0;
        at<uint16_t>(p + 0xa) = static_cast<uint16_t>(-4 - (vfx::random15() & 0x1f));
        at<uint16_t>(p + 0x10) = 0x96;
        at<uint16_t>(p + 0x12) = static_cast<uint16_t>(vfx::random15());
        ringPush(m);
    }
    VfxSimpleParticleMover(ptr32<_sVfx_PartHeader>(m + kRing), ptr32<_VFX_SIMPLEPARTICLE>(m + kParticles));
    for (int32_t index = ringHead(m); index != at<uint8_t>(m + kRing + 3); index = ringNext(m, index))
    {
        const uint32_t p = m + kParticles + static_cast<uint32_t>(index) * 0x14u;
        at<uint16_t>(p + 8) = static_cast<uint16_t>(at<uint16_t>(p + 8) + (at<int16_t>(p) < 0 ? 1 : -1));
        at<uint16_t>(p + 0xc) = static_cast<uint16_t>(at<uint16_t>(p + 0xc) + (at<int16_t>(p + 4) < 0 ? 1 : -1));
        if (at<int16_t>(p + 2) < -0x8fc)
        {
            at<uint16_t>(p + 0xc) = 0;
            at<uint16_t>(p + 8) = 0;
            at<uint16_t>(p + 0xa) = 0;
        }
    }
    sortAtScenery(m, 0x70);
}

// The route finder's arrows: only for the player's own car and with track direction hints on. The counter at +0x48 runs
// round 64 steps; the finder fills the car's possible routes with 32 points, and each point within three steps of the
// counter is drawn as a sprite (brightest at the counter).
void Vfx2Renderer_RouteFinder(ptr32<_sVfx_Object> mem)
{
    GuestFrame frame(0xb0);
    const uint32_t m = mem.address;
    if (G(frontEndVarG).gameStateData.gameConfig.trackDirHints == 0)
        return;
    const uint32_t car = at<uint32_t>(m + 0x40);
    if (G(cameraVarG).pCamera->pTarget.address != car)
        return;
    const uint32_t counter = (at<uint32_t>(m + 0x48) + 1) & 0x3f;
    at<uint32_t>(m + 0x48) = counter;
    AINodeRouteFinder(ptr32<_sObject>(car), 0x80, 0);
    const uint32_t sp = vfx::sprite();
    at<uint8_t>(sp + vfx::spr::kCode) = 2;
    at<uint32_t>(sp + vfx::spr::kTexture) = vfx::vfxTexture(0x17);
    at<uint8_t>(sp + vfx::spr::kFlipX) = 0;
    at<float>(sp + vfx::spr::kScale) = 32.0f;
    at<uint16_t>(sp + vfx::spr::kAngle) = 0;
    for (uint32_t i = 0; i < 0x20; ++i)
    {
        uint32_t brightness = 0;
        if (counter == i)
            brightness = 0x80;
        else if (counter == i + 1)
            brightness = 0x46;
        else if (counter == i + 2)
            brightness = 0x30;
        if (brightness == 0)
            continue;
        const int32_t angle = vfx::random15();
        const SVECTOR &route = G(aiNodeVarG).possibleAIRoutes[i];
        at<uint32_t>(sp + vfx::spr::kRgb) = brightness * 0x10001u;
        at<uint16_t>(sp + vfx::spr::kAngle) = static_cast<uint16_t>(angle);
        at<float>(sp + vfx::spr::kPos) = ee_itof(route.vx);
        at<float>(sp + vfx::spr::kPos + 4) = ee_itof(route.vy + 0x32);
        at<float>(sp + vfx::spr::kPos + 8) = ee_itof(route.vz);
        VfxPS2SpriteRotated3D(ee_float(-80.0f));
    }
}

// The time trial clock that floats over the player's car: on its first frame (life 0x80) it takes the car's position
// (raised 40 units) and, for the player's own car, its velocity in units per frame; the position then follows the
// velocity, a spin speed and a clock hand angle run down, and a puff of smoke (the 0x14-byte particles from +0x48) is
// thrown out at random while the clock has more than 7 frames to live.
void Vfx2Handler_TTclock(ptr32<_sVfx_Object> mem)
{
    GuestFrame frame(0x80);
    const uint32_t m = mem.address;
    const ptr32<_sObject> car(at<uint32_t>(m + 0x44));
    if (at<uint16_t>(m + kLifeSpan) == 0x80)
    {
        const ptr32<ee_float> where = VfxVUCarPositionPoi(car);
        at<float>(m + 0xf0) = where[0];
        const ee_float y = where[1];
        const ptr32<_sObject> target = G(cameraVarG).pCamera->pTarget;
        at<float>(m + 0xf4) = y + ee_float(40.0f);
        at<float>(m + 0xf8) = where[2];
        if (target.address == car.address)
        {
            const uint64_t velocity = VfxCarVelocityVal(car);
            at<float>(m + 0x100) = ee_itof(static_cast<int16_t>(velocity)) * ee_float(0.00390625f);
            at<float>(m + 0x104) = ee_itof(static_cast<int16_t>(velocity >> 16)) * ee_float(0.00390625f);
            at<float>(m + 0x108) = ee_itof(static_cast<int16_t>(velocity >> 32)) * ee_float(0.00390625f);
        }
        at<int16_t>(m + 0x110) = -0x334;
        at<int16_t>(m + 0x112) = 0x280;
    }
    at<float>(m + 0xf0) = ee_float(at<float>(m + 0xf0)) + ee_float(at<float>(m + 0x100));
    at<float>(m + 0xf4) = ee_float(at<float>(m + 0xf4)) + ee_float(at<float>(m + 0x104));
    at<float>(m + 0xf8) = ee_float(at<float>(m + 0xf8)) + ee_float(at<float>(m + 0x108));
    sceVu0CopyVector(ptr32<ee_float>(m + 0x20), ptr32<ee_float>(m + 0xf0));
    const uint16_t life = at<uint16_t>(m + kLifeSpan);
    int16_t spin = at<int16_t>(m + 0x112);
    if (life < 0x41)
        at<uint16_t>(m + 0x110) = 0;
    else
        at<uint16_t>(m + 0x110) = static_cast<uint16_t>(at<uint16_t>(m + 0x110) + static_cast<uint16_t>(spin));
    spin = static_cast<int16_t>(static_cast<uint16_t>(spin) - 0x10);
    at<int16_t>(m + 0x112) = spin < 0 ? 0 : spin;
    if (life > 7 && at<uint8_t>(m + kRing + 1) < at<uint8_t>(m + kRing))
    {
        const uint32_t p = m + 0x48 + static_cast<uint32_t>(at<uint8_t>(m + kRing + 3)) * 0x14u;
        at<uint16_t>(p) = static_cast<uint16_t>((vfx::random15() & 0x7f) - 0x40);
        at<uint16_t>(p + 2) = static_cast<uint16_t>((vfx::random15() & 0x7f) - 0x40);
        at<uint16_t>(p + 4) = static_cast<uint16_t>((vfx::random15() & 0x7f) - 0x40);
        at<uint16_t>(p + 8) = static_cast<uint16_t>((vfx::random15() & 0xff) - 0x80);
        at<uint16_t>(p + 0xa) = static_cast<uint16_t>((vfx::random15() & 0xff) - 0x80);
        at<uint16_t>(p + 0xc) = static_cast<uint16_t>((vfx::random15() & 0xff) - 0x80);
        at<uint16_t>(p + 0x10) = 7;
        ringPush(m);
    }
    VfxSimpleParticleMover(ptr32<_sVfx_PartHeader>(m + kRing), ptr32<_VFX_SIMPLEPARTICLE>(m + 0x48));
    VfxCarSortPos(car, mem);
    at<uint16_t>(m + kLifeSpan) = static_cast<uint16_t>(at<uint16_t>(m + kLifeSpan) - 1);
}

// The clock itself, for every car but the human players' second kind (type 2): the smoke puffs around the clock's
// position (pulled towards a point in front of the camera over the clock's last 0x40 frames), then the clock face
// with its hand angle swinging on the game timer.
void Vfx2Renderer_TTclock(ptr32<_sVfx_Object> mem)
{
    GuestFrame frame(0xe0);
    const uint32_t m = mem.address;
    const uint32_t hud = G(pLvlG)->pHudRes.address;
    const uint32_t car = at<uint32_t>(m + 0x44);
    if (at<uint8_t>(car + 0x44) == 2)
        return;
    if (!(VfxHasCarBeenRendered(ptr32<_sObject>(car)) < 0x157c))
        return;
    uint32_t sp = vfx::sprite();
    at<float>(sp + vfx::spr::kPos) = at<float>(m + 0xf0);
    at<float>(sp + vfx::spr::kPos + 4) = at<float>(m + 0xf4);
    at<float>(sp + vfx::spr::kPos + 8) = at<float>(m + 0xf8);
    sceVu0CopyVector(ptr32<ee_float>(m + 0x20), ptr32<ee_float>(m + 0xf0));
    const uint32_t offset = frame.local<uint8>(0).address; // vuOffset
    const uint32_t pos = frame.local<uint8>(0x10).address;  // vuPos
    at<float>(offset) = 6.0f;
    at<float>(offset + 4) = at<float>(0x3278d8u);
    at<float>(offset + 8) = 13.5f;
    sceVu0ApplyMatrix(ptr32<ee_float>(offset), ptr32<ee_float>(0x1f1170u), ptr32<ee_float>(offset));
    vfx::vu0Add(offset, 0x1f1160u, offset);
    int32_t remaining = static_cast<int32_t>(at<uint16_t>(m + kLifeSpan)) - 0x40;
    remaining = remaining > -1 ? remaining : 0;
    guest_call(0x1c8308u, ee_float(1.0f) - ee_itof(remaining) * ee_float(0.015625f), ptr32<ee_float>(pos), ptr32<ee_float>(offset),
               ptr32<ee_float>(sp + vfx::spr::kPos)); // sceVu0InterVectorXYZ
    sp = vfx::sprite();
    at<uint8_t>(sp + vfx::spr::kCode) = 2;
    at<uint32_t>(sp + vfx::spr::kRgb) = 0x80808080;
    at<uint32_t>(sp + vfx::spr::kTexture) = vfx::vfxTexture(0xe);
    for (uint32_t index = at<uint8_t>(m + kRing + 2); index != at<uint8_t>(m + kRing + 3); index = (index + 1) & at<uint8_t>(m + kRing))
    {
        const uint32_t p = m + 0x48 + index * 0x14u;
        at<float>(sp + vfx::spr::kScale) = ee_itof(at<uint16_t>(p + 0x10) + 6) * ee_float(at<float>(0x3278dcu)) + ee_float(at<float>(0x3278e0u));
        const ee_float step = ee_float(0.00390625f);
        const ee_float dx = ee_itof(at<int16_t>(p) >> 4) * step, dy = ee_itof(at<int16_t>(p + 2) >> 4) * step, dz = ee_itof(at<int16_t>(p + 4) >> 4) * step;
        at<float>(sp + vfx::spr::kPos) = ee_float(at<float>(pos)) + dx;
        at<float>(sp + vfx::spr::kPos + 4) = ee_float(at<float>(pos + 4)) + dy;
        at<float>(sp + vfx::spr::kPos + 8) = ee_float(at<float>(pos + 8)) + dz;
        at<uint16_t>(sp + vfx::spr::kAngle) = static_cast<uint16_t>(vfx::random15());
        VfxPS2SpriteRotated3D(ee_float(0.0f));
    }
    remaining = static_cast<int32_t>(at<uint16_t>(m + kLifeSpan)) - 0x40;
    remaining = remaining > -1 ? remaining : 0;
    at<float>(sp + vfx::spr::kScale) = ee_itof(remaining) * ee_float(at<float>(0x3278e4u)) + ee_float(at<float>(0x3278e8u));
    guest_call(0x1b4188u, ptr32<uint8>(0x1ed910u)); // TimerGet(&mainVarG.gameTimer)
    const uint32_t time = guest_call<uint32_t>(0x1b4188u, ptr32<uint8>(0x1ed910u));
    const ee_float turn = ee_itof(static_cast<int32_t>((time << 3) & 0xfff));
    const ee_float sine = ee_sinf((turn + turn) * ee_float(at<float>(0x3278ecu)));
    at<uint16_t>(sp + vfx::spr::kAngle) = static_cast<uint16_t>(at<uint16_t>(m + 0x110) + (ee_ftoi(sine * ee_float(4096.0f)) >> 5));
    at<uint8_t>(sp + vfx::spr::kFlipX) = 1;
    sp = vfx::sprite();
    at<uint8_t>(sp + vfx::spr::kCode) = 0;
    at<uint32_t>(sp + vfx::spr::kRgb) = 0x80c0c0c0;
    at<uint32_t>(sp + vfx::spr::kTexture) = at<uint32_t>(hud + 0xd4);
    guest_call(0x1c82e8u, ptr32<ee_float>(sp + vfx::spr::kPos), ptr32<ee_float>(pos)); // sceVu0CopyVectorXYZ
    VfxPS2SpriteRotated3D(ee_float(0.0f));
}

// Not ported: `Vfx2PodiumInitialise` (0x16ab88), which creates the podium's cars (through `ObjectCreate`, from the front end's
// car definitions and the level's start data) after a race. No demo race reaches it, and a random test would need a
// whole front end and level start record; its second-to-fourth place paths also use a register ($s1) from the previous loop
// pass or the caller when neither of its two conditions hold, which Ghidra shows as `unaff_s1_lo`. It stays recompiled.

// The cannon's smoke: the scenery's position raised (and moved back) 0x10e units; each 0x14-byte particle is drawn twice,
// first as a puff that cools from yellow to red as it ages, then as a smaller spinning grey disc.
void Vfx2Renderer_Cannon(ptr32<_sVfx_TSFX> mem)
{
    GuestFrame frame(0x90);
    const uint32_t m = mem.address;
    const uint32_t tob = at<uint32_t>(m + 0x10);
    const int16_t x = static_cast<int16_t>(at<int32_t>(tob + 4) >> 8);
    const int16_t z = static_cast<int16_t>(static_cast<int16_t>(at<int32_t>(tob + 0xc) >> 8) - 0x10e);
    const int16_t y = static_cast<int16_t>(static_cast<int16_t>(at<int32_t>(tob + 8) >> 8) + 0x10e);
    const uint32_t sp = vfx::sprite();
    at<uint8_t>(sp + vfx::spr::kFlipX) = 0;
    at<uint8_t>(sp + vfx::spr::kCode) = 2;
    at<uint32_t>(sp + vfx::spr::kTexture) = vfx::vfxTexture(0);
    for (int32_t index = ringHead(m); index != at<uint8_t>(m + kRing + 3); index = ringNext(m, index))
    {
        const uint32_t p = m + kParticles + static_cast<uint32_t>(index) * 0x14u;
        const int32_t life = at<uint16_t>(p + 0x10);
        const int32_t warm = std::max(life * -4 + 200, 0);
        const int32_t green = std::max(200 - (warm + (warm >> 1)), 0);
        const int32_t blue = std::max(warm * -4 + 200, 0);
        at<uint16_t>(sp + vfx::spr::kAngle) = at<uint16_t>(p + 0x12);
        at<float>(sp + vfx::spr::kPos) = ee_itof(x + at<int16_t>(p));
        at<float>(sp + vfx::spr::kPos + 4) = ee_itof(y + (at<int16_t>(p + 2) >> 3));
        at<float>(sp + vfx::spr::kPos + 8) = ee_itof(z + (at<int16_t>(p + 4) >> 1));
        at<uint32_t>(sp + vfx::spr::kRgb) = static_cast<uint32_t>((200 - warm) + green * 0x100 + blue * 0x10000);
        at<float>(sp + vfx::spr::kScale) = ee_itof((life * -2 + 0x50) * 4) + ee_float(40.0f);
        VfxPS2SpriteRotated3D(ee_float(0.0f));
    }
    at<uint8_t>(sp + vfx::spr::kCode) = 2;
    at<uint32_t>(sp + vfx::spr::kTexture) = vfx::vfxTexture(5);
    for (int32_t index = ringHead(m); index != at<uint8_t>(m + kRing + 3); index = ringNext(m, index))
    {
        const uint32_t p = m + kParticles + static_cast<uint32_t>(index) * 0x14u;
        const uint32_t life = at<uint16_t>(p + 0x10);
        at<float>(sp + vfx::spr::kPos) = ee_itof(x + (at<int16_t>(p) >> 1));
        at<float>(sp + vfx::spr::kPos + 4) = ee_itof(y + (at<int16_t>(p + 2) >> 1));
        at<uint16_t>(sp + vfx::spr::kAngle) = static_cast<uint16_t>(-static_cast<int32_t>(at<uint16_t>(p + 0x12)));
        at<uint32_t>(sp + vfx::spr::kRgb) = life * 0x10101u;
        at<float>(sp + vfx::spr::kScale) = ee_itof(static_cast<int32_t>((life * static_cast<uint32_t>(-2) + 0x50) * 8)) + ee_float(20.0f);
        at<float>(sp + vfx::spr::kPos + 8) = ee_itof(z + at<int16_t>(p + 4));
        VfxPS2SpriteRotated3D(ee_float(0.0f));
    }
}

// The jungle fire's sprites: every flame particle (0x18 bytes) is drawn twice, a dull glow (texture 5) that is brighter
// the older the flame, then a hot core (texture 0) that fades from yellow-white to red as it ages.
void Vfx2Renderer_JungleFire(ptr32<_sVfx_TSFX> mem)
{
    GuestFrame frame(0x80);
    const uint32_t m = mem.address;
    const SceneryPos where = sceneryPosition(m);
    const uint32_t sp = vfx::sprite();
    at<uint8_t>(sp + vfx::spr::kCode) = 4;
    at<uint32_t>(sp + vfx::spr::kTexture) = vfx::vfxTexture(5);
    for (int32_t index = ringHead(m); index != at<uint8_t>(m + kRing + 3); index = ringNext(m, index))
    {
        const uint32_t p = m + kParticles + static_cast<uint32_t>(index) * 0x18u;
        const uint32_t life = at<uint16_t>(p + 0x14);
        at<float>(sp + vfx::spr::kPos) = ee_itof(where.x + (at<int16_t>(p) >> 2));
        at<float>(sp + vfx::spr::kPos + 4) = ee_itof(where.y + (at<int16_t>(p + 2) >> 1) + 0x5a);
        at<float>(sp + vfx::spr::kPos + 8) = ee_itof(where.z + (at<int16_t>(p + 4) >> 2));
        at<uint16_t>(sp + vfx::spr::kAngle) = at<uint16_t>(p + 0x10);
        at<uint32_t>(sp + vfx::spr::kRgb) = (life + (life >> 2)) * 0x10101u;
        at<float>(sp + vfx::spr::kScale) = ee_itof(at<uint8_t>(p + 0x16));
        VfxPS2SpriteRotated3D(ee_float(50.0f));
    }
    at<uint8_t>(sp + vfx::spr::kCode) = 2;
    at<uint32_t>(sp + vfx::spr::kTexture) = vfx::vfxTexture(0);
    for (int32_t index = ringHead(m); index != at<uint8_t>(m + kRing + 3); index = ringNext(m, index))
    {
        const uint32_t p = m + kParticles + static_cast<uint32_t>(index) * 0x18u;
        const uint32_t life = at<uint16_t>(p + 0x14);
        const int32_t age = static_cast<int32_t>(life) * -2 + 0x40;
        at<float>(sp + vfx::spr::kPos) = ee_itof(where.x + (at<int16_t>(p) >> 2));
        at<float>(sp + vfx::spr::kPos + 8) = ee_itof(where.z + (at<int16_t>(p + 4) >> 2));
        at<float>(sp + vfx::spr::kPos + 4) = ee_itof(where.y + (at<int16_t>(p + 2) >> 2) + 0x28);
        const int32_t green = std::max(age * -2 + 0x46, 0);
        const int32_t blue = std::max(age * -3 + 0x46, 0);
        const uint32_t size = at<uint8_t>(p + 0x16);
        at<uint8_t>(sp + vfx::spr::kFlipX) = 0;
        at<float>(sp + vfx::spr::kScale) = ee_itof(static_cast<int32_t>(size + (size >> 1)));
        at<uint16_t>(sp + vfx::spr::kAngle) = at<uint16_t>(p + 0x10);
        at<uint32_t>(sp + vfx::spr::kRgb) = static_cast<uint32_t>(static_cast<int32_t>(life * 2 + 0xd) + green * 0x100 + blue * 0x10000);
        VfxPS2SpriteRotated3D(ee_float(50.0f));
    }
}

// The bees' (and the flies') first frame: every one of the swarm's particles gets a random position, no speed
// to start with, a random angle and a random long life (about 60000 as a 16-bit count). Its two halves of the loop
// each fill one particle: 15 passes fill particles 1 to 30 (the ring header then claims 0x3f of them, or 0x1f, in
// use). After the movers run, the swarm's sideways, up and down speeds are nudged by eight towards zero.
static void swarmHandler(uint32_t m, uint32_t inUse)
{
    if (at<int16_t>(m + 0x544) == 0)
    {
        at<int16_t>(m + 0x544) = 1;
        const auto fill = [](uint32_t p)
        {
            at<uint16_t>(p) = static_cast<uint16_t>((vfx::random15() & 0xfff) - 0x800);
            at<uint16_t>(p + 2) = static_cast<uint16_t>((vfx::random15() & 0xfff) - 0x800);
            const uint16_t z = static_cast<uint16_t>((vfx::random15() & 0xfff) - 0x800);
            at<uint16_t>(p + 8) = 0;
            at<uint16_t>(p + 0xa) = 0;
            at<uint16_t>(p + 0xc) = 0;
            at<uint16_t>(p + 4) = z;
            at<uint16_t>(p + 0x12) = static_cast<uint16_t>(vfx::random15());
            at<uint16_t>(p + 0x10) = static_cast<uint16_t>((vfx::random15() & 0xfff) - 0x15a0);
        };
        fill(m + kParticles);
        for (uint32_t pair = m + 0x58; pair < m + 0x2b0; pair += 0x28)
        {
            fill(pair);
            fill(pair + 0x14);
        }
        at<uint8_t>(m + kRing + 1) = static_cast<uint8_t>(inUse);
        at<uint8_t>(m + kRing + 3) = static_cast<uint8_t>(inUse);
    }
    VfxSimpleParticleMover(ptr32<_sVfx_PartHeader>(m + kRing), ptr32<_VFX_SIMPLEPARTICLE>(m + kParticles));
    for (int32_t index = ringHead(m); index != at<uint8_t>(m + kRing + 3); index = ringNext(m, index))
    {
        const uint32_t p = m + kParticles + static_cast<uint32_t>(index) * 0x14u;
        at<uint16_t>(p + 8) = static_cast<uint16_t>(at<uint16_t>(p + 8) + (at<int16_t>(p) < 0 ? 8 : -8));
        at<uint16_t>(p + 0xa) = static_cast<uint16_t>(at<uint16_t>(p + 0xa) + (at<int16_t>(p + 2) < 0 ? 8 : -8));
        at<uint16_t>(p + 0xc) = static_cast<uint16_t>(at<uint16_t>(p + 0xc) + (at<int16_t>(p + 4) < 0 ? 8 : -8));
    }
}

void Vfx2Handler_Bees(ptr32<_sVfx_TSFX> mem)
{
    swarmHandler(mem.address, 0x3f);
    sortAtScenery(mem.address, 0x90);
}

void Vfx2Handler_Flies(ptr32<_sVfx_TSFX> mem)
{
    swarmHandler(mem.address, 0x1f);
    sortAtScenery(mem.address, 0x90);
}

// The rocket car's exhaust (only on one level, 0x62, where the cars are rockets): each frame while the ring has room a
// puff starts behind the car (0, 11, -75 turned by the car's matrix) with a random speed minus the car's own
// forward direction, living 15 frames; every puff is drawn twice at the car's position plus its own offset: a bright
// one and a bigger, redder one, both fading with the puff's age.
void Vfx3Renderer_RocketCar(ptr32<_sVfx_Object> mem)
{
    GuestFrame frame(0x100);
    const uint32_t m = mem.address;
    const ptr32<_sObject> car(at<uint32_t>(m + 0x350));
    if (car->sDontRenderMe != 0 && G(mainVarG).level == 0x62)
        return;
    if (G(mainVarG).level == 0x62)
    {
        const ptr32<MATRIX> matrix = VfxCarMatrixPoi(car);
        const ptr32<ee_float> vuMatrix = VfxPSXToVUMatrix(matrix);
        const ee_float spread = at<float>(0x327914u), push = at<float>(0x327918u);
        if (at<uint8_t>(m + kRing + 1) < at<uint8_t>(m + kRing))
        {
            const uint32_t p = m + 0x50 + static_cast<uint32_t>(at<uint8_t>(m + kRing + 3)) * 0x30u;
            at<float>(p) = 0.0f;
            at<float>(p + 4) = 11.0f;
            at<float>(p + 8) = -75.0f;
            sceVu0ApplyMatrix(ptr32<ee_float>(p), vuMatrix, ptr32<ee_float>(p));
            at<float>(p + 0x10) = ee_itof((vfx::random15() & 0x3f) - 0x1f) * spread - ee_itof(matrix->m[0][2]) * push;
            at<float>(p + 0x14) = ee_itof((vfx::random15() & 0x3f) - 0x1f) * spread - ee_itof(matrix->m[1][2]) * push;
            const int32_t z = (vfx::random15() & 0x3f) - 0x1f;
            at<int32_t>(p + 0x24) = 15;
            at<float>(p + 0x18) = ee_itof(z) * spread - ee_itof(matrix->m[2][2]) * push;
            ringPush(m);
        }
    }
    if (!(VfxHasCarBeenRendered(car) < 0x157c))
        return;
    const ptr32<ee_float> where = VfxVUCarPositionPoi(car);
    const ptr32<ee_float> pos = frame.local<ee_float>(0x10);
    pos[0] = where[0];
    pos[1] = where[1];
    pos[2] = where[2];
    const uint32_t sp = vfx::sprite();
    at<uint8_t>(sp + vfx::spr::kFlipX) = 0;
    at<uint8_t>(sp + vfx::spr::kCode) = 2;
    at<uint32_t>(sp + vfx::spr::kTexture) = vfx::vfxTexture(1);
    at<uint16_t>(sp + vfx::spr::kAngle) = static_cast<uint16_t>(vfx::random15());
    for (uint32_t index = at<uint8_t>(m + kRing + 2); index != at<uint8_t>(m + kRing + 3); index = (index + 1) & at<uint8_t>(m + kRing))
    {
        const uint32_t p = m + 0x50 + index * 0x30u;
        const uint32_t life = static_cast<uint32_t>(at<int32_t>(p + 0x24)) * 2 + 10;
        if (static_cast<int32_t>(life) < 0)
            at<float>(sp + vfx::spr::kScale) = ee_itof(static_cast<int32_t>(life >> 1)) + ee_itof(static_cast<int32_t>(life >> 1));
        else
            at<float>(sp + vfx::spr::kScale) = ee_itof(static_cast<int32_t>(life));
        at<uint32_t>(sp + vfx::spr::kRgb) = static_cast<uint32_t>(at<int32_t>(p + 0x24)) * 0x102u;
        sceVu0CopyVector(ptr32<ee_float>(sp + vfx::spr::kPos), pos);
        vfx::vu0Add(sp + vfx::spr::kPos, sp + vfx::spr::kPos, p);
        VfxPS2SpriteRotated3D(ee_float(-10.0f));
        at<uint32_t>(sp + vfx::spr::kRgb) = static_cast<uint32_t>(at<int32_t>(p + 0x24)) * 0x40200u;
        at<float>(sp + vfx::spr::kScale) = ee_float(at<float>(sp + vfx::spr::kScale)) * ee_float(at<float>(0x32791cu));
        VfxPS2SpriteRotated3D(ee_float(-10.0f));
    }
}
} // namespace rc

namespace rc::fuzz {
void vfxUfoBurnerRenderer(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxLavaSpit(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxFeathersHandler(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxRouteFinderRenderer(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxTTclockHandler(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxTTclockRenderer(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxCannon(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxJungleFireRenderer(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxBeesHandler(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxRocketCar(R5900Context *ctx, uint64_t &state, uint32_t scratch);
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
RC_DECOMP_FN_SETUP(Vfx2Renderer_UFOBurner, 0x171978, rc::fuzz::vfxUfoBurnerRenderer)
RC_DECOMP_FN_SETUP(Vfx2Renderer_LavaSpit, 0x1755f8, rc::fuzz::vfxLavaSpit)
RC_DECOMP_FN_SETUP(Vfx2Handler_Feathers, 0x172a78, rc::fuzz::vfxFeathersHandler)
RC_DECOMP_FN_SETUP(Vfx2Renderer_RouteFinder, 0x176788, rc::fuzz::vfxRouteFinderRenderer)
RC_DECOMP_FN_SETUP(Vfx2Handler_TTclock, 0x174418, rc::fuzz::vfxTTclockHandler)
RC_DECOMP_FN_SETUP(Vfx2Renderer_TTclock, 0x174710, rc::fuzz::vfxTTclockRenderer)
RC_DECOMP_FN_SETUP(Vfx2Renderer_Cannon, 0x177e10, rc::fuzz::vfxCannon)
RC_DECOMP_FN_SETUP(Vfx2Renderer_JungleFire, 0x16fe80, rc::fuzz::vfxJungleFireRenderer)
RC_DECOMP_FN_SETUP(Vfx2Handler_Bees, 0x176a10, rc::fuzz::vfxBeesHandler)
RC_DECOMP_FN_SETUP(Vfx2Handler_Flies, 0x176ec0, rc::fuzz::vfxBeesHandler)
RC_DECOMP_FN_SETUP(Vfx3Renderer_RocketCar, 0x17a158, rc::fuzz::vfxRocketCar)
