// Decompiled from OBJECT.c: drawing a car (body, wheels at their blur level, the dump tug's sled and rock, the lights
// and their flares). See object.cpp.

#include "object.h"
#include "object_fuzz.h"
#include "visfx.h"

#include <cstring>

namespace rc {

int16 RendObjRender(ptr32<_sRendObj> pRendObj, ptr32<SVECTOR> pPOI__unused, ptr32<SVECTOR> pNormal__unused, int32_t doTint);
void PS2C_GteMatrixMulMatrix(ptr32<_sMat> pMatOut, ptr32<_sMat> pMat1, ptr32<_sMat> pMat2);
void FPhysLibUpdateCarSpheres(ptr32<_sFPhysLibCar> pCar, int32_t bFullUpdate);
void Vfx2TargetCar(ptr32<_sObject> pObj);
void VfxSetupCameraMatrix();
void Vec32MulMatPsx(ptr32<vec32_> p, ptr32<_matPsx> mp, ptr32<vec32_> r);
void RdrPolyTexture(int32_t channel, ptr32<_sGfxTexture> pTexture, u32 style);
void RdrPoly(int32_t channel, ptr32<_sRdrPoly> poly);

using namespace object;

namespace {

constexpr uint32_t kGameTaskShadowCalc = 0x123d98u, kOldRotMatrix = 0x19e268u;
constexpr uint32_t kCameraVars = 0x1e9fb0u, kVfxRes = 0x1f106cu, kSplitScreen = 0x1edc78u, kWheel = 0x328580u;
constexpr uint32_t kRdrCamera = 0x1f40b8u, kCurrentDo = 0x1ed908u, kResultTrigger = 0x1f3730u;
constexpr uint32_t kDumperRockHead = 0x1ee50cu, kSledOffset = 0x323938u, kRockOffset = 0x323948u, kSledTrig = 0x32769cu;

void copyBytes(uint32_t to, uint32_t from, uint32_t size) { std::memmove(guest_host(to), guest_host(from), size); }

bool isCar(ptr32<_sObject> o) { return static_cast<uint32_t>(o->pDef->type) < 0x14u; }

} // namespace

// The dump tug's sled: placed behind the cab, tipped by tipAngle (a rotation about the cab's x axis built from twelve
// trig calls), and the rock it carries while the tug holds its weapon (0xf).
void ObjectDumptugSled(ptr32<_sObject> pObject)
{
    GuestFrame frame(0x130);
    const uint32_t sp = frame.local<uint8_t>(0).address;
    std::memset(guest_host(sp), 0, 6); // angles
    copyBytes(sp + 0x10, kSledOffset, 12);
    copyBytes(pObject->pRendObj2.address + 4u, pObject->pPhysics->pInteger.address + 4u, 12);
    copyBytes(pObject->pRendObj2.address + 0x18u, pObject->pPhysics->pInteger.address + 0x28u, 32);
    Vec32MulMatPsx(ptr32<vec32_>(sp + 0x10), ptr32<_matPsx>(pObject->pRendObj2.address + 0x18u), ptr32<vec32_>(sp + 0x20));
    const uint32_t sled = pObject->pRendObj2.address;
    at<int32_t>(sled + 4) += at<int32_t>(sp + 0x20) << 8;
    at<int32_t>(sled + 8) += at<int32_t>(sp + 0x24) << 8;
    at<int32_t>(sled + 0xc) += at<int32_t>(sp + 0x28) << 8;
    const uint16_t pitch = at<uint16_t>(pObject.address + 0x1ccu);
    at<uint16_t>(sp) = pitch;
    int k = 0;
    // ftoi(trig((float)a + (float)a) * K[k]) * 4096), a the angle's 12 bits.
    const auto trig = [&](bool cosine)
    {
        const uint32_t a = (static_cast<uint32_t>(-static_cast<int32_t>(at<int16_t>(sp))) >> 4) & 0xfffu;
        ee_float f = ee_itof(static_cast<int32_t>(a));
        f = f + f;
        const ee_float arg = f * at<ee_float>(kSledTrig + 4u * static_cast<uint32_t>(k++));
        const ee_float r = cosine ? ee_cosf(arg) : ee_sinf(arg);
        return static_cast<uint32_t>(ee_ftoi(r * ee_float(4096.0f)));
    };
    const auto m = [&](uint32_t off) { return static_cast<uint32_t>(static_cast<int32_t>(at<int16_t>(sled + off))); };
    const auto row = [](uint32_t sum) { return static_cast<uint16_t>(sum >> 12); };
    uint32_t c = trig(true);
    const uint32_t m1a = m(0x1a), m1c = m(0x1c), m20 = m(0x20), m22 = m(0x22), m26 = m(0x26), m28 = m(0x28);
    const uint16_t n1a = row(m1a * c + m1c * trig(false));
    c = trig(true);
    const uint16_t n20 = row(m20 * c + m22 * trig(false));
    c = trig(true);
    const uint16_t n26 = row(m26 * c + m28 * trig(false));
    c = trig(false);
    const uint16_t n1c = row(m1a * (0u - c) + m1c * trig(true));
    c = trig(false);
    const uint16_t n22 = row(m20 * (0u - c) + m22 * trig(true));
    c = trig(false);
    const uint16_t n28 = row(m26 * (0u - c) + m28 * trig(true));
    const uint32_t s = pObject->pRendObj2.address;
    at<uint16_t>(s + 0x1a) = n1a;
    at<uint16_t>(s + 0x20) = n20;
    at<uint16_t>(s + 0x26) = n26;
    at<uint16_t>(s + 0x1c) = n1c;
    at<uint16_t>(s + 0x22) = n22;
    at<uint16_t>(s + 0x28) = n28;
    if (pObject->weaponType != 0xf)
        return;
    copyBytes(sp + 0x70, kRockOffset, 12);
    Vec32MulMatPsx(ptr32<vec32_>(sp + 0x70), ptr32<_matPsx>(pObject->pRendObj.address + 0x18u), ptr32<vec32_>(sp + 0x80));
    const uint32_t integer = pObject->pPhysics->pInteger.address;
    const uint32_t rock = sp + 0x30; // theRock: its angles and flags are whatever the stack held
    copyBytes(rock + 4, integer + 4, 12);
    at<int32_t>(rock + 4) += at<int32_t>(sp + 0x80) << 8;
    at<int32_t>(rock + 8) += at<int32_t>(sp + 0x84) << 8;
    at<int32_t>(rock + 0xc) += at<int32_t>(sp + 0x88) << 8;
    copyBytes(rock + 0x18, integer + 0x28, 32);
    at<uint32_t>(rock) = at<uint32_t>(kDumperRockHead);
    RendObjRender(ptr32<_sRendObj>(rock), nullptr, nullptr, 1);
}

// A car: the body (and its second part, for cars), shiny when invincible, the shadow, the wheels (each at the blur
// level its spin calls for, the right-hand ones mirrored; the six-wheeler's middle pair between), the lock-on target
// and the lights.
void ObjectRender(ptr32<_sObject> pObject)
{
    GuestFrame frame(0x150);
    const uint32_t obj = pObject.address;
    const uint32_t physics = pObject->pPhysics.address;
    const ptr32<_sRendObj> rend = pObject->pRendObj;
    const auto integer = [&] { return at<uint32_t>(physics + 0x1e8u); };
    copyBytes(rend.address + 4u, integer() + 4u, 12);
    copyBytes(rend.address + 0x18u, integer() + 0x28u, 32);
    uint32_t rend2 = pObject->pRendObj2.address;
    if (rend2 != 0 && isCar(pObject))
    {
        ObjectDumptugSled(pObject);
        rend2 = pObject->pRendObj2.address;
    }
    const uint32_t head = rend->pRendObjHead.address;
    if (pObject->playerType < 2 && at<int32_t>(kCameraVars + pObject->playerType * 0x5b8u) == 0)
        at<uint16_t>(head + 0x58) |= 0x400;
    at<uint32_t>(head + 0x64) = at<uint32_t>(obj + 4); // tint and brightness
    const uint32_t inv = at<uint8_t>(obj + 0x19cu) & 3u;
    const auto head2 = [&] { return at<uint32_t>(rend2); };
    if (inv == 1 || inv == 2)
    {
        const uint32_t tex = at<uint32_t>(at<uint32_t>(kVfxRes) + (inv == 1 ? 0x78u : 0x7cu));
        at<uint16_t>(head + 0x58) |= 0x8000;
        at<uint32_t>(head + 0x60) = tex;
        if (rend2 != 0 && isCar(pObject))
        {
            at<uint32_t>(head2() + 0x60) = tex;
            at<uint16_t>(head2() + 0x58) |= 0x8000;
        }
    }
    else
    {
        const uint32_t level = G(pLvlG).address;
        at<uint16_t>(head + 0x58) &= 0x7fff;
        at<uint32_t>(head + 0x60) = at<uint32_t>(level + 0x60);
        if (rend2 != 0 && isCar(pObject))
        {
            at<uint32_t>(head2() + 0x60) = at<uint32_t>(level + 0x60);
            at<uint16_t>(head2() + 0x58) &= 0x7fff;
        }
    }
    if (at<uint8_t>(obj + 0x19cu) & 4u)
    {
        at<uint16_t>(head + 0x58) |= 0x2000;
        if (rend2 != 0 && isCar(pObject))
            at<uint16_t>(head2() + 0x58) |= 0x2000;
    }
    else
    {
        at<uint16_t>(head + 0x58) &= 0xdfff;
        if (rend2 != 0 && isCar(pObject))
            at<uint16_t>(head2() + 0x58) &= 0xdfff;
    }
    uint16_t dist = static_cast<uint16_t>(RendObjRender(rend, nullptr, nullptr, 5));
    rend2 = pObject->pRendObj2.address;
    if (rend2 != 0 && isCar(pObject))
    {
        at<uint32_t>(head2() + 0x64) = at<uint32_t>(obj + 4);
        dist = static_cast<uint16_t>(RendObjRender(ptr32<_sRendObj>(rend2), nullptr, nullptr, 5));
    }
    if (at<int32_t>(kSplitScreen) == 0 || dist < pObject->zShortestDrawnDist)
        pObject->zShortestDrawnDist = dist;
    pObject->zDrawnDist = dist;
    pObject->pushRenderDist = static_cast<int16>((rend->pRendObjHead->flags & 0x600u) ? 0 : 0x34);
    if (VfxHasCarBeenRendered(pObject) < 0x157c)
        guest_call(kGameTaskShadowCalc, pObject);
    const uint32_t type = static_cast<uint32_t>(pObject->pDef->type);
    if (type != 0xfu && type < 0x14u && pObject->pWheels && VfxHasCarBeenRendered(pObject) < 0x157c)
    {
        if (pObject->pPhysics->flags & 0xe0u)
            FPhysLibUpdateCarSpheres(pObject->pPhysics, 0);
        const uint32_t wheelMat = frame.local<uint8_t>(0x70).address;
        const uint32_t angles = frame.local<uint8_t>(0x40).address, rotMat = frame.local<uint8_t>(0x50).address;
        const auto wheelList = [&](uint32_t n) { return at<uint32_t>(at<uint32_t>(pObject->pWheels.address + 0x10u) + n * 4u); };
        for (uint32_t i = 0; i < 4; ++i)
        {
            const uint32_t w = physics + i * 0x48u; // wheel[i] - 0xac
            copyBytes(wheelMat, integer() + (i < 2 ? 0x3cu : 0x28u), 32);
            if (VfxHasCarBeenRendered(pObject) < 1000)
            {
                at<int16_t>(angles + 4) = 0;
                at<int16_t>(angles + 2) = 0;
                at<int16_t>(angles) = static_cast<int16_t>(ee_ftoi(-at<ee_float>(w + 0xe4u) * ee_float(0.0625f)));
                guest_call(kOldRotMatrix, angles, rotMat);
                PS2C_GteMatrixMulMatrix(ptr32<_sMat>(wheelMat), ptr32<_sMat>(wheelMat), ptr32<_sMat>(rotMat));
            }
            const int32_t spin = ee_ftoi(at<ee_float>(w + 0xe0u));
            const uint32_t speed = spin < 0 ? 0u - static_cast<uint32_t>(spin) : static_cast<uint32_t>(spin);
            uint32_t level;
            if (G(ObjectBlurMaxVel) < speed)
                level = at<uint32_t>(wheelList(i)) - 1u;
            else
                level = G(ObjectBlurMedVel) < speed ? 1u : 0u;
            const uint32_t list = wheelList(i);
            const uint32_t last = at<uint32_t>(list) - 1u;
            if (!(level < last))
                level = last;
            at<uint32_t>(kWheel) = at<uint32_t>(list + 4u + level * 4u);
            copyBytes(kWheel + 0x18u, wheelMat, 32);
            at<uint32_t>(at<uint32_t>(kWheel) + 0x64u) = at<uint32_t>(obj + 4);
            if (i == 1 || i == 3)
                for (uint32_t off : {0x18u, 0x1eu, 0x24u})
                    at<uint16_t>(kWheel + off) = static_cast<uint16_t>(0u - at<uint16_t>(kWheel + off));
            at<int32_t>(kWheel + 4) = ee_ftoi(at<ee_float>(w + 0xacu));
            at<int32_t>(kWheel + 8) = ee_ftoi(at<ee_float>(w + 0xb0u));
            at<int32_t>(kWheel + 0xc) = ee_ftoi(at<ee_float>(w + 0xb4u));
            RendObjRender(ptr32<_sRendObj>(kWheel), nullptr, nullptr, 1);
            if (static_cast<uint32_t>(pObject->pDef->type) == 0x12u && i >= 2)
            {
                const uint32_t front = physics + (i - 2) * 0x48u;
                const ee_float half(0.5f);
                const ee_float x = (ee_itof(at<int32_t>(kWheel + 4)) + at<ee_float>(front + 0xacu)) * half;
                const ee_float y = (ee_itof(at<int32_t>(kWheel + 8)) + at<ee_float>(front + 0xb0u)) * half;
                const ee_float z = (ee_itof(at<int32_t>(kWheel + 0xc)) + at<ee_float>(front + 0xb4u)) * half;
                const uint32_t middle = at<uint32_t>(wheelList(i + 2) + 4u);
                at<uint32_t>(kWheel) = middle;
                at<int32_t>(kWheel + 4) = ee_ftoi(x);
                at<int32_t>(kWheel + 8) = ee_ftoi(y);
                at<int32_t>(kWheel + 0xc) = ee_ftoi(z);
                at<uint32_t>(middle + 0x64u) = at<uint32_t>(obj + 4);
                RendObjRender(ptr32<_sRendObj>(kWheel), nullptr, nullptr, 1);
            }
        }
    }
    if (VfxHasCarBeenRendered(pObject) >= 0x157c)
        return;
    if (pObject->lockOnMe != 0)
    {
        VfxSetupCameraMatrix();
        Vfx2TargetCar(pObject);
    }
    ObjectRenderLights(pObject);
}

// ---------------------------------------------------------------------------------------------------------------------
// Lights: a glow quad per lamp (four corners in the car's frame) and a camera-facing flare at its fifth point, each
// faded by how squarely the car faces the camera.

namespace {

const ee_float kInv4096 = float_from_bits(0x39800000u), kInv256 = float_from_bits(0x3b800000u);

struct Lights
{
    uint32_t obj, mat, renderPos, poly;

