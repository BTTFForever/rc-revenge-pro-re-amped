// WEAPON.c, continued: the weapons' Setups, the short Updates, a weapon hitting a car and the oil slick's collision.
// See weapon.cpp.

#include "weapon.h"
#include "weapon_fuzz.h"

#include <cstring>

namespace rc {

using namespace weapon;

namespace {

const ee_float k256 = 256.0f, k40 = 40.0f, k50 = 50.0f, k25 = 25.0f, k60 = 60.0f, k10 = 10.0f, k20 = 20.0f, kZero = 0.0f;
const ee_float k6000 = 6000.0f, k1024 = 1024.0f;

int32_t toFixed(ee_float v) { return ee_ftoi(v * k256); }

// (c * k) / 4096, rounded towards zero, as the weapons scale a 4.12 direction.
int32_t scale12(int16_t c, int32_t k) { return (static_cast<int32_t>(c) * k) / 4096; }

// NormalMulMatPsx(&n, &car->pRendObj->matrix, &r) with n and r the caller's frame locals at nOffset and rOffset.
normal_ turnByCar(const GuestFrame &frame, uint32_t nOffset, uint32_t rOffset, int16_t x, int16_t y, int16_t z, ptr32<_sObject> car)
{
    normal_ &n = *frame.local<normal_>(nOffset);
    n.x = x;
    n.y = y;
    n.z = z;
    guest_call(kNormalMulMatPsx, frame.local<normal_>(nOffset), ptr32<uint8>(car->pRendObj.address + 0x18u), frame.local<normal_>(rOffset));
    return *frame.local<normal_>(rOffset);
}

// The rocket's and water balloon's start: aerial point 5 of the owner's car.
void aerialPoint(_sWeapon &w)
{
    const uint64_t sv = VfxCarAerialVal(objectAt(w.owner), 5);
    w.pos16.x = ee_itof(static_cast<int16_t>(sv));
    w.pos16.y = ee_itof(static_cast<int16_t>(sv >> 16));
    w.pos16.z = ee_itof(static_cast<int16_t>(sv >> 32));
}

void initOcol(ptr32<_sWeapon> p)
{
    OColInitObj(ptr32<_sOColObj>(p.address + 0x28u), ptr32<_sColCellVecPSX>(p.address + 0x94u), ptr32<ee_float>(p.address + 0xb4u), ptr32<void>(p.address));
}

void deleteOcol(ptr32<_sWeapon> p)
{
    OColDeleteObject(ptr32<_sOColObj>(p.address + 0x28u));
    p->oCol.type = 4;
}

// The weapon's position as 1/256 units in a frame local (pos32).
uint32_t pos32Local(const GuestFrame &frame, uint32_t offset, const _sWeapon &w)
{
    vec32_ &v = *frame.local<vec32_>(offset);
    v.x = toFixed(w.pos16.x);
    v.y = toFixed(w.pos16.y);
    v.z = toFixed(w.pos16.z);
    return frame.local<vec32_>(offset).address;
}

// The owner may fire this kind again when it has more of them.
void rearm(int32_t owner, int8_t kind)
{
    _sObject &car = *objectAt(owner);
    if (car.weaponsHeld != 0 && car.weaponsHeldType == kind)
        car.weaponType = static_cast<uint8>(kind);
}

// Copies the owner's integer velocity (vel >> 9) onto the throw (water balloon, armoured car).
void addCarVelocity(_sWeapon &w, ptr32<_sObject> car)
{
    const _sOPhysLibIntegerCarState &state = *car->pPhysics->pInteger;
    w.destPos16.x = static_cast<int16>(w.destPos16.x + (state.vel.x >> 9));
    w.destPos16.z = static_cast<int16>(w.destPos16.z + (state.vel.z >> 9));
}

} // namespace

// ---------------------------------------------------------------------------------------------------------------------
// Setups.

void WeaponRocketSetup(ptr32<_sWeapon> pWeapon)
{
    GuestFrame frame(0xc0);
    _sWeapon &w = *pWeapon;
    std::memset(frame.local<uint8>(0).get(), 0, 6); // temp
    aerialPoint(w);
    _sObject &owner = *objectAt(w.owner);
    owner.pVfx_Aerial->sTwang = 300;
    if (owner.weaponLock == 0)
        w.target = 0x7f;
    else
    {
        const uint8_t lock = owner.weaponLock;
        w.target = static_cast<int16>(lock - 1);
        if (w.specialFire == 1)
        {
            const ptr32<_sObject> behind = owner.carAI.pNearestObjBehind;
            if (!behind)
            {
                objectAt(lock - 1)->lockOnMe = 0;
                objectAt(w.owner)->weaponLock = 0;
                w.target = 0x7f;
            }
            else
            {
                const uint8_t no = behind->playerNo;
                w.target = no;
                objectAt(no)->lockOnMe = static_cast<int8>(owner.playerNo + 1);
                objectAt(w.owner)->weaponLock = static_cast<uint8>(objectAt(w.target)->playerNo + 1);
            }
        }
    }
    objectAt(w.owner)->weaponLock = 0;
    const int16 held = objectAt(w.owner)->weaponsHeldSfx;
    if (held != -1)
    {
        sfxStop(held);
        objectAt(w.owner)->weaponsHeldSfx = -1;
    }
    if (w.target != 0x7f)
        objectAt(w.target)->lockOnMe = ' ';
    w.oCol.type = 3;
    initOcol(pWeapon);
    w.aColrad = w.oColrad;
    MatPsxInit(ptr32<_matPsx>(pWeapon.address + 0xdcu));
    w.rendObj.pRendObjHead = G(pickupVarG).rendObj.pRendObjHead;
    w.rendObj.pos.x = toFixed(w.pos16.x);
    w.rendObj.pos.z = toFixed(w.pos16.z);
    w.rendObj.pos.y = ee_ftoi((w.pos16.y - k40) * k256);
    w.rendObj.angles.pitch = 0;
    w.rendObj.angles.head = 0;
    w.rendObj.angles.roll = 0;
    guest_call(kMat3nBuildXYZ, ptr32<uint8>(pWeapon.address + 0xdcu), ptr32<uint8>(pWeapon.address + 0xd4u));
    if (w.target == 0x7f)
    {
        const normal_ r = turnByCar(frame, 0x30, 0x40, 0, 0, static_cast<int16_t>(w.specialFire == 1 ? -0x1000 : 0x1000), objectAt(w.owner));
        const ptr32<_sObject> car = objectAt(w.owner);
        w.destPos16.x = static_cast<int16>(-r.x);
        w.destPos16.y = objectType(car) < 0x10u ? static_cast<int16>(-r.y) : 0;
        w.destPos16.z = static_cast<int16>(-r.z);
    }
    else
    {
        const ptr32<_sObject> target = objectAt(w.target);
        const ptr32<_sAINode> node = objectAt(w.owner)->carAI.pCurNode;
        w.pAINode = node;
        if (node == target->carAI.pCurNode)
            w.status = 1;
        else
        {
            w.pAINode = w.specialFire == 0 ? node->next[0] : node->prev[0];
            const _sAINode &next = *w.pAINode;
            w.destPos16.x = next.centre.vx;
            w.destPos16.y = next.centre.vy;
            w.destPos16.z = next.centre.vz;
        }
    }
    w.pVfx = VfxAddNewPlace(ee_ftoi(w.pos16.x), ee_ftoi(w.pos16.y), ee_ftoi(w.pos16.z), 0x1e);
    w.sfxHandle1 = sfx3D(0x1c, 0xff, 0x100, pWeapon.address + 0xc8u, carSfxFlags(objectAt(w.owner)), 5000);
}

void WeaponFakeSetup(ptr32<_sWeapon> pWeapon, tTime time)
{
    GuestFrame frame(0xc0);
    _sWeapon &w = *pWeapon;
    std::memset(frame.local<uint8>(0).get(), 0, 6); // temp
    vec32_ &offset = *frame.local<vec32_>(0x20);
    offset.x = 0;
    offset.y = 0x1e;
    offset.z = -150;
    const ptr32<_sObject> owner = objectAt(w.owner);
    Vec32MulMatPsx(frame.local<vec32_>(0x20), ptr32<_matPsx>(owner->pRendObj.address + 0x18u), frame.local<vec32_>(0x30));
    const vec32_ &moved = *frame.local<vec32_>(0x30);
    w.otherTimer = time + 0x5dc;
    w.pos16.x = w.pos16.x + ee_itof(moved.x);
    w.pos16.z = w.pos16.z + ee_itof(moved.z);
    w.pos16.y = w.pos16.y + ee_itof(moved.y + 0x32);
    MatPsxInit(ptr32<_matPsx>(pWeapon.address + 0xdcu));
    w.rendObj.pRendObjHead = G(pickupVarG).rendObj.pRendObjHead;
    w.rendObj.pos.x = toFixed(w.pos16.x);
    w.rendObj.pos.y = toFixed(w.pos16.y);
    w.rendObj.pos.z = toFixed(w.pos16.z);
    w.rendObj.angles.pitch = 0;
    w.rendObj.angles.head = 0;
    w.rendObj.angles.roll = 0;
    guest_call(kMat3nBuildXYZ, ptr32<uint8>(pWeapon.address + 0xdcu), ptr32<uint8>(pWeapon.address + 0xd4u));
    const normal_ r = turnByCar(frame, 0x40, 0x50, 0, -1000, -0x1000, objectAt(w.owner));
    w.status = 0;
    w.destPos16.x = static_cast<int16>(scale12(r.x, 0xf));
    w.destPos16.y = static_cast<int16>(scale12(r.y, 0xe) + 0xe);
    w.destPos16.z = static_cast<int16>(scale12(r.z, 0xf));
    sfx3D(0x14, 0xff, 0x100, pWeapon.address + 0xc8u, carSfxFlags(owner), 5000);
}

void WeaponShockwaveSetup(ptr32<_sWeapon> pWeapon, tTime time)
{
    GuestFrame frame(0x90);
    _sWeapon &w = *pWeapon;
    const uint32_t pos = pos32Local(frame, 0x10, w);
    for (uint32_t i = 0; i < G(objectVarG).noObjects; ++i)
    {
        _sObject &car = *objectAt(static_cast<int32_t>(i));
        const _sObject &owner = *objectAt(w.owner);
        vec32_ &d = *frame.local<vec32_>(0);
        d.y = 0;
        d.x = ee_ftoi(car.pos16.x - owner.pos16.x);
        d.z = ee_ftoi(car.pos16.z - owner.pos16.z);
        car.shockWaved = static_cast<int32_t>(guest_call<uint32_t>(kVec32Length, frame.local<vec32_>(0))) < 0x1389 ? 1 : 0;
        if (car.playerNo == w.owner)
            car.shockWaved = 0;
    }
    w.misc = 0x19;
    w.otherTimer = time + 0x4b;
    w.status = 0;
    sfx3D(0x1d, 0xff, 0x100, pos, carSfxFlags(objectAt(w.owner)), 5000);
}

void WeaponWaterSetup(ptr32<_sWeapon> pWeapon, tTime time)
{
    GuestFrame frame(0xa0);
    _sWeapon &w = *pWeapon;
    std::memset(frame.local<uint8>(0).get(), 0, 6); // temp
    aerialPoint(w);
    w.oCol.type = 3;
    initOcol(pWeapon);
    objectAt(w.owner)->pVfx_Aerial->sTwang = 300;
    w.pVfx = VfxAddNewPlace(ee_ftoi(w.pos16.x), ee_ftoi(w.pos16.y), ee_ftoi(w.pos16.z), 0x1d);
    w.target = 0x7f;
    const normal_ r = turnByCar(frame, 0x30, 0x40, 0, 0, 0x1000, objectAt(w.owner));
    const ptr32<_sObject> owner = objectAt(w.owner);
    w.destPos16.y = static_cast<int16>(scale12(r.y, 0x19) + 0x19);
    w.destPos16.x = static_cast<int16>(scale12(r.x, 0x32));
    w.destPos16.z = static_cast<int16>(scale12(r.z, 0x32));
    addCarVelocity(w, owner);
    const uint32_t pos = pos32Local(frame, 0x10, w);
    w.aColrad = k25;
    sfx3D(0x20, 0xff, 0x100, pos, carSfxFlags(owner), 5000);
    w.misc = 0;
    w.otherTimer = time + 0xfa;
}

void WeaponMegaSetup(ptr32<_sWeapon> pWeapon, tTime)
{
    GuestFrame frame(0x10);
    G(visFxVarG).iSkyTerror = 1;
    VfxAddNew(objectAt(pWeapon->owner), 0, 0, 0x13);
    guest_call<int32_t>(kSfxPlay, 0x1e, 0xff, 0xf, 0xf, 0xff, 0, 0);
}

void WeaponHeatraySetup(ptr32<_sWeapon> pWeapon, tTime)
{
    GuestFrame frame(0x40);
    _sWeapon &w = *pWeapon;
    const ptr32<_sObject> car = objectAt(w.owner);
    vec32_ &pos = *frame.local<vec32_>(0);
    pos.x = toFixed(car->pos16.x);
    pos.y = toFixed(car->pos16.y);
    pos.z = toFixed(car->pos16.z);
    w.pVfx = VfxAddNew(car, 0, 0, 5);
    w.sfxHandle1 = sfx3D(0x16, 0xff, 0x100, frame.local<vec32_>(0).address, carSfxFlags(objectAt(w.owner)), 5000);
    objectAt(w.owner)->heatrayStore = 4000;
}

void WeaponInvincibleSetup(ptr32<_sWeapon> pWeapon, tTime time)
{
    GuestFrame frame(0x50);
    _sWeapon &w = *pWeapon;
    int8 level;
    if (w.target == 0)
    {
        level = 1;
        w.status = 0;
    }
    else
    {
        w.status = 1;
        level = 2;
    }
    objectAt(w.owner)->invincible = level;
    const uint32_t pos = pos32Local(frame, 0, w);
    const int16 handle = sfx3D(0x17, 0xa4, 0x100, pos, carSfxFlags(objectAt(w.owner)), 5000);
    w.otherTimer = time + 8000;
    w.sfxHandle1 = handle;
    w.target = 0x7f;
    if (objectAt(w.owner)->weaponAffect != 0)
    {
        guest_call(kObjectWeaponAffectRemove, objectAt(w.owner));
        objectAt(w.owner)->weaponAffect = 0;
    }
}

void WeaponBouncySetup(ptr32<_sWeapon> pWeapon, tTime time)
{
    GuestFrame frame(0x80);
    _sWeapon &w = *pWeapon;
    std::memset(frame.local<uint8>(0).get(), 0, 6); // temp
    w.oCol.type = 3;
    w.target = 0x7f;
    if (objectType(objectAt(w.owner)) > 0xfu)
        w.pos16.y = w.pos16.y + k50;
    w.oColrad = k40;
    initOcol(pWeapon);
    w.aColrad = k60;
    w.pVfx = VfxAddNewPlace(ee_ftoi(w.pos16.x), ee_ftoi(w.pos16.y), ee_ftoi(w.pos16.z), 0x21);
    const normal_ r = turnByCar(frame, 0x10, 0x20, 0, -1000, static_cast<int16_t>(w.specialFire == 1 ? 0x1000 : -0x1000), objectAt(w.owner));
    const int16 special = w.specialFire;
    const int16 x = static_cast<int16>(scale12(r.x, 0x1e)), z = static_cast<int16>(scale12(r.z, 0x1e));
    w.destPos16.x = x;
    w.destPos16.y = static_cast<int16>(scale12(r.y, 0x1c) + 0x1c);
    w.destPos16.z = z;
    const _sOPhysLibIntegerCarState &state = *objectAt(w.owner)->pPhysics->pInteger;
    if (special == 0)
    {
        w.destPos16.x = static_cast<int16>(x + (state.vel.x >> 9));
        w.destPos16.z = static_cast<int16>(z + (state.vel.z >> 9));
    }
    else
    {
        w.destPos16.x = static_cast<int16>(x + static_cast<int16_t>(static_cast<uint32_t>((static_cast<int32_t>(state.vel.x) << 1) / 3) >> 8));
        w.destPos16.z = static_cast<int16>(z + static_cast<int16_t>(static_cast<uint32_t>((static_cast<int32_t>(state.vel.z) << 1) / 3) >> 8));
    }
    vec16_ &vel = *frame.local<vec16_>(0x30);
    vel.x = w.destPos16.x;
    vel.z = w.destPos16.z;
    w.status = 0;
    w.misc = 0;
    vel.y = 0;
    w.specialFire = static_cast<int16>(Vec16Magnitude(frame.local<vec16_>(0x30)));
    w.otherTimer = objectAt(w.owner)->weaponsHeld == 0 ? 0 : time + 1000;
}

void WeaponSargeSetup(ptr32<_sWeapon> pWeapon, tTime)
{
    GuestFrame frame(0x30);
    _sWeapon &w = *pWeapon;
    if (!objectAt(w.owner)->pVfxSarge)
    {
        const ptr32<_sVfx_Object> fx = VfxAddNew(objectAt(w.owner), 0, 0, 0x19);
        objectAt(w.owner)->pVfxSarge = fx;
    }
    const ptr32<_sObject> car = objectAt(w.owner);
    w.sfxHandle1 = sfx3D(0x34, 0x100, 0x100, carPos(car), carSfxFlags(car), 5000);
    objectAt(w.owner)->pPhysics->powerUpCounter = 0x115;
    w.target = 0x7f;
}

void WeaponSkullSetup(ptr32<_sWeapon> pWeapon, tTime time)
{
    {
        GuestFrame frame(0x30);
        const ptr32<_sObject> car = objectAt(pWeapon->owner);
        sfx3D(0x35, 0x100, 0x60, carPos(car), carSfxFlags(car), 5000);
    }
    pWeapon->status = time + 1000;
    pWeapon->target = 0x7f;
    pWeapon->misc = 0;
}

void WeaponArmouredSetup(ptr32<_sWeapon> pWeapon, tTime time)
{
    GuestFrame frame(0x70);
    _sWeapon &w = *pWeapon;
    w.oCol.type = 3;
    w.oColrad = k50;
    w.pos16.y = w.pos16.y + k50;
    initOcol(pWeapon);
    w.pVfx = VfxAddNewPlace(ee_ftoi(w.pos16.x), ee_ftoi(w.pos16.y), ee_ftoi(w.pos16.z), 0x33);
    w.target = 0x7f;
    const normal_ r = turnByCar(frame, 0x10, 0x20, 0, 0, 0x1000, objectAt(w.owner));
    const ptr32<_sObject> owner = objectAt(w.owner);
    w.destPos16.y = static_cast<int16>(scale12(r.y, 0x19) + 0x19);
    w.destPos16.x = static_cast<int16>(scale12(r.x, 100));
    w.destPos16.z = static_cast<int16>(scale12(r.z, 100));
    addCarVelocity(w, owner);
    const uint32_t pos = pos32Local(frame, 0, w);
    w.aColrad = k25;
    sfx3D(0x5c, 0xff, 0x100, pos, carSfxFlags(owner), 5000);
    w.misc = 0;
    w.otherTimer = time + 0xfa;
}

void WeaponDumptugSetup(ptr32<_sWeapon> pWeapon, tTime time)
{
    GuestFrame frame(0x90);
    _sWeapon &w = *pWeapon;
    vec32_ &offset = *frame.local<vec32_>(0);
    offset.x = 0;
    offset.y = 0;
    offset.z = -0x14;
    Vec32MulMatPsx(frame.local<vec32_>(0), ptr32<_matPsx>(objectAt(w.owner)->pRendObj.address + 0x18u), frame.local<vec32_>(0x10));
    const vec32_ &moved = *frame.local<vec32_>(0x10);
    w.target = 0x7f;
    const _sObject &car = *objectAt(w.owner);
    w.pos16.x = car.pos16.x + ee_itof(moved.x);
    w.pos16.y = car.pos16.y + ee_itof(moved.y);
    w.oColrad = k40;
    w.oCol.type = 3;
    w.pos16.z = car.pos16.z + ee_itof(moved.z);
    initOcol(pWeapon);
    w.aColrad = k40;
    // The car's integer matrix and what follows it, 32 bytes, over the rock's matrix.
    std::memmove(guest_host(pWeapon.address + 0xdcu), guest_host(objectAt(w.owner)->pPhysics->pInteger.address + 0x28u), 0x20);
    const ptr32<_sObject> owner = objectAt(w.owner);
    w.rendObj.pos.x = toFixed(w.pos16.x);
    w.rendObj.pos.y = toFixed(w.pos16.y);
    w.rendObj.pos.z = toFixed(w.pos16.z);
    w.rendObj.pRendObjHead = G(objectVarG).pDumperRendObjHeadWeapon;
    owner->tipAngle = 0;
    const normal_ r = turnByCar(frame, 0x20, 0x30, 0, -1000, -0x1000, owner);
    const int16 ownerNo = w.owner;
    w.status = 0;
    w.destPos16.z = static_cast<int16>(scale12(r.z, 0x14));
    w.destPos16.x = static_cast<int16>(scale12(r.x, 0x14));
    w.destPos16.y = static_cast<int16>(scale12(r.y, 10) + 10);
    w.misc = 0;
    w.sfxHandle1 = sfx3D(0x4f, 0xff, 0x100, pWeapon.address + 0xc8u, carSfxFlags(objectAt(ownerNo)), 5000);
    w.otherTimer = time + 1000;
}

void WeaponDumptugWaterSetup(ptr32<_sWeapon> pWeapon, tTime time)
{
    GuestFrame frame(0x70);
    _sWeapon &w = *pWeapon;
    const normal_ r = turnByCar(frame, 0, 0x10, 0, 0, -0x1000, objectAt(w.owner));
    const int32_t x = (static_cast<int32_t>(r.x) * 0x14) / 512, z = (static_cast<int32_t>(r.z) * 0x14) / 512;
    w.oColrad = k40;
    w.destPos16.y = 0;
    w.oCol.type = 3;
    w.destPos16.x = static_cast<int16>(x);
    w.destPos16.z = static_cast<int16>(z);
    w.pos16.y = w.pos16.y + k50;
    w.pos16.x = w.pos16.x + ee_itof(x);
    w.pos16.z = w.pos16.z + ee_itof(z);
    initOcol(pWeapon);
    w.aColrad = k40;
    w.timeWhenEnds = time + 8000;
    w.status = 3;
    w.otherTimer = time + 0x5dc;
    w.target = 0x7f;
    w.pVfx = VfxAddNewPlace(ee_ftoi(w.pos16.x), ee_ftoi(w.pos16.y), ee_ftoi(w.pos16.z), 0x31);
    const ptr32<_sObject> car = objectAt(w.owner);
    sfx3D(0x56, 0xff, 0x100, carPos(car), carSfxFlags(car), 5000);
}

void WeaponMoonbuggySetup(ptr32<_sWeapon> pWeapon, tTime time)
{
    GuestFrame frame(0x50);
    _sWeapon &w = *pWeapon;
    const ptr32<_sVfx_Object> fx = VfxAddNew(objectAt(w.owner), 0, 0, 0x30);
    w.oColrad = k10;
    w.pVfx = fx;
    w.oCol.type = 3;
    initOcol(pWeapon);
    w.aColrad = k10;
    w.misc = 0;
    const ptr32<_sObject> car = objectAt(w.owner);
    car->tipAngle = time + 500;
    sfx3D(0x4e, 0xff, 0x100, carPos(car), carSfxFlags(car), 5000);
}

void WeaponOilSetup(ptr32<_sWeapon> pWeapon)
{
    GuestFrame frame(0x60);
    _sWeapon &w = *pWeapon;
    w.oCol.type = 2;
    w.pos16.y = w.pos16.y + k20;
    AColDownCast(ptr32<_sColCellVecPSX>(pWeapon.address + 0x94u), frame.local<_sColCellVecPSX>(0), frame.local<_sColNormalPSX>(0x10),
                 frame.local<uint8>(0x20));
    w.pVfx = ptr32<_sVfx_Object>(nullptr);
    initOcol(pWeapon);
}

void WeaponLightningSetup(ptr32<_sWeapon> pWeapon, tTime)
{
    GuestFrame frame(0x30);
    _sWeapon &w = *pWeapon;
    w.pVfx = VfxAddNew(objectAt(w.owner), 0, 0, 0x10);
    const ptr32<_sObject> car = objectAt(w.owner);
    w.sfxHandle1 = sfx3D(0x12, 0x80, 0x100, carPos(car), carSfxFlags(car), 5000);
}

void WeaponBatterySetup(ptr32<_sWeapon> pWeapon, tTime)
{
    GuestFrame frame(0x30);
    _sWeapon &w = *pWeapon;
    const ptr32<_sObject> car = objectAt(w.owner);
    w.pVfx = VfxAddNew(car, 0, 0, 0x1a);
    car->pPhysics->powerUpCounter = w.target == 1 ? 400 : w.target == 2 ? 500 : 0xfa;
    w.target = 0x7f;
    w.sfxHandle1 = sfx3D(0x11, 0xa4, 0x100, carPos(car), carSfxFlags(car), 5000);
}

// ---------------------------------------------------------------------------------------------------------------------
// The shorter Updates.

// A looping sound: started (sound, flags | 0x40) when the handle is free, else kept going (flags + 0x58).
static int16 keepSound(_sWeapon &w, int32_t sound, int32_t volume, int32_t pitch, int32_t distance)
{
    const ptr32<_sObject> car = objectAt(w.owner);
    int32_t id = w.sfxHandle1, flags;
    if (id == -1)
    {
        id = sound;
        flags = carSfxFlags(car);
    }
    else
        flags = car->playerType + 0x58;
    return sfx3D(id, volume, pitch, carPos(objectAt(w.owner)), flags, distance);
}

void WeaponBatteryUpdate(ptr32<_sWeapon> pWeapon, tTime time)
{
    GuestFrame frame(0x20);
    _sWeapon &w = *pWeapon;
    if (w.timeWhenEnds - 300 < time && w.pVfx->iLifeSpan >= 0xc)
        w.pVfx->iLifeSpan = 0xb;
    w.sfxHandle1 = keepSound(w, 0x11, 0xa4, 0x100, 5000);
}

void WeaponInvincibleUpdate(ptr32<_sWeapon> pWeapon, tTime time)
{
    GuestFrame frame(0x20);
    _sWeapon &w = *pWeapon;
    if (w.otherTimer < time)
    {
        const int32_t left = w.timeWhenEnds - time;
        const int32_t times256 = static_cast<int32_t>(static_cast<uint32_t>(left) << 8);
        int32_t pitch = static_cast<int16_t>(times256 / 2000);
        int32_t volume = static_cast<int16_t>(static_cast<int32_t>(static_cast<uint32_t>(times256) - static_cast<uint32_t>(left)) / 2000);
        if (volume < 1)
        {
            volume = 0;
            pitch = 0;
        }
        _sObject &car = *objectAt(w.owner);
        car.invincible = static_cast<int8>(car.invincible | 4);
        const int32_t flags = objectAt(w.owner)->playerType + 0x58;
        w.sfxHandle1 = sfx3D(w.sfxHandle1, volume, pitch, carPos(objectAt(w.owner)), flags, 5000);
        return;
    }
    w.sfxHandle1 = keepSound(w, 0x17, 0xa4, 0x100, 5000);
}

void WeaponLightningUpdate(ptr32<_sWeapon> pWeapon, tTime)
{
    GuestFrame frame(0x70);
    _sWeapon &w = *pWeapon;
    w.sfxHandle1 = keepSound(w, 0x12, 0x80, 0x100, 2000);
    for (uint32_t i = 0; i < G(objectVarG).noObjects; ++i)
    {
        const ptr32<_sObject> pCar = objectAt(static_cast<int32_t>(i));
        _sObject &car = *pCar;
        if (!(car.weaponAffect == 0 || car.weaponAffect == 7))
            continue;
        const int16 owner = w.owner;
        if (car.playerNo == owner || car.invincible != 0)
            continue;
        if (!(car.weaponAffectOwner == 0xff || owner == car.weaponAffectOwner))
            continue;
        vec32_ &d = *frame.local<vec32_>(0);
        d.y = 0;
        d.z = ee_ftoi(car.pos16.z - objectAt(owner)->pos16.z);
        d.x = ee_ftoi(car.pos16.x - objectAt(owner)->pos16.x);
        if (static_cast<int32_t>(guest_call<uint32_t>(kVec32Length, frame.local<vec32_>(0))) < 0x3e9)
        {
            if (car.weaponAffect != 7)
            {
                car.weaponAffect = 7;
                car.weaponAffectOwner = w.owner;
                car.pVfxMisc = VfxAddNew(objectAt(w.owner), static_cast<int32>(pCar.address), 0, 0x11);
                VfxAddNew(pCar, 0, 0, 0x12);
                car.lightningSfx = static_cast<int8>(sfx3D(0x13, 0x80, 0x100, carPos(pCar), carSfxFlags(pCar), 0x5dc));
            }
        }
        else
        {
            guest_call(kObjectWeaponAffectRemove, pCar);
            car.weaponAffect = 0;
        }
    }
}

void WeaponSargeUpdate(ptr32<_sWeapon> pWeapon, tTime)
{
    GuestFrame frame(0x70);
    _sWeapon &w = *pWeapon;
    w.sfxHandle1 = keepSound(w, 0x34, 0x100, 0x100, 5000);
    int32_t pos = G(mainVarG).noAI + G(mainVarG).noHuman - 1;
    const int32_t behind = objectAt(w.owner)->iRacePos + 1;
    if (behind <= pos)
        pos = behind;
    for (; pos >= 0; --pos)
    {
        const int16 owner = w.owner;
        const ptr32<_sObject> pTarget = at<ptr32<_sObject>>(kRaceOrder + static_cast<uint32_t>(pos) * kRaceOrderStride);
        _sObject &target = *pTarget;
        if (target.playerNo == owner || target.invincible != 0)
            continue;
        vec32_ &d = *frame.local<vec32_>(0);
        d.y = 0;
        d.z = ee_ftoi(objectAt(owner)->pos16.z - target.pos16.z);
        d.x = ee_ftoi(objectAt(owner)->pos16.x - target.pos16.x);
        if (static_cast<int32_t>(guest_call<uint32_t>(kVec32Length, frame.local<vec32_>(0))) >= 1000)
            continue;
        const int16 angle = WeaponRelativeAngle(objectAt(w.owner), pTarget);
        _sFPhysLibCar &physics = *target.pPhysics;
        if (angle < 0)
        {
            physics.inputSteerAngle = static_cast<int16>(physics.inputSteerAngle - 0x28);
            if (physics.inputSteerAngle < -0x7e)
                physics.inputSteerAngle = -0x7f;
        }
        else if (angle > 0)
        {
            physics.inputSteerAngle = static_cast<int16>(physics.inputSteerAngle + 0x28);
            if (physics.inputSteerAngle > 0x7e)
                physics.inputSteerAngle = 0x7f;
        }
    }
}

void WeaponMoonbuggyUpdate(ptr32<_sWeapon> pWeapon, tTime)
{
    const ee_float kHalf = 0.5f;
    _sWeapon &w = *pWeapon;
    w.oCol.updated = 1;
    const uint32_t fx = w.pVfx.address;
    const auto mid = [&](uint32_t lane)
    {
        const ee_float start = at<ee_float>(fx + 0x50u + lane), end = at<ee_float>(fx + 0x60u + lane);
        return ee_itof(static_cast<int16_t>(ee_ftoi(start + (end - start) * kHalf)));
    };
    w.pos16.x = mid(0);
    w.pos16.y = mid(4);
    w.pos16.z = mid(8);
    GuestFrame frame(0x20);
    if (AColIsPositionOutsideWorldExtentsXZ(ptr32<_sColCellVecPSX>(pWeapon.address + 0x94u)))
    {
        OColDeleteObject(ptr32<_sOColObj>(pWeapon.address + 0x28u));
        w.timeWhenEnds = 0;
        w.oCol.type = 4;
    }
}

// The shock wave and the skull's hex reach the cars marked when they were fired, as the effect's ring (radius at
// fx+0x58, centre at fx+0x654) passes them.
bool weapon::inRing(const GuestFrame &frame, uint32_t fx, const _sObject &car, int32_t radius)
{
    vec32_ &d = *frame.local<vec32_>(0);
    d.y = 0;
    d.x = ee_ftoi(car.pos16.x - ee_itof(at<int16_t>(fx + 0x654u)));
    d.z = ee_ftoi(car.pos16.z - ee_itof(at<int16_t>(fx + 0x658u)));
    return static_cast<int32_t>(guest_call<uint32_t>(kVec32Length, frame.local<vec32_>(0))) < radius;
}

// Pushes a car away from a point (fromX, fromZ): force (-4 * d, up, -4 * d) at offset d, d = 2000 along the direction
// from the car to the point. The frame locals are the shock wave's and the armoured car's (frame 0x100).
void weapon::pushAway(const GuestFrame &frame, _sObject &car, int16_t up, ee_float fromX, ee_float fromZ)
{
    vec32_ &temp = *frame.local<vec32_>(0x30);
    temp.y = 0;
    temp.z = ee_ftoi(fromZ - car.pos16.z);
    temp.x = ee_ftoi(fromX - car.pos16.x);
    guest_call(kVec32Normalise, frame.local<_sVec16>(0x20), frame.local<vec32_>(0x30));
    const _sVec16 &offset = *frame.local<_sVec16>(0x20);
    _sVec16 &force = *frame.local<_sVec16>(0x10);
    _sVec16 &at = *frame.local<_sVec16>(0x40);
    force.y = up;
    at.y = 0;
    at.x = static_cast<int16>(scale12(offset.x, 2000));
    force.x = static_cast<int16>(at.x * -4);
    at.z = static_cast<int16>(scale12(offset.z, 2000));
    force.z = static_cast<int16>(at.z * -4);
    guest_call(kPhysicsApplyIntForce, car.pPhysics, frame.local<_sVec16>(0x10), frame.local<_sVec16>(0x40), 0);
}

void WeaponShockwaveUpdate(ptr32<_sWeapon> pWeapon, tTime time)
{
    _sWeapon &w = *pWeapon;
    {
        GuestFrame frame(0x100);
        if (w.otherTimer < time)
        {
            const uint32_t fx = w.pVfx.address;
            const int32_t radius = static_cast<int16_t>(ee_ftoi(at<ee_float>(fx + 0x58u)));
            for (uint32_t i = 0; i < G(objectVarG).noObjects; ++i)
            {
                _sObject &car = *objectAt(static_cast<int32_t>(i));
                if (car.invincible != 0 || w.owner == static_cast<int32_t>(i) || car.shockWaved != 1)
                    continue;
                if (!inRing(frame, fx, car, radius))
                    continue;
                car.shockWaved = 2;
                VfxAddNew(objectAt(static_cast<int32_t>(i)), 0, 0, 0);
                int32_t status = w.status;
                if (car.playerType < 2)
                {
                    sfx3D(0x38, 0xff, 0x100, car.pRendObj.address + 4u, car.playerType + 4, 5000);
                    status = w.status;
                }
                else if (status < 3)
                    sfx3D(0x38, 0xff, 0x100, car.pRendObj.address + 4u, 4, 5000);
                w.status = status + 1;
                pushAway(frame, car, 4000, objectAt(w.owner)->pos16.x, objectAt(w.owner)->pos16.z);
                guest_call(kObjectWeaponAffectRemove, ptr32<_sObject>(guest_addr(&car)));
                car.weaponAffectTime = time + 0x5dc;
                car.weaponAffect = 4;
            }
        }
        if (w.misc == 0)
            return;
    }
    WeaponShockwaveShake(pWeapon, time);
    w.misc = static_cast<int16>(w.misc - 1);
}

void WeaponSkullUpdate(ptr32<_sWeapon> pWeapon, tTime time)
{
    GuestFrame frame(0xb0);
    _sWeapon &w = *pWeapon;
    if (w.status < time)
    {
        w.pVfx = VfxAddNew(objectAt(w.owner), 0, 0, 0x26);
        const ptr32<_sObject> owner = objectAt(w.owner);
        sfx3D(0x35, 0x100, 0x60, carPos(owner), carSfxFlags(objectAt(w.owner)), 5000);
        w.status = time + 8000;
        w.otherTimer = time + 0x4b;
        w.misc = 1;
        for (uint32_t i = 0; i < G(objectVarG).noObjects; ++i)
        {
            _sObject &car = *objectAt(static_cast<int32_t>(i));
            vec32_ &d = *frame.local<vec32_>(0);
            d.y = 0;
            d.z = ee_ftoi(car.pos16.z - objectAt(w.owner)->pos16.z);
            d.x = ee_ftoi(car.pos16.x - objectAt(w.owner)->pos16.x);
            if (static_cast<int32_t>(guest_call<uint32_t>(kVec32Length, frame.local<vec32_>(0))) < 5000)
                car.shockWaved = 1;
        }
    }
    if (w.misc != 1 || !(w.otherTimer < time))
        return;
    const uint32_t fx = w.pVfx.address;
    const int32_t radius = static_cast<int16_t>(ee_ftoi(at<ee_float>(fx + 0x58u)));
    for (uint32_t i = 0; i < G(objectVarG).noObjects; ++i)
    {
        const ptr32<_sObject> pCar = objectAt(static_cast<int32_t>(i));
        _sObject &car = *pCar;
        if (car.invincible != 0 || car.playerNo == w.owner || car.weaponAffect == 0xd || car.shockWaved != 1)
            continue;
        if (!inRing(frame, fx, car, radius))
            continue;
        car.weaponAffectTime = time + 3000;
        guest_call(kObjectWeaponAffectRemove, pCar);
        car.weaponAffect = 0xd;
        car.shockWaved = 2;
        car.pVfxHexed = VfxAddNew(pCar, 0, 0, 0x25);
    }
}

void WeaponWaterUpdate(ptr32<_sWeapon> pWeapon, tTime time)
{
    GuestFrame frame(0x80);
    _sWeapon &w = *pWeapon;
    w.destPos16.y = static_cast<int16>(w.destPos16.y - 2);
    w.oldPos16 = w.pos16;
    const ee_float x = w.pos16.x + ee_itof(w.destPos16.x);
    const ee_float y = w.pos16.y + ee_itof(w.destPos16.y);
    const ee_float z = w.pos16.z + ee_itof(w.destPos16.z);
    w.pos16.x = x;
    w.pos16.y = y;
    w.pos16.z = z;
    if (w.misc == 0)
    {
        _sVfx_Object &fx = *w.pVfx;
        fx.svHookPos.vx = static_cast<int16>(ee_ftoi(x));
        fx.svHookPos.vy = static_cast<int16>(ee_ftoi(y));
        fx.svHookPos.vz = static_cast<int16>(ee_ftoi(z));
        _sVec16 &normal = *frame.local<_sVec16>(0);
        int kind = 0; // 0 flying, 1 hit something, 2 left the world
        uint32_t material = 0;
        if (AColIsPositionOutsideWorld(ptr32<_sColCellVecPSX>(pWeapon.address + 0x94u), kZero))
        {
            normal.x = 0;
            normal.y = 0x1000;
            material = 0x48;
            normal.z = 0;
            kind = 2;
        }
        bool hit = true;
        if (kind == 0)
        {
            material = static_cast<uint32_t>(WeaponTestAcol(pWeapon, frame.local<_sVec16>(0))) & 0xffu;
            hit = material != 0x48;
            kind = hit ? 1 : 0;
        }
        if (hit)
        {
            w.timeWhenEnds = 0;
            OColDeleteObject(ptr32<_sOColObj>(pWeapon.address + 0x28u));
            w.oCol.type = 4;
            rearm(w.owner, 5);
            if (kind != 2)
            {
                const ptr32<_sVfx_Object> splash = VfxAddNewPlace(ee_ftoi(w.pos16.x), ee_ftoi(w.pos16.y), ee_ftoi(w.pos16.z), 0xc);
                splash->svHookPos.vx = normal.x;
                splash->svHookPos.vy = normal.y;
                splash->svHookPos.vz = normal.z;
                vec32_ &pos = *frame.local<vec32_>(0x10);
                pos.x = toFixed(w.pos16.x);
                pos.z = toFixed(w.pos16.z);
                pos.y = ee_ftoi((w.pos16.y - k25) * k256);
                if (material == 0x19)
                    guest_call(kRevSfxSpecialTrigger, 2, frame.local<vec32_>(0x10), G(mainVarG).time);
                sfx3D(0x21, 0xff, 0x100, frame.local<vec32_>(0x10).address, carSfxFlags(objectAt(w.owner)), 5000);
            }
        }
    }
    const int32_t timer = w.otherTimer;
    w.oCol.updated = 1;
    if (timer != 0 && timer < time)
    {
        w.otherTimer = 0;
        rearm(w.owner, 5);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// A weapon reaching a car (OColCar_Weapon_Collide) and a car on an oil slick.

void WeaponHitCar(ptr32<_sWeapon> pWeapon, ptr32<_sObject> pObject)
{
    GuestFrame frame(0xb0);
    _sWeapon &w = *pWeapon;
    _sObject &car = *pObject;
    const auto boom = [&] { sfx3D(0x38, 0xff, 0x100, carPos(pObject), carSfxFlags(pObject), 5000); };
    const auto stopDead = [&](ee_float lift)
    {
        _sFPhysLibCar &physics = *car.pPhysics;
        const ee_float vy = physics.vel.y;
        physics.vel.z = kZero;
        physics.speed = kZero;
        physics.vel.x = kZero;
        physics.vel.y = vy + lift;
    };
    switch (w.no)
    {
    case 1:
    {
        VfxAddNewPlace(ee_ftoi(w.pos16.x), ee_ftoi(w.pos16.y), ee_ftoi(w.pos16.z), 0x16);
        if (car.invincible != 0)
            break;
        VfxAddNew(objectAt(car.playerNo), 0x32, 0, 1);
        VfxAddNew(objectAt(car.playerNo), 0, 0, 0);
        car.lockOnMe = 0;
        boom();
        vec32_ &temp = *frame.local<vec32_>(0x20);
        temp.y = 0;
        temp.z = ee_ftoi(objectAt(w.owner)->pos16.z - car.pos16.z);
        temp.x = ee_ftoi(objectAt(w.owner)->pos16.x - car.pos16.x);
        guest_call(kVec32Normalise, frame.local<_sVec16>(0x10), frame.local<vec32_>(0x20));
        const _sVec16 &offset = *frame.local<_sVec16>(0x10);
        _sVec16 &force = *frame.local<_sVec16>(0);
        _sVec16 &at = *frame.local<_sVec16>(0x30);
        force.y = 5000;
        at.x = static_cast<int16>(scale12(offset.x, 4000));
        force.x = static_cast<int16>(at.x * -2);
        at.z = static_cast<int16>(scale12(offset.z, 4000));
        force.z = static_cast<int16>(at.z * -2);
        at.y = 0;
        guest_call(kPhysicsApplyIntForce, car.pPhysics, frame.local<_sVec16>(0), frame.local<_sVec16>(0x30), 0);
        guest_call(kObjectWeaponAffectRemove, pObject);
        car.weaponAffect = 1;
        car.weaponAffectTime = static_cast<int32>(timerGet() + 2000);
        break;
    }
    case 3:
        VfxAddNewPlace(ee_ftoi(w.pos16.x), ee_ftoi(w.pos16.y), ee_ftoi(w.pos16.z), 9);
        if (car.invincible != 0)
            break;
        VfxAddNew(objectAt(car.playerNo), 0, 0, 0);
        boom();
        stopDead(k6000);
        break;
    case 0xf:
        if (car.invincible != 0 || w.status != 3 || car.weaponAffect == 0xf)
            return;
        if (w.owner == car.playerNo && G(mainVarG).time < w.otherTimer)
            return;
        guest_call(kObjectWeaponAffectRemove, pObject);
        car.weaponAffect = 0xf;
        car.weaponAffectUniqueId = w.uniqueId;
        car.weaponAffectTime = G(mainVarG).time + 4000;
        car.pVfxTugBubble = VfxAddNew(pObject, 0, 0, 0x32);
        return;
    case 0x10:
        if (car.invincible == 0)
        {
            VfxAddNew(objectAt(car.playerNo), 0, 0, 0);
            boom();
            stopDead(k6000);
        }
        w.timeWhenEnds = 0;
        deleteOcol(pWeapon);
        return;
    case 5:
    {
        if (w.owner == car.playerNo || car.weaponAffect == 5)
            return;
        if (car.invincible != 0)
        {
            const ptr32<_sVfx_Object> splash = VfxAddNewPlace(ee_ftoi(car.pos16.x), ee_ftoi(car.pos16.y), ee_ftoi(car.pos16.z), 0xc);
            const uint32_t pos = carPos(pObject);
            splash->svHookPos.vz = 0;
            splash->svHookPos.vy = 0x1000;
            splash->svHookPos.vx = 0;
            sfx3D(0x21, 0xff, 0x100, pos, carSfxFlags(pObject), 5000);
            w.timeWhenEnds = 0;
            deleteOcol(pWeapon);
            rearm(w.owner, 5);
            return;
        }
        if (AColIsPositionOutsideWorld(ptr32<_sColCellVecPSX>(pObject.address + 0xbcu), kZero))
            return;
        w.misc = 1;
        w.pVfx2 = VfxAddNew(pObject, 0, 0, 0x17);
        sfx3D(0x1f, 0xff, 0x100, carPos(pObject), carSfxFlags(pObject), 5000);
        if (car.heatrayStore == 5000)
        {
            if (car.weaponsHeldSfx != -1)
            {
                sfxStop(car.weaponsHeldSfx);
                car.weaponsHeldSfx = -1;
            }
            car.heatrayStore = 0;
            car.weaponsHeld = 0;
            car.heatrayStoreAngle = 0;
            car.weaponType = 0;
        }
        const ptr32<_sVfx_Object> splash = VfxAddNewPlace(ee_ftoi(car.pos16.x), ee_ftoi(car.pos16.y), ee_ftoi(car.pos16.z), 0xc);
        const ptr32<_sVfx_Object> fx = w.pVfx;
        splash->svHookPos.vz = 0;
        splash->svHookPos.vy = 0x1000;
        splash->svHookPos.vx = 0;
        if (fx)
        {
            VfxDelete(fx);
            w.pVfx = ptr32<_sVfx_Object>(nullptr);
        }
        w.timeWhenEnds = static_cast<tTime>(timerGet() + 4000);
        guest_call(kObjectWeaponAffectRemove, pObject);
        car.weaponAffect = w.no;
        car.waterEscape = 0;
        w.target = car.playerNo;
        deleteOcol(pWeapon);
        sfx3D(0x21, 0xff, 0x100, carPos(pObject), carSfxFlags(pObject), 5000);
        return;
    }
    case 0xb:
    {
        if (w.owner == car.playerNo && static_cast<int32_t>(timerGet()) < w.timeWhenEnds - 7000)
            return;
        if (car.invincible != 0)
            break;
        VfxAddNew(objectAt(car.playerNo), 0x32, 0, 1);
        VfxAddNew(objectAt(car.playerNo), 0, 0, 0);
        boom();
        _sFPhysLibCar &physics = *car.pPhysics;
        _sVec16 &force = *frame.local<_sVec16>(0x40);
        _sVec16 &at = *frame.local<_sVec16>(0x50);
        force.y = 0x157c;
        physics.vel.z = kZero;
        physics.speed = kZero;
        physics.vel.x = kZero;
        physics.vel.y = kZero;
        force.x = 0;
        force.z = 0;
        at.x = static_cast<int16>(RandomGetNDRange(0, 14000) + 0xe4a8);
        at.y = 0;
        at.z = static_cast<int16>(RandomGetNDRange(0, 14000) + 0xe4a8);
        guest_call(kPhysicsApplyIntForce, car.pPhysics, frame.local<_sVec16>(0x40), frame.local<_sVec16>(0x50), 0);
        break;
    }
    default:
        return;
    }
    w.timeWhenEnds = 0;
}

void WeaponOilCollide(ptr32<_sWeapon> pWeapon, ptr32<_sObject> pObject)
{
    _sWeapon &w = *pWeapon;
    _sObject &car = *pObject;
    const int32_t maxX = toFixed(w.oCol.max.x), maxZ = toFixed(w.oCol.max.z);
    const int32_t minX = toFixed(w.oCol.min.x), minZ = toFixed(w.oCol.min.z);
    const uint32_t type = objectType(pObject);
    if (type == 7 || type == 0x1a || type == 0xf || type == 0x22 || car.invincible != 0)
        return;
    _sFPhysLibCar &physics = *car.pPhysics;
    if (type >= 0x10u && kZero < physics.pSphere[4].liquidDepth)
    {
        // A boat skids: the speed goes and the sideways velocity drops by 1024.
        physics.speed = kZero;
        const auto slow = [&](ee_float &v)
        {
            if (k1024 < v)
                v = v - k1024;
            else if (v < -k1024)
                v = v + k1024;
        };
        slow(physics.vel.x);
        slow(physics.vel.z);
        return;
    }
    GuestFrame frame(0x40);
    uint8_t onOil[4];
    int32_t grounded = 0;
    for (int i = 0; i < 4; ++i)
    {
        const _sFPhysLibWheel &wheel = physics.wheel[i];
        onOil[i] = 0;
        if ((wheel.flags & 1u) == 0)
            continue;
        if (wheel.pos.x <= ee_itof(maxX) && ee_itof(minX) <= wheel.pos.x && wheel.pos.z <= ee_itof(maxZ) && !(wheel.pos.z < ee_itof(minZ)))
            onOil[i] = 1;
        ++grounded;
    }
    std::memcpy(frame.local<uint8>(0).get(), onOil, 4);
    if (grounded < 3)
        return;
    for (int i = 0; i < 4; ++i)
    {
        if (!onOil[i])
            continue;
        if (car.skidOil == 0)
        {
            const uint32_t now = timerGet();
            car.skidOil = static_cast<int32>(now + 0x5dc);
            car.pPhysics->wheel[3].oilyCounter = 0x3c;
            car.pPhysics->wheel[2].oilyCounter = 0x3c;
        }
        if (i == 0 || i == 2)
            at<uint8_t>(car.pVfx_SkidLeft.address + 0x18bu) |= 1u;
        if (i == 1 || i == 3)
            at<uint8_t>(car.pVfx_SkidRight.address + 0x18bu) |= 1u;
    }
}

} // namespace rc

RC_DECOMP_FN_SETUP_HEAP(WeaponRocketSetup, rc::fuzz::weaponSetup<1>)
RC_DECOMP_FN_SETUP_HEAP(WeaponFakeSetup, rc::fuzz::weaponSetup<3>)
RC_DECOMP_FN_SETUP_HEAP(WeaponShockwaveSetup, rc::fuzz::weaponSetup<4>)
RC_DECOMP_FN_SETUP_HEAP(WeaponWaterSetup, rc::fuzz::weaponSetup<5>)
RC_DECOMP_FN_SETUP_HEAP(WeaponMegaSetup, rc::fuzz::weaponSetup<9>)
RC_DECOMP_FN_SETUP_HEAP(WeaponHeatraySetup, rc::fuzz::weaponSetup<8>)
RC_DECOMP_FN_SETUP_HEAP(WeaponInvincibleSetup, rc::fuzz::weaponSetup<10>)
RC_DECOMP_FN_SETUP_HEAP(WeaponBouncySetup, rc::fuzz::weaponSetup<11>)
RC_DECOMP_FN_SETUP_HEAP(WeaponSargeSetup, rc::fuzz::weaponSetup<12>)
RC_DECOMP_FN_SETUP_HEAP(WeaponSkullSetup, rc::fuzz::weaponSetup<13>)
RC_DECOMP_FN_SETUP_HEAP(WeaponArmouredSetup, rc::fuzz::weaponSetup<14>)
RC_DECOMP_FN_SETUP_HEAP(WeaponDumptugSetup, rc::fuzz::weaponSetup<15>)
RC_DECOMP_FN_SETUP_HEAP(WeaponDumptugWaterSetup, rc::fuzz::weaponSetup<15>)
RC_DECOMP_FN_SETUP_HEAP(WeaponMoonbuggySetup, rc::fuzz::weaponSetup<16>)
RC_DECOMP_FN_SETUP_HEAP(WeaponOilSetup, rc::fuzz::weaponSetup<6>)
RC_DECOMP_FN_SETUP_HEAP(WeaponLightningSetup, rc::fuzz::weaponSetup<7>)
RC_DECOMP_FN_SETUP_HEAP(WeaponBatterySetup, rc::fuzz::weaponSetup<2>)
RC_DECOMP_FN_SETUP_HEAP(WeaponBatteryUpdate, rc::fuzz::weaponLive<2>)
RC_DECOMP_FN_SETUP_HEAP(WeaponInvincibleUpdate, rc::fuzz::weaponLive<10>)
RC_DECOMP_FN_SETUP_HEAP(WeaponLightningUpdate, rc::fuzz::weaponLive<7>)
RC_DECOMP_FN_SETUP_HEAP(WeaponSargeUpdate, rc::fuzz::weaponLive<12>)
RC_DECOMP_FN_SETUP_HEAP(WeaponMoonbuggyUpdate, rc::fuzz::weaponLive<16>)
RC_DECOMP_FN_SETUP_HEAP(WeaponShockwaveUpdate, rc::fuzz::weaponLive<4>)
RC_DECOMP_FN_SETUP_HEAP(WeaponSkullUpdate, rc::fuzz::weaponLive<13>)
RC_DECOMP_FN_SETUP_HEAP(WeaponWaterUpdate, rc::fuzz::weaponLive<5>)
RC_DECOMP_FN_SETUP_HEAP(WeaponHitCar, rc::fuzz::weaponHitCar)
RC_DECOMP_FN_SETUP_HEAP(WeaponOilCollide, rc::fuzz::weaponOil)
