// Decompiled from PHYSICS.c: the game's side of the car physics library (physics.cpp). PhysicsAddCar turns a car's
// definition (its integer _sOPhysLibCarData, the wheel models' heights) into the library's float car, finds its
// spheres (four wheels by tag, then every body sphere node, the one nearest the middle moved to slot 4), drops it onto
// the ground at its start and points it the way the definition says. After each step the float state is copied into
// the integer state (_private) the rest of the game reads.
//
// Written from the instructions. The constants at 0x3276d0-0x3276e0 are read from the game's data as the original does.

#include "physics_fuzz.h"
#include "weapon.h"

namespace rc {

bool_ MemBlockAllocLo(ptr32<memHandle> pHandle, uint32 size);   // mem.cpp
void MemBlockFree(ptr32<memHandle> pHandle);                     // mem.cpp
void MemBlockSet32(memHandle handle, uint32 value, uint32 size); // mem.cpp
bool_ AColDownCast(ptr32<_sColCellVecPSX> pPos, ptr32<_sColCellVecPSX> pPoi, ptr32<_sColNormalPSX> pNormal,
                   ptr32<uint8> pMatId);                              // collision.cpp
int32 AINodeGetNearestHeading(ptr32<SVECTOR> svPos);                  // ai.cpp
ptr32<_sVfx_Object> VfxAddNew(ptr32<_sObject> pPlayerObj, int32 iY, int32 iZ, int32 iType); // visfx12.cpp
void PlayerPhysicsVibrations(ptr32<_sObject> pObject);                // ai.cpp
ptr32<_sNode> NodeFindByTag(ptr32<_sNodeHead> pNodeHead, uint32 tag); // misc.cpp
ptr32<_sNode> NodeFindNextByTag(ptr32<_sNode> pCurrNode, ptr32<_sNodeHead> pNodeHead, uint32 tag); // misc.cpp
int32_t FPhysLibInit();                                               // physics.cpp
int32_t FPhysLibTerm();
int32_t FPhysLibAddCar(ptr32<_sFPhysLibCar> pCar);
int32_t FPhysLibDeleteCar(ptr32<_sFPhysLibCar> pCar);
int32_t FPhysLibSimulation();
int32_t FPhysLibCollisionReponse();
int32_t FPhysLibSphereInitFixup(ptr32<_sFPhysLibCar> pCar);
void FPhysLibUpdateCarSpheres(ptr32<_sFPhysLibCar> pCar, int32_t bFullUpdate);
void FMatRotateY(ptr32<_sFMat> pMatrix, ee_float psxangle);
void GameTaskGetShadowMaterialInfo(bool_ fastMethod); // gametask.cpp

void PhysicsApplyForce(ptr32<_sFPhysLibCar> pPhysics, ptr32<_sFVec> pForce, ptr32<_sFVec> pApplicationOffset, int16 flags);
void PhysicsSetOldIntegerState(ptr32<_sFPhysLibCar> pPhysics, int32_t bSet);
void PhysicsCollisionHelpOut(ptr32<_sFPhysLibCar> pPhysics);
void InitCarSpheres(ptr32<_sFPhysLibCar> pPhysics, ptr32<_sObjectDef> pObjDef);
void LoadCarDataResource(ptr32<_sFPhysLibCar> pPhysics, ptr32<_sObjectDef> pObjDef);

namespace {

using weapon::at;

constexpr uint32_t kPhysicsGlobal = sym::PhysicsGlobal; // bInited, bLevelInited, unused, numCars
constexpr uint32_t kWheelRadiusScale = sym::lit4_3276d0, kRideHeight = sym::lit4_3276d4, kForceScale = sym::lit4_3276d8;
constexpr uint32_t kSpringScale = sym::lit4_3276dc, kBoatThrustScale = sym::lit4_3276e0, kHelpOutScale = sym::_dbgHelpOutScale_534;
constexpr uint32_t kWheelTags = sym::wheelTag_562, kBodySphereTag = 0x10000235u;
constexpr uint32_t kObjectList = sym::objectVarG, kMainVars = sym::mainVarG, kFPhysLib = sym::FPhysLib;
const ee_float kS = ee_float(0.00024414062f); // 1 / 4096
const ee_float kToCell = ee_float(0.00390625f); // 1 / 256

ee_float fat(uint32_t address) { return at<ee_float>(address); }

// Row r of the car's matrix times v, scaled by 1/4096: (a*x + b*y) + c*z.
ee_float matRow(uint32_t mat, int r, ee_float x, ee_float y, ee_float z)
{
    const uint32_t row = mat + static_cast<uint32_t>(r) * 12u;
    return (fat(row) * x + fat(row + 4u) * y + fat(row + 8u) * z) * kS;
}

// Sets the matrix to the identity (scaled by 4096) and turns it to a heading.
void headingMatrix(uint32_t mat, ee_float heading)
{
    std::memset(guest_host(mat), 0, 0x24);
    at<ee_float>(mat + 0x00u) = ee_float(4096.0f);
    at<ee_float>(mat + 0x20u) = ee_float(4096.0f);
    at<ee_float>(mat + 0x10u) = ee_float(4096.0f);
    FMatRotateY(ptr32<_sFMat>(mat), heading);
}

} // namespace

int32_t PhysicsAddCar(ptr32<_sObject> pObject, ptr32<_sObjectDef> pObjDef)
{
    GuestFrame frame(0x80);
    const uint32_t sp = frame.local<uint8_t>(0).address;
    const uint32_t phys = at<uint32_t>(pObject.address + 0x20u);
    MemBlockSet32(memHandle(phys), 0, 0x23c);
    if (at<uint32_t>(pObjDef.address + 4u) == 0xfu) // OBJECT_TYPE_UFO
        at<uint8_t>(phys + 0x1cfu) = 1;
    LoadCarDataResource(ptr32<_sFPhysLibCar>(phys), pObjDef);
    const uint32_t wheelRes = at<uint32_t>(at<uint32_t>(pObject.address + 0x2cu) + 0x10u);
    if (wheelRes != 0)
    {
        const ee_float scale = fat(kWheelRadiusScale);
        const uint32_t front = at<uint32_t>(at<uint32_t>(wheelRes) + 4u);
        const uint32_t data = at<uint32_t>(phys + 0x1e4u);
        const uint32_t rearList = at<uint32_t>(wheelRes + 8u);
        at<ee_float>(data + 0x2cu) =
            ee_itof((static_cast<int32_t>(at<int16_t>(front + 0x70u)) - at<int16_t>(front + 0x6au)) << 7) * scale;
        const uint32_t rear = at<uint32_t>(rearList + 4u);
        at<ee_float>(data + 0x68u) =
            ee_itof((static_cast<int32_t>(at<int16_t>(rear + 0x70u)) - at<int16_t>(rear + 0x6au)) << 7) * scale;
    }
    InitCarSpheres(ptr32<_sFPhysLibCar>(phys), pObjDef);
    FPhysLibAddCar(ptr32<_sFPhysLibCar>(phys));
    at<uint8_t>(kPhysicsGlobal + 3u) = static_cast<uint8_t>(at<uint8_t>(kPhysicsGlobal + 3u) + 1u);
    // pos at sp, poi at sp+0x10 (a copy of all 16 bytes of pos, its pad word stale), norm at sp+0x20, mat at sp+0x30.
    at<ee_float>(sp + 0u) = ee_itof(at<int32_t>(pObjDef.address + 8u));
    at<ee_float>(sp + 4u) = ee_itof(at<int32_t>(pObjDef.address + 0xcu));
    at<ee_float>(sp + 8u) = ee_itof(at<int32_t>(pObjDef.address + 0x10u));
    std::memcpy(guest_host(sp + 0x10u), guest_host(sp), 16);
    at<ee_float>(sp + 4u) = fat(sp + 4u) + ee_float(64.0f);
    AColDownCast(ptr32<_sColCellVecPSX>(sp), ptr32<_sColCellVecPSX>(sp + 0x10u), ptr32<_sColNormalPSX>(sp + 0x20u),
                 ptr32<uint8>(sp + 0x30u));
    const ee_float k256 = ee_float(256.0f);
    at<ee_float>(phys + 0x14u) = fat(sp + 0x10u) * k256;
    at<ee_float>(phys + 0x18u) = fat(sp + 0x14u) * k256 + fat(kRideHeight);
    at<ee_float>(phys + 0x1cu) = fat(sp + 0x18u) * k256;
    headingMatrix(phys + 0x4cu, ee_itof(static_cast<int32_t>(at<int16_t>(pObjDef.address + 0x16u)) << 4));
    at<uint8_t>(phys + 0x1cdu) = at<uint8_t>(phys + 0x1cdu) | 1u;
    PhysicsSetOldIntegerState(ptr32<_sFPhysLibCar>(phys), 1);
    return 0;
}

// The steering help for a stuck player car (a body sphere in contact for 6 frames): turns it by steer * throttle.
void PhysicsCollisionHelpOut(ptr32<_sFPhysLibCar> pPhysics)
{
    const uint32_t p = pPhysics.address;
    const uint32_t spheres = at<uint32_t>(p + 0x1e0u);
    if (at<int16_t>(spheres + 0x830u) < 6 && at<int16_t>(spheres + 0x9c0u) < 6 && at<int16_t>(spheres + 0xb50u) < 6 &&
        at<int16_t>(spheres + 0xce0u) < 6)
        return;
    const int32_t v = static_cast<int32_t>(static_cast<uint32_t>(at<int16_t>(p + 0u) * at<int16_t>(p + 2u)) *
                                           static_cast<uint32_t>(at<int32_t>(kHelpOutScale)));
    const int32_t turn = (v < 0 ? v + 0xfff : v) >> 12;
    at<ee_float>(p + 0xa4u) = fat(p + 0xa4u) - ee_itof(turn);
}

int32_t PhysicsCollisionResponseUpdate()
{
    FPhysLibCollisionReponse();
    for (uint32_t player = 0; player < 2; ++player)
    {
        if (at<uint8_t>(kMainVars + 0x369u) <= player) // noHuman
            continue;
        const ptr32<_sObject> obj = at<ptr32<_sObject>>(kObjectList + 4u * player);
        PlayerPhysicsVibrations(obj);
        PhysicsCollisionHelpOut(ptr32<_sFPhysLibCar>(at<uint32_t>(obj.address + 0x20u)));
    }
    for (int32_t i = 0; i < at<int32_t>(kFPhysLib + 0x60u); ++i)
        PhysicsSetOldIntegerState(at<ptr32<_sFPhysLibCar>>(kFPhysLib + 4u * static_cast<uint32_t>(i)), 1);
    GameTaskGetShadowMaterialInfo(at<int32_t>(kMainVars + 8u) != 1 ? 1 : 0); // not DO_FRONT_END
    return 0;
}

// Puts a car back on its wheels where it is: on the ground below (or 32 above where it was), stopped, facing the
// route's nearest heading.
int32_t PhysicsRightCar(ptr32<_sObject> pObject)
{
    GuestFrame frame(0xe0);
    const uint32_t sp = frame.local<uint8_t>(0).address;
    const uint32_t car = at<uint32_t>(pObject.address + 0x20u);
    at<ee_float>(sp + 0u) = fat(car + 0x14u) * kToCell;
    at<ee_float>(sp + 4u) = fat(car + 0x18u) * kToCell + ee_float(64.0f);
    at<ee_float>(sp + 8u) = fat(car + 0x1cu) * kToCell;
    std::memcpy(guest_host(sp + 0x10u), guest_host(sp), 16);
    if (!AColDownCast(ptr32<_sColCellVecPSX>(sp), ptr32<_sColCellVecPSX>(sp + 0x10u), ptr32<_sColNormalPSX>(sp + 0x20u),
                      ptr32<uint8>(sp + 0x50u)))
        at<ee_float>(sp + 0x14u) = fat(sp + 4u) + ee_float(32.0f);
    at<uint8_t>(car + 0x1cdu) = at<uint8_t>(car + 0x1cdu) | 1u;
    at<ee_float>(car + 0x18u) = (fat(sp + 0x14u) + ee_float(32.0f)) * ee_float(256.0f);
    // The zero vectors go through stack temporaries (sp+0x60 to 0x88).
    for (uint32_t off : {0x70u, 0x74u, 0x78u, 0x60u, 0x64u, 0x68u})
        at<uint32_t>(sp + off) = 0;
    at<uint32_t>(car + 0x20u) = 0;
    at<uint32_t>(car + 0x24u) = 0;
    at<uint32_t>(car + 0x28u) = 0;
    for (uint32_t off : {0x80u, 0x84u, 0x88u})
        at<uint32_t>(sp + off) = 0;
    at<uint32_t>(car + 0x40u) = 0;
    at<uint32_t>(car + 0x44u) = 0;
    at<uint32_t>(car + 0x48u) = 0;
    for (uint32_t w = 0; w < 4; ++w)
    {
        at<uint32_t>(car + 0xe0u + w * 0x48u) = 0; // angVel
        at<uint16_t>(car + 0xe8u + w * 0x48u) = 0; // flags
    }
    at<int16_t>(sp + 0x40u) = static_cast<int16_t>(ee_ftoi(fat(sp + 0x10u)));
    at<int16_t>(sp + 0x42u) = static_cast<int16_t>(ee_ftoi(fat(sp + 0x14u)));
    at<int16_t>(sp + 0x44u) = static_cast<int16_t>(ee_ftoi(fat(sp + 0x18u)));
    at<int16_t>(sp + 0x30u) = 0;
    const int32 heading = AINodeGetNearestHeading(ptr32<SVECTOR>(sp + 0x40u));
    at<int16_t>(sp + 0x32u) = static_cast<int16_t>(heading);
    at<int16_t>(sp + 0x34u) = 0;
    headingMatrix(car + 0x4cu, ee_itof(at<int16_t>(sp + 0x32u)));
    FPhysLibUpdateCarSpheres(ptr32<_sFPhysLibCar>(car), 1);
    at<ee_float>(pObject.address + 0xbcu) = fat(car + 8u) * kToCell;
    at<ee_float>(pObject.address + 0xc0u) = fat(car + 0xcu) * kToCell;
    at<ee_float>(pObject.address + 0xc4u) = fat(car + 0x10u) * kToCell;
    at<uint8_t>(car + 0x1cdu) = at<uint8_t>(car + 0x1cdu) | 1u;
    return 0;
}

// Adds a force (scaled by the constant at 0x3276d8) and, given a point of application, its torque. Flags: 2, the
// force is in the car's frame; 1, the point is.
void PhysicsApplyForce(ptr32<_sFPhysLibCar> pPhysics, ptr32<_sFVec> pForce, ptr32<_sFVec> pApplicationOffset, int16 flags)
{
    GuestFrame frame(0x20);
    const uint32_t sp = frame.local<uint8_t>(0).address;
    const uint32_t p = pPhysics.address, mat = p + 0x4cu;
    const uint32_t f = pForce.address;
    ee_float ix, iy, iz;
    if (flags & 2)
    {
        const ee_float x = fat(f), y = fat(f + 4u), z = fat(f + 8u);
        iz = matRow(mat, 2, x, y, z);
        ix = matRow(mat, 0, x, y, z);
        iy = matRow(mat, 1, x, y, z);
    }
    else
    {
        ix = fat(f);
        iy = fat(f + 4u);
        iz = fat(f + 8u);
    }
    const ee_float scale = fat(kForceScale);
    ix = ix * scale;
    iy = iy * scale;
    iz = iz * scale;
    at<ee_float>(sp + 0u) = ix;
    at<ee_float>(sp + 4u) = iy;
    at<ee_float>(sp + 8u) = iz;
    at<ee_float>(p + 0x94u) = fat(p + 0x94u) + ix;
    at<ee_float>(p + 0x98u) = fat(p + 0x98u) + iy;
    at<ee_float>(p + 0x9cu) = fat(p + 0x9cu) + iz;
    if (pApplicationOffset.address == 0)
        return;
    const uint32_t o = pApplicationOffset.address;
    ee_float ox, oy, oz;
    if (flags & 1)
    {
        const ee_float x = fat(o), y = fat(o + 4u), z = fat(o + 8u);
        oz = matRow(mat, 2, x, y, z);
        ox = matRow(mat, 0, x, y, z);
        oy = matRow(mat, 1, x, y, z);
    }
    else
    {
        ox = fat(o);
        oy = fat(o + 4u);
        oz = fat(o + 8u);
    }
    at<ee_float>(sp + 0x10u) = ox;
    at<ee_float>(sp + 0x14u) = oy;
    at<ee_float>(sp + 0x18u) = oz;
    const ee_float tx = (iy * oz - iz * oy) * kS;
    const ee_float tz = (ix * oy - iy * ox) * kS;
    const ee_float ty = (iz * ox - ix * oz) * kS;
    at<ee_float>(sp + 0u) = tx;
    at<ee_float>(sp + 8u) = tz;
    at<ee_float>(p + 0xa0u) = fat(p + 0xa0u) + tx;
    at<ee_float>(sp + 4u) = ty;
    at<ee_float>(p + 0xa4u) = fat(p + 0xa4u) + ty;
    at<ee_float>(p + 0xa8u) = fat(p + 0xa8u) + tz;
}

// The car's spheres: the four wheels (by the tags at 0x323bb8; a boat's from the nodes' radii), then a sphere for every
// body node, the one nearest the middle (less half its radius) swapped into slot 4.
void InitCarSpheres(ptr32<_sFPhysLibCar> pPhysics, ptr32<_sObjectDef> pObjDef)
{
    GuestFrame frame(0xc0);
    const uint32_t sp = frame.local<uint8_t>(0).address;
    const uint32_t p = pPhysics.address, def = pObjDef.address;
    if (at<uint32_t>(p + 0x1e0u) == 0)
        MemBlockAllocLo(ptr32<memHandle>(p + 0x1e0u), 4000);
    MemBlockSet32(memHandle(at<uint32_t>(p + 0x1e0u)), 0, 4000);
    uint32_t sphere = at<uint32_t>(p + 0x1e0u);
    for (int32_t i = 0; i < 4; ++i, sphere += 0x190u)
    {
        const uint32_t node = NodeFindByTag(at<ptr32<_sNodeHead>>(def + 0x28u), at<uint32_t>(kWheelTags + 4u * static_cast<uint32_t>(i))).address;
        if (at<int8_t>(p + 0x1ceu) == 2)
            at<ee_float>(sphere + 0x58u) = ee_itof(static_cast<int32_t>(at<int16_t>(node + 8u)) << 6);
        else
            at<ee_float>(sphere + 0x58u) = fat(at<uint32_t>(p + 0x1e4u) + (i < 2 ? 0x2cu : 0x68u));
        const ee_float z = ee_itof(at<int32_t>(node + 0x28u) << 6);
        const ee_float x = ee_itof(at<int32_t>(node + 0x20u) << 6);
        const ee_float y = ee_itof(at<int32_t>(node + 0x24u) << 6);
        at<ee_float>(sphere + 0x5cu) = fat(sphere + 0x58u);
        at<ee_float>(sphere + 0x4cu) = x;
        at<ee_float>(sphere + 0x50u) = y;
        at<ee_float>(sphere + 0x54u) = z;
    }
    uint32_t s = sphere + 0x4cu; // &sphere[4].sphereOffset
    uint32_t node = NodeFindByTag(at<ptr32<_sNodeHead>>(def + 0x28u), kBodySphereTag).address;
    ee_float closest = ee_float(16777216.0f);
    int32_t best = 4, index = 4;
    do
    {
        at<ee_float>(s + 0xcu) = ee_itof(static_cast<int32_t>(at<int16_t>(node + 8u)) << 6);
        const int16_t radius2 = at<int16_t>(node + 0xau);
        if (at<int8_t>(p + 0x1ceu) == 2 && radius2 != 0)
            at<ee_float>(s + 0x10u) = ee_itof(static_cast<int32_t>(radius2) << 6);
        else
            at<ee_float>(s + 0x10u) = fat(s + 0xcu);
        at<ee_float>(s + 0u) = ee_itof(at<int32_t>(node + 0x20u) << 6);
        at<ee_float>(s + 4u) = ee_itof(at<int32_t>(node + 0x24u) << 6);
        at<ee_float>(s + 8u) = ee_itof(at<int32_t>(node + 0x28u) << 6);
        if (at<int8_t>(p + 0x1ceu) == 2)
        {
            at<uint64_t>(sp + 0x10u) = at<uint64_t>(s);
            at<uint32_t>(sp + 0x18u) = at<uint32_t>(s + 8u);
        }
        else
        {
            const uint32_t model = at<uint32_t>(p + 0x1e4u) + 0xe0u;
            at<ee_float>(sp + 0x10u) = fat(model) + fat(s);
            at<ee_float>(sp + 0x14u) = fat(model + 4u) + fat(s + 4u);
            at<ee_float>(sp + 0x18u) = fat(model + 8u) + fat(s + 8u);
        }
        const ee_float vx = fat(sp + 0x10u), vy = fat(sp + 0x14u), vz = fat(sp + 0x18u);
        const ee_float dist = ee_sqrt(vx * vx + vy * vy + vz * vz) - fat(s + 0xcu) * ee_float(0.5f);
        if (dist < closest)
        {
            closest = dist;
            best = index;
        }
        node = NodeFindNextByTag(ptr32<_sNode>(node), at<ptr32<_sNodeHead>>(def + 0x28u), kBodySphereTag).address;
        s += 0x190u;
        ++index;
    } while (node != 0);
    at<int8_t>(p + 0x1ccu) = static_cast<int8_t>(index);
    // Swap sphere 4's offset and radius with the chosen one's (the old offset kept at sp).
    const uint32_t spheres = at<uint32_t>(p + 0x1e0u);
    const uint32_t chosen = static_cast<uint32_t>(best) * 0x190u;
    at<uint64_t>(sp) = at<uint64_t>(spheres + 0x68cu);
    at<uint32_t>(sp + 8u) = at<uint32_t>(spheres + 0x694u);
    const uint32_t oldRadius = at<uint32_t>(spheres + 0x698u);
    at<uint64_t>(spheres + 0x68cu) = at<uint64_t>(spheres + chosen + 0x4cu);
    at<uint32_t>(spheres + 0x694u) = at<uint32_t>(spheres + chosen + 0x54u);
    const uint32_t spheres2 = at<uint32_t>(p + 0x1e0u);
    at<uint32_t>(spheres2 + 0x698u) = at<uint32_t>(spheres2 + chosen + 0x58u);
    at<uint64_t>(spheres2 + chosen + 0x4cu) = at<uint64_t>(sp);
    at<uint32_t>(spheres2 + chosen + 0x54u) = at<uint32_t>(sp + 8u);
    at<uint32_t>(at<uint32_t>(p + 0x1e0u) + chosen + 0x58u) = oldRadius;
}

// The car's float data from its definition's integer data; the springs scaled by the constant at 0x3276dc, and the
// boat's thrust by the one at 0x3276e0 on the levels whose number is 21 in 24.
void LoadCarDataResource(ptr32<_sFPhysLibCar> pPhysics, ptr32<_sObjectDef> pObjDef)
{
    const uint32_t p = pPhysics.address;
    at<uint32_t>(p + 0x1e4u) = 0;
    MemBlockAllocLo(ptr32<memHandle>(p + 0x1e4u), 0x108);
    const uint32_t src = at<uint32_t>(pObjDef.address + 0x2cu) + 4u; // &pPhysicsDef->carData
    const uint32_t dst = at<uint32_t>(p + 0x1e4u);
    const ee_float spring = fat(kSpringScale);
    const auto i2f = [&](uint32_t to, uint32_t from) { at<ee_float>(dst + to) = ee_itof(at<int32_t>(src + from)); };
    const auto s2f = [&](uint32_t to, uint32_t from) { at<ee_float>(dst + to) = ee_itof(at<int16_t>(src + from)); };
    for (uint32_t k = 0; k < 3; ++k)
        s2f(0x0u + 4u * k, 0x0u + 2u * k); // comFromWheelMidPoint
    for (uint32_t k = 0; k < 7; ++k)
        i2f(0xcu + 4u * k, 0x8u + 4u * k); // mass .. colFricConstant
    at<int32_t>(dst + 0x28u) = at<int32_t>(src + 0x24u); // maxSteerAngle
    for (uint32_t w = 0; w < 2; ++w)
        for (uint32_t k = 0; k < 15; ++k)
            i2f(0x2cu + 0x3cu * w + 4u * k, 0x28u + 0x3cu * w + 4u * k);
    at<ee_float>(dst + 0x3cu) = ee_itof(at<int32_t>(src + 0x38u)) * spring; // frontWheels.springConstant
    at<ee_float>(dst + 0x78u) = ee_itof(at<int32_t>(src + 0x74u)) * spring; // rearWheels.springConstant
    at<int32_t>(dst + 0xa4u) = at<int32_t>(src + 0xa0u);                    // maxSteerAngleBoat
    const ee_float thrust = ee_itof(at<int32_t>(src + 0xa4u));
    at<ee_float>(dst + 0xa8u) = thrust;
    for (uint32_t k = 0; k < 4; ++k)
        i2f(0xacu + 4u * k, 0xa8u + 4u * k); // waterDragCoef .. rudderConstant
    for (uint32_t k = 0; k < 3; ++k)
    {
        s2f(0xbcu + 4u * k, 0xb8u + 2u * k); // motorOffset
        s2f(0xc8u + 4u * k, 0xc0u + 2u * k); // comBoatOffset
        i2f(0xd4u + 4u * k, 0xc8u + 4u * k); // carExtents
        s2f(0xe0u + 4u * k, 0xd4u + 2u * k); // modelOffset
        s2f(0xecu + 4u * k, 0xdcu + 2u * k); // modelOffsetBoat
    }
    i2f(0xf8u, 0xe4u); // invMass
    i2f(0xfcu, 0xe8u); // invInertiaX
    at<int32_t>(dst + 0x100u) = at<int32_t>(src + 0xecu); // notused
    at<int32_t>(dst + 0x104u) = 0;                        // flags
    if ((static_cast<int32_t>(at<uint8_t>(kMainVars + 0x34au)) - 1) % 24 + 1 == 21) // mainVarG.level
        at<ee_float>(dst + 0xa8u) = thrust * fat(kBoatThrustScale);
}

int32_t PhysicsInit()
{
    MemBlockSet32(memHandle(kPhysicsGlobal), 0, 4);
    FPhysLibInit();
    at<uint8_t>(kPhysicsGlobal) = 1;
    return 0;
}

int32_t PhysicsTerm()
{
    FPhysLibTerm();
    at<uint8_t>(kPhysicsGlobal) = 0;
    return 0;
}

int32_t PhysicsLevelInit()
{
    at<uint8_t>(kPhysicsGlobal + 1u) = 1;
    return 0;
}

int32_t PhysicsLevelTerm()
{
    at<uint8_t>(kPhysicsGlobal + 1u) = 0;
    return 0;
}

int32_t PhysicsDeleteCar(ptr32<_sObject> pObject)
{
    const uint32_t p = at<uint32_t>(pObject.address + 0x20u);
    FPhysLibDeleteCar(ptr32<_sFPhysLibCar>(p));
    at<uint8_t>(kPhysicsGlobal + 3u) = static_cast<uint8_t>(at<uint8_t>(kPhysicsGlobal + 3u) - 1u);
    MemBlockFree(ptr32<memHandle>(p + 0x1e0u));
    MemBlockFree(ptr32<memHandle>(p + 0x1e4u));
    MemBlockSet32(memHandle(p), 0xffffffffu, 0x23c);
    at<uint32_t>(p + 0x1e4u) = 0;
    at<uint32_t>(p + 0x1e0u) = 0;
    return 0;
}

int32_t PhysicsMotion()
{
    FPhysLibSimulation();
    return 0;
}

int32_t PhysicsCarBoatSwap(ptr32<_sFPhysLibCar> pPhysics, ptr32<_sObject> pObject)
{
    const uint32_t p = pPhysics.address;
    at<int8_t>(p + 0x1ceu) = at<int8_t>(p + 0x1ceu) == 1 ? 2 : 1;
    InitCarSpheres(pPhysics, at<ptr32<_sObjectDef>>(pObject.address + 0xcu));
    FPhysLibSphereInitFixup(pPhysics);
    at<uint8_t>(at<uint32_t>(pObject.address + 0x24u) + 2u) = at<uint8_t>(p + 0x1ccu);
    at<uint8_t>(p + 0x1cdu) = at<uint8_t>(p + 0x1cdu) | 1u;
    return at<int8_t>(p + 0x1ceu);
}

int32_t PhysicsResetCar(ptr32<_sObject> pObject)
{
    if (at<int8_t>(pObject.address + 0x18au) == 0)
        VfxAddNew(pObject, 0, 0, 0x24);
    return 0;
}

// PhysicsApplyForce with whole-number vectors (either may be null).
void PhysicsApplyIntForce(ptr32<_sFPhysLibCar> pPhysics, ptr32<_sVec16> pForce, ptr32<_sVec16> pApplicationOffset, int16 flags)
{
    GuestFrame frame(0x30);
    const uint32_t sp = frame.local<uint8_t>(0).address;
    if (pForce.address != 0)
        for (uint32_t k = 0; k < 3; ++k)
            at<ee_float>(sp + 4u * k) = ee_itof(at<int16_t>(pForce.address + 2u * k));
    if (pApplicationOffset.address != 0)
        for (uint32_t k = 0; k < 3; ++k)
            at<ee_float>(sp + 0x10u + 4u * k) = ee_itof(at<int16_t>(pApplicationOffset.address + 2u * k));
    PhysicsApplyForce(pPhysics, ptr32<_sFVec>(pForce.address ? sp : 0u),
                      ptr32<_sFVec>(pApplicationOffset.address ? sp + 0x10u : 0u), flags);
}

// Copies the float state into the integer state the rest of the game reads (or clears the pointer to it).
void PhysicsSetOldIntegerState(ptr32<_sFPhysLibCar> pPhysics, int32_t bSet)
{
    const uint32_t p = pPhysics.address;
    if (bSet == 0)
    {
        at<uint32_t>(p + 0x1e8u) = 0;
        return;
    }
    const uint32_t s = p + 0x1ecu;
    at<int16_t>(s + 0u) = at<int16_t>(p + 0x1d0u); // tickSet
    for (uint32_t k = 0; k < 3; ++k)
    {
        at<int32_t>(s + 4u + 4u * k) = ee_ftoi(fat(p + 8u + 4u * k));      // renderPos
        at<int32_t>(s + 0x10u + 4u * k) = ee_ftoi(fat(p + 0x14u + 4u * k)); // pos
        at<int16_t>(s + 0x1cu + 2u * k) = static_cast<int16_t>(ee_ftoi(fat(p + 0x20u + 4u * k))); // vel
    }
    at<int32_t>(s + 0x24u) = ee_ftoi(fat(p + 0x38u)); // speed
    at<uint32_t>(p + 0x1e8u) = s;
    for (uint32_t k = 0; k < 9; ++k)
    {
        at<int16_t>(s + 0x28u + 2u * k) = static_cast<int16_t>(ee_ftoi(fat(p + 0x4cu + 4u * k)));
        at<int16_t>(s + 0x3cu + 2u * k) = static_cast<int16_t>(ee_ftoi(fat(p + 0x70u + 4u * k)));
    }
}

RC_DECOMP_FN_SETUP_HEAP(PhysicsAddCar, rc::fuzz::physicsAddCar)
RC_DECOMP_FN_SETUP_HEAP(PhysicsCollisionResponseUpdate, rc::fuzz::physicsRace)
RC_DECOMP_FN_SETUP_HEAP(PhysicsRightCar, rc::fuzz::physicsObject)
RC_DECOMP_FN_SETUP(PhysicsApplyForce, rc::fuzz::physicsForce)
RC_DECOMP_FN_SETUP_HEAP(InitCarSpheres, rc::fuzz::physicsCarDef)
RC_DECOMP_FN_SETUP_HEAP(LoadCarDataResource, rc::fuzz::physicsCarDef)
RC_DECOMP_FN_SETUP_HEAP(PhysicsInit, rc::fuzz::physicsRace)
RC_DECOMP_FN_SETUP_HEAP(PhysicsTerm, rc::fuzz::physicsRace)
RC_DECOMP_FN(PhysicsLevelInit)
RC_DECOMP_FN(PhysicsLevelTerm)
RC_DECOMP_FN_SETUP_HEAP(PhysicsDeleteCar, rc::fuzz::physicsObject)
RC_DECOMP_FN_SETUP_HEAP(PhysicsMotion, rc::fuzz::physicsRace)
RC_DECOMP_FN_SETUP_HEAP(PhysicsCarBoatSwap, rc::fuzz::physicsBoatSwap)
RC_DECOMP_FN_SETUP_HEAP(PhysicsResetCar, rc::fuzz::physicsObject)
RC_DECOMP_FN_SETUP(PhysicsApplyIntForce, rc::fuzz::physicsIntForce)
RC_DECOMP_FN_SETUP(PhysicsSetOldIntegerState, rc::fuzz::physicsCarFlag)
RC_DECOMP_FN_SETUP(PhysicsCollisionHelpOut, rc::fuzz::physicsCarFlag)

} // namespace rc
