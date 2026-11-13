// Decompiled from VISFX.c, continued (sixth batch): the camera set-up, the heat ray aerial, quake, water, oil, aerial,
// pickup, rocket and ready-steady-go effects. Same layout as visfx4.cpp: an effect is a `_sVfx_Object` whose own data
// starts at +0x40 with the ring header (mask, count, head, tail) and the particles from +0x44 (or later).

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
void PS2C_GteMatrixMulVec16(ptr32<_sVec16> pVec);              // psxdefs.cpp
void PS2C_SetGteMatrix(ptr32<_sMat> pMat);
void PS2C_GetVec16GteResult(ptr32<_sVec16> pOut);
void PS2C_Vec16MulScaler(ptr32<_sVec16> pOut, ptr32<_sVec16> pVec, int16 scale);
uint32 VecNormalise(ptr32<SVECTOR> pOut, ptr32<SVECTOR> pVec); // ai.cpp
bool_ AColDownCast(ptr32<_sColCellVecPSX> pPos, ptr32<_sColCellVecPSX> pPoi, ptr32<_sColNormalPSX> pNormal, ptr32<uint8> pMatId); // collision.cpp
void RdrPolyCamera(int32_t channel, ptr32<sceVu0FMATRIX> pMatrix, ptr32<sceVu0FVECTOR> pPos, ee_float dValX, ee_float dValY);

namespace {

using vfx::at;

constexpr uint32_t kRing = 0x40;      // the ring header: mask, count, head, tail
constexpr uint32_t kParticles = 0x44; // the first particle
constexpr uint32_t kNodeCalcWorldPos = 0x1339d0;
constexpr uint32_t kVisFx = 0x1f1000; // visFxVarG

// sceVu0ITOF0Vector(fx + 0x20, ints) on a local of a frame the function really has: all four integers become the effect's
// sort position, the fourth being a word of the frame nobody wrote.
void sortPosFromLocal(uint32_t fx, uint32_t ints)
{
    const __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i *>(&at<uint8_t>(ints)));
    _mm_storeu_ps(&at<float>((fx + 0x20) & ~0xFu), _mm_cvtepi32_ps(v));
}

} // namespace

// Sets up the effects' view of the camera: the view matrix as shorts (the second column negated) in
// visFxVarG.mCamInverseRot, copied for the first screen of a split screen along with the camera position, the camera
// position in whole units, and the float camera (matrix, translation, depth scale) the sprite drawing and the polygon
// builder use. (Offsets are from visFxVarG: matrix at +0xbc, its split copy at +0xdc, camera position at +0xfc (floats)
// and +0x108 (shorts), split copy +0x110, ps2CameraMatrix +0x120, its inverse +0x170, translation +0x160, depth scale
// +0x1b0 and +0x1b4.)
void VfxSetupCameraMatrix()
{
    GuestFrame frame(0xd0);
    const uint32_t s = kVisFx;
    const uint32_t view = at<uint32_t>(0x1f4808u); // view3dVarG.pPsxViewMatrix
    const auto h = [&](uint32_t offset) { return at<uint16_t>(view + offset); };
    at<uint16_t>(s + 0xbc) = h(0);
    at<uint16_t>(s + 0xc2) = h(2);
    at<uint16_t>(s + 0xc8) = h(4);
    at<uint16_t>(s + 0xbe) = static_cast<uint16_t>(-static_cast<int32_t>(h(6)));
    at<uint16_t>(s + 0xc4) = static_cast<uint16_t>(-static_cast<int32_t>(h(8)));
    at<uint16_t>(s + 0xca) = static_cast<uint16_t>(-static_cast<int32_t>(h(0xa)));
    at<uint16_t>(s + 0xc0) = h(0xc);
    at<uint16_t>(s + 0xc6) = h(0xe);
    at<uint16_t>(s + 0xcc) = h(0x10);
    if (at<uint8_t>(0x1edc68u) != 0 && at<uint32_t>(0x1edc78u) == 0) // mainVarG.splitScreen, iWhichSplitScreen
    {
        // The split copy is 32 bytes copied unaligned, as four doublewords: from +0xbc to +0xdc.
        for (uint32_t offset = 0; offset < 32; offset += 8)
            std::memcpy(&at<uint8_t>(s + 0xdc + offset), &at<uint8_t>(s + 0xbc + offset), 8);
        std::memcpy(&at<uint8_t>(s + 0x110), &at<uint8_t>(s + 0x108), 8);
    }
    const uint32_t camera = at<uint32_t>(0x1eab20u); // cameraVarG.pCamera
    const ee_float fov = ee_float(at<float>(0x1f3c3cu)) * ee_float(at<float>(0x3277a4u));
    at<uint32_t>(s + 0xfc) = at<uint32_t>(camera + 0xc);
    at<int16_t>(s + 0x108) = static_cast<int16_t>(at<int32_t>(camera + 0xc) >> 8);
    at<uint32_t>(s + 0x100) = at<uint32_t>(camera + 0x10);
    const ee_float half(0.5f);
    ee_float angle = ee_itof(ee_ftoi(fov) & 0xffff) * half;
    at<int16_t>(s + 0x10a) = static_cast<int16_t>(at<int32_t>(camera + 0x10) >> 8);
    at<uint32_t>(s + 0x104) = at<uint32_t>(camera + 0x14);
    at<int16_t>(s + 0x10c) = static_cast<int16_t>(at<int32_t>(camera + 0x14) >> 8);
    angle = ee_itof(ee_ftoi(angle) & 0xffff) * ee_float(at<float>(0x3277a8u));
    ee_float tangent = ee_sinf(angle);
    tangent = tangent / ee_cosf(angle);
    const uint32_t trans = view + 0x14;
    const ee_float tx = ee_itof(at<int32_t>(trans)), ty = ee_itof(at<int32_t>(trans + 4)), tz = ee_itof(at<int32_t>(trans + 8));
    ee_float depth = ee_float(1.0f) / tangent;
    depth = depth * ee_itof(at<int32_t>(0x1f4b30u)); // gGfxP2.wd
    depth = depth * half;
    const ee_float depthY = depth * ee_float(at<float>(0x3277acu));
    at<float>(s + 0x1b0) = depth;
    at<float>(s + 0x1b4) = depthY;
    // ps2CameraMatrix at +0x120 (the PSX matrix's 4.12 shorts as floats, row by row, w lanes zero), the translation at
    // +0x160, and the matrix's transpose at +0x170.
    const ee_float unit(0.000244140625f); // 2^-12
    static const uint32_t kElement[9][2] = {{0, 0x120}, {6, 0x124}, {0xc, 0x128}, {2, 0x130}, {8, 0x134}, {0xe, 0x138}, {4, 0x140}, {0xa, 0x144}, {0x10, 0x148}};
    for (const auto &e : kElement)
        at<float>(s + e[1]) = ee_itof(at<int16_t>(view + e[0])) * unit;
    for (uint32_t offset : {0x12cu, 0x13cu, 0x14cu, 0x150u, 0x154u, 0x158u, 0x15cu, 0x16cu})
        at<uint32_t>(s + offset) = 0;
    at<float>(s + 0x160) = tx;
    at<float>(s + 0x164) = ty;
    at<float>(s + 0x168) = tz;
    static const uint32_t kTranspose[9][2] = {{0x120, 0x170}, {0x130, 0x174}, {0x140, 0x178}, {0x124, 0x180}, {0x134, 0x184}, {0x144, 0x188}, {0x128, 0x190}, {0x138, 0x194}, {0x148, 0x198}};
    for (const auto &e : kTranspose)
        at<uint32_t>(s + e[1]) = at<uint32_t>(s + e[0]);
    for (uint32_t offset : {0x17cu, 0x18cu, 0x19cu, 0x1a0u, 0x1a4u, 0x1a8u, 0x1acu})
        at<uint32_t>(s + offset) = 0;
    VfxPutUV0(ee_float(1.0f), ee_float(0.0f));
    VfxPutUV1(ee_float(0.0f), ee_float(0.0f));
    VfxPutUV2(ee_float(1.0f), ee_float(1.0f));
    VfxPutUV3(ee_float(0.0f), ee_float(1.0f));
    RdrPolyCamera(0, ptr32<sceVu0FMATRIX>(s + 0x120), ptr32<sceVu0FVECTOR>(s + 0x160), at<float>(s + 0x1b0), at<float>(s + 0x1b4));
    RdrPolyCamera(2, ptr32<sceVu0FMATRIX>(s + 0x120), ptr32<sceVu0FVECTOR>(s + 0x160), at<float>(s + 0x1b0), at<float>(s + 0x1b4));
}

