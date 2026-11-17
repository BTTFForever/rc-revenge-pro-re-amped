// WEAPON.c, continued: the Updates of the weapons that fly and land (the fake pickup, the dump tug's rock, the
// armoured car's shell, the rocket, the bouncy bomb, the heat ray) and the weapons' test against the level's polys.
// See weapon.cpp.

#include "weapon.h"
#include "weapon_fuzz.h"

#include <cstring>

namespace rc {

using namespace weapon;

namespace {

const ee_float k256 = 256.0f, k50 = 50.0f, k20 = 20.0f, kZero = 0.0f, kHalf = 0.5f, k4096 = 4096.0f;
constexpr uint32_t kPointInPolyTable = sym::AColPointInPoly_NCLIP_DWO; // AColPointInPoly_NCLIP_DWO[major axis]
constexpr uint32_t kTriggerState = (sym::carColG + 0xe0u);     // carColG.triggerState[9]

int32_t toFixed(ee_float v) { return ee_ftoi(v * k256); }
int32_t scale12(int16_t c, int32_t k) { return (static_cast<int32_t>(c) * k) / 4096; }

bool pointInPoly(uint32_t point, uint32_t poly)
{
    const uint8_t axis = at<uint8_t>(poly + 0x52u);
    return guest_call<uint32_t>(at<uint32_t>(kPointInPolyTable + 4u * axis), ptr32<uint8>(point), ptr32<uint8>(poly)) != 0;
}

// The weapon falls under gravity (destPos16.y down 2 a frame, no faster than 20) and moves by destPos16.
void fall(_sWeapon &w)
{
    w.destPos16.y = static_cast<int16>(w.destPos16.y - 2);
    if (w.destPos16.y < -0x14)
        w.destPos16.y = -0x14;
    w.pos16.x = w.pos16.x + ee_itof(w.destPos16.x);
    w.pos16.y = w.pos16.y + ee_itof(w.destPos16.y);
    w.pos16.z = w.pos16.z + ee_itof(w.destPos16.z);
}

void rearm(int32_t owner, int8_t kind)
{
    _sObject &car = *objectAt(owner);
    if (car.weaponsHeld != 0 && car.weaponsHeldType == kind)
        car.weaponType = static_cast<uint8>(kind);
}

// The bouncy bomb's bounce off the poly whose normal is the frame's norm (sp+0): the throw is reflected; off a wall
// (normal y below 0x800) it carries on at speed 30, off a floor it hops, higher the steeper the ground and lower where
// the ground 8 frames ahead is high (a down cast from 250 above it).
void bounce(const GuestFrame &frame, ptr32<_sWeapon> pWeapon)
{
    const ee_float kHop = 250.0f;
    _sWeapon &w = *pWeapon;
    const _sVec16 &norm = *frame.local<_sVec16>(0);
    const uint32_t fx = w.pVfx.address;
    const ptr32<_sVec16> pInput = frame.local<_sVec16>(0x30);
    guest_call(kVec16Normalise, pInput, ptr32<uint8>(pWeapon.address + 0xbcu));
    NormalDotProduct(ptr32<normal_>(frame.local<uint8>(0).address), ptr32<normal_>(pInput.address), frame.local<frac>(0x40));
    const int32_t twice = static_cast<int32_t>(*frame.local<int16_t>(0x40)) * 2;
    vec32_ &doubled = *frame.local<vec32_>(0x10);
    doubled.x = (static_cast<int32_t>(norm.x) * twice) >> 12;
    doubled.y = (static_cast<int32_t>(norm.y) * twice) >> 12;
    doubled.z = (static_cast<int32_t>(norm.z) * twice) >> 12;
    _sVec16 &reflected = *frame.local<_sVec16>(0x20);
    const _sVec16 &input = *pInput;
    reflected.x = static_cast<int16>(input.x - doubled.x);
    reflected.y = static_cast<int16>(input.y - doubled.y);
    reflected.z = static_cast<int16>(input.z - doubled.z);
    guest_call(kVec16Normalise, pInput, frame.local<_sVec16>(0x20));
    at<uint32_t>(fx + 0x2d0u) = 0xff; // the bounce effect's timer
    at<int16_t>(fx + 0x2c8u) = static_cast<int16_t>(ee_ftoi(w.pos16.x));
    at<int16_t>(fx + 0x2cau) = static_cast<int16_t>(ee_ftoi(w.pos16.y));
    at<int16_t>(fx + 0x2ccu) = static_cast<int16_t>(ee_ftoi(w.pos16.z));
    sfx3D(0x18, 0xff, 0x100, pWeapon.address + 0xc8u, carSfxFlags(objectAt(w.owner)), 5000);
    int16 z;
    if (norm.y < 0x800)
    {
        w.specialFire = 0x1e;
        w.destPos16.x = static_cast<int16>(scale12(input.x, 0x1e));
        z = static_cast<int16>(scale12(input.z, 0x1e));
    }
    else
    {
        const int32_t dx = w.destPos16.x, dz = w.destPos16.z;
        const uint32_t speed = Sqr32(static_cast<uint32_t>(dx) * static_cast<uint32_t>(dx) + static_cast<uint32_t>(dz) * static_cast<uint32_t>(dz));
        vec16_ &ahead = *frame.local<vec16_>(0x50);
        ahead.x = static_cast<int16>(ee_ftoi(w.pos16.x + ee_itof(static_cast<int32_t>(static_cast<uint32_t>(w.destPos16.x) << 3))));
        ahead.z = static_cast<int16>(ee_ftoi(w.pos16.z + ee_itof(static_cast<int32_t>(static_cast<uint32_t>(w.destPos16.z) << 3))));
        ahead.y = static_cast<int16>(ee_ftoi(w.pos16.y + kHop));
        std::memset(frame.local<uint8>(0x90).get(), 0, 0x10);
        _sColCellVecPSX &from = *frame.local<_sColCellVecPSX>(0x90);
        from.x = ee_itof(ahead.x);
        from.y = ee_itof(ahead.y);
        from.z = ee_itof(ahead.z);
        std::memcpy(frame.local<uint8>(0x80).get(), frame.local<uint8>(0x90).get(), 0x10);
        AColDownCast(frame.local<_sColCellVecPSX>(0x80), frame.local<_sColCellVecPSX>(0x60), frame.local<_sColNormalPSX>(0x70), frame.local<uint8>(0xa0));
        // The ground point is read even when the cast found nothing (then it is what the stack held).
        int32_t height = 0x1c;
        if (!(frame.local<_sColCellVecPSX>(0x60)->y + kHop < ee_itof(ahead.y)))
            height = 0x1c - ((norm.y - 0x1000) * static_cast<int32_t>(speed)) / 1024;
        w.destPos16.y = static_cast<int16>(height);
        if (w.destPos16.y > 0x32)
            w.destPos16.y = 0x32;
        if (w.destPos16.y < 10)
            w.destPos16.y = 10;
        w.destPos16.x = static_cast<int16>(scale12(input.x, w.specialFire));
        z = static_cast<int16>(scale12(input.z, w.specialFire));
    }
    w.destPos16.z = z;
    w.misc = 0;
    w.status = 1;
}

} // namespace

// Which level poly, if any, the weapon's sphere (oColrad) touches in its cell; the poly's material, with its normal in
// 4.12 at pNormal, or 0x48 with straight up. Closed trigger doors (materials 0x1a, 0x3a to 0x41) do not count.
int32 WeaponTestAcol(ptr32<_sWeapon> pWeapon, ptr32<_sVec16> pNormal)
{
    GuestFrame frame(0xd0);
    _sWeapon &w = *pWeapon;
    const ptr32<_sColGridPSX> pGrid = G(acolG).pGrid;
    const ee_float r = w.oColrad, x = w.pos16.x, y = w.pos16.y, z = w.pos16.z;
    const ee_float minX = x - r, minY = y - r, minZ = z - r, maxX = x + r, maxY = y + r, maxZ = z + r;
    const int32_t id = AColCellIdFromPosUnbounded(pGrid, x, z);
    const uint32_t cell = pGrid->pCell.address + static_cast<uint32_t>(id) * 16u;
    uint32_t ids = at<uint32_t>(pGrid.address + 0x50u) + at<uint32_t>(cell + 8u) * 4u;
    for (int32_t list = 1; list >= 0; --list)
    {
        for (int32_t left = at<int32_t>(cell + 4u * static_cast<uint32_t>(list)); left > 0; --left)
        {
            const uint32_t poly = pGrid->pPoly.address + at<uint32_t>(ids) * 0x60u;
            ids += 4;
            const _sColPolyPSX &p = *ptr32<_sColPolyPSX>(poly);
            const int32_t d = ee_ftoi(p.d);
            if (p.v[0].y < minY)
                break; // the list is sorted by height: nothing further down reaches
            if (maxY < p.v[0].pad || p.v[2].pad < minX || maxX < p.v[1].pad || p.bbMaxZ < minZ || maxZ < p.v[3].pad)
                continue;
            const uint32_t mat = p.mat;
            int32_t trigger = -1;
            if (mat == 0x1a)
                trigger = 0;
            else if (mat >= 0x3a && mat <= 0x41)
                trigger = static_cast<int32_t>(mat) - 0x39;
            if (trigger >= 0 && at<uint32_t>(kTriggerState + 4u * static_cast<uint32_t>(trigger)) == 1)
                continue;
            const ee_float nx = p.n.x, ny = p.n.y, nz = p.n.z, fd = ee_itof(d);
            const ee_float dist = ee_itof(ee_ftoi(w.pos16.x * nx + w.pos16.y * ny + w.pos16.z * nz + fd));
            const ee_float rad = w.oColrad;
            if (rad < dist)
                continue;
            bool hit;
            if (!(dist < -rad))
                hit = pointInPoly(pWeapon.address + 0x94u, poly);
            else
            {
                const int32_t old = ee_ftoi(w.oldPos16.x * nx + w.oldPos16.y * ny + w.oldPos16.z * nz + fd);
                if (old <= 0)
                {
                    if (!(ee_itof(-old) <= rad))
                        continue;
                    hit = pointInPoly(pWeapon.address + 0xa4u, poly);
                }
                else if (pointInPoly(pWeapon.address + 0x94u, poly) || pointInPoly(pWeapon.address + 0xa4u, poly))
                    hit = true;
                else
                {
                    _sColCellVecPSX &pm = *frame.local<_sColCellVecPSX>(0x20);
                    const ee_float px = w.pos16.x, py = w.pos16.y, pz = w.pos16.z;
                    pm.x = px + (w.oldPos16.x - px) * kHalf;
                    pm.y = py + (w.oldPos16.y - py) * kHalf;
                    pm.z = pz + (w.oldPos16.z - pz) * kHalf;
                    hit = pointInPoly(frame.local<uint8>(0x20).address, poly);
                }
            }
            if (hit)
            {
                pNormal->x = static_cast<int16>(ee_ftoi(p.n.x * k4096));
                pNormal->y = static_cast<int16>(ee_ftoi(p.n.y * k4096));
                pNormal->z = static_cast<int16>(ee_ftoi(p.n.z * k4096));
                return static_cast<int32>(mat);
            }
        }
    }
    pNormal->z = 0;
    pNormal->y = 0x1000;
    pNormal->x = 0;
    return 0x48;
}

void WeaponFakeUpdate(ptr32<_sWeapon> pWeapon, tTime time)
{
    GuestFrame frame(0x60);
    _sWeapon &w = *pWeapon;
    const ptr32<_sColCellVecPSX> pos(pWeapon.address + 0x94u);
    if (w.status != 1)
    {
        fall(w);
        w.rendObj.pos.x = toFixed(w.pos16.x);
        w.rendObj.pos.y = ee_ftoi((w.pos16.y - k50) * k256);
        w.rendObj.pos.z = toFixed(w.pos16.z);
        if (!AColDownCast(pos, ptr32<_sColCellVecPSX>(pWeapon.address + 0x10cu), ptr32<_sColNormalPSX>(pWeapon.address + 0x11cu),
                          frame.local<uint8>(0)))
        {
            w.oCol.type = 4;
            w.status = 1;
            w.normal.x = w.normal.x * k4096;
            w.normal.y = w.normal.y * k4096;
            w.normal.z = w.normal.z * k4096;
            return;
        }
        const ee_float y = w.pos16.y;
        const int32_t above = ee_ftoi((y - k50) - w.poi.y);
        if (above < 0x19)
        {
            w.status = 1;
            const ee_float landed = y - ee_itof(above);
            w.pos16.y = landed;
            w.rendObj.pos.y = ee_ftoi((landed - k50) * k256);
        }
    }
    if (w.otherTimer < time)
    {
        if (AColIsPositionOutsideWorld(pos, kZero))
            w.oCol.type = 4;
        else
        {
            w.oCol.type = 3;
            OColInitObj(ptr32<_sOColObj>(pWeapon.address + 0x28u), pos, ptr32<ee_float>(pWeapon.address + 0xb4u), ptr32<void>(pWeapon.address));
        }
        w.otherTimer = w.timeWhenEnds + 1000;
    }
    if (w.oCol.type == 3)
        w.oCol.updated = 1;
    w.rendObj.angles.head = static_cast<angle>(w.rendObj.angles.head - 0x100);
    guest_call(kMat3nBuildXYZ, ptr32<uint8>(pWeapon.address + 0xdcu), ptr32<uint8>(pWeapon.address + 0xd4u));
}

void WeaponDumptugUpdate(ptr32<_sWeapon> pWeapon, tTime time)
{
    GuestFrame frame(0x80);
    _sWeapon &w = *pWeapon;
    if (w.status == 0)
    {
        _sObject &car = *objectAt(w.owner);
        car.tipAngle = car.tipAngle + 1000;
        if (car.tipAngle > 10000)
        {
            car.tipAngle = 10000;
            w.status = 1;
        }
        int32_t id = w.sfxHandle1, flags;
        if (id == -1)
        {
            id = 0x4f;
            flags = carSfxFlags(objectAt(w.owner));
        }
        else
            flags = objectAt(w.owner)->playerType + 0x58;
        w.sfxHandle1 = sfx3D(id, 0xff, 0x100, carPos(objectAt(w.owner)), flags, 2000);
    }
    else if (w.status == 1)
    {
        const ptr32<_sObject> pCar = objectAt(w.owner);
        pCar->tipAngle = pCar->tipAngle - 400;
        int32_t id = w.sfxHandle1, flags;
        if (id == -1)
        {
            id = 0x4f;
            flags = carSfxFlags(pCar);
        }
        else
            flags = pCar->playerType + 0x58;
        w.sfxHandle1 = sfx3D(id, 0xff, 0xfb, carPos(pCar), flags, 2000);
        if (objectAt(w.owner)->tipAngle < 0)
        {
            objectAt(w.owner)->tipAngle = 0;
            w.status = 2;
            if (w.sfxHandle1 != -1)
            {
                sfxStop(w.sfxHandle1);
                w.sfxHandle1 = -1;
            }
        }
    }
    if (w.misc == 0)
    {
        fall(w);
        w.rendObj.pos.x = toFixed(w.pos16.x);
        w.rendObj.pos.y = toFixed(w.pos16.y);
        w.rendObj.pos.z = toFixed(w.pos16.z);
        if (!AColDownCast(ptr32<_sColCellVecPSX>(pWeapon.address + 0x94u), ptr32<_sColCellVecPSX>(pWeapon.address + 0x10cu),
                          ptr32<_sColNormalPSX>(pWeapon.address + 0x11cu), frame.local<uint8>(0)))
        {
            w.timeWhenEnds = time + 1;
            w.misc = 1;
            return;
        }
        const ee_float y = w.pos16.y;
        const int32_t above = ee_ftoi((y - k20) - w.poi.y);
        if (above < 0x19)
        {
            w.misc = 1;
            const ee_float landed = y - ee_itof(above);
            const ptr32<_sObject> car = objectAt(w.owner);
            w.destPos16.x = 0;
            w.pos16.y = landed;
            w.destPos16.y = 0;
            w.destPos16.z = 0;
            w.rendObj.pos.y = toFixed(landed);
            sfx3D(0x51, 0xff, 0x100, pWeapon.address + 0xc8u, carSfxFlags(car), 2000);
        }
    }
    if (w.oCol.type == 3)
        w.oCol.updated = 1;
    if (w.otherTimer != 0 && w.otherTimer < time)
    {
        w.otherTimer = 0;
        rearm(w.owner, 0xf);
    }
}

void WeaponArmouredUpdate(ptr32<_sWeapon> pWeapon, tTime time)
{
    GuestFrame frame(0x100);
    _sWeapon &w = *pWeapon;
    if (w.misc == 1)
    {
        // The shell has burst: its ring throws the cars it reaches, as the shock wave does.
        if (!(w.otherTimer < time))
            return;
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
            pushAway(frame, car, 4000, w.pos16.x, w.pos16.z);
            guest_call(kObjectWeaponAffectRemove, ptr32<_sObject>(guest_addr(&car)));
            car.weaponAffectTime = time + 0x5dc;
            car.weaponAffect = 4;
        }
        return;
    }
    w.destPos16.y = static_cast<int16>(w.destPos16.y - 2);
    w.oldPos16 = w.pos16;
    const ee_float x = w.pos16.x + ee_itof(w.destPos16.x);
    const ee_float y = w.pos16.y + ee_itof(w.destPos16.y);
    const ee_float z = w.pos16.z + ee_itof(w.destPos16.z);
    _sVfx_Object &shell = *w.pVfx;
    w.pos16.x = x;
    w.pos16.y = y;
    w.pos16.z = z;
    shell.svHookPos.vx = static_cast<int16>(ee_ftoi(x));
    shell.svHookPos.vy = static_cast<int16>(ee_ftoi(y));
    shell.svHookPos.vz = static_cast<int16>(ee_ftoi(z));
    if (AColIsPositionOutsideWorld(ptr32<_sColCellVecPSX>(pWeapon.address + 0x94u), kZero) ||
        WeaponTestAcol(pWeapon, frame.local<_sVec16>(0)) != 0x48)
    {
        vec32_ &pos = *frame.local<vec32_>(0x10);
        pos.x = toFixed(w.pos16.x);
        pos.y = toFixed(w.pos16.y);
        pos.z = toFixed(w.pos16.z);
        VfxDelete(w.pVfx);
        w.pVfx = ptr32<_sVfx_Object>(nullptr);
        const ptr32<_sVfx_Object> blast = VfxAddNewPlace(ee_ftoi(w.pos16.x), ee_ftoi(w.pos16.y + k50), ee_ftoi(w.pos16.z), 0x2f);
        w.otherTimer = time + 0x4b;
        w.timeWhenEnds = time + 1000;
        w.pVfx = blast;
        w.misc = 1;
        for (uint32_t i = 0; i < G(objectVarG).noObjects; ++i)
            objectAt(static_cast<int32_t>(i))->shockWaved = 1;
        OColDeleteObject(ptr32<_sOColObj>(pWeapon.address + 0x28u));
        w.oCol.type = 4;
        sfx3D(0x5b, 0xff, 0xe2, frame.local<vec32_>(0x10).address, carSfxFlags(objectAt(w.owner)), 0x1964);
        rearm(w.owner, 0xe);
    }
    if (w.oCol.type != 4)
        w.oCol.updated = 1;
    if (w.otherTimer != 0 && w.otherTimer < time)
    {
        w.otherTimer = 0;
        rearm(w.owner, 0xe);
    }
}

