// Decompiled from GAMETASK.c: the race's physics task. Each step (GameTaskProcess, every gameTaskTimeStep) holds the
// cars at the start and finish, runs the physics, moves the cars' collision positions, grows the metamorphosing
// cars' middle spheres, resolves collisions and runs the animations and the camera. After the collision response
// GameTaskGetShadowMaterialInfo finds each wheel's shadow height (once a car has gone unrendered for three frames,
// the fast method skips it).
//
// Written from the instructions.

#include "physics_fuzz.h"
#include "weapon.h"

namespace rc {

bool_ AColDownCast(ptr32<_sColCellVecPSX> pPos, ptr32<_sColCellVecPSX> pPoi, ptr32<_sColNormalPSX> pNormal,
                   ptr32<uint8> pMatId);                                   // collision.cpp
bool_ AColIsPositionOutsideWorld(ptr32<_sColCellVecPSX> pPos, ee_float tol); // collision.cpp
ptr32<_sVfx_Object> VfxAddNew(ptr32<_sObject> pPlayerObj, int32 iY, int32 iZ, int32 iType); // visfx12.cpp
uint16 VfxShortestLastDistRendered(ptr32<_sObject> pObj);                  // visfx.cpp
void AIRespawnFurther(ptr32<_sObject> pObj, int32 times);                  // ai.cpp
void OColProcess(tTime);                                                   // ocol.cpp
void CarColColDetCars();                                                   // collision.cpp
void CameraProcess(int32 time, int32 split);                               // camera.cpp
int32_t PhysicsMotion();                                                   // physics2.cpp
int32_t PhysicsCollisionResponseUpdate();

void GameTaskProcess(ptr32<void> pData, tTime time);
void GameTaskSpecialPhysicsCases();
void GameTaskUpdateCarOcols();
void GameTaskShadowCalc(ptr32<_sObject> pObject);

namespace {

using weapon::at;

constexpr uint32_t kTask = 0x1ecd48u, kTimeStep = 0x327de0u, kGameTaskProcess = 0x123c10u, kAnimUpdateAnims = 0x10fd10u;
constexpr uint32_t kObjectList = 0x1ee390u, kNoObjects = kObjectList + 0x170u, kMainVars = 0x1ed900u;
constexpr uint32_t kReadySteadyGo = 0x1ece10u, kResultTrigger = 0x1f3730u, kDoApplyAIBoost = 0x328490u;
constexpr uint32_t kCameraMode = 0x1e9fb0u;
constexpr uint32_t kMorphScaleProcess = 0x32762cu, kShadowDefault = 0x327630u, kMorphScale = 0x327634u;
const ee_float kToCell = ee_float(0.00390625f); // 1 / 256

ee_float fat(uint32_t address) { return at<ee_float>(address); }
uint32_t objectAt(int32_t i) { return at<uint32_t>(kObjectList + 4u * static_cast<uint32_t>(i)); }

// A metamorphosing car's middle sphere grows from its radius when it started to the collision object's over 400 ms.
void metamorphose(uint32_t obj, int32_t time, uint32_t scaleAddress)
{
    if (at<uint8_t>(obj + 0x176u) == 0)
    {
        at<int32_t>(obj + 0x1d0u) = 0;
        return;
    }
    int32_t started = at<int32_t>(obj + 0x1d0u);
    if (started < 0)
        return;
    const uint32_t phys = at<uint32_t>(obj + 0x20u);
    if (started == 0)
    {
        at<int32_t>(obj + 0x1d0u) = time;
        at<int32_t>(obj + 0x1d4u) = ee_ftoi(fat(at<uint32_t>(phys + 0x1e0u) + 0x698u));
        started = time;
    }
    const int32_t t = time - started;
    if (t >= 400)
    {
        at<int32_t>(obj + 0x1d0u) = -1;
        return;
    }
    const ee_float grown = ee_itof(t) * ee_float(256.0f);
    const ee_float target = fat(at<uint32_t>(at<uint32_t>(at<uint32_t>(obj + 0x24u) + 4u) + 0x28u)); // *pCarCol->pOcol->pRad
    const ee_float start = ee_itof(static_cast<int32_t>(static_cast<uint32_t>(at<int32_t>(obj + 0x1d4u)) * static_cast<uint32_t>(400 - t)));
    at<ee_float>(at<uint32_t>(phys + 0x1e0u) + 0x698u) = (start + target * grown) * fat(scaleAddress);
}

} // namespace

void GameTaskLvlInit()
{
    guest_call(weapon::kTaskSet, kTask, kGameTaskProcess, 0, 0);
    guest_call(weapon::kTaskCachedInsert, kTask); // a tail call
}

void GameTaskLvlTerm()
{
    guest_call(weapon::kTaskDelete, kTask); // a tail call
}

void GameTaskInit() {}

void GameTaskTerm() {}

// Each car's collision position from its render position, and a car off the world or being respawned put back.
void GameTaskUpdateCarOcols()
{
    for (int32_t i = at<int32_t>(kNoObjects) - 1; i >= 0; --i)
    {
        const uint32_t obj = objectAt(i);
        const uint32_t phys = at<uint32_t>(obj + 0x20u);
        at<ee_float>(obj + 0xbcu) = fat(phys + 8u) * kToCell;
        at<ee_float>(obj + 0xc0u) = fat(phys + 0xcu) * kToCell;
        at<ee_float>(obj + 0xc4u) = fat(phys + 0x10u) * kToCell;
        if (AColIsPositionOutsideWorld(ptr32<_sColCellVecPSX>(obj + 0xbcu), ee_float(100.0f)) && at<int8_t>(obj + 0x18au) == 0)
            VfxAddNew(ptr32<_sObject>(obj), 0, 0, 0x24);
        else if (at<int8_t>(obj + 0x18au) == 2)
        {
            const uint32_t p = at<uint32_t>(obj + 0x20u);
            at<uint8_t>(p + 0x1cdu) = at<uint8_t>(p + 0x1cdu) & 0xfdu;
            at<int32_t>(obj + 0x6cu) = 1; // oCol.enabled
            at<int8_t>(obj + 0x18au) = 1;
            AIRespawnFurther(ptr32<_sObject>(obj), 2);
        }
        at<uint32_t>(obj + 0x64u) = ((at<uint8_t>(phys + 0x1cdu) >> 1) ^ 1u) & 1u; // oCol.updated
    }
}

// Holds the cars still before the start (the players only until "go") and after the result; at "go" the computer's
// cars get a boost, shorter the further back they start. Then the hold flags are cleared again.
void GameTaskSpecialPhysicsCases()
{
    const int32_t rsg = at<int32_t>(kReadySteadyGo);
    if (rsg != 0 || at<int32_t>(kResultTrigger) != 0)
    {
        for (int32_t i = at<int32_t>(kNoObjects) - 1; i >= 0; --i)
        {
            const uint32_t phys = at<uint32_t>(objectAt(i) + 0x20u);
            if (!(i < static_cast<int32_t>(at<uint8_t>(kMainVars + 0x369u)) && at<int32_t>(kReadySteadyGo) < 3))
            {
                at<int16_t>(phys + 0u) = 0;
                at<int16_t>(phys + 2u) = 0;
                at<int16_t>(phys + 4u) = 0;
            }
            at<uint8_t>(phys + 0x1cdu) = at<uint8_t>(phys + 0x1cdu) | 0x1cu;
        }
        if (at<int32_t>(kReadySteadyGo) == 1)
            at<int32_t>(kDoApplyAIBoost) = 1;
        return;
    }
    const uint32_t count = at<uint32_t>(kNoObjects);
    if (at<int32_t>(kDoApplyAIBoost) != 0)
    {
        at<int32_t>(kDoApplyAIBoost) = 0;
        for (uint32_t i = at<uint8_t>(kMainVars + 0x369u); i < count; ++i)
        {
            const int32_t back = static_cast<int32_t>(i - at<uint8_t>(kMainVars + 0x369u));
            const int32_t boost = ee_div(back * 600 + 600, at<uint8_t>(kMainVars + 0x36au)); // noAI
            at<int16_t>(at<uint32_t>(objectAt(static_cast<int32_t>(i)) + 0x20u) + 0x1d2u) = static_cast<int16_t>(boost);
        }
    }
    for (int32_t i = static_cast<int32_t>(at<uint32_t>(kNoObjects)) - 1; i >= 0; --i)
    {
        const uint32_t phys = at<uint32_t>(objectAt(i) + 0x20u);
        at<uint8_t>(phys + 0x1cdu) = at<uint8_t>(phys + 0x1cdu) & 0xe3u;
    }
}

void GameTaskProcess(ptr32<void>, tTime time)
{
    GameTaskSpecialPhysicsCases();
    PhysicsMotion();
    GameTaskUpdateCarOcols();
    for (int32_t i = at<int32_t>(kNoObjects) - 1; i >= 0; --i)
        metamorphose(objectAt(i), time, kMorphScaleProcess);
    OColProcess(time);
    CarColColDetCars();
    PhysicsCollisionResponseUpdate();
    guest_call(kAnimUpdateAnims);
    if (at<int32_t>(kCameraMode) != 1) // CAMERA_MODE_FRONT_END
        CameraProcess(time, at<uint8_t>(kMainVars + 0x368u));
    at<int32_t>(kTask + 4u) = time + at<int32_t>(kTimeStep);
    guest_call(weapon::kTaskCachedInsert, kTask); // a tail call
}

void MetaMorphiseCarSpheres(int32_t time)
{
    for (int32_t i = at<int32_t>(kNoObjects) - 1; i >= 0; --i)
        metamorphose(objectAt(i), time, kMorphScale);
}

// The ground under the car (its normal kept, scaled to 4096) and each wheel's shadow height: the highest ground contact
// of its sphere, else (driving on material 0x46, none) a cast down from the sphere, else the constant at 0x327630;
// within 128 of the ground under the middle, or that.
void GameTaskShadowCalc(ptr32<_sObject> pObject)
{
    GuestFrame frame(0x110);
    const uint32_t sp = frame.local<uint8_t>(0).address;
    const uint32_t obj = pObject.address;
    const uint32_t phys = at<uint32_t>(obj + 0x20u);
    uint32_t wheel = phys + 0xacu;
    uint32_t sphere = at<uint32_t>(phys + 0x1e0u);
    at<ee_float>(sp + 0u) = fat(phys + 0x14u) * kToCell;
    at<ee_float>(sp + 4u) = fat(phys + 0x18u) * kToCell + ee_float(80.0f);
    at<ee_float>(sp + 8u) = fat(phys + 0x1cu) * kToCell;
    const bool_ hit = AColDownCast(ptr32<_sColCellVecPSX>(sp), ptr32<_sColCellVecPSX>(obj + 0x1ecu),
                                   ptr32<_sColNormalPSX>(obj + 0x1dcu), ptr32<uint8>(sp + 0x10u));
    at<uint32_t>(sp + 0x58u) = sp + 0x20u;
    at<uint32_t>(sp + 0x54u) = sp + 0x30u;
    at<int32_t>(obj + 0x1d8u) = hit;
    for (uint32_t k = 0; k < 3; ++k)
        at<ee_float>(obj + 0x1dcu + 4u * k) = fat(obj + 0x1dcu + 4u * k) * ee_float(4096.0f);
    for (int32_t w = 4; w > 0; --w, wheel += 0x48u, sphere += 0x190u)
    {
        at<uint8_t>(sp + 0x50u) = 0x46;
        ee_float height = fat(kShadowDefault);
        const int32_t first = static_cast<int32_t>(at<int8_t>(sphere + 0x64u)) + at<int8_t>(sphere + 0x65u);
        for (int32_t k = at<int8_t>(sphere + 0x67u) - 1; k >= first; --k)
        {
            if (at<int8_t>(sphere + 0x94u + static_cast<uint32_t>(k)) == 2)
                break;
            const uint32_t normal = sphere + 0xa0u + 16u * static_cast<uint32_t>(k);
            if (ee_float(10.0f) < fat(normal + 4u))
            {
                const ee_float y = fat(sphere + 4u);
                at<int16_t>(wheel + 0x3eu) = static_cast<int16_t>(at<uint8_t>(at<uint32_t>(sphere + 0x160u + 4u * static_cast<uint32_t>(k)) + 0x53u));
                height = ((y - fat(sphere + 0x58u)) + fat(normal + 0xcu)) * kToCell;
                break;
            }
        }
        if (at<int16_t>(wheel + 0x3eu) == 0x46)
        {
            at<ee_float>(sp + 0x20u) = fat(sphere + 0u) * kToCell;
            at<ee_float>(sp + 0x24u) = fat(sphere + 4u) * kToCell + ee_float(80.0f);
            at<ee_float>(sp + 0x28u) = fat(sphere + 8u) * kToCell;
            if (AColDownCast(ptr32<_sColCellVecPSX>(sp + 0x20u), ptr32<_sColCellVecPSX>(sp + 0x30u),
                             ptr32<_sColNormalPSX>(sp + 0x40u), ptr32<uint8>(sp + 0x50u)))
                height = fat(sp + 0x34u);
        }
        at<ee_float>(wheel + 0x40u) = height;
        if (at<int32_t>(obj + 0x1d8u) != 0)
        {
            const ee_float ground = fat(obj + 0x1f0u);
            if (ee_float(128.0f) < ee_abs(ground - height))
                at<ee_float>(wheel + 0x40u) = ground;
        }
    }
}

void GameTaskGetShadowMaterialInfo(bool_ fastMethod)
{
    for (int32_t i = at<int32_t>(kNoObjects) - 1; i >= 0; --i)
    {
        const uint32_t obj = objectAt(i);
        if (VfxShortestLastDistRendered(ptr32<_sObject>(obj)) < 0x2000)
            at<int8_t>(obj + 0x21cu) = 0;
        else
        {
            const int8_t frames = static_cast<int8_t>(at<int8_t>(obj + 0x21cu) + 1);
            at<int8_t>(obj + 0x21cu) = frames < 4 ? frames : int8_t{3};
        }
        if (fastMethod != 0 && at<int8_t>(obj + 0x21cu) >= 3)
            at<int32_t>(obj + 0x1d8u) = 0;
        else
            GameTaskShadowCalc(ptr32<_sObject>(obj));
    }
}

RC_DECOMP_FN_SETUP_HEAP(GameTaskLvlInit, 0x123708, rc::fuzz::physicsRace)
RC_DECOMP_FN_SETUP_HEAP(GameTaskLvlTerm, 0x123748, rc::fuzz::physicsRace)
RC_DECOMP_FN_SETUP_HEAP(GameTaskUpdateCarOcols, 0x123758, rc::fuzz::physicsRace)
RC_DECOMP_FN_SETUP_HEAP(GameTaskSpecialPhysicsCases, 0x123890, rc::fuzz::physicsRace)
RC_DECOMP_FN_SETUP_HEAP(GameTaskProcess, 0x123c10, rc::fuzz::physicsTask)
RC_DECOMP_FN_SETUP_HEAP(GameTaskShadowCalc, 0x123d98, rc::fuzz::physicsObject)
RC_DECOMP_FN(GameTaskInit, 0x124028)
RC_DECOMP_FN(GameTaskTerm, 0x124030)
RC_DECOMP_FN_SETUP_HEAP(GameTaskGetShadowMaterialInfo, 0x124038, rc::fuzz::physicsRace)
RC_DECOMP_FN_SETUP_HEAP(MetaMorphiseCarSpheres, 0x1241f0, rc::fuzz::physicsTask)

} // namespace rc