// The heat ray aerial's glow: sprites round the aerial's tip. The tip is the car's aerial node in the world (24.8 fixed
// point, through NodeCalcWorldPos) added to section 4 of the aerial's chain, in whole units, as shorts in svVelTemp. Each
// particle (0x18 bytes: three shorts of 6.4-bit offsets, a spin at +0x10, a life at +0x14, a size byte at +0x16) is drawn
// twice: first a grey flare at a random brightness, then a coloured one that fades with its life.
void VfxRenderer_CarHeatRayAerial(ptr32<_sVfx_Object> mem)
{
    GuestFrame frame(0xa0);
    const uint32_t m = mem.address;
    const uint32_t car = at<uint32_t>(m + 0x344);
    const uint16_t drawn = (at<uint32_t>(0x1ed900u) & 0x30) == 0 ? at<uint16_t>(car + 0x174) : at<int16_t>(car + 0x172) == 0 ? 100 : at<uint16_t>(car + 0x174);
    if (!(drawn < 0x157c))
        return;
    const uint32_t rendObj = at<uint32_t>(car + 0x18);
    guest_call(kNodeCalcWorldPos, ptr32<uint8>(at<uint32_t>(car + 0x3c)), ptr32<uint8>(rendObj + 0x18), ptr32<uint8>(rendObj + 4),
               frame.local<uint8>(0x10));
    const uint32_t position = frame.local<uint8>(0x10).address;
    const ee_float unit(0.00390625f); // 1/256
    const uint32_t tip = at<uint32_t>(car + 0x40) + 0x80;
    const ee_float x = ee_itof(at<int32_t>(position)) * unit, y = ee_itof(at<int32_t>(position + 4)) * unit, z = ee_itof(at<int32_t>(position + 8)) * unit;
    const ee_float tx = ee_float(at<float>(tip)) + x, ty = ee_float(at<float>(tip + 4)) + y, tz = ee_float(at<float>(tip + 8)) + z;
    SVECTOR &where = G(svVelTemp);
    where.vx = static_cast<int16>(ee_ftoi(tx));
    where.vy = static_cast<int16>(ee_ftoi(ty));
    where.vz = static_cast<int16>(ee_ftoi(tz));
    at<uint8_t>(vfx::sprite() + vfx::spr::kFlipX) = 0;
    at<uint8_t>(vfx::sprite() + vfx::spr::kCode) = 2;
    at<uint32_t>(vfx::sprite() + vfx::spr::kTexture) = vfx::vfxTexture(0xd);
    const ee_float depth(-10.0f);
    for (uint32_t index = at<uint8_t>(m + kRing + 2); index != at<uint8_t>(m + kRing + 3); index = (index + 1) & at<uint8_t>(m + kRing))
    {
        const uint32_t p = m + kParticles + index * 0x18;
        const int32_t grey = (vfx::random15() & 0x7f);
        const uint32_t sp = vfx::sprite();
        at<uint32_t>(sp + vfx::spr::kRgb) = static_cast<uint32_t>(grey) * 0x10101;
        at<float>(sp + vfx::spr::kPos) = ee_itof(where.vx + (at<int16_t>(p) >> 6));
        at<float>(sp + vfx::spr::kPos + 4) = ee_itof(where.vy + (at<int16_t>(p + 2) >> 5));
        at<float>(sp + vfx::spr::kPos + 8) = ee_itof(where.vz + (at<int16_t>(p + 4) >> 6));
        at<uint16_t>(sp + vfx::spr::kAngle) = static_cast<uint16_t>(-static_cast<int32_t>(at<uint16_t>(p + 0x10)));
        at<float>(sp + vfx::spr::kScale) = ee_itof((at<uint8_t>(p + 0x16) >> 2) + 2);
        VfxPS2SpriteRotated3D(depth);
    }
    at<uint8_t>(vfx::sprite() + vfx::spr::kCode) = 2;
    at<uint32_t>(vfx::sprite() + vfx::spr::kTexture) = vfx::vfxTexture(1);
    for (uint32_t index = at<uint8_t>(m + kRing + 2); index != at<uint8_t>(m + kRing + 3); index = (index + 1) & at<uint8_t>(m + kRing))
    {
        const uint32_t p = m + kParticles + index * 0x18;
        const uint32_t sp = vfx::sprite();
        at<float>(sp + vfx::spr::kPos) = ee_itof(where.vx + (at<int16_t>(p) >> 6));
        const int32_t blue = 0x3c - static_cast<int32_t>(at<uint16_t>(p + 0x14)) * 4;
        const int32_t red = std::max(0x82 - blue, 0), green = std::max(0x82 - blue * 2, 0);
        at<float>(sp + vfx::spr::kPos + 4) = ee_itof(where.vy + (at<int16_t>(p + 2) >> 6));
        at<float>(sp + vfx::spr::kPos + 8) = ee_itof(where.vz + (at<int16_t>(p + 4) >> 6));
        at<uint16_t>(sp + vfx::spr::kAngle) = at<uint16_t>(p + 0x10);
        at<uint32_t>(sp + vfx::spr::kRgb) = static_cast<uint32_t>(red + green * 0x100 + blue * 0x10000);
        at<float>(sp + vfx::spr::kScale) = ee_itof(at<uint8_t>(p + 0x16) + 8);
        VfxPS2SpriteRotated3D(depth);
    }
}