// The heat ray burns down the owner's store by 25 a frame; while it lasts, each car in front (within a quarter turn)
// that the beam sweeps across (the side it is on changed since last frame, or it is within 0x1ff of dead ahead), within
// 2000 units and not too far above, is set alight and thrown.
void WeaponHeatrayUpdate(ptr32<_sWeapon> pWeapon, tTime)
{
    const ee_float kQuarter = 0.25f, kLift = 3500.0f;
    GuestFrame frame(0xc0);
    _sWeapon &w = *pWeapon;
    const ptr32<_sObject> pFirer = objectAt(w.owner);
    _sObject &firer = *pFirer;
    firer.heatrayStore = static_cast<int16>(firer.heatrayStore - 0x19);
    int32_t id = w.sfxHandle1, flags;
    if (id == -1)
    {
        id = 0x16;
        flags = carSfxFlags(pFirer);
    }
    else
        flags = firer.playerType + 0x58;
    w.sfxHandle1 = sfx3D(id, 0xff, 0x100, carPos(objectAt(w.owner)), flags, 2000);
    if (firer.heatrayStore < 0)
    {
        firer.weaponsHeld = 0;
        firer.heatrayStore = 0;
        w.timeWhenEnds = 0;
        if (w.pVfx)
        {
            VfxDelete(w.pVfx);
            w.pVfx = ptr32<_sVfx_Object>(nullptr);
        }
        firer.heatrayStoreAngle = 0;
        for (uint32_t i = 0; i < G(objectVarG).noObjects; ++i)
            objectAt(static_cast<int32_t>(i))->heatrayStoreAngle = 0;
        sfxStop(objectAt(w.owner)->weaponsHeldSfx);
        objectAt(w.owner)->weaponsHeldSfx = -1;
        sfxStop(w.sfxHandle1);
        w.sfxHandle1 = -1;
        return;
    }
    for (uint32_t i = 0; i < G(objectVarG).noObjects; ++i)
    {
        if (static_cast<int32_t>(i) == w.owner)
            continue;
        const ptr32<_sObject> pTarget = objectAt(static_cast<int32_t>(i));
        _sObject &target = *pTarget;
        if (target.weaponAffect == 8 || target.invincible != 0)
            continue;
        const int32_t a = WeaponRelativeAngle(objectAt(w.owner), pTarget);
        const int32_t size = a < 0 ? -a : a;
        bool swept = false;
        if (size < 0x4000)
        {
            const int32_t before = target.heatrayStoreAngle;
            if ((before < 0 && a > 0) || (before > 0 && a < 0))
                swept = true;
            else
                swept = size <= 0x1ff;
        }
        if (swept)
        {
            vec32_ &d = *frame.local<vec32_>(0);
            d.y = 0;
            d.x = ee_ftoi(firer.pos16.x - target.pos16.x);
            d.z = ee_ftoi(firer.pos16.z - target.pos16.z);
            const int32_t length = static_cast<int32_t>(guest_call<uint32_t>(kVec32Length, frame.local<vec32_>(0)));
            if (length < 0x7d1)
            {
                const int32_t dy = ee_ftoi(firer.pos16.y - target.pos16.y);
                d.y = dy;
                if (static_cast<int16_t>(ee_div(static_cast<int32_t>(static_cast<uint32_t>(dy) << 12), length)) < 0x144)
                {
                    VfxAddNew(pTarget, 0, 0, 6);
                    VfxAddNew(pTarget, 0, 0, 0);
                    guest_call(kObjectWeaponAffectRemove, pTarget);
                    _sFPhysLibCar &physics = *target.pPhysics;
                    target.weaponAffect = 8;
                    const ee_float vx = physics.vel.x, vz = physics.vel.z;
                    physics.wheel[1].oilyCounter = 0x14;
                    physics.wheel[0].oilyCounter = 0x14;
                    target.heatrayStoreAngle = 0;
                    physics.vel.y = physics.vel.y + kLift;
                    physics.vel.x = vx + vx * kQuarter;
                    physics.vel.z = vz + vz * kQuarter;
                    continue;
                }
            }
        }
        target.heatrayStoreAngle = static_cast<int16>(a);
    }
}

