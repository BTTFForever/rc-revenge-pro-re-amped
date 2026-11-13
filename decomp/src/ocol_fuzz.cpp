// Builds random but valid object collision data for the random test. See ocol_fuzz.h.
//
// The scratch area starts as random bytes, so every field not set here is random.

#include "ocol_fuzz.h"

#include <cstring>

namespace rc {
uint32_t &randState(); // ai.cpp: newlib's rand state
}

namespace rc::fuzz {

namespace {

constexpr uint32_t kObjA = 0x0000u, kObjB = 0x0500u;                  // _sObject (0x42c)
constexpr uint32_t kPhysA = 0x1000u, kPhysB = 0x1800u;                // _sFPhysLibCar (0x23c)
constexpr uint32_t kSpheresA = 0x2000u, kSpheresB = 0x2a00u;          // up to 6 spheres of 0x190
constexpr uint32_t kAnim = 0x3400u, kWeapon = 0x3500u, kPickup = 0x3700u, kOilWeapon = 0x3800u;
constexpr uint32_t kOcols = 0x3a00u;                                   // 0x80 apart: carA, carB, anim, weapon, pickup, oil
constexpr uint32_t kVecs = 0x3d00u, kRads = 0x3e00u;
constexpr uint32_t kGrid = 0x4000u, kCells = 0x4100u, kListObjs = 0x4400u, kListVecs = 0x4a00u, kListRads = 0x4b00u;
constexpr int kMaxSpheres = 5, kGridSide = 4, kListCars = 2;
static_assert(sizeof(_sObject) <= kObjB - kObjA);
static_assert(sizeof(_sFPhysLibCar) <= kPhysB - kPhysA);
static_assert(kMaxSpheres * sizeof(_sFPhysLibColSphere) <= kSpheresB - kSpheresA);
static_assert(sizeof(_sWeapon) <= kPickup - kWeapon);

struct Rng
{
    uint64_t &state;