    ee_float M(uint32_t n) const { return at<ee_float>(mat + 4u * n); }
    ee_float P(uint32_t n) const { return at<ee_float>(renderPos + 4u * n); }

    bool lampOn(uint32_t base, uint32_t i) const { return at<ee_float>(obj + base + i * 0x3cu + 8u) != ee_float(0.0f); }

    // The glow quads of the lamps from base (rear 0x224, head 0x314), stopping at the first with corner z 0.
    void glow(uint32_t base, uint32_t rgba) const
    {
        for (uint32_t i = 0; i < 4 && lampOn(base, i); ++i)
        {
            uint32_t l = obj + base + i * 0x3cu;
            for (uint32_t k = 0; k < 4; ++k, l += 0xc)
            {
                const ee_float x = at<ee_float>(l), y = at<ee_float>(l + 4), z = at<ee_float>(l + 8);
                ee_float vx = (M(0) * x + M(1) * y + M(2) * z) * kInv4096;
                ee_float vy = (M(3) * x + M(4) * y + M(5) * z) * kInv4096;
                ee_float vz = (M(6) * x + M(7) * y + M(8) * z) * kInv4096;
                at<uint32_t>(poly + 0x60 + 4 * k) = rgba;
                vx = vx + P(0);
                vy = vy + P(1);
                vz = vz + P(2);
                at<ee_float>(poly + 0x10 * k) = vx * kInv256;
                at<ee_float>(poly + 0x10 * k + 4) = vy * kInv256;
                at<ee_float>(poly + 0x10 * k + 8) = vz * kInv256;
            }
            RdrPoly(0, ptr32<_sRdrPoly>(poly));
        }
    }

