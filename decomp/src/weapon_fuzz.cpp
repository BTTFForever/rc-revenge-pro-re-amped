// Builds the weapons' random-test inputs. See weapon_fuzz.h.
//
// The whole-RAM setups (most of them) change the dump freely: RAM is put back after every input. The live weapons are
// made by the recompiled WeaponSpawn (a setup runs while comparing, so guest calls reach the originals).

#include "weapon_fuzz.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>

namespace rc::fuzz {

namespace {

constexpr uint32_t kObjectList = 0x1ee390u, kWeaponVars = 0x1f3a80u, kSfxInitOk = 0x31e488u, kMainTime = 0x1ed91cu;
constexpr uint32_t kWeaponSpawn = 0x17a8d0u, kTaskDelete = 0x1afc48u, kVfxAddNew = 0x157c50u;
constexpr uint32_t kTriggerState = 0x1eb778u;

// Scratch layout.
constexpr uint32_t kWeapon = 0x0000u, kDef = 0x0200u, kPos = 0x0240u, kNormal = 0x0260u;
constexpr uint32_t kCarA = 0x1000u, kCarB = 0x1800u, kRendA = 0x2000u, kRendB = 0x2100u;

template <typename T>
T &mem(uint32_t address) { return *static_cast<T *>(guest_host(address)); }

uint32_t rnd(uint64_t &state, uint32_t n) { return n == 0 ? 0u : nextFuzz(state) % n; }
bool chance(uint64_t &state, uint32_t percent) { return rnd(state, 100) < percent; }
float frnd(uint64_t &state, float scale) { return (static_cast<float>(static_cast<int32_t>(nextFuzz(state) % 20001u) - 10000) / 10000.0f) * scale; }

void setArg(R5900Context *ctx, int index, uint32_t value) { SET_GPR_S32(ctx, 4 + index, static_cast<int32_t>(value)); }

uint32_t noObjects() { return mem<uint32_t>(kObjectList + 0x170u); }
uint32_t objectAt(uint32_t n) { return mem<uint32_t>(kObjectList + 4u * n); }

bool inRam(uint32_t address) { return address != 0 && address < 0x2000000u; }

// A car of the race: an object with a render object, physics, its integer state and a definition.
bool usableCar(uint32_t car)
{
    if (!inRam(car))
        return false;
    const _sObject &o = mem<_sObject>(car);
    return inRam(o.pRendObj.address) && inRam(o.pPhysics.address) && inRam(o.pDef.address) &&
           inRam(mem<uint32_t>(o.pPhysics.address + 0x1e8u)) && inRam(o.pVfx_Aerial.address) && inRam(o.pVfx_SkidLeft.address) &&
           inRam(o.pVfx_SkidRight.address);
}

int32_t pickCar(uint64_t &state)
{
    const uint32_t count = noObjects();
    for (int tries = 0; tries < 30 && count; ++tries)
    {
        const uint32_t n = rnd(state, count);
        if (usableCar(objectAt(n)))
            return static_cast<int32_t>(n);
    }
    return 0;
}

void soundOff() { mem<uint8_t>(kSfxInitOk) = 0; }

int32_t timeNear(uint64_t &state) { return static_cast<int32_t>(mem<uint32_t>(kMainTime)) + static_cast<int32_t>(rnd(state, 0x4000)); }

// Varies the cars the weapons touch: their state of being hit, held weapons, positions near one another.
void varyCars(uint64_t &state)
{
    const uint32_t count = noObjects();
    for (uint32_t i = 0; i < count; ++i)
    {
        const uint32_t car = objectAt(i);
        if (!usableCar(car))
            continue;
        _sObject &o = mem<_sObject>(car);
        if (chance(state, 30))
            o.invincible = static_cast<int8>(chance(state, 70) ? 1 : 2);
        if (chance(state, 40))
        {
            static const int16 kAffects[] = {0, 0, 1, 4, 5, 7, 8, 9, 0xd, 0xf};
            o.weaponAffect = kAffects[rnd(state, std::size(kAffects))];
        }
        if (chance(state, 30))
            o.weaponAffectOwner = static_cast<int16>(chance(state, 30) ? 0xff : rnd(state, count));
        if (chance(state, 30))
            o.shockWaved = static_cast<int16>(rnd(state, 3));
        if (chance(state, 30))
        {
            o.weaponsHeld = static_cast<int8>(rnd(state, 3));
            o.weaponsHeldType = static_cast<int8>(rnd(state, 17));
        }
        if (chance(state, 20))
            o.weaponType = static_cast<uint8>(rnd(state, 17));
        if (chance(state, 20))
            o.heatrayStore = static_cast<int16>(chance(state, 50) ? 5000 : rnd(state, 6000));
        if (chance(state, 20))
            o.heatrayStoreAngle = static_cast<int16>(rnd(state, 0x10000));
        if (chance(state, 40))
            o.weaponsHeldSfx = -1;
        if (chance(state, 30))
            o.lockOnMe = static_cast<int8>(chance(state, 30) ? ' ' : rnd(state, 4));
        if (chance(state, 30))
            o.weaponLock = static_cast<uint8>(rnd(state, count + 1));
        if (chance(state, 20))
            o.playerType = static_cast<uint8>(rnd(state, 4));
        if (chance(state, 25))
        {
            // Brought near car 0, so the short reaches (1000, 2000, 5000) are met.
            const _sObject &first = mem<_sObject>(objectAt(0));
            o.pos16.x = first.pos16.x.v + frnd(state, 1500.0f);
            o.pos16.z = first.pos16.z.v + frnd(state, 1500.0f);
            o.pos16.y = first.pos16.y.v + frnd(state, 300.0f);
        }
        if (chance(state, 15))
            o.skidOil = 0;
    }
}

// A weapon definition at scratch+kDef and a position at scratch+kPos (the owner's).
void buildDef(uint64_t &state, uint32_t scratch, int no, int32_t owner)
{
    _sWeaponDef &def = mem<_sWeaponDef>(scratch + kDef);
    std::memset(&def, 0, sizeof(def));
    def.no = static_cast<int16>(no);
    def.specialFire = static_cast<int16>(chance(state, 70) ? 0 : rnd(state, 3));
    def.owner = static_cast<int16>(owner);
    const uint32_t count = noObjects();
    def.target = static_cast<int16>(chance(state, 40) ? 0x7f : chance(state, 30) ? rnd(state, 3) : rnd(state, count));
    def.lifetime = static_cast<int16>(rnd(state, 10000));
    def.rad = static_cast<int16>(10 + rnd(state, 60));
    const _sObject &car = mem<_sObject>(objectAt(static_cast<uint32_t>(owner)));
    vec16_ &pos = mem<vec16_>(scratch + kPos);
    pos.x = static_cast<int16>(ee_ftoi(car.pos16.x) + static_cast<int32_t>(rnd(state, 40)) - 20);
    pos.y = static_cast<int16>(ee_ftoi(car.pos16.y) + static_cast<int32_t>(rnd(state, 40)) - 20);
    pos.z = static_cast<int16>(ee_ftoi(car.pos16.z) + static_cast<int32_t>(rnd(state, 40)) - 20);
}

// The last weapon of the list (the newest), or 0.
uint32_t newestWeapon()
{
    uint32_t p = mem<uint32_t>(kWeaponVars + 4u);
    for (int guard = 0; p && guard < 64 && mem<uint32_t>(p + 0x134u); ++guard)
        p = mem<uint32_t>(p + 0x134u);
    return p;
}

// Spawns a weapon of kind `no` with the original WeaponSpawn; returns it, or 0 if none was made.
uint32_t spawnLive(uint64_t &state, uint32_t scratch, int no, int32_t owner, int32_t time)
{
    _sObject &car = mem<_sObject>(objectAt(static_cast<uint32_t>(owner)));
    car.weaponsHeld = static_cast<int8>(1 + rnd(state, 2));
    if (no == 1)
        car.weaponLock = static_cast<uint8>(chance(state, 50) ? 0 : 1 + rnd(state, noObjects()));
    buildDef(state, scratch, no, owner);
    const int32_t before = mem<int32_t>(kWeaponVars + 8u);
    guest_call(kWeaponSpawn, ptr32<uint8>(scratch + kPos), ptr32<uint8>(scratch + kDef), time);
    if (mem<int32_t>(kWeaponVars + 8u) == before)
        return 0;
    return newestWeapon();
}

// Varies a live weapon's state the way its Update finds it later in its life.
void varyLive(uint64_t &state, uint32_t weapon, int32_t time)
{
    _sWeapon &w = mem<_sWeapon>(weapon);
    if (chance(state, 50))
        w.sfxHandle1 = static_cast<int16>(chance(state, 50) ? -1 : rnd(state, 0x30));
    if (chance(state, 50))
        w.otherTimer = chance(state, 30) ? 0 : time + static_cast<int32_t>(rnd(state, 4000)) - 2000;
    if (chance(state, 30))
        w.timeWhenEnds = time + static_cast<int32_t>(rnd(state, 4000)) - 1000;
    if (chance(state, 40))
        w.misc = static_cast<int16>(rnd(state, 3));
    if (chance(state, 40))
        w.status = static_cast<int32>(chance(state, 50) ? rnd(state, 4) : time + static_cast<int32_t>(rnd(state, 4000)) - 2000);
    if (chance(state, 40))
    {
        w.destPos16.x = static_cast<int16>(rnd(state, 80) - 40);
        w.destPos16.y = static_cast<int16>(rnd(state, 80) - 40);
        w.destPos16.z = static_cast<int16>(rnd(state, 80) - 40);
    }
    if (chance(state, 40))
    {
        w.pos16.x = w.pos16.x.v + frnd(state, 200.0f);
        w.pos16.y = w.pos16.y.v + frnd(state, 200.0f);
        w.pos16.z = w.pos16.z.v + frnd(state, 200.0f);
    }
    if (chance(state, 30))
        w.specialFire = static_cast<int16>(rnd(state, 3));
    if (chance(state, 20))
        w.target = static_cast<int16>(chance(state, 50) ? 0x7f : rnd(state, noObjects()));
    if (w.pVfx && chance(state, 30))
        mem<uint16_t>(w.pVfx.address + 0x10u) = static_cast<uint16_t>(rnd(state, 30)); // iLifeSpan
    if (w.pVfx && chance(state, 30))
        mem<float>(w.pVfx.address + 0x58u) = static_cast<float>(rnd(state, 4000)); // the ring's radius
}

// A weapon record in scratch as WeaponCreate builds it, before the Setup.
void buildRecord(uint64_t &state, uint32_t scratch, int no, int32_t owner, int32_t time)
{
    buildDef(state, scratch, no, owner);
    const _sWeaponDef &def = mem<_sWeaponDef>(scratch + kDef);
    const vec16_ &pos = mem<vec16_>(scratch + kPos);
    _sWeapon &w = mem<_sWeapon>(scratch + kWeapon);
    std::memset(&w, 0, sizeof(w));
    w.no = static_cast<int16>(no);
    w.owner = def.owner;
    w.target = def.target;
    w.specialFire = def.specialFire;
    w.timeWhenEnds = time + def.lifetime;
    w.pos16.x = ee_itof(pos.x);
    w.pos16.y = ee_itof(pos.y);
    w.pos16.z = ee_itof(pos.z);
    w.aColrad = ee_itof(def.rad);
    w.oColrad = ee_itof(def.rad);
    w.oCol.type = 4;
    w.sfxHandle1 = -1;
    w.sfxHandle2 = -1;
    w.uniqueId = static_cast<int32>(rnd(state, 1000));
}

} // namespace

void weaponOnly(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    mem<_sWeapon>(scratch + kWeapon).oCol.updated = static_cast<bool_>(rnd(state, 3));
    setArg(ctx, 0, scratch + kWeapon);
    setArg(ctx, 1, nextFuzz(state));
}

void weaponTwoCars(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    for (uint32_t k = 0; k < 2; ++k)
    {
        const uint32_t car = scratch + (k ? kCarB : kCarA), rend = scratch + (k ? kRendB : kRendA);
        _sObject &o = mem<_sObject>(car);
        o.pRendObj = ptr32<_sRendObj>(rend);
        o.pos16.x = chance(state, 10) ? frnd(state, 5.0f) : frnd(state, 20000.0f);
        o.pos16.y = frnd(state, 2000.0f);
        o.pos16.z = chance(state, 10) ? frnd(state, 5.0f) : frnd(state, 20000.0f);
        // A rotation about y, as the cars' matrices mostly are, or random entries.
        int16_t *m = reinterpret_cast<int16_t *>(guest_host(rend + 0x18u));
        if (chance(state, 70))
        {
            const float a = frnd(state, 3.14159f);
            const int16_t c = static_cast<int16_t>(std::cos(a) * 4096.0f), s = static_cast<int16_t>(std::sin(a) * 4096.0f);
            const int16_t r[9] = {c, 0, static_cast<int16_t>(-s), 0, 4096, 0, s, 0, c};
            std::memcpy(m, r, sizeof(r));
        }
        else
            for (int i = 0; i < 9; ++i)
                m[i] = static_cast<int16_t>(rnd(state, 0x2001) - 0x1000);
    }
    setArg(ctx, 0, scratch + kCarA);
    setArg(ctx, 1, scratch + (chance(state, 5) ? kCarA : kCarB));
}

void weaponOneCar(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    soundOff();
    varyCars(state);
    const int32_t me = pickCar(state);
    _sObject &car = mem<_sObject>(objectAt(static_cast<uint32_t>(me)));
    car.iRacePos = static_cast<int16>(chance(state, 10) ? 0 : rnd(state, noObjects() + 1));
    car.carAI.iTotalRacedDist = static_cast<int32>(rnd(state, 20000));
    for (uint32_t i = 0; i < noObjects(); ++i)
        if (usableCar(objectAt(i)) && chance(state, 50))
            mem<_sObject>(objectAt(i)).carAI.iTotalRacedDist = car.carAI.iTotalRacedDist + static_cast<int32>(rnd(state, 16000)) - 4000;
    // Now and then a boat among the cars (or the car a boat), for the car-against-boat rule.
    if (chance(state, 20))
    {
        const uint32_t other = objectAt(rnd(state, noObjects()));
        if (usableCar(other))
            mem<uint32_t>(mem<uint32_t>(other + 0xcu) + 4u) = 0x14 + rnd(state, 0x14);
    }
    setArg(ctx, 0, objectAt(static_cast<uint32_t>(me)));
}

void weaponList(R5900Context *, uint64_t &state, uint32_t scratch)
{
    soundOff();
    varyCars(state);
    const int32_t time = timeNear(state);
    const uint32_t extra = rnd(state, 5);
    for (uint32_t i = 0; i < extra; ++i)
    {
        static const int kKinds[] = {1, 1, 2, 3, 4, 5, 7, 7, 8, 9, 9, 10, 11, 12, 13, 14, 15, 16};
        const uint32_t w = spawnLive(state, scratch, kKinds[rnd(state, std::size(kKinds))], pickCar(state), time);
        if (w)
            varyLive(state, w, time);
    }
    if (chance(state, 30))
        for (uint32_t i = 0; i < noObjects(); ++i)
            if (usableCar(objectAt(i)))
                mem<_sObject>(objectAt(i)).weaponType = static_cast<uint8>(chance(state, 80) ? 0 : 1);
}

void weaponShake(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    _sWeapon &w = mem<_sWeapon>(scratch + kWeapon);
    w.owner = static_cast<int16>(rnd(state, 4) - 1);
    w.misc = static_cast<int16>(chance(state, 70) ? rnd(state, 0x1a) : nextFuzz(state));
    setArg(ctx, 0, scratch + kWeapon);
    setArg(ctx, 1, nextFuzz(state));
}

void weaponBlast(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    soundOff();
    varyCars(state);
    const int32_t owner = pickCar(state);
    _sWeapon &w = mem<_sWeapon>(scratch + kWeapon);
    w.owner = static_cast<int16>(owner);
    for (uint32_t i = 0; i < noObjects(); ++i)
    {
        const uint32_t car = objectAt(i);
        if (!usableCar(car))
            continue;
        _sObject &o = mem<_sObject>(car);
        if (chance(state, 50))
            o.weaponAffectOwner = static_cast<int16>(owner);
        o.pVfxMisc = ptr32<_sVfx_Object>(nullptr);
        if (chance(state, 40))
            o.pVfxMisc = ptr32<_sVfx_Object>(guest_call<uint32_t>(kVfxAddNew, ptr32<uint8>(objectAt(static_cast<uint32_t>(owner))), car, 0, 0x11));
        o.lightningSfx = static_cast<int8>(rnd(state, 0x40) - 1);
        if (chance(state, 40))
            o.weaponAffect = 9;
    }
    setArg(ctx, 0, scratch + kWeapon);
}

void weaponInList(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    soundOff();
    varyCars(state);
    const int32_t time = timeNear(state);
    uint32_t w = 0;
    for (int tries = 0; tries < 4 && !w; ++tries)
    {
        const uint32_t extra = rnd(state, 3);
        for (uint32_t i = 0; i < extra; ++i)
            spawnLive(state, scratch, 1 + static_cast<int>(rnd(state, 16)), pickCar(state), time);
        w = spawnLive(state, scratch, 1 + static_cast<int>(rnd(state, 16)), pickCar(state), time);
    }
    if (!w)
        w = scratch + kWeapon; // nothing could be spawned: a record that is not in the list
    else
    {
        varyLive(state, w, time);
        // Most callers find the task popped (WeaponProcess runs from the task list; Remove from Process).
        if (chance(state, 70))
            guest_call(kTaskDelete, ptr32<uint8>(w + 0x10u));
        if (chance(state, 10))
            mem<int16_t>(w) = static_cast<int16_t>(rnd(state, 0x13)); // a kind with no Update, or none
    }
    setArg(ctx, 0, w);
    setArg(ctx, 1, static_cast<uint32_t>(time + static_cast<int32_t>(rnd(state, 3000))));
}

void weaponCreate(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    soundOff();
    varyCars(state);
    const int32_t owner = pickCar(state);
    const int no = chance(state, 5) ? static_cast<int>(rnd(state, 20)) : 1 + static_cast<int>(rnd(state, 16));
    buildDef(state, scratch, no == 6 ? 7 : no, owner); // oil comes through WeaponSpawn's squirt, not here
    mem<_sObject>(objectAt(static_cast<uint32_t>(owner))).weaponsHeld = static_cast<int8>(rnd(state, 3));
    if (chance(state, 20))
        mem<uint8_t>(0x1ed900u + 0x369u) = static_cast<uint8_t>(rnd(state, 3)); // mainVarG.noHuman
    setArg(ctx, 0, scratch + kPos);
    setArg(ctx, 1, static_cast<uint32_t>(timeNear(state)));
    setArg(ctx, 2, scratch + kDef);
}

void weaponSpawn(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    soundOff();
    varyCars(state);
    const int32_t owner = pickCar(state);
    buildDef(state, scratch, static_cast<int>(rnd(state, 0x15)), owner);
    mem<_sObject>(objectAt(static_cast<uint32_t>(owner))).weaponsHeld = static_cast<int8>(1 + rnd(state, 2));
    setArg(ctx, 0, scratch + kPos);
    setArg(ctx, 1, scratch + kDef);
    setArg(ctx, 2, static_cast<uint32_t>(timeNear(state)));
}

void weaponRender(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    soundOff();
    const int32_t time = timeNear(state);
    const bool rocks = mem<uint32_t>(kObjectList + 0x17cu) != 0; // a dump tug's rock needs its model
    const uint32_t count = 1 + rnd(state, 4);
    for (uint32_t i = 0; i < count; ++i)
    {
        const int no = rocks && chance(state, 40) ? 15 : chance(state, 20) ? 1 : 3;
        const uint32_t w = spawnLive(state, scratch, no, pickCar(state), time);
        if (!w)
            continue;
        _sWeapon &weapon = mem<_sWeapon>(w);
        weapon.misc = static_cast<int16>(rnd(state, 2));
        if (chance(state, 50))
            weapon.oCol.type = static_cast<uint8>(chance(state, 50) ? 4 : 3);
        if (no == 15)
            weapon.status = static_cast<int32>(rnd(state, 4));
    }
    mem<uint16_t>(0x1ed900u + 0x366u) = static_cast<uint16_t>(chance(state, 30) ? 1 : 0); // mainVarG.pauseMenu
    setArg(ctx, 0, chance(state, 50) ? 0u : 1u);
}

void weaponHitCar(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    soundOff();
    varyCars(state);
    const int32_t time = timeNear(state);
    static const int kKinds[] = {1, 3, 5, 5, 11, 11, 15, 15, 16, 2, 4, 7};
    const int32_t owner = pickCar(state);
    uint32_t w = spawnLive(state, scratch, kKinds[rnd(state, std::size(kKinds))], owner, time);
    if (!w)
        w = spawnLive(state, scratch, 3, owner, time);
    if (!w)
    {
        buildRecord(state, scratch, 3, owner, time);
        w = scratch + kWeapon;
    }
    varyLive(state, w, time);
    if (chance(state, 40))
        mem<_sWeapon>(w).status = 3;
    if (chance(state, 30))
        mem<_sWeapon>(w).otherTimer = time + static_cast<int32_t>(rnd(state, 0x8000)) - 0x4000;
    const int32_t victim = chance(state, 25) ? owner : pickCar(state);
    setArg(ctx, 0, w);
    setArg(ctx, 1, objectAt(static_cast<uint32_t>(victim)));
}

void weaponOil(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    soundOff();
    varyCars(state);
    const uint32_t car = objectAt(static_cast<uint32_t>(pickCar(state)));
    _sObject &o = mem<_sObject>(car);
    _sFPhysLibCar &physics = *o.pPhysics;
    // The slick's box: around the car's wheels (positions in 1/256 units), now and then off to one side.
    float minX = 1e30f, maxX = -1e30f, minZ = 1e30f, maxZ = -1e30f;
    for (const _sFPhysLibWheel &wheel : physics.wheel)
    {
        minX = std::min(minX, static_cast<float>(wheel.pos.x));
        maxX = std::max(maxX, static_cast<float>(wheel.pos.x));
        minZ = std::min(minZ, static_cast<float>(wheel.pos.z));
        maxZ = std::max(maxZ, static_cast<float>(wheel.pos.z));
    }
    _sWeapon &w = mem<_sWeapon>(scratch + kWeapon);
    const float shiftX = chance(state, 50) ? 0.0f : frnd(state, 2000.0f), shiftZ = chance(state, 50) ? 0.0f : frnd(state, 2000.0f);
    w.oCol.min.x = (minX + shiftX) / 256.0f - static_cast<float>(rnd(state, 4));
    w.oCol.max.x = (maxX + shiftX) / 256.0f + static_cast<float>(rnd(state, 4));
    w.oCol.min.z = (minZ + shiftZ) / 256.0f - static_cast<float>(rnd(state, 4));
    w.oCol.max.z = (maxZ + shiftZ) / 256.0f + static_cast<float>(rnd(state, 4));
    for (_sFPhysLibWheel &wheel : physics.wheel)
        if (chance(state, 20))
            wheel.flags = static_cast<uint16>(wheel.flags ^ 1u);
    if (chance(state, 25))
    {
        static const uint32_t kTypes[] = {7, 0x1a, 0xf, 0x22, 0x10, 0x14, 0x18, 0x26};
        mem<uint32_t>(o.pDef.address + 4u) = kTypes[rnd(state, std::size(kTypes))];
    }
    if (chance(state, 40))
        physics.pSphere[4].liquidDepth = chance(state, 50) ? frnd(state, 100.0f) : 0.0f;
    if (chance(state, 40))
    {
        physics.vel.x = frnd(state, 3000.0f);
        physics.vel.z = frnd(state, 3000.0f);
    }
    setArg(ctx, 0, scratch + kWeapon);
    setArg(ctx, 1, car);
}

// TestAcol reads the level's polys: the weapon is put next to a random poly of the level (on either side, or across),
// with a random radius, and the trigger doors open or shut. Only scratch and the trigger states change.
void weaponAcol(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    static bool saved = false;
    static uint32_t triggers[9];
    if (!saved)
    {
        std::memcpy(triggers, guest_host(kTriggerState), sizeof(triggers));
        saved = true;
    }
    for (uint32_t i = 0; i < 9; ++i)
        mem<uint32_t>(kTriggerState + 4u * i) = chance(state, 30) ? 1u : chance(state, 50) ? triggers[i] : 0u;
    const uint32_t grid = mem<uint32_t>(0x328200u);
    _sWeapon &w = mem<_sWeapon>(scratch + kWeapon);
    w.oColrad = chance(state, 10) ? frnd(state, 5.0f) : static_cast<float>(5 + rnd(state, 80));
    const uint32_t polys = mem<uint32_t>(grid + 0x30u);
    // Only next to the level's polys: AColCellIdFromPosUnbounded does not clamp, and a position off the grid reads a
    // cell beyond it whose poly counts are garbage (the original then runs for minutes).
    if (!inRam(grid) || polys == 0)
    {
        setArg(ctx, 0, scratch + kWeapon);
        setArg(ctx, 1, scratch + kNormal);
        return;
    }
    {
        const _sColPolyPSX &p = mem<_sColPolyPSX>(mem<uint32_t>(grid + 0x4cu) + rnd(state, polys) * 0x60u);
        const uint32_t a = rnd(state, 3), b = rnd(state, 3);
        const float t = static_cast<float>(rnd(state, 1000)) / 1000.0f;
        const auto vx = [&](uint32_t i) { return p.v[i].x.v; };
        const auto vy = [&](uint32_t i) { return p.v[i].y.v; };
        const auto vz = [&](uint32_t i) { return p.v[i].z.v; };
        const float cx = vx(a) + (vx(b) - vx(a)) * t, cy = vy(a) + (vy(b) - vy(a)) * t, cz = vz(a) + (vz(b) - vz(a)) * t;
        const float nx = p.n.x.v, ny = p.n.y.v, nz = p.n.z.v;
        const float side = frnd(state, 2.0f * w.oColrad.v + 10.0f);
        w.pos16.x = cx + nx * side + frnd(state, 5.0f);
        w.pos16.y = cy + ny * side + frnd(state, 5.0f);
        w.pos16.z = cz + nz * side + frnd(state, 5.0f);
        const float back = chance(state, 50) ? frnd(state, 150.0f) : -side * 2.0f;
        const float drift = chance(state, 50) ? 10.0f : 80.0f;
        w.oldPos16.x = w.pos16.x.v + nx * back + frnd(state, drift);
        w.oldPos16.y = w.pos16.y.v + ny * back + frnd(state, drift);
        w.oldPos16.z = w.pos16.z.v + nz * back + frnd(state, drift);
    }
    setArg(ctx, 0, scratch + kWeapon);
    setArg(ctx, 1, scratch + kNormal);
}

template <int No>
void weaponSetup(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    soundOff();
    varyCars(state);
    const int32_t time = timeNear(state);
    const int32_t owner = pickCar(state);
    if (No == 1)
        mem<_sObject>(objectAt(static_cast<uint32_t>(owner))).weaponLock = static_cast<uint8>(chance(state, 50) ? 0 : 1 + rnd(state, noObjects()));
    if (chance(state, 20))
        mem<_sObject>(objectAt(static_cast<uint32_t>(owner))).pVfxSarge = ptr32<_sVfx_Object>(nullptr);
    buildRecord(state, scratch, No, owner, time);
    setArg(ctx, 0, scratch + kWeapon);
    setArg(ctx, 1, static_cast<uint32_t>(time));
}

// Puts a weapon just above the ground under a car (its last down cast), falling, so that it lands or hits this step.
void nearGround(uint64_t &state, uint32_t weapon)
{
    const _sObject &car = mem<_sObject>(objectAt(static_cast<uint32_t>(pickCar(state))));
    _sWeapon &w = mem<_sWeapon>(weapon);
    w.pos16.x = car.poiUnderCentre.x.v + frnd(state, 100.0f);
    w.pos16.y = car.poiUnderCentre.y.v + 10.0f + frnd(state, 40.0f);
    w.pos16.z = car.poiUnderCentre.z.v + frnd(state, 100.0f);
    w.oldPos16 = w.pos16;
    w.oldPos16.y = w.pos16.y.v + 30.0f;
    w.destPos16.y = static_cast<int16>(-static_cast<int32_t>(rnd(state, 25)));
}

// Puts a weapon next to a random poly of the level (within its radius or so, either side), as it is about to hit it.
void nearPoly(uint64_t &state, uint32_t weapon)
{
    const uint32_t grid = mem<uint32_t>(0x328200u);
    if (!inRam(grid) || mem<uint32_t>(grid + 0x30u) == 0)
        return;
    const _sColPolyPSX &p = mem<_sColPolyPSX>(mem<uint32_t>(grid + 0x4cu) + rnd(state, mem<uint32_t>(grid + 0x30u)) * 0x60u);
    // Walls now and then on purpose: most polys are floors.
    _sWeapon &w = mem<_sWeapon>(weapon);
    const uint32_t a = rnd(state, 3), b = rnd(state, 3);
    const float t = static_cast<float>(rnd(state, 1000)) / 1000.0f;
    const float cx = p.v[a].x.v + (p.v[b].x.v - p.v[a].x.v) * t, cy = p.v[a].y.v + (p.v[b].y.v - p.v[a].y.v) * t;
    const float cz = p.v[a].z.v + (p.v[b].z.v - p.v[a].z.v) * t;
    const float side = frnd(state, w.oColrad.v + 20.0f), back = 10.0f + static_cast<float>(rnd(state, 60));
    w.pos16.x = cx + p.n.x.v * side;
    w.pos16.y = cy + p.n.y.v * side;
    w.pos16.z = cz + p.n.z.v * side;
    w.oldPos16.x = w.pos16.x.v + p.n.x.v * back;
    w.oldPos16.y = w.pos16.y.v + p.n.y.v * back;
    w.oldPos16.z = w.pos16.z.v + p.n.z.v * back;
}

// A rocket with a target, on its way along the route: near its route node (or a few nodes on), or near the target.
void rocketOnRoute(uint64_t &state, uint32_t weapon)
{
    _sWeapon &w = mem<_sWeapon>(weapon);
    if (w.target == 0x7f || !inRam(w.pAINode.address))
    {
        // Give it a target and the owner's route node, as WeaponRocketSetup does for a locked rocket.
        const int32_t target = pickCar(state);
        if (target == w.owner)
            return;
        w.target = static_cast<int16>(target);
        w.pAINode = mem<_sObject>(objectAt(static_cast<uint32_t>(w.owner))).carAI.pCurNode;
        if (!inRam(w.pAINode.address))
        {
            w.target = 0x7f;
            return;
        }
    }
    w.status = chance(state, 70) ? 0 : 1;
    uint32_t node = w.pAINode.address;
    if (chance(state, 50))
    {
        // On to a fork of the route, if there is one ahead.
        uint32_t probe = node;
        for (int steps = 0; steps < 300 && inRam(probe); ++steps, probe = mem<uint32_t>(probe + 0x10u))
            if (inRam(mem<uint32_t>(probe + 0x14u)))
            {
                node = probe;
                break;
            }
    }
    else
        for (uint32_t steps = rnd(state, 4); steps && inRam(mem<uint32_t>(node + 0x10u)); --steps)
            node = mem<uint32_t>(node + 0x10u);
    w.pAINode = ptr32<_sAINode>(node);
    const _sAINode &n = mem<_sAINode>(node);
    if (chance(state, 20))
    {
        const _sObject &target = mem<_sObject>(objectAt(static_cast<uint32_t>(w.target)));
        w.pos16.x = target.pos16.x.v + frnd(state, 1100.0f);
        w.pos16.y = target.pos16.y.v + frnd(state, 200.0f);
        w.pos16.z = target.pos16.z.v + frnd(state, 1100.0f);
    }
    else
    {
        const float spread = chance(state, 60) ? 350.0f : 800.0f;
        w.pos16.x = static_cast<float>(n.centre.vx) + frnd(state, spread);
        w.pos16.y = static_cast<float>(n.centre.vy + 0x5a) + frnd(state, 100.0f);
        w.pos16.z = static_cast<float>(n.centre.vz) + frnd(state, spread);
    }
}

// The ring of a shock wave, skull or burst shell (fx+0x58 radius, fx+0x654 centre) over the cars, which are marked.
void ringOverCars(uint64_t &state, uint32_t weapon, int32_t now)
{
    _sWeapon &w = mem<_sWeapon>(weapon);
    const uint32_t fx = w.pVfx.address;
    if (!inRam(fx))
        return;
    const _sObject &centre = mem<_sObject>(objectAt(static_cast<uint32_t>(pickCar(state))));
    mem<float>(fx + 0x58u) = static_cast<float>(200 + rnd(state, 6000));
    mem<int16_t>(fx + 0x654u) = static_cast<int16_t>(ee_ftoi(centre.pos16.x) + static_cast<int32_t>(rnd(state, 600)) - 300);
    mem<int16_t>(fx + 0x658u) = static_cast<int16_t>(ee_ftoi(centre.pos16.z) + static_cast<int32_t>(rnd(state, 600)) - 300);
    if (chance(state, 70))
        w.otherTimer = now - 1 - static_cast<int32_t>(rnd(state, 50));
    for (uint32_t i = 0; i < noObjects(); ++i)
        if (usableCar(objectAt(i)) && chance(state, 60))
            mem<_sObject>(objectAt(i)).shockWaved = 1;
}

template <int No>
void weaponLive(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    soundOff();
    varyCars(state);
    const int32_t born = timeNear(state), now = born + static_cast<int32_t>(rnd(state, 3000));
    uint32_t w = 0;
    for (int tries = 0; tries < 4 && !w; ++tries)
        w = spawnLive(state, scratch, No, pickCar(state), born);
    if (!w)
    {
        buildRecord(state, scratch, No, pickCar(state), born);
        w = scratch + kWeapon;
    }
    _sWeapon &weapon = mem<_sWeapon>(w);
    const int16 target = weapon.target;
    varyLive(state, w, now);
    if (No == 1)
        weapon.target = target; // the rocket's route node goes with its target
    if ((No == 1 || No == 3 || No == 5 || No == 11 || No == 14 || No == 15) && chance(state, 40))
        nearGround(state, w);
    else if ((No == 1 || No == 5 || No == 11 || No == 14) && chance(state, 40))
        nearPoly(state, w);
    if (No == 1 && chance(state, 70))
        rocketOnRoute(state, w);
    if (No == 11 && chance(state, 40))
    {
        weapon.status = 0;
        weapon.misc = static_cast<int16>(chance(state, 70) ? 0 : 2);
    }
    if (No == 13)
    {
        if (!weapon.pVfx || chance(state, 30))
            weapon.pVfx = ptr32<_sVfx_Object>(guest_call<uint32_t>(kVfxAddNew, ptr32<uint8>(objectAt(static_cast<uint32_t>(weapon.owner))), 0, 0, 0x26));
        if (chance(state, 70))
        {
            weapon.misc = 1;
            weapon.status = now + 1 + static_cast<int32_t>(rnd(state, 1000));
        }
    }
    if (No == 14 && chance(state, 60))
    {
        weapon.misc = 1;
        weapon.pVfx = ptr32<_sVfx_Object>(guest_call<uint32_t>(0x16a158u, ee_ftoi(weapon.pos16.x), ee_ftoi(weapon.pos16.y), ee_ftoi(weapon.pos16.z), 0x2f));
    }
    if ((No == 4 || No == 13 || No == 14) && chance(state, 70))
        ringOverCars(state, w, now);
    if (No == 10 && chance(state, 40))
    {
        weapon.otherTimer = now - 1 - static_cast<int32_t>(rnd(state, 100));
        weapon.timeWhenEnds = now + static_cast<int32_t>(rnd(state, 30)) - 15;
    }
    if (No == 8)
    {
        _sObject &owner = mem<_sObject>(objectAt(static_cast<uint32_t>(weapon.owner)));
        if (chance(state, 25))
            owner.heatrayStore = static_cast<int16>(rnd(state, 40));
        for (uint32_t i = 0; i < noObjects(); ++i)
            if (usableCar(objectAt(i)) && objectAt(i) != objectAt(static_cast<uint32_t>(weapon.owner)) && chance(state, 50))
            {
                _sObject &o = mem<_sObject>(objectAt(i));
                o.pos16.x = owner.pos16.x.v + frnd(state, 1500.0f);
                o.pos16.y = owner.pos16.y.v + frnd(state, 150.0f);
                o.pos16.z = owner.pos16.z.v + frnd(state, 1500.0f);
            }
    }
    if (No == 10 && chance(state, 30))
        mem<_sObject>(objectAt(static_cast<uint32_t>(weapon.owner))).invincible = static_cast<int8>(rnd(state, 8));
    setArg(ctx, 0, w);
    setArg(ctx, 1, static_cast<uint32_t>(now));
}

#define RC_WEAPON_KINDS(X) X(1) X(2) X(3) X(4) X(5) X(6) X(7) X(8) X(9) X(10) X(11) X(12) X(13) X(14) X(15) X(16)
#define RC_WEAPON_INSTANTIATE(n)                                                                                      \
    template void weaponSetup<n>(R5900Context *, uint64_t &, uint32_t);                                              \
    template void weaponLive<n>(R5900Context *, uint64_t &, uint32_t);
RC_WEAPON_KINDS(RC_WEAPON_INSTANTIATE)

} // namespace rc::fuzz
