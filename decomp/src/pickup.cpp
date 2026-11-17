// Decompiled from PICKUP.c: the weapon pickups on the track. The level's pickup positions (pLvlG->pPickupPosData) are
// spawned as a linked list of 200-byte _sPickup records (pickupVarG.pPickups), each an OCOL object that CarHitPickup
// is called for and a task (PickupProcess) that brings a taken pickup back. Taken pickups (state 3) wait 5 s, flicker
// (state 2) for 1 s, then are live again (state 1). The weapon given comes from the level's tables: one per race
// position for the players and for the computer's cars (a count, then weapon numbers), or a car's special weapon.
//
// Written from the instructions. PickupGiveWeapon divides unsigned (Ghidra shows signed) and the computer's table
// entries are signed bytes, the players' unsigned.

#include "pickup_fuzz.h"
#include "weapon.h"

namespace rc {

bool_ MemBlockAllocLo(ptr32<memHandle> pHandle, uint32 size); // mem.cpp
void MemBlockFree(ptr32<memHandle> pHandle);                  // mem.cpp
uint32 RandomGet();                                           // math_core.cpp
uint16 RandomGetRange(uint16 lower, uint16 upper);            // math_core.cpp

void PickupTest(ptr32<_sPickup> pPickup, tTime time);
int32 PickupTestUltimate(int32 weaponNo, int32 flags);

namespace {

using weapon::at;

constexpr uint32_t kPickupVars = sym::pickupVarG;
constexpr uint32_t kPickups = kPickupVars + 0x40u, kPickupPos = kPickupVars + 0x44u, kNumPickups = kPickupVars + 0x48u;
constexpr uint32_t kLastUpdated = kPickupVars + 0x4cu, kUltimatesGiven = kPickupVars + 0x50u, kRadius = kPickupVars + 0x54u;
constexpr uint32_t kRendObj = kPickupVars + 0x58u, kAngles = kPickupVars + 0x68u, kMatrix = kPickupVars + 0x70u;
constexpr uint32_t kMainVars = sym::mainVarG, kMainTime = kMainVars + 0x1cu, kMainLevel = kMainVars + 0x34au;
constexpr uint32_t kMainPauseMenu = kMainVars + 0x366u, kMainPickupsOnOff = kMainVars + 0x3a8u;
constexpr uint32_t kVisMask0 = (sym::visVarG + 0x8u), kVisMask1 = (sym::visVarG + 0xcu);
constexpr uint32_t kCameraPtr = (sym::cameraVarG + 0xb70u), kObjectList = sym::objectVarG, kNoObjects = (sym::objectVarG + 0x170u), kRaceOrder = (sym::aiVarG + 0x4u);
constexpr uint32_t kLevelPtr = sym::pLvlG;
constexpr uint32_t kPickupProcess = sym::PickupProcess, kPrintf = sym::printf, kNotOverGround = (sym::sec_rodata + 0x15d0u);

// A car's special weapon (25% of the time), by object type: the jump table at 0x323c00 (0x323ca0, 0x323d40).
int32_t specialWeapon(uint32_t type)
{
    switch (type)
    {
    case 0x02: case 0x15: return 0xd;  // Skullduggery
    case 0x03: case 0x16: return 0xc;  // Sarge
    case 0x0b: case 0x1e: return 0x12; // Concept
    case 0x0e: case 0x21: return 0x11; // Yella
    case 0x10: case 0x23: return 0xe;  // armoured car
    case 0x11: case 0x24: return 0xf;  // dumper
    case 0x12: case 0x25: return 0x10; // moon rover
    case 0x13: case 0x26: return 0x13; // rocket car
    default: return 0;
    }
}

int32_t rollSpecial(uint32_t type)
{
    return RandomGetRange(1, 100) < 0x19 ? specialWeapon(type) : 0;
}

} // namespace

void PickupInit()
{
    at<uint32_t>(kRadius) = 0x42100000u; // 36.0
    at<uint32_t>(kPickups) = 0;
    at<int32_t>(kLastUpdated) = 0;
    MatPsxInit(ptr32<_matPsx>(kMatrix));
    at<uint16_t>(kAngles + 0u) = 0;
    at<uint16_t>(kAngles + 2u) = 0;
    at<uint16_t>(kAngles + 4u) = 0;
    guest_call(weapon::kMat3nBuildXYZ, kMatrix, kAngles); // a tail call
}

void PickupTerm() {}

void PickupTest(ptr32<_sPickup>, tTime) {}

// Every pickup of the level: placed 60 above its position and dropped onto the ground (or left where it is, with a
// message, over none), its collision object, and its task, started within 10 ms of time.
void PickupAllSpawn(tTime time)
{
    GuestFrame frame(0xb0);
    const uint32_t sp = frame.local<uint8_t>(0).address;
    at<int32_t>(sp + 8u) = time;
    at<int32_t>(kUltimatesGiven) = 0;
    if (at<uint32_t>(kNumPickups) == 0)
        return;
    uint32_t pos = at<uint32_t>(kPickupPos);
    uint32_t i = 0;
    do
    {
        at<uint32_t>(sp + 4u) = 0;
        MemBlockAllocLo(ptr32<memHandle>(sp + 4u), 200);
        uint32_t p = at<uint32_t>(sp + 4u);
        const uint32_t next = at<uint32_t>(kPickups);
        at<uint32_t>(p + 4u) = at<uint32_t>(pos + 0u);
        at<uint32_t>(kPickups) = p;
        at<uint8_t>(p + 0xa8u) = 1;
        at<uint32_t>(p + 8u) = at<uint32_t>(pos + 4u);
        at<uint32_t>(at<uint32_t>(sp + 4u)) = next;
        at<uint8_t>(at<uint32_t>(sp + 4u) + 0xa9u) = 0;
        p = at<uint32_t>(sp + 4u);
        at<int32_t>(p + 0xacu) = 0;
        at<uint16_t>(p + 0xaau) = at<uint16_t>(pos + 0xeu);
        at<ee_float>(p + 0x78u) = ee_itof(at<int16_t>(pos + 8u));
        at<ee_float>(p + 0x7cu) = ee_itof(at<int16_t>(pos + 0xau) + 0x3c);
        at<ee_float>(p + 0x80u) = ee_itof(at<int16_t>(pos + 0xcu));
        const bool_ ground = AColDownCast(ptr32<_sColCellVecPSX>(p + 0x78u), ptr32<_sColCellVecPSX>(p + 0x88u),
                                          ptr32<_sColNormalPSX>(p + 0x98u), ptr32<uint8>(sp));
        p = at<uint32_t>(sp + 4u);
        if (!ground)
        {
            guest_call(kPrintf, kNotOverGround, static_cast<uint32_t>(at<uint8_t>(kMainLevel)), i);
            p = at<uint32_t>(sp + 4u);
            const uint64_t a = at<uint64_t>(p + 0x78u), b = at<uint64_t>(p + 0x80u);
            at<uint64_t>(p + 0x88u) = a;
            at<uint64_t>(p + 0x90u) = b;
            const ee_float y = at<ee_float>(p + 0x8cu);
            at<uint32_t>(p + 0x98u) = 0;
            at<uint32_t>(p + 0x9cu) = 0x45800000u; // 4096.0
            at<uint32_t>(p + 0xa0u) = 0;
            at<ee_float>(p + 0x8cu) = y - ee_float(60.0f);
        }
        if (at<uint8_t>(kMainLevel) == 0x61)
            at<ee_float>(p + 0x7cu) = at<ee_float>(p + 0x8cu) + ee_float(60.0f);
        at<uint8_t>(p + 0x2cu) = 1;
        ++i;
        pos += 0x10u;
        p = at<uint32_t>(sp + 4u);
        OColInitObj(ptr32<_sOColObj>(p + 0xcu), ptr32<_sColCellVecPSX>(p + 0x78u), ptr32<ee_float>(kRadius), ptr32<void>(p));
        p = at<uint32_t>(sp + 4u);
        const uint16_t delay = RandomGetNDRange(0, 10);
        guest_call(weapon::kTaskSet, p + 0xb0u, kPickupProcess, p, at<int32_t>(sp + 8u) + static_cast<int32_t>(delay));
    } while (i < at<uint32_t>(kNumPickups));
}

// A pickup's task: a taken one waits until its respawn time, then flickers for a second, then is live again.
void PickupProcess(ptr32<void> pData, tTime time)
{
    const uint32_t p = pData.address;
    {
        GuestFrame frame(0x30);
        const uint8_t state = at<uint8_t>(p + 0xa8u);
        if (state == 3)
        {
            const int32_t respawn = at<int32_t>(p + 0xacu);
            if (time < respawn)
                at<int32_t>(p + 0xb4u) = respawn;
            else
            {
                at<uint8_t>(p + 0xa8u) = 2;
                at<int32_t>(p + 0xacu) = time + 1000;
                at<int32_t>(p + 0xb4u) = time + 1000;
            }
            guest_call(weapon::kTaskCachedInsert, p + 0xb0u);
        }
        else if (state == 2 && !(time < at<int32_t>(p + 0xacu)))
        {
            at<uint8_t>(p + 0xa8u) = 1;
            OColInitObj(ptr32<_sOColObj>(p + 0xcu), ptr32<_sColCellVecPSX>(p + 0x78u), ptr32<ee_float>(kRadius), pData);
        }
    }
    PickupTest(ptr32<_sPickup>(p), time); // a tail call
}

// Draws the pickups the camera can see (in a visible cell and within 0x3a9800 on each axis): live ones, and
// flickering ones every other frame (or always while paused). The model spins 5 units a millisecond.
void PickupRenderAll(tTime time)
{
    const uint32_t camera = at<uint32_t>(kCameraPtr);
    const int32_t delta = time - at<int32_t>(kLastUpdated);
    if (at<uint32_t>(kMainPickupsOnOff) == 0)
        return;
    uint32_t p = at<uint32_t>(kPickups);
    GuestFrame frame(0x100);
    const uint32_t sp = frame.local<uint8_t>(0).address;
    at<int32_t>(sp + 0x40u) = time;
    const ptr32<sRdrStateBlock> block(sp + 0x10u);
    RdrState_SetStateBlockToDefaults(block);
    RdrState_SetAlphaBlending(block, 1, 2, 2, 0, 0x7f);
    RdrState_SetPixelAlphaBlendControlEnable(block, 0);
    RdrState_SetZBufferWriteEnable(block, 0);
    RdrState_ForceApplyStateBlock(block);
    at<uint16_t>(kAngles + 2u) = static_cast<uint16_t>(at<uint16_t>(kAngles + 2u) + static_cast<uint32_t>(delta) * 5u);
    guest_call(weapon::kMat3nBuildXYZ, kMatrix, kAngles);
    uint32_t count = at<uint32_t>(kNumPickups);
    const uint32_t mask1 = at<uint32_t>(kVisMask1), mask0 = at<uint32_t>(kVisMask0);
    if (count != 0)
    {
        const ee_float k256(256.0f);
        for (uint32_t i = 0; i < count; ++i, p = at<uint32_t>(p))
        {
            if ((mask0 & at<uint32_t>(p + 4u)) == 0 && (mask1 & at<uint32_t>(p + 8u)) == 0)
                continue;
            const ee_float y = at<ee_float>(p + 0x7cu) - ee_float(60.0f);
            const int32_t x = ee_ftoi(at<ee_float>(p + 0x78u) * k256);
            const int32_t z = ee_ftoi(at<ee_float>(p + 0x80u) * k256);
            const int32_t yy = ee_ftoi(y * k256);
            at<int32_t>(kRendObj + 4u) = x;
            at<int32_t>(kRendObj + 0xcu) = z;
            at<int32_t>(kRendObj + 8u) = yy;
            const auto absolute = [](int32_t v) { return v < 0 ? static_cast<int32_t>(0u - static_cast<uint32_t>(v)) : v; };
            const int32_t dx = static_cast<int32_t>(static_cast<uint32_t>(x) - at<uint32_t>(camera + 0xcu));
            at<int32_t>(sp + 0u) = dx;
            const int32_t dy = static_cast<int32_t>(static_cast<uint32_t>(yy) - at<uint32_t>(camera + 0x10u));
            at<int32_t>(sp + 4u) = dy;
            const int32_t dz = static_cast<int32_t>(static_cast<uint32_t>(z) - at<uint32_t>(camera + 0x14u));
            at<int32_t>(sp + 8u) = dz;
            if (0x3a97ff < absolute(dx) || 0x3a97ff < absolute(dy) || 0x3a97ff < absolute(dz))
                continue;
            const uint8_t state = at<uint8_t>(p + 0xa8u);
            if (state == 1)
                RendObjRender(ptr32<_sRendObj>(kRendObj), ptr32<SVECTOR>(nullptr), ptr32<SVECTOR>(nullptr), 0);
            else if (state == 2 && (at<uint8_t>(p + 0xa9u) != 0 || at<uint16_t>(kMainPauseMenu) != 0))
                RendObjRender(ptr32<_sRendObj>(kRendObj), ptr32<SVECTOR>(nullptr), ptr32<SVECTOR>(nullptr), 0);
            at<uint8_t>(p + 0xa9u) = static_cast<uint8_t>(at<uint8_t>(p + 0xa9u) ^ 1u);
            count = at<uint32_t>(kNumPickups);
        }
    }
    at<int32_t>(kLastUpdated) = at<int32_t>(sp + 0x40u);
}

// 0, or the ultimate weapon (9) once per lap of the leading player's car: otherwise a replacement, from the
// computer's last table (flags set) or at random.
int32 PickupTestUltimate(int32 weaponNo, int32 flags)
{
    if (weaponNo != 9)
        return 0;
    GuestFrame frame(0x30);
    const int32_t given = at<int32_t>(kUltimatesGiven);
    if (given == at<int16_t>(at<uint32_t>(kObjectList) + 0x104u))
    {
        at<int32_t>(kUltimatesGiven) = given + 1;
        return 9;
    }
    if (flags != 0)
    {
        const uint32_t table = at<uint32_t>(kPickupVars + 0x3cu);
        return RandomGetRange(0, 100) < 0x32 ? at<uint8_t>(table + 2u) : at<uint8_t>(table + 3u);
    }
    RandomGet();
    for (;;)
    {
        const int32_t weapon = static_cast<int32_t>(RandomGet() % 11u + 1u);
        if (weapon != 9)
            return weapon;
    }
}

int32 PickupGiveWeaponSpecial(int32 objectType)
{
    GuestFrame frame(0x20);
    return rollSpecial(static_cast<uint32_t>(objectType));
}

// The weapon a pickup gives: its own type if set, else the car's special weapon, else one from the table for the
// race position (scaled to 8 positions in a smaller race), else one at random.
int32 PickupGiveWeapon(ptr32<_sPickup> pPickup, int32 playerType, int32 position)
{
    GuestFrame frame(0x50);
    const uint32_t objects = at<uint32_t>(kNoObjects);
    uint32_t row = static_cast<uint32_t>(position);
    if (objects < 8)
    {
        if (row == objects - 1)
            row = 7;
        else if (row != 0)
            row = ee_divu((row + 1u) * 8u, objects) - 1u;
    }
    const uint16_t type = at<uint16_t>(pPickup.address + 0xaau);
    if (type != 0)
        return type;
    const uint32_t car = at<uint32_t>(kRaceOrder + static_cast<uint32_t>(position) * 12u);
    const uint32_t objectType = at<uint32_t>(at<uint32_t>(car + 0xcu) + 4u);
    const int32_t special = rollSpecial(objectType);
    if (special != 0)
        return special;
    if (static_cast<uint32_t>(playerType) < 2u)
    {
        if (RandomGetRange(0, 100) < 0x50)
        {
            const uint8_t n = at<uint8_t>(at<uint32_t>(kPickupVars + row * 4u));
            const uint32_t r = RandomGet();
            return at<uint8_t>(at<uint32_t>(kPickupVars + row * 4u) + ((n ? r % n : r) + 1u & 0xffu));
        }
        return static_cast<int32_t>(RandomGet() % 11u + 1u);
    }
    int32_t weapon;
    int32_t flags;
    if (RandomGetRange(0, 100) < 0x50)
    {
        const uint8_t n = at<uint8_t>(at<uint32_t>(kPickupVars + 0x20u + row * 4u));
        const uint32_t r = RandomGet();
        flags = 1;
        weapon = at<int8_t>(at<uint32_t>(kPickupVars + 0x20u + row * 4u) + ((n ? r % n : r) + 1u & 0xffu));
    }
    else
    {
        flags = 0;
        weapon = static_cast<int32_t>(RandomGet() % 11u + 1u);
    }
    const int32_t ultimate = PickupTestUltimate(weapon, flags);
    return ultimate != 0 ? ultimate : weapon;
}

// Frees every pickup (task, collision object, memory); the list head is left as it was.
void PickupDeleteAll()
{
    GuestFrame frame(0x50);
    const uint32_t sp = frame.local<uint8_t>(0).address;
    at<uint32_t>(sp) = at<uint32_t>(kPickups);
    if (at<uint32_t>(kMainPickupsOnOff) == 0)
        return;
    for (uint32_t i = 0; i < at<uint32_t>(kNumPickups);)
    {
        const uint32_t p = at<uint32_t>(sp);
        const uint32_t next = at<uint32_t>(p);
        guest_call(weapon::kTaskDelete, p + 0xb0u);
        OColDeleteObject(ptr32<_sOColObj>(at<uint32_t>(sp) + 0xcu));
        MemBlockFree(ptr32<memHandle>(sp));
        ++i;
        if (!(i < at<uint32_t>(kNumPickups)))
            break;
        at<uint32_t>(sp) = next;
    }
    at<int32_t>(kLastUpdated) = 0;
}

// A car drives through a live pickup (unless the race is over or it already holds or is spinning for a weapon): the
// pickup is taken for 5 s and the car gets its weapon, with the spin effect and, for a player, a sound.
void CarHitPickup(ptr32<_sObject> pCar, ptr32<_sPickup> pPickup)
{
    const uint32_t car = pCar.address, p = pPickup.address;
    if (at<uint32_t>(kMainVars) == 0x20)
        return;
    if ((at<uint32_t>(car + 0x194u) & 0x00ff00ffu) != 0)
        return;
    {
        GuestFrame frame(0x30);
        at<uint8_t>(p + 0xa8u) = 3;
        const int32_t t = at<int32_t>(kMainTime) + 5000;
        at<int32_t>(p + 0xb4u) = t;
        at<int32_t>(p + 0xacu) = t;
        guest_call(weapon::kTaskCachedInsert, p + 0xb0u);
        const int32_t weaponNo = PickupGiveWeapon(pPickup, at<uint8_t>(car + 0x44u), at<int16_t>(car + 0x13cu));
        VfxAddNew(pCar, static_cast<int32>(p), weaponNo, 0x15);
        at<uint8_t>(car + 0x196u) = 1;
        if (at<uint8_t>(car + 0x44u) != 2)
            guest_call(weapon::kSfxPlay, 0x1a, 0xc4, 0xf, 0xf, 0x100, 0, 0);
    }
    OColDeleteObject(ptr32<_sOColObj>(p + 0xcu)); // a tail call
}

// The level's pickup positions, model and weapon tables; the level's table pointer moves past the 16 used.
void PickupSetupClass()
{
    const uint32_t level = at<uint32_t>(kLevelPtr);
    const uint32_t res = at<uint32_t>(level + 0x38u);
    const uint32_t head = at<uint32_t>(res), num = at<uint32_t>(res + 4u);
    at<uint32_t>(kPickupPos) = res + 8u;
    at<uint32_t>(kNumPickups) = num;
    at<uint32_t>(kRendObj) = head;
    const uint32_t table = at<uint32_t>(level + 0x34u);
    uint32_t words[16];
    for (uint32_t k = 0; k < 8; ++k)
        words[k] = at<uint32_t>(table + k * 4u);
    for (uint32_t k = 0; k < 8; ++k)
        at<uint32_t>(kPickupVars + k * 4u) = words[k];
    for (uint32_t k = 8; k < 16; ++k)
        words[k] = at<uint32_t>(table + k * 4u);
    at<uint32_t>(level + 0x34u) = table + 0x40u;
    at<uint32_t>(kPickupVars + 0x3cu) = words[15];
    for (uint32_t k = 8; k < 15; ++k)
        at<uint32_t>(kPickupVars + k * 4u) = words[k];
}

} // namespace rc

RC_DECOMP_FN_SETUP_HEAP(PickupInit, rc::fuzz::pickupNone)
RC_DECOMP_FN(PickupTerm)
RC_DECOMP_FN(PickupTest)
RC_DECOMP_FN_SETUP_HEAP(PickupAllSpawn, rc::fuzz::pickupSpawn)
RC_DECOMP_FN_SETUP_HEAP(PickupProcess, rc::fuzz::pickupOne)
RC_DECOMP_FN_SETUP_HEAP(PickupRenderAll, rc::fuzz::pickupRender)
RC_DECOMP_FN_SETUP(PickupTestUltimate, rc::fuzz::pickupUltimate)
RC_DECOMP_FN_SETUP(PickupGiveWeaponSpecial, rc::fuzz::pickupSpecial)
RC_DECOMP_FN_SETUP_HEAP(PickupGiveWeapon, rc::fuzz::pickupGive)
RC_DECOMP_FN_SETUP_HEAP(PickupDeleteAll, rc::fuzz::pickupRace)
RC_DECOMP_FN_SETUP_HEAP(CarHitPickup, rc::fuzz::pickupHit)
RC_DECOMP_FN_SETUP_HEAP(PickupSetupClass, rc::fuzz::pickupLevel)
