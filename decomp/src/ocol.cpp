// Decompiled from OCOL.c: collision between the race's objects (cars, pickups, oil slicks, weapons and animated
// scenery). Every object is an sOColObj that sits in one cell of the collision grid (the cell's pObj list) and in the
// update list oColG.pHead. OColProcess moves the objects whose position changed to their new cells and then tests each
// changed object against the objects in the cells it reaches, calling OColCollide[type of A][type of B](A, B).
//
// Object types: 0 car, 1 pickup, 2 oil slick, 3 weapon, 4 anim (scenery that moves). Positions in a car's spheres are
// in 1/256 units; the objects' 16-bit style positions (pos16) are in units, hence the 256 and 1/256 below.

#include "ee_math.h"
#include "ocol_fuzz.h"

namespace rc {

uint32 VecNormalise(ptr32<SVECTOR> pOut, ptr32<SVECTOR> pVec); // ai.cpp
int32 AColGetCellIdFromXZ(ptr32<_sColGridPSX> pGrid, int32 x, int32 z);
void AColCellXZFromPos(ptr32<_sColGridPSX> pGrid, ee_float x, ee_float z, ptr32<int32> pX, ptr32<int32> pZ);
void OColDeleteObjFromCell(ptr32<_sOColObj> pObj);
bool_ OColCar_Anim_Collide(ptr32<_sOColObj> pA, ptr32<_sOColObj> pB);
bool_ OColWeapon_Anim_Collide(ptr32<_sOColObj> pA, ptr32<_sOColObj> pB);
bool_ CarHitAnimDefault(ptr32<_sOColObj> pA, ptr32<_sOColObj> pB);
bool_ CarHitAnim(ptr32<_sOColObj> pA, ptr32<_sOColObj> pB);
bool_ CarHitWeaponRock(ptr32<_sOColObj> pA, ptr32<_sOColObj> pB);
void OColReorderCarCollisionLists();

namespace {

const uint32_t kWeaponRemove = sym::WeaponRemove, kWeaponHitCar = sym::WeaponHitCar, kWeaponOilCollide = sym::WeaponOilCollide, kCarHitPickup = sym::CarHitPickup;
const uint32_t kOColCollideTable = sym::OColCollide;

const ee_float k256 = 256.0f, kEighth = 0.00390625f /* 1/256 */, kQuarter = 0.25f;
const ee_float kSquareScale = 1.5258789e-05f; // 2^-16: the reach is compared in units of 256.

// A uint converted as the library does: the top-bit case halves (keeping the low bit), converts and doubles.
ee_float u32ToFloat(uint32_t value)
{
    if (static_cast<int32_t>(value) >= 0)
        return ee_itof(static_cast<int32_t>(value));
    const ee_float half = ee_itof(static_cast<int32_t>((value >> 1) | (value & 1u)));
    return half + half;
}

// Is the point (dx, dy, dz), in 1/256 units, within r? The original squares each part after dividing by 256 (rounding
// towards zero, the square wraps in 32 bits) and compares the sum, as a double, with r * r / 65536 as a float.
bool withinReach(int32_t dx, int32_t dy, int32_t dz, ee_float r)
{
    const auto square = [](int32_t v)
    {
        const int32_t q = (v >= 0 ? v : static_cast<int32_t>(static_cast<uint32_t>(v) + 0xffu)) >> 8;
        return static_cast<int32_t>(static_cast<uint32_t>(q) * static_cast<uint32_t>(q));
    };
    double sum = static_cast<double>(square(dx));
    sum += static_cast<double>(square(dy));
    sum += static_cast<double>(square(dz));
    const ee_float limit = r * (r * kSquareScale);
    return sum <= static_cast<double>(limit.v);
}

// The direction from the centre of a sphere to an object, as 16-bit components scaled down from 1/256 units. A
// direction that is all zero in the low 16 bits becomes straight along z.
void setDirection(SVECTOR &d, int32_t x, int32_t y, int32_t z, ee_float scale)
{
    const int32_t sx = ee_ftoi(ee_itof(x) * scale), sy = ee_ftoi(ee_itof(y) * scale), sz = ee_ftoi(ee_itof(z) * scale);
    d.vx = static_cast<int16>(sx);
    d.vy = static_cast<int16>(sy);
    d.vz = static_cast<int16>(sz);
    if ((sx & 0xffff) == 0 && (sy & 0xffff) == 0 && (sz & 0xffff) == 0)
        d.vz = 100;
}

int32_t firstSphere(const _sFPhysLibCar &phys) { return phys.mode != 1 ? 4 : 0; }

// A sphere's contact arrays hold 12 entries, but the reordering runs one or two past the end; index by byte offset.
int8 &typeAt(_sFPhysLibColSphere &s, int32_t n) { return *(reinterpret_cast<int8 *>(&s) + 0x94 + n); }
_sFVecDepth &normalAt(_sFPhysLibColSphere &s, int32_t n) { return *reinterpret_cast<_sFVecDepth *>(reinterpret_cast<uint8_t *>(&s) + 0xa0 + 16 * n); }
uint32_t &ptrAt(_sFPhysLibColSphere &s, int32_t n) { return *reinterpret_cast<uint32_t *>(reinterpret_cast<uint8_t *>(&s) + 0x160 + 4 * n); }

// Records a contact on a sphere of a car (type 4 is a hit by scenery or a rock): direction, depth, what it hit.
void addContact(_sFPhysLibColSphere &sphere, const SVECTOR &d, ee_float depth, uint8_t type)
{
    const int32_t n = sphere.totalCols;
    _sFVecDepth &normal = normalAt(sphere, n);
    normal.x = ee_itof(d.vx);
    normal.y = ee_itof(d.vy);
    normal.z = ee_itof(d.vz);
    normal.depth = depth;
    typeAt(sphere, n) = static_cast<int8>(type);
}

// The tornado's pull on the car whose collision with it was just found.
void tornadoPull(ptr32<_sOColObj> pA, ptr32<_sOColObj> pB)
{
    const ee_float ang = ee_itof(G(tornadoAngForceG)), up = ee_itof(G(tornadoUpForceG));
    _sFPhysLibCar &car = *pA->pData.cast<_sObject>()->pPhysics;
    car.angVel.y = ang;
    car.vel.y = up;
    car.angImpulse.y = car.angImpulse.y + ang;
    car.impulse.y = car.impulse.y + up;
    const ee_float suck = G(tornadoSuckForceG);
    car.vel.x = (pB->pPos->x - pA->pPos->x) * suck;
    car.vel.z = (pB->pPos->z - pA->pPos->z) * suck;
}

} // namespace

// ---- Lists ------------------------------------------------------------------------------------------------

void OColInit() {}
void OColTerm() {}

void OColLvlInit()
{
    G(oColG).numObjects = 0;
    G(oColG).pHead = nullptr;
}

void OColLvlTerm()
{
    G(oColG).numObjects = 0;
    G(oColG).pHead = nullptr;
}

void OColRender() {}

// pPrev of an object at the head of a list is the address of the list's head pointer; so is pUpdPrev for oColG.pHead.
void OColInitObj(ptr32<_sOColObj> pObj, ptr32<_sColCellVecPSX> pPos, ptr32<ee_float> pRad, ptr32<void> pData)
{
    uint8 mask = 0, type = 0;
    bool known = true, nonMover = false;
    switch (pObj->type)
    {
    case 0: mask = 0x1f; type = 1; break;
    case 1: mask = 1; type = 2; nonMover = true; break;
    case 2: mask = 1; type = 4; nonMover = true; break;
    case 3: mask = 0x11; type = 8; break;
    case 4: mask = 9; type = 0x10; break;
    default: known = false;
    }
    if (known)
    {
        pObj->interestMask = mask;
        pObj->interestType = type;
        pObj->nonMover = nonMover ? 1 : 0;
    }
    const ptr32<_sOColObj> headAddress((sym::oColG + 0x4u)); // &oColG.pHead
    pObj->pUpdNext = nullptr;
    pObj->pUpdPrev = headAddress;
    pObj->pNext = nullptr;
    pObj->pPrev = nullptr;
    pObj->pData = pData;
    pObj->pPos = pPos;
    pObj->pUpdNext = G(oColG).pHead;
    pObj->pRad = pRad;
    pObj->enabled = 1;
    pObj->cellZE = -999;
    pObj->dealtWith = 0;
    pObj->updated = 1;
    pObj->cellX = -999;
    pObj->cellZ = -999;
    pObj->cellXS = -999;
    pObj->cellZS = -999;
    pObj->cellXE = -999;
    pObj->cellId = 0;
    if (G(oColG).pHead)
        G(oColG).pHead->pUpdPrev = pObj;
    G(oColG).pHead = pObj;
    G(oColG).numObjects = G(oColG).numObjects + 1;
}

namespace {

// Takes an object out of the update list (oColG.pHead's address stands for the list's start).
void unlinkUpdate(ptr32<_sOColObj> pObj, ptr32<_sOColObj> prev, ptr32<_sOColObj> next)
{
    const ptr32<_sOColObj> headAddress((sym::oColG + 0x4u));
    ptr32<_sOColObj> head = G(oColG).pHead;
    if (prev)
    {
        head = next;
        if (prev != headAddress)
        {
            prev->pUpdNext = next;
            head = G(oColG).pHead;
        }
    }
    G(oColG).pHead = head;
    if (next)
        next->pUpdPrev = prev;
    (void)pObj;
}

} // namespace

void OColDeleteObject(ptr32<_sOColObj> pObj)
{
    ptr32<_sOColObj> prev;
    if (!pObj->pNext)
    {
        prev = pObj->pUpdPrev;
        if (!pObj->pPrev && !pObj->pUpdNext && !prev)
            return;
    }
    else
        prev = pObj->pUpdPrev;
    if (prev)
    {
        unlinkUpdate(pObj, prev, pObj->pUpdNext);
        pObj->pUpdNext = nullptr;
        pObj->pUpdPrev = nullptr;
    }
    OColDeleteObjFromCell(pObj);
    G(oColG).numObjects = G(oColG).numObjects - 1;
}

void OColDeleteObjFromCell(ptr32<_sOColObj> pObj)
{
    const ptr32<_sOColObj> prev = pObj->pPrev, next = pObj->pNext;
    if (prev)
    {
        const int32 id = AColGetCellIdFromXZ(G(acolG).pGrid, pObj->cellX, pObj->cellZ);
        _sColGridCellPSX &cell = G(acolG).pGrid->pCell[id];
        if (prev.address == guest_addr(&cell.pObj))
            cell.pObj = next;
        else
            prev->pNext = next;
    }
    if (next)
        next->pPrev = prev;
    pObj->pNext = nullptr;
    pObj->pPrev = nullptr;
}

void OColAddObjToCell(ptr32<_sOColObj> pObj)
{
    const ptr32<_sColGridPSX> grid = G(acolG).pGrid;
    const int32 id = AColGetCellIdFromXZ(grid, pObj->cellX, pObj->cellZ);
    _sColGridCellPSX &cell = grid->pCell[id];
    pObj->cellId = id;
    const ptr32<_sOColObj> first = cell.pObj;
    pObj->pPrev = ptr32<_sOColObj>(guest_addr(&cell.pObj));
    pObj->pNext = first;
    if (cell.pObj)
        cell.pObj->pPrev = pObj;
    cell.pObj = pObj;
}

void OColAddObjectHead(ptr32<_sOColObj> pObj)
{
    pObj->pUpdPrev = ptr32<_sOColObj>((sym::oColG + 0x4u));
    pObj->pUpdNext = G(oColG).pHead;
    if (G(oColG).pHead)
        G(oColG).pHead->pUpdPrev = pObj;
    G(oColG).pHead = pObj;
    G(oColG).numObjects = G(oColG).numObjects + 1;
}

void OColDeleteObjectUpdList(ptr32<_sOColObj> pObj)
{
    const ptr32<_sOColObj> prev = pObj->pUpdPrev, next = pObj->pUpdNext;
    unlinkUpdate(pObj, prev, next);
    pObj->pUpdPrev = nullptr;
    pObj->pUpdNext = nullptr;
}

// Cars' contact lists: a sphere's contacts of type 4 (hit by scenery or a rock) move to the end of the list, so the
// car-car contacts and polys come first. The copy runs to one past the end, which also overwrites the moved entry
// (as the original does).
void OColReorderCarCollisionLists()
{
    for (int32_t c = 0; c < G(FPhysLib).numCars; ++c)
    {
        _sFPhysLibCar &car = *G(FPhysLib).pCarList[c];
        for (int32_t s = 0; s < car.numSpheres; ++s)
        {
            _sFPhysLibColSphere &sphere = car.pSphere[s];
            const int32_t total = sphere.numCarCarCols + sphere.numOtherCols;
            int32_t shunted = 0;
            const auto copyEntry = [&](int32_t to, int32_t from)
            {
                typeAt(sphere, to) = typeAt(sphere, from);
                normalAt(sphere, to) = normalAt(sphere, from);
                ptrAt(sphere, to) = ptrAt(sphere, from); // The poly (or car) pointers, +0x160.
            };
            int32_t j = 0;
            while (j < total - 1)
            {
                if (typeAt(sphere, j) == 4)
                {
                    copyEntry(total, j);
                    ++shunted;
                    for (int32_t k = j; k < total + 1; ++k)
                        copyEntry(k, k + 1);
                    j = (shunted < total ? j - 1 : total);
                }
                ++j;
            }
        }
    }
}

// ---- The main loop ------------------------------------------------------------------------------------

void OColProcess(tTime)
{
    GuestFrame frame(0xa0);
    const ptr32<int32> pCellX = frame.local<int32>(0), pCellZ = frame.local<int32>(4);
    const ptr32<_sColGridPSX> grid = G(acolG).pGrid;
    const ee_float edge = 128.0f;

    // First, every object whose position was updated gets its cell and bounds; unmoving ones leave the update list.
    ptr32<_sOColObj> p = G(oColG).pHead;
    while (p)
    {
        const ptr32<_sOColObj> next = p->pUpdNext;
        if (p->enabled == 0)
        {
            p->dealtWith = 0;
        }
        else if (p->updated != 0)
        {
            const ee_float z = p->pPos->z, x = p->pPos->x;
            AColCellXZFromPos(grid, x, z, pCellX, pCellZ);
            if (*pCellX != p->cellX || *pCellZ != p->cellZ)
            {
                const ptr32<_sOColObj> prev = p->pPrev, following = p->pNext;
                if (prev)
                {
                    const int32 id = AColGetCellIdFromXZ(G(acolG).pGrid, p->cellX, p->cellZ);
                    _sColGridCellPSX &cell = G(acolG).pGrid->pCell[id];
                    if (prev.address == guest_addr(&cell.pObj))
                        cell.pObj = following;
                    else
                        prev->pNext = following;
                }
                if (following)
                    following->pPrev = prev;
                p->cellX = *pCellX;
                p->pPrev = nullptr;
                p->pNext = nullptr;
                p->cellZ = *pCellZ;
                const int32 id = AColGetCellIdFromXZ(G(acolG).pGrid, *pCellX, *pCellZ);
                _sColGridCellPSX &cell = G(acolG).pGrid->pCell[id];
                p->cellId = id;
                const ptr32<_sOColObj> first = cell.pObj;
                p->pPrev = ptr32<_sOColObj>(guest_addr(&cell.pObj));
                p->pNext = first;
                if (cell.pObj)
                    cell.pObj->pPrev = p;
                cell.pObj = p;
            }
            const ee_float r = *p->pRad;
            p->min.x = x - r;
            const ee_float lowZ = z - r;
            p->max.x = x + r;
            p->min.z = lowZ;
            p->min.y = p->pPos->y - r;
            p->max.z = z + r;
            p->max.y = p->pPos->y + r;
            AColCellXZFromPos(grid, p->min.x - edge, lowZ - edge, ptr32<int32>(guest_addr(&p->cellXS)), ptr32<int32>(guest_addr(&p->cellZS)));
            AColCellXZFromPos(grid, p->max.x + edge, p->max.z + edge, ptr32<int32>(guest_addr(&p->cellXE)), ptr32<int32>(guest_addr(&p->cellZE)));
            // Clamped to the grid: only one end of each range is checked.
            if (p->cellXS < 0)
                p->cellXS = 0;
            else if (!(p->cellXE < grid->numCellsX))
                p->cellXE = grid->numCellsX - 1;
            if (p->cellZS < 0)
                p->cellZS = 0;
            else if (!(p->cellZE < grid->numCellsZ))
                p->cellZE = grid->numCellsZ - 1;
            p->dealtWith = 0;
        }
        else
        {
            if (p->nonMover != 0)
            {
                unlinkUpdate(p, p->pUpdPrev, next);
                p->pUpdNext = nullptr;
                p->pUpdPrev = nullptr;
            }
            p->dealtWith = 0;
        }
        p = next;
    }

    // Then each updated object is tested against the objects in the cells it reaches.
    p = G(oColG).pHead;
    while (p)
    {
        p->dealtWith = 1;
        const ptr32<_sOColObj> following = p->pUpdNext;
        if (p->updated == 0)
        {
            p = following;
            continue;
        }
        p->updated = 0;
        int32_t cellX = p->cellXS, cellZ = p->cellZS;
        bool abandoned = false;
        for (;;) // At least one cell is visited, whatever the range.
        {
            do
            {
                const int32 id = AColGetCellIdFromXZ(grid, cellX, cellZ);
                ptr32<_sOColObj> o = grid->pCell[id].pObj;
                while (o)
                {
                    const ptr32<_sOColObj> nextInCell = o->pNext;
                    if (o->dealtWith == 0)
                    {
                        // A big object (radius over 128) only meets other big ones; reaching a big one ends the cell.
                        if (edge < *o->pRad && *p->pRad <= edge)
                            break;
                        if ((p->interestMask & o->interestType) != 0 && !(o->max.x < p->min.x) && !(p->max.x < o->min.x) &&
                            !(o->max.z < p->min.z) && !(p->max.z < o->min.z) && !(o->max.y < p->min.y) && !(p->max.y < o->min.y))
                        {
                            uint32_t entry = kOColCollideTable + static_cast<uint32_t>(p->type) * 20u + static_cast<uint32_t>(o->type) * 4u;
                            guest_call(*static_cast<uint32_t *>(guest_host(entry)), p, o);
                            if (!p->pPrev)
                            {
                                abandoned = true;
                                break;
                            }
                        }
                    }
                    o = nextInCell;
                }
                if (abandoned)
                    break;
                ++cellZ;
            } while (cellZ <= p->cellZE);
            if (abandoned)
                break;
            ++cellX;
            if (p->cellXE < cellX)
                break;
            cellZ = p->cellZS;
        }
        p = following;
    }
    OColReorderCarCollisionLists();
}

// ---- Car against scenery, rocks and cars ----------------------------------------------------------------------

namespace {

enum class HitKind { Default, Probe };

// A car against an anim: any sphere within reach is a contact (HitKind::Default) or just a yes (Probe).
bool_ carHitsAnim(ptr32<_sOColObj> pA, ptr32<_sOColObj> pB, HitKind kind)
{
    GuestFrame frame(kind == HitKind::Default ? 0x110 : 0xe0);
    const ptr32<SVECTOR> delta = frame.local<SVECTOR>(0x10);
    bool_ collided = 0;
    _sObject &car = *pA->pData.cast<_sObject>();
    if (car.weaponAffect == 5)
        return 0;
    const animHead &anim = *pB->pData.cast<animHead>();
    _sFPhysLibCar &phys = *car.pPhysics;
    for (int32_t i = firstSphere(phys); i < phys.numSpheres; ++i)
    {
        _sFPhysLibColSphere &sphere = phys.pSphere[i];
        const int32_t dx = ee_ftoi(sphere.pos.x - anim.pos16.x * k256);
        const int32_t dy = ee_ftoi(sphere.pos.y - anim.pos16.y * k256);
        const int32_t dz = ee_ftoi(sphere.pos.z - anim.pos16.z * k256);
        ee_float depth = sphere.radius + anim.radius * k256;
        if (!withinReach(dx, dy, dz, depth))
            continue;
        if (kind == HitKind::Probe)
        {
            collided = 1;
            continue;
        }
        const int32_t oldX = ee_ftoi(sphere.pos16.x - anim.pos16.x), oldY = ee_ftoi(sphere.pos16.y - anim.pos16.y),
                      oldZ = ee_ftoi(sphere.pos16.z - anim.pos16.z);
        const int32_t velX = ee_ftoi(sphere.oldPos16.x - anim.oldPos16.x), velY = ee_ftoi(sphere.oldPos16.y - anim.oldPos16.y),
                      velZ = ee_ftoi(sphere.oldPos16.z - anim.oldPos16.z);
        const int32_t dot = static_cast<int32_t>(static_cast<uint32_t>(oldX) * static_cast<uint32_t>(velX) +
                                                 static_cast<uint32_t>(oldY) * static_cast<uint32_t>(velY) +
                                                 static_cast<uint32_t>(oldZ) * static_cast<uint32_t>(velZ));
        const bool closing = ee_itof(dot) < ee_float(0.0f);
        const int32_t sign = closing ? -1 : 1;
        setDirection(*delta, static_cast<int32_t>(static_cast<uint32_t>(dx) * static_cast<uint32_t>(sign)),
                     static_cast<int32_t>(static_cast<uint32_t>(dy) * static_cast<uint32_t>(sign)),
                     static_cast<int32_t>(static_cast<uint32_t>(dz) * static_cast<uint32_t>(sign)), kEighth);
        const ee_float length = u32ToFloat(VecNormalise(delta, delta)) * k256;
        depth = closing ? depth + length : depth - length;
        if (sphere.totalCols < 9 && !(phys.flags & 2))
        {
            addContact(sphere, *delta, depth, 4);
            ptrAt(sphere, sphere.totalCols) = 0;
            sphere.totalCols = sphere.totalCols + 1;
            sphere.numOtherCols = sphere.numOtherCols + 1;
        }
        collided = 1;
    }
    return collided;
}

} // namespace

bool_ CarHitAnimDefault(ptr32<_sOColObj> pA, ptr32<_sOColObj> pB) { return carHitsAnim(pA, pB, HitKind::Default); }
bool_ CarHitAnim(ptr32<_sOColObj> pA, ptr32<_sOColObj> pB) { return carHitsAnim(pA, pB, HitKind::Probe); }

// A rock (weapon 15) against a car: contacts are scaled by 128 and the car's velocity is halved and reversed.
bool_ CarHitWeaponRock(ptr32<_sOColObj> pA, ptr32<_sOColObj> pB)
{
    GuestFrame frame(0x120);
    const ptr32<SVECTOR> delta = frame.local<SVECTOR>(0x10);
    bool_ collided = 0;
    _sFPhysLibCar &phys = *pA->pData.cast<_sObject>()->pPhysics;
    const _sWeapon &weapon = *pB->pData.cast<_sWeapon>();
    for (int32_t i = firstSphere(phys); i < phys.numSpheres; ++i)
    {
        _sFPhysLibColSphere &sphere = phys.pSphere[i];
        const int32_t dx = ee_ftoi(sphere.pos.x - weapon.pos16.x * k256);
        const int32_t dy = ee_ftoi(sphere.pos.y - weapon.pos16.y * k256);
        const int32_t dz = ee_ftoi(sphere.pos.z - weapon.pos16.z * k256);
        const ee_float depth = sphere.radius + weapon.oColrad * ee_float(384.0f);
        if (!withinReach(dx, dy, dz, depth))
            continue;
        setDirection(*delta, dx, dy, dz, kEighth);
        const ee_float length = u32ToFloat(VecNormalise(delta, delta));
        if (sphere.totalCols < 9 && !(phys.flags & 2))
        {
            const ee_float scaled = (depth - length * k256) * ee_float(128.0f);
            addContact(sphere, *delta, scaled, 4);
            ptrAt(sphere, sphere.totalCols) = 0;
            sphere.totalCols = sphere.totalCols + 1;
            sphere.numOtherCols = sphere.numOtherCols + 1;
            const ee_float half = ee_float(-0.5f);
            phys.vel.x = phys.vel.x * half;
            phys.vel.y = phys.vel.y * half;
            phys.vel.z = phys.vel.z * half;
        }
        collided = 1;
    }
    return collided;
}

bool_ OColCar_Car_Collide(ptr32<_sOColObj> pA, ptr32<_sOColObj> pB)
{
    GuestFrame frame(0x120);
    const ptr32<SVECTOR> delta = frame.local<SVECTOR>(0x10);
    ptr32<_sObject> carX = pA->pData.cast<_sObject>(), carY = pB->pData.cast<_sObject>();
    if (carX->weaponAffect == 5 || carY->weaponAffect == 5)
        return 0;
    // mode: 0 normal; 1 exactly one car has its invincible flag, so its centre and radius stand for its spheres.
    int mode = (carX->invincible != 0 ? 1 : 0) + (carY->invincible != 0 ? 2 : 0);
    ptr32<_sOColObj> flagged = pA;
    if (mode == 2)
    {
        mode = 1;
        carX = pB->pData.cast<_sObject>();
        carY = pA->pData.cast<_sObject>();
        flagged = pB;
    }
    else if (mode == 3)
        mode = 0;
    const ptr32<_sFPhysLibCar> physX = carX->pPhysics, physY = carY->pPhysics;
    const int32_t startJ = physY->mode == 1 ? 0 : 4;
    bool_ collided = 0;
    const ee_float quarter = kQuarter;
    for (int32_t i = firstSphere(*physX); i < physX->numSpheres; ++i)
    {
        _sFPhysLibColSphere *sphereA = nullptr;
        ee_float ax, ay, az, ar;
        if (mode == 0)
        {
            sphereA = &physX->pSphere[i];
            ax = sphereA->pos.x;
            ay = sphereA->pos.y;
            az = sphereA->pos.z;
            ar = sphereA->radius;
        }
        else
        {
            ax = physX->renderPos.x;
            ay = physX->renderPos.y;
            az = physX->renderPos.z;
            ar = *flagged->pRad * k256;
        }
        for (int32_t j = startJ; j < physY->numSpheres; ++j)
        {
            _sFPhysLibColSphere &sphereB = physY->pSphere[j];
            const int32_t dx = ee_ftoi(ax - sphereB.pos.x), dy = ee_ftoi(ay - sphereB.pos.y), dz = ee_ftoi(az - sphereB.pos.z);
            const ee_float reach = ar + sphereB.radius;
            const ee_float fx = ee_itof(dx), fy = ee_itof(dy), fz = ee_itof(dz);
            if (reach < ee_abs(fx) || reach < ee_abs(fz) || reach < ee_abs(fy))
                continue;
            ee_float depth;
            bool closing = false;
            if (mode == 0)
            {
                const auto diff = [](ee_float a, ee_float b) { return ee_ftoi(a - b); };
                const int32_t px = diff(sphereA->pos16.x, sphereB.pos16.x), py = diff(sphereA->pos16.y, sphereB.pos16.y),
                              pz = diff(sphereA->pos16.z, sphereB.pos16.z);
                const int32_t vx = diff(sphereA->oldPos16.x, sphereB.oldPos16.x), vy = diff(sphereA->oldPos16.y, sphereB.oldPos16.y),
                              vz = diff(sphereA->oldPos16.z, sphereB.oldPos16.z);
                const int32_t dot = static_cast<int32_t>(static_cast<uint32_t>(vx) * static_cast<uint32_t>(px) +
                                                         static_cast<uint32_t>(vy) * static_cast<uint32_t>(py) +
                                                         static_cast<uint32_t>(vz) * static_cast<uint32_t>(pz));
                closing = dot < 0;
            }
            if (closing)
            {
                setDirection(*delta, static_cast<int32_t>(0u - static_cast<uint32_t>(dx)), static_cast<int32_t>(0u - static_cast<uint32_t>(dy)),
                             static_cast<int32_t>(0u - static_cast<uint32_t>(dz)), quarter);
                depth = reach + u32ToFloat(VecNormalise(delta, delta)) * quarter;
            }
            else
            {
                setDirection(*delta, dx, dy, dz, quarter);
                depth = reach - u32ToFloat(VecNormalise(delta, delta)) * quarter;
            }
            if (!(ee_float(0.0f) < depth))
                continue;
            if (mode == 0 && sphereA->totalCols < 9 && !(physX->flags & 2))
            {
                addContact(*sphereA, *delta, depth, 2);
                ptrAt(*sphereA, sphereA->totalCols) = physY.address;
                sphereA->totalCols = sphereA->totalCols + 1;
                sphereA->numCarCarCols = sphereA->numCarCarCols + 1;
            }
            delta->vx = static_cast<int16>(0u - static_cast<uint16>(delta->vx));
            delta->vy = static_cast<int16>(0u - static_cast<uint16>(delta->vy));
            delta->vz = static_cast<int16>(0u - static_cast<uint16>(delta->vz));
            if (sphereB.totalCols < 9 && !(physY->flags & 2))
            {
                addContact(sphereB, *delta, depth, 2);
                ptrAt(sphereB, sphereB.totalCols) = physX.address;
                sphereB.totalCols = sphereB.totalCols + 1;
                sphereB.numCarCarCols = sphereB.numCarCarCols + 1;
            }
            collided = 1;
        }
        if (mode != 0)
            break;
    }
    if (collided && mode != 0)
        physY->angImpulse.y = physY->angImpulse.y + ee_float(1000.0f);
    return collided;
}

bool_ OColCar_Anim_Collide(ptr32<_sOColObj> pA, ptr32<_sOColObj> pB)
{
    GuestFrame frame(0x30);
    switch (pB->pData.cast<animHead>()->type)
    {
    case 0:
    case 1:
    case 2:
    case 4:
        return CarHitAnimDefault(pA, pB);
    case 3:
        return CarHitAnimDefault(pA, pB) != 0 ? 1 : 0;
    case 5:
        if (!CarHitAnim(pA, pB))
            return 0;
        tornadoPull(pA, pB);
        return 1;
    default:
        return 0;
    }
}

// ---- Weapons, pickups, oil -------------------------------------------------------------------------------

namespace {

// Weapons 1, 3, 5 and 11 are used up when they touch scenery.
void weaponHitsAnim(ptr32<_sOColObj> pWeapon)
{
    const int16 no = pWeapon->pData.cast<_sWeapon>()->no;
    if (no == 1 || no == 3 || no == 5 || no == 11)
        guest_call(kWeaponRemove, pWeapon->pData);
}

bool_ carHitByWeapon(ptr32<_sOColObj> pCar, ptr32<_sOColObj> pWeapon)
{
    GuestFrame frame(0x10);
    const ptr32<_sWeapon> weapon = pWeapon->pData.cast<_sWeapon>();
    const ptr32<_sObject> object = pCar->pData.cast<_sObject>();
    const int16 no = weapon->no;
    const int32_t player = object->playerNo;
    if (no == 15 && (weapon->owner != player || weapon->misc == 1))
    {
        CarHitWeaponRock(pCar, pWeapon);
        return 1;
    }
    if (weapon->owner != player || no == 3 || no == 11 || no == 15)
    {
        guest_call(kWeaponHitCar, weapon, object);
        return 1;
    }
    return 0;
}

bool_ carHitsPickup(ptr32<_sOColObj> pCar, ptr32<_sOColObj> pPickup)
{
    GuestFrame frame(0x10);
    if (pPickup->pData.cast<_sPickup>()->state != 1)
        return 0;
    guest_call(kCarHitPickup, pCar->pData, pPickup->pData);
    return 1;
}

bool_ oilHitsCar(ptr32<_sOColObj> pOil, ptr32<_sOColObj> pCar)
{
    GuestFrame frame(0x10);
    guest_call(kWeaponOilCollide, pOil->pData, pCar->pData);
    return 0;
}

} // namespace

bool_ OColCar_Pickup_Collide(ptr32<_sOColObj> pA, ptr32<_sOColObj> pB) { return carHitsPickup(pA, pB); }
bool_ OColPickup_Car_Collide(ptr32<_sOColObj> pA, ptr32<_sOColObj> pB) { return carHitsPickup(pB, pA); }
bool_ OColCar_OilSlick_Collide(ptr32<_sOColObj> pA, ptr32<_sOColObj> pB) { return oilHitsCar(pB, pA); }
bool_ OColOilSlick_Car_Collide(ptr32<_sOColObj> pA, ptr32<_sOColObj> pB) { return oilHitsCar(pA, pB); }
bool_ OColCar_Weapon_Collide(ptr32<_sOColObj> pA, ptr32<_sOColObj> pB) { return carHitByWeapon(pA, pB); }
bool_ OColWeapon_Car_Collide(ptr32<_sOColObj> pA, ptr32<_sOColObj> pB) { return carHitByWeapon(pB, pA); }

bool_ OColWeapon_Anim_Collide(ptr32<_sOColObj> pA, ptr32<_sOColObj> pB)
{
    GuestFrame frame(0x10);
    if (pB->pData.cast<animHead>()->type <= 4 && pB->pData.cast<animHead>()->type >= 0)
        weaponHitsAnim(pA);
    return 0;
}

bool_ OColAnim_Car_Collide(ptr32<_sOColObj> pA, ptr32<_sOColObj> pB) { return OColCar_Anim_Collide(pB, pA); }
bool_ OColAnim_Weapon_Collide(ptr32<_sOColObj> pA, ptr32<_sOColObj> pB) { return OColWeapon_Anim_Collide(pB, pA); }

// Nothing else happens between these pairs.
bool_ OColPickup_Pickup_Collide(ptr32<_sOColObj>, ptr32<_sOColObj>) { return 0; }
bool_ OColPickup_OilSlick_Collide(ptr32<_sOColObj>, ptr32<_sOColObj>) { return 0; }
bool_ OColPickup_Weapon_Collide(ptr32<_sOColObj>, ptr32<_sOColObj>) { return 0; }
bool_ OColPickup_Anim_Collide(ptr32<_sOColObj>, ptr32<_sOColObj>) { return 0; }
bool_ OColOilSlick_Pickup_Collide(ptr32<_sOColObj>, ptr32<_sOColObj>) { return 0; }
bool_ OColOilSlick_OilSlick_Collide(ptr32<_sOColObj>, ptr32<_sOColObj>) { return 0; }
bool_ OColOilSlick_Weapon_Collide(ptr32<_sOColObj>, ptr32<_sOColObj>) { return 0; }
bool_ OColOilSlick_Anim_Collide(ptr32<_sOColObj>, ptr32<_sOColObj>) { return 0; }
bool_ OColWeapon_Pickup_Collide(ptr32<_sOColObj>, ptr32<_sOColObj>) { return 0; }
bool_ OColWeapon_OilSlick_Collide(ptr32<_sOColObj>, ptr32<_sOColObj>) { return 0; }
bool_ OColWeapon_Weapon_Collide(ptr32<_sOColObj>, ptr32<_sOColObj>) { return 0; }
bool_ OColAnim_Pickup_Collide(ptr32<_sOColObj>, ptr32<_sOColObj>) { return 0; }
bool_ OColAnim_OilSlick_Collide(ptr32<_sOColObj>, ptr32<_sOColObj>) { return 0; }
bool_ OColAnim_Anim_Collide(ptr32<_sOColObj>, ptr32<_sOColObj>) { return 0; }

// ---- Cars against the animals and the tornado --------------------------------------------------------------

bool_ CarHitSharkFin(ptr32<_sOColObj> pA, ptr32<_sOColObj> pB) { GuestFrame frame(0x10); return CarHitAnimDefault(pA, pB); }
bool_ CarHitRat(ptr32<_sOColObj> pA, ptr32<_sOColObj> pB) { GuestFrame frame(0x10); return CarHitAnimDefault(pA, pB); }
bool_ CarHitCrab(ptr32<_sOColObj> pA, ptr32<_sOColObj> pB) { GuestFrame frame(0x10); return CarHitAnimDefault(pA, pB); }
bool_ CarHitBottle(ptr32<_sOColObj> pA, ptr32<_sOColObj> pB) { GuestFrame frame(0x10); return CarHitAnimDefault(pA, pB); }
bool_ CarHitTentacles(ptr32<_sOColObj> pA, ptr32<_sOColObj> pB) { GuestFrame frame(0x10); return CarHitAnimDefault(pA, pB); }

bool_ CarHitTornado(ptr32<_sOColObj> pA, ptr32<_sOColObj> pB)
{
    GuestFrame frame(0x30);
    const bool_ hit = CarHitAnim(pA, pB);
    if (hit)
        tornadoPull(pA, pB);
    return hit;
}

bool_ WeaponHitSharkFin(ptr32<_sOColObj> pA, ptr32<_sOColObj>) { GuestFrame frame(0x10); weaponHitsAnim(pA); return 0; }
bool_ WeaponHitRat(ptr32<_sOColObj> pA, ptr32<_sOColObj>) { GuestFrame frame(0x10); weaponHitsAnim(pA); return 0; }
bool_ WeaponHitCrab(ptr32<_sOColObj> pA, ptr32<_sOColObj>) { GuestFrame frame(0x10); weaponHitsAnim(pA); return 0; }
bool_ WeaponHitBottle(ptr32<_sOColObj> pA, ptr32<_sOColObj>) { GuestFrame frame(0x10); weaponHitsAnim(pA); return 0; }
bool_ WeaponHitTentacles(ptr32<_sOColObj> pA, ptr32<_sOColObj>) { GuestFrame frame(0x10); weaponHitsAnim(pA); return 0; }
bool_ WeaponHitTornado(ptr32<_sOColObj>, ptr32<_sOColObj>) { return 0; }

} // namespace rc