    // The flares at each lamp's fifth point, pulled toward the camera (by a quarter of 4096 for the rear lamps).
    void flares(uint32_t base, uint32_t sizes, bool quarter, uint32_t rgba) const
    {
        for (uint32_t i = 0; i < 4 && lampOn(base, i); ++i)
        {
            const uint32_t l = obj + base + i * 0x3cu + 0x30u;
            const ee_float x = at<ee_float>(l), y = at<ee_float>(l + 4), z = at<ee_float>(l + 8);
            ee_float px = (M(0) * x + M(1) * y + M(2) * z) * kInv4096;
            ee_float py = (M(3) * x + M(4) * y + M(5) * z) * kInv4096;
            ee_float pz = (M(6) * x + M(7) * y + M(8) * z) * kInv4096;
            const ee_float s = at<ee_float>(obj + sizes + 4u * i) * kInv256;
            const ee_float k256(256.0f);
            const ee_float cx = at<ee_float>(kRdrCamera + 0xbcu) * k256, cy = at<ee_float>(kRdrCamera + 0xc0u) * k256,
                           cz = at<ee_float>(kRdrCamera + 0xc4u) * k256;
            px = px + P(0);
            ee_float dx = cx - px;
            py = py + P(1);
            ee_float dy = cy - py;
            pz = pz + P(2);
            ee_float dz = cz - pz;
            const ee_float r = ee_rsqrt(ee_float(4096.0f), dx * dx + dy * dy + dz * dz);
            dz = dz * r;
            dx = dx * r;
            dy = dy * r;
            if (quarter)
            {
                dz = dz * ee_float(0.25f);
                dx = dx * ee_float(0.25f);
                dy = dy * ee_float(0.25f);
            }
            pz = pz + dz;
            px = px + dx;
            py = py + dy;
            const ee_float sx = px * kInv256, sy = py * kInv256, sz = pz * kInv256;
            const auto cam = [](uint32_t off) { return at<ee_float>(kRdrCamera + off); };
            for (uint32_t k = 0; k < 4; ++k)
            {
                const ee_float a = (k & 1u) ? s : -s;
                const ee_float b = k < 2 ? s : -s;
                at<uint32_t>(poly + 0x60 + 4 * k) = rgba;
                at<ee_float>(poly + 0x10 * k) = sx + a * cam(0x98) + b * cam(0xa4);
                at<ee_float>(poly + 0x10 * k + 4) = sy + a * cam(0x9c) + b * cam(0xa8);
                at<ee_float>(poly + 0x10 * k + 8) = sz + a * cam(0xa0) + b * cam(0xac);
            }
            RdrPoly(0, ptr32<_sRdrPoly>(poly));
        }
    }
};

// The flare's strength from how squarely the lamp faces the camera (4096 head on).
int32_t flareLevel(int32_t facing)
{
    if (facing < 0xb50)
        return 0;
    if (facing < 4000)
        return ee_div(static_cast<int32_t>(static_cast<uint32_t>(facing - 0xb50) * 0x4au), 0x450);
    return ee_div(static_cast<int32_t>(static_cast<uint32_t>(facing - 4000) * 0xb5u), 0x60) + 0x4a;
}

uint32_t scaled(int32_t level, uint32_t factor, int32_t limit)
{
    const int32_t v = static_cast<int32_t>(static_cast<uint32_t>(level) * factor) >> 8;
    return static_cast<uint32_t>(v < limit + 1 ? v : limit);
}

} // namespace

void ObjectRenderLights(ptr32<_sObject> pObject)
{
    GuestFrame frame(0x190);
    const uint32_t obj = pObject.address;
    if (at<int8_t>(obj + 0x222u) != 0)
    {
        at<uint8_t>(obj + 0x222u) = static_cast<uint8_t>(at<uint8_t>(obj + 0x222u) - 1);
        at<uint8_t>(obj + 0x220u) |= 1;
    }
    if (at<int8_t>(obj + 0x223u) != 0)
    {
        at<uint8_t>(obj + 0x223u) = static_cast<uint8_t>(at<uint8_t>(obj + 0x223u) - 1);
        at<uint8_t>(obj + 0x220u) |= 2;
    }
    const uint32_t physics = pObject->pPhysics.address;
    if (at<int8_t>(physics + 0x1ceu) != 1)
        return;
    uint32_t on = at<uint8_t>(obj + 0x220u);
    if (on == 0)
        return;
    Lights L{obj, physics + 0x4cu, physics + 8u, frame.local<uint8_t>(0x60).address};
    if (at<int32_t>(kCurrentDo) == 1 && at<int32_t>(kResultTrigger) == 0)
    {
        // The front end's cars: the matrix used transposed.
        const uint32_t t = frame.local<uint8_t>(0x30).address;
        static constexpr uint32_t kFrom[9] = {0, 3, 6, 1, 4, 7, 2, 5, 8};
        for (uint32_t n = 0; n < 9; ++n)
            at<uint32_t>(t + 4 * n) = at<uint32_t>(L.mat + 4 * kFrom[n]);
        L.mat = t;
    }
    const ee_float k256(256.0f);
    const ee_float dx = at<ee_float>(kRdrCamera + 0xbcu) * k256 - L.P(0);
    const ee_float dy = at<ee_float>(kRdrCamera + 0xc0u) * k256 - L.P(1);
    const ee_float dz = at<ee_float>(kRdrCamera + 0xc4u) * k256 - L.P(2);
    const uint32_t vfxRes = at<uint32_t>(kVfxRes);
    const ptr32<_sGfxTexture> glowTex(at<uint32_t>(vfxRes + 4u)), flareTex(at<uint32_t>(vfxRes + 0x5cu));
    const ee_float tx = (L.M(0) * dx + L.M(3) * dy + L.M(6) * dz) * kInv4096;
    ee_float ty = (L.M(1) * dx + L.M(4) * dy + L.M(7) * dz) * kInv4096;
    const ee_float tz = (L.M(2) * dx + L.M(5) * dy + L.M(8) * dz) * kInv4096;
    ty = ty * ee_float(0.5f);
    const ee_float k4096(4096.0f);
    int32_t facing = ee_ftoi((tz * (tz * k4096)) / (tz * tz + tx * tx + ty * ty));
    if (tz < ee_float(0.0f))
        facing = static_cast<int32_t>(0u - static_cast<uint32_t>(facing));
    uint32_t present = at<uint8_t>(obj + 0x221u);
    const uint32_t poly = L.poly;
    static constexpr float kU[4] = {0, 1, 0, 1}, kV[4] = {0, 0, 1, 1};
    for (uint32_t k = 0; k < 4; ++k)
    {
        at<ee_float>(poly + 0x40 + 4 * k) = kU[k];
        at<ee_float>(poly + 0x50 + 4 * k) = kV[k];
        at<ee_float>(poly + 0x10 * k + 0xc) = 1.0f;
    }
    at<uint32_t>(poly + 0x70) = 0;
    if ((on & present & 1u) != 0 && facing <= 1000)
    {
        RdrPolyTexture(0, glowTex, 2);
        L.glow(0x224u, 0x1122ffu);
        const int32_t level = flareLevel(static_cast<int32_t>(0u - static_cast<uint32_t>(facing)));
        uint32_t rgba = 0;
        if (level > 0)
            rgba = (scaled(level, 10, 0xff) << 16) + (scaled(level, 0x15, 0xff) << 8) + scaled(level, 0x90, 0xff);
        if (rgba != 0)
        {
            RdrPolyTexture(0, flareTex, 2);
            L.flares(0x224u, 0x404u, true, rgba);
        }
        on = at<uint8_t>(obj + 0x220u);
        present = at<uint8_t>(obj + 0x221u);
    }
    if (((on & present) & 2u) == 0 || facing < -1000)
        return;
    if (facing < 0)
        facing = 0;
    const uint32_t def = pObject->pDef.address;
    const auto R = [&] { return static_cast<uint32_t>(at<uint8_t>(def + 0x50)); };
    const auto Gc = [&] { return static_cast<uint32_t>(at<uint8_t>(def + 0x51)); };
    const auto B = [&] { return static_cast<uint32_t>(at<uint8_t>(def + 0x52)); };
    const int32_t bright = (facing >> 5) + 0x6c;
    const uint32_t glowRgba = (scaled(bright, B(), 0xe6) << 16) + (scaled(bright, Gc(), 0xe6) << 8) + scaled(bright, R(), 0xe6);
    int32_t level = flareLevel(facing);
    uint32_t rgba = 0;
    if (level > 0)
    {
        const uint32_t scale = at<uint8_t>(def + 0x53);
        if (scale != 0xff)
            level = static_cast<int32_t>(static_cast<uint32_t>(level) * scale) >> 8;
        rgba = (scaled(level, B(), 0xff) << 16) + (scaled(level, Gc(), 0xff) << 8) + scaled(level, R(), 0xff);
    }
    RdrPolyTexture(0, glowTex, 2);
    L.glow(0x314u, glowRgba);
    if (rgba == 0)
        return;
    RdrPolyTexture(0, flareTex, 2);
    L.flares(0x314u, 0x414u, false, rgba);
}

} // namespace rc

RC_DECOMP_FN_SETUP_HEAP(ObjectRender, 0x1357e0, rc::fuzz::objectRender)
RC_DECOMP_FN_SETUP_HEAP(ObjectRenderLights, 0x1343f0, rc::fuzz::objectRender)
RC_DECOMP_FN_SETUP_HEAP(ObjectDumptugSled, 0x1387a8, rc::fuzz::objectRender)
