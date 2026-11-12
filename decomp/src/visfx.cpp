// Decompiled from VISFX.c: the visual effects (smoke, skid marks, lightning, shadows...). This file holds the small
// functions: the accessors that read a car's physics for the effects, the setters the effects use to fill the
// renderer's current quad (GT4: a textured, Gouraud-shaded four-sided polygon), the sort-position helpers, and the
// handlers that only follow their car. The effects' own update and draw code is still recompiled.
//
// An effect is a `_sVfx_Object`: a 0x40-byte header (list links, handler and renderer, life in frames, the position
// it is sorted by) followed by data of its own kind. The handlers here reach that data by offset.

#include "ai_fuzz.h"
#include "ee_math.h"

#include <emmintrin.h>

namespace rc {

void ResTypeUnSetConstructDaemon(ptr32<char> pType);

namespace {

template <typename T>
T &at(uint32_t address)
{
    return *static_cast<T *>(guest_host(address));
}

// The effect's data fields, by offset from the start of the object.
constexpr uint32_t kLifeSpan = 0x10;     // iLifeSpan, frames left
constexpr uint32_t kSortPos = 0x20;      // vuSortPos: the position the effect is depth-sorted by
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

// sceVu0ITOF0Vector as VISFX calls it: converts the three integers the caller put at the start of a 16-byte local to
// floats and stores all four lanes. The fourth integer is a word of the caller's stack frame the caller never wrote,
// so it holds whatever an earlier call left there (frameSize is the original's frame size; the local is at sp+0).
void sortPosFromInts(uint32_t fx, int32_t x, int32_t y, int32_t z, uint32_t frameSize)
{
    const int32_t w = stale_stack<int32_t>(frameSize, 0xc);
    const __m128i ints = _mm_set_epi32(w, z, y, x);
    _mm_storeu_ps(&at<float>((fx + kSortPos) & ~0xFu), _mm_cvtepi32_ps(ints));
}

// A car's position in whole units: the physics position divided by 256.
void sortPosFromCar(uint32_t fx, ptr32<_sObject> pObj, uint32_t frameSize)
{
    const sVec32 &pos = integerState(pObj).renderPos;
    sortPosFromInts(fx, pos.x >> 8, pos.y >> 8, pos.z >> 8, frameSize);
}

// Wheel records are 0x48 bytes. The index is multiplied in 32 bits, as the original does.
const _sFPhysLibWheel &wheelOf(ptr32<_sObject> pObj, int32 iWheel)
{
    return at<_sFPhysLibWheel>(pObj->pPhysics.address + offsetof(_sFPhysLibCar, wheel) + static_cast<uint32_t>(iWheel) * sizeof(_sFPhysLibWheel));
}

} // namespace

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

} // namespace rc

namespace rc::fuzz { // visfx_fuzz.cpp
void vfxWheel(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxRendered(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxCarAndEffect(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxEffectOnCar(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxQuad(R5900Context *ctx, uint64_t &state, uint32_t scratch);
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
