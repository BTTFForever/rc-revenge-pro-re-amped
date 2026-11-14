// Decompiled from WEAPON.c: the weapons the cars fire. Each live weapon is an _sWeapon from weaponVarG's fragment pool,
// in the doubly linked list weaponVarG.pList, with a task that runs WeaponProcess every 25 ms until timeWhenEnds.
// WeaponSpawn fills in the lifetime and radius of a definition and WeaponCreate builds the weapon and calls its Setup;
// WeaponProcess calls its Update. Weapon numbers: 1 rocket, 2 battery, 3 fake (pickup), 4 shockwave, 5 water balloon,
// 6 oil, 7 lightning, 8 heat ray, 9 mega (blast), 10 invincible, 11 bouncy bomb, 12 sarge, 13 skull, 14 armoured,
// 15 dump tug (or its boat), 16 moon buggy. owner and target are indexes into objectVarG.list (0x7f: no target).
//
// The big per-weapon updates are in weapon2.cpp; the random-test setups in weapon_fuzz.cpp.

#include "weapon.h"
#include "weapon_fuzz.h"

namespace rc {

using namespace weapon;

// ---------------------------------------------------------------------------------------------------------------------
// Small helpers and the targeting functions.

int16 WeaponGetOwnerBitfield(int32 owner)
{
    return static_cast<uint32_t>(owner) < 8u ? static_cast<int16>(1 << owner) : 0;
}

void WeaponRocketLocked(ptr32<_sObject>) {}
void WeaponSkullFreeHexed(ptr32<_sWeapon>) {}

void WeaponDumptugWaterUpdate(ptr32<_sWeapon> pWeapon, tTime)
{
    pWeapon->oCol.updated = 1;
}

int16 WeaponTarget(ptr32<_sObject> pMe, ptr32<_sObject> pYou)
{
    const int32_t x = ee_ftoi(pMe->pos16.x - pYou->pos16.x);
    const int32_t z = ee_ftoi(pMe->pos16.z - pYou->pos16.z);
    GuestFrame frame(0x20);
    // The dead delta local (x, y, z at sp+0).
    *frame.local<int32_t>(0) = x;
    *frame.local<int32_t>(4) = ee_ftoi(pMe->pos16.y - pYou->pos16.y);
    *frame.local<int32_t>(8) = z;
    return static_cast<int16>(-static_cast<int32_t>(ArcTan(x, z)));
}

int16 WeaponRelativeAngle(ptr32<_sObject> pObject, ptr32<_sObject> pTarget)
{
    const int32_t x = ee_ftoi(pObject->pos16.x - pTarget->pos16.x);
    const int32_t y = ee_ftoi(pObject->pos16.y - pTarget->pos16.y);
    const int32_t z = ee_ftoi(pObject->pos16.z - pTarget->pos16.z);
    GuestFrame frame(0x50);
    *frame.local<int32_t>(0x10) = x;
    *frame.local<int32_t>(0x14) = y;
    *frame.local<int32_t>(0x18) = z;
    const int32_t toTarget = static_cast<int16>(-static_cast<int32_t>(ArcTan(x, z)));
    guest_call(kMat3nDecompose, ptr32<uint8>(pObject->pRendObj.address + 0x18u), frame.local<_matAngles>(0));
    const uint16_t head = static_cast<uint16_t>(*frame.local<uint16_t>(2) + 0x4000u);
    *frame.local<uint16_t>(2) = head;
    return static_cast<int16>(toTarget - head);
}

ptr32<_sObject> WeaponFindValidTarget(ptr32<_sObject> pObject)
{
    if (pObject->iRacePos == 0)
        return ptr32<_sObject>(nullptr);
    GuestFrame frame(0x50);
    for (int32_t pos = pObject->iRacePos - 1; pos >= 0; --pos)
    {
        const ptr32<_sObject> pTarget = at<ptr32<_sObject>>(kRaceOrder + static_cast<uint32_t>(pos) * kRaceOrderStride);
        if (WeaponRelativeAngle(pObject, pTarget) < 1)
        {
            if (0xfff < -static_cast<int32_t>(WeaponRelativeAngle(pObject, pTarget)))
                continue;
        }
        else if (0xfff < WeaponRelativeAngle(pObject, pTarget))
            continue;
        // A car targets cars, a boat boats.
        if ((objectType(pTarget) < kTypeFirstBoat) == (objectType(pObject) < kTypeFirstBoat))
            return pTarget;
    }
    return ptr32<_sObject>(nullptr);
}

void WeaponRocketTestLockon(ptr32<_sObject> pObject)
{
    GuestFrame frame(0x50);
    _sObject &me = *pObject;
    const ptr32<_sObject> pTarget = WeaponFindValidTarget(pObject);
    const auto stopHeldSfx = [&]
    {
        if (me.playerType < 2)
        {
            if (me.weaponsHeldSfx != -1)
                sfxStop(me.weaponsHeldSfx);
            me.weaponsHeldSfx = -1;
        }
    };
    if (!pTarget)
    {
        stopHeldSfx();
        if (me.weaponLock != 0)
            objectAt(me.weaponLock - 1)->lockOnMe = 0;
        me.weaponLock = 0;
        return;
    }
    if (me.weaponLock != 0 && objectAt(me.weaponLock - 1)->lockOnMe != ' ')
        objectAt(me.weaponLock - 1)->lockOnMe = 0;
    if (pTarget->carAI.iTotalRacedDist - me.carAI.iTotalRacedDist < 8000)
    {
        if (me.weaponLock == 0 && me.playerType < 2)
            me.weaponsHeldSfx = static_cast<int16>(guest_call<int32_t>(kSfxPlay, 0x1b, 0x80, 0xf, 0xf, 0xff, 0, 0));
        me.weaponLock = static_cast<uint8>(pTarget->playerNo + 1);
        pTarget->lockOnMe = static_cast<int8>(me.playerNo + 1);
    }
    else
    {
        stopHeldSfx();
        me.weaponLock = 0;
        pTarget->lockOnMe = 0;
    }
}

void WeaponRocketParanoiaCheck()
{
    sObjectClass &objects = G(objectVarG);
    for (uint32_t i = 0; i < objects.noObjects; ++i)
        if (objectAt(static_cast<int32_t>(i))->weaponType == 1)
            return;
    ptr32<_sWeapon> pWeapon = G(weaponVarG).pList;
    for (int32_t i = 0; i < G(weaponVarG).noWeapons; ++i)
    {
        if (pWeapon->no == 1)
            return;
        if (pWeapon->pNext)
            pWeapon = pWeapon->pNext;
    }
    for (uint32_t i = 0; i < objects.noObjects; ++i)
        objectAt(static_cast<int32_t>(i))->lockOnMe = 0;
}

void WeaponShockwaveShake(ptr32<_sWeapon> pWeapon, tTime)
{
    const int32_t owner = pWeapon->owner;
    if (owner >= 2)
        return;
    const uint16_t misc = static_cast<uint16_t>(pWeapon->misc);
    const int32_t amount = static_cast<int16_t>(misc);
    const uint32_t camera = kCameras + static_cast<uint32_t>(owner * 0x5b8);
    int32_t &y = at<int32_t>(camera + 0x3c);
    int32_t &z = at<int32_t>(camera + 0x40);
    if (misc & 1u)
    {
        y += amount;
        z += amount;
    }
    else
    {
        y -= amount;
        z -= amount;
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// The blasts that end with their weapon.

void WeaponLightningFreeBlast(ptr32<_sWeapon> pWeapon)
{
    GuestFrame frame(0x80);
    for (uint32_t i = 0; i < G(objectVarG).noObjects; ++i)
    {
        _sObject &car = *objectAt(static_cast<int32_t>(i));
        if (car.weaponAffectOwner != pWeapon->owner)
            continue;
        if (car.pVfxMisc)
        {
            VfxDelete(car.pVfxMisc);
            car.pVfxMisc = ptr32<_sVfx_Object>(nullptr);
        }
        sfxStop(car.lightningSfx);
        car.lightningSfx = -1;
        car.weaponAffect = 0;
        car.weaponAffectOwner = 0xff;
    }
}

void WeaponMegaFreeBlast(ptr32<_sWeapon>)
{
    for (uint32_t i = 0; i < G(objectVarG).noObjects; ++i)
    {
        _sObject &car = *objectAt(static_cast<int32_t>(i));
        if (car.weaponAffect == 9)
            car.weaponAffect = 0;
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// The weapons' life: init, spawn, create, the task, remove, destroy, term.

void WeaponInit()
{
    {
        GuestFrame frame(0x20);
        guest_call(kMemFragPoolCreate, ptr32<uint8>(kWeaponVars), 10, 0x14, 0x13c, 0);
    }
    sWeaponClass &vars = G(weaponVarG);
    vars.latestWeaponId = 0;
    vars.pList = ptr32<_sWeapon>(nullptr);
    vars.noWeapons = 0;
    vars.pDumperRendObjHead = ptr32<_sRendObjHead>(nullptr);
}

void WeaponDestroy(ptr32<_sWeapon> pWeapon)
{
    GuestFrame frame(0x40);
    sWeaponClass &vars = G(weaponVarG);
    _sWeapon &w = *pWeapon;
    const int16 no = w.no;
    if (no == 7 || no == 10 || no == 2 || no == 8 || no == 0xc || no == 0xd)
        objectAt(w.owner)->weaponFiring = 0;
    bool linked = false;
    if (w.pPrev)
    {
        if (!w.pNext)
            w.pPrev->pNext = ptr32<_sWeapon>(nullptr);
        else
        {
            w.pPrev->pNext = w.pNext;
            w.pNext->pPrev = w.pPrev;
        }
        linked = static_cast<bool>(w.pPrev);
    }
    if (!linked)
    {
        const ptr32<_sWeapon> head = vars.pList;
        if (!head->pNext)
        {
            head->pPrev = ptr32<_sWeapon>(nullptr);
            head->pNext = ptr32<_sWeapon>(nullptr);
            vars.pList = ptr32<_sWeapon>(nullptr);
        }
        else
        {
            const ptr32<_sWeapon> next = w.pNext;
            next->pPrev = ptr32<_sWeapon>(nullptr);
            vars.pList = next;
        }
    }
    if (w.oCol.type != 4)
        OColDeleteObject(ptr32<_sOColObj>(pWeapon.address + 0x28u));
    if (const ptr32<_sVfx_Object> fx = w.pVfx)
    {
        if (w.no == 1)
        {
            w.pVfx = ptr32<_sVfx_Object>(nullptr);
            fx->iLifeSpan = 0x1f;
        }
        else
        {
            VfxDelete(fx);
            w.pVfx = ptr32<_sVfx_Object>(nullptr);
        }
    }
    if (const ptr32<_sVfx_Object> fx = w.pVfx2)
    {
        if (fx->iType == 0x17)
        {
            w.pVfx2 = ptr32<_sVfx_Object>(nullptr);
            fx->iLifeSpan = 0xf;
        }
        else
        {
            VfxDelete(fx);
            w.pVfx2 = ptr32<_sVfx_Object>(nullptr);
        }
    }
    if (w.sfxHandle1 != -1)
        sfxStop(w.sfxHandle1);
    if (w.sfxHandle2 != -1)
        sfxStop(w.sfxHandle2);
    guest_call(kTaskDelete, ptr32<uint8>(pWeapon.address + 0x10u));
    guest_call(kMemFragDelete, vars.pPool, pWeapon);
    if (--vars.noWeapons == 0)
        vars.pList = ptr32<_sWeapon>(nullptr);
}

// The last weapon of the list: the head moved on noWeapons - 1 times.
static ptr32<_sWeapon> lastWeapon()
{
    ptr32<_sWeapon> p = G(weaponVarG).pList;
    for (int32_t i = G(weaponVarG).noWeapons - 1; i > 0; --i)
        p = p->pNext;
    return p;
}

void WeaponTerm()
{
    {
        GuestFrame frame(0x60);
        ptr32<_sWeapon> p = lastWeapon();
        const int32_t count = G(weaponVarG).noWeapons;
        for (int32_t i = 0; i < count; ++i)
        {
            p->sfxHandle1 = -1;
            p->sfxHandle2 = -1;
            const ptr32<_sWeapon> prev = p->pPrev;
            WeaponDestroy(p);
            p = prev;
        }
    }
    guest_call(kMemFragPoolDelete, ptr32<uint8>(kWeaponVars));
}

void WeaponRemoveAll()
{
    GuestFrame frame(0xa0);
    ptr32<_sWeapon> p = lastWeapon();
    const int32_t count = G(weaponVarG).noWeapons;
    for (int32_t n = 0; n < count; ++n)
    {
        const ptr32<_sWeapon> prev = p->pPrev;
        _sWeapon &w = *p;
        if (w.no == 7)
            for (uint32_t i = 0; i < G(objectVarG).noObjects; ++i)
            {
                _sObject &car = *objectAt(static_cast<int32_t>(i));
                if (car.weaponAffectOwner != w.owner)
                    continue;
                if (car.pVfxMisc)
                {
                    VfxDelete(car.pVfxMisc);
                    car.pVfxMisc = ptr32<_sVfx_Object>(nullptr);
                }
                sfxStop(car.lightningSfx);
                car.lightningSfx = -1;
                car.weaponAffectOwner = 0xff;
                car.weaponAffect = 0;
            }
        if (w.no == 9)
            for (uint32_t i = 0; i < G(objectVarG).noObjects; ++i)
            {
                _sObject &car = *objectAt(static_cast<int32_t>(i));
                if (car.weaponAffect == 9)
                    car.weaponAffect = 0;
            }
        if (w.no == 2)
            clearSkidOil(w.owner);
        if (w.no == 10)
            objectAt(w.owner)->invincible = 0;
        WeaponDestroy(p);
        p = prev;
    }
}

void WeaponRemove(ptr32<_sWeapon> pWeapon)
{
    _sWeapon &w = *pWeapon;
    const int16 no = w.no;
    if (static_cast<uint32_t>(static_cast<int32_t>(no)) > 0x10u)
        return;
    switch (no)
    {
    case 1:
    {
        bool paranoia = false;
        {
            GuestFrame frame(0x30);
            _sObject &owner = *objectAt(w.owner);
            if (owner.weaponsHeld == 0 || owner.weaponsHeldType != 1)
                paranoia = true;
            else
                owner.weaponType = 1;
            if (w.target != 0x7f)
                objectAt(w.target)->lockOnMe = 0;
            WeaponDestroy(pWeapon);
        }
        if (paranoia)
            WeaponRocketParanoiaCheck();
        return;
    }
    case 2:
        clearSkidOil(w.owner);
        break;
    case 7:
    {
        GuestFrame frame(0x30);
        WeaponLightningFreeBlast(pWeapon);
        break;
    }
    case 8:
        objectAt(w.owner)->aiFire = 0;
        break;
    case 9:
    {
        GuestFrame frame(0x30);
        WeaponMegaFreeBlast(pWeapon);
        break;
    }
    case 10:
        objectAt(w.owner)->invincible = 0;
        w.pVfx = ptr32<_sVfx_Object>(nullptr);
        break;
    case 0xb:
    {
        GuestFrame frame(0x30);
        _sObject &owner = *objectAt(w.owner);
        if (owner.weaponsHeld != 0 && owner.weaponsHeldType == 0xb)
            owner.weaponType = 0xb;
        VfxAddNewPlace(ee_ftoi(w.pos16.x), ee_ftoi(w.pos16.y), ee_ftoi(w.pos16.z), 9);
        break;
    }
    case 0xc:
    {
        GuestFrame frame(0x30);
        if (const ptr32<_sVfx_Object> fx = objectAt(w.owner)->pVfxSarge)
        {
            VfxDelete(fx);
            objectAt(w.owner)->pVfxSarge = ptr32<_sVfx_Object>(nullptr);
        }
        break;
    }
    case 0xf:
    {
        _sObject &owner = *objectAt(w.owner);
        if (owner.weaponsHeld != 0 && owner.weaponsHeldType == 0xf)
            owner.weaponType = 0xf;
        break;
    }
    case 0:
        return;
    default: // 3 to 6, 13, 14, 16
        break;
    }
    WeaponDestroy(pWeapon);
}

void WeaponProcess(ptr32<void> pData, tTime time)
{
    const ptr32<_sWeapon> pWeapon(pData.address);
    _sWeapon &w = *pWeapon;
    bool ended;
    {
        GuestFrame frame(0x30);
        switch (static_cast<int16_t>(w.no - 1))
        {
        case 0: WeaponRocketUpdate(pWeapon); break;
        case 1: WeaponBatteryUpdate(pWeapon, time); break;
        case 2: WeaponFakeUpdate(pWeapon, time); break;
        case 3: WeaponShockwaveUpdate(pWeapon, time); break;
        case 4: WeaponWaterUpdate(pWeapon, time); break;
        case 6: WeaponLightningUpdate(pWeapon, time); break;
        case 7: WeaponHeatrayUpdate(pWeapon, time); break;
        case 9: WeaponInvincibleUpdate(pWeapon, time); break;
        case 10: WeaponBouncyUpdate(pWeapon, time); break;
        case 11: WeaponSargeUpdate(pWeapon, time); break;
        case 12: WeaponSkullUpdate(pWeapon, time); break;
        case 13: WeaponArmouredUpdate(pWeapon, time); break;
        case 14:
            if (static_cast<uint32_t>(objectType(objectAt(w.owner))) < kTypeFirstBoat)
                WeaponDumptugUpdate(pWeapon, time);
            else
                WeaponDumptugWaterUpdate(pWeapon, time);
            break;
        case 15: WeaponMoonbuggyUpdate(pWeapon, time); break;
        default: break;
        }
        ended = w.timeWhenEnds < time;
        if (ended)
        {
            if (w.target != 0x7f && w.no != 7)
            {
                guest_call(kObjectWeaponAffectRemove, objectAt(w.target));
                objectAt(w.target)->weaponAffect = 0;
            }
        }
    }
    if (ended)
    {
        WeaponRemove(pWeapon);
        return;
    }
    w.task.time = time + 0x19;
    guest_call(kTaskCachedInsert, ptr32<uint8>(pWeapon.address + 0x10u));
}

ptr32<_sWeapon> WeaponCreate(ptr32<vec16_> pPos, tTime time, ptr32<_sWeaponDef> pDef)
{
    GuestFrame frame(0xa0);
    sWeaponClass &vars = G(weaponVarG);
    _sWeaponDef &def = *pDef;
    const ptr32<_sWeapon> pWeapon(guest_call<uint32_t>(kMemFragCreate, vars.pPool));
    _sWeapon &w = *pWeapon;
    const int16 owner = def.owner;
    w.uniqueId = vars.latestWeaponId;
    vars.latestWeaponId = vars.latestWeaponId + 1;
    w.otherTimer = 0;
    w.timeWhenEnds = time + def.lifetime;
    w.pos16.x = ee_itof(pPos->x);
    w.pos16.y = ee_itof(pPos->y);
    w.pos16.z = ee_itof(pPos->z);
    w.owner = owner;
    w.misc = 0;
    const ee_float rad = ee_itof(def.rad);
    w.status = 0;
    w.aColrad = rad;
    w.no = def.no;
    w.oColrad = rad;
    w.pAINode = ptr32<_sAINode>(nullptr);
    w.target = def.target;
    w.sfxHandle2 = -1;
    w.specialFire = def.specialFire;
    w.oCol.type = 4;
    w.destPos16.x = 0;
    w.destPos16.y = 0;
    w.destPos16.z = 0;
    w.sfxHandle1 = -1;
    w.rendObj.pRendObjHead = ptr32<_sRendObjHead>(nullptr);
    w.pVfx2 = ptr32<_sVfx_Object>(nullptr);
    w.pVfx = def.pVfx;
    uint8_t vibLeft = 0, vibRight = 0;
    switch (static_cast<int16_t>(def.no - 1))
    {
    case 0: WeaponRocketSetup(pWeapon); vibLeft = 0x1e; vibRight = 0xa; break;
    case 1: WeaponBatterySetup(pWeapon, time); vibLeft = 0x14; vibRight = 0xe; break;
    case 2: WeaponFakeSetup(pWeapon, time); vibLeft = 0x14; vibRight = 6; break;
    case 3: WeaponShockwaveSetup(pWeapon, time); vibLeft = 0x23; vibRight = 0xc; break;
    case 4: WeaponWaterSetup(pWeapon, time); vibLeft = 0x14; vibRight = 6; break;
    case 5: WeaponOilSetup(pWeapon); vibLeft = 0x14; vibRight = 6; break;
    case 6: WeaponLightningSetup(pWeapon, time); vibLeft = 0x24; vibRight = 8; break;
    case 7: WeaponHeatraySetup(pWeapon, time); vibLeft = 0x14; vibRight = 6; break;
    case 8: WeaponMegaSetup(pWeapon, time); vibLeft = 0x1e; vibRight = 9; break;
    case 9: WeaponInvincibleSetup(pWeapon, time); vibLeft = 0x14; vibRight = 0xe; break;
    case 10: WeaponBouncySetup(pWeapon, time); vibLeft = 0x19; vibRight = 7; break;
    case 11: WeaponSargeSetup(pWeapon, time); vibLeft = 0x19; vibRight = 7; break;
    case 12: WeaponSkullSetup(pWeapon, time); vibLeft = 0x19; vibRight = 7; break;
    case 13: WeaponArmouredSetup(pWeapon, time); vibLeft = 0x19; vibRight = 7; break;
    case 14:
        if (static_cast<uint32_t>(objectType(objectAt(def.owner))) < kTypeFirstBoat)
            WeaponDumptugSetup(pWeapon, time);
        else
            WeaponDumptugWaterSetup(pWeapon, time);
        vibLeft = 0x19;
        vibRight = 7;
        break;
    case 15: WeaponMoonbuggySetup(pWeapon, time); vibLeft = 0x19; vibRight = 7; break;
    default: break;
    }
    const uint8 noHuman = G(mainVarG).noHuman;
    if (noHuman != 0 && def.owner < static_cast<int32_t>(noHuman))
        PlayerSetVibrate(vibLeft, vibRight, static_cast<uint8>(def.owner));
    w.pNext = ptr32<_sWeapon>(nullptr);
    w.pPrev = ptr32<_sWeapon>(nullptr);
    if (vars.noWeapons == 0)
    {
        vars.pList = pWeapon;
        w.pPrev = ptr32<_sWeapon>(nullptr);
    }
    else
    {
        ptr32<_sWeapon> last = vars.pList;
        while (last->pNext)
            last = last->pNext;
        last->pNext = pWeapon;
        w.pPrev = last;
    }
    const ptr32<uint8> task(pWeapon.address + 0x10u);
    guest_call(kTaskSet, task, kWeaponProcessAddr, pWeapon, time);
    guest_call(kTaskCachedInsert, task);
    vars.noWeapons = vars.noWeapons + 1;
    const uint16_t no = static_cast<uint16_t>(w.no);
    const int16_t sno = static_cast<int16_t>(no);
    objectAt(def.owner)->weaponFiring =
        (static_cast<uint16_t>(no - 7) < 2u || sno == 10 || sno == 2 || sno == 0xc || sno == 0xd) ? 1 : 0;
    if (def.no == 6)
        return ptr32<_sWeapon>(6u);
    _sObject &ownerCar = *objectAt(def.owner);
    const uint32_t held = static_cast<uint8_t>(ownerCar.weaponsHeld) - 1u;
    ownerCar.weaponsHeld = static_cast<int8>(held);
    return ptr32<_sWeapon>(held);
}

void WeaponSpawn(ptr32<vec16_> pPos, ptr32<_sWeaponDef> pDef, tTime time)
{
    _sWeaponDef &def = *pDef;
    def.pVfx = ptr32<_sVfx_Object>(nullptr);
    const int16 no = def.no;
    if (static_cast<uint32_t>(static_cast<int32_t>(no)) >= 0x14u)
        return;
    switch (no)
    {
    case 1: def.lifetime = 8000; def.rad = 0x1e; break;
    case 2: def.target = 0; def.lifetime = 5000; break;
    case 3: def.lifetime = 20000; def.rad = 0x24; break;
    case 4:
    {
        def.lifetime = 1000;
        GuestFrame frame(0x40);
        def.pVfx = VfxAddNew(objectAt(def.owner), 0, 0, 0x18);
        break;
    }
    case 5: def.lifetime = 3000; def.rad = 0x32; break;
    case 6:
    {
        GuestFrame frame(0x40);
        _sObject &owner = *objectAt(def.owner);
        guest_call<int32_t>(kRevSfxPlay3D, 0x19, 0xff, 0x100, ptr32<uint8>(owner.pRendObj.address + 4u), (owner.playerType + 4) | 0x40, 5000);
        const ptr32<_sVfx_Object> fx = VfxAddNew(objectAt(def.owner), 0, 0, 3);
        objectAt(def.owner)->pVfxOilSquirt = fx;
        return;
    }
    case 7: def.lifetime = 5000; break;
    case 8: def.lifetime = 4000; break;
    case 9: def.lifetime = 3000; break;
    case 10: def.target = 0; def.lifetime = 10000; break;
    case 0xb: def.lifetime = 8000; def.rad = 0x3c; break;
    case 0xc: def.lifetime = 6000; break;
    case 0xd: def.lifetime = 3000; break;
    case 0xe: def.lifetime = 4000; break;
    case 0xf: def.lifetime = 10000; break;
    case 0x10: def.lifetime = 4000; break;
    case 0x11: def.no = 10; def.lifetime = 10000; def.target = 1; break;
    case 0x12: def.no = 2; def.lifetime = 8000; def.target = 1; break;
    case 0x13: def.lifetime = 10000; def.no = 2; def.target = 2; break;
    default: return; // 0
    }
    WeaponCreate(pPos, time, pDef);
}

// ---------------------------------------------------------------------------------------------------------------------
// Drawing: the fake pickups (3) and the dump tug's rock (15); the rest are effects.

void WeaponRenderAll(int32 split)
{
    GuestFrame frame(0xe0);
    const ptr32<sRdrStateBlock> fakeBlock = frame.local<sRdrStateBlock>(0);
    const ptr32<sRdrStateBlock> rockBlock = frame.local<sRdrStateBlock>(0x30);
    ptr32<_sWeapon> p = G(weaponVarG).pList;
    for (int32_t count = 0; count < G(weaponVarG).noWeapons; ++count, p = p->pNext)
    {
        _sWeapon &w = *p;
        const ptr32<_sRendObj> rendObj(p.address + 0xc4u);
        if (w.no == 1)
            continue;
        if (w.no == 3)
        {
            RdrState_SetStateBlockToDefaults(fakeBlock);
            RdrState_SetAlphaBlending(fakeBlock, 1, 2, 2, 0, 0x7f);
            RdrState_SetPixelAlphaBlendControlEnable(fakeBlock, 0);
            RdrState_SetZBufferWriteEnable(fakeBlock, 0);
            RdrState_ForceApplyStateBlock(fakeBlock);
            if (w.oCol.type == 4)
            {
                int16 misc = w.misc;
                if (split != 0)
                {
                    w.misc = misc == 0 ? 1 : 0;
                    misc = w.misc;
                }
                if (misc == 0 && G(mainVarG).pauseMenu == 0)
                    w.misc = 1;
                else
                {
                    RendObjRender(rendObj, ptr32<SVECTOR>(nullptr), ptr32<SVECTOR>(nullptr), 0);
                    w.misc = 0;
                }
            }
            else
                RendObjRender(rendObj, ptr32<SVECTOR>(nullptr), ptr32<SVECTOR>(nullptr), 0);
            RdrState_SetStateBlockToDefaults(fakeBlock);
            RdrState_ForceApplyStateBlock(fakeBlock);
        }
        else if (w.no == 0xf && w.status != 3)
        {
            RdrState_SetStateBlockToDefaults(rockBlock);
            RdrState_ForceApplyStateBlock(rockBlock);
            RendObjRender(rendObj, ptr32<SVECTOR>(nullptr), ptr32<SVECTOR>(nullptr), 0);
        }
    }
}

} // namespace rc

RC_DECOMP_FN(WeaponGetOwnerBitfield, 0x181ce0)
RC_DECOMP_FN(WeaponRocketLocked, 0x181368)
RC_DECOMP_FN(WeaponSkullFreeHexed, 0x181d48)
RC_DECOMP_FN_SETUP(WeaponDumptugWaterUpdate, 0x181bd0, rc::fuzz::weaponOnly)
RC_DECOMP_FN_SETUP(WeaponTarget, 0x181248, rc::fuzz::weaponTwoCars)
RC_DECOMP_FN_SETUP(WeaponRelativeAngle, 0x1812b8, rc::fuzz::weaponTwoCars)
RC_DECOMP_FN_SETUP_HEAP(WeaponFindValidTarget, 0x181370, rc::fuzz::weaponOneCar)
RC_DECOMP_FN_SETUP_HEAP(WeaponRocketTestLockon, 0x17bf38, rc::fuzz::weaponOneCar)
RC_DECOMP_FN_SETUP_HEAP(WeaponRocketParanoiaCheck, 0x181468, rc::fuzz::weaponList)
RC_DECOMP_FN_SETUP(WeaponShockwaveShake, 0x181788, rc::fuzz::weaponShake)
RC_DECOMP_FN_SETUP_HEAP(WeaponLightningFreeBlast, 0x181888, rc::fuzz::weaponBlast)
RC_DECOMP_FN_SETUP_HEAP(WeaponMegaFreeBlast, 0x181a60, rc::fuzz::weaponBlast)
RC_DECOMP_FN_SETUP_HEAP(WeaponInit, 0x1811f8, rc::fuzz::weaponList)
RC_DECOMP_FN_SETUP_HEAP(WeaponDestroy, 0x17b518, rc::fuzz::weaponInList)
RC_DECOMP_FN_SETUP_HEAP(WeaponTerm, 0x17b6d0, rc::fuzz::weaponList)
RC_DECOMP_FN_SETUP_HEAP(WeaponRemoveAll, 0x180ed8, rc::fuzz::weaponList)
RC_DECOMP_FN_SETUP_HEAP(WeaponRemove, 0x17ad48, rc::fuzz::weaponInList)
RC_DECOMP_FN_SETUP_HEAP(WeaponProcess, 0x17a6b8, rc::fuzz::weaponInList)
RC_DECOMP_FN_SETUP_HEAP(WeaponCreate, 0x17b098, rc::fuzz::weaponCreate)
RC_DECOMP_FN_SETUP_HEAP(WeaponSpawn, 0x17a8d0, rc::fuzz::weaponSpawn)
RC_DECOMP_FN_SETUP_HEAP(WeaponRenderAll, 0x17c098, rc::fuzz::weaponRender)
