// Decompiled from VISFX.c and VISFX2.c, continued (eighth batch, renderers): the give-terror pickup, the heat ray's
// blast, the water-covered car, the invincibility shield and the finish position. Each completes a handler/renderer
// pair whose handler is native (the give-terror handler is in visfx10.cpp). See visfx5.cpp for the conventions; all of
// these draw sprites or polygons, so they keep the original's frame (GuestFrame) for the sprites' stale stack lanes.

#include "ai_fuzz.h"
#include "ee_math.h"
#include "vifpk.h"
#include "visfx.h"

namespace rc {

uint16 Sqr32(uint32 n);                                                           // math_core.cpp
uint16 RandomGetNDRange(uint16 lower, uint16 upper);                              // math_core.cpp
void PS2C_Vec16MulScaler(ptr32<_sVec16> pOut, ptr32<_sVec16> pVec, int16 scale); // psxdefs.cpp
void VfxPutUV0(ee_float u, ee_float v);                                           // visfx.cpp
void VfxPutUV1(ee_float u, ee_float v);
void VfxPutUV2(ee_float u, ee_float v);
void VfxPutUV3(ee_float u, ee_float v);

namespace {

using vfx::at;

constexpr uint32_t kRing = 0x40;          // the ring header: mask, count, head, tail
constexpr uint32_t kMainVar = sym::mainVarG;   // mainVarG (its first word is the status)
constexpr uint32_t kVisFx = sym::visFxVarG;     // visFxVarG
constexpr uint32_t kCamera = (sym::cameraVarG + 0xb70u);    // cameraVarG.pCamera
constexpr uint32_t kGlobalVUVec = sym::globalVUVec;
constexpr uint32_t kGameTimer = (sym::mainVarG + 0x10u); // mainVarG.gameTimer
constexpr uint32_t kTimerGet = sym::TimerGet, kNormalMulMatPsx = sym::NormalMulMatPsx, kScaleVectorXYZ = sym::sceVu0ScaleVectorXYZ;

inline uint8_t &ringMask(uint32_t m) { return at<uint8_t>(m + kRing); }
inline uint8_t &ringHeadU(uint32_t m) { return at<uint8_t>(m + kRing + 2); }
inline uint8_t &ringTail(uint32_t m) { return at<uint8_t>(m + kRing + 3); }

inline int32_t sh16(uint32_t address) { return at<int16_t>(address); }
inline int32_t rnd(int32_t mask) { return vfx::random15() & mask; }
inline int32_t atLeastZero(int32_t value) { return value > -1 ? value : 0; }
inline uint32_t quad() { return G(visFxVarG).pGT4.address; }

// The car's draw distance as the renderers test it (as VfxHasCarBeenRendered): in a race that is over or a demo race, a
// car that is drawn counts as 100 units away.
uint16_t carDistance(uint32_t car)
{
    if ((at<uint32_t>(kMainVar) & 0x30) == 0)
        return at<uint16_t>(car + 0x174);
    return at<int16_t>(car + 0x172) != 0 ? at<uint16_t>(car + 0x174) : 100;
}

namespace spr = vfx::spr;
inline void spritePos(ee_float x, ee_float y, ee_float z)
{
    const uint32_t s = vfx::sprite();
    at<float>(s + spr::kPos) = x;
    at<float>(s + spr::kPos + 4) = y;
    at<float>(s + spr::kPos + 8) = z;
}

inline void scaleXYZ(ee_float factor, uint32_t dst, uint32_t src) { guest_call(kScaleVectorXYZ, factor, ptr32<ee_float>(dst), ptr32<ee_float>(src)); }
inline void copyVector(uint32_t dst, uint32_t src) { sceVu0CopyVector(ptr32<ee_float>(dst), ptr32<ee_float>(src)); }

// sin(angle) for a 12-bit angle (doubled, times the constant at `turn`), times 4096 and truncated.
int32_t sine4096(uint32_t angle, uint32_t turn)
{
    const ee_float a = ee_itof(static_cast<int32_t>(angle & 0xfff));
    return ee_ftoi(ee_sinf((a + a) * ee_float(at<float>(turn))) * ee_float(4096.0f));
}
inline int32_t cosine4096(uint32_t angle, uint32_t turn)
{
    const ee_float a = ee_itof(static_cast<int32_t>(angle & 0xfff));
    return ee_ftoi(ee_cosf((a + a) * ee_float(at<float>(turn))) * ee_float(4096.0f));
}

} // namespace

// The give-terror pickup on a car (the car at +0x2cc; the handler is in visfx10.cpp): while the car is near enough to
// be drawn, a lightning bolt from the car's aerial up into the sky (rising for the effect's first frames, up to 1000),
// reddish and fading; a glow at the aerial's tip (two sprites, one flickering in size) and the sparks (0x14 bytes from
// +0x44) round the aerial, brighter while young.
void VfxRenderer_GiveTerror(ptr32<_sVfx_Object> mem)
{
    GuestFrame frame(0x150);
    const uint32_t m = mem.address;
    const uint32_t car = at<uint32_t>(m + 0x2cc);
    if (!(carDistance(car) < 0x157c))
        return;
    const uint64_t aerial = VfxCarAerialVal(ptr32<_sObject>(car), 5);
    (void)VfxCarAerialVal(ptr32<_sObject>(car), 5); // the original fetches it twice and keeps the second in a dead local
    const int16_t ax = static_cast<int16_t>(aerial), ay = static_cast<int16_t>(aerial >> 16), az = static_cast<int16_t>(aerial >> 32);
    const uint32_t ground = frame.local<float>(0x20).address, sky = frame.local<float>(0x30).address;
    at<float>(ground) = ee_itof(ax);
    at<float>(ground + 4) = ee_itof(ay);
    at<float>(ground + 8) = ee_itof(az);
    copyVector(sky, ground);
    const uint16_t life = at<uint16_t>(m + 0x10);
    ee_float rise = ee_itof((100 - static_cast<int32_t>(life)) << 3);
    if (ee_float(1000.0f) < rise)
        rise = ee_float(1000.0f);
    at<float>(sky + 4) = ee_float(at<float>(sky + 4)) + rise;
    const int32_t base = 0x8c - static_cast<int32_t>(life);
    const int32_t red = base + rnd(0x3f);
    const int32_t green = (base >> 1) + rnd(0x3f);
    const uint32_t colour = static_cast<uint32_t>(red + (green << 8));
    vfx::lightningBolt(ground, sky, colour, vfx::vfxTexture(0x19), ee_float(20.0f), ee_float(-10.0f), frame.local<float>(0x40).address,
                       frame.local<float>(0x60).address, frame.local<float>(0x50).address, frame.local<float>(0x70).address);
    at<uint16_t>(vfx::sprite() + spr::kAngle) = static_cast<uint16_t>(vfx::random15());
    const uint32_t tip = at<uint32_t>(car + 0x40); // the aerial effect: its last section is at +0x90
    for (uint32_t lane = 0; lane < 12; lane += 4)
        at<uint32_t>(vfx::sprite() + spr::kPos + lane) = at<uint32_t>(tip + 0x90 + lane);
    int32_t glow = static_cast<int32_t>(at<uint16_t>(m + 0x10)) << 2;
    if (!(glow < 0x81))
        glow = 0x80;
    const int32_t half = glow >> 1;
    at<uint32_t>(vfx::sprite() + spr::kRgb) = static_cast<uint32_t>(glow + (half << 8));
    const int32_t flicker = rnd(7);
    at<uint8_t>(vfx::sprite() + spr::kCode) = 2;
    at<float>(vfx::sprite() + spr::kScale) = ee_itof(flicker + 0x10);
    at<uint32_t>(vfx::sprite() + spr::kTexture) = vfx::vfxTexture(1);
    VfxPS2SpriteRotated3D(ee_float(-10.0f));
    at<uint8_t>(vfx::sprite() + spr::kCode) = 2;
    at<float>(vfx::sprite() + spr::kScale) = 24.0f;
    at<uint32_t>(vfx::sprite() + spr::kRgb) = static_cast<uint32_t>(half << 16);
    at<uint32_t>(vfx::sprite() + spr::kTexture) = vfx::vfxTexture(0xf);
    VfxPS2SpriteRotated3D(ee_float(-10.0f));
    at<float>(vfx::sprite() + spr::kScale) = 14.0f;
    at<uint32_t>(vfx::sprite() + spr::kTexture) = vfx::vfxTexture(0xe);
    for (uint32_t index = ringHeadU(m); index != ringTail(m); index = (index + 1) & ringMask(m))
    {
        const uint32_t p = m + 0x44 + index * 0x14;
        if (at<uint16_t>(p + 0x10) == 0)
            continue;
        at<uint16_t>(vfx::sprite() + spr::kAngle) = static_cast<uint16_t>(vfx::random15());
        const uint32_t sparkLife = at<uint16_t>(p + 0x10), twice = sparkLife << 1;
        spritePos(ee_itof(ax + (sh16(p) >> 4)), ee_itof(ay + (sh16(p + 2) >> 4)), ee_itof(az + (sh16(p + 4) >> 4)));
        at<uint32_t>(vfx::sprite() + spr::kRgb) = twice + (sparkLife << 9) + (twice << 15);
        VfxPS2SpriteRotated3D(ee_float(-10.0f));
    }
}

// The heat ray's blast from a car (the car at +0x40): drawn while the car is near enough to be drawn or the camera is
// within 2000 units. A beam from the aerial along a random direction (forward, turned by the car's matrix; boats and the
// type 0x1a aim lower) 2000 long: seven segments whose colours pulse along it (sines of the effect's counter at +0x48),
// the texture scrolling with the life; each segment also scales the shared vector at visFxVarG+0x40 by 1.2. Then a few
// random sparks along the beam.
void VfxRenderer_CarHeatRayBlast(ptr32<_sVfx_Object> mem)
{
    GuestFrame frame(0x1f0);
    const uint32_t m = mem.address;
    const uint32_t car = at<uint32_t>(m + 0x40);
    const uint16_t aimZ = at<uint16_t>((sym::sec_sdata + 0x4dcu)); // iTestRP: the original copies its x, y and z, then replaces x and y
    const uint32_t camera = at<uint32_t>(kCamera), rendObj = at<uint32_t>(car + 0x18);
    const int32_t dx = static_cast<int32_t>(at<uint32_t>(camera + 0xc) - at<uint32_t>(rendObj + 4)) >> 8;
    const int32_t dy = static_cast<int32_t>(at<uint32_t>(camera + 0x10) - at<uint32_t>(rendObj + 8)) >> 8;
    const int32_t dz = static_cast<int32_t>(at<uint32_t>(camera + 0x14) - at<uint32_t>(rendObj + 0xc)) >> 8;
    const auto within = [](int32_t d) { return (d < 0 ? -d : d) < 0x7d1; };
    int32_t distance = 0x7d0;
    if (within(dx) && within(dy) && within(dz))
        distance = Sqr32(static_cast<uint32_t>(dx * dx + dy * dy + dz * dz));
    if (!(carDistance(car) < 0x157c) && !(distance < 0x7d0))
        return;
    const uint64_t aerial = VfxCarAerialVal(ptr32<_sObject>(car), 5);
    const uint32_t aim = frame.local<SVECTOR>(0xd0).address, turned = frame.local<SVECTOR>(0xc0).address;
    at<uint16_t>(aim) = static_cast<uint16_t>(RandomGetNDRange(0, 0x80) - 0x40);
    const uint32_t type = at<uint32_t>(at<uint32_t>(car + 0xc) + 4);
    if (type < 0x10 || type == 0x1a)
        at<uint16_t>(aim + 2) = static_cast<uint16_t>(RandomGetNDRange(0, 0x80) - 0x40);
    else
        at<uint16_t>(aim + 2) = static_cast<uint16_t>(RandomGetNDRange(0, 0x80) - 0x238);
    at<uint16_t>(aim + 4) = aimZ;
    guest_call(kNormalMulMatPsx, ptr32<SVECTOR>(aim), ptr32<uint8>(rendObj + 0x18), ptr32<SVECTOR>(turned));
    const uint32_t start = frame.local<float>(0x70).address, dir = frame.local<float>(0x80).address, point = frame.local<float>(0x90).address,
                   last = frame.local<float>(0xa0).address;
    for (uint32_t axis = 0; axis < 3; ++axis)
        at<float>(dir + 4 * axis) = ee_itof((sh16(turned + 2 * axis) * 0x7d0) >> 12);
    const uint16_t life = at<uint16_t>(m + 0x10);
    at<float>(start) = ee_itof(static_cast<int16_t>(aerial));
    at<float>(start + 4) = ee_itof(static_cast<int16_t>(aerial >> 16));
    at<float>(start + 8) = ee_itof(static_cast<int16_t>(aerial >> 32));
    const ee_float step(0.125f), zero(0.0f), one(1.0f), half(0.5f), four(4.0f), height(80.0f);
    ee_float v = ee_itof(life & 7) * step;
    copyVector(vfx::sprite() + spr::kPos, start);
    // The beam's colour at a point: a sine of the point and the counter, as red (plus 100) and green (plus 50, none
    // below zero), with blue 0x14.
    const auto pulse = [&](ee_float at4)
    {
        const ee_float phase = at4 + ee_itof(at<uint16_t>(m + 0x48)) * half;
        const int32_t wave = ee_ftoi(ee_sinf(phase) * height);
        return static_cast<uint32_t>(wave + 0x64 + (atLeastZero(wave + 0x32) << 8) + 0x140000);
    };
    ee_float t = step;
    int32_t segment = 0;
    do
    {
        VfxPutUV0(zero, v);
        const ee_float v2 = v + half;
        VfxPutUV1(one, v);
        VfxPutUV2(zero, v2);
        v = v2;
        VfxPutUV3(one, v2);
        const uint32_t near = pulse(t * four);
        const uint32_t far = pulse((t + step) * four);
        scaleXYZ(t, point, dir);
        vfx::vu0Add(point, start, point);
        if (segment == 0)
        {
            VfxPutGT4rgba0(near);
            VfxPutGT4rgba0(near); // the original sets corner 0 twice and leaves corner 1 as it was
            VfxPutGT4rgba2(far);
            VfxPutGT4rgba3(far);
            VfxPutGT4FacingPolyVU(ptr32<ee_float>(start), ptr32<ee_float>(point), ptr32<_sGfxTexture>(vfx::vfxTexture(2)), 0x12, ee_float(34.0f),
                                  ee_float(-10.0f));
        }
        else
        {
            VfxPutGT4rgba0(near);
            VfxPutGT4rgba1(near);
            VfxPutGT4rgba2(segment == 8 ? 0 : far);
            VfxPutGT4rgba3(segment == 8 ? 0 : far);
            VfxPutGT4FacingContinueVU(ptr32<ee_float>(last), ptr32<ee_float>(point));
        }
        copyVector(last, point);
        t = t + step;
        sceVu0ScaleVector(ptr32<ee_float>(kVisFx + 0x40), ptr32<ee_float>(kVisFx + 0x40), ee_float(at<float>(sym::lit4_3277d4)));
        ++segment;
    } while (t < one);
    const uint32_t s = vfx::sprite();
    at<uint8_t>(s + spr::kFlipX) = 0;
    at<uint8_t>(s + spr::kCode) = 2;
    at<uint32_t>(s + spr::kTexture) = vfx::vfxTexture(0);
    at<uint32_t>(s + spr::kRgb) = 0x14465a;
    ee_float along = ee_itof(rnd(0xf)) * ee_float(at<float>(sym::lit4_3277d8));
    const ee_float gap(at<float>(sym::lit4_3277dc));
    while (along < one)
    {
        scaleXYZ(along, point, dir);
        along = along + gap;
        vfx::vu0Add(vfx::sprite() + spr::kPos, start, point);
        at<float>(vfx::sprite() + spr::kScale) = ee_itof(rnd(0x1f) + 0x14);
        at<uint16_t>(vfx::sprite() + spr::kAngle) = static_cast<uint16_t>(vfx::random15());
        VfxPS2SpriteRotated3D(ee_float(0.0f));
    }
}

// The water-covered car (the car at +0x650; its position also goes to svVelTemp): while the car is near enough to be
// drawn, three water sprites circle it on sines of the effect's life, then the drops (0x30 bytes from +0x50, their
// life word at +0x24: smaller, darker and the splash texture in their last five frames), then a sheet of water over the
// car the size of its shape (two sprites turning opposite ways, fading in) and a glint that grows as the effect ends.
void VfxRenderer_CarWaterCovered(ptr32<_sVfx_Object> mem)
{
    GuestFrame frame(0xe0);
    const uint32_t m = mem.address;
    const uint32_t car = at<uint32_t>(m + 0x650);
    const uint32_t state = at<uint32_t>(at<uint32_t>(car + 0x20) + 0x1e8);
    SVECTOR &v = G(svVelTemp);
    v.vx = static_cast<int16>(at<int32_t>(state + 4) >> 8);
    v.vy = static_cast<int16>(at<int32_t>(state + 8) >> 8);
    v.vz = static_cast<int16>(at<int32_t>(state + 0xc) >> 8);
    const int16_t cx = v.vx, cy = v.vy, cz = v.vz;
    if (!(carDistance(car) < 0x157c))
        return;
    const uint32_t shape = at<uint32_t>(at<uint32_t>(car + 0x18));
    at<uint8_t>(vfx::sprite() + spr::kFlipX) = 0;
    at<uint8_t>(vfx::sprite() + spr::kCode) = 2;
    at<uint32_t>(vfx::sprite() + spr::kRgb) = 0x8c8c8c;
    at<uint32_t>(vfx::sprite() + spr::kTexture) = vfx::vfxTexture(7);
    at<float>(vfx::sprite() + spr::kScale) = 30.0f;
    at<uint16_t>(vfx::sprite() + spr::kAngle) = at<uint16_t>(m + 0x654);
    constexpr uint32_t kTurn = sym::lit4_327820;
    for (uint32_t i = 0; i < 3; ++i)
    {
        const uint32_t life = at<uint16_t>(m + 0x10), step = i << 8;
        const int32_t dx = sine4096((life << ((i + 5) & 31)) + step, kTurn) >> 7;
        const int32_t dy = sine4096((at<uint16_t>(m + 0x10) << ((7 - i) & 31)) - step, kTurn) >> 7;
        const int32_t dz = cosine4096((at<uint16_t>(m + 0x10) << ((8 - i) & 31)) + step, kTurn) >> 7;
        at<uint16_t>(vfx::sprite() + spr::kAngle) = static_cast<uint16_t>(step - at<uint16_t>(m + 0x654));
        spritePos(ee_itof(cx) + ee_itof(dx), ee_itof(cy) + ee_itof(dy), ee_itof(cz) + ee_itof(dz));
        VfxPS2SpriteRotated3D(ee_float(-50.0f));
    }
    at<uint8_t>(vfx::sprite() + spr::kCode) = 2;
    for (uint32_t index = ringHeadU(m); index != ringTail(m); index = (index + 1) & ringMask(m))
    {
        const uint32_t p = m + 0x50 + index * 0x30;
        copyVector(vfx::sprite() + spr::kPos, p);
        const uint32_t life = at<uint32_t>(p + 0x24);
        int32_t red = 0x5a, green = 0x78, blue = 0x78;
        if (life < 6)
        {
            const int32_t fade = static_cast<int32_t>(5 - life);
            blue = 0x78 - fade * 8;
            at<uint32_t>(vfx::sprite() + spr::kTexture) = vfx::vfxTexture(8);
            red = 0x5a - fade * 8;
            green = blue;
            at<float>(vfx::sprite() + spr::kScale) = ee_itof(fade * 4 + 8); // never negative: fade is 0 to 5
        }
        else
        {
            at<float>(vfx::sprite() + spr::kScale) = 8.0f;
            at<uint32_t>(vfx::sprite() + spr::kTexture) = vfx::vfxTexture(7);
        }
        at<uint16_t>(vfx::sprite() + spr::kAngle) = 0;
        at<uint32_t>(vfx::sprite() + spr::kRgb) = static_cast<uint32_t>(red + (green << 8) + (blue << 16));
        VfxPS2SpriteRotated3D(ee_float(-50.0f));
    }
    spritePos(ee_itof(cx), ee_itof(cy), ee_itof(cz));
    const int32_t life = at<uint16_t>(m + 0x10);
    const int32_t fadeIn = atLeastZero(0x96 - (life << 4));
    const int32_t blue = atLeastZero(0x2d - fadeIn), green = atLeastZero(0x3c - fadeIn), red = atLeastZero(0x96 - fadeIn);
    at<uint16_t>(vfx::sprite() + spr::kAngle) = at<uint16_t>(m + 0x654);
    at<uint8_t>(vfx::sprite() + spr::kCode) = 4;
    at<float>(vfx::sprite() + spr::kScale) = ee_itof(sh16(shape + 0x72) - sh16(shape + 0x6c)) * ee_float(at<float>(sym::lit4_327824));
    at<uint32_t>(vfx::sprite() + spr::kTexture) = vfx::vfxTexture(6);
    at<uint32_t>(vfx::sprite() + spr::kRgb) = static_cast<uint32_t>(red + (green << 8) + (blue << 16));
    VfxPS2SpriteRotated3D(ee_float(-80.0f));
    at<uint16_t>(vfx::sprite() + spr::kAngle) = static_cast<uint16_t>(-at<uint16_t>(m + 0x654));
    VfxPS2SpriteRotated3D(ee_float(-80.0f));
    int32_t shine = static_cast<int32_t>(at<uint16_t>(m + 0x10)) << 4;
    if (!(shine < 0x65))
        shine = 100;
    const uint32_t s = vfx::sprite();
    at<uint8_t>(s + spr::kCode) = 2;
    at<uint16_t>(s + spr::kAngle) = 0;
    at<uint32_t>(s + spr::kRgb) = static_cast<uint32_t>(shine + (shine << 8) + (shine << 16));
    if (shine < 0x64)
    {
        at<float>(s + spr::kScale) = ee_float(at<float>(s + spr::kScale)) * ee_float(at<float>(sym::lit4_327828));
        at<uint32_t>(s + spr::kTexture) = vfx::vfxTexture(8);
    }
    else
    {
        at<float>(s + spr::kScale) = ee_float(at<float>(s + spr::kScale)) * ee_float(at<float>(sym::lit4_32782c));
        at<uint32_t>(s + spr::kTexture) = vfx::vfxTexture(7);
    }
    VfxPS2SpriteRotated3D(ee_float(-85.0f));
}

namespace {
// The shield's brightness for one edge: a triangle wave of the life (one byte, folded at 0x80), dimmed while the life
// is under 256.
int32_t shieldWave(uint32_t life, int32_t phase)
{
    int32_t value = phase & 0xff;
    if (!(value < 0x80))
        value = 0xff - value;
    if (life < 0x100)
        value = (value * static_cast<int32_t>(life)) >> 8;
    return value;
}
} // namespace

// The invincibility shield round a car (the car at +0x250): while it is near enough (8500) and the life is not 0xffff, a
// ring of 32 points (VU vectors from +0x50) is drawn as a band of 32 segments, each two quads high (the shield's
// bottom, its middle at 0.6 of the point and the shape's height, and its top at 0.3 and 34 higher), through the car's
// matrix and position (globalVUVec), both ways round (blue unless +0x254 is set), with brightness waves running along
// it and the texture scrolling with the life.
void VfxRenderer_Invincible(ptr32<_sVfx_Object> mem)
{
    GuestFrame frame(0x180);
    const uint32_t m = mem.address;
    const uint32_t car = at<uint32_t>(m + 0x250);
    if (!(carDistance(car) < 0x2134) || at<uint16_t>(m + 0x10) > 0xfffe)
        return;
    const uint32_t physics = at<uint32_t>(car + 0x20);
    const ee_float unit(0.00390625f);
    const uint32_t carMatrix = at<uint32_t>(physics + 0x1e8) + 0x28;
    at<float>(kGlobalVUVec) = ee_float(at<float>(physics + 8)) * unit;
    at<float>(kGlobalVUVec + 4) = ee_float(at<float>(physics + 0xc)) * unit;
    at<float>(kGlobalVUVec + 8) = ee_float(at<float>(physics + 0x10)) * unit;
    const uint32_t mat = VfxPSXToVUMatrix(ptr32<MATRIX>(carMatrix)).address;
    RdrPolyTexture(0, ptr32<_sGfxTexture>(vfx::vfxTexture(0x12)), 0x12);
    const ee_float wide(at<float>(sym::lit4_327854)), narrow(at<float>(sym::lit4_327858)), step(0.03125f), half(0.5f), one(1.0f);
    at<uint32_t>(quad() + 0x70) = 0;
    const uint32_t shape = at<uint32_t>(at<uint32_t>(car + 0x18));
    ee_float uRight = ee_itof(at<uint16_t>(m + 0x10) & 0x7f) * ee_float(0.0078125f);
    ee_float uLeft = uRight;
    const ee_float top(at<float>(quad() + 0x70));
    uint32_t v[6];
    for (uint32_t i = 0; i < 6; ++i)
        v[i] = frame.local<float>(0x10 * i).address;
    const auto corners = [&](uint32_t a, uint32_t b, uint32_t c, uint32_t d)
    {
        copyVector(quad(), v[a]);
        copyVector(quad() + 0x10, v[b]);
        copyVector(quad() + 0x20, v[c]);
        copyVector(quad() + 0x30, v[d]);
    };
    uint32_t phase = 0;
    for (uint32_t segment = 0; segment < 32; ++segment)
    {
        uint32_t life = at<uint16_t>(m + 0x10);
        const int32_t a = shieldWave(life, static_cast<int32_t>((life << 3) - (phase << 3)));
        const uint32_t nextPhase = phase + 4;
        const int32_t b = shieldWave(life, static_cast<int32_t>((life << 3) - (nextPhase << 3)));
        uint32_t c1 = static_cast<uint32_t>(a) << 8, c2 = static_cast<uint32_t>(b) << 8;
        const uint32_t from = m + 0x50 + segment * 0x10, to = m + 0x50 + ((segment + 1) & 0x1f) * 0x10;
        for (uint32_t lane = 0; lane < 12; lane += 4)
        {
            at<uint32_t>(v[0] + lane) = at<uint32_t>(from + lane);
            at<uint32_t>(v[1] + lane) = at<uint32_t>(to + lane);
        }
        copyVector(v[2], v[0]);
        sceVu0ScaleVector(ptr32<ee_float>(v[2]), ptr32<ee_float>(v[2]), wide);
        copyVector(v[3], v[1]);
        sceVu0ScaleVector(ptr32<ee_float>(v[3]), ptr32<ee_float>(v[3]), wide);
        const ee_float height = ee_itof(sh16(shape + 0x70));
        at<float>(v[3] + 4) = height;
        at<float>(v[2] + 4) = height;
        copyVector(v[4], v[2]);
        sceVu0ScaleVector(ptr32<ee_float>(v[4]), ptr32<ee_float>(v[4]), narrow);
        copyVector(v[5], v[3]);
        sceVu0ScaleVector(ptr32<ee_float>(v[5]), ptr32<ee_float>(v[5]), narrow);
        at<float>(v[4] + 4) = ee_float(at<float>(v[4] + 4)) + ee_float(34.0f);
        at<float>(v[5] + 4) = ee_float(at<float>(v[5] + 4)) + ee_float(34.0f);
        for (uint32_t i = 0; i < 6; ++i)
        {
            sceVu0ApplyMatrix(ptr32<ee_float>(v[i]), ptr32<ee_float>(mat), ptr32<ee_float>(v[i]));
            for (uint32_t lane = 0; lane < 12; lane += 4)
                at<float>(v[i] + lane) = ee_float(at<float>(v[i] + lane)) + ee_float(at<float>(kGlobalVUVec + lane));
        }
        // One way round.
        ee_float u2 = uLeft + step;
        VfxPutUV0(uLeft, top);
        VfxPutUV2(uLeft, half);
        VfxPutUV1(u2, top);
        VfxPutUV3(u2, half);
        corners(0, 1, 2, 3);
        VfxPutGT4rgba0(c1);
        VfxPutGT4rgba1(c2);
        VfxPutGT4rgba2(c1);
        VfxPutGT4rgba3(c2);
        RdrPoly(0, ptr32<_sRdrPoly>(quad()));
        VfxPutUV0(uLeft, half);
        VfxPutUV2(uLeft, one);
        VfxPutUV1(u2, half);
        VfxPutUV3(u2, one);
        corners(2, 3, 4, 5);
        VfxPutGT4rgba2(0);
        VfxPutGT4rgba3(0);
        RdrPoly(0, ptr32<_sRdrPoly>(quad()));
        // The other way round.
        life = at<uint16_t>(m + 0x10);
        const int32_t a2 = shieldWave(life, static_cast<int32_t>((life << 3) + (phase << 3)));
        const int32_t b2 = shieldWave(life, static_cast<int32_t>((life << 3) + (nextPhase << 3)));
        if (at<uint32_t>(m + 0x254) == 0)
        {
            c1 = static_cast<uint32_t>(a2) << 17;
            c2 = static_cast<uint32_t>(b2) << 17;
        }
        else
        {
            c1 = static_cast<uint32_t>(a2);
            c2 = static_cast<uint32_t>(b2);
        }
        u2 = uRight - step;
        VfxPutUV0(uRight, top);
        VfxPutUV2(uRight, half);
        uLeft = uLeft + step;
        VfxPutUV1(u2, top);
        VfxPutUV3(u2, half);
        corners(0, 1, 2, 3);
        VfxPutGT4rgba0(c1);
        VfxPutGT4rgba1(c2);
        VfxPutGT4rgba2(c1);
        VfxPutGT4rgba3(c2);
        RdrPoly(0, ptr32<_sRdrPoly>(quad()));
        VfxPutUV0(uRight, half);
        VfxPutUV2(uRight, one);
        uRight = u2;
        VfxPutUV1(u2, half);
        VfxPutUV3(u2, one);
        corners(2, 3, 4, 5);
        VfxPutGT4rgba2(0);
        VfxPutGT4rgba3(0);
        RdrPoly(0, ptr32<_sRdrPoly>(quad()));
        phase = nextPhase & 0xff;
    }
    VfxPutUV0(one, top);
    VfxPutUV1(top, top);
    VfxPutUV2(one, one);
    VfxPutUV3(top, one);
}

// The finish position over a car after the race (the car at +0x44, the position 0 to 3 at +0x1a4): while the car is
// near enough to be drawn, the position's number (a texture from the level's table) at +0x188, wobbling for its first
// frames (life under 0x417). With the race camera's flag at +0xac the number also glides from the car (bobbing above
// it) to that point and fades in. Then its dust puffs (0x14 bytes from +0x48) in the position's colour, fading out.
void Vfx2Renderer_FinishPosition(ptr32<_sVfx_Object> mem)
{
    GuestFrame frame(0xc0);
    const uint32_t m = mem.address;
    const uint32_t table = at<uint32_t>(at<uint32_t>(sym::pLvlG) + 0x40); // pLvlG's finish-position textures
    const uint32_t car = at<uint32_t>(m + 0x44);
    if (!(VfxHasCarBeenRendered(ptr32<_sObject>(car)) < 0x157c))
        return;
    at<uint8_t>(vfx::sprite() + spr::kFlipX) = 1;
    at<uint32_t>(vfx::sprite() + spr::kTexture) = at<uint32_t>(at<uint32_t>(table + 0x24 + at<uint32_t>(m + 0x1a4) * 4));
    if (at<uint16_t>(m + 0x10) < 0x417)
    {
        guest_call(kTimerGet, ptr32<uint8>(kGameTimer));
        const uint32_t time = guest_call<uint32_t>(kTimerGet, ptr32<uint8>(kGameTimer));
        const int32_t wobble = sine4096(time << 3, sym::lit4_3278d0) >> 5;
        at<uint16_t>(vfx::sprite() + spr::kAngle) = static_cast<uint16_t>(at<uint16_t>(m + 0x1a0) + wobble);
    }
    else
        at<uint16_t>(vfx::sprite() + spr::kAngle) = at<uint16_t>(m + 0x1a0);
    at<uint8_t>(vfx::sprite() + spr::kCode) = 2;
    at<float>(vfx::sprite() + spr::kScale) = 48.0f;
    const uint32_t where = frame.local<SVECTOR>(0).address;
    if (at<uint32_t>(at<uint32_t>(kCamera) + 0xac) == 0)
        std::memcpy(&at<uint8_t>(where), &at<uint8_t>(m + 0x188), 8);
    else
    {
        const int32_t life = at<uint16_t>(m + 0x10);
        int32_t mix = (life - 0x417) * 0x67;
        if (!(mix < 0x1001))
            mix = 0x1000;
        mix = atLeastZero(mix);
        if (life < 0x417)
        {
            const int32_t grey = atLeastZero(0x80 - ((0x400 - life) >> 3));
            at<uint32_t>(vfx::sprite() + spr::kRgb) = static_cast<uint32_t>(grey + (grey << 8) + (grey << 16));
            int32_t size = atLeastZero(0x417 - life);
            if (!(size < 0x21))
                size = 0x20;
            at<float>(vfx::sprite() + spr::kScale) = ee_itof(size + 0x30);
        }
        else
            at<uint32_t>(vfx::sprite() + spr::kRgb) = 0x606060;
        PS2C_Vec16MulScaler(ptr32<_sVec16>(where), ptr32<_sVec16>(m + 0x188), static_cast<int16>(mix));
        const uint32_t above = frame.local<SVECTOR>(0x10).address;
        const uint32_t state = at<uint32_t>(at<uint32_t>(car + 0x20) + 0x1e8);
        at<int16_t>(above) = static_cast<int16_t>(at<int32_t>(state + 4) >> 8);
        int32_t y = at<int32_t>(state + 8) >> 8;
        guest_call(kTimerGet, ptr32<uint8>(kGameTimer));
        const uint32_t time = guest_call<uint32_t>(kTimerGet, ptr32<uint8>(kGameTimer));
        y += (sine4096(time << 2, sym::lit4_3278d4) >> 8) + 0x64;
        const uint32_t again = at<uint32_t>(at<uint32_t>(car + 0x20) + 0x1e8);
        at<int16_t>(above + 4) = static_cast<int16_t>(at<int32_t>(again + 0xc) >> 8);
        at<int16_t>(above + 2) = static_cast<int16_t>(y);
        PS2C_Vec16MulScaler(ptr32<_sVec16>(above), ptr32<_sVec16>(above), static_cast<int16>(0x1000 - mix));
        for (uint32_t axis = 0; axis < 6; axis += 2)
            at<uint16_t>(where + axis) = static_cast<uint16_t>(at<uint16_t>(where + axis) + at<uint16_t>(above + axis));
    }
    spritePos(ee_itof(sh16(where)), ee_itof(sh16(where + 2)), ee_itof(sh16(where + 4)));
    VfxPS2SpriteRotated3D(ee_float(0.0f));
    at<uint32_t>(vfx::sprite() + spr::kTexture) = vfx::vfxTexture(0xe);
    int32_t red, green, blue;
    switch (at<int32_t>(m + 0x1a4))
    {
    case 0: red = 0x40; green = 0x40; blue = 0; break;
    case 1: red = 0x30; green = 0x30; blue = 0x30; break;
    case 2: red = 0x40; green = 0x20; blue = 0; break;
    default: red = 0; green = 0x10; blue = 0x40; break;
    }
    at<uint8_t>(vfx::sprite() + spr::kCode) = 2;
    const int32_t fade = (0x400 - static_cast<int32_t>(at<uint16_t>(m + 0x10))) >> 4;
    green = atLeastZero(green - fade);
    blue = atLeastZero(blue - fade);
    red = atLeastZero(red - fade);
    at<uint32_t>(vfx::sprite() + spr::kRgb) = static_cast<uint32_t>(red + (green << 8) + (blue << 16));
    for (uint32_t index = ringHeadU(m); index != ringTail(m); index = (index + 1) & ringMask(m))
    {
        const uint32_t p = m + 0x48 + index * 0x14;
        at<float>(vfx::sprite() + spr::kScale) = ee_itof(static_cast<int32_t>(at<uint16_t>(p + 0x10)) * 2 + 4);
        spritePos(ee_itof(sh16(where) + (sh16(p) >> 3)), ee_itof(sh16(where + 2) + (sh16(p + 2) >> 3)), ee_itof(sh16(where + 4) + (sh16(p + 4) >> 3)));
        at<uint16_t>(vfx::sprite() + spr::kAngle) = static_cast<uint16_t>(vfx::random15());
        VfxPS2SpriteRotated3D(ee_float(4.0f));
    }
}

} // namespace rc

// Forward declarations of the random-input setups (visfx_fuzz.cpp).
namespace rc::fuzz {
void vfxGiveTerrorRenderer(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxCarHeatRayBlast(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxCarWaterCoveredRenderer(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxInvincibleRenderer(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxFinishPositionRenderer(R5900Context *ctx, uint64_t &state, uint32_t scratch);
}

RC_DECOMP_FN_SETUP(VfxRenderer_GiveTerror, rc::fuzz::vfxGiveTerrorRenderer)
RC_DECOMP_FN_SETUP(VfxRenderer_CarHeatRayBlast, rc::fuzz::vfxCarHeatRayBlast)
RC_DECOMP_FN_SETUP(VfxRenderer_CarWaterCovered, rc::fuzz::vfxCarWaterCoveredRenderer)
RC_DECOMP_FN_SETUP(VfxRenderer_Invincible, rc::fuzz::vfxInvincibleRenderer)
RC_DECOMP_FN_SETUP(Vfx2Renderer_FinishPosition, rc::fuzz::vfxFinishPositionRenderer)
