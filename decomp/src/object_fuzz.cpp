// Builds the cars' random-test inputs. See object_fuzz.h.
//
// The whole-RAM setups change the dump freely: RAM is put back after every input. Guest calls made here reach the
// originals (a setup runs while comparing).

#include "object_fuzz.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <iterator>

namespace rc::fuzz {

namespace {

constexpr uint32_t kObjectList = sym::objectVarG, kWeaponVars = sym::weaponVarG, kSfxInitOk = sym::sfxVarG, kMainVars = sym::mainVarG;
constexpr uint32_t kVfxAddNew = sym::VfxAddNew, kObjectCreateAddr = sym::ObjectCreate;

// Scratch layout.
constexpr uint32_t kObject = 0x0000u, kDef = 0x1000u, kHandle = 0x1080u, kWeapons = 0x2000u;

template <typename T>
T &mem(uint32_t address) { return *static_cast<T *>(guest_host(address)); }

uint32_t rnd(uint64_t &state, uint32_t n) { return n == 0 ? 0u : nextFuzz(state) % n; }
bool chance(uint64_t &state, uint32_t percent) { return rnd(state, 100) < percent; }
float frnd(uint64_t &state, float scale) { return (static_cast<float>(static_cast<int32_t>(nextFuzz(state) % 20001u) - 10000) / 10000.0f) * scale; }

void setArg(R5900Context *ctx, int index, uint32_t value) { SET_GPR_S32(ctx, 4 + index, static_cast<int32_t>(value)); }

uint32_t noObjects() { return mem<uint32_t>(kObjectList + 0x170u); }
uint32_t objectAt(uint32_t n) { return mem<uint32_t>(kObjectList + 4u * n); }
uint32_t boatAt(uint32_t n) { return mem<uint32_t>(kObjectList + 0xb8u + 4u * n); }

bool inRam(uint32_t address) { return address != 0 && address < 0x2000000u; }

bool usableCar(uint32_t car)
{
    if (!inRam(car))
        return false;
    const _sObject &o = mem<_sObject>(car);
    return inRam(o.pRendObj.address) && inRam(o.pPhysics.address) && inRam(o.pDef.address) && inRam(o.pSounds.address) &&
           inRam(o.pDef->pNodeHead.address) && inRam(o.pVfx_SkidLeft.address) && inRam(o.pVfx_SkidRight.address) &&
           inRam(o.pCarCol.address) && inRam(o.pWheels.address);
}

uint32_t pickCar(uint64_t &state)
{
    const uint32_t count = noObjects();
    for (int tries = 0; tries < 30 && count; ++tries)
    {
        const uint32_t n = rnd(state, count);
        if (usableCar(objectAt(n)))
            return n;
    }
    return 0;
}

void soundOff() { mem<uint8_t>(kSfxInitOk) = 0; }

int32_t timeNear(uint64_t &state) { return static_cast<int32_t>(mem<uint32_t>(kMainVars + 0x1cu)) + static_cast<int32_t>(rnd(state, 0x4000)); }

// The id of a resource the loader already has (a handle with its low bits clear, which ResLoad returns as it is), or 0.
uint32_t loadedResource(uint64_t &state)
{
    const resClass &res = G(resVarG);
    const uint32_t maxId = res.lookupHeader.maxId;
    if (res.handlesHandle.address == 0 || res.handlesHandle.address >= 0x02000000u || maxId < 2 || maxId > 20000)
        return 0;
    for (int tries = 0; tries < 200; ++tries)
    {
        const uint32_t id = 1 + nextFuzz(state) % (maxId - 1);
        const uint32_t handle = res.handlesHandle[id];
        if ((handle & 3u) == 0 && (handle != 0 || (nextFuzz(state) & 7u) == 0))
            return id;
    }
    return 0;
}

// A copy of a real car's definition at scratch+kDef, with its flags, type, place and angles varied.
uint32_t buildDef(uint64_t &state, uint32_t scratch, uint32_t car)
{
    const uint32_t def = scratch + kDef;
    std::memcpy(guest_host(def), guest_host(mem<_sObject>(car).pDef.address), sizeof(_sObjectDef));
    _sObjectDef &d = mem<_sObjectDef>(def);
    if (chance(state, 50))
        d.memFlags = static_cast<eObjectMemFlags>(nextFuzz(state) & 0xffu);
    if (chance(state, 40))
        d.type = static_cast<eObjectType>(rnd(state, 41));
    if (chance(state, 50))
    {
        d.pos.x = static_cast<int32_t>(nextFuzz(state) % 200000u) - 100000;
        d.pos.y = static_cast<int32_t>(nextFuzz(state) % 20000u) - 10000;
        d.pos.z = static_cast<int32_t>(nextFuzz(state) % 200000u) - 100000;
        d.angles.pitch = static_cast<angle>(nextFuzz(state));
        d.angles.head = static_cast<angle>(nextFuzz(state));
        d.angles.roll = static_cast<angle>(nextFuzz(state));
    }
    if (chance(state, 30))
        d.pWheelResP2 = ptr32<_sWheelRes>(nextFuzz(state));
    if (chance(state, 30))
        d.uiRibbonColour = nextFuzz(state);
    return def;
}

// A copy of a real car at scratch+kObject, its parts (laid out as ObjectSetPointers would for the real definition's
// flags) copied after it, so everything it writes is in scratch.
uint32_t buildObject(uint64_t &state, uint32_t scratch, uint32_t car)
{
    const uint32_t o = scratch + kObject;
    const _sObject &real = mem<_sObject>(car);
    std::memcpy(guest_host(o), guest_host(car), 0x42c);
    _sObject &c = mem<_sObject>(o);
    uint32_t at = o + 0x42cu;
    const auto part = [&](auto &field, uint32_t from, uint32_t size)
    {
        using P = std::decay_t<decltype(field)>;
        if (inRam(from))
            std::memcpy(guest_host(at), guest_host(from), size);
        else
            for (uint32_t i = 0; i < size; i += 4)
                mem<uint32_t>(at + i) = nextFuzz(state);
        field = P(at);
        at += size;
    };
    part(c.pRendObj, real.pRendObj.address, 0x38);
    part(c.pRendObj2, real.pRendObj2.address, 0x38);
    part(c.pTask, real.pTask.address, 0x18);
    part(c.pPhysics, real.pPhysics.address, 0x23c);
    part(c.pCarCol, real.pCarCol.address, 0x14);
    part(c.pWheels, real.pWheels.address, 0x14);
    part(c.pSounds, real.pSounds.address, 0x58);
    if (chance(state, 40))
        for (int i = 4; i < 12; ++i)
            mem<uint8_t>(o + static_cast<uint32_t>(i)) = static_cast<uint8_t>(chance(state, 30) ? rnd(state, 9) : nextFuzz(state));
    if (chance(state, 40))
        c.playerType = static_cast<uint8>(rnd(state, 4));
    if (chance(state, 20))
        c.playerType = static_cast<uint8>(nextFuzz(state));
    return o;
}

void varyMain(uint64_t &state)
{
    mainClass &main = G(mainVarG);
    if (chance(state, 40))
        main.noHuman = static_cast<uint8>(rnd(state, 3));
    if (chance(state, 30))
        main.status = static_cast<eMain>(chance(state, 50) ? 0x10 : rnd(state, 0x12));
    if (chance(state, 30))
        G(objectVarG).noObjects = rnd(state, 9);
}

// Varies the race's cars: tints, lights, sounds, weapons held.
void varyCars(uint64_t &state)
{
    for (uint32_t i = 0; i < noObjects(); ++i)
    {
        const uint32_t car = objectAt(i);
        if (!usableCar(car))
            continue;
        _sObject &o = mem<_sObject>(car);
        if (chance(state, 40))
            for (int b = 4; b < 12; ++b)
                mem<uint8_t>(car + static_cast<uint32_t>(b)) = static_cast<uint8_t>(nextFuzz(state));
        if (chance(state, 30))
            o.lightsOn = static_cast<int8>(rnd(state, 4));
        if (chance(state, 30))
            o.playerType = static_cast<uint8>(rnd(state, 3));
    }
}

} // namespace

void objectNone(R5900Context *, uint64_t &state, uint32_t)
{
    if (chance(state, 50))
        G(dumperHackG) = static_cast<int32_t>(rnd(state, 2));
}

void objectDaemon(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    uint32_t *w = &mem<uint32_t>(scratch + kDef);
    for (int i = 0; i < 0x16; ++i)
        w[i] = chance(state, 20) ? 0u : loadedResource(state);
    w[0] = nextFuzz(state) & 0xffu;
    G(carListVarG).alternativeRendObj = static_cast<int32>(rnd(state, 4));
    G(objectConstructorNoLoadROBs) = static_cast<int32_t>(chance(state, 70) ? 0 : 1);
    mem<uint32_t>(scratch + kHandle) = scratch + kDef;
    setArg(ctx, 0, scratch + kHandle);
}

void objectDefOnly(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    _sObjectDef &d = mem<_sObjectDef>(scratch + kDef);
    d.memFlags = static_cast<eObjectMemFlags>(nextFuzz(state));
    setArg(ctx, 0, scratch + kDef);
}

void objectScratchDef(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    const uint32_t car = objectAt(pickCar(state));
    const uint32_t def = buildDef(state, scratch, car);
    const uint32_t o = buildObject(state, scratch, car);
    if (chance(state, 70))
        mem<_sObject>(o).pDef = ptr32<_sObjectDef>(def);
    varyMain(state);
    if (chance(state, 30))
        G(dumperHackG) = static_cast<int32_t>(rnd(state, 2));
    setArg(ctx, 0, o);
    setArg(ctx, 1, def);
    setArg(ctx, 2, static_cast<uint32_t>(timeNear(state)));
}

void objectInitObject(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    soundOff();
    const uint32_t car = objectAt(pickCar(state));
    const uint32_t def = buildDef(state, scratch, car);
    mem<_sObjectDef>(def).memFlags = mem<_sObject>(car).pDef->memFlags;
    if (chance(state, 50))
        mem<_sObjectDef>(def).memFlags = static_cast<eObjectMemFlags>(mem<uint32_t>(def) & nextFuzz(state));
    const uint32_t o = buildObject(state, scratch, car);
    varyMain(state);
    setArg(ctx, 0, o);
    setArg(ctx, 1, def);
    setArg(ctx, 2, static_cast<uint32_t>(timeNear(state)));
}

void objectCreate(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    soundOff();
    const uint32_t car = objectAt(pickCar(state));
    uint32_t def = mem<_sObject>(car).pDef.address;
    if (chance(state, 30))
    {
        def = buildDef(state, scratch, car);
        mem<_sObjectDef>(def).memFlags = static_cast<eObjectMemFlags>(mem<_sObject>(car).pDef->memFlags & nextFuzz(state));
    }
    varyMain(state);
    if (noObjects() > 30)
        G(objectVarG).noObjects = 8;
    setArg(ctx, 0, def);
    setArg(ctx, 1, static_cast<uint32_t>(timeNear(state)));
    setArg(ctx, 2, rnd(state, 8));
}

void objectDelete(R5900Context *ctx, uint64_t &state, uint32_t)
{
    soundOff();
    uint32_t victim = objectAt(pickCar(state));
    if (chance(state, 40))
    {
        const uint32_t made = guest_call<uint32_t>(kObjectCreateAddr, mem<_sObject>(victim).pDef, timeNear(state), rnd(state, 4));
        if (made)
            victim = made;
    }
    else if (chance(state, 30) && inRam(boatAt(mem<_sObject>(victim).playerNo)))
        victim = boatAt(mem<_sObject>(victim).playerNo);
    setArg(ctx, 0, victim);
}

void objectRace(R5900Context *, uint64_t &state, uint32_t)
{
    soundOff();
    varyCars(state);
}

void objectSfxAll(R5900Context *ctx, uint64_t &state, uint32_t)
{
    soundOff();
    varyCars(state);
    mainClass &main = G(mainVarG);
    if (chance(state, 20))
        main.pauseMenu = static_cast<uint16>(rnd(state, 3));
    if (chance(state, 20))
        main.status = static_cast<eMain>(rnd(state, 6));
    if (chance(state, 20))
        main.gameEndPos = static_cast<eGameEndPos>(rnd(state, 3));
    static const int32_t kFlags[] = {-1, 0, 0, 1};
    setArg(ctx, 0, static_cast<uint32_t>(kFlags[rnd(state, std::size(kFlags))]));
    setArg(ctx, 1, static_cast<uint32_t>(timeNear(state)));
}

namespace {

// Weapons held and suffered by car n, with effects the original VfxAddNew makes.
void varyWeapons(uint64_t &state, uint32_t car)
{
    _sObject &o = mem<_sObject>(car);
    static const int16 kAffects[] = {0, 5, 5, 7, 7, 0xd, 0xd, 1, 9};
    o.weaponAffect = kAffects[rnd(state, std::size(kAffects))];
    o.waterEscape = static_cast<int8>(rnd(state, 10));
    o.lightningSfx = static_cast<int8>(chance(state, 50) ? -1 : rnd(state, 0x30));
    o.warningLockonSfx = static_cast<int8>(chance(state, 50) ? -1 : rnd(state, 0x30));
    o.weaponsHeldSfx = static_cast<int16>(chance(state, 50) ? -1 : rnd(state, 0x30));
    for (uint32_t offset : {0x1b8u, 0x1bcu, 0x1c0u, 0x1c4u})
        if (chance(state, 40))
            mem<uint32_t>(car + offset) = chance(state, 30) ? 0u : guest_call<uint32_t>(kVfxAddNew, ptr32<uint8>(car), 0, 0, 0x26);
    o.weaponsHeld = static_cast<int8>(rnd(state, 3));
    o.weaponsHeldType = static_cast<int8>(chance(state, 30) ? 0x10 : chance(state, 30) ? 5 : rnd(state, 17));
    o.weaponsHeldNext = static_cast<int8>(rnd(state, 3));
    static const int8 kNext[] = {1, 5, 8, 0x10, 2, 3};
    o.weaponsHeldTypeNext = kNext[rnd(state, std::size(kNext))];
    o.weaponFiring = static_cast<int8>(chance(state, 70) ? 0 : 1);
    o.tipAngle = static_cast<int32_t>(mem<uint32_t>(kMainVars + 0x1cu)) + static_cast<int32_t>(rnd(state, 200)) - 100;
    o.weaponType = static_cast<uint8>(rnd(state, 17));
}

} // namespace

void objectWeapons(R5900Context *ctx, uint64_t &state, uint32_t)
{
    soundOff();
    for (uint32_t i = 0; i < noObjects(); ++i)
        if (usableCar(objectAt(i)) && chance(state, 50))
            varyWeapons(state, objectAt(i));
    const uint32_t car = objectAt(pickCar(state));
    varyWeapons(state, car);
    setArg(ctx, 0, car);
}

void objectWeaponList(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    soundOff();
    const uint32_t car = objectAt(pickCar(state));
    varyWeapons(state, car);
    const uint32_t count = rnd(state, 8);
    for (uint32_t i = 0; i < count; ++i)
    {
        const uint32_t w = scratch + kWeapons + i * 0x140u;
        _sWeapon &weapon = mem<_sWeapon>(w);
        weapon.no = static_cast<int16>(chance(state, 50) ? 5 : rnd(state, 17));
        weapon.misc = static_cast<int16>(chance(state, 60) ? 1 : rnd(state, 3));
        weapon.owner = static_cast<int16>(pickCar(state));
        weapon.target = static_cast<int16>(chance(state, 40) ? mem<_sObject>(car).playerNo : rnd(state, noObjects() + 1));
        weapon.timeWhenEnds = static_cast<int32_t>(nextFuzz(state) | 1u);
        weapon.pNext = ptr32<_sWeapon>(i + 1 < count && !chance(state, 10) ? w + 0x140u : 0u);
    }
    sWeaponClass &weapons = G(weaponVarG);
    weapons.pList = ptr32<_sWeapon>(count ? scratch + kWeapons : 0u);
    weapons.noWeapons = static_cast<int32>(count) + static_cast<int32>(rnd(state, 3)) - 1;
    if (!count)
        weapons.noWeapons = 0;
    setArg(ctx, 0, car);
}

void objectChangeOver(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    soundOff();
    const uint32_t car = objectAt(pickCar(state));
    _sObject &o = mem<_sObject>(car);
    uint32_t boat = boatAt(o.playerNo);
    if (!usableCar(boat) || chance(state, 30))
    {
        // A boat made of a copy of the car, its definition one with the boat's type.
        boat = buildObject(state, scratch, car);
        const uint32_t def = buildDef(state, scratch, car);
        _sObjectDef &d = mem<_sObjectDef>(def);
        d.type = static_cast<eObjectType>((static_cast<uint32_t>(o.pDef->type) + 0x13u) % 0x27u + 1u);
        mem<_sObject>(boat).pDef = ptr32<_sObjectDef>(def);
        mem<_sObject>(boat).pAerialNode = ptr32<_sNode>(nextFuzz(state));
        mem<uint32_t>(kObjectList + 0xb8u + 4u * o.playerNo) = boat;
    }
    if (chance(state, 40))
        mem<uint32_t>(boat + 0x34u) = nextFuzz(state); // its shadow
    mainClass &main = G(mainVarG);
    if (chance(state, 30))
        main.toDo = static_cast<eMainToDo>(chance(state, 50) ? 1 : rnd(state, 5));
    if (chance(state, 30))
        main.gameEndPos = static_cast<eGameEndPos>(rnd(state, 2));
    if (chance(state, 30))
        main.splitScreen = static_cast<uint8>(rnd(state, 2));
    if (chance(state, 40))
        o.playerType = static_cast<uint8>(rnd(state, 3));
    if (chance(state, 30))
        G(frontEndVarG).active = static_cast<uint8>(rnd(state, 3));
    setArg(ctx, 0, car);
    setArg(ctx, 1, rnd(state, 4));
}

namespace {

int32_t soundHandle(uint64_t &state) { return chance(state, 50) ? -1 : static_cast<int32_t>(rnd(state, 0x30)); }

// A car's sounds and the physics they are made from: speed, wheels on the ground, materials, throttle.
void varySounds(uint64_t &state, uint32_t car)
{
    _sObject &o = mem<_sObject>(car);
    _sAllObjSounds &s = *o.pSounds;
    for (sObjSound *snd : {&s.engine[0], &s.engine[1], &s.scrape[0], &s.scrape[1], &s.skid[0], &s.skid[1], &s.rocket[0], &s.rocket[1]})
    {
        if (chance(state, 50))
            snd->handle = soundHandle(state);
        if (chance(state, 30))
            snd->pitch = static_cast<int16>(rnd(state, 0x400));
        if (chance(state, 30))
            snd->volume = static_cast<int16>(rnd(state, 0x100));
    }
    if (chance(state, 50))
        s.material.handle = soundHandle(state);
    if (chance(state, 50))
        s.material.sample = static_cast<int16>(chance(state, 50) ? 0xff : rnd(state, 0x60));
    if (chance(state, 50))
        s.material.oldMaterial = static_cast<int16>(rnd(state, 0x20));
    if (chance(state, 30))
    {
        s.engineAccel = static_cast<int16>(chance(state, 20) ? 0 : rnd(state, 25000));
        s.enginePitchModify = static_cast<int16>(rnd(state, 0x200));
        s.engineMaxInAir = static_cast<int16>(rnd(state, 0x600));
    }
    if (chance(state, 40))
    {
        s.enginePitchRate = static_cast<int16>(rnd(state, 8));
        s.engineOldPitch = static_cast<int16>(rnd(state, 0x100));
    }
    if (chance(state, 50))
    {
        // Within earshot of the camera, so RevSfxPlay3D's result (the handle, or -1) depends on the call made.
        const vec32 &eye = mem<vec32>(sym::cameraVarG + 0xcu);
        _sRendObj &r = *o.pRendObj;
        r.pos.x = eye.x + static_cast<int32_t>(rnd(state, 0x40000)) - 0x20000;
        r.pos.y = eye.y + static_cast<int32_t>(rnd(state, 0x40000)) - 0x20000;
        r.pos.z = eye.z + static_cast<int32_t>(rnd(state, 0x40000)) - 0x20000;
    }
    _sFPhysLibCar &p = *o.pPhysics;
    if (chance(state, 60))
        p.speed = static_cast<float>(rnd(state, 30000)) * (chance(state, 10) ? -0.1f : 1.0f) + frnd(state, 1.0f);
    for (int w = 0; w < 4; ++w)
    {
        if (chance(state, 40))
            p.wheel[w].flags = static_cast<uint16>(nextFuzz(state));
        static const int16 kMaterials[] = {2, 3, 4, 7, 10, 11, 16, 17, 0x1d, 5, 0};
        if (chance(state, 40))
            p.wheel[w].drivingOnMtl = chance(state, 70) ? kMaterials[rnd(state, std::size(kMaterials))] : static_cast<int16>(rnd(state, 0x45));
        if (chance(state, 20))
            p.wheel[w].oilyCounter = static_cast<int16>(nextFuzz(state));
    }
    if (chance(state, 40))
        p.inputEngineThrottle = static_cast<int16>(static_cast<int32_t>(rnd(state, 0x200)) - 0x100);
    static const int16 kSteers[] = {100, 101, -100, -101, 0x40, 0x41, -0x40, -0x41, 0};
    if (chance(state, 75))
    {
        p.wheel[0].flags = static_cast<uint16>(p.wheel[0].flags | 3u);
        p.wheel[3].flags = static_cast<uint16>(p.wheel[3].flags | 3u);
    }
    if (chance(state, 60))
        p.inputSteerAngle = chance(state, 70) ? kSteers[rnd(state, std::size(kSteers))] : static_cast<int16>(static_cast<int32_t>(rnd(state, 0x200)) - 0x100);
    if (chance(state, 40) && inRam(p.pSphere.address))
        mem<float>(p.pSphere.address + 0x528u) = frnd(state, 50.0f);
    if (chance(state, 70))
        o.playerType = static_cast<uint8>(chance(state, 70) ? rnd(state, 2) : 2);
    if (chance(state, 70))
    {
        // The dumped races are mostly boats: cars (below 0x14) are made common here.
        static const uint32_t kTypes[] = {1, 5, 8, 0xf, 0x13, 0x14, 0x19, 0x22, 0x26, 0xe, 0x11, 3, 7, 2, 0xc};
        mem<uint32_t>(o.pDef.address + 4u) = kTypes[rnd(state, std::size(kTypes))];
    }
    if (chance(state, 30))
        o.skidOil = chance(state, 30) ? 0 : timeNear(state) + static_cast<int32_t>(rnd(state, 0x2000)) - 0x1000;
}

} // namespace

void objectCar(R5900Context *ctx, uint64_t &state, uint32_t)
{
    soundOff();
    varyCars(state);
    const uint32_t car = objectAt(pickCar(state));
    varySounds(state, car);
    if (chance(state, 30))
        mem<int32_t>((sym::hudVarG + 0x70u)) = static_cast<int32_t>(rnd(state, 2)); // hudVarG.iReadySteadyGo
    if (chance(state, 30))
    {
        mem<int32_t>(sym::revSfxVarG + 0x1160u) = static_cast<int32_t>(rnd(state, 2)); // revSfxVarG.startFade
        mem<int32_t>(sym::revSfxVarG + 0x1164u) = static_cast<int32_t>(rnd(state, 10000));
    }
    SET_GPR_S32(ctx, 18, static_cast<int32_t>(nextFuzz(state)));
    static const int32_t kFlags[] = {-1, 0, 0, 1};
    setArg(ctx, 0, car);
    setArg(ctx, 1, static_cast<uint32_t>(kFlags[rnd(state, std::size(kFlags))]));
    setArg(ctx, 2, static_cast<uint32_t>(timeNear(state)));
}

// a0 a real car whose outer spheres touch polys (copies in scratch with chosen materials) and cars.
void objectScrape(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    soundOff();
    const uint32_t car = objectAt(pickCar(state));
    varySounds(state, car);
    _sObject &o = mem<_sObject>(car);
    const uint32_t spheres = o.pCarCol->pSphere.address;
    const uint32_t count = o.pCarCol->numSpheres;
    static const uint8_t kMaterials[] = {0x20, 5, 7, 0xc, 0x10, 0x11, 0x21, 0x27, 0x29, 0x31, 0x33, 0x34, 0x39, 0x42, 0x43, 0x1, 0x2};
    for (uint32_t i = 0; i < 16; ++i)
    {
        const uint32_t poly = scratch + 0x3000u + i * 0x60u;
        mem<uint8_t>(poly + 0x53u) = kMaterials[rnd(state, std::size(kMaterials))];
    }
    for (uint32_t n = 4; n < count && n < 32; ++n)
    {
        const uint32_t sphere = spheres + n * 0x190u;
        if (!chance(state, 60))
            continue;
        const uint32_t cols = rnd(state, 4);
        mem<int8_t>(sphere + 0x67u) = static_cast<int8_t>(cols);
        for (uint32_t c = 0; c < cols; ++c)
        {
            mem<int8_t>(sphere + 0x94u + c) = static_cast<int8_t>(chance(state, 45) ? 1 : chance(state, 60) ? 2 : rnd(state, 4));
            mem<uint32_t>(sphere + 0x160u + c * 4u) = scratch + 0x3000u + rnd(state, 16) * 0x60u;
            mem<float>(sphere + 0xa4u + c * 16u) = chance(state, 40) ? 1000.0f + static_cast<float>(rnd(state, 3000)) : frnd(state, 4096.0f);
            mem<float>(sphere + 0xacu + c * 16u) = chance(state, 20) ? 9000.0f : frnd(state, 4000.0f);
        }
        for (uint32_t k = 0; k < 3; ++k)
            mem<float>(sphere + 4u * k) = mem<float>(sphere + 4u * k) + frnd(state, 500.0f);
    }
    if (chance(state, 50))
        o.pPhysics->mat.Yy = chance(state, 50) ? 4096.0f : frnd(state, 4096.0f);
    setArg(ctx, 0, car);
    setArg(ctx, 1, rnd(state, 2));
}

// a0 a real car that may just have passed another (places, the car passed near or far), its taunt in any state.
void objectOvertaking(R5900Context *ctx, uint64_t &state, uint32_t)
{
    soundOff();
    const int32_t time = timeNear(state);
    for (uint32_t i = 0; i < noObjects(); ++i)
        if (usableCar(objectAt(i)) && chance(state, 50))
        {
            _sObject &c = mem<_sObject>(objectAt(i));
            c.iRacePos = static_cast<int16>(1 + rnd(state, 8));
            if (chance(state, 40))
            {
                const _sObject &first = mem<_sObject>(objectAt(0));
                c.pos16.x = first.pos16.x.v + frnd(state, 2500.0f);
                c.pos16.y = first.pos16.y.v + frnd(state, 300.0f);
                c.pos16.z = first.pos16.z.v + frnd(state, 2500.0f);
            }
        }
    const uint32_t car = objectAt(pickCar(state));
    _sObject &o = mem<_sObject>(car);
    o.playerType = static_cast<uint8>(rnd(state, 3));
    o.iRacePos = static_cast<int16>(1 + rnd(state, 8));
    o.iRacePosAverage = static_cast<int16>(1 + rnd(state, 8));
    o.overtakingPrev = static_cast<int8>(1 + rnd(state, 9));
    o.overtakingTimerStart = time + static_cast<int32_t>(rnd(state, 200)) - 100;
    o.overtakingTimer = chance(state, 55) ? time : chance(state, 20) ? 0 : time + static_cast<int32_t>(rnd(state, 400)) - 200;
    o.overtakingSfx = static_cast<int8>(chance(state, 50) ? -1 : rnd(state, 0x30));
    o.overtakingMisc = static_cast<int8>(chance(state, 20) ? 0 : rnd(state, 8));
    o.weaponFiring = static_cast<int8>(rnd(state, 2));
    o.weaponsHeldType = static_cast<int8>(chance(state, 50) ? 0xc : rnd(state, 17));
    if (chance(state, 50))
        o.pVfxSarge = ptr32<_sVfx_Object>(chance(state, 30) ? 0u : guest_call<uint32_t>(kVfxAddNew, ptr32<uint8>(car), 0, 0, 0x19));
    mem<uint32_t>(o.pDef.address + 4u) = rnd(state, 40);
    if (chance(state, 10))
        mem<int32_t>((sym::hudVarG + 0x70u)) = 1;
    if (chance(state, 5))
        G(objectVarG).noObjects = 0;
    SET_GPR_S32(ctx, 7, static_cast<int32_t>(objectAt(pickCar(state))));
    setArg(ctx, 0, car);
    setArg(ctx, 1, rnd(state, 2));
    setArg(ctx, 2, static_cast<uint32_t>(time));
}

// a0 a real car under a weapon's effect (its time running out or not), an oil squirt in scratch at any stage.
void objectAffect(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    soundOff();
    const int32_t time = timeNear(state);
    const uint32_t car = objectAt(pickCar(state));
    varyWeapons(state, car);
    _sObject &o = mem<_sObject>(car);
    static const int16 kAffects[] = {0, 1, 4, 5, 7, 8, 9, 0xd, 0xd, 0xf, 0xf, 2};
    o.weaponAffect = kAffects[rnd(state, std::size(kAffects))];
    o.weaponAffectTime = time + static_cast<int32_t>(rnd(state, 1200)) - 600;
    o.weaponType = static_cast<uint8>(chance(state, 40) ? 8 : rnd(state, 17));
    o.playerType = static_cast<uint8>(rnd(state, 3));
    if (o.weaponAffect == 0xd && (!o.pVfxHexed || chance(state, 50)))
        o.pVfxHexed = ptr32<_sVfx_Object>(guest_call<uint32_t>(kVfxAddNew, ptr32<uint8>(car), 0, 0, 0x26));
    if (o.pVfxHexed && chance(state, 60))
        mem<uint16_t>(o.pVfxHexed.address + 0x10u) = static_cast<uint16_t>(rnd(state, 0x40));
    if (chance(state, 40))
        o.pVfxTugBubble = ptr32<_sVfx_Object>(chance(state, 30) ? 0u : guest_call<uint32_t>(kVfxAddNew, ptr32<uint8>(car), 0, 0, 0x26));
    if (chance(state, 50))
    {
        const uint32_t squirt = scratch + 0x4000u;
        mem<int32_t>(squirt + 0x1d4u) = static_cast<int32_t>(rnd(state, 4));
        mem<int16_t>(squirt + 0x1c8u) = static_cast<int16_t>(ee_ftoi(o.pos16.x) + static_cast<int32_t>(rnd(state, 200)) - 100);
        mem<int16_t>(squirt + 0x1cau) = static_cast<int16_t>(ee_ftoi(o.pos16.y));
        mem<int16_t>(squirt + 0x1ccu) = static_cast<int16_t>(ee_ftoi(o.pos16.z) + static_cast<int32_t>(rnd(state, 200)) - 100);
        o.pVfxOilSquirt = ptr32<_sVfx_Object>(squirt);
    }
    else
        o.pVfxOilSquirt = ptr32<_sVfx_Object>(0u);
    if (chance(state, 40))
        o.pPhysics->inputEngineThrottle = static_cast<int16>(static_cast<int32_t>(rnd(state, 0x2000)) - 0x1000);
    if (chance(state, 5))
        G(mainVarG).status = static_cast<eMain>(0x20);
    setArg(ctx, 0, car);
    setArg(ctx, 1, static_cast<uint32_t>(time));
}

void objectFrontEnd(R5900Context *ctx, uint64_t &state, uint32_t)
{
    soundOff();
    const uint32_t segments = 1 + rnd(state, 5);
    mem<uint32_t>(sym::carouselVarG + 0x1cu) = segments;
    for (uint32_t i = 0; i < segments; ++i)
    {
        const uint32_t car = objectAt(pickCar(state));
        varySounds(state, car);
        mem<uint32_t>(sym::carouselVarG + 0x2cu + i * 0x34u) = car;
    }
    setArg(ctx, 0, rnd(state, 6));
}
void objectCreateCars(R5900Context *ctx, uint64_t &state, uint32_t)
{
    soundOff();
    sCarListClass &list = G(carListVarG);
    const uint32_t cars = std::min<uint32_t>(noObjects(), 8u);
    uint32_t first = 0;
    for (uint32_t i = 0; i < cars; ++i)
        if (inRam(list.car[i].pDef.address))
            first = list.car[i].pDef.address;
    if (!first)
        first = mem<_sObject>(objectAt(pickCar(state))).pDef.address;
    for (uint32_t i = 0; i < 8; ++i)
    {
        if (!inRam(list.car[i].pDef.address))
            list.car[i].pDef = ptr32<_sObjectDef>(first);
        if (!inRam(list.boat[i].pDef.address) || chance(state, 20))
            list.boat[i].pDef = list.car[rnd(state, 8)].pDef;
        list.car[i].altRendObj = static_cast<int32>(chance(state, 70) ? 0 : 1);
        list.boat[i].altRendObj = static_cast<int32>(chance(state, 70) ? 0 : 1);
    }
    const uint32_t players = rnd(state, 3);
    G(playerVarG).noPlayers = static_cast<int32>(players);
    G(aiVarG).noAIPlayers = static_cast<int32>(1 + rnd(state, 8 - players));
    mainClass &main = G(mainVarG);
    main.noHuman = static_cast<uint8>(rnd(state, 3));
    if (chance(state, 60))
    {
        static const uint8_t kLevels[] = {5, 8, 12, 9, 11, 17, 20, 0x61, 0x15, 0x14};
        main.level = kLevels[rnd(state, std::size(kLevels))];
    }
    main.splitScreen = static_cast<uint8>(chance(state, 70) ? 0 : 1);
    G(objectVarG).noObjects = 0;
    if (chance(state, 15) && inRam(G(pLvlG).address))
        mem<uint32_t>(G(pLvlG).address + 0x30u) = 0;
    setArg(ctx, 0, static_cast<uint32_t>(timeNear(state)));
}

// a0 a real car set up to be drawn: invincibility, lights and their facing, wheel spin and blur levels, the dump tug's
// sled (a second render object in scratch when the car has none) and rock, the six-wheeler's middle pair (a wheel
// table with six lists in scratch), the front end's transposed matrix.
void objectRender(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    soundOff();
    varyCars(state);
    const uint32_t car = objectAt(pickCar(state));
    _sObject &o = mem<_sObject>(car);
    const uint32_t physics = o.pPhysics.address;
    if (chance(state, 50))
        o.invincible = static_cast<int8>(nextFuzz(state));
    if (chance(state, 30))
        o.playerType = static_cast<uint8>(rnd(state, 4));
    if (chance(state, 30))
        o.lockOnMe = static_cast<int8>(rnd(state, 2));
    if (chance(state, 60))
    {
        o.lightsOn = static_cast<int8>(rnd(state, 4));
        o.lightsPresent = static_cast<int8>(chance(state, 70) ? 3 : rnd(state, 4));
        o.bodgeRearLightOn = static_cast<int8>(rnd(state, 3));
        o.bodgeHeadLightOn = static_cast<int8>(rnd(state, 3));
    }
    if (chance(state, 30))
    {
        const int lamp = static_cast<int>(rnd(state, 4));
        if (chance(state, 50))
            o.rearLight[lamp][0].z = 0.0f;
        else
            o.headLight[lamp][0].z = 0.0f;
    }
    if (chance(state, 30))
        for (int i = 0; i < 4; ++i)
        {
            o.rearLightFlareSize[i] = frnd(state, 4000.0f);
            o.headLightFlareSize[i] = frnd(state, 4000.0f);
        }
    mem<int8_t>(physics + 0x1ceu) = static_cast<int8_t>(chance(state, 75) ? 1 : rnd(state, 3)); // mode
    if (chance(state, 30))
        mem<uint8_t>(physics + 0x1cdu) = static_cast<uint8_t>(nextFuzz(state));
    if (chance(state, 30))
    {
        mem<int32_t>(kMainVars + 8u) = 1; // currentDo: the front end
        mem<int32_t>((sym::visFxVarG + 0x2730u)) = static_cast<int32_t>(rnd(state, 2)); // visFxVarG.iResultTrigger
    }
    if (chance(state, 30))
        mem<int32_t>(kMainVars + 0x378u) = static_cast<int32_t>(rnd(state, 2)); // iWhichSplitScreen
    if (chance(state, 30))
        o.zShortestDrawnDist = static_cast<uint16>(nextFuzz(state));
    if (chance(state, 30))
        o.zDrawnDist = static_cast<uint16>(chance(state, 50) ? rnd(state, 2000) : nextFuzz(state));
    if (chance(state, 20))
        mem<int32_t>(sym::cameraVarG + 0x5b8u * rnd(state, 2)) = static_cast<int32_t>(rnd(state, 3)); // camera mode
    if (chance(state, 50))
        for (uint32_t i = 0; i < 4; ++i)
        {
            mem<float>(physics + 0xe0u + i * 0x48u) = frnd(state, chance(state, 50) ? 100.0f : 100000.0f);
            mem<float>(physics + 0xe4u + i * 0x48u) = frnd(state, chance(state, 50) ? 100.0f : 100000.0f);
        }
    if (chance(state, 40))
    {
        mem<uint32_t>(sym::ObjectBlurMaxVel) = rnd(state, 60); // ObjectBlurMaxVel
        mem<uint32_t>(sym::ObjectBlurMedVel) = rnd(state, 30); // ObjectBlurMedVel
    }
    if (chance(state, 30)) // spins on the blur thresholds
        for (uint32_t i = 0; i < 4; ++i)
        {
            const uint32_t edge = mem<uint32_t>(chance(state, 50) ? sym::ObjectBlurMaxVel : sym::ObjectBlurMedVel) + rnd(state, 3) - 1;
            mem<float>(physics + 0xe0u + i * 0x48u) = static_cast<float>(static_cast<int32_t>(edge)) * (chance(state, 50) ? 1.0f : -1.0f);
        }
    if (chance(state, 40))
    {
        // The camera straight ahead of or behind the car (its z column), so the lights face it squarely.
        const float *m = &mem<float>(physics + 0x4cu);
        const float cx = m[2], cy = m[5], cz = m[8];
        const float n = std::sqrt(cx * cx + cy * cy + cz * cz);
        if (n > 0.0f)
        {
            const float side = chance(state, 50) ? 1.0f : -1.0f, dist = 5.0f + static_cast<float>(rnd(state, 60));
            const float spread = chance(state, 50) ? 0.02f : 0.3f;
            float *cam = &mem<float>(sym::rdrCameraG + 0xbcu);
            for (int k = 0; k < 3; ++k)
            {
                const float c = k == 0 ? cx : k == 1 ? cy : cz;
                cam[k] = mem<float>(physics + 8u + 4u * static_cast<uint32_t>(k)) / 256.0f + (side * c / n + frnd(state, spread)) * dist;
            }
        }
    }
    if (chance(state, 30))
        for (int b = 0x50; b < 0x54; ++b)
            mem<uint8_t>(o.pDef.address + static_cast<uint32_t>(b)) = static_cast<uint8_t>(chance(state, 20) ? 0xff : nextFuzz(state));
    // The wheel lists: a copy with six, the last two the rear pair's.
    const uint32_t res = scratch + 0x3000u;
    if (inRam(o.pWheels.address) && inRam(o.pWheels->pWheelRes.address))
    {
        std::memcpy(guest_host(res), guest_host(o.pWheels->pWheelRes.address), 16);
        mem<uint32_t>(res + 16) = mem<uint32_t>(res + 8);
        mem<uint32_t>(res + 20) = mem<uint32_t>(res + 12);
        // The dumped cars have one blur level per wheel: lists of up to five (copies of the wheel's part) in scratch.
        if (chance(state, 70))
            for (uint32_t i = 0; i < 6; ++i)
            {
                const uint32_t real = mem<uint32_t>(res + 4 * i);
                if (!inRam(real) || !inRam(mem<uint32_t>(real + 4)))
                    continue;
                const uint32_t list = scratch + 0x3200u + i * 0x20u, count = 1 + rnd(state, 5);
                mem<uint32_t>(list) = count;
                for (uint32_t j = 0; j < count; ++j)
                {
                    const uint32_t head = scratch + 0x3400u + (i * 5 + j) * 0x80u;
                    std::memcpy(guest_host(head), guest_host(mem<uint32_t>(real + 4)), 0x74);
                    mem<uint32_t>(list + 4 + 4 * j) = head;
                }
                mem<uint32_t>(res + 4 * i) = list;
            }
        o.pWheels->pWheelRes = ptr32<_sWheelRes>(res);
    }
    const uint32_t r = rnd(state, 100);
    if (r < 20)
        o.pDef->type = static_cast<eObjectType>(0x12);
    else if (r < 30)
        o.pDef->type = static_cast<eObjectType>(0x11);
    else if (r < 35)
        o.pDef->type = static_cast<eObjectType>(0xf);
    else if (r < 40)
        o.pDef->type = static_cast<eObjectType>(0x14 + rnd(state, 19));
    if (!o.pRendObj2 && chance(state, 60))
    {
        const uint32_t sled = scratch + 0x3100u;
        std::memcpy(guest_host(sled), guest_host(o.pRendObj.address), 0x38);
        o.pRendObj2 = ptr32<_sRendObj>(sled);
    }
    if (chance(state, 50))
        o.weaponType = 0xf;
    if (chance(state, 60))
        o.tipAngle = static_cast<int32>(nextFuzz(state));
    if (!inRam(mem<uint32_t>(kObjectList + 0x17cu)))
        mem<uint32_t>(kObjectList + 0x17cu) = o.pRendObj->pRendObjHead.address;
    setArg(ctx, 0, car);
}

} // namespace rc::fuzz