// The rocket flies 80 units a frame: straight along destPos16 when it has no target, else at the target once it is
// near it or on its route node, and otherwise along the route (the next node, or of a fork the branch whose direction
// is closer to the target's; prev for one fired backwards). A rocket without a target bursts on the level's polys.
void WeaponRocketUpdate(ptr32<_sWeapon> pWeapon)
{
    const ee_float kDrop = -40.0f;
    GuestFrame frame(0x120);
    _sWeapon &w = *pWeapon;
    const uint32_t rendPos = pWeapon.address + 0xc8u;
    {
        const ptr32<_sObject> owner = objectAt(w.owner);
        const int32_t handle = w.sfxHandle1;
        w.sfxHandle1 = handle == -1 ? sfx3D(0x1c, 0xff, 0x100, rendPos, (owner->playerType + 0x10) | 0x44, 2000)
                                    : sfx3D(handle, 0xff, 0x100, rendPos, (owner->playerType + 8) | 0x240, 2000);
    }
    w.oldPos16 = w.pos16;
    _sVec16 &n = *frame.local<_sVec16>(0x20);
    vec32_ &delta = *frame.local<vec32_>(0);
    const auto length = [&] { return static_cast<int32_t>(guest_call<uint32_t>(kVec32Length, frame.local<vec32_>(0))); };
    const auto toNode = [&]
    {
        const _sAINode &node = *w.pAINode;
        delta.x = ee_ftoi(w.pos16.x - ee_itof(node.centre.vx));
        delta.y = ee_ftoi(w.pos16.y - ee_itof(node.centre.vy + 0x5a));
        delta.z = ee_ftoi(w.pos16.z - ee_itof(node.centre.vz));
    };
    if (w.target == 0x7f)
    {
        n.x = w.destPos16.x;
        n.y = w.destPos16.y;
        n.z = w.destPos16.z;
    }
    else
    {
        const _sObject &target = *objectAt(w.target);
        if (w.status == 1)
        {
            delta.x = ee_ftoi(w.pos16.x - target.pos16.x);
            delta.y = ee_ftoi(w.pos16.y - target.pos16.y);
            delta.z = ee_ftoi(w.pos16.z - target.pos16.z);
        }
        else
        {
            delta.x = ee_ftoi(w.pos16.x - target.pos16.x);
            delta.y = ee_ftoi(w.pos16.y - target.pos16.y);
            delta.z = ee_ftoi(w.pos16.z - target.pos16.z);
            if (length() < 1000)
                w.status = 1;
            if (w.pAINode == target.carAI.pCurNode)
                w.status = 1;
            toNode();
            while (length() < 500)
            {
                const _sAINode &node = *w.pAINode;
                if (w.specialFire != 0)
                    w.pAINode = node.prev[0];
                else if (!node.next[1])
                    w.pAINode = node.next[0];
                else
                {
                    // Which branch of the fork points nearer the target. (The original takes x for z.)
                    const ee_float x = w.pos16.x;
                    const _sAINode &left = *node.next[0], &right = *node.next[1];
                    const int32_t toTargetX = ee_ftoi(x - target.pos16.x), toTargetZ = ee_ftoi(w.pos16.z - target.pos16.z);
                    const int16_t leftX = static_cast<int16_t>(ee_ftoi(x - ee_itof(left.centre.vx)));
                    const int16_t leftZ = static_cast<int16_t>(ee_ftoi(x - ee_itof(left.centre.vz)));
                    const int16_t rightX = static_cast<int16_t>(ee_ftoi(x - ee_itof(right.centre.vx)));
                    const int16_t rightZ = static_cast<int16_t>(ee_ftoi(x - ee_itof(right.centre.vz)));
                    const int32_t aLeft = ArcTan(leftX, leftZ);
                    const int32_t aRight = ArcTan(rightX, rightZ);
                    const int32_t aTarget = ArcTan(static_cast<int16_t>(toTargetX), static_cast<int16_t>(toTargetZ));
                    int32_t offRight = static_cast<int16_t>(aTarget - aRight), offLeft = static_cast<int16_t>(aTarget - aLeft);
                    if (offRight < 0)
                        offRight = -offRight;
                    if (offLeft < 0)
                        offLeft = -offLeft;
                    w.pAINode = offLeft < offRight ? w.pAINode->next[0] : w.pAINode->next[1];
                }
                toNode();
            }
        }
        guest_call(kVec32Normalise, frame.local<_sVec16>(0x20), frame.local<vec32_>(0));
    }
    const int32_t stepY = scale12(n.y, 0x50), stepZ = scale12(n.z, 0x50), stepX = scale12(n.x, 0x50);
    const ee_float y = w.pos16.y - ee_itof(stepY), x = w.pos16.x - ee_itof(stepX), z = w.pos16.z - ee_itof(stepZ);
    w.oCol.updated = 1;
    _sVec16 &step = *frame.local<_sVec16>(0x10);
    step.x = static_cast<int16>(stepX);
    step.y = static_cast<int16>(stepY);
    step.z = static_cast<int16>(stepZ);
    w.pos16.x = x;
    w.rendObj.pos.x = ee_ftoi(x * k256);
    w.pos16.y = y;
    w.rendObj.pos.y = ee_ftoi((y + kDrop) * k256);
    w.pos16.z = z;
    w.rendObj.pos.z = ee_ftoi(z * k256);
    int burst = 0;
    if (AColIsPositionOutsideWorld(ptr32<_sColCellVecPSX>(pWeapon.address + 0x94u), kZero))
    {
        burst = 1;
        OColDeleteObject(ptr32<_sOColObj>(pWeapon.address + 0x28u));
        w.oCol.type = 4;
    }
    else if (w.target == 0x7f)
    {
        const int32_t material = WeaponTestAcol(pWeapon, frame.local<_sVec16>(0x30));
        *frame.local<int32_t>(0x70) = material;
        burst = material != 0x48 ? 2 : 0;
    }
    if (burst != 0)
    {
        VfxAddNewPlace(ee_ftoi(w.pos16.x), ee_ftoi(w.pos16.y), ee_ftoi(w.pos16.z), 0x16);
        sfx3D(0x38, 0xff, 0x100, rendPos, carSfxFlags(objectAt(w.owner)), 5000);
        w.timeWhenEnds = 0;
        // The material is only set by the poly test: a rocket that left the world reads a stale word here.
        if (*frame.local<int32_t>(0x70) == 0x19)
            guest_call(kRevSfxSpecialTrigger, 2, ptr32<uint8>(rendPos), G(mainVarG).time);
    }
    _sVfx_Object &fx = *w.pVfx;
    const uint32_t fxAddress = w.pVfx.address;
    fx.svHookPos.vz = static_cast<int16>(ee_ftoi(w.pos16.z));
    fx.svHookPos.vx = static_cast<int16>(ee_ftoi(w.pos16.x));
    fx.svHookPos.vy = static_cast<int16>(ee_ftoi(w.pos16.y));
    at<int16_t>(fxAddress + 0x2ccu) = static_cast<int16_t>(-step.x);
    at<int16_t>(fxAddress + 0x2ceu) = static_cast<int16_t>(-step.y);
    at<int16_t>(fxAddress + 0x2d0u) = static_cast<int16_t>(-step.z);
}

