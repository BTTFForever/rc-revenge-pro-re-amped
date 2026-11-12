// Decompiled from VISFX.c: the visual effects (smoke, skid marks, lightning, shadows...). This file holds the small
// functions: the accessors that read a car's physics for the effects, the setters the effects use to fill the
// renderer's current quad (GT4: a textured, Gouraud-shaded four-sided polygon), the sort-position helpers, and the
// handlers that only follow their car. The effects' own update and draw code is still recompiled.
//
// An effect is a `_sVfx_Object`: a 0x40-byte header (list links, handler and renderer, life in frames, the position
// it is sorted by) followed by data of its own kind. The handlers here reach that data by offset.

#include "ai_fuzz.h"
#include "ee_math.h"
#include "vifpk.h"
#include "visfx.h"

#include <emmintrin.h>

namespace rc {

void ResTypeUnSetConstructDaemon(ptr32<char> pType);
void ResTypeSetConstructDaemon(ptr32<char> pType, uint32 constructDaemon, int16 options);

namespace {

using vfx::at;
using vfx::sortPosFromCar;
using vfx::sortPosFromInts;

// The effect's data fields, by offset from the start of the object.
constexpr uint32_t kLifeSpan = 0x10;     // iLifeSpan, frames left
constexpr uint32_t kCar = 0x40;          // the car most effects follow (in the first word of their own data)
constexpr uint32_t kCounter = 0x48;      // a frame countdown some effects keep next to it

// Offsets into the car (_sObject), its float physics and its integer physics.
constexpr uint32_t kObjPhysics = 0x20;
constexpr uint32_t kPhysInteger = 0x1e8;
constexpr uint32_t kIntRenderPos = 0x4;  // _sVec32, the position in 24.8 fixed point

// A car's integer physics state.
_sOPhysLibIntegerCarState &integerState(ptr32<_sObject> pObj)
{
    return *pObj->pPhysics->pInteger;
}

// Wheel records are 0x48 bytes. The index is multiplied in 32 bits, as the original does.
const _sFPhysLibWheel &wheelOf(ptr32<_sObject> pObj, int32 iWheel)
{
    return at<_sFPhysLibWheel>(pObj->pPhysics.address + offsetof(_sFPhysLibCar, wheel) + static_cast<uint32_t>(iWheel) * sizeof(_sFPhysLibWheel));
}

} // namespace

namespace vfx {

void sortPosFromInts(uint32_t fx, int32_t x, int32_t y, int32_t z, uint32_t frameSize)
{
    const int32_t w = stale_stack<int32_t>(frameSize, 0xc);
    const __m128i ints = _mm_set_epi32(w, z, y, x);
    _mm_storeu_ps(&at<float>((fx + kSortPos) & ~0xFu), _mm_cvtepi32_ps(ints));
}

void sortPosFromCar(uint32_t fx, ptr32<_sObject> pObj, uint32_t frameSize)
{
    const sVec32 &pos = pObj->pPhysics->pInteger->renderPos;
    sortPosFromInts(fx, pos.x >> 8, pos.y >> 8, pos.z >> 8, frameSize);
}

void vu0Add(uint32_t dst, uint32_t a, uint32_t b)
{
    const __m128 va = _mm_loadu_ps(&at<float>(a & ~0xFu));
    const __m128 vb = _mm_loadu_ps(&at<float>(b & ~0xFu));
    _mm_storeu_ps(&at<float>(dst & ~0xFu), PS2_VADD(va, vb));
}

void vu0Sub(uint32_t dst, uint32_t a, uint32_t b)
{
    const __m128 va = _mm_loadu_ps(&at<float>(a & ~0xFu));
    const __m128 vb = _mm_loadu_ps(&at<float>(b & ~0xFu));
    _mm_storeu_ps(&at<float>(dst & ~0xFu), PS2_VSUB(va, vb));
}

} // namespace vfx

// The effects that need no code (the draw or update was removed from the shipped game, or never needed any).
void VfxSpriteRotated3D(int32) {}
void VfxSpriteGroundRotated3D(ptr32<SVECTOR>, int32) {}
void VfxSkyBackdrop(ptr32<uint8>) {}
void VfxSkyActualSlowChange(ptr32<uint8>, ptr32<uint8>) {}
void VfxRenderer_CarAerialRibbon(ptr32<_sVfx_Object>) {}
void VfxHandler_Shadow(ptr32<_sVfx_Object>) {}
void VfxRenderer_Shadow(ptr32<_sVfx_Object>) {}
void VfxRenderer_Milk(ptr32<_sVfx_Object>) {}
void VfxSpriteRotatedHud() {}

void VfxTerm()
{
    ResTypeUnSetConstructDaemon(ptr32<char>(0x3280d0u));
}

// The level's effects data, kept where the effects code reads it.
void VfxSetup()
{
    G(visFxVarG).pVfxRes = G(pLvlG)->pVFXData;
}

// Car accessors.

int32 VfxCarSpeedVal(ptr32<_sObject> pObj)
{
    return integerState(pObj).speed;
}

// The effects' distance since the car was last drawn, or 100 (far) while a race is over or a demo runs and the car
// is meant to be drawn.
uint16 VfxHasCarBeenRendered(ptr32<_sObject> pObj)
{
    if ((G(mainVarG).status & (MAIN_RACE_OVER | MAIN_RACE_DEMO)) != 0 && pObj->sDontRenderMe == 0)
        return 100;
    return pObj->zDrawnDist;
}

uint16 VfxShortestLastDistRendered(ptr32<_sObject> pObj)
{
    if ((G(mainVarG).status & (MAIN_RACE_OVER | MAIN_RACE_DEMO)) != 0 && pObj->sDontRenderMe == 0)
        return 100;
    return pObj->zShortestDrawnDist;
}

// Copies 32 bytes of the car's integer state (0x28..0x47: the body matrix and the start of the front wheel matrix) into
// the caller's return buffer, which is returned. The original is a MATRIX return: the 16-bit rotation and the
// translation words it holds.
ptr32<MATRIX> VfxCarMatrixVal(ptr32<MATRIX> ret, ptr32<_sObject> pObj)
{
    uint8_t copy[0x20];
    std::memcpy(copy, &integerState(pObj).mat, sizeof(copy));
    std::memcpy(ret.get(), copy, sizeof(copy));
    return ret;
}

// The car's velocity vector, returned by value: four shorts in one 64-bit register.
uint64_t VfxCarVelocityVal(ptr32<_sObject> pObj)
{
    const sVec16 &v = integerState(pObj).vel;
    return static_cast<uint64_t>(static_cast<uint16_t>(v.x)) | (static_cast<uint64_t>(static_cast<uint16_t>(v.y)) << 16) |
           (static_cast<uint64_t>(static_cast<uint16_t>(v.z)) << 32) | (static_cast<uint64_t>(static_cast<uint16_t>(v.pad)) << 48);
}

// The car's position in float units in a shared vector (whose fourth float is left alone), returned.
ptr32<ee_float> VfxVUCarPositionPoi(ptr32<_sObject> pObj)
{
    const _sFPhysLibCar &physics = *pObj->pPhysics;
    const ee_float scale = 0.00390625f; // 1/256
    sceVu0FVECTOR &out = G(globalVUVec);
    out[0] = physics.renderPos.x * scale;
    out[1] = physics.renderPos.y * scale;
    out[2] = physics.renderPos.z * scale;
    return ptr32<ee_float>(guest_addr(&out));
}

// Wheel queries.

int32 VfxIsWheelSkidding(ptr32<_sObject> pObj, int32 iWheel)
{
    return wheelOf(pObj, iWheel).flags & 2;
}

// A car's wheel touches the ground when its flag says so; a boat's when its sphere is in water.
int32 VfxIsWheelTouching(ptr32<_sObject> pObj, int32 iWheel)
{
    if (static_cast<uint32_t>(pObj->pDef->type) < static_cast<uint32_t>(OBJECT_TYPE_BADDRC_BOAT))
        return wheelOf(pObj, iWheel).flags & 1;
    const uint32_t sphere = pObj->pPhysics->pSphere.address + static_cast<uint32_t>(iWheel) * sizeof(_sFPhysLibColSphere);
    return ee_float(0.0f) < at<_sFPhysLibColSphere>(sphere).liquidDepth ? 1 : 0;
}

// The surface material under a wheel; anything past the last known one counts as 0x1d.
uint8 VfxMaterialUnderWheel(ptr32<_sObject> pObj, int32 iWheel)
{
    const uint8_t material = static_cast<uint8_t>(wheelOf(pObj, iWheel).drivingOnMtl);
    return material < 0x48 ? material : 0x1d;
}

// Sort positions.

void VfxSVtoV3(ptr32<SVECTOR> sv, ptr32<sVec3> v3)
{
    v3->x = ee_itof(sv->vx);
    v3->y = ee_itof(sv->vy);
    v3->z = ee_itof(sv->vz);
}

void VfxSortPos(ptr32<SVECTOR> sv, ptr32<_sVfx_Object> fx)
{
    sortPosFromInts(fx.address, sv->vx, sv->vy, sv->vz, 0x20);
}

void VfxCarSortPos(ptr32<_sObject> pObj, ptr32<_sVfx_Object> fx)
{
    sortPosFromCar(fx.address, pObj, 0x20);
}

// qsort's comparison for effects: farther sort positions first (the second float of each sort position).
int32 VfxSortFunc(ptr32<ee_float> arg1, ptr32<ee_float> arg2)
{
    return ee_ftoi(arg2[1] - arg1[1]);
}

// The renderer's current quad (visFxVarG.pGT4): the effects fill its colours and texture coordinates one corner at a
// time.

void VfxPutGT4rgba(uint32 v1, uint32 v2, uint32 v3, uint32 v4)
{
    _sRdrPoly &poly = *G(visFxVarG).pGT4;
    poly.rgba[3] = v4;
    poly.rgba[0] = v1;
    poly.rgba[1] = v2;
    poly.rgba[2] = v3;
}

void VfxPutGT4rgba0(uint32 v1) { G(visFxVarG).pGT4->rgba[0] = v1; }
void VfxPutGT4rgba1(uint32 v1) { G(visFxVarG).pGT4->rgba[1] = v1; }
void VfxPutGT4rgba2(uint32 v1) { G(visFxVarG).pGT4->rgba[2] = v1; }
void VfxPutGT4rgba3(uint32 v1) { G(visFxVarG).pGT4->rgba[3] = v1; }

void VfxPutUV0(ee_float u, ee_float v)
{
    _sRdrPoly &poly = *G(visFxVarG).pGT4;
    poly.v[0] = v;
    poly.u[0] = u;
}

void VfxPutUV1(ee_float u, ee_float v)
{
    _sRdrPoly &poly = *G(visFxVarG).pGT4;
    poly.v[1] = v;
    poly.u[1] = u;
}

void VfxPutUV2(ee_float u, ee_float v)
{
    _sRdrPoly &poly = *G(visFxVarG).pGT4;
    poly.v[2] = v;
    poly.u[2] = u;
}

void VfxPutUV3(ee_float u, ee_float v)
{
    _sRdrPoly &poly = *G(visFxVarG).pGT4;
    poly.v[3] = v;
    poly.u[3] = u;
}

// Handlers that keep an effect on its car.

// The heat ray's blast: one frame less of life and of its own countdown, sorted at the car it follows.
void VfxHandler_CarHeatRayBlast(ptr32<_sVfx_Object> mem)
{
    const ptr32<_sObject> car(at<uint32_t>(mem.address + kCar));
    const uint16_t counter = at<uint16_t>(mem.address + kCounter);
    const uint16_t life = at<uint16_t>(mem.address + kLifeSpan);
    at<uint16_t>(mem.address + kCounter) = static_cast<uint16_t>(counter - 1);
    at<uint16_t>(mem.address + kLifeSpan) = static_cast<uint16_t>(life - 1);
    sortPosFromCar(mem.address, car, 0x20);
}

// As above; the sort position is set first, and the frame is 0x30 bytes.
void VfxHandler_BlastLightning(ptr32<_sVfx_Object> mem)
{
    const ptr32<_sObject> car(at<uint32_t>(mem.address + kCar));
    sortPosFromCar(mem.address, car, 0x30);
    const uint16_t life = at<uint16_t>(mem.address + kLifeSpan);
    const uint16_t counter = at<uint16_t>(mem.address + kCounter);
    at<uint16_t>(mem.address + kLifeSpan) = static_cast<uint16_t>(life - 1);
    at<uint16_t>(mem.address + kCounter) = static_cast<uint16_t>(counter - 1);
}

// Sarge's lights: the effect object is large; its car is at +0x650 and a rotation counter at +0x654 advances by 16.
void VfxHandler_SargeLights(ptr32<_sVfx_Object> mem)
{
    const ptr32<_sObject> car(at<uint32_t>(mem.address + 0x650));
    at<int32_t>(mem.address + 0x654) += 0x10;
    sortPosFromCar(mem.address, car, 0x20);
}


// ---------------------------------------------------------------------------------------------------------------------
// The effect list, wheel and car positions, particle movers and the effects that draw polygons.

// The effects' resources are loaded by a daemon that a later function installs: this clears it, and sets the shadow
// sizes the effects use for each car body. The table is two halfwords per body, in the order the original stores them.
void VfxInit()
{
    ResTypeSetConstructDaemon(ptr32<char>(0x3280d0u), 0u, 4);
    static constexpr uint16_t kShadowSizes[][2] = {
        {0x2738, 22}, {0x2740, 18}, {0x2748, 11}, {0x275c, 9}, {0x2760, 7}, {0x2764, 13}, {0x2766, 3}, {0x276e, 4},
        {0x2774, 10}, {0x2734, 9}, {0x273a, 10}, {0x273c, 10}, {0x273e, 4}, {0x2742, 10}, {0x2744, 10}, {0x2746, 8},
        {0x274a, 7}, {0x274c, 6}, {0x274e, 4}, {0x2750, 3}, {0x2752, 5}, {0x2754, 9}, {0x2756, 7}, {0x2758, 7},
        {0x275a, 5}, {0x275e, 7}, {0x2762, 6}, {0x2768, 10}, {0x276a, 6}, {0x276c, 4}, {0x2770, 8}, {0x2772, 8},
        {0x2736, 0}, {0x2776, 6}, {0x277e, 8}, {0x277a, 5}, {0x277c, 14}, {0x2778, 5}};
    for (const auto &entry : kShadowSizes)
        at<uint16_t>(0x1f1000u + entry[0]) = entry[1];
}

// The PSX rotation matrix (3 by 3 shorts, 4.12 fixed point) as a PS2 matrix in a shared global, transposed.
ptr32<ee_float> VfxPSXToVUMatrix(ptr32<MATRIX> pMat)
{
    const ee_float scale = float_from_bits(0x39800000u); // 1/4096
    ee_float *out = &G(globalVUMat)[0][0];
    const MATRIX &m = *pMat;
    out[0] = ee_itof(m.m[0][0]) * scale;
    out[1] = ee_itof(m.m[1][0]) * scale;
    out[2] = ee_itof(m.m[2][0]) * scale;
    out[3] = 0.0f;
    out[4] = ee_itof(m.m[0][1]) * scale;
    out[5] = ee_itof(m.m[1][1]) * scale;
    out[6] = ee_itof(m.m[2][1]) * scale;
    out[7] = 0.0f;
    out[8] = ee_itof(m.m[0][2]) * scale;
    out[9] = ee_itof(m.m[1][2]) * scale;
    out[10] = ee_itof(m.m[2][2]) * scale;
    out[11] = 0.0f;
    out[12] = out[13] = out[14] = out[15] = 0.0f;
    return ptr32<ee_float>(guest_addr(out));
}

// Unlinks an effect from the list and frees it.
void VfxDelete(ptr32<_sVfx_Object> pFX)
{
    GuestFrame frame(0x20);
    const ptr32<memHandle> handle = frame.local<memHandle>(0);
    *handle = memHandle(pFX.address);
    visFxClass &fx = G(visFxVarG);
    if (pFX->Prev)
        pFX->Prev->Next = pFX->Next;
    if (pFX->Next)
        pFX->Next->Prev = pFX->Prev;
    if (pFX == fx.vfxHeader)
    {
        fx.vfxHeader = pFX->Next;
        if (fx.vfxHeader)
            fx.vfxHeader->Prev = nullptr;
    }
    if (pFX == fx.vfxFooter)
    {
        fx.vfxFooter = pFX->Prev;
        if (fx.vfxFooter)
            fx.vfxFooter->Next = nullptr;
    }
    MemBlockFree(handle);
}

// Deletes every effect except types 0x23 and 0x29, and clears the cars' alternative render object.
void VfxHandOverFXDelete()
{
    GuestFrame frame(0x50);
    const ptr32<memHandle> handle = frame.local<memHandle>(0);
    visFxClass &fx = G(visFxVarG);
    ptr32<_sVfx_Object> cur = fx.vfxHeader;
    *handle = memHandle(cur.address);
    while (cur)
    {
        const uint16_t type = cur->iType;
        if (type == 0x23 || type == 0x29)
        {
            cur = cur->Next;
            *handle = memHandle(cur.address);
        }
        else
        {
            if (cur->Prev)
                cur->Prev->Next = cur->Next;
            if (cur->Next)
                cur->Next->Prev = cur->Prev;
            if (cur == fx.vfxHeader)
            {
                fx.vfxHeader = cur->Next;
                if (fx.vfxHeader)
                    fx.vfxHeader->Prev = nullptr;
            }
            if (cur == fx.vfxFooter)
            {
                fx.vfxFooter = cur->Prev;
                if (fx.vfxFooter)
                    fx.vfxFooter->Next = nullptr;
            }
            const ptr32<_sVfx_Object> next = cur->Next;
            MemBlockFree(handle);
            *handle = memHandle(next.address);
            cur = next;
        }
        cur = ptr32<_sVfx_Object>(handle->address);
    }
    G(carListVarG).alternativeRendObj = 0;
}

// Runs the handler of every effect that has life left. A handler may delete or add effects, so each effect's next
// pointer is read after its handler has run.
void VfxCallHandlers()
{
    GuestFrame frame(0x20);
    for (ptr32<_sVfx_Object> fx = G(visFxVarG).vfxHeader; fx; fx = fx->Next)
        if (fx->iLifeSpan != 0)
            guest_call(fx->Handler.address, fx);
}

// A car's position in whole units, as four shorts in one register (the fourth, the padding, is the shared vector's).
uint64_t VfxCarPositionVal(ptr32<_sObject> pObj)
{
    const sVec32 &pos = integerState(pObj).renderPos;
    SVECTOR &out = G(svVelTemp);
    out.vx = static_cast<int16>(pos.x >> 8);
    out.vy = static_cast<int16>(pos.y >> 8);
    out.vz = static_cast<int16>(pos.z >> 8);
    return static_cast<uint64_t>(static_cast<uint16_t>(out.vx)) | (static_cast<uint64_t>(static_cast<uint16_t>(out.vy)) << 16) |
           (static_cast<uint64_t>(static_cast<uint16_t>(out.vz)) << 32) | (static_cast<uint64_t>(static_cast<uint16_t>(out.pad)) << 48);
}

namespace {

// A wheel's position in whole units, in the second shared vector: a car's wheel from its physics; a boat's, which has
// no wheels, a random point around the boat.
void wheelPosition(ptr32<_sObject> pObj, int32 iWheel)
{
    SVECTOR &out = G(svVelTemp2);
    if (static_cast<uint32_t>(pObj->pDef->type) < static_cast<uint32_t>(OBJECT_TYPE_BADDRC_BOAT))
    {
        const ee_float scale = 0.00390625f; // 1/256
        const _sFPhysLibWheel &wheel = wheelOf(pObj, iWheel);
        const ee_float x = wheel.pos.x * scale;
        const ee_float y = wheel.pos.y * scale;
        const ee_float z = wheel.pos.z * scale;
        out.vx = static_cast<int16>(ee_ftoi(x));
        out.vy = static_cast<int16>(ee_ftoi(y));
        out.vz = static_cast<int16>(ee_ftoi(z));
    }
    else
    {
        int32 r = rand();
        sOPhysLibIntegerCarState *state = pObj->pPhysics->pInteger.get();
        out.vx = static_cast<int16>((state->renderPos.x >> 8) + ((r >> 15) & 0x3f) - 0x20);
        out.vy = static_cast<int16>((state->renderPos.y >> 8) - 0x10);
        r = rand();
        out.vz = static_cast<int16>((pObj->pPhysics->pInteger->renderPos.z >> 8) + ((r >> 15) & 0x3f) - 0x20);
    }
}

} // namespace

ptr32<SVECTOR> VfxWheelPositionPoi(ptr32<_sObject> pObj, int32 iWheel)
{
    wheelPosition(pObj, iWheel);
    return ptr32<SVECTOR>(guest_addr(&G(svVelTemp2)));
}

uint64_t VfxWheelPositionVal(ptr32<_sObject> pObj, int32 iWheel)
{
    wheelPosition(pObj, iWheel);
    const SVECTOR &out = G(svVelTemp2);
    return static_cast<uint64_t>(static_cast<uint16_t>(out.vx)) | (static_cast<uint64_t>(static_cast<uint16_t>(out.vy)) << 16) |
           (static_cast<uint64_t>(static_cast<uint16_t>(out.vz)) << 32) | (static_cast<uint64_t>(static_cast<uint16_t>(out.pad)) << 48);
}

// The milk float: both of the car's wheel objects (the front pair) get the "slippery" bit while it lasts, and the
// player's state word is flagged.
void VfxHandler_Milk(ptr32<_sVfx_Object> mem)
{
    const uint32_t car = at<uint32_t>(mem.address + kCar);
    at<uint16_t>(mem.address + kLifeSpan) = static_cast<uint16_t>(at<uint16_t>(mem.address + kLifeSpan) - 1);
    const uint32_t first = at<uint32_t>(car + 0x178);
    at<uint8_t>(first + 0x18b) = at<uint8_t>(first + 0x18b) | 4;
    const uint32_t second = at<uint32_t>(car + 0x17c);
    at<uint8_t>(second + 0x18b) = at<uint8_t>(second + 0x18b) | 4;
    uint16_t &state = at<uint16_t>(0x1f37e0u + static_cast<uint32_t>(at<uint8_t>(car + 0x45)) * 4u);
    state = state | 0x20;
    if (at<uint16_t>(mem.address + kLifeSpan) == 0)
    {
        const uint32_t a = at<uint32_t>(car + 0x178);
        at<uint8_t>(a + 0x18b) = at<uint8_t>(a + 0x18b) & 0xfb;
        const uint32_t b = at<uint32_t>(car + 0x17c);
        at<uint8_t>(b + 0x18b) = at<uint8_t>(b + 0x18b) & 0xfb;
    }
}

// Aerial lightning: the three previous positions shift along by one, a fresh random offset is chosen for the newest,
// and the effect is sorted at the car's position (stack frame 0x50).
void VfxHandler_AerialLightning(ptr32<_sVfx_Object> mem)
{
    const uint32_t m = mem.address;
    uint64_t t;
    std::memcpy(&t, &at<uint8_t>(m + 0x58), 8);
    std::memcpy(&at<uint8_t>(m + 0x60), &t, 8);
    std::memcpy(&t, &at<uint8_t>(m + 0x50), 8);
    std::memcpy(&at<uint8_t>(m + 0x58), &t, 8);
    std::memcpy(&t, &at<uint8_t>(m + 0x48), 8);
    std::memcpy(&at<uint8_t>(m + 0x50), &t, 8);
    int32 r = rand();
    at<int16_t>(m + 0x48) = static_cast<int16_t>((r >> 15) % 15 - 7);
    r = rand();
    at<int16_t>(m + 0x4a) = static_cast<int16_t>((r >> 15) % 15 - 7);
    r = rand();
    const sVec32 &pos = integerState(ptr32<_sObject>(at<uint32_t>(m + 0x78))).renderPos;
    const int32_t x = pos.x >> 8, y = pos.y >> 8, z = pos.z >> 8;
    at<int16_t>(m + 0x4c) = static_cast<int16_t>((r >> 15) % 15 - 8);
    sortPosFromInts(m, x, y, z, 0x50);
    at<uint16_t>(m + kLifeSpan) = static_cast<uint16_t>(at<uint16_t>(m + kLifeSpan) - 1);
}

// Damage lightning: as above with five stored positions and larger offsets (stack frame 0x60).
void VfxHandler_DamageLightning(ptr32<_sVfx_Object> mem)
{
    const uint32_t m = mem.address;
    const uint32_t car = at<uint32_t>(m + 0x78);
    uint64_t t;
    for (uint32_t offset = 0x40; offset < 0x60; offset += 8)
    {
        std::memcpy(&t, &at<uint8_t>(m + offset + 8), 8);
        std::memcpy(&at<uint8_t>(m + offset), &t, 8);
    }
    std::memcpy(&t, &at<uint8_t>(m + 0x68), 8);
    std::memcpy(&at<uint8_t>(m + 0x60), &t, 8);
    int32 r = rand();
    at<int16_t>(m + 0x68) = static_cast<int16_t>((r >> 15) % 0x3f - 0x20);
    r = rand();
    at<int16_t>(m + 0x6a) = static_cast<int16_t>((r >> 15) % 0x3f - 0x20);
    r = rand();
    const sVec32 &pos = integerState(ptr32<_sObject>(car)).renderPos;
    const int32_t x = pos.x >> 8, y = pos.y >> 8, z = pos.z >> 8;
    at<int16_t>(m + 0x6c) = static_cast<int16_t>((r >> 15) % 0x3f - 0x20);
    sortPosFromInts(m, x, y, z, 0x60);
    at<uint16_t>(m + kLifeSpan) = static_cast<uint16_t>(at<uint16_t>(m + kLifeSpan) - 1);
}

// The blast of the moon rover's weapon: the first frame (life 0xffff) picks the direction from the car's render object
// and fires a particle pair along it; every frame after moves them by that step.
void VfxHandler_BlastMoonrover(ptr32<_sVfx_Object> mem)
{
    const uint32_t m = mem.address;
    const uint16_t life = at<uint16_t>(m + kLifeSpan);
    const uint32_t car = at<uint32_t>(m + 0x40);
    if (life == 0xffff)
    {
        const uint32_t render = at<uint32_t>(car + 0x18);
        const uint16_t sx = at<uint16_t>(render + 0x1c);
        uint16_t sy, sz;
        sz = at<uint16_t>(render + 0x28);
        if (at<int32_t>(at<uint32_t>(car + 0xc) + 4) == 0x25)
        {
            const uint32_t physics = at<uint32_t>(at<uint32_t>(car + 0x20) + 0x1e0);
            if (ee_float(at<float>(physics + 0x528)) < ee_float(0.0f))
                sy = at<uint16_t>(render + 0x22);
            else
                sy = static_cast<uint16_t>(ee_ftoi(ee_float(4096.0f) - ee_float(at<float>(physics + 0xa4))));
        }
        else
            sy = at<uint16_t>(render + 0x22);
        const ee_float px = at<float>(m + 0x50), py = at<float>(m + 0x54), pz = at<float>(m + 0x58);
        const ee_float x = px + ee_itof((static_cast<int16_t>(sx) * 200) >> 12);
        const ee_float y = py + ee_itof((static_cast<int16_t>(sy) * 200) >> 12);
        const ee_float z = pz + ee_itof((static_cast<int16_t>(sz) * 200) >> 12);
        const ee_float k = 0.75f;
        at<float>(m + 0x60) = x;
        at<float>(m + 0x64) = y;
        at<float>(m + 0x68) = z;
        at<float>(m + 0x70) = (x - px) * k;
        at<float>(m + 0x74) = (y - py) * k;
        at<float>(m + 0x78) = (z - pz) * k;
    }
    else
    {
        const ee_float vx = at<float>(m + 0x70), vy = at<float>(m + 0x74), vz = at<float>(m + 0x78);
        const ee_float px = ee_float(at<float>(m + 0x50)) + vx;
        const ee_float py = ee_float(at<float>(m + 0x54)) + vy;
        const ee_float pz = ee_float(at<float>(m + 0x58)) + vz;
        const ee_float qx = ee_float(at<float>(m + 0x60)) + vx;
        const ee_float qy = ee_float(at<float>(m + 0x64)) + vy;
        const ee_float qz = ee_float(at<float>(m + 0x68)) + vz;
        at<float>(m + 0x50) = px;
        at<float>(m + 0x54) = py;
        at<float>(m + 0x58) = pz;
        at<float>(m + 0x60) = qx;
        at<float>(m + 0x64) = qy;
        at<float>(m + 0x68) = qz;
    }
    at<uint16_t>(m + 0x80) = static_cast<uint16_t>(at<uint16_t>(m + 0x80) - 1);
    at<float>(m + 0x20) = at<float>(m + 0x60);
    at<float>(m + 0x24) = at<float>(m + 0x64);
    at<float>(m + 0x28) = at<float>(m + 0x68);
    at<uint16_t>(m + kLifeSpan) = static_cast<uint16_t>(life - 1);
}

// Adds a spark to a spark effect's ring of particles, if the car is near enough to be seen and the ring has room.
void VfxCreate_Sparks(ptr32<_sVfx_Object> mem, ptr32<vec32> vPos)
{
    const uint32_t m = mem.address;
    const ptr32<_sObject> car(at<uint32_t>(m + 0x44));
    const int32_t px = vPos->x, py = vPos->y, pz = vPos->z;
    if (VfxHasCarBeenRendered(car) < 0x157c)
    {
        const _sOPhysLibIntegerCarState &state = integerState(car);
        SVECTOR &vel = G(svVelTemp);
        vel.vx = static_cast<int16>(state.renderPos.x >> 8);
        vel.vy = static_cast<int16>(state.renderPos.y >> 8);
        vel.vz = static_cast<int16>(state.renderPos.z >> 8);
        uint8_t &totalAnd = at<uint8_t>(m + 0x40), &count = at<uint8_t>(m + 0x41), &footer = at<uint8_t>(m + 0x43);
        if (count < totalAnd)
        {
            const uint32_t p = m + 0x48 + static_cast<uint32_t>(footer) * 0x14u;
            const int32_t bodyX = state.renderPos.x >> 8;
            at<uint16_t>(p) = static_cast<uint16_t>((px >> 4) - (bodyX << 4));
            at<uint16_t>(p + 2) = static_cast<uint16_t>((py >> 4) - (static_cast<int32_t>(static_cast<uint16_t>(vel.vy)) << 4));
            at<uint16_t>(p + 4) = static_cast<uint16_t>((pz >> 4) - (static_cast<int32_t>(static_cast<uint16_t>(vel.vz)) << 4));
            int32 r = rand();
            at<uint16_t>(p + 8) = static_cast<uint16_t>(((r >> 15) & 0xff) - ((state.vel.x >> 5) + 0x80));
            r = rand();
            at<uint16_t>(p + 0xa) = static_cast<uint16_t>(((r >> 15) & 0xff) - ((state.vel.y >> 5) + 0x80));
            r = rand();
            at<uint16_t>(p + 0xc) = static_cast<uint16_t>(((r >> 15) & 0xff) - ((state.vel.z >> 5) + 0x80));
            at<uint16_t>(p + 0x10) = 7;
            footer = static_cast<uint8_t>(totalAnd & (footer + 1));
            count = static_cast<uint8_t>(count + 1);
        }
    }
}

// Moves every live particle of a ring by its velocity, spinning and growing it, and removes the dead ones by moving the
// oldest (the head) into their place. Particles are 0x18 bytes.
void VfxParticleMover(ptr32<_sVfx_PartHeader> pHeader, ptr32<_VFX_PARTICLE> Particles)
{
    uint8_t &totalAnd = pHeader->TotalAnd, &counter = pHeader->Counter, &header = pHeader->Header;
    const uint8_t &footer = pHeader->Footer;
    uint32_t index = header;
    if (header == footer)
        return;
    do
    {
        const ptr32<_VFX_PARTICLE> p = Particles + static_cast<int32_t>(index);
        const uint16_t remaining = static_cast<uint16_t>(p->usLife - 1);
        if (p->usLife != 0)
        {
            const uint16_t x = static_cast<uint16_t>(p->svRelPos.vx + p->svVelocity.vx);
            const uint16_t y = static_cast<uint16_t>(p->svRelPos.vy + p->svVelocity.vy);
            const uint16_t z = static_cast<uint16_t>(p->svRelPos.vz + p->svVelocity.vz);
            const uint16_t angle = static_cast<uint16_t>(p->sAngle + p->sVelAngle);
            const uint8_t size = static_cast<uint8_t>(p->i8Size + static_cast<uint8_t>(p->i8VelSize));
            p->svRelPos.vx = static_cast<int16>(x);
            p->svRelPos.vy = static_cast<int16>(y);
            p->svRelPos.vz = static_cast<int16>(z);
            p->sAngle = static_cast<int16>(angle);
            p->i8Size = size;
            p->usLife = remaining;
            if (remaining == 0)
            {
                if (index != header)
                {
                    uint8_t tmp[sizeof(_VFX_PARTICLE)];
                    std::memcpy(tmp, (Particles + static_cast<int32_t>(header)).get(), sizeof(tmp));
                    std::memcpy(p.get(), tmp, sizeof(tmp));
                }
                counter = static_cast<uint8_t>(counter + 0xff);
                header = static_cast<uint8_t>(totalAnd & (header + 1));
            }
        }
        index = (index + 1) & totalAnd;
    } while (index != footer);
}

// As VfxParticleMover for the simple particles (0x14 bytes: no size or spin).
void VfxSimpleParticleMover(ptr32<_sVfx_PartHeader> pHeader, ptr32<_VFX_SIMPLEPARTICLE> Particles)
{
    uint8_t &totalAnd = pHeader->TotalAnd, &counter = pHeader->Counter, &header = pHeader->Header;
    const uint8_t &footer = pHeader->Footer;
    uint32_t index = header;
    if (header == footer)
        return;
    do
    {
        const ptr32<_VFX_SIMPLEPARTICLE> p = Particles + static_cast<int32_t>(index);
        const uint16_t remaining = static_cast<uint16_t>(p->usLife - 1);
        if (p->usLife != 0)
        {
            const uint16_t x = static_cast<uint16_t>(p->svRelPos.vx + p->svVelocity.vx);
            const uint16_t y = static_cast<uint16_t>(p->svRelPos.vy + p->svVelocity.vy);
            const uint16_t z = static_cast<uint16_t>(p->svRelPos.vz + p->svVelocity.vz);
            p->svRelPos.vx = static_cast<int16>(x);
            p->svRelPos.vy = static_cast<int16>(y);
            p->svRelPos.vz = static_cast<int16>(z);
            p->usLife = remaining;
            if (remaining == 0)
            {
                if (index != header)
                {
                    uint8_t tmp[sizeof(_VFX_SIMPLEPARTICLE)];
                    std::memcpy(tmp, (Particles + static_cast<int32_t>(header)).get(), sizeof(tmp));
                    std::memcpy(p.get(), tmp, sizeof(tmp));
                }
                counter = static_cast<uint8_t>(counter + 0xff);
                header = static_cast<uint8_t>(totalAnd & (header + 1));
            }
        }
        index = (index + 1) & totalAnd;
    } while (index != footer);
}

// The renderer's current quad (pGT4) built from two ends and a width, for the effects that draw ribbons.

namespace {

constexpr uint32_t kCont = 0x1f1040u;      // visFxVarG.vuCont: the half-width vector the ribbon is built around
constexpr uint32_t kCont2 = 0x1f1050u;     // visFxVarG.vuCont2: the previous one, for continuing a ribbon
constexpr uint32_t kCamTrans = 0x1f1160u;  // visFxVarG.ps2CameraTrans

// Corners 2 and 3 are corner 1 minus and plus the vector in kCont; 1 and 0 are corner 0 plus and minus the vector in
// `back` (kCont2, or kCont itself); the vector is then kept in kCont2.
void ribbonCorners(uint32_t quad, uint32_t back)
{
    vfx::vu0Sub(quad + 0x20, quad + 0x10, kCont);
    vfx::vu0Add(quad + 0x30, quad + 0x10, kCont);
    vfx::vu0Add(quad + 0x10, quad, back);
    vfx::vu0Sub(quad, quad, back);
    std::memcpy(guest_host(kCont2), guest_host(kCont), 16);
}

// The camera-facing width vector for the ribbon from the quad's corner 0 to corner 1: the normal of the ends and the
// direction to the camera, scaled to `width`. The VU0 library routines are the game's own.
void facingVector(const GuestFrame &frame, uint32_t quad, ee_float width)
{
    const ptr32<ee_float> vu0 = frame.local<ee_float>(0);
    const ptr32<ee_float> vu1 = frame.local<ee_float>(0x10);
    vfx::vu0Sub(vu0.address, quad + 0x10, quad);
    vfx::vu0Sub(vu1.address, kCamTrans, quad);
    guest_call(0x1c7820u, vu0, vu0, vu1);                                     // sceVu0OuterProduct
    guest_call(0x1c7868u, ptr32<ee_float>(kCont), vu0);                       // sceVu0Normalize
    sceVu0ScaleVector(ptr32<ee_float>(kCont), ptr32<ee_float>(kCont), width); // sceVu0ScaleVector
}

} // namespace

// Ends A and B as shorts (whole units).
void VfxPutGT4vecs(ptr32<SVECTOR> svA, ptr32<SVECTOR> svB, ptr32<SVECTOR> svC, ptr32<SVECTOR> svD)
{
    const uint32_t quad = G(visFxVarG).pGT4.address;
    const SVECTOR *corners[4] = {svA.get(), svB.get(), svC.get(), svD.get()};
    for (int c = 0; c < 4; ++c)
    {
        at<float>(quad + 0x10u * c) = ee_itof(corners[c]->vx);
        at<float>(quad + 0x10u * c + 4) = ee_itof(corners[c]->vy);
        at<float>(quad + 0x10u * c + 8) = ee_itof(corners[c]->vz);
    }
}

// Continues a ribbon from ends given as shorts: the width vector is the one the last call left.
void VfxPutGT4FacingContinue(ptr32<SVECTOR> svA, ptr32<SVECTOR> svB)
{
    GuestFrame frame(0x40);
    const uint32_t quad = G(visFxVarG).pGT4.address;
    at<float>(quad) = ee_itof(svA->vx);
    at<float>(quad + 4) = ee_itof(svA->vy);
    at<float>(quad + 8) = ee_itof(svA->vz);
    at<float>(quad + 0x10) = ee_itof(svB->vx);
    at<float>(quad + 0x14) = ee_itof(svB->vy);
    at<float>(quad + 0x18) = ee_itof(svB->vz);
    ribbonCorners(quad, kCont2);
    RdrPoly(0, ptr32<_sRdrPoly>(quad));
}

// As above for ends given as float vectors.
void VfxPutGT4FacingContinueVU(ptr32<ee_float> vA, ptr32<ee_float> vB)
{
    GuestFrame frame(0x40);
    const uint32_t quad = G(visFxVarG).pGT4.address;
    for (int i = 0; i < 3; ++i)
    {
        at<float>(quad + 4u * i) = vA[i];
        at<float>(quad + 0x10 + 4u * i) = vB[i];
    }
    ribbonCorners(quad, kCont2);
    RdrPoly(0, ptr32<_sRdrPoly>(quad));
}

// A ribbon between two ends (shorts) of the given width, facing the camera, textured and drawn.
void VfxPutGT4FacingPoly(ptr32<SVECTOR> svA, ptr32<SVECTOR> svB, ptr32<_sGfxTexture> texture, int32 style, ee_float width, ee_float fZOffset)
{
    GuestFrame frame(0x90);
    const uint32_t quad = G(visFxVarG).pGT4.address;
    at<float>(quad) = ee_itof(svA->vx);
    at<float>(quad + 4) = ee_itof(svA->vy);
    at<float>(quad + 8) = ee_itof(svA->vz);
    at<float>(quad + 0x10) = ee_itof(svB->vx);
    at<float>(quad + 0x14) = ee_itof(svB->vy);
    at<float>(quad + 0x18) = ee_itof(svB->vz);
    facingVector(frame, quad, width);
    ribbonCorners(quad, kCont);
    at<float>(quad + 0x70) = fZOffset;
    RdrPolyTexture(0, texture, static_cast<u32>(style));
    RdrPoly(0, ptr32<_sRdrPoly>(quad));
}

// As above for ends given as float vectors.
void VfxPutGT4FacingPolyVU(ptr32<ee_float> vA, ptr32<ee_float> vB, ptr32<_sGfxTexture> texture, int32 style, ee_float width, ee_float fZOffset)
{
    GuestFrame frame(0x90);
    const uint32_t quad = G(visFxVarG).pGT4.address;
    for (int i = 0; i < 3; ++i)
    {
        at<float>(quad + 4u * i) = vA[i];
        at<float>(quad + 0x10 + 4u * i) = vB[i];
    }
    facingVector(frame, quad, width);
    ribbonCorners(quad, kCont);
    at<float>(quad + 0x70) = fZOffset;
    RdrPolyTexture(0, texture, static_cast<u32>(style));
    RdrPoly(0, ptr32<_sRdrPoly>(quad));
}

namespace {

// The four corners of the sprite quad, built from the sprite effect's own 4.12 corner offsets: rotate by the matrix,
// move to the sprite's position, colour, texture coordinates, draw. `frame` is the caller's frame (local vector at
// vPos). Both variants end the same way.
void spriteQuadFinish(const ptr32<ee_float> mat, const ptr32<ee_float> vPos, uint32_t sprite, uint32_t quad, bool flipX, bool ground, ee_float zOffset)
{
    ptr32<_sRdrPoly> poly(quad);
    // Each corner is rotated in place, and the sprite position is written just before the call that rotates the next.
    sceVu0ApplyMatrix(ptr32<ee_float>(quad), mat, ptr32<ee_float>(quad));
    vPos[0] = ee_float(at<float>(sprite + 0x10));
    sceVu0ApplyMatrix(ptr32<ee_float>(quad + 0x10), mat, ptr32<ee_float>(quad + 0x10));
    vPos[1] = ee_float(at<float>(sprite + 0x14));
    sceVu0ApplyMatrix(ptr32<ee_float>(quad + 0x20), mat, ptr32<ee_float>(quad + 0x20));
    vPos[2] = ee_float(at<float>(sprite + 0x18));
    sceVu0ApplyMatrix(ptr32<ee_float>(quad + 0x30), mat, ptr32<ee_float>(quad + 0x30));
    for (uint32_t corner = 0; corner < 4; ++corner)
        vfx::vu0Add(quad + 0x10 * corner, vPos.address, quad + 0x10 * corner);
    const uint32_t rgb = at<uint32_t>(sprite + 0x28);
    at<uint32_t>(quad + 0x60) = rgb;
    at<uint32_t>(quad + 0x6c) = rgb;
    at<uint32_t>(quad + 0x68) = rgb;
    at<uint32_t>(quad + 0x64) = rgb;
    if (ground || flipX)
    {
        VfxPutUV0(1.0f, 0.0f);
        VfxPutUV1(0.0f, 0.0f);
        VfxPutUV2(1.0f, 1.0f);
        VfxPutUV3(0.0f, 1.0f);
    }
    else
    {
        VfxPutUV0(0.0f, 0.0f);
        VfxPutUV1(1.0f, 0.0f);
        VfxPutUV2(0.0f, 1.0f);
        VfxPutUV3(1.0f, 1.0f);
    }
    RdrPolyTexture(0, ptr32<_sGfxTexture>(at<uint32_t>(sprite + 0x20)), at<uint8_t>(sprite + 0x24));
    poly->zOffset = zOffset;
    RdrPoly(0, poly);
}

} // namespace

// A camera-facing sprite effect: the current sprite (visFxVarG.sprite) as a quad rotated by its angle in the camera's
// plane, placed at its position and drawn with the given depth offset. (Stack frame 0xf0: the matrix at +0x10, the
// position at +0; its fourth float is stale stack.)
void VfxPS2SpriteRotated3D(ee_float zOffset)
{
    GuestFrame frame(0xF0);
    const ptr32<ee_float> mat = frame.local<ee_float>(0x10);
    const ptr32<ee_float> vPos = frame.local<ee_float>(0);
    guest_call(0x1c7ac8u, mat); // sceVu0UnitMatrix
    const uint32_t sprite = G(visFxVarG).sprite.address;
    const uint32_t quad = G(visFxVarG).pGT4.address;
    const ee_float angleScale = at<float>(0x32786cu);
    guest_call(0x1c7b68u, mat, mat, ee_itof(static_cast<int32_t>(at<uint16_t>(sprite + 0x26) & 0xfffu) - 0x800) * angleScale); // sceVu0RotMatrixZ
    const ee_float scale = at<float>(sprite);
    at<float>(quad) = -scale;
    at<float>(quad + 8) = 0.0f;
    at<float>(quad + 4) = ee_float(at<float>(sprite));
    at<float>(quad + 0x10) = ee_float(at<float>(sprite));
    at<float>(quad + 0x18) = 0.0f;
    at<float>(quad + 0x14) = ee_float(at<float>(sprite));
    guest_call(0x1c77d8u, mat, ptr32<ee_float>(0x1f1170u), mat); // sceVu0MulMatrix
    at<float>(quad + 0x20) = -ee_float(at<float>(sprite));
    at<float>(quad + 0x28) = 0.0f;
    at<float>(quad + 0x24) = -ee_float(at<float>(sprite));
    at<float>(quad + 0x30) = ee_float(at<float>(sprite));
    at<float>(quad + 0x38) = 0.0f;
    at<float>(quad + 0x34) = -ee_float(at<float>(sprite));
    spriteQuadFinish(mat, vPos, sprite, quad, at<uint8_t>(sprite + 0x25) != 0, false, zOffset);
}

// The same for a sprite lying on the ground, tilted to the surface normal (svNormal, 4.12 fixed point).
void VfxPS2SpriteGroundRotated3D(ptr32<SVECTOR> svNormal, ee_float zOffset)
{
    GuestFrame frame(0x110);
    const ptr32<ee_float> mat = frame.local<ee_float>(0x20);
    const ptr32<ee_float> vPos = frame.local<ee_float>(0x10);
    guest_call(0x1c7ac8u, mat); // sceVu0UnitMatrix
    const uint32_t sprite = G(visFxVarG).sprite.address;
    const uint32_t quad = G(visFxVarG).pGT4.address;
    const ee_float scale = at<float>(sprite);
    at<float>(quad + 4) = 0.0f;
    at<float>(quad) = -scale;
    at<float>(quad + 8) = ee_float(at<float>(sprite));
    at<float>(quad + 0x14) = 0.0f;
    at<float>(quad + 0x10) = ee_float(at<float>(sprite));
    at<float>(quad + 0x18) = ee_float(at<float>(sprite));
    at<float>(quad + 0x24) = 0.0f;
    at<float>(quad + 0x20) = -ee_float(at<float>(sprite));
    at<float>(quad + 0x28) = -ee_float(at<float>(sprite));
    at<float>(quad + 0x34) = 0.0f;
    at<float>(quad + 0x30) = ee_float(at<float>(sprite));
    const ee_float angleScale = at<float>(0x327870u);
    const ee_float angle = ee_itof(static_cast<int32_t>(at<uint16_t>(sprite + 0x26) & 0xfffu) - 0x800) * angleScale;
    at<float>(quad + 0x38) = -ee_float(at<float>(sprite));
    guest_call(0x1c7cb8u, mat, mat, angle); // sceVu0RotMatrixY
    ee_float a = guest_call<ee_float>(0x1c87d0u, ee_itof(svNormal->vz), ee_itof(svNormal->vy)); // atan2f
    guest_call(0x1c7c10u, mat, mat, a); // sceVu0RotMatrixX
    a = guest_call<ee_float>(0x1c87d0u, ee_itof(svNormal->vx), ee_itof(svNormal->vy));
    guest_call(0x1c7b68u, mat, mat, -a); // sceVu0RotMatrixZ
    spriteQuadFinish(mat, vPos, sprite, quad, false, true, zOffset);
}

} // namespace rc