RC_DECOMP_FN(OColInit)
RC_DECOMP_FN(OColTerm)
RC_DECOMP_FN(OColLvlInit)
RC_DECOMP_FN(OColLvlTerm)
RC_DECOMP_FN(OColRender)
RC_DECOMP_FN_SETUP(OColInitObj, rc::fuzz::ocolInitObject)
RC_DECOMP_FN_SETUP(OColDeleteObject, rc::fuzz::ocolObject)
RC_DECOMP_FN_SETUP(OColDeleteObjFromCell, rc::fuzz::ocolObject)
RC_DECOMP_FN_SETUP(OColAddObjToCell, rc::fuzz::ocolObject)
RC_DECOMP_FN_SETUP(OColAddObjectHead, rc::fuzz::ocolObject)
RC_DECOMP_FN_SETUP(OColDeleteObjectUpdList, rc::fuzz::ocolObject)
RC_DECOMP_FN_SETUP(OColReorderCarCollisionLists, rc::fuzz::ocolReorder)
RC_DECOMP_FN_SETUP(OColProcess, rc::fuzz::ocolList)

RC_DECOMP_FN_SETUP(CarHitAnimDefault, rc::fuzz::ocolCarAnim)
RC_DECOMP_FN_SETUP(CarHitAnim, rc::fuzz::ocolCarAnim)
RC_DECOMP_FN_SETUP(CarHitWeaponRock, rc::fuzz::ocolCarRock)
RC_DECOMP_FN_SETUP(OColCar_Car_Collide, rc::fuzz::ocolCarCar)
RC_DECOMP_FN_SETUP(OColCar_Anim_Collide, rc::fuzz::ocolCarAnim)
RC_DECOMP_FN_SETUP(OColAnim_Car_Collide, rc::fuzz::ocolAnimCar)
RC_DECOMP_FN_SETUP(CarHitSharkFin, rc::fuzz::ocolCarAnim)
RC_DECOMP_FN_SETUP(CarHitRat, rc::fuzz::ocolCarAnim)
RC_DECOMP_FN_SETUP(CarHitCrab, rc::fuzz::ocolCarAnim)
RC_DECOMP_FN_SETUP(CarHitBottle, rc::fuzz::ocolCarAnim)
RC_DECOMP_FN_SETUP(CarHitTentacles, rc::fuzz::ocolCarAnim)
RC_DECOMP_FN_SETUP(CarHitTornado, rc::fuzz::ocolCarAnim)
RC_DECOMP_FN_SETUP_HEAP(OColWeapon_Anim_Collide, rc::fuzz::ocolWeaponAnim)
RC_DECOMP_FN_SETUP_HEAP(OColAnim_Weapon_Collide, rc::fuzz::ocolAnimWeapon)
RC_DECOMP_FN_SETUP_HEAP(WeaponHitSharkFin, rc::fuzz::ocolWeaponAnim)
RC_DECOMP_FN_SETUP_HEAP(WeaponHitRat, rc::fuzz::ocolWeaponAnim)
RC_DECOMP_FN_SETUP_HEAP(WeaponHitCrab, rc::fuzz::ocolWeaponAnim)
RC_DECOMP_FN_SETUP_HEAP(WeaponHitBottle, rc::fuzz::ocolWeaponAnim)
RC_DECOMP_FN_SETUP_HEAP(WeaponHitTentacles, rc::fuzz::ocolWeaponAnim)
RC_DECOMP_FN(WeaponHitTornado)
RC_DECOMP_FN_SETUP(OColCar_Pickup_Collide, rc::fuzz::ocolCarPickup)
RC_DECOMP_FN_SETUP(OColPickup_Car_Collide, rc::fuzz::ocolPickupCar)
RC_DECOMP_FN_SETUP(OColCar_OilSlick_Collide, rc::fuzz::ocolCarOil)
RC_DECOMP_FN_SETUP(OColOilSlick_Car_Collide, rc::fuzz::ocolOilCar)
RC_DECOMP_FN_SETUP_HEAP(OColCar_Weapon_Collide, rc::fuzz::ocolCarWeapon)
RC_DECOMP_FN_SETUP_HEAP(OColWeapon_Car_Collide, rc::fuzz::ocolWeaponCar)
RC_DECOMP_FN(OColPickup_Pickup_Collide)
RC_DECOMP_FN(OColPickup_OilSlick_Collide)
RC_DECOMP_FN(OColPickup_Weapon_Collide)
RC_DECOMP_FN(OColPickup_Anim_Collide)
RC_DECOMP_FN(OColOilSlick_Pickup_Collide)
RC_DECOMP_FN(OColOilSlick_OilSlick_Collide)
RC_DECOMP_FN(OColOilSlick_Weapon_Collide)
RC_DECOMP_FN(OColOilSlick_Anim_Collide)
RC_DECOMP_FN(OColWeapon_Pickup_Collide)
RC_DECOMP_FN(OColWeapon_OilSlick_Collide)
RC_DECOMP_FN(OColWeapon_Weapon_Collide)
RC_DECOMP_FN(OColAnim_Pickup_Collide)
RC_DECOMP_FN(OColAnim_OilSlick_Collide)
RC_DECOMP_FN(OColAnim_Anim_Collide)
