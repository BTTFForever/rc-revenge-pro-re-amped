// Shared by object.cpp and object2.cpp (OBJECT.c): the unit's functions, the native functions of other units they call,
// guest addresses and small helpers.
#pragma once

#include "ee_math.h"

namespace rc {

// Other units, native.
bool_ MemBlockAllocLo(ptr32<memHandle> pHandle, uint32 size);
void MemBlockFree(ptr32<memHandle> pHandle);
void ResTypeSetConstructDaemon(ptr32<char> pType, uint32 constructDaemon, int16 options);
void ResTypeUnSetConstructDaemon(ptr32<char> pType);
ptr32<_sNode> NodeFindByTag(ptr32<_sNodeHead> pNodeHead, uint32 tag);
ptr32<_sNode> NodeFindNextByTag(ptr32<_sNode> pCurrNode, ptr32<_sNodeHead> pNodeHead, uint32 tag);
void MatPsxInit(ptr32<_matPsx> in);
void CarColInitObj(ptr32<_sObject> pObject);
void OColInitObj(ptr32<_sOColObj> pObj, ptr32<_sColCellVecPSX> pPos, ptr32<ee_float> pRad, ptr32<void> pData);
void OColDeleteObject(ptr32<_sOColObj> pObj);
void RdrZedsWriteOff(int32_t channel);
ptr32<_sVfx_Object> VfxAddNew(ptr32<_sObject> pPlayerObj, int32 iY, int32 iZ, int32 iType);
void VfxDelete(ptr32<_sVfx_Object> pFX);
void VfxCreate_Sparks(ptr32<_sVfx_Object> mem, ptr32<vec32> vPos);
void WeaponRemoveAll();
uint16 RandomGetNDRange(uint16 lower, uint16 upper);
uint32 Vec16Magnitude(ptr32<vec16_> p);
int32 AINodeGetNearestHeading(ptr32<SVECTOR> svPos);
angle ArcTan(int32 x, int32 y);
ptr32<_sVfx_Object> VfxAddNewPlace(int32 iX, int32 iY, int32 iZ, int32 iType);
ptr32<_sWeapon> WeaponCreate(ptr32<vec16_> pPos, tTime time, ptr32<_sWeaponDef> pDef);

// OBJECT.c.
void ObjectTerm();
void ObjectConstructDaemon(ptr32<memHandle> pHandle);
ptr32<_sObject> ObjectCreate(ptr32<_sObjectDef> pDef, tTime time, int32 type);
void ObjectInitLights(ptr32<_sObject> pObject, ptr32<_sObjectDef> pDef);
void ObjectRenderLights(ptr32<_sObject> pObject);
void ObjectInitObject(ptr32<_sObject> pObject, ptr32<_sObjectDef> pDef, tTime time);
void ObjectInitRendObj(ptr32<_sObject> pObject, ptr32<_sObjectDef> pDef, tTime time);
void ObjectInitTask(ptr32<_sObject> pObject, ptr32<_sObjectDef> pDef, tTime time);
void ObjectInitSounds(ptr32<_sObject> pObject, ptr32<_sObjectDef> pDef, tTime time);
void ObjectClearSfx(ptr32<_sObject> pObject);
void ObjectDeleteAll();
void ObjectRender(ptr32<_sObject> pObject);
void ObjectUpdateEngine(ptr32<_sObject> pObject, int32 flags);
void ObjectUpdateSkid(ptr32<_sObject> pObject, int32 flags, tTime time);
void ObjectUpdateScrape(ptr32<_sObject> pObject, int32 flags);
void ObjectCreateCars(tTime time);
void ObjectEngineSFXParms(ptr32<_sObject> pObject, int32 flags);
void ObjectCarChangeOver(ptr32<_sObject> pObj, int32 iToWhat);
void ObjectWeaponAffect(ptr32<_sObject> pObject, tTime time);
void ObjectWeaponAffectRemove(ptr32<_sObject> pObject);
void ObjectAllocateEngineSfx(ptr32<_sObject> pObject);
void ObjectUpdateOvertaking(ptr32<_sObject> pObject, int32 flags, tTime time);
void ObjectAllRemoveWeapons();
void ObjectGiveNextWeapon(ptr32<_sObject> pObject);
void ObjectDumptugSled(ptr32<_sObject> pObject);
int32 ObjectMaterialSfx(int32 type, int32 material);
void ObjectSfxFrontEnd(int32 which);
void ObjectInit();
ptr32<_sObject> ObjectCreateBoat(ptr32<_sObjectDef> pDef, tTime time, int32 type);
void ObjectDelete(ptr32<_sObject> pObject);
void ObjectRenderAll();
void ObjectResetAllTint();
void ObjectSfxDoAll(int32 flags, tTime time);
void ObjectTestWaterEscape(ptr32<_sObject> pObject);
void ObjectDefaultControl(ptr32<void> pData, tTime time);
uint32 ObjectCalcSize(ptr32<_sObjectDef> pDef);
uint32 ObjectSetPointers(ptr32<_sObject> pObject, ptr32<_sObjectDef> pDef);
void ObjectInitWheels(ptr32<_sObject> pObject, ptr32<_sObjectDef> pDef, tTime time);
void ObjectInitTBoxMask(ptr32<_sObject> pObject, ptr32<_sObjectDef> pDef);
void ObjectInterpolateTint(ptr32<_sObject> pObject);

namespace object {

// Guest functions still recompiled.
constexpr uint32_t kResLoad = 0x1ab1b0, kTaskSet = 0x1afad8, kTaskCachedInsert = 0x1afb68, kTaskDelete = 0x1afc48;
constexpr uint32_t kPhysicsAddCar = 0x13d1e0, kPhysicsDeleteCar = 0x13e3e8, kPhysicsCarBoatSwap = 0x13e470;
constexpr uint32_t kSfxStop = 0x1ad448, kSfxPlay = 0x1ad0a0, kRevSfxPlay3D = 0x1418a8, kRevSfxPlay3DCombine = 0x141bd8;
constexpr uint32_t kRCMusicSwitchAlternate = 0x1411e8, kSfxStopNow = 0x1adff0;
constexpr uint32_t kTimerGet = 0x1b4188, kPhysicsApplyIntForce = 0x13e520;
constexpr uint32_t kObjectConstructDaemonAddr = 0x133ab0, kObjectDefaultControlAddr = 0x139a28;

// Globals.
constexpr uint32_t kObjectVars = 0x1ee390u, kObjectType = 0x327f18u, kObjectType2 = 0x327f20u, kCarList = 0x1eb7a0u;

template <typename T>
T &at(uint32_t address) { return *static_cast<T *>(guest_host(address)); }

inline ptr32<_sObject> objectAt(uint32_t n) { return at<ptr32<_sObject>>(kObjectVars + n * 4u); }
inline uint32_t noObjects() { return G(objectVarG).noObjects; }

inline void sfxStop(int32_t handle) { guest_call(kSfxStop, static_cast<int32_t>(static_cast<int16_t>(handle))); }

// RevSfxPlay3D(sound, volume, pitch, &pos, flags, range); its result as the sign-extended 16 bits callers keep.
inline int32_t sfx3D(int32_t sound, int32_t volume, int32_t pitch, uint32_t pos, int32_t flags, int32_t range)
{
    return static_cast<int16_t>(guest_call<int32_t>(kRevSfxPlay3D, static_cast<int32_t>(static_cast<int16_t>(sound)),
                                                    static_cast<int32_t>(static_cast<int16_t>(volume)),
                                                    static_cast<int32_t>(static_cast<int16_t>(pitch)), ptr32<uint8>(pos),
                                                    static_cast<int32_t>(static_cast<int16_t>(flags)),
                                                    static_cast<int32_t>(static_cast<int16_t>(range))));
}
// ObjectEngineSFXParms keeps the material it last heard in a boat's sounds from $s2 without setting it there, so the
// value is its caller's $s2 (ObjectUpdateEngine passes its own caller's on). A native caller puts there what the
// original would have had, for the length of the calls, and gives the caller's back after.
class HeldS2
{
public:
    explicit HeldS2(uint32_t value) : m_ctx(g_runtime->eeScheduler().runningContext()), m_saved(m_ctx.r[18])
    {
        SET_GPR_S32((&m_ctx), 18, static_cast<int32_t>(value));
    }
    ~HeldS2() { m_ctx.r[18] = m_saved; }
    HeldS2(const HeldS2 &) = delete;
    HeldS2 &operator=(const HeldS2 &) = delete;

private:
    R5900Context &m_ctx;
    std::remove_reference_t<decltype(std::declval<R5900Context &>().r[0])> m_saved;
};
inline uint32_t callerS2() { return getRegU32(&g_runtime->eeScheduler().runningContext(), 18); }

inline uint32_t rendPos(ptr32<_sObject> o) { return o->pRendObj.address + 4u; } // &pRendObj->pos

} // namespace object

} // namespace rc
