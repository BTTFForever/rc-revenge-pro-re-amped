// Decompiled from VISFX.c and VISFX2.c, continued (eighth batch, handlers): the quake shock waves, the oil dump, the
// skid marks, the water and car splashes, the boat's wake, the aerial's chain, the spin pickup, the respawn sparks, the
// spray and the give-terror pickup. Each completes a handler/renderer pair whose renderer is already native (the
// give-terror renderer is in visfx11.cpp). See visfx5.cpp for the conventions.
//
// The ring loops of these handlers read the ring's head and tail with lbu (unsigned indexes), as in visfx8.cpp.
// Functions that call guest code or pass locals by address keep the original's frame (GuestFrame); their
// sceVu0ITOF0Vector then takes the vector's stale fourth lane from that frame (sortPosFromInts with a frame size of 0).

#include "ai_fuzz.h"
#include "ee_math.h"
#include "vifpk.h"
#include "visfx.h"

namespace rc {

void VfxSortPos(ptr32<SVECTOR> sv, ptr32<_sVfx_Object> fx);       // visfx.cpp
void VfxCarSortPos(ptr32<_sObject> pObj, ptr32<_sVfx_Object> fx);  // visfx.cpp
int32 VfxIsWheelTouching(ptr32<_sObject> pObj, int32 iWheel);     // visfx.cpp
ptr32<MATRIX> VfxCarMatrixPoi(ptr32<_sObject> pObj);              // ai.cpp
uint32 VecNormalise(ptr32<SVECTOR> pOut, ptr32<SVECTOR> pVec);    // ai.cpp
uint16 RandomGetNDRange(uint16 lower, uint16 upper);              // math_core.cpp
void PS2C_GteMatrixMulVec16(ptr32<_sVec16> pVec);                 // psxdefs.cpp
void PS2C_SetGteMatrix(ptr32<_sMat> pMat);
void PS2C_GetVec16GteResult(ptr32<_sVec16> pOut);
bool_ AColDownCast(ptr32<_sColCellVecPSX> pPos, ptr32<_sColCellVecPSX> pPoi, ptr32<_sColNormalPSX> pNormal, ptr32<uint8> pMatId); // collision.cpp
bool_ AColDownCastInt(ptr32<SVECTOR> pPosI, ptr32<SVECTOR> pPoiI, ptr32<SVECTOR> pNormalI, ptr32<uint8> pMatId);

namespace {

using vfx::at;

constexpr uint32_t kRing = 0x40;          // the ring header: mask, count, head, tail
constexpr uint32_t kMainVar = 0x1ed900;   // mainVarG (its first word is the status)
constexpr uint32_t kPlayerStates = 0x1f37e0; // u8PlayerStates: per player a word, sNew (+0) and sOld (+2)
constexpr uint32_t kMaterials = 0x1edce8; // mtlG.mtl: 0x18 bytes each, the wheel-mark bits at +4
constexpr uint32_t kObjects = 0x1ee390;   // objectVarG: the object list, the count at +0x170

constexpr uint32_t kVfxAddNew = 0x157c50, kRevSfxPlay3D = 0x1418a8, kObjectWeaponAffectRemove = 0x137800, kNodeCalcWorldPos = 0x1339d0;
constexpr uint32_t kAtan2f = 0x1c87d0, kUnitMatrix = 0x1c7ac8, kRotMatrixX = 0x1c7c10, kRotMatrixZ = 0x1c7b68, kMulMatrix = 0x1c77d8;

inline uint8_t &ringMask(uint32_t m) { return at<uint8_t>(m + kRing); }
inline uint8_t &ringCount(uint32_t m) { return at<uint8_t>(m + kRing + 1); }
inline uint8_t &ringHeadU(uint32_t m) { return at<uint8_t>(m + kRing + 2); }
inline uint8_t &ringTail(uint32_t m) { return at<uint8_t>(m + kRing + 3); }

inline void pushTail(uint32_t m)
{
    ringTail(m) = static_cast<uint8_t>(ringMask(m) & (ringTail(m) + 1));
    ringCount(m) = static_cast<uint8_t>(ringCount(m) + 1);
}

// Removes a dead particle from the ring: the one at the head takes its place (unless it is the head) and the head moves on.
inline void dropParticle(uint32_t m, uint32_t base, uint32_t index, uint32_t stride)
{
    const uint32_t head = ringHeadU(m);
    if (index != head)
        std::memcpy(&at<uint8_t>(base + index * stride), &at<uint8_t>(base + head * stride), stride);
    ringCount(m) = static_cast<uint8_t>(ringCount(m) - 1);
    ringHeadU(m) = static_cast<uint8_t>(ringMask(m) & (ringHeadU(m) + 1));
}

inline void add16(uint32_t address, int32_t value) { at<uint16_t>(address) = static_cast<uint16_t>(at<uint16_t>(address) + value); }
inline int32_t sh16(uint32_t address) { return at<int16_t>(address); }
inline int32_t rnd(int32_t mask) { return vfx::random15() & mask; }

// sceVu0CopyVectorXYZ: three words copied as they are.
inline void copyXYZ(uint32_t dst, uint32_t src)
{
    for (uint32_t lane = 0; lane < 12; lane += 4)
        at<uint32_t>(dst + lane) = at<uint32_t>(src + lane);
}

// The car's integer state (physics +0x1e8): the position in 24.8 at +4, the velocity shorts at +0x1c, the rotation
// matrix shorts from +0x28.
inline uint32_t carState(uint32_t car) { return at<uint32_t>(at<uint32_t>(car + 0x20) + 0x1e8); }

// The car's position in whole units as the sort position, inside the caller's GuestFrame (the vector at localOffset).
inline void sortAtCarInFrame(uint32_t m, uint32_t car, uint32_t localOffset)
{
    const uint32_t state = carState(car);
    vfx::sortPosFromInts(m, at<int32_t>(state + 4) >> 8, at<int32_t>(state + 8) >> 8, at<int32_t>(state + 0xc) >> 8, 0, localOffset);
}

// The car's position in whole units in the shared vector svVelTemp (x, y, z; its pad is left alone).
inline void carPositionToVelTemp(uint32_t state)
{
    SVECTOR &v = G(svVelTemp);
    v.vx = static_cast<int16>(at<int32_t>(state + 4) >> 8);
    v.vy = static_cast<int16>(at<int32_t>(state + 8) >> 8);
    v.vz = static_cast<int16>(at<int32_t>(state + 0xc) >> 8);
}

// The car's draw distance as the effects test it (as VfxHasCarBeenRendered, with the field at +0x170): in a race that
// is over or a demo race, a car that is drawn counts as 100 units away.
uint16_t carDistance(uint32_t car, uint32_t field)
{
    if ((at<uint32_t>(kMainVar) & 0x30) == 0)
        return at<uint16_t>(car + field);
    return at<int16_t>(car + field + 2) != 0 ? at<uint16_t>(car + field) : 100;
}

inline uint16_t &playerState(uint32_t car, uint32_t which) { return at<uint16_t>(kPlayerStates + at<uint8_t>(car + 0x45) * 4u + which); }

} // namespace

// --- the quake shock waves -------------------------------------------------------------------------------------------

namespace {
// The ring of a quake's shock wave: 31 particles of 0x30 bytes from +0x50 (a float position, a velocity at +0x10, a life
// word at +0x20) round a circle of radius 100 in the ground's plane (`mat`), each moving outwards (its velocity is its
// position), the angles `step` apart.
void quakeWave(uint32_t m, uint32_t mat, ee_float step, int32_t life)
{
    const ee_float radius(100.0f);
    for (int32_t i = 0; i < 31; ++i)
    {
        const uint32_t p = m + 0x50 + static_cast<uint32_t>(i) * 0x30;
        const ee_float angle = i == 0 ? ee_float(0.0f) : ee_itof(i) * step;
        at<float>(p) = ee_sinf(angle) * radius;
        at<float>(p + 4) = 1.0f;
        at<float>(p + 8) = ee_cosf(angle) * radius;
        sceVu0ApplyMatrix(ptr32<ee_float>(p), ptr32<ee_float>(mat), ptr32<ee_float>(p));
        copyXYZ(p + 0x10, p);
        at<int32_t>(p + 0x20) = life;
    }
    ringCount(m) = 0x1f;
    ringTail(m) = 0x1f;
}

// Moves the wave's particles by their velocity times `scale` (Quake2) or by the velocity itself (Quake: scale 0), and
// drops those whose life ends.
void quakeMove(uint32_t m, bool quarter)
{
    for (uint32_t index = ringHeadU(m); index != ringTail(m); index = (index + 1) & ringMask(m))
    {
        const uint32_t p = m + 0x50 + index * 0x30;
        const int32_t life = at<int32_t>(p + 0x20);
        if (life == 0)
            continue;
        at<int32_t>(p + 0x20) = life - 1;
        for (uint32_t lane = 0; lane < 12; lane += 4)
        {
            ee_float v(at<float>(p + 0x10 + lane));
            if (quarter)
                v = v * ee_float(0.25f);
            at<float>(p + lane) = ee_float(at<float>(p + lane)) + v;
        }
        if (life - 1 == 0)
            dropParticle(m, m + 0x50, index, 0x30);
    }
}

inline int16_t truncToShort(ee_float value) { return static_cast<int16_t>(ee_ftoi(value)); }
} // namespace

// The quake weapon's shock wave (the object at +0x650). On the first frame (life 32) a cast down from just above the
// object (its position in svVelTemp) finds the ground: the hit point (shorts at +0x654, 8 higher) is the wave's centre,
// the ground's normal (written into the object at +0x1dc, then scaled by 4096 there) tilts the ring, and the object's
// velocity over 200 (shorts at +0x65c) moves the centre. Later frames move the centre and the particles. The two
// ArcTan calls of the original only fill locals nobody reads, and are left out.
void VfxHandler_Quake(ptr32<_sVfx_Object> mem)
{
    GuestFrame frame(0x180);
    const uint32_t m = mem.address;
    const uint32_t obj = at<uint32_t>(m + 0x650);
    if (at<uint16_t>(m + 0x10) == 0x20)
    {
        const uint32_t mat = frame.local<float>(0x30).address;
        guest_call(kUnitMatrix, ptr32<float>(mat));
        carPositionToVelTemp(carState(obj));
        const SVECTOR &v = G(svVelTemp);
        const uint32_t from = frame.local<float>(0x70).address, hit = frame.local<float>(0x80).address;
        at<float>(from) = ee_itof(v.vx);
        at<float>(from + 4) = ee_itof(static_cast<int16_t>(v.vy + 0x50));
        at<float>(from + 8) = ee_itof(v.vz);
        if (!AColDownCast(ptr32<_sColCellVecPSX>(from), ptr32<_sColCellVecPSX>(hit), ptr32<_sColNormalPSX>(obj + 0x1dc), frame.local<uint8>(0xa0)))
        {
            at<uint16_t>(m + 0x10) = 0x1f;
            return;
        }
        const ee_float scale(4096.0f);
        const ee_float ny = ee_float(at<float>(obj + 0x1e0)) * scale, nz = ee_float(at<float>(obj + 0x1e4)) * scale,
                       nx = ee_float(at<float>(obj + 0x1dc)) * scale;
        at<int16_t>(m + 0x654) = truncToShort(at<float>(hit));
        at<int16_t>(m + 0x656) = truncToShort(at<float>(hit + 4));
        at<int16_t>(m + 0x658) = truncToShort(at<float>(hit + 8));
        at<float>(obj + 0x1e0) = ny;
        at<float>(obj + 0x1dc) = nx;
        at<float>(obj + 0x1e4) = nz;
        const ee_float step(at<float>(0x327830u)); // 2 pi / 32
        guest_call(kRotMatrixX, ptr32<float>(mat), ptr32<float>(mat), guest_call<ee_float>(kAtan2f, ee_float(at<float>(obj + 0x1e4)), ee_float(at<float>(obj + 0x1e0))));
        const ee_float tilt = guest_call<ee_float>(kAtan2f, ee_float(at<float>(obj + 0x1dc)), ee_float(at<float>(obj + 0x1e0)));
        guest_call(kRotMatrixZ, ptr32<float>(mat), ptr32<float>(mat), -tilt);
        add16(m + 0x656, 8);
        std::memcpy(&at<uint8_t>(m + 0x65c), &at<uint8_t>(carState(obj) + 0x1c), 8);
        for (uint32_t axis = 0; axis < 3; ++axis)
            at<int16_t>(m + 0x65c + 2 * axis) = static_cast<int16_t>(at<int16_t>(m + 0x65c + 2 * axis) / 200);
        quakeWave(m, mat, step, 0x28);
    }
    else
    {
        const uint16_t x = static_cast<uint16_t>(at<uint16_t>(m + 0x654) + at<uint16_t>(m + 0x65c));
        add16(m + 0x656, at<uint16_t>(m + 0x65e));
        add16(m + 0x658, at<uint16_t>(m + 0x660));
        at<uint16_t>(m + 0x654) = x;
        vfx::sortPosFromInts(m, static_cast<int16_t>(x), sh16(m + 0x656), sh16(m + 0x658), 0, 0xb0);
        quakeMove(m, false);
    }
    const uint16_t life = at<uint16_t>(m + 0x10);
    if (life > 1)
        at<uint16_t>(m + 0x10) = static_cast<uint16_t>(life - 1);
}

// The second quake: as the first, but cast down from its own centre (shorts at +0x654), the ground's normal kept in a
// local, the centre not moving after the start, the particles moving a quarter of their velocity and living 20 frames.
// It sorts at its centre every frame.
void VfxHandler_Quake2(ptr32<_sVfx_Object> mem)
{
    GuestFrame frame(0x190);
    const uint32_t m = mem.address;
    if (at<uint16_t>(m + 0x10) == 0x20)
    {
        const uint32_t mat = frame.local<float>(0x30).address;
        guest_call(kUnitMatrix, ptr32<float>(mat));
        const uint32_t from = frame.local<float>(0x70).address, hit = frame.local<float>(0x80).address, normal = frame.local<float>(0x90).address;
        at<float>(from) = ee_itof(sh16(m + 0x654));
        at<float>(from + 4) = ee_itof(static_cast<int16_t>(at<uint16_t>(m + 0x656) + 0x50));
        at<float>(from + 8) = ee_itof(sh16(m + 0x658));
        if (AColDownCast(ptr32<_sColCellVecPSX>(from), ptr32<_sColCellVecPSX>(hit), ptr32<_sColNormalPSX>(normal), frame.local<uint8>(0xa0)))
        {
            const ee_float scale(4096.0f);
            const ee_float nx = ee_float(at<float>(normal)) * scale, ny = ee_float(at<float>(normal + 4)) * scale,
                           nz = ee_float(at<float>(normal + 8)) * scale;
            at<float>(normal) = nx;
            at<int16_t>(m + 0x654) = truncToShort(at<float>(hit));
            at<int16_t>(m + 0x656) = truncToShort(at<float>(hit + 4));
            at<int16_t>(m + 0x658) = truncToShort(at<float>(hit + 8));
            at<float>(normal + 4) = ny;
            at<float>(normal + 8) = nz;
            guest_call(kRotMatrixX, ptr32<float>(mat), ptr32<float>(mat), guest_call<ee_float>(kAtan2f, nz, ny));
            const ee_float step(at<float>(0x327878u)); // 2 pi / 32
            const ee_float tilt = guest_call<ee_float>(kAtan2f, ee_float(at<float>(normal)), ee_float(at<float>(normal + 4)));
            guest_call(kRotMatrixZ, ptr32<float>(mat), ptr32<float>(mat), -tilt);
            at<uint16_t>(m + 0x65c) = 0;
            at<uint16_t>(m + 0x65e) = 0;
            at<uint16_t>(m + 0x660) = 0;
            add16(m + 0x656, 8);
            quakeWave(m, mat, step, 0x14);
        }
    }
    else
    {
        add16(m + 0x656, at<uint16_t>(m + 0x65e));
        add16(m + 0x654, at<uint16_t>(m + 0x65c));
        add16(m + 0x658, at<uint16_t>(m + 0x660));
        quakeMove(m, true);
    }
    vfx::sortPosFromInts(m, sh16(m + 0x654), sh16(m + 0x656), sh16(m + 0x658), 0, 0xb0);
    const uint16_t life = at<uint16_t>(m + 0x10);
    if (life > 1)
        at<uint16_t>(m + 0x10) = static_cast<uint16_t>(life - 1);
}

// --- the oil dump ----------------------------------------------------------------------------------------------------

// The oil weapon (the car at +0x1c4): from life 185 up (one frame at 200 drops twelve) a blob of 0x18 bytes leaves the
// car's back, along the car's forward axis (the matrix's third column) with a random scatter; its flags at +0x16/+0x17
// say a big blob or a drop. While no blob has landed (+0x1d0), a cast down below the oldest blob finds the ground (the
// hit point to +0x1c8, or +0x1d4 = 2 for none). Blobs fall (faster when big); a blob below the ground stops, spreads
// (big ones double their speed), and the first one to land sets +0x1d0 and +0x1d4. Then the particle mover.
void VfxHandler_OilDump(ptr32<_sVfx_Object> mem)
{
    GuestFrame frame(0x110);
    const uint32_t m = mem.address;
    const uint32_t car = at<uint32_t>(m + 0x1c4);
    const uint32_t base = m + 0x44;
    const uint16_t startLife = at<uint16_t>(m + 0x10);
    if (!(startLife < 0xb9))
    {
        const uint32_t matrix = carState(car) + 0x28;
        const int32_t drops = startLife == 200 ? 12 : 1;
        for (int32_t i = 0; i < drops; ++i)
        {
            if (!(ringCount(m) < ringMask(m)))
                continue;
            const uint32_t p = base + ringTail(m) * 0x18u;
            at<uint16_t>(p) = static_cast<uint16_t>(-sh16(matrix + 4) >> 2);
            at<uint16_t>(p + 2) = static_cast<uint16_t>(-(sh16(matrix + 0xa) >> 2) + 0x15e);
            at<uint16_t>(p + 4) = static_cast<uint16_t>(-sh16(matrix + 0x10) >> 2);
            at<uint16_t>(p + 0x10) = static_cast<uint16_t>(vfx::random15());
            at<uint16_t>(p + 0x12) = static_cast<uint16_t>(rnd(0x1ff) - 0x100);
            at<uint16_t>(p + 8) = static_cast<uint16_t>(rnd(0xff) - ((sh16(matrix + 4) >> 4) + 0x7f));
            at<uint16_t>(p + 0xa) = static_cast<uint16_t>(rnd(0x40) - ((sh16(matrix + 0xa) >> 4) - 0x15e));
            at<uint16_t>(p + 0xc) = static_cast<uint16_t>(rnd(0xff) - ((sh16(matrix + 0x10) >> 4) + 0x7f));
            const bool big = drops == 12;
            at<uint16_t>(p + 0x14) = big ? 0x400 : 8;
            at<uint8_t>(p + 0x16) = big ? 0xf : 1;
            at<uint8_t>(p + 0x17) = big ? 4 : 1;
            pushTail(m);
        }
    }
    at<uint16_t>(m + 0x10) = static_cast<uint16_t>(at<uint16_t>(m + 0x10) - 1);
    const uint32_t state = carState(car);
    carPositionToVelTemp(state);
    const int32_t carX = at<int32_t>(state + 4) >> 8;
    if (at<uint32_t>(m + 0x1d0) == 0 && ringHeadU(m) != ringTail(m))
    {
        const uint32_t p = base + ringHeadU(m) * 0x18u;
        const uint32_t from = frame.local<SVECTOR>(0).address, hit = frame.local<SVECTOR>(0x20).address;
        at<uint16_t>(from) = static_cast<uint16_t>(carX + (sh16(p) >> 4));
        at<uint16_t>(from + 2) = static_cast<uint16_t>(static_cast<uint16_t>(G(svVelTemp).vy) + (sh16(p + 2) >> 4) + 0xc8);
        at<uint16_t>(from + 4) = static_cast<uint16_t>(static_cast<uint16_t>(G(svVelTemp).vz) + (sh16(p + 4) >> 4));
        if (AColDownCastInt(ptr32<SVECTOR>(from), ptr32<SVECTOR>(hit), frame.local<SVECTOR>(0x10), frame.local<uint8>(0x40)))
            std::memcpy(&at<uint8_t>(m + 0x1c8), &at<uint8_t>(hit), 8);
        else
            at<uint32_t>(m + 0x1d4) = 2;
    }
    if (at<uint16_t>(m + 0x10) != 0)
    {
        for (uint32_t index = ringHeadU(m); index != ringTail(m); index = (index + 1) & ringMask(m))
        {
            const uint32_t p = base + index * 0x18;
            if (!(at<uint16_t>(p + 0x14) < 4))
                add16(p + 0xa, at<uint8_t>(p + 0x16) < 0x10 ? -0x1e : -0x5a);
            if (at<uint16_t>(p + 0x14) < 4)
                continue;
            if (!(G(svVelTemp).vy + (sh16(p + 2) >> 4) < sh16(m + 0x1ca)))
                continue;
            if (!(at<uint8_t>(p + 0x16) < 0x10))
            {
                at<uint16_t>(p + 8) = static_cast<uint16_t>(at<uint16_t>(p + 8) << 1);
                at<uint16_t>(p + 0xc) = static_cast<uint16_t>(at<uint16_t>(p + 0xc) << 1);
            }
            at<uint16_t>(p + 0x14) = 3;
            at<uint8_t>(p + 0x17) = 8;
            at<uint16_t>(p + 0xa) = 0xa0;
            if (at<uint32_t>(m + 0x1d0) == 0 && at<uint32_t>(m + 0x1d4) != 2)
            {
                at<uint32_t>(m + 0x1d0) = 1;
                at<uint32_t>(m + 0x1d4) = 1;
            }
        }
        VfxParticleMover(ptr32<_sVfx_PartHeader>(m + kRing), ptr32<_VFX_PARTICLE>(base));
    }
    sortAtCarInFrame(m, car, 0x50);
}

// --- skid marks -------------------------------------------------------------------------------------------------------

namespace {
// Adds an effect (VfxAddNew) for the car: a1 the wheel, a2 the material, a3 the type.
inline void addEffect(uint32_t car, int32_t a1, int32_t a2, int32_t type) { guest_call(kVfxAddNew, ptr32<_sObject>(car), a1, a2, type); }

// Whether a wheel is on the ground (cars: the wheel's flag word at +0xe8 of its 0x48-byte record) or in the water
// (boats, types 0x14 up: the depth float at +0x78 of the boat's 0x190-byte record above zero).
bool wheelDown(uint32_t car, uint32_t physics, uint32_t wheel, uint16_t flags)
{
    if (at<uint32_t>(at<uint32_t>(car + 0xc) + 4) < 0x14)
        return (flags & 1) != 0;
    return ee_float(0.0f) < ee_float(at<float>(at<uint32_t>(physics + 0x1e0) + wheel * 0x190 + 0x78));
}
} // namespace

// The skid marks of one wheel (the car at +0x184, the wheel at +0x188): the material under the wheel marks the player's
// state (sNew) every frame. Every other frame, for cars (not boats, not type 15) near enough to be drawn: a splash
// effect on deep water, and while the wheel skids (flag 2 of its record) or is forced to (+0x18b: 1, 2 or 4 pick a
// material), a mark of 0x14 bytes (the wheel's position plus and minus the car's sideways axis, the material, a joined
// flag from +0x189), now and then dust and a puff (VfxAddNew type 8), and a new chain (+0x189) starts with a puff. Now
// and then a puff for a fast car (speed above 5000) when the state's bit 8 is set. Marks age (+0x10) and the oldest
// leaves once dead.
void VfxHandler_CarSkidMark(ptr32<_sVfx_Object> mem)
{
    GuestFrame frame(0xd0);
    const uint32_t m = mem.address;
    const uint32_t wheel = at<uint8_t>(m + 0x188);
    const uint32_t car = at<uint32_t>(m + 0x184);
    const uint32_t base = m + 0x44;
    const uint32_t surface = at<uint8_t>(at<uint32_t>(car + 0x20) + wheel * 0x48 + 0xea);
    uint32_t material = surface < 0x48 ? surface : 0x1d;
    playerState(car, 0) |= at<uint16_t>(kMaterials + material * 0x18 + 4);
    const uint8_t counter = at<uint8_t>(m + 0x18a);
    if (at<uint32_t>(at<uint32_t>(car + 0xc) + 4) < 0x14)
        at<uint8_t>(m + 0x18a) = static_cast<uint8_t>(counter + 1);
    if (at<uint32_t>(at<uint32_t>(car + 0xc) + 4) < 0x14 && (counter & 1) == 0 && at<uint32_t>(at<uint32_t>(car + 0xc) + 4) != 0xf)
    {
        if (carDistance(car, 0x170) < 0x1d4c)
        {
            uint32_t physics = at<uint32_t>(car + 0x20);
            const bool down = wheelDown(car, physics, wheel, at<uint16_t>(physics + wheel * 0x48 + 0xe8));
            if (down && material == 0x23 && (playerState(car, 2) & 0x20) == 0)
                addEffect(at<uint32_t>(m + 0x184), 0, 0, 0x22);
            physics = at<uint32_t>(car + 0x20);
            const uint16_t flags = at<uint16_t>(physics + wheel * 0x48 + 0xe8);
            if ((flags & 2) == 0 && at<uint8_t>(m + 0x18b) == 0)
                at<uint8_t>(m + 0x189) = 0;
            else if (!wheelDown(car, physics, wheel, flags) || !(ringCount(m) < 0xf))
                at<uint8_t>(m + 0x189) = 0;
            else
            {
                switch (at<uint8_t>(m + 0x18b))
                {
                case 1: material = 0xe; break;
                case 2: material = 0x14; break;
                case 4: material = 0x23; break;
                default: break;
                }
                playerState(car, 0) |= at<uint16_t>(kMaterials + material * 0x18 + 4);
                if (rnd(0x3f) == 0)
                    addEffect(at<uint32_t>(m + 0x184), static_cast<int32_t>(wheel), static_cast<int32_t>(material), 8);
                physics = at<uint32_t>(car + 0x20);
                const int32_t rise = ee_ftoi(ee_float(at<float>(at<uint32_t>(physics + 0x1e4) + (wheel < 2 ? 0x2c : 0x68))));
                const uint32_t state = at<uint32_t>(physics + 0x1e8);
                const int32_t shift = at<uint32_t>(at<uint32_t>(car + 0xc) + 4) == 0x11 ? 8 : 9;
                const int16_t sideX = static_cast<int16_t>(sh16(state + 0x28) >> shift), sideY = static_cast<int16_t>(sh16(state + 0x2e) >> shift),
                              sideZ = static_cast<int16_t>(sh16(state + 0x34) >> shift);
                const uint32_t record = physics + wheel * 0x48;
                const int32_t x = ee_ftoi(ee_float(at<float>(record + 0xac))) >> 8;
                const int32_t y = (ee_ftoi(ee_float(at<float>(record + 0xb0))) >> 8) - (rise >> 8);
                const int32_t z = ee_ftoi(ee_float(at<float>(record + 0xb4))) >> 8;
                const uint32_t p = base + ringTail(m) * 0x14u;
                at<uint16_t>(p + 0x10) = 0xf;
                at<uint8_t>(p + 0x13) = static_cast<uint8_t>(material);
                at<uint16_t>(p) = static_cast<uint16_t>(x + static_cast<uint16_t>(sideX));
                at<uint16_t>(p + 2) = static_cast<uint16_t>(y + static_cast<uint16_t>(sideY));
                at<uint16_t>(p + 4) = static_cast<uint16_t>(z + static_cast<uint16_t>(sideZ));
                at<uint16_t>(p + 8) = static_cast<uint16_t>(x - static_cast<uint16_t>(sideX));
                at<uint16_t>(p + 0xa) = static_cast<uint16_t>(y - static_cast<uint16_t>(sideY));
                at<uint16_t>(p + 0xc) = static_cast<uint16_t>(z - static_cast<uint16_t>(sideZ));
                at<uint8_t>(p + 0x12) = at<uint8_t>(m + 0x189) != 0 ? 1 : 0;
                pushTail(m);
                if (at<uint8_t>(m + 0x189) == 0)
                {
                    addEffect(at<uint32_t>(m + 0x184), static_cast<int32_t>(wheel), static_cast<int32_t>(material), 8);
                    at<uint8_t>(m + 0x189) = 1;
                }
            }
            if (rnd(0x1f) == 0 && (playerState(car, 0) & 8) != 0 &&
                ee_float(at<float>(0x3277d0u)) < ee_float(at<float>(at<uint32_t>(car + 0x20) + 0x38)))
                addEffect(at<uint32_t>(m + 0x184), static_cast<int32_t>(wheel), static_cast<int32_t>(material), 8);
        }
        for (uint32_t index = ringHeadU(m); index != ringTail(m); index = (index + 1) & ringMask(m))
        {
            const uint32_t p = base + index * 0x14;
            if (at<uint16_t>(p + 0x10) != 0)
                add16(p + 0x10, -1);
        }
        if (at<uint16_t>(base + ringHeadU(m) * 0x14u + 0x10) == 0 && ringCount(m) != 0)
        {
            ringCount(m) = static_cast<uint8_t>(ringCount(m) - 1);
            ringHeadU(m) = static_cast<uint8_t>(ringMask(m) & (ringHeadU(m) + 1));
        }
    }
    sortAtCarInFrame(m, car, 0x30);
}

// --- the water and car splashes ---------------------------------------------------------------------------------------

namespace {
// How one half of a splash's drops start: each position axis is the effect's direction (shorts at +0x14) over 16 plus a
// random number (posMask) less posBias; the velocity is a random number (velMask for x and z, 0xff for y) plus the
// direction over 32 plus a bias.
struct SplashDrops
{
    int32_t posMask, posBias, xzMask, xzBias, yBias;
};

// Starts drop `i` (0x14 bytes from +0x44: position shorts, velocity shorts at +8, life at +0x10, a flag at +0x12).
void startDrop(uint32_t m, uint32_t i, int32_t lifeBase, const SplashDrops &kind, bool first)
{
    const uint32_t p = m + 0x44 + i * 0x14;
    at<uint16_t>(p + 0x10) = static_cast<uint16_t>(rnd(0x1f) + lifeBase);
    at<uint16_t>(p + 0x12) = first ? 1 : 0;
    for (uint32_t axis = 0; axis < 3; ++axis)
        at<uint16_t>(p + 2 * axis) = static_cast<uint16_t>((sh16(m + 0x14 + 2 * axis) >> 4) + rnd(kind.posMask) - kind.posBias);
    at<uint16_t>(p + 8) = static_cast<uint16_t>(rnd(kind.xzMask) + ((sh16(m + 0x14) >> 5) + kind.xzBias));
    at<uint16_t>(p + 0xa) = static_cast<uint16_t>(rnd(0xff) + ((sh16(m + 0x16) >> 5) + kind.yBias));
    at<uint16_t>(p + 0xc) = static_cast<uint16_t>(rnd(kind.xzMask) + ((sh16(m + 0x18) >> 5) + kind.xzBias));
}

// Moves the drops (gravity 20 a frame) and drops those that fall below the water (the short at `floor`).
void moveDrops(uint32_t m, uint32_t floor)
{
    for (uint32_t index = ringHeadU(m); index != ringTail(m); index = (index + 1) & ringMask(m))
    {
        const uint32_t p = m + 0x44 + index * 0x14;
        const uint16_t vy = at<uint16_t>(p + 0xa);
        const uint16_t y = static_cast<uint16_t>(at<uint16_t>(p + 2) + vy);
        at<uint16_t>(p + 0xa) = static_cast<uint16_t>(vy - 0x14);
        add16(p, at<uint16_t>(p + 8));
        add16(p + 4, at<uint16_t>(p + 0xc));
        at<uint16_t>(p + 2) = y;
        if (static_cast<int16_t>(y) < sh16(floor))
            dropParticle(m, m + 0x44, index, 0x14);
    }
}
} // namespace

// Something landing in water (its position at +0x544): on the first frame (life 50) a cast down from 200 above finds
// the water (the hit point to +0x54c, its height then turned into a depth below the start, times 16) and its normal (the
// splash's direction, shorts at +0x14; straight up when nothing is hit), and 63 drops start, the first 32 low and slow,
// the rest wide and fast. Sorted at the start position.
void VfxHandler_WaterSploosh(ptr32<_sVfx_Object> mem)
{
    GuestFrame frame(0xb0);
    const uint32_t m = mem.address;
    if (at<uint16_t>(m + 0x10) == 0x32)
    {
        const uint32_t from = frame.local<SVECTOR>(0).address;
        std::memcpy(&at<uint8_t>(from), &at<uint8_t>(m + 0x544), 8);
        add16(from + 2, 0xc8);
        if (AColDownCastInt(ptr32<SVECTOR>(from), ptr32<SVECTOR>(m + 0x54c), ptr32<SVECTOR>(m + 0x14), frame.local<uint8>(0x10)))
            at<uint16_t>(m + 0x54e) = static_cast<uint16_t>((at<uint16_t>(m + 0x54e) - at<uint16_t>(m + 0x546)) << 4);
        else
        {
            at<uint16_t>(m + 0x54e) = 0;
            at<uint16_t>(m + 0x16) = 0x1000;
            at<uint16_t>(m + 0x14) = 0;
            at<uint16_t>(m + 0x18) = 0;
        }
        static constexpr SplashDrops kLow{0xff, 0x80, 0x3f, -0x20, 0x3c}, kHigh{0x1ff, 0x100, 0x7f, -0x40, 0x50};
        for (uint32_t i = 0; i < 0x3f; ++i)
            startDrop(m, i, 0x1f, i < 0x20 ? kLow : kHigh, i < 0x20);
        ringCount(m) = 0x3f;
        ringTail(m) = 0x3f;
    }
    moveDrops(m, m + 0x54e);
    vfx::sortPosFromInts(m, sh16(m + 0x544), sh16(m + 0x546), sh16(m + 0x548), 0, 0x20);
    add16(m + 0x10, -1);
}

// A car landing in water (the car at +0x544): marks the player's state (0x10); on the first frame (life 80) the car's
// position (+0x548) is cast down onto the water (the hit point replaces it, cast or not) and as many drops start as the
// ring holds, the first half slow, the second wide and fast. Sorted at the position.
void VfxHandler_CarSplash(ptr32<_sVfx_Object> mem)
{
    GuestFrame frame(0xa0);
    const uint32_t m = mem.address;
    const uint32_t car = at<uint32_t>(m + 0x544);
    playerState(car, 0) |= 0x10;
    if (at<uint16_t>(m + 0x10) == 0x50)
    {
        const uint64_t position = VfxCarPositionVal(ptr32<_sObject>(car));
        std::memcpy(&at<uint8_t>(m + 0x548), &position, 8);
        const uint32_t from = frame.local<SVECTOR>(0).address;
        std::memcpy(&at<uint8_t>(from), &at<uint8_t>(m + 0x548), 8);
        add16(from + 2, 0xc8);
        AColDownCastInt(ptr32<SVECTOR>(from), ptr32<SVECTOR>(m + 0x548), ptr32<SVECTOR>(m + 0x14), frame.local<uint8>(0x20));
        static constexpr SplashDrops kLow{0x1ff, 0x100, 0x7f, -0x40, 0x32}, kHigh{0x1ff, 0x100, 0xff, -0x80, 0x5a};
        for (uint32_t i = 0; static_cast<int32_t>(i) < ringMask(m); ++i)
            startDrop(m, i, 0x2d, static_cast<int32_t>(i) < (ringMask(m) >> 1) ? kLow : kHigh, static_cast<int32_t>(i) < (ringMask(m) >> 1));
        ringCount(m) = ringMask(m);
        ringTail(m) = ringMask(m);
    }
    moveDrops(m, m + 0x54a);
    VfxSortPos(ptr32<SVECTOR>(m + 0x548), mem);
    add16(m + 0x10, -1);
}

// --- the boat's wake --------------------------------------------------------------------------------------------------

// A boat's wake (the boat at +0x204): marks the player's state (4). Every other frame (the counter at +0x20a), for a
// boat near enough to be drawn and moving (speed over 128, capped at 15 as the slice's age), on the water (state bit 2,
// less than 200 above it, both back wheels or floats in it), a cross-section of 0x1c bytes starts at the boat: two
// copies of its position (in svVelTemp) at the water's height, spreading sideways along its velocity (+0x10, +0x14),
// a joined flag (+0x18, from +0x209), a first-frame flag (+0x19) and the age (+0x1a). A boat falling fast that starts
// a new wake makes a splash (VfxAddNew type 0x27) and a sound for the first two players. Sections spread (slower after
// their first frame) and age every other frame; once the oldest is dead it leaves, and the effect ends with the last.
void VfxHandler_BoatWake(ptr32<_sVfx_Object> mem)
{
    GuestFrame frame(0x70);
    const uint32_t m = mem.address;
    const uint32_t car = at<uint32_t>(m + 0x204);
    const uint32_t base = m + 0x44;
    playerState(car, 0) |= 4;
    const uint16_t counter = at<uint16_t>(m + 0x20a);
    at<uint16_t>(m + 0x20a) = static_cast<uint16_t>(counter + 1);
    if ((counter & 1) == 0 && carDistance(car, 0x170) < 0x1d4c)
    {
        const uint32_t physics = at<uint32_t>(car + 0x20);
        int32_t age = at<int32_t>(at<uint32_t>(physics + 0x1e8) + 0x24) >> 7;
        if (!(age < 0x10))
            age = 0xf;
        const auto inWater = [&](uint32_t flagOffset, uint32_t depthOffset)
        {
            if (at<uint32_t>(at<uint32_t>(car + 0xc) + 4) < 0x14)
                return (at<uint16_t>(physics + flagOffset) & 1) != 0;
            return ee_float(0.0f) < ee_float(at<float>(at<uint32_t>(physics + 0x1e0) + depthOffset));
        };
        if (age == 0 || (playerState(car, 2) & 2) == 0 ||
            !(ee_float(at<float>(physics + 0x18)) * ee_float(0.00390625f) - ee_float(at<float>(car + 0x1f0)) < ee_float(200.0f)) ||
            !inWater(0x178, 0x398) || !inWater(0x1c0, 0x528) || !(ringCount(m) < 0xf))
            at<uint8_t>(m + 0x209) = 0;
        else
        {
            const uint32_t state = at<uint32_t>(physics + 0x1e8);
            const uint32_t p = base + ringTail(m) * 0x1cu;
            at<uint16_t>(p + 0x10) = static_cast<uint16_t>(sh16(state + 0x20) >> 8);
            at<uint16_t>(p + 0x14) = static_cast<uint16_t>(-sh16(state + 0x1c) >> 8);
            carPositionToVelTemp(state);
            int32_t height;
            if (at<uint32_t>(at<uint32_t>(car + 0xc) + 4) < 0x14)
            {
                const int32_t sink = ee_ftoi(ee_float(at<float>(at<uint32_t>(physics + 0x1e4) + 0x2c))) >> 8;
                height = ee_ftoi(ee_float(at<float>(at<uint32_t>(car + 0x20) + 0xb0)) * ee_float(0.00390625f) - ee_itof(sink));
            }
            else
            {
                const uint32_t boat = at<uint32_t>(physics + 0x1e0);
                height = ee_ftoi((ee_float(at<float>(boat + 0x324)) - ee_float(at<float>(boat + 0x378))) + ee_float(at<float>(boat + 0x398))) >> 8;
            }
            std::memcpy(&at<uint8_t>(p + 8), &G(svVelTemp), 8);
            std::memcpy(&at<uint8_t>(p), &at<uint8_t>(p + 8), 8);
            at<uint16_t>(p + 2) = static_cast<uint16_t>(height);
            at<uint16_t>(p + 0xa) = static_cast<uint16_t>(height);
            at<uint16_t>(p + 0x1a) = static_cast<uint16_t>(age);
            at<uint8_t>(p + 0x19) = 1;
            at<uint8_t>(p + 0x18) = at<uint8_t>(m + 0x209) != 0 ? 1 : 0;
            pushTail(m);
            if (at<uint8_t>(m + 0x209) == 0)
            {
                if ((playerState(car, 2) & 0x10) == 0 && sh16(state + 0x1e) < -0xdac)
                {
                    guest_call(kVfxAddNew, ptr32<_sObject>(car), 3, 0, 0x27);
                    if (at<uint8_t>(car + 0x44) < 2)
                        guest_call<int32_t>(kRevSfxPlay3D, 0x42, 0xc4, 0x100, ptr32<void>(at<uint32_t>(car + 0x18) + 4), at<uint8_t>(car + 0x44) + 4, 0x1388);
                }
                at<uint8_t>(m + 0x209) = 1;
            }
        }
    }
    for (uint32_t index = ringHeadU(m); index != ringTail(m); index = (index + 1) & ringMask(m))
    {
        const uint32_t p = base + index * 0x1c;
        const uint16_t dx = at<uint16_t>(p + 0x10), dz = at<uint16_t>(p + 0x14);
        add16(p, dx);
        add16(p + 4, dz);
        add16(p + 8, -dx);
        add16(p + 0xc, -dz);
        if (at<uint8_t>(p + 0x19) != 0)
        {
            at<uint16_t>(p + 0x10) = static_cast<uint16_t>(static_cast<int16_t>(dx) >> 3);
            at<uint16_t>(p + 0x14) = static_cast<uint16_t>(static_cast<int16_t>(dz) >> 3);
            at<uint8_t>(p + 0x19) = 0;
        }
        if ((at<uint16_t>(m + 0x20a) & 1) != 0 && at<uint16_t>(p + 0x1a) != 0)
            add16(p + 0x1a, -1);
    }
    if (at<uint16_t>(base + ringHeadU(m) * 0x1cu + 0x1a) == 0 && ringCount(m) != 0)
    {
        const uint8_t count = static_cast<uint8_t>(ringCount(m) - 1);
        ringCount(m) = count;
        ringHeadU(m) = static_cast<uint8_t>(ringMask(m) & (ringHeadU(m) + 1));
        if (count == 0)
            at<uint16_t>(m + 0x10) = 0;
    }
    sortAtCarInFrame(m, car, 0);
}

// --- the aerial -------------------------------------------------------------------------------------------------------

// A car's aerial (the car at +0xb0, which points back at the effect from +0x40): a sprung chain of six sections (VU
// vectors from +0x40 to +0x90) hanging from the aerial's tip. For its first ten frames the bend (+0xc0) is zero; after
// that the bend's speed (+0xd0) follows the car's velocity, springs back and is damped, limited to 250, and the bend
// follows (x and z; y feeds into z). A flag at +0xb4 resets the bend to the car's tilt. The sections are built going up
// from the tip in 12-unit steps, each bent by two small rotations more than the last; sorted at the car, 50 lower.
void VfxHandler_Aerial(ptr32<_sVfx_Object> mem)
{
    GuestFrame frame(0x240);
    const uint32_t m = mem.address;
    const uint32_t car = at<uint32_t>(m + 0xb0);
    at<uint32_t>(car + 0x40) = m;
    const uint32_t rendObj = at<uint32_t>(car + 0x18);
    guest_call(kNodeCalcWorldPos, ptr32<uint8>(at<uint32_t>(car + 0x3c)), ptr32<uint8>(rendObj + 0x18), ptr32<uint8>(rendObj + 4), frame.local<uint8>(0x10));
    const uint16_t life = at<uint16_t>(m + 0x10);
    if (life < 0xa)
    {
        at<uint32_t>(m + 0xc0) = 0;
        at<uint32_t>(m + 0xc4) = 0;
        at<uint32_t>(m + 0xc8) = 0;
        at<uint16_t>(m + 0x10) = static_cast<uint16_t>(life + 1);
    }
    else
    {
        const ee_float follow(at<float>(0x3277f0u)), gain(at<float>(0x3277f4u)), damp(at<float>(0x3277f8u)), spring(at<float>(0x3277fcu));
        const uint32_t velocity = carState(car) + 0x1c;
        const ee_float bendX(at<float>(m + 0xc0)), bendY(at<float>(m + 0xc4)), bendZ(at<float>(m + 0xc8));
        for (uint32_t axis = 0; axis < 3; ++axis)
        {
            const ee_float speed(at<float>(m + 0xd0 + 4 * axis));
            const ee_float bend = axis == 0 ? bendX : axis == 1 ? bendY : bendZ;
            const ee_float push = ee_itof(sh16(velocity + 2 * axis)) * follow;
            at<float>(m + 0xd0 + 4 * axis) = speed + ((push * gain - speed * damp) - bend * spring);
        }
        const ee_float low(-250.0f), high(250.0f);
        ee_float sx(at<float>(m + 0xd0));
        if (sx < low)
            at<float>(m + 0xd0) = low;
        ee_float sy(at<float>(m + 0xd4));
        if (sy < low)
        {
            at<float>(m + 0xd4) = low;
            sy = low;
        }
        ee_float sz(at<float>(m + 0xd8));
        if (sz < low)
        {
            at<float>(m + 0xd8) = low;
            sz = low;
        }
        sx = ee_float(at<float>(m + 0xd0));
        if (high < sx)
        {
            at<float>(m + 0xd0) = high;
            sx = high;
        }
        if (high < sy)
        {
            at<float>(m + 0xd4) = high;
            sy = high;
        }
        if (high < sz)
        {
            at<float>(m + 0xd8) = high;
            sz = high;
        }
        at<float>(m + 0xc0) = bendX + sx;
        at<float>(m + 0xc8) = bendZ + (sz + sy * ee_float(at<float>(0x327800u)));
    }
    const uint32_t state = carState(car);
    if (at<uint32_t>(m + 0xb4) != 0)
    {
        at<uint32_t>(m + 0xb4) = 0;
        at<uint32_t>(m + 0xc4) = 0;
        at<float>(m + 0xc8) = ee_itof(sh16(state + 0x28 + 0x10) >> 3);
        at<float>(m + 0xc0) = ee_itof(sh16(state + 0x28 + 4) >> 3);
    }
    const uint32_t carMatrix = VfxPSXToVUMatrix(ptr32<MATRIX>(carState(car) + 0x28)).address;
    const ee_float bendScale(at<float>(0x327804u)), stepScale(at<float>(0x327808u));
    const uint32_t mat = frame.local<float>(0x40).address, step = frame.local<float>(0x80).address;
    guest_call(kUnitMatrix, ptr32<float>(mat));
    guest_call(kRotMatrixX, ptr32<float>(mat), ptr32<float>(mat), -ee_float(at<float>(m + 0xc8)) * bendScale);
    guest_call(kRotMatrixZ, ptr32<float>(mat), ptr32<float>(mat), ee_float(at<float>(m + 0xc0)) * bendScale);
    guest_call(kUnitMatrix, ptr32<float>(step));
    guest_call(kRotMatrixX, ptr32<float>(step), ptr32<float>(step), ee_float(at<float>(m + 0xc8)) * stepScale);
    guest_call(kRotMatrixZ, ptr32<float>(step), ptr32<float>(step), -ee_float(at<float>(m + 0xc0)) * stepScale);
    // The original copies `step` onto itself here (sceVu0CopyMatrix), which changes nothing.
    const uint32_t up = frame.local<float>(0xd0).address, world = frame.local<float>(0xe0).address;
    at<uint32_t>(up) = 0;
    at<float>(up + 4) = 60.0f;
    at<uint32_t>(up + 8) = 0;
    for (uint32_t section = 0x90; section >= 0x40; section -= 0x10)
    {
        sceVu0ApplyMatrix(ptr32<ee_float>(world), ptr32<ee_float>(carMatrix), ptr32<ee_float>(up));
        sceVu0ApplyMatrix(ptr32<ee_float>(m + section), ptr32<ee_float>(mat), ptr32<ee_float>(world));
        guest_call(kMulMatrix, ptr32<float>(mat), ptr32<float>(mat), ptr32<float>(step));
        at<float>(up + 4) = ee_float(at<float>(up + 4)) - ee_float(12.0f);
    }
    sortAtCarInFrame(m, car, 0x160);
    at<float>(m + 0x24) = ee_float(at<float>(m + 0x24)) - ee_float(50.0f);
}

// --- the spin pickup --------------------------------------------------------------------------------------------------

namespace {
// The number of rounds a weapon pickup of `kind` gives (1 to 19): the battery types 1 and 11 one or three at random,
// the dumper boat's kind 15 one, the others one, three or seven. Zero for a kind with no entry.
uint8_t pickupRounds(uint32_t car, uint32_t kind)
{
    switch (kind)
    {
    case 1:
    case 11: return RandomGetNDRange(1, 3) == 2 ? 1 : 3;
    case 5:
    case 14: return 3;
    case 15: return at<uint32_t>(at<uint32_t>(car + 0xc) + 4) == 0x24 ? 1 : 3;
    case 16: return 7;
    default: return 1;
    }
}
} // namespace

// The spin of a weapon pickup round a car (the car at +0x44): on the first frame (life 63) the car's velocity over 256
// (shorts at +0xf0) moves the spin's centre (+0xe8); the angle (+0xf8) turns faster each frame (+0xfa). Until its last
// eight frames sparks of 0x14 bytes (from +0x48) fly out. On the last frame the car gets the weapon (kind at +0xfc):
// if it is firing it only queues it (+0x1af, +0x1ae and the rounds at +0x194); otherwise it holds it now (+0x46, +0x195,
// the rounds), with a shield (VfxAddNew 0x36) for kind 1, a sound and an effect for kinds 5 (0xb) and 8 (the heat ray:
// its store and sound handle, effect 4), and the queue is cleared. Not at the end of a race.
void VfxHandler_SpinPickup(ptr32<_sVfx_Object> mem)
{
    GuestFrame frame(0x80);
    const uint32_t m = mem.address;
    const uint32_t car = at<uint32_t>(m + 0x44);
    if (at<uint16_t>(m + 0x10) == 0x3f)
    {
        std::memcpy(&at<uint8_t>(m + 0xf0), &at<uint8_t>(carState(car) + 0x1c), 8);
        at<uint16_t>(m + 0xfa) = 1;
        for (uint32_t axis = 0; axis < 3; ++axis)
        {
            const int32_t v = sh16(m + 0xf0 + 2 * axis);
            at<uint16_t>(m + 0xf0 + 2 * axis) = static_cast<uint16_t>((v > -1 ? v : v + 0xff) >> 8);
        }
        at<uint16_t>(m + 0xf8) = 0;
    }
    const uint16_t x = static_cast<uint16_t>(at<uint16_t>(m + 0xe8) + at<uint16_t>(m + 0xf0));
    add16(m + 0xea, at<uint16_t>(m + 0xf2));
    add16(m + 0xec, at<uint16_t>(m + 0xf4));
    at<uint16_t>(m + 0xe8) = x;
    vfx::sortPosFromInts(m, static_cast<int16_t>(x), sh16(m + 0xea), sh16(m + 0xec), 0, 0x10);
    const uint16_t speed = at<uint16_t>(m + 0xfa);
    at<uint16_t>(m + 0xfa) = static_cast<uint16_t>(speed + 6);
    add16(m + 0xf8, speed);
    if (!(at<uint16_t>(m + 0x10) < 8) && ringCount(m) < ringMask(m))
    {
        const uint32_t base = m + 0x48;
        const auto spark = [&](uint32_t offset, int32_t mask, int32_t bias)
        { at<uint16_t>(base + ringTail(m) * 0x14u + offset) = static_cast<uint16_t>(rnd(mask) - bias); };
        spark(0, 0x7f, 0x40);
        spark(2, 0x7f, 0x40);
        spark(4, 0x7f, 0x40);
        spark(8, 0xff, 0x80);
        spark(0xa, 0xff, 0x80);
        spark(0xc, 0xff, 0x80);
        at<uint16_t>(base + ringTail(m) * 0x14u + 0x10) = 7;
        pushTail(m);
    }
    VfxSimpleParticleMover(ptr32<_sVfx_PartHeader>(m + kRing), ptr32<_VFX_SIMPLEPARTICLE>(m + 0x48));
    const uint16_t life = static_cast<uint16_t>(at<uint16_t>(m + 0x10) - 1);
    at<uint16_t>(m + 0x10) = life;
    if (life != 0)
        return;
    at<uint8_t>(car + 0x196) = 0;
    if (at<uint32_t>(kMainVar) == 0x20)
        return;
    const uint8_t kind = at<uint8_t>(m + 0xfc);
    const bool known = static_cast<uint32_t>(static_cast<int8_t>(kind - 1)) < 0x13;
    if (at<int8_t>(car + 0x1a3) != 0) // firing: queue it
    {
        at<uint8_t>(car + 0x1af) = kind;
        if (!known)
            return;
        const uint8_t rounds = pickupRounds(car, kind);
        at<uint8_t>(car + 0x1ae) = rounds;
        at<uint8_t>(car + 0x194) = rounds;
        return;
    }
    at<uint8_t>(car + 0x46) = kind;
    at<uint8_t>(car + 0x195) = kind;
    if (known)
    {
        const ptr32<void> where(at<uint32_t>(car + 0x18) + 4);
        if (kind == 5)
        {
            at<uint8_t>(car + 0x194) = 3;
            guest_call<int32_t>(kRevSfxPlay3D, 0x1f, 0xff, 0x100, where, at<uint8_t>(car + 0x44) + 4, 0x1388);
            guest_call(kVfxAddNew, ptr32<_sObject>(car), 0, 0, 0xb);
        }
        else if (kind == 8)
        {
            at<uint32_t>(car + 0x1b0) = 0;
            at<uint8_t>(car + 0x194) = 1;
            at<uint16_t>(car + 0x198) = 0x1388;
            at<uint16_t>(car + 0x19e) =
                static_cast<uint16_t>(guest_call<int32_t>(kRevSfxPlay3D, 0x15, 0xff, 0x100, where, at<uint8_t>(car + 0x44) + 4, 0x1388));
            guest_call(kVfxAddNew, ptr32<_sObject>(car), 0, 0, 4);
        }
        else
        {
            if (kind == 1)
                guest_call(kVfxAddNew, ptr32<_sObject>(car), 0, 0, 0x36);
            at<uint8_t>(car + 0x194) = pickupRounds(car, kind);
        }
    }
    at<uint8_t>(car + 0x1af) = 0;
    at<uint8_t>(car + 0x1ae) = 0;
}

// --- the respawn sparks -----------------------------------------------------------------------------------------------

namespace {
// A respawn spark: a random direction, normalised, a 32nd of it as its position and half that as its velocity.
void respawnSpark(uint32_t p)
{
    at<uint16_t>(p) = static_cast<uint16_t>(rnd(0xff) - 0x80);
    at<uint16_t>(p + 2) = static_cast<uint16_t>(rnd(0xff) - 0x80);
    at<uint16_t>(p + 4) = static_cast<uint16_t>(rnd(0xff) - 0x80);
    VecNormalise(ptr32<SVECTOR>(p), ptr32<SVECTOR>(p));
    for (uint32_t axis = 0; axis < 3; ++axis)
    {
        const int32_t v = sh16(p + 2 * axis) >> 5;
        at<uint16_t>(p + 2 * axis) = static_cast<uint16_t>(v);
        at<uint16_t>(p + 8 + 2 * axis) = static_cast<uint16_t>(v >> 1);
    }
    at<uint16_t>(p + 0x10) = 100;
}
} // namespace

// A car respawning (the car at +0x2c4): on the first frame (life 100) 31 sparks burst out (0x14 bytes from +0x44) and a
// spin starts (+0x2c8, speed 400 at +0x2ca); unless the car is still drawn (+0x172), its physics is held (+0x1cd bit 2)
// and from then on the sparks move and the spin slows. At life 33 the car is put back (its float position at +0xbc from
// the physics, +0x18a = 2, +0x172 cleared) and the sparks turn back four times as fast; until then its physics is
// stopped (velocities and spins zero), and at the end +0x18a is cleared. Sorted at the car (the original's last call).
void VfxHandler_CarRespawn(ptr32<_sVfx_Object> mem)
{
    const uint32_t m = mem.address;
    const uint32_t car = at<uint32_t>(m + 0x2c4);
    if (at<uint16_t>(m + 0x10) == 100)
    {
        const int32_t spin = rnd(0xfff);
        at<uint16_t>(m + 0x2ca) = 400;
        at<uint16_t>(m + 0x2c8) = static_cast<uint16_t>(spin);
        for (uint32_t p = m + 0x44; p < m + 0x2b0; p += 0x14)
            respawnSpark(p);
        ringCount(m) = 0x1f;
        ringTail(m) = 0x1f;
        if (at<int16_t>(car + 0x172) == 0)
            at<uint8_t>(at<uint32_t>(car + 0x20) + 0x1cd) |= 2;
    }
    else
    {
        VfxSimpleParticleMover(ptr32<_sVfx_PartHeader>(m + kRing), ptr32<_VFX_SIMPLEPARTICLE>(m + 0x44));
        const uint16_t speed = at<uint16_t>(m + 0x2ca);
        add16(m + 0x2c8, speed);
        at<uint16_t>(m + 0x2ca) = static_cast<uint16_t>(speed - (static_cast<int16_t>(speed) >> 5));
    }
    const uint16_t life = static_cast<uint16_t>(at<uint16_t>(m + 0x10) - 1);
    at<uint16_t>(m + 0x10) = life;
    if (life == 0x21)
    {
        at<uint8_t>(car + 0x18a) = 2;
        at<uint16_t>(car + 0x172) = 0;
        const uint32_t state = carState(car);
        const int32_t z = at<int32_t>(state + 0xc) >> 8, x = at<int32_t>(state + 4) >> 8, y = at<int32_t>(state + 8) >> 8;
        at<float>(car + 0xc4) = ee_itof(z);
        at<float>(car + 0xbc) = ee_itof(x);
        at<float>(car + 0xc0) = ee_itof(y);
        for (uint32_t index = ringHeadU(m); index != ringTail(m); index = (index + 1) & ringMask(m))
        {
            const uint32_t p = m + 0x44 + index * 0x14;
            for (uint32_t offset : {8u, 0xcu, 0xau})
                at<uint16_t>(p + offset) = static_cast<uint16_t>(-(at<uint16_t>(p + offset) << 2));
        }
    }
    const uint16_t now = at<uint16_t>(m + 0x10);
    if (!(now < 0x21))
    {
        const uint32_t physics = at<uint32_t>(car + 0x20);
        at<uint32_t>(physics + 0x28) = 0;
        at<uint16_t>(physics) = 0;
        at<uint16_t>(physics + 2) = 0;
        at<uint32_t>(physics + 0x40) = 0;
        at<uint32_t>(physics + 0x44) = 0;
        at<uint32_t>(physics + 0x48) = 0;
        at<uint32_t>(physics + 0x20) = 0;
        at<uint32_t>(physics + 0x24) = 0;
    }
    else if (now == 0)
        at<uint8_t>(car + 0x18a) = 0;
    VfxCarSortPos(ptr32<_sObject>(car), mem);
}

// --- the spray --------------------------------------------------------------------------------------------------------

// A boat's spray (the boat at +0x44): marks the player's state (0x40). For a boat near enough to be drawn and fast
// (over 2000), on the water (state bit 2, less than 200 above it, both back floats in it), a drop of 0x1c bytes starts
// at a random side of the boat's back (the shape header's bounds at +0x6c/+0x6e, through the boat's matrix), thrown
// back and up by the speed (velocity at +0x10, rise at +0x12 up to 512), with a life word of 7 at +0x18. Drops fall 5 a
// frame and leave below -40. When the oldest drop is not alive (+0x18) one leaves, and the effect ends with the last;
// only then, or with live drops, is the sort position set (at the boat).
void VfxHandler_Spray(ptr32<_sVfx_Object> mem)
{
    GuestFrame frame(0x90);
    const uint32_t m = mem.address;
    const uint32_t car = at<uint32_t>(m + 0x44);
    const uint32_t base = m + 0x48;
    playerState(car, 0) |= 0x40;
    if (VfxHasCarBeenRendered(ptr32<_sObject>(car)) < 0x157c && ee_float(2000.0f) < ee_float(at<float>(at<uint32_t>(car + 0x20) + 0x38)) &&
        (playerState(car, 2) & 2) != 0 &&
        ee_float(at<float>(at<uint32_t>(car + 0x20) + 0x18)) * ee_float(0.00390625f) - ee_float(at<float>(car + 0x1f0)) < ee_float(200.0f) &&
        VfxIsWheelTouching(ptr32<_sObject>(car), 2) != 0 && VfxIsWheelTouching(ptr32<_sObject>(car), 3) != 0 && ringCount(m) < 0xf &&
        ringCount(m) < ringMask(m))
    {
        // The original also fetches the car's velocity and matrix pointers here and does not use them.
        const uint32_t shape = at<uint32_t>(at<uint32_t>(car + 0x18));
        const ptr32<MATRIX> matrix = VfxCarMatrixPoi(ptr32<_sObject>(car));
        const uint32_t v = frame.local<SVECTOR>(0).address;
        const uint16_t side = at<uint16_t>(shape + 0x6e);
        at<uint16_t>(v) = static_cast<uint16_t>((vfx::random15() & 1) != 0 ? side - 0x14 : -side + 0x14);
        at<uint16_t>(v + 2) = 0;
        at<uint16_t>(v + 4) = static_cast<uint16_t>(at<uint16_t>(shape + 0x6c) + 0xc);
        PS2C_SetGteMatrix(ptr32<_sMat>(matrix.address));
        PS2C_GteMatrixMulVec16(ptr32<_sVec16>(v));
        PS2C_GetVec16GteResult(ptr32<_sVec16>(base + ringTail(m) * 0x1cu + 8));
        uint32_t p = base + ringTail(m) * 0x1cu;
        at<uint16_t>(p + 0xa) = 0;
        at<uint16_t>(p) = 0;
        at<uint16_t>(p + 2) = 0;
        at<uint16_t>(p + 4) = 0;
        at<uint16_t>(v) = static_cast<uint16_t>(rnd(0x1f) - 0x10);
        at<uint16_t>(v + 4) = static_cast<uint16_t>(-ee_ftoi(ee_float(at<float>(at<uint32_t>(car + 0x20) + 0x38))) >> 7);
        PS2C_SetGteMatrix(ptr32<_sMat>(matrix.address));
        PS2C_GteMatrixMulVec16(ptr32<_sVec16>(v));
        PS2C_GetVec16GteResult(ptr32<_sVec16>(base + ringTail(m) * 0x1cu + 0x10));
        const int32_t r = rnd(0x1f);
        p = base + ringTail(m) * 0x1cu;
        int32_t rise = (r + ee_ftoi(ee_float(at<float>(at<uint32_t>(car + 0x20) + 0x38)))) >> 9;
        if (!(rise < 0x201))
            rise = 0x200;
        at<uint16_t>(p + 0x12) = static_cast<uint16_t>(rise);
        at<uint16_t>(p + 0x18) = 7;
        pushTail(m);
    }
    if (ringCount(m) != 0)
    {
        const ee_float floor(-40.0f);
        for (uint32_t index = ringHeadU(m); index != ringTail(m); index = (index + 1) & ringMask(m))
        {
            const uint32_t p = base + index * 0x1c;
            const uint16_t vy = at<uint16_t>(p + 0x12);
            const uint16_t y = static_cast<uint16_t>(at<uint16_t>(p + 2) + vy);
            add16(p, at<uint16_t>(p + 0x10));
            add16(p + 4, at<uint16_t>(p + 0x14));
            at<uint16_t>(p + 0x12) = static_cast<uint16_t>(vy - 5);
            at<uint16_t>(p + 2) = y;
            if (ee_itof(static_cast<int16_t>(y)) < floor)
                dropParticle(m, base, index, 0x1c);
        }
    }
    if (at<uint16_t>(base + ringHeadU(m) * 0x1cu + 0x18) == 0 && ringCount(m) != 0)
    {
        const uint8_t count = static_cast<uint8_t>(ringCount(m) - 1);
        ringCount(m) = count;
        ringHeadU(m) = static_cast<uint8_t>(ringMask(m) & (ringHeadU(m) + 1));
        if (count == 0)
            at<uint16_t>(m + 0x10) = 0;
        return;
    }
    VfxCarSortPos(ptr32<_sObject>(car), mem);
}

// --- the give-terror pickup -------------------------------------------------------------------------------------------

// A car giving terror (the car at +0x2cc, a timer at +0x2d0 counting down): on the first frame (life 96) 31 sparks of
// 0x14 bytes start at the car (their position zero, the pad short a word of the original's stack), flying up, and every
// other car that is not invincible (+0x19c) gets a take-terror effect (VfxAddNew 0x14). Half way (life 48) those cars
// lose their weapon effects (ObjectWeaponAffectRemove), are stunned (+0x18c = 9) and thrown 2000 up. Sorted at the car;
// the sparks move.
void VfxHandler_GiveTerror(ptr32<_sVfx_Object> mem)
{
    GuestFrame frame(0xd0);
    const uint32_t m = mem.address;
    const uint32_t car = at<uint32_t>(m + 0x2cc);
    add16(m + 0x2d0, -1);
    const uint32_t count = kObjects + 0x170;
    if (at<uint16_t>(m + 0x10) == 0x60)
    {
        const uint32_t start = frame.local<SVECTOR>(0).address;
        at<uint16_t>(start) = 0;
        at<uint16_t>(start + 2) = 0;
        at<uint16_t>(start + 4) = 0;
        for (int32_t i = 0; i < 31; ++i)
        {
            const uint32_t p = m + 0x44 + ringTail(m) * 0x14u;
            std::memcpy(&at<uint8_t>(p), &at<uint8_t>(start), 8);
            at<uint16_t>(m + 0x44 + ringTail(m) * 0x14u + 8) = static_cast<uint16_t>(rnd(0x7f) - 0x40);
            at<uint16_t>(m + 0x44 + ringTail(m) * 0x14u + 0xa) = static_cast<uint16_t>(rnd(0x3f));
            at<uint16_t>(m + 0x44 + ringTail(m) * 0x14u + 0xc) = static_cast<uint16_t>(rnd(0x7f) - 0x3f);
            at<uint16_t>(m + 0x44 + ringTail(m) * 0x14u + 0x10) = static_cast<uint16_t>(rnd(0xf) + 0x50);
            pushTail(m);
        }
        for (uint32_t i = 0; i < at<uint32_t>(count); ++i)
        {
            if (i == at<uint8_t>(car + 0x45))
                continue;
            const uint32_t other = at<uint32_t>(kObjects + 4 * i);
            if (at<int8_t>(other + 0x19c) == 0)
                guest_call(kVfxAddNew, ptr32<_sObject>(other), 0, 0, 0x14);
        }
    }
    else if (at<uint16_t>(m + 0x10) == 0x30)
    {
        for (uint32_t i = 0; i < at<uint32_t>(count); ++i)
        {
            if (i == at<uint8_t>(car + 0x45) || at<int8_t>(at<uint32_t>(kObjects + 4 * i) + 0x19c) != 0)
                continue;
            guest_call(kObjectWeaponAffectRemove, ptr32<_sObject>(at<uint32_t>(kObjects + 4 * i)));
            const uint32_t other = at<uint32_t>(kObjects + 4 * i);
            at<uint16_t>(other + 0x18c) = 9;
            const uint32_t physics = at<uint32_t>(other + 0x20);
            at<float>(physics + 0x24) = ee_float(at<float>(physics + 0x24)) + ee_float(2000.0f);
        }
    }
    sortAtCarInFrame(m, car, 0x10);
    VfxSimpleParticleMover(ptr32<_sVfx_PartHeader>(m + kRing), ptr32<_VFX_SIMPLEPARTICLE>(m + 0x44));
    add16(m + 0x10, -1);
}

} // namespace rc