    uint32_t next() { return nextFuzz(state); }
    int below(int count) { return count > 0 ? static_cast<int>(next() % static_cast<uint32_t>(count)) : 0; }
    bool chance(int percent) { return below(100) < percent; }
    float range(float low, float high) { return low + (high - low) * (static_cast<float>(next() >> 8) / 16777216.0f); }
};

template <typename T>
T &at(uint32_t address) { return *static_cast<T *>(guest_host(address)); }

void setArg(R5900Context *ctx, int index, uint32_t value) { SET_GPR_S32(ctx, 4 + index, static_cast<int32_t>(value)); }

struct Vec3 { float x, y, z; };

void setVec(_sColCellVecPSX &v, Vec3 p)
{
    v.x = p.x;
    v.y = p.y;
    v.z = p.z;
}

Vec3 near(Rng &rng, Vec3 c, float spread) { return {c.x + rng.range(-spread, spread), c.y + rng.range(-spread, spread), c.z + rng.range(-spread, spread)}; }

// A car: spheres around the centre c (in units; the sphere positions are 256 times that).
void buildCar(Rng &rng, uint32_t objAddress, uint32_t physAddress, uint32_t spheresAddress, Vec3 c, int playerNo)
{
    _sObject &object = at<_sObject>(objAddress);
    _sFPhysLibCar &phys = at<_sFPhysLibCar>(physAddress);
    object.pPhysics = ptr32<_sFPhysLibCar>(physAddress);
    object.weaponAffect = static_cast<int16>(rng.chance(10) ? 5 : rng.below(5));
    object.invincible = rng.chance(25) ? 1 : 0;
    object.playerNo = static_cast<uint8>(playerNo);
    phys.numSpheres = static_cast<int8>(1 + rng.below(kMaxSpheres));
    phys.flags = static_cast<uint8>(rng.chance(25) ? 2 : rng.below(2));
    phys.mode = static_cast<int8>(rng.chance(40) ? 1 : rng.below(3));
    phys.pSphere = ptr32<_sFPhysLibColSphere>(spheresAddress);
    phys.renderPos.x = c.x * 256.0f + rng.range(-3000.0f, 3000.0f);
    phys.renderPos.y = c.y * 256.0f + rng.range(-3000.0f, 3000.0f);
    phys.renderPos.z = c.z * 256.0f + rng.range(-3000.0f, 3000.0f);
    phys.vel.x = rng.range(-5000.0f, 5000.0f);
    phys.vel.y = rng.range(-5000.0f, 5000.0f);
    phys.vel.z = rng.range(-5000.0f, 5000.0f);
    phys.impulse.y = rng.range(-5000.0f, 5000.0f);
    phys.angImpulse.y = rng.range(-5000.0f, 5000.0f);
    phys.angVel.y = rng.range(-50.0f, 50.0f);
    for (int s = 0; s < kMaxSpheres; ++s)
    {
        _sFPhysLibColSphere &sphere = at<_sFPhysLibColSphere>(spheresAddress + s * static_cast<uint32_t>(sizeof(_sFPhysLibColSphere)));
        const Vec3 p = near(rng, c, 60.0f);
        setVec(sphere.pos16, p);
        setVec(sphere.oldPos16, near(rng, p, 25.0f));
        sphere.pos.x = p.x * 256.0f;
        sphere.pos.y = p.y * 256.0f;
        sphere.pos.z = p.z * 256.0f;
        sphere.radius = rng.range(25.0f, 110.0f) * 256.0f;
        sphere.totalCols = rng.chance(80) ? static_cast<int8>(rng.below(5)) : static_cast<int8>(7 + rng.below(5));
        sphere.numCarCarCols = static_cast<int8>(rng.below(4));
        sphere.numOtherCols = static_cast<int8>(rng.below(5));
        for (int k = 0; k < 14; ++k)
            at<uint8_t>(spheresAddress + s * static_cast<uint32_t>(sizeof(_sFPhysLibColSphere)) + 0x94 + k) =
                static_cast<uint8_t>(rng.chance(30) ? 4 : rng.below(4));
    }
}

void buildOcol(uint32_t address, int type, uint32_t data, uint32_t pos, uint32_t rad)
{
    _sOColObj &o = at<_sOColObj>(address);
    o.type = static_cast<uint8>(type);
    o.pData = ptr32<void>(data);
    o.pPos = ptr32<_sColCellVecPSX>(pos);
    o.pRad = ptr32<ee_float>(rad);
}

struct World
{
    uint32_t carA, carB, anim, weapon, pickup, oil, rock; // the collision objects' addresses
};

// centreB: where car B is, relative to car A. weaponNo -1: a random weapon.
World buildWorld(Rng &rng, uint32_t scratch, int weaponNo = -1)
{
    World w{};
    randState() = rng.next(); // weapons and effects draw random numbers; a whole-RAM input starts from the game's own state
    const Vec3 a = {rng.range(-300.0f, 300.0f), rng.range(0.0f, 100.0f), rng.range(-300.0f, 300.0f)};
    const Vec3 b = rng.chance(60) ? near(rng, a, 90.0f) : Vec3{rng.range(-300.0f, 300.0f), rng.range(0.0f, 100.0f), rng.range(-300.0f, 300.0f)};
    const int playerA = rng.below(4), playerB = rng.below(4);
    buildCar(rng, scratch + kObjA, scratch + kPhysA, scratch + kSpheresA, a, playerA);
    buildCar(rng, scratch + kObjB, scratch + kPhysB, scratch + kSpheresB, b, playerB);

    animHead &anim = at<animHead>(scratch + kAnim);
    const Vec3 ap = near(rng, a, 80.0f);
    setVec(anim.pos16, ap);
    setVec(anim.oldPos16, near(rng, ap, 20.0f));
    anim.radius = rng.range(20.0f, 100.0f);
    anim.type = static_cast<int16>(rng.chance(15) ? rng.below(40) - 4 : rng.below(7));

    _sWeapon &weapon = at<_sWeapon>(scratch + kWeapon);
    std::memset(&weapon, 0, sizeof(weapon)); // its task and links would be random pointers otherwise
    static const int16 kNumbers[] = {0, 1, 2, 3, 4, 5, 6, 11, 15, 15, 12, 7};
    weapon.no = static_cast<int16>(weaponNo >= 0 ? weaponNo : kNumbers[rng.below(12)]);
    weapon.misc = static_cast<int16>(rng.below(3));
    weapon.owner = static_cast<int16>(rng.chance(50) ? playerA : rng.below(4));
    const Vec3 wp = near(rng, a, 80.0f);
    setVec(weapon.pos16, wp);
    setVec(weapon.oldPos16, near(rng, wp, 20.0f));
    weapon.oColrad = rng.range(10.0f, 80.0f);

    at<_sPickup>(scratch + kPickup).state = static_cast<uint8>(rng.chance(60) ? 1 : rng.below(3));
    _sWeapon &oil = at<_sWeapon>(scratch + kOilWeapon);
    std::memset(&oil, 0, sizeof(oil));
    oil.no = 2;

    const uint32_t ocolAddresses[6] = {scratch + kOcols, scratch + kOcols + 0x80, scratch + kOcols + 0x100, scratch + kOcols + 0x180,
                                       scratch + kOcols + 0x200, scratch + kOcols + 0x280};
    const uint32_t datas[6] = {scratch + kObjA, scratch + kObjB, scratch + kAnim, scratch + kWeapon, scratch + kPickup, scratch + kOilWeapon};
    const int types[6] = {0, 0, 4, 3, 1, 2};
    const Vec3 centres[6] = {a, b, ap, wp, near(rng, a, 80.0f), near(rng, a, 80.0f)};
    for (int i = 0; i < 6; ++i)
    {
        setVec(at<_sColCellVecPSX>(scratch + kVecs + 16u * i), centres[i]);
        at<float>(scratch + kRads + 4u * i) = rng.range(20.0f, 120.0f);
        buildOcol(ocolAddresses[i], types[i], datas[i], scratch + kVecs + 16u * i, scratch + kRads + 4u * i);
    }
    w.carA = ocolAddresses[0];
    w.carB = ocolAddresses[1];
    w.anim = ocolAddresses[2];
    w.weapon = ocolAddresses[3];
    w.pickup = ocolAddresses[4];
    w.oil = ocolAddresses[5];
    G(tornadoAngForceG) = rng.below(2000) - 1000;
    G(tornadoUpForceG) = rng.below(2000) - 1000;
    G(tornadoSuckForceG) = rng.range(-2.0f, 2.0f);
    return w;
}

void pair(R5900Context *ctx, uint64_t &state, uint32_t scratch, uint32_t World::*first, uint32_t World::*second, int weaponNo = -1)
{
    Rng rng{state};
    const World w = buildWorld(rng, scratch, weaponNo);
    setArg(ctx, 0, w.*first);
    setArg(ctx, 1, w.*second);
}

} // namespace

void ocolCarAnim(R5900Context *ctx, uint64_t &state, uint32_t scratch) { pair(ctx, state, scratch, &World::carA, &World::anim); }
void ocolAnimCar(R5900Context *ctx, uint64_t &state, uint32_t scratch) { pair(ctx, state, scratch, &World::anim, &World::carA); }
void ocolWeaponAnim(R5900Context *ctx, uint64_t &state, uint32_t scratch) { pair(ctx, state, scratch, &World::weapon, &World::anim); }
void ocolAnimWeapon(R5900Context *ctx, uint64_t &state, uint32_t scratch) { pair(ctx, state, scratch, &World::anim, &World::weapon); }
void ocolCarRock(R5900Context *ctx, uint64_t &state, uint32_t scratch) { pair(ctx, state, scratch, &World::carA, &World::weapon, 15); }
void ocolCarCar(R5900Context *ctx, uint64_t &state, uint32_t scratch) { pair(ctx, state, scratch, &World::carA, &World::carB); }
void ocolCarPickup(R5900Context *ctx, uint64_t &state, uint32_t scratch) { pair(ctx, state, scratch, &World::carA, &World::pickup); }
void ocolPickupCar(R5900Context *ctx, uint64_t &state, uint32_t scratch) { pair(ctx, state, scratch, &World::pickup, &World::carA); }
void ocolCarOil(R5900Context *ctx, uint64_t &state, uint32_t scratch) { pair(ctx, state, scratch, &World::carA, &World::oil); }
void ocolOilCar(R5900Context *ctx, uint64_t &state, uint32_t scratch) { pair(ctx, state, scratch, &World::oil, &World::carA); }
void ocolCarWeapon(R5900Context *ctx, uint64_t &state, uint32_t scratch) { pair(ctx, state, scratch, &World::carA, &World::weapon); }
void ocolWeaponCar(R5900Context *ctx, uint64_t &state, uint32_t scratch) { pair(ctx, state, scratch, &World::weapon, &World::carA); }

namespace {

// A grid of 4 by 4 cells with a few objects, some in cells and some in the update list.
struct Objects
{
    _sColGridPSX *grid;
    uint32_t addr[8];
    int count;
};

void linkIntoCell(uint32_t scratch, uint32_t objAddress, int cellX, int cellZ)
{
    _sOColObj &o = at<_sOColObj>(objAddress);
    const int id = cellX * kGridSide + cellZ;
    _sColGridCellPSX &cell = at<_sColGridCellPSX>(scratch + kCells + 16u * id);
    o.cellX = cellX;
    o.cellZ = cellZ;
    o.cellId = id;
    o.pPrev = ptr32<_sOColObj>(scratch + kCells + 16u * id + 0xc);
    o.pNext = cell.pObj;
    if (cell.pObj)
        cell.pObj->pPrev = ptr32<_sOColObj>(objAddress);
    cell.pObj = ptr32<_sOColObj>(objAddress);
}

void buildGrid(uint32_t scratch)
{
    _sColGridPSX &grid = at<_sColGridPSX>(scratch + kGrid);
    std::memset(&grid, 0, sizeof(grid));
    grid.min.x = grid.min.y = grid.min.z = -1024.0f;
    grid.numCellsX = grid.numCellsZ = kGridSide;
    grid.pCell = ptr32<_sColGridCellPSX>(scratch + kCells);
    std::memset(guest_host(scratch + kCells), 0, 16 * kGridSide * kGridSide);
    G(acolG).pGrid = ptr32<_sColGridPSX>(scratch + kGrid);
}

// Fills the list of objects. safe: only objects whose collisions do nothing outside this unit's code (and cars).
void buildList(Rng &rng, uint32_t scratch, bool inCells, Objects &out)
{
    buildGrid(scratch);
    World w = buildWorld(rng, scratch, 0 + 2 * rng.below(3)); // a weapon number that is never used up or a hit
    // Players and owners agree, so weapon-car pairs do nothing.
    at<_sObject>(scratch + kObjA).playerNo = at<_sObject>(scratch + kObjB).playerNo = 1;
    at<_sWeapon>(scratch + kWeapon).owner = 1;
    at<_sPickup>(scratch + kPickup).state = 0;
    at<_sWeapon>(scratch + kOilWeapon).no = 2;
    at<animHead>(scratch + kAnim).type = static_cast<int16>(rng.below(7));
    // Cars list: the two cars.
    G(FPhysLib).numCars = kListCars;
    G(FPhysLib).pCarList[0] = ptr32<_sFPhysLibCar>(scratch + kPhysA);
    G(FPhysLib).pCarList[1] = ptr32<_sFPhysLibCar>(scratch + kPhysB);
    out.count = 3 + rng.below(6);
    const int types[5] = {0, 0, 4, 3, 1};
    const uint32_t datas[5] = {scratch + kObjA, scratch + kObjB, scratch + kAnim, scratch + kWeapon, scratch + kPickup};
    const uint32_t srcVec[5] = {scratch + kVecs, scratch + kVecs + 16, scratch + kVecs + 32, scratch + kVecs + 48, scratch + kVecs + 64};
    (void)w;
    // Update list head is built backwards so the first object ends up at the head.
    ptr32<_sOColObj> head = nullptr;
    int linked = 0;
    for (int k = out.count - 1; k >= 0; --k)
    {
        const uint32_t address = scratch + kListObjs + 0x80u * k;
        const int kind = (k < 2) ? k : 2 + rng.below(3);
        _sOColObj &o = at<_sOColObj>(address);
        std::memset(&o, 0, sizeof(o));
        o.type = static_cast<uint8>(types[kind]);
        o.pData = ptr32<void>(datas[kind]);
        const Vec3 p = k < 2 ? Vec3{at<float>(srcVec[kind]), at<float>(srcVec[kind] + 4), at<float>(srcVec[kind] + 8)}
                              : Vec3{rng.range(-300.0f, 300.0f), rng.range(0.0f, 100.0f), rng.range(-300.0f, 300.0f)};
        // Near the grid's edge now and then (its cell range is clamped), never so far that a cell is outside RAM's reach.
        const bool edge = rng.chance(15);
        setVec(at<_sColCellVecPSX>(scratch + kListVecs + 16u * k),
               edge ? Vec3{rng.range(-800.0f, 800.0f), p.y, rng.range(-800.0f, 800.0f)} : p);
        at<float>(scratch + kListRads + 4u * k) = (!edge && rng.chance(15)) ? rng.range(129.0f, 220.0f) : rng.range(20.0f, 128.0f);
        o.pPos = ptr32<_sColCellVecPSX>(scratch + kListVecs + 16u * k);
        o.pRad = ptr32<ee_float>(scratch + kListRads + 4u * k);
        o.interestMask = static_cast<uint8>(rng.chance(80) ? 0x1f : rng.below(32));
        o.interestType = static_cast<uint8>(1u << rng.below(5));
        o.enabled = rng.chance(85) ? 1 : 0;
        o.updated = rng.chance(60) ? 1 : 0;
        o.nonMover = rng.chance(30) ? 1 : 0;
        o.dealtWith = rng.chance(10) ? 1 : 0;
        o.cellX = o.cellZ = o.cellXS = o.cellZS = o.cellXE = o.cellZE = -999;
        o.min.x = o.min.y = o.min.z = -50.0f;
        o.max.x = o.max.y = o.max.z = 50.0f;
        if (inCells && rng.chance(60))
            linkIntoCell(scratch, address, rng.below(kGridSide), rng.below(kGridSide));
        o.cellXS = rng.below(kGridSide);
        o.cellZS = rng.below(kGridSide);
        o.cellXE = rng.below(kGridSide);
        o.cellZE = rng.below(kGridSide);
        if (rng.chance(85)) // the update list
        {
            o.pUpdNext = head;
            if (head)
                head->pUpdPrev = ptr32<_sOColObj>(address);
            head = ptr32<_sOColObj>(address);
            ++linked;
        }
        out.addr[k] = address;
    }
    // Fix the head sentinel: the first object's pUpdPrev is the address of oColG.pHead.
    if (head)
        head->pUpdPrev = ptr32<_sOColObj>(0x327f44u);
    G(oColG).pHead = head;
    G(oColG).numObjects = linked;
}

} // namespace

void ocolObject(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    Objects objects{};
    buildList(rng, scratch, true, objects);
    // The cell functions index the grid with the object's cell, so give every object a real one.
    for (int k = 0; k < objects.count; ++k)
    {
        _sOColObj &o = at<_sOColObj>(objects.addr[k]);
        if (o.cellX == -999)
        {
            o.cellX = rng.below(kGridSide);
            o.cellZ = rng.below(kGridSide);
        }
    }
    setArg(ctx, 0, objects.addr[rng.below(objects.count)]);
}

void ocolInitObject(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    Objects objects{};
    buildList(rng, scratch, false, objects);
    // One more object that is in neither list, of any type (types over 4 leave the masks alone).
    const uint32_t address = scratch + kListObjs + 0x80u * 8;
    std::memset(guest_host(address), 0, sizeof(_sOColObj));
    at<_sOColObj>(address).type = static_cast<uint8>(rng.chance(85) ? rng.below(5) : rng.below(256));
    if (rng.chance(30))
    {
        G(oColG).pHead = nullptr;
        G(oColG).numObjects = 0;
    }
    setArg(ctx, 0, address);
    setArg(ctx, 1, scratch + kListVecs);
    setArg(ctx, 2, scratch + kListRads);
    setArg(ctx, 3, scratch + kObjA);
}

void ocolList(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    Objects objects{};
    buildList(rng, scratch, true, objects);
    (void)ctx;
}

void ocolReorder(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    Objects objects{};
    buildList(rng, scratch, false, objects);
    G(FPhysLib).numCars = rng.below(3);
    (void)ctx;
}

} // namespace rc::fuzz