namespace rc::fuzz { // visfx_fuzz.cpp
void vfxWheel(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxRendered(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxCarAndEffect(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxEffectOnCar(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxQuad(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxMilk(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxLightning(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxBlast(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxSparks(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxMover(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxSimpleMover(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxRibbon(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxRibbonShorts(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxFourEnds(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxSprite(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxGroundSprite(R5900Context *ctx, uint64_t &state, uint32_t scratch);
}

RC_DECOMP_FN(VfxSpriteRotated3D, 0x16a5c0)
RC_DECOMP_FN(VfxSpriteGroundRotated3D, 0x16a5c8)
RC_DECOMP_FN(VfxSkyBackdrop, 0x16a5d0)
RC_DECOMP_FN(VfxSkyActualSlowChange, 0x16a5d8)
RC_DECOMP_FN(VfxRenderer_CarAerialRibbon, 0x16a5e0)
RC_DECOMP_FN(VfxHandler_Shadow, 0x16a658)
RC_DECOMP_FN(VfxRenderer_Shadow, 0x16a660)
RC_DECOMP_FN(VfxRenderer_Milk, 0x16aa98)
RC_DECOMP_FN(VfxSpriteRotatedHud, 0x16ab80)
RC_DECOMP_FN_NOFUZZ(VfxTerm, 0x156db0)
RC_DECOMP_FN(VfxSetup, 0x16a0b0)
RC_DECOMP_FN_SETUP(VfxCarSpeedVal, 0x16a320, rc::fuzz::aiCar)
RC_DECOMP_FN_SETUP(VfxHasCarBeenRendered, 0x16a340, rc::fuzz::vfxRendered)
RC_DECOMP_FN_SETUP(VfxShortestLastDistRendered, 0x16aaa0, rc::fuzz::vfxRendered)
RC_DECOMP_FN_SETUP(VfxCarMatrixVal, 0x16a1a8, rc::fuzz::aiMatrixAndCar)
RC_DECOMP_FN_SETUP(VfxCarVelocityVal, 0x16a2a8, rc::fuzz::aiCar)
RC_DECOMP_FN_SETUP(VfxVUCarPositionPoi, 0x16a430, rc::fuzz::aiCar)
RC_DECOMP_FN_SETUP(VfxIsWheelSkidding, 0x16aad0, rc::fuzz::vfxWheel)
RC_DECOMP_FN_SETUP(VfxIsWheelTouching, 0x16aaf0, rc::fuzz::vfxWheel)
RC_DECOMP_FN_SETUP(VfxMaterialUnderWheel, 0x16ab58, rc::fuzz::vfxWheel)
RC_DECOMP_FN(VfxSVtoV3, 0x169d68)
RC_DECOMP_FN(VfxSortPos, 0x169fb8)
RC_DECOMP_FN_SETUP(VfxCarSortPos, 0x169f68, rc::fuzz::vfxCarAndEffect)
RC_DECOMP_FN(VfxSortFunc, 0x16a178)
RC_DECOMP_FN_SETUP(VfxPutGT4rgba, 0x169ea8, rc::fuzz::vfxQuad)
RC_DECOMP_FN_SETUP(VfxPutGT4rgba0, 0x169ec8, rc::fuzz::vfxQuad)
RC_DECOMP_FN_SETUP(VfxPutGT4rgba1, 0x169ed8, rc::fuzz::vfxQuad)
RC_DECOMP_FN_SETUP(VfxPutGT4rgba2, 0x169ee8, rc::fuzz::vfxQuad)
RC_DECOMP_FN_SETUP(VfxPutGT4rgba3, 0x169ef8, rc::fuzz::vfxQuad)
RC_DECOMP_FN_SETUP(VfxPutUV0, 0x169f08, rc::fuzz::vfxQuad)
RC_DECOMP_FN_SETUP(VfxPutUV1, 0x169f20, rc::fuzz::vfxQuad)
RC_DECOMP_FN_SETUP(VfxPutUV2, 0x169f38, rc::fuzz::vfxQuad)
RC_DECOMP_FN_SETUP(VfxPutUV3, 0x169f50, rc::fuzz::vfxQuad)
RC_DECOMP_FN_SETUP(VfxHandler_CarHeatRayBlast, 0x16a5e8, rc::fuzz::vfxEffectOnCar)
RC_DECOMP_FN_SETUP(VfxHandler_BlastLightning, 0x16a780, rc::fuzz::vfxEffectOnCar)
RC_DECOMP_FN_SETUP(VfxHandler_SargeLights, 0x16a938, rc::fuzz::vfxEffectOnCar)
RC_DECOMP_FN(VfxInit, 0x156bb8)
RC_DECOMP_FN(VfxPSXToVUMatrix, 0x156aa8)
RC_DECOMP_FN_NOFUZZ(VfxDelete, 0x16a0c8)           // Frees a heap block that is on the effect list.
RC_DECOMP_FN_NOFUZZ(VfxHandOverFXDelete, 0x157668) // As above, for every effect.
RC_DECOMP_FN_NOFUZZ(VfxCallHandlers, 0x16a470)     // Runs every live effect's handler.
RC_DECOMP_FN_SETUP(VfxCarPositionVal, 0x16a1f8, rc::fuzz::aiCar)
RC_DECOMP_FN_SETUP(VfxWheelPositionPoi, 0x156840, rc::fuzz::vfxWheel)
RC_DECOMP_FN_SETUP(VfxWheelPositionVal, 0x156940, rc::fuzz::vfxWheel)
RC_DECOMP_FN_SETUP(VfxHandler_Milk, 0x16aa10, rc::fuzz::vfxMilk)
RC_DECOMP_FN_SETUP(VfxHandler_AerialLightning, 0x16a668, rc::fuzz::vfxLightning)
RC_DECOMP_FN_SETUP(VfxHandler_DamageLightning, 0x16a7f8, rc::fuzz::vfxLightning)
RC_DECOMP_FN_SETUP(VfxHandler_BlastMoonrover, 0x168d60, rc::fuzz::vfxBlast)
RC_DECOMP_FN_SETUP(VfxCreate_Sparks, 0x1645c0, rc::fuzz::vfxSparks)
RC_DECOMP_FN_SETUP(VfxParticleMover, 0x158e00, rc::fuzz::vfxMover)
RC_DECOMP_FN_SETUP(VfxSimpleParticleMover, 0x158f18, rc::fuzz::vfxSimpleMover)
RC_DECOMP_FN_SETUP(VfxPutGT4vecs, 0x169da8, rc::fuzz::vfxFourEnds)
RC_DECOMP_FN_SETUP(VfxPutGT4FacingContinue, 0x167e88, rc::fuzz::vfxRibbonShorts)
RC_DECOMP_FN_SETUP(VfxPutGT4FacingContinueVU, 0x168118, rc::fuzz::vfxRibbon)
RC_DECOMP_FN_SETUP(VfxPutGT4FacingPoly, 0x167cd0, rc::fuzz::vfxRibbonShorts)
RC_DECOMP_FN_SETUP(VfxPutGT4FacingPolyVU, 0x167fa8, rc::fuzz::vfxRibbon)
RC_DECOMP_FN_SETUP(VfxPS2SpriteRotated3D, 0x167788, rc::fuzz::vfxSprite)
RC_DECOMP_FN_SETUP(VfxPS2SpriteGroundRotated3D, 0x167a20, rc::fuzz::vfxGroundSprite)
