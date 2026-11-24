// Decompiled from OBJECT.c: the race's cars (and boats). An object is one heap block: the _sObject (0x42c bytes) and
// then, as its definition's memFlags ask, a render object (0x38), a second render object (the dump tug's sled, 0x38),
// a task (0x18), physics (0x23c) with its car collision record (0x14), wheels (0x14) and sounds (0x58). objectVarG.list
// holds the race's cars in player order and objectVarG.boatStore the boat each car changes into.
//
// This file: creation, set-up, deletion, tint, the weapon and sound bookkeeping. object2.cpp: the big per-frame sound
// updates, lights and drawing. The random-test setups are in object_fuzz.cpp.

#include "object.h"
#include "object_fuzz.h"

#include "rc_decomp.h"
#include "../rc_cars.h"
#include <cstdlib>

// Europe variant: ObjectInitRendObj differs only in register use and instruction order (the same body;
// tools/asm_listing.py NAME --diff).
RC_VERSION_VARIANT(ObjectInitRendObj)

namespace rc {

using namespace object;

// ---------------------------------------------------------------------------------------------------------------------
// Set-up and tear-down.

void ObjectTerm()
{
    ResTypeUnSetConstructDaemon(ptr32<char>(kObjectType));
}

void ObjectInit()
{
    ResTypeSetConstructDaemon(ptr32<char>(kObjectType), kObjectConstructDaemonAddr, 1);
    ResTypeSetConstructDaemon(ptr32<char>(kObjectType2), 0, 6);
    sObjectClass &vars = G(objectVarG);
    for (int i = 0; i < 8; ++i)
        vars.list[i] = ptr32<_sObject>(0u);
    vars.noObjects = 0;
    G(dumperHackG) = 0;
}

void ObjectDefaultControl(ptr32<void>, tTime) {}

// The definition's resource ids become the loaded resources. Words: 0 memFlags, 8 pRendObjHead, 9 pRendObjHeadAlt,
// 10 pNodeHead, 11 pPhysicsDef, 12 pWheelRes, 13-15 the soundHandles (the dump tug's three render objects first),
// 16 the shadow resource, 21 pWheelResP2.
void ObjectConstructDaemon(ptr32<memHandle> pHandle)
{
    GuestFrame frame(0x30);
    const uint32_t def = at<uint32_t>(pHandle.address);
    uint32_t *w = &at<uint32_t>(def);
    const auto load = [](uint32_t id) { return guest_call<uint32_t>(kResLoad, id); };
    if (G(objectConstructorNoLoadROBs) != 0)
    {
        w[8] = 0;
        w[9] = 0;
        w[12] = 0;
    }
    if (w[8] != 0)
        w[8] = load(w[8]);
    sCarListClass &cars = G(carListVarG);
    if (cars.alternativeRendObj == 1 && w[9] != 0)
    {
        cars.altRendObjResource = ptr32<_sRendObjHead>(w[9]);
        w[9] = load(w[9]);
    }
    if (cars.alternativeRendObj == 2 && w[9] != 0)
    {
        cars.altRendObjResourceBoat = ptr32<_sRendObjHead>(w[9]);
        w[9] = load(w[9]);
    }
    if (w[10] != 0)
        w[10] = load(w[10]);
    if (w[11] != 0)
        w[11] = load(w[11]);
    if (w[12] != 0)
        w[12] = load(w[12]);
    if (w[0] & 0x80u)
    {
        sObjectClass &vars = G(objectVarG);
        vars.pDumperRendObjHead2 = ptr32<_sRendObjHead>(load(w[13]));
        vars.pDumperRendObjHeadAlt2 = ptr32<_sRendObjHead>(load(w[14]));
        vars.pDumperRendObjHeadWeapon = ptr32<_sRendObjHead>(load(w[15]));
    }
    w[13] = 1;
    w[14] = 2;
    w[15] = 3;
    w[16] = load(w[16]);
    if (w[21] != 0)
        w[21] = load(w[21]);
}

uint32 ObjectCalcSize(ptr32<_sObjectDef> pDef)
{
    const uint32_t flags = pDef->memFlags;
    uint32_t size = (flags & 1u) ? 0x464u : 0x42cu;
    if (flags & 0x80u)
        size += 0x38;
    if (flags & 2u)
        size += 0x18;
    if (flags & 4u)
        size += 0x250;
    if (flags & 8u)
        size += 0x14;
    if (flags & 0x10u)
        size += 0x58;
    return size;
}

uint32 ObjectSetPointers(ptr32<_sObject> pObject, ptr32<_sObjectDef> pDef)
{
    const uint32_t flags = pDef->memFlags, base = pObject.address;
    _sObject &o = *pObject;
    uint32_t size = 0x42c;
    if (flags & 1u)
    {
        o.pRendObj = ptr32<_sRendObj>(base + 0x42cu);
        size = 0x464;
    }
    else
        o.pRendObj = ptr32<_sRendObj>(0u);
    if (flags & 0x80u)
    {
        o.pRendObj2 = ptr32<_sRendObj>(base + size);
        size += 0x38;
    }
    else
        o.pRendObj2 = ptr32<_sRendObj>(0u);
    if (flags & 2u)
    {
        o.pTask = ptr32<_sTask>(base + size);
        size += 0x18;
    }
    else
        o.pTask = ptr32<_sTask>(0u);
    if (flags & 4u)
    {
        o.pPhysics = ptr32<_sFPhysLibCar>(base + size);
        o.pCarCol = ptr32<_sCarCol>(base + size + 0x23cu);
        size += 0x250;
    }
    else
    {
        o.pPhysics = ptr32<_sFPhysLibCar>(0u);
        o.pCarCol = ptr32<_sCarCol>(0u);
    }
    if (flags & 8u)
    {
        o.pWheels = ptr32<_sWheels>(base + size);
        size += 0x14;
    }
    else
        o.pWheels = ptr32<_sWheels>(0u);
    if (flags & 0x10u)
    {
        o.pSounds = ptr32<_sAllObjSounds>(base + size);
        size += 0x58;
    }
    else
        o.pSounds = ptr32<_sAllObjSounds>(0u);
    return size;
}

// A quarter of the object's radius node, rounded toward zero.
static ee_float radiusOf(ptr32<_sObjectDef> pDef)
{
    const int32_t r = NodeFindByTag(pDef->pNodeHead, 0x10000237u)->radius;
    return ee_itof((r < 0 ? r + 3 : r) >> 2);
}

ptr32<_sObject> ObjectCreate(ptr32<_sObjectDef> pDef, tTime time, int32 type)
{
    GuestFrame frame(0x60);
    const ptr32<ptr32<_sObject>> slot = frame.local<ptr32<_sObject>>(0);
    *slot = ptr32<_sObject>(0u);
    if (!MemBlockAllocLo(ptr32<memHandle>(slot.address), ObjectCalcSize(pDef)))
        return *slot;
    (*slot)->pDef = pDef;
    ObjectSetPointers(*slot, pDef);
    sObjectClass &vars = G(objectVarG);
    at<ptr32<_sObject>>(kObjectVars + vars.noObjects * 4u) = *slot;
    vars.noObjects = vars.noObjects + 1;
    _sObject &o = **slot;
    o.playerType = static_cast<uint8>(type < 2 ? type : 2);
    o.playerNo = static_cast<uint8>(type);
    o.shakeCameraStrength = 8.0f;
    at<uint32_t>(slot->address + 0x424u) = 0;
    o.weaponType = 0;
    o.weaponLock = 0;
    o.weaponsHeld = 0;
    o.weaponAffectOwner = 0xff;
    o.shockWaved = 0;
    o.weaponAffect = 0;
    o.skidOil = 0;
    o.weaponAffectUniqueId = 0;
    o.weaponsHeldType = 0;
    o.pickupSpinning = 0;
    o.lightningSfx = -1;
    o.invincible = 0;
    o.heatrayStore = 0;
    o.heatrayStoreAngle = 0;
    o.waterEscape = 0;
    o.overtakingPrev = 0;
    o.weaponsHeldSfx = -1;
    o.overtakingSfx = -1;
    o.overtakingMisc = 0;
    o.weaponFiring = 0;
    o.overtakingTimer = 0;
    o.overtakingTimerStart = 0;
    o.warningLockonSfx = -1;
    o.lockOnMe = 0;
    o.weaponsHeldNext = 0;
    o.weaponsHeldTypeNext = 0;
    o.metamorphosis = 0;
    o.respawn = 0;
    o.rightCar = 0;
    o.hasAttachments = 0;
    o.framesUnrendered = 0;
    o.iRacePos = 1;
    o.aiFire = 0;
    o.weaponAffectTime = 0;
    o.pVfxMisc = ptr32<_sVfx_Object>(0u);
    o.pVfxSarge = ptr32<_sVfx_Object>(0u);
    o.pVfxOilSquirt = ptr32<_sVfx_Object>(0u);
    o.pVfxHexed = ptr32<_sVfx_Object>(0u);
    o.pVfxTugBubble = ptr32<_sVfx_Object>(0u);
    o.iRacePosAverage = 1;
    o.sDontRenderMe = 0;
    o.tipAngle = 0;
    o.zShortestDrawnDist = 0x7fff;
    o.pushRenderDist = 0;
    o.zDrawnDist = 0x7fff;
    ObjectInitObject(*slot, pDef, time);
    const ptr32<_sObject> obj = *slot;
    obj->oCol.type = 0;
    const int32_t x = pDef->pos.x, y = pDef->pos.y, z = pDef->pos.z;
    obj->pos16.x = ee_itof(x);
    obj->pos16.y = ee_itof(y);
    obj->pos16.z = ee_itof(z);
    const uint32_t physics = obj->pPhysics.address; // read after the set-up, written even when there is none
    at<ee_float>(physics + 8u) = ee_itof(x << 8);
    at<ee_float>(physics + 12u) = ee_itof(y << 8);
    at<ee_float>(physics + 16u) = ee_itof(z << 8);
    const ee_float rad = radiusOf(pDef);
    const ptr32<_sObject> same = *slot;
    same->rad = rad;
    OColInitObj(ptr32<_sOColObj>(same.address + 0x50u), ptr32<_sColCellVecPSX>(same.address + 0xbcu),
                ptr32<ee_float>(same.address + 0xccu), ptr32<void>(same.address));
    return *slot;
}

ptr32<_sObject> ObjectCreateBoat(ptr32<_sObjectDef> pDef, tTime time, int32 type)
{
    GuestFrame frame(0x50);
    const ptr32<ptr32<_sObject>> slot = frame.local<ptr32<_sObject>>(0);
    *slot = ptr32<_sObject>(0u);
    if (!MemBlockAllocLo(ptr32<memHandle>(slot.address), ObjectCalcSize(pDef)))
        return *slot;
    (*slot)->playerType = static_cast<uint8>(type < 2 ? type : 2);
    (*slot)->pDef = pDef;
    ObjectSetPointers(*slot, pDef);
    const ee_float rad = radiusOf(pDef);
    (*slot)->rad = rad;
    ObjectInitObject(*slot, pDef, time);
    return *slot;
}

void ObjectInitTBoxMask(ptr32<_sObject> pObject, ptr32<_sObjectDef>)
{
    const mainClass &main = G(mainVarG);
    uint32_t mask;
    if (pObject->playerType == 0 && main.noHuman != 0)
        mask = 0x4000004u;
    else if (pObject->playerType == 1 && main.noHuman == 2)
        mask = 0x4000008u;
    else
        mask = (0x100u << ((G(objectVarG).noObjects - 1u) & 31u)) | 0x4000000u;
    if (static_cast<uint32_t>(main.status) == 0x10u)
        mask |= 0x8000000u;
    mask |= at<uint32_t>(pObject->pDef.address + 4u) < 0x14u ? 0x1000000u : 0x2000000u;
    pObject->tBoxMask = mask;
}

void ObjectInitRendObj(ptr32<_sObject> pObject, ptr32<_sObjectDef> pDef, tTime arg2)
{
    at<uint32_t>(pObject.address + 8u) = 0x80000000u; // target tint 0, 0, 0, brightness 0x80
    const ptr32<_sRendObj> r = pObject->pRendObj;
    at<uint32_t>(pObject.address + 4u) = 0x80000000u;
    const auto place = [&](ptr32<_sRendObj> ro, int32_t raise)
    {
        ro->angles.pitch = pDef->angles.pitch;
        ro->angles.head = pDef->angles.head;
        ro->angles.roll = pDef->angles.roll;
        ro->pos.x = pDef->pos.x << 8;
        ro->pos.y = (pDef->pos.y + raise) << 8;
        ro->pos.z = pDef->pos.z << 8;
        MatPsxInit(ptr32<_matPsx>(ro.address + 0x18u));
    };
    r->pRendObjHead = pDef->pRendObjHead;
    place(r, 0);
    if (pDef->memFlags & 0x80u)
    {
        const ptr32<_sRendObj> r2 = pObject->pRendObj2;
        if (G(dumperHackG) == 0)
        {
            G(dumperHackG) = 1;
            r2->pRendObjHead = G(objectVarG).pDumperRendObjHead2;
        }
        else
        {
            G(dumperHackG) = 0;
            r2->pRendObjHead = G(objectVarG).pDumperRendObjHeadAlt2;
        }
        place(r2, 100);
    }
}

void ObjectInitTask(ptr32<_sObject> pObject, ptr32<_sObjectDef>, tTime time)
{
    const ptr32<_sTask> task = pObject->pTask;
    {
        GuestFrame frame(0x20);
        guest_call(kTaskSet, task, kObjectDefaultControlAddr, pObject, time);
    }
    guest_call(kTaskCachedInsert, task); // a tail call
}

void ObjectInitWheels(ptr32<_sObject> pObject, ptr32<_sObjectDef> pDef, tTime)
{
    const ptr32<_sWheels> wheels = pObject->pWheels;
    wheels->pFrontL = NodeFindByTag(pDef->pNodeHead, 0x10000232u);
    wheels->pFrontR = NodeFindByTag(pDef->pNodeHead, 0x10000231u);
    wheels->pRearL = NodeFindByTag(pDef->pNodeHead, 0x10000233u);
    wheels->pRearR = NodeFindByTag(pDef->pNodeHead, 0x10000234u);
    wheels->pWheelRes = pDef->pWheelRes;
    const int32_t n = static_cast<int32_t>(G(objectVarG).noObjects) - 1;
    if (at<int32_t>(kCarList + static_cast<uint32_t>(n * 12) + 8u) != 0) // carListVarG.car[n].altRendObj
        pObject->pWheels->pWheelRes = pDef->pWheelResP2;
}

void ObjectInitSounds(ptr32<_sObject> pObject, ptr32<_sObjectDef>, tTime)
{
    ObjectAllocateEngineSfx(pObject);
    _sAllObjSounds &s = *pObject->pSounds;
    for (sObjSound *snd : {&s.engine[0], &s.engine[1], &s.scrape[0], &s.scrape[1], &s.skid[0], &s.skid[1], &s.rocket[0], &s.rocket[1]})
    {
        snd->handle = -1;
        snd->pitch = 0x100;
        snd->volume = 0x6c;
    }
    s.material.handle = -1;
    s.material.pitch = 0x100;
    s.material.volume = 0x6c;
    s.material.sample = 0xff;
    s.material.oldMaterial = 0;
    if (pObject->playerType != 2)
        at<int16_t>(pObject->pSounds.address + pObject->playerType * 8u + 6u) = 0x80;
}

void ObjectClearSfx(ptr32<_sObject> pObject)
{
    if (!pObject)
        return;
    _sAllObjSounds &s = *pObject->pSounds;
    for (sObjSound *snd : {&s.engine[0], &s.engine[1], &s.scrape[0], &s.scrape[1], &s.skid[0], &s.skid[1], &s.rocket[0], &s.rocket[1]})
    {
        snd->handle = -1;
        snd->pitch = 0x100;
        snd->volume = 0x6c;
    }
    s.engine[0].volume = 0x50;
    s.engine[1].volume = 0x50;
    s.material.handle = -1;
    s.material.pitch = 0x100;
    s.material.volume = 0x6c;
    s.material.sample = 0xff;
    s.material.oldMaterial = 0;
    pObject->weaponsHeldSfx = -1;
}

void ObjectInitObject(ptr32<_sObject> pObject, ptr32<_sObjectDef> pDef, tTime time)
{
    GuestFrame frame(0x40);
    ObjectInitTBoxMask(pObject, pDef);
    pObject->pNodeHead = pDef->pNodeHead;
    ObjectInitLights(pObject, pDef);
    if (pDef->memFlags & 1u)
        ObjectInitRendObj(pObject, pDef, time);
    if (pDef->memFlags & 2u)
        ObjectInitTask(pObject, pDef, time);
    if (pDef->memFlags & 8u)
    {
        ObjectInitWheels(pObject, pDef, time);
        pObject->wheelSizeFront = pDef->wheelSizeFront;
        pObject->wheelSizeRear = pDef->wheelSizeRear;
    }
    if (pDef->memFlags & 4u)
    {
        guest_call(kPhysicsAddCar, pObject, pDef);
        CarColInitObj(pObject);
    }
    if (pDef->memFlags & 0x10u)
        ObjectInitSounds(pObject, pDef, time);
    if (pDef->memFlags & 0x20u)
        pObject->ShadowRes = pDef->ShadowRes;
    if (pDef->memFlags & 0x40u)
    {
        pObject->pAerialNode = NodeFindByTag(pDef->pNodeHead, 0x10000230u);
        pObject->uiRibbonColour = pDef->uiRibbonColour;
    }
}

// The body of ObjectDelete, with the object held in the stack word at slot (MemBlockFree takes its address).
static void deleteIn(ptr32<ptr32<_sObject>> slot)
{
    if ((*slot)->pPhysics)
    {
        G(objectVarG).noObjects = G(objectVarG).noObjects - 1;
        guest_call(kPhysicsDeleteCar, *slot);
        OColDeleteObject(ptr32<_sOColObj>(slot->address + 0x50u));
    }
    if ((*slot)->pTask)
        guest_call(kTaskDelete, (*slot)->pTask);
    MemBlockFree(ptr32<memHandle>(slot.address));
}

void ObjectDelete(ptr32<_sObject> pObject)
{
    GuestFrame frame(0x20);
    const ptr32<ptr32<_sObject>> slot = frame.local<ptr32<_sObject>>(0);
    *slot = pObject;
    deleteIn(slot);
}

void ObjectDeleteAll()
{
    GuestFrame frame(0xa0);
    const int32_t count = static_cast<int32_t>(G(objectVarG).noObjects);
    for (int32_t i = 0; i < count; ++i)
    {
        const uint32_t car = kObjectVars + static_cast<uint32_t>(i) * 4u, boat = car + 0xb8u;
        const ptr32<ptr32<_sObject>> first = frame.local<ptr32<_sObject>>(0);
        *first = at<ptr32<_sObject>>(car);
        deleteIn(first);
        at<uint32_t>(car) = 0;
        if (at<uint32_t>(boat) != 0)
        {
            const ptr32<ptr32<_sObject>> second = frame.local<ptr32<_sObject>>(4);
            *second = at<ptr32<_sObject>>(boat);
            deleteIn(second);
            at<uint32_t>(boat) = 0;
        }
    }
}

// The race's cars, made from the car list on the level's start grid (as boats on the water levels), each given to a
// player or the AI; then the boats the cars change into on the levels with both.
void ObjectCreateCars(tTime time)
{
    GuestFrame frame(0x110);
    constexpr uint32_t kPlayerControl = sym::PlayerControl, kAIControl = sym::AIControl;
    const int32_t count = G(playerVarG).noPlayers + G(aiVarG).noAIPlayers;
    const auto boatLevel = []() { const uint32_t l = G(mainVarG).level % 12u; return l == 5 || l == 0 || l == 8; };
    const auto carAt = [](int32_t i) { return kCarList + static_cast<uint32_t>(i) * 12u; };
    // pStartData; enhancement: with more than 8 cars, a copy with rows added behind along the route (rc_cars.h)
    const uint32_t grid = cars::startGrid(count);
    for (int32_t i = 0; i < count; ++i)
    {
        ptr32<_sObjectDef> def;
        if (boatLevel())
        {
            def = at<ptr32<_sObjectDef>>(carAt(i) + 0xf4u);
            def->pPhysicsDef = at<ptr32<_sObjectDef>>(carAt(i) + 4u)->pPhysicsDef;
            def->memFlags = static_cast<eObjectMemFlags>(def->memFlags | 6u);
        }
        else
            def = at<ptr32<_sObjectDef>>(carAt(i) + 4u);
        const int32_t x = def->pos.x;
        def->pos.z = 0;
        def->angles.pitch = 0;
        def->angles.head = 0;
        def->pos.x = x + i * 0x7d;
        def->angles.roll = 0;
        const uint32_t start = grid;
        if (start != 0)
        {
            const uint32_t place = start + static_cast<uint32_t>(count - (i + 1)) * 8u;
            def->pos.x = at<int16_t>(place);
            def->pos.y = at<int16_t>(place + 2u);
            def->pos.z = at<int16_t>(place + 4u);
            def->angles.head = static_cast<angle>(AINodeGetNearestHeading(ptr32<SVECTOR>(place)) & 0xfff);
            const uint32_t a = start + ((i & 1) ? 8u : 0u), b = start + ((i & 1) ? 0x38u : 0x30u);
            const int32_t dz = static_cast<int16_t>(at<uint16_t>(b + 4u) - at<uint16_t>(a + 4u));
            const int32_t dx = static_cast<int16_t>(at<uint16_t>(b) - at<uint16_t>(a));
            def->angles.head = static_cast<angle>((static_cast<int32_t>(ArcTan(dz, dx)) + 0x8000) >> 4);
            if (int16_t heading; cars::addedPlaceHeading(count - (i + 1), heading))
                def->angles.head = static_cast<angle>(heading); // an added place: along the route there
        }
        const ptr32<_sObject> o = ObjectCreate(def, time, i);
        if (at<int32_t>(carAt(i) + 8u) != 0) // altRendObj
        {
            const ptr32<_sRendObjHead> alt = o->pDef->pRendObjHeadAlt;
            o->uiRibbonColour = def->uiRibbonColourAlt;
            o->pRendObj->pRendObjHead = alt;
        }
        if (i < static_cast<int32_t>(G(mainVarG).noHuman))
        {
            // RC_TEST_AUTOPILOT=1 (a test aid, NOTES.md "Car limits"): the players' cars drive themselves, as the computer's do.
            static const bool autopilot = [] { const char *v = std::getenv("RC_TEST_AUTOPILOT"); return v && *v == '1'; }();
            at<uint32_t>(o->pTask.address + 8u) = autopilot ? kAIControl : kPlayerControl;
            at<ptr32<_sObject>>(sym::playerVarG + 8u + static_cast<uint32_t>(i) * 0x38u) = o; // playerVarG.player[i].pObject
            o->playerType = static_cast<uint8>(i);
        }
        else
        {
            at<uint32_t>(o->pTask.address + 8u) = kAIControl;
            o->playerType = 2;
        }
        if (boatLevel())
            guest_call(kPhysicsCarBoatSwap, o->pPhysics, o);
        else if (G(mainVarG).splitScreen == 0)
        {
            uint16_t &flags = at<uint16_t>(o->pRendObj->pRendObjHead.address + 0x58u);
            flags = static_cast<uint16_t>(flags | 0x100u);
        }
    }
    for (int32_t i = 0; i < count; ++i)
    {
        const uint32_t store = kObjectVars + 0xb8u + static_cast<uint32_t>(i) * 4u;
        const uint32_t l = G(mainVarG).level % 12u;
        if (!((l == 9 || l == 0xb) && G(mainVarG).level != 0x61))
        {
            at<uint32_t>(store) = 0;
            continue;
        }
        const ptr32<ptr32<_sObject>> slot = frame.local<ptr32<_sObject>>(0x60);
        *slot = ptr32<_sObject>(0u);
        const ptr32<_sObjectDef> def = at<ptr32<_sObjectDef>>(carAt(i) + 0xf4u);
        if (MemBlockAllocLo(ptr32<memHandle>(slot.address), ObjectCalcSize(def)))
        {
            (*slot)->playerType = static_cast<uint8>(i < 2 ? i : 2);
            (*slot)->pDef = def;
            ObjectSetPointers(*slot, def);
            const ee_float rad = radiusOf(def);
            (*slot)->rad = rad;
            ObjectInitObject(*slot, def, time);
        }
        const ptr32<_sObject> boat = *slot;
        if (at<int32_t>(carAt(i) + 0xf8u) != 0)
        {
            const ptr32<_sRendObjHead> alt = boat->pDef->pRendObjHeadAlt;
            boat->uiRibbonColour = def->uiRibbonColourAlt;
            boat->pRendObj->pRendObjHead = alt;
        }
        at<uint32_t>(store) = boat.address;
        if (G(mainVarG).splitScreen == 0)
        {
            uint16_t &flags = at<uint16_t>(boat->pRendObj->pRendObjHead.address + 0x58u);
            flags = static_cast<uint16_t>(flags | 0x100u);
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Tint and the per-frame loops.

void ObjectInterpolateTint(ptr32<_sObject> pObject)
{
    uint8_t *b = &at<uint8_t>(pObject.address);
    for (int i = 4; i < 8; ++i)
    {
        const int32_t delta = static_cast<int32_t>(b[i + 4]) - static_cast<int32_t>(b[i]);
        if (delta >= 5)
            b[i] = static_cast<uint8_t>(b[i] + 4);
        else if (delta < -4)
            b[i] = static_cast<uint8_t>(b[i] - 4);
    }
}

void ObjectResetAllTint()
{
    for (uint32_t i = 0; i < G(objectVarG).noObjects; ++i)
        at<uint32_t>(objectAt(i).address + 8u) = 0; // the four target tints, as four byte stores
}

void ObjectRenderAll()
{
    GuestFrame frame(0x50);
    RdrZedsWriteOff(0);
    for (uint32_t i = 0; i < G(objectVarG).noObjects; ++i)
    {
        const ptr32<_sObject> o = objectAt(i);
        o->lightsOn = static_cast<int8>(static_cast<uint8_t>(o->lightsOn) | 2u);
        ObjectInterpolateTint(o);
        ObjectRender(o);
    }
}

void ObjectSfxDoAll(int32 flags, tTime time)
{
    const mainClass &main = G(mainVarG);
    if (at<uint16_t>(sym::mainVarG + 0x366u) == 1 || static_cast<uint32_t>(main.status) == 4u ||
        static_cast<uint32_t>(main.gameEndPos) >= 2u)
        return;
    GuestFrame frame(0x70);
    const HeldS2 s2(static_cast<uint32_t>(time));
    for (uint32_t i = 0; i < G(objectVarG).noObjects; ++i)
    {
        const ptr32<_sObject> o = objectAt(i);
        ObjectUpdateEngine(o, flags);
        ObjectUpdateSkid(o, flags, time);
        ObjectUpdateScrape(o, flags);
        ObjectUpdateOvertaking(o, flags, time);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Weapons held and suffered.

void ObjectTestWaterEscape(ptr32<_sObject> pObject)
{
    if (pObject->weaponAffect != 5)
        return;
    pObject->waterEscape = static_cast<int8>(pObject->waterEscape + 1);
    if (pObject->waterEscape < 7)
        return;
    const sWeaponClass &weapons = G(weaponVarG);
    ptr32<_sWeapon> w = weapons.pList;
    for (int32_t i = 0; i < G(weaponVarG).noWeapons; ++i)
    {
        if (w)
        {
            if (w->target == static_cast<int16>(pObject->playerNo) && w->no == 5)
            {
                w->timeWhenEnds = 0;
                return;
            }
            w = w->pNext;
        }
    }
    pObject->waterEscape = 0;
}

void ObjectWeaponAffectRemove(ptr32<_sObject> pObject)
{
    GuestFrame frame(0x20);
    const int16 affect = pObject->weaponAffect;
    if (affect == 7)
    {
        pObject->weaponAffectOwner = 0xff;
        if (pObject->pVfxMisc)
        {
            VfxDelete(pObject->pVfxMisc);
            pObject->pVfxMisc = ptr32<_sVfx_Object>(0u);
            sfxStop(pObject->lightningSfx);
            pObject->lightningSfx = -1;
        }
    }
    else if (affect == 5)
    {
        ptr32<_sWeapon> w = G(weaponVarG).pList;
        for (int32_t i = 0; i < G(weaponVarG).noWeapons; ++i)
        {
            if (at<uint32_t>(w.address) == 0x10005u && w->target == static_cast<int16>(pObject->playerNo)) // no 5, misc 1
            {
                w->timeWhenEnds = 0;
                const ptr32<_sObject> owner = objectAt(static_cast<uint32_t>(static_cast<int32_t>(w->owner)));
                if (owner->weaponsHeld != 0 && owner->weaponsHeldType == 5)
                    owner->weaponType = 5;
                return;
            }
            if (w->pNext)
                w = w->pNext;
        }
    }
    else if (affect == 0xd && pObject->pVfxHexed)
    {
        VfxDelete(pObject->pVfxHexed);
        pObject->pVfxHexed = ptr32<_sVfx_Object>(0u);
    }
}

void ObjectAllRemoveWeapons()
{
    GuestFrame frame(0x60);
    WeaponRemoveAll();
    for (uint32_t i = 0; i < G(objectVarG).noObjects; ++i)
    {
        const ptr32<_sObject> o = objectAt(i);
        for (uint32_t offset : {0x1c0u, 0x1b8u, 0x1bcu, 0x1c4u}) // pVfxOilSquirt, pVfxMisc, pVfxSarge, pVfxHexed
        {
            ptr32<_sVfx_Object> &fx = at<ptr32<_sVfx_Object>>(o.address + offset);
            if (fx)
            {
                VfxDelete(fx);
                at<uint32_t>(o.address + offset) = 0;
            }
        }
        const int8 lockon = o->warningLockonSfx;
        o->weaponsHeld = 0;
        o->weaponsHeldType = 0;
        o->weaponsHeldNext = 0;
        o->weaponsHeldTypeNext = 0;
        o->weaponType = 0;
        o->heatrayStore = 0;
        o->heatrayStoreAngle = 0;
        o->weaponAffect = 0;
        o->weaponLock = 0;
        o->lockOnMe = 0;
        if (lockon != -1)
        {
            sfxStop(lockon);
            o->warningLockonSfx = -1;
        }
        if (o->weaponsHeldSfx != -1)
        {
            sfxStop(o->weaponsHeldSfx);
            o->weaponsHeldSfx = -1;
        }
    }
}

void ObjectGiveNextWeapon(ptr32<_sObject> pObject)
{
    GuestFrame frame(0x20);
    _sObject &o = *pObject;
    if (o.weaponsHeld != 0 && o.weaponsHeldType == 0x10 && o.tipAngle < static_cast<int32_t>(G(mainVarG).time))
        o.weaponType = 0x10;
    if (o.weaponFiring != 0 || o.weaponsHeldNext == 0)
        return;
    const uint8_t next = static_cast<uint8_t>(o.weaponsHeldTypeNext);
    o.weaponsHeld = o.weaponsHeldNext;
    o.weaponType = next;
    o.weaponsHeldType = static_cast<int8>(next);
    if (next == 5)
    {
        sfx3D(0x1f, 0xff, 0x100, rendPos(pObject), 4, 5000);
        VfxAddNew(pObject, 0, 0, 0xb);
    }
    if (o.weaponType == 1)
        VfxAddNew(pObject, 0, 0, 0x36);
    if (o.weaponType == 8)
    {
        o.heatrayStore = 4000;
        if (o.weaponsHeldSfx != -1)
            sfxStop(o.weaponsHeldSfx);
        o.weaponsHeldSfx = static_cast<int16>(sfx3D(0x15, 0xff, 0x100, rendPos(pObject), 4, 5000));
        VfxAddNew(pObject, 0, 0, 4);
    }
    o.weaponsHeldNext = 0;
    o.weaponsHeldTypeNext = 0;
}

// The effects of a weapon on the car that suffers it, once a frame: the oil squirt drops its slick, the heat ray hums,
// the water balloon lifts and slows, lightning and the dump tug's bubble choke the throttle, the timed ones end.
void ObjectWeaponAffect(ptr32<_sObject> pObject, tTime time)
{
    _sObject &o = *pObject;
    if (static_cast<uint32_t>(G(mainVarG).status) == 0x20u)
    {
        o.weaponAffect = 0;
        return;
    }
    GuestFrame frame(0xa0);
    const ptr32<_sVfx_Object> squirt = o.pVfxOilSquirt;
    if (squirt)
    {
        const uint32_t fx = squirt.address;
        const int32_t stage = at<int32_t>(fx + 0x1d4u);
        if (stage == 1)
        {
            const uint32_t def = frame.local<uint8_t>(0).address; // specialFire (+2) is left as the stack has it
            at<int16_t>(def) = 6;
            at<int16_t>(def + 8u) = 5000;
            at<int16_t>(def + 0xau) = 0x7d;
            at<int16_t>(def + 4u) = o.playerNo;
            at<int16_t>(def + 6u) = 0;
            VfxAddNewPlace(at<int16_t>(fx + 0x1c8u), at<int16_t>(fx + 0x1cau), at<int16_t>(fx + 0x1ccu), 10);
            const tTime now = guest_call<int32_t>(kTimerGet, ptr32<uint8>((sym::mainVarG + 0x10u)));
            WeaponCreate(ptr32<vec16_>(fx + 0x1c8u), now, ptr32<_sWeaponDef>(def));
            o.pVfxOilSquirt = ptr32<_sVfx_Object>(0u);
            if (at<int32_t>(fx + 0x1d4u) == 2)
                o.pVfxOilSquirt = ptr32<_sVfx_Object>(0u);
        }
        else if (stage == 2)
            o.pVfxOilSquirt = ptr32<_sVfx_Object>(0u);
    }
    if (o.weaponType == 8)
    {
        int32_t number, kind;
        if (o.weaponsHeldSfx != -1)
            number = o.weaponsHeldSfx, kind = o.playerType + 0x18;
        else
            number = 0x15, kind = o.playerType + 4;
        o.weaponsHeldSfx = static_cast<int16>(sfx3D(number, 0xff, 0x100, rendPos(pObject), kind, 2000));
    }
    if (o.weaponAffect == 0)
        return;
    const auto quarterThrottle = [&]()
    {
        int16 &t = o.pPhysics->inputEngineThrottle;
        t = static_cast<int16>((t < 0 ? t + 3 : t) >> 2);
    };
    if (o.weaponAffect == 5)
    {
        _sFPhysLibCar &p = *o.pPhysics;
        p.inputEngineThrottle = 0;
        p.vel.y = 800.0f;
        at<uint32_t>(o.pPhysics.address + 0x20u) = 0;
        at<uint32_t>(o.pPhysics.address + 0x28u) = 0;
        const ptr32<sVec16> force = frame.local<sVec16>(0), offset = frame.local<sVec16>(0x10);
        offset->z = 0x1194;
        offset->x = 0;
        offset->y = 0;
        force->x = static_cast<int16>(o.playerType < 2 ? 0x96 : 0x258);
        force->y = 0;
        force->z = 0;
        guest_call(kPhysicsApplyIntForce, o.pPhysics, force, offset, 3);
        if (RandomGetNDRange(1, 0x19) == 1)
        {
            const int32_t pitch = static_cast<int16_t>(RandomGetNDRange(0, 100) + 0xc4);
            sfx3D(0x1f, 0xc4, pitch, rendPos(pObject), o.playerType + 4, 2000);
        }
        o.carAI.iSlowSpeedAccum = 0;
    }
    if (o.weaponAffect == 7)
    {
        quarterThrottle();
        int32_t number, kind;
        if (o.lightningSfx != -1)
            number = o.lightningSfx, kind = o.playerType + 0x18;
        else
            number = 0x13, kind = o.playerType + 4;
        o.lightningSfx = static_cast<int8>(sfx3D(number, 0x80, 0x100, rendPos(pObject), kind, 0x5dc));
    }
    if (o.weaponAffect == 9)
        o.pPhysics->inputEngineThrottle = 0;
    if (o.weaponAffect == 8)
        o.pPhysics->inputEngineThrottle = 0;
    for (int16 frozen : {int16(4), int16(1)})
        if (o.weaponAffect == frozen)
        {
            at<uint32_t>(o.pPhysics.address + 0x38u) = 0; // speed
            o.pPhysics->inputEngineThrottle = 0;
            if (o.weaponAffectTime < time)
            {
                ObjectWeaponAffectRemove(pObject);
                o.weaponAffect = 0;
            }
        }
    if (o.weaponAffect == 0xd)
    {
        if (o.weaponAffectTime < time)
        {
            ObjectWeaponAffectRemove(pObject);
            o.weaponAffect = 0;
            if (o.pVfxHexed)
            {
                VfxDelete(o.pVfxHexed);
                o.pVfxHexed = ptr32<_sVfx_Object>(0u);
            }
        }
        else if (o.weaponAffectTime - 500 < time && at<uint16_t>(o.pVfxHexed.address + 0x10u) >= 0x20u)
            at<uint16_t>(o.pVfxHexed.address + 0x10u) = 0x1f; // iLifeSpan: the hex fades
    }
    if (o.weaponAffect != 0xf)
        return;
    quarterThrottle();
    const ptr32<sVec16> force = frame.local<sVec16>(0x20), offset = frame.local<sVec16>(0x30);
    offset->z = 0x1194;
    offset->x = 0;
    offset->y = 0;
    force->x = static_cast<int16>(o.playerType < 2 ? 0xf : 0x3c);
    force->y = 0;
    force->z = 0;
    guest_call(kPhysicsApplyIntForce, o.pPhysics, force, offset, 3);
    o.carAI.iSlowSpeedAccum = 0;
    if (o.weaponAffectTime < time)
    {
        ObjectWeaponAffectRemove(pObject);
        o.weaponAffect = 0;
        if (o.pVfxTugBubble)
        {
            VfxDelete(o.pVfxTugBubble);
            o.pVfxTugBubble = ptr32<_sVfx_Object>(0u);
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Sounds: the sample for a material, the engine's sample and pitch range per car.

int32 ObjectMaterialSfx(int32 type, int32 material)
{
    if (type != 0)
    {
        switch (material)
        {
        case 5: case 0xc: case 0x21: case 0x39:
            return 0x58;
        case 7: case 0x31: case 0x32: case 0x34:
            return 0x25;
        case 0x10: case 0x11: case 0x27: case 0x28: case 0x2a: case 0x2b: case 0x38:
            return 0x4d;
        case 0x29: case 0x33: case 0x35: case 0x42: case 0x43:
            return 0x24;
        default:
            return 0x23;
        }
    }
    switch (material)
    {
    case 7:
        return 0x48;
    case 0x11: case 0x27: case 0x2a:
        return 0x49;
    case 0x29:
        return 0x59;
    case 0x31: case 0x32:
        return 0x47;
    case 0x33: case 0x35:
        return 0x4b;
    case 0x34:
        return 0x4c;
    case 0x42:
        return G(mainVarG).level == 0x61 ? 0x5f : 0x4a;
    case 0x43:
        return G(mainVarG).level == 0x61 ? 0x60 : 0x4a;
    default:
        return 0x4a;
    }
}

void ObjectAllocateEngineSfx(ptr32<_sObject> pObject)
{
    const ptr32<_sObjectDef> def = pObject->pDef;
    _sAllObjSounds &s = *pObject->pSounds;
    s.engineOldPitch = 0;
    s.enginePitchRate = 0;
    // Per car (boats share their car's entry): soundHandle2, soundHandle1, pitch modify, accel, max in air.
    struct Engine
    {
        int32_t handle2, handle1;
        int16_t modify, accel, maxInAir;
    };
    static const Engine kEngines[19] = {
        {0x28, 0, 0x80, 14000, 0x26c},     {0x35, 1, 0x9c, 22000, 0x172},     {0x34, 2, 0x9c, 0x2616, 0x26c},
        {0x33, 3, 0x15a, 3000, 0x5f0},     {0x32, 4, 0xf1, 16000, 0x26c},     {0x31, 5, 0x11e, 0x128e, 900},
        {0x30, 2, 0x83, 10000, 0x26c},     {0x2e, 6, 0xd2, 0x1d4c, 0x348},    {0x2d, 7, 200, 13000, 0x2d0},
        {0x2c, 8, 0x13b, 7000, 0x366},     {0x2b, 9, 0xa6, 0x1d4c, 0x26c},    {0x2a, 10, 0xb5, 14000, 0x26c},
        {0x29, 4, 0x140, 0x2616, 0x334},   {0x2f, 0, 0x8d, 0x4844, 0x26c},    {0x36, 0xb, 0xc4, 21000, 0x26c},
        {0x57, 0xc, 0xa6, 23000, 0x26c},   {0x53, 0xd, 0xa1, 19000, 0x26c},   {0x5a, 0xe, 0x119, 0x4556, 0x26c},
        {0x52, 0xf, 0xc4, 16000, 0x26c},
    };
    const uint32_t index = static_cast<uint32_t>(def->type) - 1u;
    if (index < 0x26u)
    {
        const Engine &e = kEngines[index % 19u];
        if (index % 19u == 18u)
            def->soundHandle3 = 0x10;
        def->soundHandle2 = e.handle2;
        def->soundHandle1 = e.handle1;
        s.enginePitchModify = e.modify;
        s.engineAccel = e.accel;
        s.engineMaxInAir = e.maxInAir;
    }
    s.enginePitchMax = static_cast<int16>(s.enginePitchModify + 0x100);
}

// ---------------------------------------------------------------------------------------------------------------------
// Car and boat: the car swaps its definition, aerial, render object and shadow with its boat's.

void ObjectCarChangeOver(ptr32<_sObject> pObj, int32 iToWhat)
{
    const bool isCar = static_cast<uint32_t>(pObj->pDef->type) < 0x14u;
    if (!(iToWhat == 2 || (isCar ? iToWhat == 1 : iToWhat == 0)))
        return;
    GuestFrame frame(0x40);
    const mainClass &main = G(mainVarG);
    if (main.toDo == 1)
        guest_call(kSfxPlay, 0x3a, 0xff, 0xf, 0xf, 0x100, 0, 0);
    else if (pObj->playerType != 2)
        sfx3D(0x3a, 0xc4, 0x100, rendPos(pObj), pObj->playerType + 4, 5000);
    const ptr32<_sObjectDef> oldDef = pObj->pDef;
    const ptr32<_sNode> oldAerial = pObj->pAerialNode;
    const ptr32<_sRendObjHead> oldHead = pObj->pRendObj->pRendObjHead;
    const ptr32<_sObject> boat = at<ptr32<_sObject>>(kObjectVars + 0xb8u + pObj->playerNo * 4u);
    const ptr32<_sRendObj> boatRend = boat->pRendObj;
    pObj->pRendObj->pRendObjHead = boatRend->pRendObjHead;
    pObj->pDef = boat->pDef;
    pObj->pAerialNode = boat->pAerialNode;
    boatRend->pRendObjHead = oldHead;
    boat->pDef = oldDef;
    boat->pAerialNode = oldAerial;
    if (G(frontEndVarG).active != 2)
    {
        const sShadowRes temp = pObj->ShadowRes;
        *frame.local<sShadowRes>(0) = temp;
        pObj->ShadowRes = boat->ShadowRes;
        boat->ShadowRes = temp;
    }
    if (main.toDo != 1 && main.gameEndPos == 0 && pObj->playerType == 0 && main.splitScreen == 0)
    {
        if (static_cast<uint32_t>(pObj->pDef->type) >= 0x14u)
        {
            guest_call(kRCMusicSwitchAlternate, 1);
            if (static_cast<uint32_t>(pObj->pDef->type) < 0x14u)
                guest_call(kRCMusicSwitchAlternate, 0);
        }
        else
            guest_call(kRCMusicSwitchAlternate, 0);
    }
    guest_call(kPhysicsCarBoatSwap, pObj->pPhysics, pObj);
    pObj->timeMetamorphosisStarted = -1;
}

} // namespace rc

RC_DECOMP_FN_SETUP_HEAP(ObjectTerm, rc::fuzz::objectNone)
RC_DECOMP_FN_SETUP_HEAP(ObjectInit, rc::fuzz::objectNone)
RC_DECOMP_FN(ObjectDefaultControl)
RC_DECOMP_FN_SETUP_HEAP(ObjectConstructDaemon, rc::fuzz::objectDaemon)
RC_DECOMP_FN_SETUP(ObjectCalcSize, rc::fuzz::objectDefOnly)
RC_DECOMP_FN_SETUP(ObjectSetPointers, rc::fuzz::objectScratchDef)
RC_DECOMP_FN_SETUP_HEAP(ObjectCreate, rc::fuzz::objectCreate)
RC_DECOMP_FN_SETUP_HEAP(ObjectCreateBoat, rc::fuzz::objectCreate)
RC_DECOMP_FN_SETUP(ObjectInitTBoxMask, rc::fuzz::objectScratchDef)
RC_DECOMP_FN_SETUP(ObjectInitRendObj, rc::fuzz::objectScratchDef)
RC_DECOMP_FN_SETUP_HEAP(ObjectInitTask, rc::fuzz::objectScratchDef)
RC_DECOMP_FN_SETUP(ObjectInitWheels, rc::fuzz::objectScratchDef)
RC_DECOMP_FN_SETUP(ObjectInitSounds, rc::fuzz::objectScratchDef)
RC_DECOMP_FN_SETUP(ObjectClearSfx, rc::fuzz::objectScratchDef)
RC_DECOMP_FN_SETUP_HEAP(ObjectInitObject, rc::fuzz::objectInitObject)
RC_DECOMP_FN_SETUP_HEAP(ObjectDelete, rc::fuzz::objectDelete)
RC_DECOMP_FN_SETUP_HEAP(ObjectDeleteAll, rc::fuzz::objectRace)
RC_DECOMP_FN_SETUP(ObjectInterpolateTint, rc::fuzz::objectScratchDef)
RC_DECOMP_FN_SETUP_HEAP(ObjectResetAllTint, rc::fuzz::objectRace)
RC_DECOMP_FN_SETUP_HEAP(ObjectRenderAll, rc::fuzz::objectRace)
RC_DECOMP_FN_SETUP_HEAP(ObjectSfxDoAll, rc::fuzz::objectSfxAll)
RC_DECOMP_FN_SETUP_HEAP(ObjectTestWaterEscape, rc::fuzz::objectWeaponList)
RC_DECOMP_FN_SETUP_HEAP(ObjectWeaponAffectRemove, rc::fuzz::objectWeaponList)
RC_DECOMP_FN_SETUP_HEAP(ObjectAllRemoveWeapons, rc::fuzz::objectWeapons)
RC_DECOMP_FN_SETUP_HEAP(ObjectGiveNextWeapon, rc::fuzz::objectWeapons)
RC_DECOMP_FN(ObjectMaterialSfx)
RC_DECOMP_FN_SETUP_HEAP(ObjectCreateCars, rc::fuzz::objectCreateCars)
RC_DECOMP_FN_SETUP_HEAP(ObjectWeaponAffect, rc::fuzz::objectAffect)
RC_DECOMP_FN_SETUP(ObjectAllocateEngineSfx, rc::fuzz::objectScratchDef)
RC_DECOMP_FN_SETUP_HEAP(ObjectCarChangeOver, rc::fuzz::objectChangeOver)