// Forward declarations of the random-input setups (visfx_fuzz.cpp).
namespace rc::fuzz {
void vfxQuakeHandler(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxOilDumpHandler(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxCarSkidMarkHandler(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxWaterSplooshHandler(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxCarSplashHandler(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxBoatWakeHandler(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxAerialHandler(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxSpinPickupHandler(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxCarRespawnHandler(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxSprayHandler(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void vfxGiveTerrorHandler(R5900Context *ctx, uint64_t &state, uint32_t scratch);
}

RC_DECOMP_FN_SETUP(VfxHandler_Quake, 0x162638, rc::fuzz::vfxQuakeHandler)
RC_DECOMP_FN_SETUP(VfxHandler_Quake2, 0x168540, rc::fuzz::vfxQuakeHandler)
RC_DECOMP_FN_SETUP(VfxHandler_OilDump, 0x15a1a0, rc::fuzz::vfxOilDumpHandler)
RC_DECOMP_FN_SETUP(VfxHandler_WaterSploosh, 0x15cde8, rc::fuzz::vfxWaterSplooshHandler)
RC_DECOMP_FN_SETUP(VfxHandler_CarSplash, 0x170be0, rc::fuzz::vfxCarSplashHandler)
RC_DECOMP_FN_SETUP(VfxHandler_Aerial, 0x15e970, rc::fuzz::vfxAerialHandler)
RC_DECOMP_FN_SETUP(VfxHandler_CarRespawn, 0x16be48, rc::fuzz::vfxCarRespawnHandler)
RC_DECOMP_FN_SETUP(VfxHandler_Spray, 0x177370, rc::fuzz::vfxSprayHandler)
// These add effects (VfxAddNew), play sounds or change other cars: their random inputs compare and restore all of RAM.
RC_DECOMP_FN_SETUP_HEAP(VfxHandler_CarSkidMark, 0x15b010, rc::fuzz::vfxCarSkidMarkHandler)
RC_DECOMP_FN_SETUP_HEAP(VfxHandler_BoatWake, 0x15d610, rc::fuzz::vfxBoatWakeHandler)
RC_DECOMP_FN_SETUP_HEAP(VfxHandler_SpinPickup, 0x160e80, rc::fuzz::vfxSpinPickupHandler)
RC_DECOMP_FN_SETUP_HEAP(VfxHandler_GiveTerror, 0x15fc28, rc::fuzz::vfxGiveTerrorHandler)