// The bouncy bomb: falls along destPos16 and bounces off the level's polys (reflected about the poly's normal; off a
// floor it hops forward by a height from a down cast 8 frames ahead); in water (materials 0x12, 0x13, 0x45) it sinks
// and sits there (misc 1) until a car sets it off. status counts frames during which it does not bounce again.
void WeaponBouncyUpdate(ptr32<_sWeapon> pWeapon, tTime time)
{
    GuestFrame frame(0x120);
    _sWeapon &w = *pWeapon;
    const ptr32<_sVec16> pNorm = frame.local<_sVec16>(0);
    _sVec16 &norm = *pNorm;
    if (w.misc == 1)
    {
        if (w.otherTimer != 0 && w.otherTimer < time)
        {
            w.otherTimer = 0;
            rearm(w.owner, 0xb);
        }
        if (static_cast<int16_t>(WeaponTestAcol(pWeapon, pNorm)) == 0x45)
        {
            VfxAddNewPlace(ee_ftoi(w.pos16.x), ee_ftoi(w.pos16.y), ee_ftoi(w.pos16.z), 0x16);
            w.timeWhenEnds = 0;
            OColDeleteObject(ptr32<_sOColObj>(pWeapon.address + 0x28u));
            w.oCol.type = 4;
        }
        w.oCol.updated = 1;
        return;
    }
    if (w.misc == 0 || w.misc == 2)
    {
        w.destPos16.y = static_cast<int16>(w.destPos16.y - 2);
        w.pos16.x = w.pos16.x + ee_itof(w.destPos16.x);
        w.pos16.y = w.pos16.y + ee_itof(w.destPos16.y);
        w.pos16.z = w.pos16.z + ee_itof(w.destPos16.z);
    }
    int32_t material;
    if (AColIsPositionOutsideWorld(ptr32<_sColCellVecPSX>(pWeapon.address + 0x94u), kZero))
    {
        material = 4;
        VfxAddNewPlace(ee_ftoi(w.pos16.x), ee_ftoi(w.pos16.y), ee_ftoi(w.pos16.z), 0x16);
        w.timeWhenEnds = 0;
        OColDeleteObject(ptr32<_sOColObj>(pWeapon.address + 0x28u));
        w.oCol.type = 4;
    }
    else
        material = static_cast<int16_t>(WeaponTestAcol(pWeapon, pNorm));
    if (w.status != 0)
    {
        material = 0x48;
        w.status = w.status - 1;
    }
    if (material != 0x48)
    {
        if (static_cast<uint32_t>(material - 0x12) < 2u || material == 0x45)
        {
            // Into water: a splash, and the bomb stops.
            const ptr32<_sVfx_Object> splash = VfxAddNewPlace(ee_ftoi(w.pos16.x), ee_ftoi(w.pos16.y), ee_ftoi(w.pos16.z), 0xc);
            splash->svHookPos.vx = norm.x;
            splash->svHookPos.vy = norm.y;
            splash->svHookPos.vz = norm.z;
            w.misc = 1;
            w.destPos16.x = 0;
            w.destPos16.y = 0;
            w.destPos16.z = 0;
        }
        else
        {
            if (material == 0x19)
            {
                guest_call(kRevSfxSpecialTrigger, 2, ptr32<uint8>(pWeapon.address + 0xc8u), G(mainVarG).time);
                uint32_t &trigger = at<uint32_t>(kTriggerState);
                if (trigger == 0)
                {
                    trigger = 1;
                    for (const uint32_t tag : {0x1000024eu, 0x1000024du})
                    {
                        const uint32_t anim = guest_call<uint32_t>(kAnimFindByTSOTag, tag);
                        at<uint16_t>(anim + 8u) = static_cast<uint16_t>(at<uint16_t>(anim + 8u) | 0x80u);
                    }
                }
            }
            if (w.misc == 0)
                bounce(frame, pWeapon);
        }
    }
    w.oCol.updated = 1;
    _sVfx_Object &fx = *w.pVfx;
    fx.svHookPos.vx = static_cast<int16>(ee_ftoi(w.pos16.x));
    fx.svHookPos.vy = static_cast<int16>(ee_ftoi(w.pos16.y));
    w.rendObj.pos.x = ee_ftoi(w.pos16.x * k256);
    fx.svHookPos.vz = static_cast<int16>(ee_ftoi(w.pos16.z));
    w.rendObj.pos.y = ee_ftoi(w.pos16.y * k256);
    w.rendObj.pos.z = ee_ftoi(w.pos16.z * k256);
    if (w.otherTimer != 0 && w.otherTimer < time)
    {
        w.otherTimer = 0;
        rearm(w.owner, 0xb);
    }
    w.oldPos16 = w.pos16;
}

} // namespace rc

RC_DECOMP_FN_SETUP(WeaponTestAcol, rc::fuzz::weaponAcol)
RC_DECOMP_FN_SETUP_HEAP(WeaponFakeUpdate, rc::fuzz::weaponLive<3>)
RC_DECOMP_FN_SETUP_HEAP(WeaponDumptugUpdate, rc::fuzz::weaponLive<15>)
RC_DECOMP_FN_SETUP_HEAP(WeaponArmouredUpdate, rc::fuzz::weaponLive<14>)
RC_DECOMP_FN_SETUP_HEAP(WeaponHeatrayUpdate, rc::fuzz::weaponLive<8>)
RC_DECOMP_FN_SETUP_HEAP(WeaponRocketUpdate, rc::fuzz::weaponLive<1>)
RC_DECOMP_FN_SETUP_HEAP(WeaponBouncyUpdate, rc::fuzz::weaponLive<11>)
