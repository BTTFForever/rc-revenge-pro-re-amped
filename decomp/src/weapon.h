// Shared by weapon.cpp and weapon2.cpp (WEAPON.c): the unit's functions, the native functions of other units they call,
// guest addresses and small helpers.
#pragma once

#include "ee_math.h"
#include "../rc_cars.h"

namespace rc {

// Other units, native.
int32 rand();                                                                                           // ai.cpp
ptr32<_sVfx_Object> VfxAddNew(ptr32<_sObject> pPlayerObj, int32 iY, int32 iZ, int32 iType);
ptr32<_sVfx_Object> VfxAddNewPlace(int32 iX, int32 iY, int32 iZ, int32 iType);
void VfxDelete(ptr32<_sVfx_Object> pFX);
uint64_t VfxCarAerialVal(ptr32<_sObject> pObj, int32 n);
void OColDeleteObject(ptr32<_sOColObj> pObj);
void OColInitObj(ptr32<_sOColObj> pObj, ptr32<_sColCellVecPSX> pPos, ptr32<ee_float> pRad, ptr32<void> pData);
bool_ AColIsPositionOutsideWorld(ptr32<_sColCellVecPSX> pPos, ee_float tol);
bool_ AColIsPositionOutsideWorldExtentsXZ(ptr32<_sColCellVecPSX> pPos);
bool_ AColDownCast(ptr32<_sColCellVecPSX> pPos, ptr32<_sColCellVecPSX> pPoi, ptr32<_sColNormalPSX> pNormal, ptr32<uint8> pMatId);
int32 AColCellIdFromPosUnbounded(ptr32<_sColGridPSX> pGrid, ee_float x, ee_float z);
angle ArcTan(int32 x, int32 y);
void MatPsxInit(ptr32<_matPsx> in);
void Vec32MulMatPsx(ptr32<vec32_> p, ptr32<_matPsx> mp, ptr32<vec32_> r);
uint16 RandomGetNDRange(uint16 lower, uint16 upper);
void PlayerSetVibrate(uint8 vibLeft, uint8 vibRight, uint8 playerNum);
void RdrState_ForceApplyStateBlock(ptr32<sRdrStateBlock> block);
void RdrState_SetAlphaBlending(ptr32<sRdrStateBlock> block, u32 A, u32 B, u32 C, u32 D, u8 fixedAlpha);
void RdrState_SetPixelAlphaBlendControlEnable(ptr32<sRdrStateBlock> block, u32 enable);
void RdrState_SetStateBlockToDefaults(ptr32<sRdrStateBlock> block);
void RdrState_SetZBufferWriteEnable(ptr32<sRdrStateBlock> block, u32 enable);
int16 RendObjRender(ptr32<_sRendObj> pRendObj, ptr32<SVECTOR> pPOI__unused, ptr32<SVECTOR> pNormal__unused, int32_t doTint);
uint16 Sqr32(uint32 n);
uint32 Vec16Magnitude(ptr32<vec16_> p);
void NormalDotProduct(ptr32<normal_> a, ptr32<normal_> b, ptr32<frac> cosine);

// WEAPON.c.
void WeaponProcess(ptr32<void> pData, tTime time);
void WeaponSpawn(ptr32<vec16_> pPos, ptr32<_sWeaponDef> pDef, tTime time);
void WeaponRemove(ptr32<_sWeapon> pWeapon);
ptr32<_sWeapon> WeaponCreate(ptr32<vec16_> pPos, tTime time, ptr32<_sWeaponDef> pDef);
void WeaponDestroy(ptr32<_sWeapon> pWeapon);
void WeaponTerm();
void WeaponRocketUpdate(ptr32<_sWeapon> pWeapon);
void WeaponRocketTestLockon(ptr32<_sObject> pObject);
void WeaponRenderAll(int32 split);
void WeaponHitCar(ptr32<_sWeapon> pWeapon, ptr32<_sObject> pObject);
void WeaponOilCollide(ptr32<_sWeapon> pWeapon, ptr32<_sObject> pObject);
void WeaponRocketSetup(ptr32<_sWeapon> pWeapon);
void WeaponFakeSetup(ptr32<_sWeapon> pWeapon, tTime time);
void WeaponShockwaveSetup(ptr32<_sWeapon> pWeapon, tTime time);
void WeaponShockwaveUpdate(ptr32<_sWeapon> pWeapon, tTime time);
void WeaponWaterSetup(ptr32<_sWeapon> pWeapon, tTime time);
void WeaponWaterUpdate(ptr32<_sWeapon> pWeapon, tTime time);
void WeaponLightningUpdate(ptr32<_sWeapon> pWeapon, tTime time);
void WeaponBatteryUpdate(ptr32<_sWeapon> pWeapon, tTime time);
void WeaponMegaSetup(ptr32<_sWeapon> pWeapon, tTime time);
void WeaponHeatraySetup(ptr32<_sWeapon> pWeapon, tTime time);
void WeaponHeatrayUpdate(ptr32<_sWeapon> pWeapon, tTime time);
void WeaponInvincibleSetup(ptr32<_sWeapon> pWeapon, tTime time);
void WeaponInvincibleUpdate(ptr32<_sWeapon> pWeapon, tTime time);
void WeaponBouncySetup(ptr32<_sWeapon> pWeapon, tTime time);
void WeaponBouncyUpdate(ptr32<_sWeapon> pWeapon, tTime time);
void WeaponSargeSetup(ptr32<_sWeapon> pWeapon, tTime time);
void WeaponSargeUpdate(ptr32<_sWeapon> pWeapon, tTime time);
void WeaponSkullUpdate(ptr32<_sWeapon> pWeapon, tTime time);
void WeaponArmouredSetup(ptr32<_sWeapon> pWeapon, tTime time);
void WeaponArmouredUpdate(ptr32<_sWeapon> pWeapon, tTime time);
void WeaponDumptugSetup(ptr32<_sWeapon> pWeapon, tTime time);
void WeaponDumptugUpdate(ptr32<_sWeapon> pWeapon, tTime time);
void WeaponDumptugWaterSetup(ptr32<_sWeapon> pWeapon, tTime time);
void WeaponMoonbuggySetup(ptr32<_sWeapon> pWeapon, tTime time);
int32 WeaponTestAcol(ptr32<_sWeapon> pWeapon, ptr32<_sVec16> pNormal);
void WeaponFakeUpdate(ptr32<_sWeapon> pWeapon, tTime time);
void WeaponRemoveAll();
void WeaponInit();
int16 WeaponTarget(ptr32<_sObject> pMe, ptr32<_sObject> pYou);
int16 WeaponRelativeAngle(ptr32<_sObject> pObject, ptr32<_sObject> pTarget);
void WeaponRocketLocked(ptr32<_sObject> pObject);
ptr32<_sObject> WeaponFindValidTarget(ptr32<_sObject> pObject);
void WeaponRocketParanoiaCheck();
void WeaponOilSetup(ptr32<_sWeapon> pWeapon);
void WeaponShockwaveShake(ptr32<_sWeapon> pWeapon, tTime time);
void WeaponLightningSetup(ptr32<_sWeapon> pWeapon, tTime time);
void WeaponLightningFreeBlast(ptr32<_sWeapon> pWeapon);
void WeaponBatterySetup(ptr32<_sWeapon> pWeapon, tTime time);
void WeaponMegaFreeBlast(ptr32<_sWeapon> pWeapon);
void WeaponSkullSetup(ptr32<_sWeapon> pWeapon, tTime time);
void WeaponDumptugWaterUpdate(ptr32<_sWeapon> pWeapon, tTime time);
void WeaponMoonbuggyUpdate(ptr32<_sWeapon> pWeapon, tTime time);
int16 WeaponGetOwnerBitfield(int32 owner);
void WeaponSkullFreeHexed(ptr32<_sWeapon> pWeapon);

namespace weapon {

// Guest functions still recompiled.
constexpr uint32_t kMemFragCreate = sym::MemFragCreate, kMemFragDelete = sym::MemFragDelete, kMemFragPoolCreate = sym::MemFragPoolCreate, kMemFragPoolDelete = sym::MemFragPoolDelete;
constexpr uint32_t kTaskSet = sym::TaskSet, kTaskCachedInsert = sym::TaskCachedInsert, kTaskDelete = sym::TaskDelete;
constexpr uint32_t kRevSfxPlay3D = sym::RevSfxPlay3D, kRevSfxSpecialTrigger = sym::RevSfxSpecialTrigger, kSfxPlay = sym::SfxPlay, kSfxStop = sym::SfxStop;
constexpr uint32_t kTimerGet = sym::TimerGet, kObjectWeaponAffectRemove = sym::ObjectWeaponAffectRemove, kPhysicsApplyIntForce = sym::PhysicsApplyIntForce;
constexpr uint32_t kMat3nBuildXYZ = sym::Mat3nBuildXYZ, kMat3nDecompose = sym::Mat3nDecompose, kNormalMulMatPsx = sym::NormalMulMatPsx, kMemset = sym::memset;
constexpr uint32_t kVec16Normalise = sym::_Vec16Normalise, kVec32Length = sym::_Vec32Length, kVec32Normalise = sym::_Vec32Normalise, kAnimFindByTSOTag = sym::AnimFindByTSOTag;
constexpr uint32_t kWeaponProcessAddr = sym::WeaponProcess;

// Globals.
constexpr uint32_t kObjectList = sym::objectVarG, kWeaponVars = sym::weaponVarG, kCameras = sym::cameraVarG, kGameTimer = (sym::mainVarG + 0x10u);
constexpr uint32_t kRaceOrder = cars::kRaceOrder /* enhancement: aiVarG.raceOrder moved for more than 8 cars (rc_cars.h) */, kRaceOrderStride = 12u;
constexpr uint32_t kTypeFirstBoat = 0x14u; // OBJECT_TYPE_BADDRC_BOAT: types below are cars

template <typename T>
T &at(uint32_t address) { return *static_cast<T *>(guest_host(address)); }

// objectVarG.list[n] for any n, as the original indexes it.
inline ptr32<_sObject> objectAt(int32_t n) { return at<ptr32<_sObject>>(kObjectList + static_cast<uint32_t>(n) * 4u); }
inline uint32_t objectType(ptr32<_sObject> p) { return at<uint32_t>(at<uint32_t>(p.address + 0xcu) + 4u); }

inline void sfxStop(int32_t handle) { guest_call(kSfxStop, static_cast<int32_t>(static_cast<int16_t>(handle))); }

// RevSfxPlay3D(sound, volume, pitch, &pos, flags, distance), its handle as the 16 bits the weapons keep.
inline int16 sfx3D(int32_t sound, int32_t volume, int32_t pitch, uint32_t pos, int32_t flags, int32_t distance)
{
    return static_cast<int16>(guest_call<int32_t>(kRevSfxPlay3D, sound, volume, pitch, ptr32<uint8>(pos), flags, distance));
}
inline uint32_t carPos(ptr32<_sObject> car) { return car->pRendObj.address + 4u; } // &pRendObj->pos
inline int32_t carSfxFlags(ptr32<_sObject> car) { return (car->playerType + 4) | 0x40; }

inline uint32_t timerGet() { return guest_call<uint32_t>(kTimerGet, ptr32<uint8>(kGameTimer)); }

// The battery's oily skid marks end: bit 1 of both skid effects' u8Alter.
inline void clearSkidOil(int32_t owner)
{
    _sObject &car = *objectAt(owner);
    at<uint8_t>(car.pVfx_SkidLeft.address + 0x18bu) &= 0xfdu;
    at<uint8_t>(objectAt(owner)->pVfx_SkidRight.address + 0x18bu) &= 0xfdu;
}

// weapon2.cpp: the shock wave's ring reaching a car, and its push.
bool inRing(const GuestFrame &frame, uint32_t fx, const _sObject &car, int32_t radius);
void pushAway(const GuestFrame &frame, _sObject &car, int16_t up, ee_float fromX, ee_float fromZ);

} // namespace weapon
} // namespace rc