// The second quake: the first (VfxRenderer_Quake) with other colours and sizes. A ring of points (floats from +0x50, 0x30
// bytes each) round the centre (shorts at +0x654), drawn as a band of quads from each point to the next (wrapping round
// by the ring's mask, as a remainder), the texture sliding along with the effect's life. Nothing is drawn on the one
// frame the life is 0x20.
void VfxRenderer_Quake2(ptr32<_sVfx_Object> mem)
{
    GuestFrame frame(0xe0);
    const uint32_t m = mem.address;
    const uint32_t life = at<uint16_t>(m + 0x10);
    if (life == 0x20)
        return;
    const uint32_t centre = frame.local<float>(0).address; // vuCentre; its fourth lane is a word of the stack nobody wrote
    int32_t fade = static_cast<int32_t>(life * static_cast<uint32_t>(-4) + 0x5a);
    fade = fade < 0 ? 0 : fade;
    const int32_t green = std::max(fade * -2 + 0x5a, 0), blue = std::max(fade * -4 + 0xbe, 0);
    at<float>(centre) = ee_itof(at<int16_t>(m + 0x654));
    at<float>(centre + 4) = ee_itof(at<int16_t>(m + 0x656));
    at<float>(centre + 8) = ee_itof(at<int16_t>(m + 0x658));
    const uint32_t colour = static_cast<uint32_t>((0x5a - (fade >> 1)) + green * 0x100 + blue * 0x10000);
    ee_float slide = ee_itof(static_cast<int32_t>(life)) * ee_float(at<float>(0x32787cu));
    const uint8_t head = at<uint8_t>(m + kRing + 2);
    const uint32_t quad = G(visFxVarG).pGT4.address;
    RdrPolyTexture(0, ptr32<_sGfxTexture>(vfx::vfxTexture(0x10)), 0x12);
    const auto scale = [](float factor, uint32_t dst, uint32_t src)
    { guest_call(0x1c8338u, ee_float(factor), ptr32<ee_float>(dst), ptr32<ee_float>(src)); }; // sceVu0ScaleVectorXYZ
    if (head != at<uint8_t>(m + kRing + 3))
    {
        const ee_float one(1.0f);
        uint32_t index = head;
        do
        {
            const uint32_t from = m + 0x50 + index * 0x30u;
            const float outer = at<float>(0x327880u);
            vfx::vu0Add(quad, centre, from);
            const uint32_t mask = at<uint8_t>(m + kRing);
            // The original divides by the ring's mask and traps on a zero one; the inputs never have one.
            const uint32_t to = m + 0x50 + ((index + 1) % mask) * 0x30u;
            vfx::vu0Add(quad + 0x10, centre, to);
            scale(outer, quad + 0x20, from);
            vfx::vu0Add(quad + 0x20, centre, quad + 0x20);
            scale(outer, quad + 0x30, to);
            vfx::vu0Add(quad + 0x30, centre, quad + 0x30);
            const ee_float lowerRight = at<float>(quad + 0x34);
            const ee_float next = slide + ee_float(at<float>(0x327884u));
            at<float>(quad + 0x24) = ee_float(at<float>(quad + 0x24)) + ee_float(200.0f);
            at<float>(quad + 0x34) = lowerRight + ee_float(200.0f);
            VfxPutGT4rgba3(colour);
            VfxPutUV0(slide, ee_float(0.0f));
            VfxPutUV1(next, ee_float(0.0f));
            VfxPutUV2(slide, one);
            VfxPutUV3(next, one);
            VfxPutGT4rgba0(colour);
            VfxPutGT4rgba1(colour);
            VfxPutGT4rgba2(colour);
            RdrPoly(0, ptr32<_sRdrPoly>(quad));
            VfxPutGT4rgba1(0);
            VfxPutGT4rgba0(0);
            scale(0.5f, quad, from);
            vfx::vu0Add(quad, centre, quad);
            scale(0.5f, quad + 0x10, to);
            vfx::vu0Add(quad + 0x10, centre, quad + 0x10);
            const ee_float upperRight = at<float>(quad + 0x14);
            at<float>(quad + 4) = ee_float(at<float>(quad + 4)) + ee_float(300.0f);
            at<float>(quad + 0x14) = upperRight + ee_float(300.0f);
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

// The splash of a boat or a dropped object in water: a spray of sprites (or, for the new drops, camera-facing streaks)
// and a ring on the water. The effect's centre is three shorts at +0x544 and the water's height at +0x54e. Each
// particle (0x14 bytes: three shorts of 12.4 offsets, a flag at +0x12) is a streak while it is rising (flag zero), whose
// colour and width come from how high above the water it is, then a sprite of falling spray (random spin, size).
void VfxRenderer_WaterSploosh(ptr32<_sVfx_Object> mem)
{
    GuestFrame frame(0xb0);
    const uint32_t m = mem.address;
    const ptr32<int16_t> sv = frame.local<int16_t>(0x10); // the particle's position, in whole units
    for (uint32_t index = at<uint8_t>(m + kRing + 2); index != at<uint8_t>(m + kRing + 3);)
    {
        const uint32_t p = m + kParticles + index * 0x14;
        const int32_t height = at<int16_t>(p + 2);
        const int16_t x = static_cast<int16_t>(at<uint16_t>(m + 0x544) + (at<int16_t>(p) >> 4));
        const int16_t y = static_cast<int16_t>(at<uint16_t>(m + 0x546) + (height >> 4));
        const int16_t z = static_cast<int16_t>(at<uint16_t>(m + 0x548) + (at<int16_t>(p + 4) >> 4));
        sv[0] = x;
        sv[1] = y;
        sv[2] = z;
        const int32_t water = at<int16_t>(m + 0x54e);
        if (at<uint16_t>(p + 0x12) == 0)
        {
            const int32_t bright = std::min((height - water) >> 6, 0x5a);
            VfxPutGT4rgba2(static_cast<uint32_t>(bright) * 0x10101);
            VfxPutGT4rgba3(static_cast<uint32_t>(bright) * 0x10101);
            const int32_t red = std::min((height - water) >> 9, 0x10);
            VfxPutGT4rgba0(static_cast<uint32_t>(red) << 16);
            VfxPutGT4rgba1(static_cast<uint32_t>(red) << 16);
            VfxPutGT4FacingPoly(ptr32<SVECTOR>(m + 0x544), ptr32<SVECTOR>(sv.address), ptr32<_sGfxTexture>(vfx::vfxTexture(10)), 2,
                                ee_itof(at<uint16_t>(m + 0x10)) + ee_float(10.0f), ee_float(-10.0f));
        }
        else
        {
            int32_t blue = (height - water) >> 4;
            blue = blue < 0x47 ? blue : 0x46;
            blue = blue > -1 ? blue : 0;
            int32_t green = blue - 0x1e;
            green = green > -1 ? green : 0;
            const uint32_t sp = vfx::sprite();
            at<uint8_t>(sp + vfx::spr::kCode) = 2;
            at<float>(sp + vfx::spr::kPos) = ee_itof(x);
            at<float>(sp + vfx::spr::kPos + 4) = ee_itof(y);
            at<float>(sp + vfx::spr::kPos + 8) = ee_itof(z);
            at<uint16_t>(sp + vfx::spr::kAngle) = static_cast<uint16_t>(vfx::random15());
            const int32_t sizeBits = vfx::random15() & 0xf;
            const uint32_t s2 = vfx::sprite();
            at<float>(s2 + vfx::spr::kScale) = ee_itof((sizeBits + at<uint16_t>(m + 0x10)) >> 1) + ee_float(8.0f);
            at<uint32_t>(s2 + vfx::spr::kRgb) = static_cast<uint32_t>(green * 0x100 + (blue << 16));
            at<uint32_t>(s2 + vfx::spr::kTexture) = vfx::vfxTexture(0x1c);
            VfxPS2SpriteRotated3D(ee_float(0.0f));
        }
        index = (index + 1) & at<uint8_t>(m + kRing);
    }
    const uint32_t life = at<uint16_t>(m + 0x10);
    const uint32_t sp = vfx::sprite();
    at<uint8_t>(sp + vfx::spr::kFlipX) = 0;
    at<uint16_t>(sp + vfx::spr::kAngle) = 0;
    at<float>(sp + vfx::spr::kPos) = ee_itof(at<int16_t>(m + 0x544));
    const uint32_t colour = life * 0x202 + life * 0x20000;
    at<float>(sp + vfx::spr::kPos + 4) = ee_itof(at<int16_t>(m + 0x546));
    at<uint8_t>(sp + vfx::spr::kCode) = 2;
    at<float>(sp + vfx::spr::kPos + 8) = ee_itof(at<int16_t>(m + 0x548));
    const uint32_t s2 = vfx::sprite();
    at<uint32_t>(s2 + vfx::spr::kRgb) = colour;
    at<float>(s2 + vfx::spr::kScale) = ee_itof(0x8c - static_cast<int32_t>(life * 2));
    at<uint32_t>(s2 + vfx::spr::kTexture) = vfx::vfxTexture(1);
    VfxPS2SpriteGroundRotated3D(ptr32<SVECTOR>(m + 0x14), ee_float(-20.0f));
    const uint32_t s3 = vfx::sprite();
    at<uint32_t>(s3 + vfx::spr::kRgb) = colour;
    at<float>(s3 + vfx::spr::kScale) = ee_itof(at<uint16_t>(m + 0x10)) + ee_float(16.0f);
    VfxPS2SpriteRotated3D(ee_float(0.0f));
}

// The oil slick on the road: a ring of blobs (0x18 bytes: position shorts, a spin at +0x10, a colour at +0x12..+0x13 and
// +0x17 (signed bytes), a size at +0x16, a ground normal at +8 and a fade-in at +0x14). Each is drawn flat on the road in
// the oil's colour brightened by its fade-in (less the effect's age beyond 0xd0), then again as a sheen: half as bright,
// or one time in eight a random glint that wanders a little.
void VfxRenderer_OilSlick(ptr32<_sVfx_Object> mem)
{
    GuestFrame frame(0xb0);
    const uint32_t m = mem.address;
    at<uint8_t>(vfx::sprite() + vfx::spr::kFlipX) = 0;
    at<uint8_t>(vfx::sprite() + vfx::spr::kCode) = 2;
    const ee_float depth(-20.0f), scale(4.656613e-10f); // 2^-31
    for (uint32_t index = at<uint8_t>(m + kRing + 2); index != at<uint8_t>(m + kRing + 3); index = (index + 1) & at<uint8_t>(m + kRing))
    {
        const uint32_t p = m + kParticles + index * 0x18;
        if (at<uint16_t>(p + 0x14) == 0)
            continue;
        const uint32_t sp = vfx::sprite();
        at<uint16_t>(sp + vfx::spr::kAngle) = at<uint16_t>(p + 0x10);
        const uint32_t grey = vfx::vfxTexture(3);
        at<float>(sp + vfx::spr::kPos) = ee_itof(at<int16_t>(p));
        int32_t fade = static_cast<int32_t>((at<uint16_t>(m + 0x10) - 0xd0u) << 6);
        fade = fade > -1 ? fade : 0;
        int32_t glow = static_cast<int32_t>(static_cast<uint32_t>(at<uint16_t>(p + 0x14)) * 4) - fade;
        glow = glow < 0x29 ? glow : 0x28;
        const uint16_t tint = at<uint16_t>(p + 0x12);
        int32_t green = static_cast<int8_t>(tint >> 8) + glow;
        int32_t red = (tint & 0xff) + glow;
        at<float>(sp + vfx::spr::kPos + 4) = ee_itof(at<int16_t>(p + 2));
        green = green > -1 ? green : 0;
        red = red > -1 ? red : 0;
        at<float>(sp + vfx::spr::kPos + 8) = ee_itof(at<int16_t>(p + 4));
        int32_t blue = at<int8_t>(p + 0x17) + glow;
        at<uint32_t>(sp + vfx::spr::kTexture) = grey;
        blue = blue > -1 ? blue : 0;
        at<uint32_t>(sp + vfx::spr::kRgb) = static_cast<uint32_t>(red + green * 0x100 + blue * 0x10000);
        at<uint8_t>(sp + vfx::spr::kCode) = 4;
        at<float>(sp + vfx::spr::kScale) = ee_itof(at<uint8_t>(p + 0x16));
        VfxPS2SpriteGroundRotated3D(ptr32<SVECTOR>(p + 8), depth);
        const uint32_t s2 = vfx::sprite();
        at<uint8_t>(s2 + vfx::spr::kCode) = 2;
        at<uint32_t>(s2 + vfx::spr::kTexture) = vfx::vfxTexture(0x1a);
        uint32_t s3 = s2;
        if ((vfx::random15() & 7) == 0)
        {
            red = std::max(red - 8 - (vfx::random15() & 0x1f), 0);
            green = std::max(green - 8 - (vfx::random15() & 0x1f), 0);
            blue = std::max(blue - 8 - (vfx::random15() & 0x1f), 0);
            const int32_t rx = rand();
            const ee_float dx = ee_itof(rx) * scale;
            at<float>(vfx::sprite() + vfx::spr::kPos) = ee_float(at<float>(vfx::sprite() + vfx::spr::kPos)) + ((dx + dx) - ee_float(1.0f));
            const int32_t rz = rand();
            const ee_float dz = ee_itof(rz) * scale;
            s3 = vfx::sprite();
            at<float>(s3 + vfx::spr::kPos + 8) = ee_float(at<float>(s3 + vfx::spr::kPos + 8)) + ((dz + dz) - ee_float(1.0f));
        }
        else
        {
            red >>= 1;
            green >>= 1;
            blue >>= 1;
            s3 = vfx::sprite();
        }
        at<uint32_t>(s3 + vfx::spr::kRgb) = static_cast<uint32_t>(red + green * 0x100 + blue * 0x10000);
        VfxPS2SpriteGroundRotated3D(ptr32<SVECTOR>(p + 8), depth);
    }
}

// The aerial of a car or a bike as a ribbon of five segments through six points of the effect (vectors at +0x40, +0x50,
// +0x60, +0x70, +0x80, +0x90), each moved by the aerial's tip position in the world (the car's aerial node through
// NodeCalcWorldPos, in 24.8). Grey, except the last segment, which is in the effect's colour (at +0x48 of the car). Not
// drawn when the car is out of range, or on the level 0x62 for a car that is hidden.
void VfxRenderer_Aerial(ptr32<_sVfx_Object> mem)
{
    GuestFrame frame(0x220);
    const uint32_t m = mem.address;
    const uint32_t car = at<uint32_t>(m + 0xb0);
    if (at<int16_t>(car + 0x172) != 0 && at<uint8_t>(0x1edc4au) == 0x62) // mainVarG.level
        return;
    const uint32_t rendObj = at<uint32_t>(car + 0x18);
    guest_call(kNodeCalcWorldPos, ptr32<uint8>(at<uint32_t>(car + 0x3c)), ptr32<uint8>(rendObj + 0x18), ptr32<uint8>(rendObj + 4),
               frame.local<uint8>(0x10));
    const uint32_t position = frame.local<uint8>(0x10).address;
    const uint32_t where = frame.local<float>(0x20).address; // vuPosition; its fourth lane is a word of the stack nobody wrote
    const ee_float unit(0.00390625f);
    const ee_float x = ee_itof(at<int32_t>(position)) * unit, y = ee_itof(at<int32_t>(position + 4)) * unit, z = ee_itof(at<int32_t>(position + 8)) * unit;
    at<float>(where) = x;
    at<float>(where + 4) = y;
    const uint16_t drawn = (at<uint32_t>(0x1ed900u) & 0x30) == 0 ? at<uint16_t>(car + 0x174) : at<int16_t>(car + 0x172) == 0 ? 100 : at<uint16_t>(car + 0x174);
    at<float>(where + 8) = z;
    if (!(drawn < 0x157c))
        return;
    const uint32_t sect = frame.local<float>(0x110).address; // six points, 16 bytes each
    for (int32_t i = 5; i >= 0; --i)
    {
        sceVu0CopyVector(ptr32<ee_float>(sect + 0x10 * i), ptr32<ee_float>(m + 0x40 + 0x10 * i));
        vfx::vu0Add(sect + 0x10 * i, where, sect + 0x10 * i);
    }
    const auto grey = [] { for (auto put : {VfxPutGT4rgba0, VfxPutGT4rgba1, VfxPutGT4rgba2, VfxPutGT4rgba3}) put(0x80505050u); };
    const auto tip = [&] { for (auto put : {VfxPutGT4rgba0, VfxPutGT4rgba1, VfxPutGT4rgba2, VfxPutGT4rgba3}) put(at<uint32_t>(car + 0x48) | 0x80000000u); };
    const ee_float width(1.5f), zOffset(at<float>(0x3280e0u));
    const uint32_t texture = vfx::vfxTexture(0x18);
    grey();
    VfxPutGT4FacingPolyVU(ptr32<ee_float>(sect), ptr32<ee_float>(sect + 0x10), ptr32<_sGfxTexture>(texture), 0x20, width, zOffset);
    // Then the segments continue in pairs (points 1-2 and 2-3, then 3-4 and 4-5); the last is in the effect's colour.
    uint32_t a = sect + 0x10, b = sect + 0x20;
    for (int32_t i = 1; i < 5; i += 2)
    {
        grey();
        if (i == 4)
            tip();
        VfxPutGT4FacingContinueVU(ptr32<ee_float>(a), ptr32<ee_float>(b));
        grey();
        if (i + 1 == 4)
            tip();
        VfxPutGT4FacingContinueVU(ptr32<ee_float>(a + 0x10), ptr32<ee_float>(b + 0x10));
        a += 0x20;
        b += 0x20;
    }
}

// The spinning pickup flying to the player's HUD: the pickup's icon (the weapon sprite, flipped) travels from where it
// was picked up to the HUD's pickup box (hudVarG.iPickupX/Y for the screen, put through the inverse camera so it sits in
// front of the camera), and a ring of 0x14-byte sparkles follows it. Only drawn for the player (the effect's car at +0x44:
// type 2 means no one) of the screen being drawn, when that car is near enough to be drawn.
void VfxRenderer_SpinPickup(ptr32<_sVfx_Object> mem)
{
    GuestFrame frame(0x90);
    const uint32_t m = mem.address;
    const uint32_t hudRes = at<uint32_t>(at<uint32_t>(0x327e80u) + 0x40); // pLvlG->pHudRes
    const uint32_t car = at<uint32_t>(m + 0x44);
    if (at<uint8_t>(car + 0x44) == 2)
        return;
    const uint32_t screen = at<uint32_t>(0x1edc78u); // mainVarG.iWhichSplitScreen
    if (screen == 0 ? at<uint8_t>(car + 0x45) != 0 : at<uint8_t>(car + 0x45) != 1)
        return;
    const uint16_t drawn = (at<uint32_t>(0x1ed900u) & 0x30) == 0 ? at<uint16_t>(car + 0x174) : at<int16_t>(car + 0x172) == 0 ? 100 : at<uint16_t>(car + 0x174);
    if (!(drawn < 0x157c))
        return;
    const uint32_t sp = vfx::sprite();
    at<uint8_t>(sp + vfx::spr::kFlipX) = 1;
    at<uint16_t>(sp + vfx::spr::kAngle) = at<uint16_t>(m + 0xf8);
    at<float>(sp + vfx::spr::kPos) = ee_itof(at<int16_t>(m + 0xe8));
    at<float>(sp + vfx::spr::kPos + 4) = ee_itof(at<int16_t>(m + 0xea));
    at<uint32_t>(sp + vfx::spr::kRgb) = 0x80808080;
    at<uint8_t>(sp + vfx::spr::kCode) = 0;
    at<float>(sp + vfx::spr::kScale) = 16.0f;
    at<float>(sp + vfx::spr::kPos + 8) = ee_itof(at<int16_t>(m + 0xec));
    const uint32_t hud = 0x1ecda0u + screen * 4; // hudVarG
    const uint32_t offset = frame.local<float>(0).address, pos = frame.local<float>(0x10).address;
    at<float>(offset) = ee_itof(at<int32_t>(hud + 0x84) - 0x140) * ee_float(0.5f);
    if (at<uint8_t>(0x1edc68u) == 0) // mainVarG.splitScreen
        at<float>(offset + 4) = ee_itof(0x78 - at<int32_t>(hud + 0x8c));
    else
        at<float>(offset + 4) = ee_itof(0x78 - at<int32_t>(hud + 0x8c)) * ee_float(at<float>(0x327810u));
    at<float>(offset + 8) = 270.0f;
    sceVu0ApplyMatrix(ptr32<ee_float>(offset), ptr32<ee_float>(kVisFx + 0x170), ptr32<ee_float>(offset));
    vfx::vu0Add(offset, kVisFx + 0x160, offset);
    const ee_float t = ee_float(1.0f) - ee_itof(at<uint16_t>(m + 0x10)) * ee_float(0.015625f);
    guest_call(0x1c8308u, t, ptr32<ee_float>(pos), ptr32<ee_float>(offset), ptr32<ee_float>(sp + vfx::spr::kPos)); // sceVu0InterVectorXYZ
    guest_call(0x1c82e8u, ptr32<ee_float>(sp + vfx::spr::kPos), ptr32<ee_float>(pos));                              // sceVu0CopyVectorXYZ
    at<uint32_t>(sp + vfx::spr::kTexture) = at<uint32_t>(at<uint32_t>(hudRes + 0x80));
    VfxPS2SpriteRotated3D(ee_float(0.0f));
    const uint32_t s2 = vfx::sprite();
    at<uint8_t>(s2 + vfx::spr::kCode) = 2;
    at<uint32_t>(s2 + vfx::spr::kTexture) = vfx::vfxTexture(0xe);
    at<uint32_t>(s2 + vfx::spr::kRgb) = 0x80808020;
    for (uint32_t index = at<uint8_t>(m + kRing + 2); index != at<uint8_t>(m + kRing + 3); index = (index + 1) & at<uint8_t>(m + kRing))
    {
        const uint32_t p = m + 0x48 + index * 0x14;
        const uint32_t s = vfx::sprite();
        at<float>(s + vfx::spr::kScale) = ee_itof(at<uint16_t>(p + 0x10) + 6);
        at<float>(s + vfx::spr::kPos) = ee_float(at<float>(pos)) + ee_itof(at<int16_t>(p) >> 4);
        at<float>(s + vfx::spr::kPos + 4) = ee_float(at<float>(pos + 4)) + ee_itof(at<int16_t>(p + 2) >> 4);
        at<float>(s + vfx::spr::kPos + 8) = ee_float(at<float>(pos + 8)) + ee_itof(at<int16_t>(p + 4) >> 4);
        at<uint16_t>(vfx::sprite() + vfx::spr::kAngle) = static_cast<uint16_t>(vfx::random15());
        VfxPS2SpriteRotated3D(ee_float(0.0f));
    }
}

// The ribbon trailing a car's aerial: a ring of 0x18-byte particles (position, velocity and a life at +0x14), each with
// a three-byte trail offset in a parallel array at +0x104 (four bytes each). Each frame the particles fall and drift, the
// trail bytes of one in eight jitter, and the lives run down; a dead head particle is dropped, and while the ring is not
// full a new one is started at the aerial's tip (section 5): its trail offset is the tip minus section 3, and its
// velocity is the car's own (damped) plus a pull along its up vector, normalised, in 1/64.
void VfxHandler_CarAerialRibbon(ptr32<_sVfx_Object> mem)
{
    GuestFrame frame(0xe0);
    const uint32_t m = mem.address;
    const uint32_t trail = m + 0x104, particles = m + kParticles, ring = m + kRing;
    const uint32_t car = at<uint32_t>(m + 0x124);
    const ptr32<uint8_t> tipSection = frame.local<uint8_t>(0), farSection = frame.local<uint8_t>(0x10); // sp+0 and sp+0x10
    const ptr32<int16_t> velocity = frame.local<int16_t>(0x20);
    const auto section = [&](uint32_t where, int32_t n)
    {
        const uint64_t v = VfxCarAerialVal(ptr32<_sObject>(car), n);
        std::memcpy(&at<uint8_t>(where), &v, 8);
    };
    section(tipSection.address, 5);
    int32_t index = static_cast<int8_t>(at<uint8_t>(ring + 2));
    if (index != at<uint8_t>(ring + 3))
    {
        do
        {
            const uint32_t p = particles + static_cast<uint32_t>(index * 0x18);
            const uint16_t vy = at<uint16_t>(p + 0xa);
            at<uint16_t>(p + 2) = static_cast<uint16_t>(at<uint16_t>(p + 2) + vy);
            at<uint16_t>(p) = static_cast<uint16_t>(at<uint16_t>(p) + at<uint16_t>(p + 8));
            at<uint16_t>(p + 4) = static_cast<uint16_t>(at<uint16_t>(p + 4) + at<uint16_t>(p + 0xc));
            at<uint16_t>(p + 0xa) = static_cast<uint16_t>(vy - 8);
            if ((vfx::random15() & 7) == 0)
            {
                const uint32_t entry = trail + static_cast<uint32_t>(index * 4);
                at<uint8_t>(entry) = static_cast<uint8_t>(at<uint8_t>(entry) - 1 + ((rand() >> 14) & 2));
                at<uint8_t>(entry + 2) = static_cast<uint8_t>(at<uint8_t>(entry + 2) - 1 + ((rand() >> 14) & 2));
            }
            at<uint16_t>(p + 0x14) = static_cast<uint16_t>(at<uint16_t>(p + 0x14) - 1);
            index = static_cast<int8_t>(at<uint8_t>(ring) & static_cast<uint8_t>(index + 1));
        } while (index != at<uint8_t>(ring + 3));
    }
    const uint8_t mask = at<uint8_t>(ring);
    const uint8_t head = at<uint8_t>(ring + 2);
    uint8_t count;
    if (at<uint16_t>(particles + head * 0x18u + 0x14) == 0 && head != at<uint8_t>(ring + 3))
    {
        at<uint8_t>(ring + 1) = static_cast<uint8_t>(at<uint8_t>(ring + 1) - 1);
        at<uint8_t>(ring + 2) = static_cast<uint8_t>(mask & (head + 1));
    }
    count = at<uint8_t>(ring + 1);
    if (!(count < mask))
        return;
    section(farSection.address, 3);
    const uint8_t tail = at<uint8_t>(ring + 3);
    const uint32_t entry = trail + tail * 4u;
    at<uint8_t>(entry) = static_cast<uint8_t>(tipSection[0] - farSection[0]);
    at<uint8_t>(entry + 1) = static_cast<uint8_t>(tipSection[2] - farSection[2]);
    at<uint8_t>(entry + 2) = static_cast<uint8_t>(tipSection[4] - farSection[4]);
    const uint32_t previous = trail + static_cast<uint32_t>(static_cast<int8_t>(mask & static_cast<uint8_t>(at<uint8_t>(ring + 3) - 1))) * 4u;
    at<uint8_t>(previous + 1) = static_cast<uint8_t>(at<uint8_t>(previous + 1) - 10);
    // The car's own velocity (a vector of four shorts at +0x1c of its integer state), damped, less its matrix's last column.
    const uint32_t integer = at<uint32_t>(at<uint32_t>(car + 0x20) + 0x1e8);
    for (uint32_t lane = 0; lane < 4; ++lane)
        velocity[lane] = at<int16_t>(integer + 0x1c + 2 * lane);
    velocity[0] = static_cast<int16_t>((-static_cast<int32_t>(velocity[0]) >> 7) - (at<int16_t>(integer + 0x28 + 4) >> 10));
    velocity[1] = static_cast<int16_t>(((-static_cast<int32_t>(velocity[1]) >> 7) - (at<int16_t>(integer + 0x28 + 0xa) >> 10)) - 10);
    velocity[2] = static_cast<int16_t>((-static_cast<int32_t>(velocity[2]) >> 7) - (at<int16_t>(integer + 0x28 + 0x10) >> 10));
    std::memcpy(&at<uint8_t>(m + 0x128), &at<uint8_t>(tipSection.address), 8);
    VecNormalise(ptr32<SVECTOR>(velocity.address), ptr32<SVECTOR>(velocity.address));
    const uint32_t fresh = particles + static_cast<uint32_t>(at<uint8_t>(ring + 3)) * 0x18;
    at<uint16_t>(fresh) = 0;
    at<uint16_t>(fresh + 4) = 0;
    at<uint16_t>(fresh + 2) = 0;
    at<uint16_t>(fresh + 8) = static_cast<uint16_t>(velocity[0] >> 6);
    at<uint16_t>(fresh + 0xa) = static_cast<uint16_t>(velocity[1] >> 6);
    at<uint16_t>(fresh + 0xc) = static_cast<uint16_t>(velocity[2] >> 6);
    at<uint16_t>(fresh + 0x14) = 7;
    at<uint8_t>(ring + 1) = static_cast<uint8_t>(at<uint8_t>(ring + 1) + 1);
    at<uint8_t>(ring + 3) = static_cast<uint8_t>(at<uint8_t>(ring) & (at<uint8_t>(ring + 3) + 1));
}

// The water ball flying through the air: drops of 0x30 bytes (position floats at +0, velocity floats at +0x10, the
// ground's height under it at +0x20, a 32-bit life at +0x24) from +0x50. Three new drops start each frame (while the ring
// has room) at a random point within 25 units of the ball's position (shorts at +0x14), with the ground height found by
// a collision query straight down (or -1000 when there is none) and a random sideways speed. A drop falls with gravity
// until it is within 10 units of the ground, then it stops and splashes for five more frames. The sort position is the
// ball's position.
void VfxHandler_WaterBallFlying(ptr32<_sVfx_Object> mem)
{
    GuestFrame frame(0xf0);
    const uint32_t m = mem.address;
    const uint32_t ring = m + kRing, particles = m + 0x50;
    const ptr32<float> hit = frame.local<float>(0);      // vu: the ground point the query found
    const ptr32<float> normal = frame.local<float>(0x10); // vu2
    const uint32_t ints = frame.local<int32_t>(0x20).address;
    at<int32_t>(ints) = at<int16_t>(m + 0x14);
    at<int32_t>(ints + 4) = at<int16_t>(m + 0x16);
    at<int32_t>(ints + 8) = at<int16_t>(m + 0x18);
    sortPosFromLocal(m, ints);
    at<int32_t>(m + 0x654) = at<int32_t>(m + 0x654) + 0x37;
    const ee_float scale(at<float>(0x327844u)), spread(2.3283064e-08f), half(25.0f);
    uint32_t tail = at<uint8_t>(ring + 3);
    for (int32_t pass = 2; pass >= 0; --pass)
    {
        if (!(at<uint8_t>(ring + 1) < at<uint8_t>(m + 0x40)))
            continue;
        const uint32_t p = particles + tail * 0x30;
        at<float>(p) = (ee_itof(rand()) * spread - half) + ee_itof(at<int16_t>(m + 0x14));
        at<float>(p + 4) = (ee_itof(rand()) * spread - half) + ee_itof(at<int16_t>(m + 0x16));
        const ee_float z = (ee_itof(rand()) * spread - half) + ee_itof(at<int16_t>(m + 0x18));
        at<float>(p + 8) = z;
        const bool found = AColDownCast(ptr32<_sColCellVecPSX>(p), ptr32<_sColCellVecPSX>(hit.address), ptr32<_sColNormalPSX>(normal.address),
                                        frame.local<uint8>(0x30));
        at<float>(p + 0x20) = found ? at<float>(hit.address + 4) : -1000.0f;
        at<uint32_t>(p + 0x24) = 0x20;
        at<uint32_t>(p + 0x14) = 0;
        at<float>(p + 0x10) = ee_itof(static_cast<int32_t>((vfx::random15() & 0x3f)) - 0x1f) * scale;
        at<float>(p + 0x18) = ee_itof(static_cast<int32_t>((vfx::random15() & 0x3f)) - 0x1f) * scale;
        at<uint8_t>(ring + 1) = static_cast<uint8_t>(at<uint8_t>(ring + 1) + 1);
        tail = at<uint8_t>(ring) & (tail + 1);
        at<uint8_t>(ring + 3) = static_cast<uint8_t>(tail);
    }
    uint32_t index = at<uint8_t>(ring + 2);
    if (index != tail)
    {
        do
        {
            const uint32_t p = particles + index * 0x30;
            const ee_float y = ee_float(at<float>(p + 4)) + ee_float(at<float>(p + 0x14));
            const ee_float ground = ee_float(at<float>(p + 0x20)) + ee_float(10.0f);
            const ee_float x = ee_float(at<float>(p)) + ee_float(at<float>(p + 0x10));
            const ee_float z = ee_float(at<float>(p + 8)) + ee_float(at<float>(p + 0x18));
            const uint32_t life = at<uint32_t>(p + 0x24) - 1;
            at<float>(p + 4) = y;
            at<float>(p) = x;
            at<float>(p + 8) = z;
            at<uint32_t>(p + 0x24) = life;
            if (!(y <= ground) || life < 6)
                at<float>(p + 0x14) = ee_float(at<float>(p + 0x14)) - ee_float(0.5f);
            else
            {
                at<uint32_t>(p + 0x14) = 0;
                at<uint32_t>(p + 0x24) = 5;
            }
            if (at<uint32_t>(p + 0x24) == 0)
            {
                const uint32_t head = at<uint8_t>(ring + 2);
                if (index != head)
                    std::memcpy(&at<uint8_t>(p), &at<uint8_t>(particles + head * 0x30), 0x30);
                at<uint8_t>(ring + 1) = static_cast<uint8_t>(at<uint8_t>(ring + 1) - 1);
                at<uint8_t>(ring + 2) = static_cast<uint8_t>(at<uint8_t>(ring) & (at<uint8_t>(ring + 2) + 1));
            }
            index = (index + 1) & at<uint8_t>(ring);
        } while (index != at<uint8_t>(ring + 3));
    }
    at<uint16_t>(m + 0x10) = static_cast<uint16_t>(at<uint16_t>(m + 0x10) - 1);
}

// The water ball: three small blobs orbiting the ball's centre (sines of the effect's life, each its own speed), the
// ball itself (two rotating sprites of the water texture and a whiter one), and each drop as a bluish sprite that
// grows and darkens while it splashes. The last sprite is a tail call to VfxPS2SpriteRotated3D (the frame is gone).
void VfxRenderer_WaterBallFlying(ptr32<_sVfx_Object> mem)
{
    const uint32_t m = mem.address;
    {
        GuestFrame frame(0xc0);
        const ee_float turn(at<float>(0x327848u)), range(4096.0f);
        at<uint8_t>(vfx::sprite() + vfx::spr::kFlipX) = 0;
        at<uint8_t>(vfx::sprite() + vfx::spr::kCode) = 2;
        const uint32_t sp = vfx::sprite();
        at<uint32_t>(sp + vfx::spr::kRgb) = 0x787878;
        at<uint32_t>(sp + vfx::spr::kTexture) = vfx::vfxTexture(7);
        at<float>(sp + vfx::spr::kScale) = 15.0f;
        at<uint16_t>(sp + vfx::spr::kAngle) = at<uint16_t>(m + 0x654);
        for (int32_t i = 0; i < 3; ++i)
        {
            const uint32_t spin = static_cast<uint32_t>(i) << 8;
            const uint32_t a = ((static_cast<uint32_t>(at<uint16_t>(m + 0x10)) << (i + 5)) + spin) & 0xfff;
            const ee_float sine = ee_sinf((ee_itof(a) + ee_itof(a)) * turn);
            const int32_t dx = ee_ftoi(sine * range) >> 9;
            const uint32_t b = ((static_cast<uint32_t>(at<uint16_t>(m + 0x10)) << (7 - i)) - spin) & 0xfff;
            const ee_float sine2 = ee_sinf((ee_itof(b) + ee_itof(b)) * turn);
            const int32_t dy = ee_ftoi(sine2 * range) >> 9;
            const uint32_t c = ((static_cast<uint32_t>(at<uint16_t>(m + 0x10)) << (8 - i)) + spin) & 0xfff;
            const ee_float cosine = ee_cosf((ee_itof(c) + ee_itof(c)) * turn);
            const int32_t dz = ee_ftoi(cosine * range) >> 9;
            const uint32_t s2 = vfx::sprite();
            at<uint16_t>(s2 + vfx::spr::kAngle) = static_cast<uint16_t>(spin - at<uint16_t>(m + 0x654));
            at<float>(s2 + vfx::spr::kPos) = ee_itof(at<int16_t>(m + 0x14)) + ee_itof(dx);
            at<float>(s2 + vfx::spr::kPos + 4) = ee_itof(at<int16_t>(m + 0x16)) + ee_itof(dy);
            at<float>(s2 + vfx::spr::kPos + 8) = ee_itof(at<int16_t>(m + 0x18)) + ee_itof(dz);
            VfxPS2SpriteRotated3D(ee_float(0.0f));
        }
        at<uint8_t>(vfx::sprite() + vfx::spr::kCode) = 2;
        for (uint32_t index = at<uint8_t>(m + kRing + 2); index != at<uint8_t>(m + kRing + 3); index = (index + 1) & at<uint8_t>(m + kRing))
        {
            const uint32_t p = m + 0x50 + index * 0x30;
            sceVu0CopyVector(ptr32<ee_float>(vfx::sprite() + vfx::spr::kPos), ptr32<ee_float>(p));
            const uint32_t s2 = vfx::sprite();
            const uint32_t life = at<uint32_t>(p + 0x24);
            int32_t red = 0x5a, green = 0x78;
            if (life < 6)
            {
                const int32_t k = 5 - static_cast<int32_t>(life);
                green = 0x78 - k * 4;
                const int32_t size = k * 2 + 4;
                at<uint32_t>(s2 + vfx::spr::kTexture) = vfx::vfxTexture(8);
                red = 0x5a - k * 4;
                at<float>(s2 + vfx::spr::kScale) = ee_itof(size);
            }
            else
            {
                at<float>(s2 + vfx::spr::kScale) = 4.0f;
                at<uint32_t>(s2 + vfx::spr::kTexture) = vfx::vfxTexture(7);
            }
            at<uint16_t>(s2 + vfx::spr::kAngle) = 0;
            at<uint32_t>(s2 + vfx::spr::kRgb) = static_cast<uint32_t>(red + green * 0x10100);
            VfxPS2SpriteRotated3D(ee_float(-30.0f));
        }
        const uint32_t s3 = vfx::sprite();
        at<float>(s3 + vfx::spr::kPos) = ee_itof(at<int16_t>(m + 0x14));
        at<float>(s3 + vfx::spr::kPos + 4) = ee_itof(at<int16_t>(m + 0x16));
        at<float>(s3 + vfx::spr::kPos + 8) = ee_itof(at<int16_t>(m + 0x18));
        at<uint8_t>(s3 + vfx::spr::kCode) = 4;
        at<uint16_t>(s3 + vfx::spr::kAngle) = at<uint16_t>(m + 0x654);
        at<float>(s3 + vfx::spr::kScale) = at<float>(0x32784cu);
        at<uint32_t>(s3 + vfx::spr::kTexture) = vfx::vfxTexture(6);
        at<uint32_t>(s3 + vfx::spr::kRgb) = 0x2d4696;
        VfxPS2SpriteRotated3D(ee_float(-31.0f));
        at<uint16_t>(vfx::sprite() + vfx::spr::kAngle) = static_cast<uint16_t>(-static_cast<int32_t>(at<uint16_t>(m + 0x654)));
        VfxPS2SpriteRotated3D(ee_float(-31.0f));
        const uint32_t s4 = vfx::sprite();
        at<uint8_t>(s4 + vfx::spr::kCode) = 2;
        at<uint32_t>(s4 + vfx::spr::kRgb) = 0x646464;
        at<float>(s4 + vfx::spr::kScale) = 32.0f;
        at<uint32_t>(s4 + vfx::spr::kTexture) = vfx::vfxTexture(7);
        at<uint16_t>(s4 + vfx::spr::kAngle) = 0;
    }
    VfxPS2SpriteRotated3D(ee_float(-33.0f));
}

// "Ready, steady, go" at the start of a race: the picture (one of the HUD's start textures, chosen by the counter at
// +0x1a4) drawn as a sprite that flies in from the camera's side to its place (mem+0x188) over the first 0x17 frames,
// shrinking and dimming out, and a ring of star sparkles round it. Drawn once, on the first screen of a split screen
// (or the only one), with the scissor override set while drawing.
void VfxRenderer_ReadySteadyGo(ptr32<_sVfx_Object> mem)
{
    GuestFrame frame(0xa0);
    const uint32_t m = mem.address;
    const uint32_t hudRes = at<uint32_t>(at<uint32_t>(0x327e80u) + 0x40); // pLvlG->pHudRes
    const uint32_t screen = at<uint32_t>(0x1edc78u);                      // mainVarG.iWhichSplitScreen
    const bool split = at<uint8_t>(0x1edc68u) != 0;                       // mainVarG.splitScreen
    if (split && screen != 1)
        return;
    const uint64_t scissor = G(gRdrStateScissorOverride);
    G(gRdrStateScissorOverride) = 0xed0003027d0003ull;
    const uint32_t picture = at<uint32_t>(m + 0x1a4);
    const uint32_t sp = vfx::sprite();
    at<uint8_t>(sp + vfx::spr::kFlipX) = 1;
    at<uint32_t>(sp + vfx::spr::kTexture) = at<uint32_t>(hudRes + 0x88 + picture * 4);
    const uint32_t s2 = vfx::sprite();
    at<uint16_t>(s2 + vfx::spr::kAngle) = at<uint16_t>(m + 0x1a0);
    at<float>(s2 + vfx::spr::kPos) = ee_itof(at<int16_t>(m + 0x188));
    at<float>(s2 + vfx::spr::kPos + 4) = ee_itof(at<int16_t>(m + 0x18a));
    at<float>(s2 + vfx::spr::kPos + 8) = ee_itof(at<int16_t>(m + 0x18c));
    at<float>(s2 + vfx::spr::kScale) = picture == 3 ? 32.0f : 16.0f;
    at<uint8_t>(s2 + vfx::spr::kCode) = 2;
    const ptr32<int16_t> offset = frame.local<int16_t>(0), pos = frame.local<int16_t>(0x10), hud = frame.local<int16_t>(0x20);
    offset[0] = 0;
    offset[1] = split ? -0x28 : -0x2c;
    const uint32_t life = at<uint16_t>(m + 0x10);
    int32_t t = static_cast<int32_t>((life - 0x17u) * 0x67u);
    offset[2] = 0xd2;
    t = t < 0x1001 ? t : 0x1000;
    t = t > -1 ? t : 0;
    const uint32_t s3 = vfx::sprite();
    if (life < 0x17)
    {
        const uint32_t grey = life * 8 + 8;
        at<uint32_t>(s3 + vfx::spr::kRgb) = grey + (grey << 8) + (grey << 16);
        at<float>(s3 + vfx::spr::kScale) = ee_itof(static_cast<int32_t>((picture == 3 ? 0x3e : 0x2d) - life));
    }
    else
        at<uint32_t>(s3 + vfx::spr::kRgb) = 0x606060;
    PS2C_Vec16MulScaler(ptr32<_sVec16>(pos.address), ptr32<_sVec16>(m + 0x188), static_cast<int16_t>(t));
    PS2C_SetGteMatrix(ptr32<_sMat>(kVisFx + 0xbc));
    PS2C_GteMatrixMulVec16(ptr32<_sVec16>(offset.address));
    PS2C_GetVec16GteResult(ptr32<_sVec16>(offset.address));
    const int32_t camera[3] = {at<int32_t>(kVisFx + 0xfc) >> 8, at<int32_t>(kVisFx + 0x100) >> 8, at<int32_t>(kVisFx + 0x104) >> 8};
    for (int lane = 0; lane < 3; ++lane)
        hud[lane] = static_cast<int16_t>(static_cast<uint16_t>(offset[lane]) + camera[lane]);
    PS2C_Vec16MulScaler(ptr32<_sVec16>(hud.address), ptr32<_sVec16>(hud.address), static_cast<int16_t>(0x1000 - t));
    for (int lane = 0; lane < 3; ++lane)
        pos[lane] = static_cast<int16_t>(static_cast<uint16_t>(pos[lane]) + static_cast<uint16_t>(hud[lane]));
    const uint32_t s4 = vfx::sprite();
    at<uint8_t>(s4 + vfx::spr::kCode) = 2;
    at<float>(s4 + vfx::spr::kPos) = ee_itof(pos[0]);
    at<float>(s4 + vfx::spr::kPos + 4) = ee_itof(pos[1]);
    at<float>(s4 + vfx::spr::kPos + 8) = ee_itof(pos[2]);
    VfxPS2SpriteRotated3D(ee_float(-10.0f));
    const uint32_t s5 = vfx::sprite();
    at<uint32_t>(s5 + vfx::spr::kTexture) = vfx::vfxTexture(0xe);
    at<uint32_t>(s5 + vfx::spr::kRgb) = at<uint16_t>(m + 0x10) < 0x18 ? 0x404040 : 0x2020;
    for (uint32_t index = at<uint8_t>(m + kRing + 2); index != at<uint8_t>(m + kRing + 3); index = (index + 1) & at<uint8_t>(m + kRing))
    {
        const uint32_t p = m + 0x48 + index * 0x14;
        const uint32_t s = vfx::sprite();
        at<float>(s + vfx::spr::kScale) = ee_itof(at<uint16_t>(p + 0x10) + 4);
        at<float>(s + vfx::spr::kPos) = ee_itof(pos[0] + (at<int16_t>(p) >> 4));
        at<float>(s + vfx::spr::kPos + 4) = ee_itof(pos[1] + (at<int16_t>(p + 2) >> 4));
        at<float>(s + vfx::spr::kPos + 8) = ee_itof(pos[2] + (at<int16_t>(p + 4) >> 4));
        at<uint16_t>(vfx::sprite() + vfx::spr::kAngle) = static_cast<uint16_t>(vfx::random15());
        VfxPS2SpriteRotated3D(ee_float(-10.0f));
    }
    G(gRdrStateScissorOverride) = scissor;
}

// The rocket's smoke trail and, while the rocket is young (life 0x20 or more), the rocket itself: a glow sprite at the
// effect's position and the rocket's shape (pWEProcket) turned to face along the rocket's direction (the shorts at
// +0x2cc), scaled up threefold. The smoke is a ring of 0x14-byte puffs (positions as shorts, a life at +0x10 and a spin at
// +0x12) that grow and brighten as they age. The rocket's spin angle counts up by 0x20 a frame.
void VfxRenderer_RocketSmoke(ptr32<_sVfx_Object> mem)
{
    GuestFrame frame(0x140);
    const uint32_t m = mem.address;
    const uint32_t sp = vfx::sprite();
    at<uint32_t>(m + 0x2d4) = at<uint32_t>(m + 0x2d4) + 0x20;
    at<uint8_t>(sp + vfx::spr::kCode) = 2;
    at<uint32_t>(sp + vfx::spr::kTexture) = vfx::vfxTexture(0xd);
    int32_t index = static_cast<int8_t>(at<uint8_t>(m + kRing + 2));
    while (index != at<uint8_t>(m + kRing + 3))
    {
        const uint32_t p = m + kParticles + static_cast<uint32_t>(index * 0x14);
        const uint32_t s = vfx::sprite();
        const uint32_t life = at<uint16_t>(p + 0x10);
        at<float>(s + vfx::spr::kPos) = ee_itof(at<int16_t>(p));
        at<float>(s + vfx::spr::kPos + 4) = ee_itof(at<int16_t>(p + 2));
        at<uint8_t>(s + vfx::spr::kFlipX) = 0;
        at<float>(s + vfx::spr::kPos + 8) = ee_itof(at<int16_t>(p + 4));
        at<uint16_t>(s + vfx::spr::kAngle) = at<uint16_t>(p + 0x12);
        const uint32_t grey = life * 2 + 4;
        at<uint32_t>(s + vfx::spr::kRgb) = grey * 0x10101;
        at<float>(s + vfx::spr::kScale) = ee_itof(static_cast<int32_t>(0x1f - life)) + ee_float(16.0f);
        VfxPS2SpriteRotated3D(ee_float(0.0f));
        index = static_cast<int8_t>(at<uint8_t>(m + kRing) & static_cast<uint8_t>(index + 1));
    }
    if (at<uint16_t>(m + 0x10) < 0x20)
        return;
    const uint32_t s = vfx::sprite();
    at<uint32_t>(s + vfx::spr::kTexture) = vfx::vfxTexture(0x17);
    at<float>(s + vfx::spr::kScale) = 100.0f;
    at<uint32_t>(s + vfx::spr::kRgb) = 0x3c64;
    at<float>(s + vfx::spr::kPos) = ee_itof(at<int16_t>(m + 0x14));
    at<float>(s + vfx::spr::kPos + 4) = ee_itof(at<int16_t>(m + 0x16));
    at<float>(s + vfx::spr::kPos + 8) = ee_itof(at<int16_t>(m + 0x18));
    at<uint16_t>(s + vfx::spr::kAngle) = at<uint16_t>(m + 0x2d4);
    VfxPS2SpriteRotated3D(ee_float(0.0f));
    // The rocket's axes: z along its direction, x across it (z's x and z swapped, one negated), y the cross product.
    const ptr32<float> xAxis = frame.local<float>(0), yAxis = frame.local<float>(0x10), zAxis = frame.local<float>(0x20);
    at<float>(zAxis.address + 8) = ee_itof(at<int16_t>(m + 0x2d0));
    at<float>(zAxis.address) = ee_itof(at<int16_t>(m + 0x2cc));
    at<float>(zAxis.address + 4) = ee_itof(at<int16_t>(m + 0x2ce));
    guest_call(0x1c7868u, ptr32<uint8>(zAxis.address), ptr32<uint8>(zAxis.address)); // sceVu0Normalize
    const ee_float zx = at<float>(zAxis.address), zz = at<float>(zAxis.address + 8);
    at<float>(xAxis.address + 4) = 0.0f;
    at<float>(xAxis.address) = zz;
    at<float>(xAxis.address + 8) = -zx;
    guest_call(0x1c7868u, ptr32<uint8>(xAxis.address), ptr32<uint8>(xAxis.address));
    guest_call(0x1c7820u, ptr32<uint8>(yAxis.address), ptr32<uint8>(xAxis.address), ptr32<uint8>(zAxis.address)); // sceVu0OuterProduct
    const ptr32<float> matrix = frame.local<float>(0x30);
    const uint32_t axes[3] = {xAxis.address, yAxis.address, zAxis.address};
    for (uint32_t row = 0; row < 3; ++row)
        for (uint32_t col = 0; col < 3; ++col)
            at<float>(matrix.address + row * 0x10 + col * 4) = at<float>(axes[row] + col * 4);
    const ptr32<uint8> block = frame.local<uint8>(0x70);
    guest_call(0x1a1550u, block); // RdrState_SetStateBlockToDefaults
    guest_call(0x1a1058u, block); // RdrState_ForceApplyStateBlock
    const uint32_t shape = at<uint32_t>(G(visFxVarG).pVfxRes.address + 0x88);
    for (uint32_t row = 0; row < 3; ++row)
        for (uint32_t col = 0; col < 3; ++col)
            at<float>(shape + 8 + row * 12 + col * 4) = ee_float(at<float>(matrix.address + row * 0x10 + col * 4)) * ee_float(3.0f);
    const uint32_t s2 = vfx::sprite();
    at<float>(shape + 0x2c) = at<float>(s2 + vfx::spr::kPos);
    at<float>(shape + 0x30) = at<float>(s2 + vfx::spr::kPos + 4);
    at<float>(shape + 0x34) = at<float>(s2 + vfx::spr::kPos + 8);
    guest_call(0x1a55b8u, ptr32<uint8>(shape), ptr32<uint8>(0x1f40b8u), 0); // TRdr_VUShape(shape, &rdrCameraG, 0)
}

// The car rocket's flight: a puff of smoke at a random point near the rocket (the car's aerial, section 5) each frame while
// the ring has room, thrown back against the car's own direction (the last column of its matrix) with a random scatter;
// the puffs drift and live 15 frames. The effect ends when the car no longer holds a weapon (byte +0x194). Sorted at the
// car's rendered position.
void VfxHandler_CarRocket(ptr32<_sVfx_Object> mem)
{
    GuestFrame frame(0x100);
    const uint32_t m = mem.address;
    const uint32_t ring = m + kRing, particles = m + 0x50;
    const uint32_t car = at<uint32_t>(m + 0x350);
    VfxCarAerialVal(ptr32<_sObject>(car), 5);
    const uint32_t state = at<uint32_t>(at<uint32_t>(car + 0x20) + 0x1e8);
    const uint32_t matrix = state + 0x28;
    const uint32_t ints = frame.local<int32_t>(0x40).address;
    at<int32_t>(ints) = at<int32_t>(state + 4) >> 8;
    at<int32_t>(ints + 4) = at<int32_t>(state + 8) >> 8;
    at<int32_t>(ints + 8) = at<int32_t>(state + 0xc) >> 8;
    sortPosFromLocal(m, ints);
    if (at<uint8_t>(ring + 1) < at<uint8_t>(m + 0x40))
    {
        const uint32_t tail = at<uint8_t>(ring + 3);
        const ee_float spread(2.3283064e-08f), centre(25.0f), scatter(at<float>(0x327888u));
        const ee_float push(at<float>(0x32788cu)), drag(at<float>(0x327890u));
        const uint32_t p = particles + tail * 0x30;
        at<float>(p) = (ee_itof(rand()) * spread - centre) * scatter;
        at<float>(p + 4) = (ee_itof(rand()) * spread - centre) * scatter;
        at<float>(p + 8) = (ee_itof(rand()) * spread - centre) * scatter;
        const uint32_t column[3] = {4, 0xa, 0x10}; // the matrix's last column (Xz, Yz, Zz)
        for (uint32_t axis = 0; axis < 3; ++axis)
        {
            const int32_t kick = (vfx::random15() & 0x3f) - 0x1f;
            at<float>(p + 0x10 + 4 * axis) = ee_itof(kick) * push - ee_itof(at<int16_t>(matrix + column[axis])) * drag;
        }
        at<uint32_t>(p + 0x24) = 0xf;
        at<uint8_t>(ring + 3) = static_cast<uint8_t>(at<uint8_t>(m + 0x40) & (tail + 1));
        at<uint8_t>(ring + 1) = static_cast<uint8_t>(at<uint8_t>(ring + 1) + 1);
    }
    uint32_t index = at<uint8_t>(ring + 2);
    if (index != at<uint8_t>(ring + 3))
    {
        do
        {
            const uint32_t p = particles + index * 0x30;
            const ee_float x = ee_float(at<float>(p)) + ee_float(at<float>(p + 0x10));
            const ee_float y = ee_float(at<float>(p + 4)) + ee_float(at<float>(p + 0x14));
            const ee_float z = ee_float(at<float>(p + 8)) + ee_float(at<float>(p + 0x18));
            const uint32_t life = at<uint32_t>(p + 0x24) - 1;
            at<float>(p) = x;
            at<float>(p + 4) = y;
            at<float>(p + 8) = z;
            at<uint32_t>(p + 0x24) = life;
            if (life == 0)
            {
                const uint32_t head = at<uint8_t>(ring + 2);
                if (index != head)
                    std::memcpy(&at<uint8_t>(p), &at<uint8_t>(particles + head * 0x30), 0x30);
                at<uint8_t>(ring + 1) = static_cast<uint8_t>(at<uint8_t>(ring + 1) - 1);
                at<uint8_t>(ring + 2) = static_cast<uint8_t>(at<uint8_t>(ring) & (at<uint8_t>(ring + 2) + 1));
            }
            index = at<uint8_t>(ring) & (index + 1);
        } while (index != at<uint8_t>(ring + 3));
    }
    if (at<int8_t>(car + 0x194) == 0)
        at<uint16_t>(m + 0x10) = 0;
}

} // namespace rc

// Forward declarations of the random-input setups (visfx_fuzz.cpp).
namespace rc::fuzz {
void vfxCameraMatrix(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxCarHeatRayAerialRenderer(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxQuake(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxWaterSploosh(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxOilSlick(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxAerialRenderer(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxSpinPickup(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxCarAerialRibbon(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxWaterBallHandler(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxWaterBallRenderer(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxReadySteadyGo(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxRocketSmokeRenderer(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxCarRocketHandler(R5900Context *ctx, uint64_t &state, uint32_t scratch);
}

RC_DECOMP_FN_SETUP(VfxSetupCameraMatrix, 0x156dc0, rc::fuzz::vfxCameraMatrix)
RC_DECOMP_FN_SETUP(VfxRenderer_CarHeatRayAerial, 0x15c1e0, rc::fuzz::vfxCarHeatRayAerialRenderer)
RC_DECOMP_FN_SETUP(VfxRenderer_Quake2, 0x1689f8, rc::fuzz::vfxQuake)
RC_DECOMP_FN_SETUP(VfxRenderer_WaterSploosh, 0x15d2a0, rc::fuzz::vfxWaterSploosh)
RC_DECOMP_FN_SETUP(VfxRenderer_OilSlick, 0x15ad18, rc::fuzz::vfxOilSlick)
RC_DECOMP_FN_SETUP(VfxRenderer_Aerial, 0x15ee38, rc::fuzz::vfxAerialRenderer)
RC_DECOMP_FN_SETUP(VfxRenderer_SpinPickup, 0x161390, rc::fuzz::vfxSpinPickup)
RC_DECOMP_FN_SETUP(VfxHandler_CarAerialRibbon, 0x159a20, rc::fuzz::vfxCarAerialRibbon)
RC_DECOMP_FN_SETUP(VfxHandler_WaterBallFlying, 0x1647d8, rc::fuzz::vfxWaterBallHandler)
RC_DECOMP_FN_SETUP(VfxRenderer_WaterBallFlying, 0x164b50, rc::fuzz::vfxWaterBallRenderer)
RC_DECOMP_FN_SETUP(VfxRenderer_ReadySteadyGo, 0x1658d0, rc::fuzz::vfxReadySteadyGo)
RC_DECOMP_FN_SETUP(VfxRenderer_RocketSmoke, 0x165070, rc::fuzz::vfxRocketSmokeRenderer)
RC_DECOMP_FN_SETUP(VfxHandler_CarRocket, 0x1690f8, rc::fuzz::vfxCarRocketHandler)
