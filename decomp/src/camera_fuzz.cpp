// Builds random but valid camera inputs for the random test. See camera_fuzz.h.
//
// The scratch area starts as random bytes, so every field not set here is random.

#include "camera_fuzz.h"

#include <cmath>
#include <cstring>

namespace rc {
uint32_t &randState(); // ai.cpp: newlib's rand state
}

namespace rc::fuzz {

namespace {

constexpr uint32_t kCar = 0x0000u, kPhysics = 0x1000u, kInteger = 0x2000u, kRendObj = 0x2200u;
constexpr uint32_t kCam = 0x3000u, kPath = 0x3600u, kVecs = 0x3700u, kMat = 0x3800u, kNode = 0x3900u, kCellOut = 0x3a00u;
constexpr uint32_t kCameraSize = 0x5b8u, kCameras = 0x1e9fb0u;
constexpr uint32_t kEndOfPath = 0x117cf8u, kHitFirstNode = 0x112f70u;
static_assert(sizeof(_sObject) <= kPhysics - kCar);
static_assert(sizeof(_sFPhysLibCar) <= kInteger - kPhysics);
static_assert(sizeof(_sOPhysLibIntegerCarState) <= kRendObj - kInteger);
static_assert(sizeof(_sCamera) == kCameraSize && kCam + kCameraSize <= kPath);

struct Rng
{
    uint64_t &state;

    uint32_t next() { return nextFuzz(state); }
    int below(int count) { return count > 0 ? static_cast<int>(next() % static_cast<uint32_t>(count)) : 0; }
    bool chance(int percent) { return below(100) < percent; }
    int between(int low, int high) { return low + below(high - low + 1); }
    float range(float low, float high) { return low + (high - low) * (static_cast<float>(next() >> 8) / 16777216.0f); }
};

template <typename T>
T &at(uint32_t address) { return *static_cast<T *>(guest_host(address)); }

// The stack below $sp gets random words (stale reads: CameraLvlInitCamera's position, CameraTagToPathNode's player
// camera angles), and rand a random state.
void setArg(R5900Context *ctx, int index, uint32_t value)
{
    static uint64_t stackState = 0xca3e5a3d12345678ull;
    if (index == 0)
    {
        const uint32_t sp = getRegU32(ctx, 29);
        for (uint32_t offset = 0x800; offset >= 4; offset -= 4)
            at<uint32_t>(sp - offset) = nextFuzz(stackState);
        randState() = nextFuzz(stackState);
    }
    SET_GPR_S32(ctx, 4 + index, static_cast<int32_t>(value));
}

// Bytes of the game's globals a setup varies, put back by the next camera setup if they still hold what was written.
struct Claim
{
    uint32_t address = 0, size = 0;
    bool active = false;
    uint8_t original[0x40]{}, written[0x40]{};

    void set(uint32_t where, const void *value, uint32_t bytes)
    {
        if (!(active && address == where && std::memcmp(guest_host(where), written, bytes) == 0))
        {
            std::memcpy(original, guest_host(where), bytes);
            active = true;
        }
        address = where;
        size = bytes;
        std::memcpy(guest_host(where), value, bytes);
        std::memcpy(written, value, bytes);
    }
    void restore()
    {
        if (active && std::memcmp(guest_host(address), written, size) == 0)
            std::memcpy(guest_host(address), original, size);
        active = false;
    }
};

Claim g_keys, g_sticks, g_split, g_trigger, g_grid;

void restoreGlobals()
{
    g_keys.restore();
    g_sticks.restore();
    g_split.restore();
    g_trigger.restore();
    g_grid.restore();
}

void claimSplit(Rng &rng)
{
    const uint8_t split = static_cast<uint8_t>(rng.chance(70) ? 0 : rng.chance(80) ? 1 : 2);
    g_split.set(guest_addr(&G(mainVarG).splitScreen), &split, 1);
}

// The pad: keys 0x0e to 0x1f held (bit 0) or just hit (bit 5) now and then, the right stick anywhere.
void claimPad(Rng &rng)
{
    const uint32_t inactive = guest_addr(&G(inputVarG).inactiveBuffer[0]);
    uint8_t keys[0x20];
    std::memcpy(keys, guest_host(inactive), 0x20);
    for (uint32_t k = 0xe; k < 0x20; ++k)
        keys[k] = static_cast<uint8_t>((rng.chance(30) ? 1 : 0) | (rng.chance(20) ? 0x20 : 0) | (rng.next() & 0xde));
    g_keys.set(inactive, keys, 0x20);
    int8_t sticks[2];
    for (int8_t &s : sticks)
        s = static_cast<int8_t>(rng.chance(50) ? rng.between(-0x60, 0x60) : static_cast<int>(rng.next() & 0xff) - 0x80);
    g_sticks.set(guest_addr(&G(inputVarG).analog2A), sticks, 2);
}

// One of the race's cars (or a car of zeroes outside a race) copied into scratch, its position and matrix varied.
uint32_t buildTarget(Rng &rng, uint32_t scratch)
{
    const sObjectClass &objects = G(objectVarG);
    ptr32<_sObject> source = nullptr;
    for (int tries = 0; tries < 16 && !source; ++tries)
        source = objects.list[rng.below(2)];
    _sObject &car = at<_sObject>(scratch + kCar);
    _sFPhysLibCar &physics = at<_sFPhysLibCar>(scratch + kPhysics);
    if (source && source->pPhysics && source->pPhysics->pInteger && source->pRendObj)
    {
        car = *source;
        physics = *source->pPhysics;
        at<_sOPhysLibIntegerCarState>(scratch + kInteger) = *physics.pInteger;
        std::memcpy(guest_host(scratch + kRendObj), source->pRendObj.get(), 0x38);
    }
    else
    {
        std::memset(guest_host(scratch + kCar), 0, 0x2300);
    }
    car.pPhysics = ptr32<_sFPhysLibCar>(scratch + kPhysics);
    car.pRendObj = ptr32<_sRendObj>(scratch + kRendObj);
    physics.pInteger = ptr32<_sOPhysLibIntegerCarState>(scratch + kInteger);
    if (rng.chance(50))
    {
        physics.pos.x = physics.pos.x.v + rng.range(-40000.0f, 40000.0f);
        physics.pos.y = physics.pos.y.v + rng.range(-10000.0f, 10000.0f);
        physics.pos.z = physics.pos.z.v + rng.range(-40000.0f, 40000.0f);
    }
    if (rng.chance(40))
    {
        ee_float *m = &physics.mat.Xx;
        for (int k = 0; k < 9; ++k)
            m[k] = rng.range(-4096.0f, 4096.0f);
    }
    return scratch + kCar;
}

// Varies a camera in place: modes, angles, offsets, the tunnel state, its path and spline points. Positions stay near
// the target car (in the camera's 1/256 units, which the car's physics position also uses).
void varyCamera(Rng &rng, uint32_t address, uint32_t target, bool anyMode)
{
    _sCamera &c = at<_sCamera>(address);
    c.cameraNo = rng.below(2);
    if (target)
        c.pTarget = ptr32<_sObject>(target);
    static const int kModes[] = {0, 1, 2, 3, 4, 5, 7, 8, 10, 11, 12, 13, 16, 17, 6, 9, 14, 15};
    c.mode = static_cast<eCameraMode>(anyMode ? (rng.chance(90) ? rng.below(18) : rng.below(30)) : kModes[rng.below(18)]);
    c.near = rng.chance(85) ? rng.below(3) : rng.between(-2, 5);
    if (c.pTarget && c.pTarget->pPhysics && rng.chance(60))
    {
        const _sFPhysLibCar &physics = *c.pTarget->pPhysics;
        c.pos.x = static_cast<int32>(physics.pos.x.v) + rng.between(-0x30000, 0x30000);
        c.pos.y = static_cast<int32>(physics.pos.y.v) + rng.between(-0x8000, 0x30000);
        c.pos.z = static_cast<int32>(physics.pos.z.v) + rng.between(-0x30000, 0x30000);
    }
    c.oldPos.x = c.pos.x + rng.between(-0x2000, 0x2000);
    for (angle *a : {&c.angles.pitch, &c.angles.head, &c.angles.roll, &c.freeangles.pitch, &c.freeangles.head,
                     &c.freeangles.roll, &c.rotAngles.pitch, &c.rotAngles.head, &c.rotAngles.roll})
        *a = static_cast<angle>(rng.chance(30) ? rng.next() : rng.between(-0x6000, 0x6000));
    for (vec32_ *v : {&c.offset, &c.origOffset, &c.lookAtOffset, &c.origLookAtOffset, &c.targetOffset, &c.desiredGoto})
        if (rng.chance(40))
        {
            v->x = rng.between(-0x30000, 0x30000);
            v->y = rng.between(-0x30000, 0x30000);
            v->z = rng.between(-0x30000, 0x30000);
        }
    if (rng.chance(10))
        c.targetOffset.x = static_cast<int32>(rng.next());
    c.inTunnel = rng.below(2);
    c.forceInTunnel = rng.chance(20) ? 1 : 0;
    c.inTunnelDelta = rng.chance(85) ? rng.between(-100, 0x1100) : static_cast<int32>(rng.next());
    c.dValue = static_cast<uint16>(rng.chance(70) ? rng.between(0xe6, 0x800) : rng.next());
    c.lookBackwards = rng.below(2);
    c.lookBackwardsOldState = rng.below(2);
    c.hasChanged = rng.below(2);

    // The path: its own nodes, a segment inside them, an interpolation step, both callbacks now and then.
    c.path.pNodes = ptr32<_sCameraPathNode>(address + 0xdcu);
    c.path.numNodesOnPath = rng.between(4, 33);
    c.path.curNodeOnPath = rng.chance(90) ? rng.between(1, 27) : rng.between(0, 3);
    c.path.interp = rng.chance(10) ? -1 : rng.chance(30) ? rng.between(0xe00, 0x1000) : rng.between(0, 0x1000);
    for (_sCameraPathNode &node : c.nodes)
    {
        node.pos.x = c.pos.x + rng.between(-0x40000, 0x40000);
        node.pos.y = c.pos.y + rng.between(-0x10000, 0x10000);
        node.pos.z = c.pos.z + rng.between(-0x40000, 0x40000);
        node.lookAtPos.x = node.pos.x + rng.between(-0x80000, 0x80000);
        node.lookAtPos.y = node.pos.y + rng.between(-0x20000, 0x20000);
        node.lookAtPos.z = node.pos.z + rng.between(-0x80000, 0x80000);
        node.speed = rng.chance(90) ? rng.between(0x20, 0x600) : static_cast<int32>(rng.next());
    }
    for (vec32_ *points : {c.splinePoint, c.splineAnglePoint})
        for (int k = 0; k < 4; ++k)
        {
            const bool wide = rng.chance(15);
            points[k].x = wide ? static_cast<int32>(rng.next()) : rng.between(-0x1000000, 0x1000000);
            points[k].y = wide ? static_cast<int32>(rng.next()) : rng.between(-0x1000000, 0x1000000);
            points[k].z = wide ? static_cast<int32>(rng.next()) : rng.between(-0x1000000, 0x1000000);
        }
    c.feFunctionCallBack.address = rng.chance(50) ? kEndOfPath : 0u;
    c.feFunctionCallBackNode.address = rng.chance(50) ? kHitFirstNode : 0u;
}

// A start-grid path: tags of the level's cameras, now and then one of the follow camera's (below 0x10000000).
uint32_t buildPath(Rng &rng, uint32_t address)
{
    sCameraPath &path = at<sCameraPath>(address);
    const uint32_t length = static_cast<uint32_t>(rng.between(1, 6));
    path.pathLength = static_cast<uint8>(length);
    path.pathSpeed = static_cast<uint16>(rng.chance(90) ? rng.between(0x40, 0x400) : rng.next());
    const ptr32<_sLevelData> level = G(pLvlG);
    const ptr32<_sCamData> cams = level ? level->pCameras : ptr32<_sCamData>(nullptr);
    const int numCams = cams ? cams->numCams : 0;
    for (uint32_t k = 0; k < length + 1; ++k)
    {
        uint32_t tag = static_cast<uint32_t>(rng.below(0x100));
        if (numCams > 0 && numCams < 200 && rng.chance(80))
            tag = cams->cameras[rng.below(numCams)].tag;
        at<uint32_t>(address + 8u + 4u * k) = tag;
    }
    return address;
}

// The camera in scratch (a copy of one of the game's two), its target car and its start-grid path.
uint32_t buildCamera(Rng &rng, uint32_t scratch)
{
    restoreGlobals();
    const uint32_t target = buildTarget(rng, scratch);
    const uint32_t camera = scratch + kCam;
    std::memcpy(guest_host(camera), guest_host(kCameras + kCameraSize * static_cast<uint32_t>(rng.below(2))), kCameraSize);
    varyCamera(rng, camera, target, true);
    at<_sCamera>(camera).pStartGridPath = ptr32<sCameraPath>(buildPath(rng, scratch + kPath));
    claimSplit(rng);
    return camera;
}

void setVec(uint32_t address, int32_t x, int32_t y, int32_t z)
{
    vec32_ &v = at<vec32_>(address);
    v.x = x;
    v.y = y;
    v.z = z;
}

} // namespace

void cameraOne(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    setArg(ctx, 0, buildCamera(rng, scratch));
}

void cameraZoom(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    setArg(ctx, 0, buildCamera(rng, scratch));
    const int kind = rng.below(4);
    const uint32_t distance = kind == 0 ? 0xe600u + rng.between(-2, 2) : kind == 1 ? 0x80000u + rng.between(-2, 2)
                              : kind == 2 ? static_cast<uint32_t>(rng.between(0, 0x90000)) : rng.next();
    setArg(ctx, 1, distance);
}

void cameraDecelerate(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    restoreGlobals();
    const uint32_t src = scratch + kVecs, dest = src + 0x10, out = src + 0x20;
    for (uint32_t v : {src, dest, out})
        setVec(v, rng.between(-0x100000, 0x100000), static_cast<int32_t>(rng.next()), rng.between(-1000, 1000));
    if (rng.chance(10))
    {
        setVec(src, 0, 0, 0);
        setVec(dest, INT32_MIN, INT32_MIN, INT32_MIN);
    }
    const int kind = rng.below(5);
    const int32_t rate = kind == 0 ? 0 : kind == 1 ? -1 : kind == 2 ? rng.between(-20, 20) : static_cast<int32_t>(rng.next() >> rng.below(31));
    setArg(ctx, 0, src);
    setArg(ctx, 1, dest);
    setArg(ctx, 2, static_cast<uint32_t>(rate));
    for (int axis = 0; axis < 3; ++axis)
        setArg(ctx, 3 + axis, rng.chance(50) ? 0u : rng.chance(80) ? 1u : rng.next());
    setArg(ctx, 6, rng.chance(15) ? src : out);
}

void cameraOffsets(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    const uint32_t camera = buildCamera(rng, scratch);
    setArg(ctx, 0, camera);
    for (int k = 1; k <= 2; ++k)
    {
        const int kind = rng.below(10);
        // Null, the camera itself (the near field is overwritten before it is read), or a vector of its own.
        setArg(ctx, k, kind == 0 ? 0u : kind == 1 ? camera + 4u * rng.below(2) : scratch + kVecs + 0x10u * k);
    }
}

void cameraWithMatrix(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    setArg(ctx, 0, buildCamera(rng, scratch));
    setArg(ctx, 1, scratch + kMat);
}

void cameraLookAt(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    const uint32_t camera = buildCamera(rng, scratch);
    const _sCamera &c = at<_sCamera>(camera);
    const int kind = rng.below(10);
    if (kind == 0)
        setVec(scratch + kVecs, c.pos.x, c.pos.y, c.pos.z);
    else if (kind == 1)
        setVec(scratch + kVecs, c.pos.x, c.pos.y + rng.between(-0x10000, 0x10000), c.pos.z);
    else
        setVec(scratch + kVecs, c.pos.x + rng.between(-0x80000, 0x80000), c.pos.y + rng.between(-0x20000, 0x20000),
               c.pos.z + rng.between(-0x80000, 0x80000));
    setArg(ctx, 0, camera);
    setArg(ctx, 1, scratch + kVecs);
}

void cameraAINode(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    setArg(ctx, 0, buildCamera(rng, scratch));
    const auto &ai = G(aiNodeVarG);
    uint32_t node = scratch + kNode;
    if (ai.pNode && ai.noNodesSingle > 0)
        node = ai.pNode.address + static_cast<uint32_t>(rng.below(ai.noNodesSingle)) * sizeof(_sAINode);
    setArg(ctx, 1, node);
    setArg(ctx, 2, static_cast<uint32_t>(rng.chance(80) ? rng.between(0, 0x100) : rng.between(-0x8000, 0x8000)));
}

void cameraMode(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    setArg(ctx, 0, buildCamera(rng, scratch));
    setArg(ctx, 1, static_cast<uint32_t>(rng.chance(90) ? rng.below(18) : rng.below(40)));
}

void cameraLvlInit(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    setArg(ctx, 0, buildCamera(rng, scratch));
    setArg(ctx, 1, rng.chance(85) ? static_cast<uint32_t>(rng.below(2)) : rng.next());
    setArg(ctx, 2, static_cast<uint32_t>(rng.below(2)));
}

// The level's cameras set up again, from the main state varied: the front end, a level past the last, a demo race, no
// start-grid path; the split screen (whole RAM, so nothing needs putting back).
void cameraLevel(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    (void)scratch;
    mainClass &main = G(mainVarG);
    if (rng.chance(15))
        main.currentDo = static_cast<eMainToDo>(1);
    if (rng.chance(10))
        main.level = static_cast<uint8>(rng.between(0x60, 0x70));
    if (rng.chance(25))
        main.status = static_cast<eMain>(0x10);
    if (rng.chance(25))
        main.status = static_cast<eMain>(rng.below(0x20));
    main.splitScreen = static_cast<uint8>(rng.below(2));
    if (rng.chance(10))
        G(pLvlG) = nullptr;
    setArg(ctx, 0, rng.chance(85) ? static_cast<uint32_t>(rng.below(2)) : 0xffffffffu);
}

void cameraPath(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    setArg(ctx, 0, buildCamera(rng, scratch));
    setArg(ctx, 1, static_cast<uint32_t>(rng.below(8)));
}

void cameraNewPath(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    setArg(ctx, 0, buildCamera(rng, scratch));
    setArg(ctx, 1, rng.chance(70) ? kEndOfPath : rng.next());
    setArg(ctx, 2, rng.chance(70) ? kHitFirstNode : rng.next());
    setArg(ctx, 3, scratch + kPath);
}

void cameraTag(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    const uint32_t camera = buildCamera(rng, scratch);
    setArg(ctx, 0, scratch + kPath);
    const int length = at<sCameraPath>(scratch + kPath).pathLength;
    const int kind = rng.below(10);
    // A position on the path, one before it, or one whose upper half the int16 argument drops.
    setArg(ctx, 1, static_cast<uint32_t>(kind == 0 ? -1 : kind == 1 ? 0x10000 + rng.below(length) : rng.below(length)));
    setArg(ctx, 2, rng.chance(80) ? scratch + kNode : camera + 0xdcu + 0x24u * static_cast<uint32_t>(rng.below(30)));
    setArg(ctx, 3, camera);
}

void cameraSpline(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    setArg(ctx, 0, buildCamera(rng, scratch));
    setArg(ctx, 1, static_cast<uint32_t>(rng.chance(85) ? rng.between(0, 0x1000) : static_cast<int32_t>(rng.next())));
    setArg(ctx, 2, scratch + kVecs);
    setArg(ctx, 3, rng.chance(50) ? 0u : rng.chance(80) ? 1u : rng.next());
}

void cameraCoeffs(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    const uint32_t camera = buildCamera(rng, scratch);
    if (rng.chance(20)) // a node's fields anywhere
    {
        _sCamera &c = at<_sCamera>(camera);
        c.nodes[rng.below(30)].pos.y = static_cast<int32>(rng.next());
        c.nodes[rng.below(30)].lookAtPos.x = static_cast<int32>(rng.next());
    }
    setArg(ctx, 0, camera);
    setArg(ctx, 1, static_cast<uint32_t>(rng.chance(90) ? rng.between(1, 28) : rng.between(0, 30)));
    setArg(ctx, 2, rng.chance(50) ? 0u : rng.chance(80) ? 1u : rng.next());
}

void cameraPad(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    const uint32_t camera = buildCamera(rng, scratch);
    claimPad(rng);
    for (uint32_t k = 0; k < 6; ++k)
        if (rng.chance(30))
            at<float>(0x327cecu + 4 * k) = rng.range(0.0f, 5.0f);
    setArg(ctx, 0, camera);
}

// Both of the game's cameras varied in place (whole RAM): any mode, the pad, the split screen; split 0 or 1, or -1 with
// $s2 (the caller's register the original then reads) at one of the cameras.
void cameraProcess(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    (void)scratch;
    for (uint32_t i = 0; i < 2; ++i)
    {
        const uint32_t camera = kCameras + i * kCameraSize;
        varyCamera(rng, camera, 0, false);
        at<_sCamera>(camera).cameraNo = static_cast<int32>(i);
    }
    claimPad(rng);
    G(mainVarG).splitScreen = static_cast<uint8>(rng.below(2));
    const int kind = rng.below(10);
    const int32_t split = kind == 0 ? -1 : kind < 5 ? 1 : 0;
    SET_GPR_S32(ctx, 18, static_cast<int32_t>(kCameras + kCameraSize * static_cast<uint32_t>(rng.below(2))));
    setArg(ctx, 0, rng.next());
    setArg(ctx, 1, static_cast<uint32_t>(split));
}

namespace {

// A point inside the level grid's extents (sometimes a little outside), heights over its range.
void gridPoint(Rng &rng, const _sColGridPSX &grid, float &x, float &y, float &z)
{
    const float spanX = grid.max.x.v - grid.min.x.v, spanZ = grid.max.z.v - grid.min.z.v;
    x = grid.min.x.v + rng.range(-0.05f, 1.05f) * spanX;
    z = grid.min.z.v + rng.range(-0.05f, 1.05f) * spanZ;
    y = rng.range(grid.min.y.v - 50.0f, grid.max.y.v + 50.0f);
}

void claimTrigger(Rng &rng)
{
    const uint32_t trigger = static_cast<uint32_t>(rng.below(3));
    g_trigger.set(guest_addr(&G(carColG).triggerState[0]), &trigger, 4);
}

} // namespace

// A cell of the level and a segment over it: from inside the cell's square to a point near it, often downwards.
void cameraCell(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    restoreGlobals();
    claimTrigger(rng);
    const ptr32<_sColGridPSX> gridPtr = G(acolG).pGrid;
    if (!gridPtr)
        return;
    const _sColGridPSX &grid = *gridPtr;
    const int32_t cellX = rng.below(grid.numCellsX), cellZ = rng.below(grid.numCellsZ);
    const float x0 = grid.min.x.v + grid.cellSize.x.v * static_cast<float>(cellX);
    const float z0 = grid.min.z.v + grid.cellSize.z.v * static_cast<float>(cellZ);
    _sColCellVecPSX &s = at<_sColCellVecPSX>(scratch + kVecs), &e = at<_sColCellVecPSX>(scratch + kVecs + 0x10);
    s.x = x0 + rng.range(0.0f, grid.cellSize.x.v);
    s.z = z0 + rng.range(0.0f, grid.cellSize.z.v);
    s.y = rng.range(grid.min.y.v, grid.max.y.v);
    e.x = s.x.v + rng.range(-grid.cellSize.x.v, grid.cellSize.x.v);
    e.z = s.z.v + rng.range(-grid.cellSize.z.v, grid.cellSize.z.v);
    e.y = rng.chance(70) ? s.y.v - rng.range(0.0f, grid.max.y.v - grid.min.y.v) : rng.range(grid.min.y.v, grid.max.y.v);
    uint32_t cell = gridPtr->pCell.address + 16u * static_cast<uint32_t>(cellX * grid.numCellsZ + cellZ);
    const _sColGridCellPSX &real = at<_sColGridCellPSX>(cell);
    const uint32_t count = real.list.num[0];
    if (rng.chance(35) && count > 0 && count <= 24)
    {
        // A copy of the grid whose cell's polys are copies in scratch, some of material 0x1a (the scifi door, which
        // no dumped level has): acolG points at it until the next camera setup.
        constexpr uint32_t kGrid = 0x4000u, kIds = 0x4100u, kPolys = 0x4200u, kCell = 0x40e0u;
        std::memcpy(guest_host(scratch + kGrid), &grid, sizeof(_sColGridPSX));
        _sColGridPSX &copy = at<_sColGridPSX>(scratch + kGrid);
        for (uint32_t k = 0; k < count; ++k)
        {
            const uint32_t id = at<uint32_t>(grid.pIds.address + 4u * (real.list.offset + k));
            std::memcpy(guest_host(scratch + kPolys + 0x60u * k), guest_host(grid.pPoly.address + 0x60u * id), 0x60);
            at<uint32_t>(scratch + kIds + 4u * k) = k;
            if (rng.chance(40))
                at<_sColPolyPSX>(scratch + kPolys + 0x60u * k).mat = 0x1a;
        }
        copy.pIds = ptr32<uint32>(scratch + kIds);
        copy.pPoly = ptr32<_sColPolyPSX>(scratch + kPolys);
        _sColGridCellPSX &fake = at<_sColGridCellPSX>(scratch + kCell);
        fake = real;
        fake.list.offset = 0;
        cell = scratch + kCell;
        const uint32_t pointer = scratch + kGrid;
        g_grid.set(guest_addr(&G(acolG).pGrid), &pointer, 4);
    }
    setArg(ctx, 0, cell);
    setArg(ctx, 1, scratch + kVecs);
    setArg(ctx, 2, scratch + kVecs + 0x10);
    setArg(ctx, 3, scratch + kCellOut);
    setArg(ctx, 4, scratch + kCellOut + 0x10);
}

// A segment over the level, a few cells long (now and then off the grid), as the follow camera casts.
void cameraRay(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    restoreGlobals();
    claimTrigger(rng);
    const ptr32<_sColGridPSX> gridPtr = G(acolG).pGrid;
    if (!gridPtr)
        return;
    const _sColGridPSX &grid = *gridPtr;
    _sColCellVecPSX &s = at<_sColCellVecPSX>(scratch + kVecs), &e = at<_sColCellVecPSX>(scratch + kVecs + 0x10);
    float x, y, z;
    gridPoint(rng, grid, x, y, z);
    s.x = x;
    s.y = y;
    s.z = z;
    const float reach = rng.range(0.0f, 3.0f);
    e.x = x + rng.range(-reach, reach) * grid.cellSize.x.v;
    e.z = z + rng.range(-reach, reach) * grid.cellSize.z.v;
    e.y = rng.chance(60) ? y - rng.range(0.0f, 300.0f) : rng.range(grid.min.y.v, grid.max.y.v);
    setArg(ctx, 0, scratch + kVecs);
    setArg(ctx, 1, scratch + kVecs + 0x10);
    setArg(ctx, 2, scratch + kCellOut);
    setArg(ctx, 3, scratch + kCellOut + 0x10);
}

// The follow camera: the scratch camera somewhere around its copied car (on the level, so the wall test, the side
// rays and the ground cast find real polys), last frame's position near the desired one, far off or close to the
// car; the car spinning, reversing or hit by the shaking weapon now and then; the camera's counters; both flags.
void cameraFollow(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    const uint32_t camera = buildCamera(rng, scratch);
    _sCamera &c = at<_sCamera>(camera);
    _sObject &car = at<_sObject>(scratch + kCar);
    _sFPhysLibCar &physics = at<_sFPhysLibCar>(scratch + kPhysics);
    const int32_t carX = static_cast<int32_t>(physics.pos.x.v), carY = static_cast<int32_t>(physics.pos.y.v),
                  carZ = static_cast<int32_t>(physics.pos.z.v);
    const int kind = rng.below(10);
    const int32_t reach = kind < 6 ? 0x30000 : kind < 8 ? 0x4000 : kind < 9 ? 0x100000 : 0x800;
    c.oldPos.x = carX + rng.between(-reach, reach);
    c.oldPos.y = carY + rng.between(-0x4000, 0x20000);
    c.oldPos.z = carZ + rng.between(-reach, reach);
    if (rng.chance(5))
        c.oldPos.y = static_cast<int32>(rng.next());
    if (rng.chance(3)) // anywhere, often off the level
    {
        c.oldPos.x = static_cast<int32>(rng.next());
        c.oldPos.z = static_cast<int32>(rng.next());
    }
    const ptr32<_sColGridPSX> gridPtr = G(acolG).pGrid;
    if (gridPtr && rng.chance(50))
    {
        // Near a wall: last frame's position just off one of the level's polys, the car a camera's distance away
        // (or against the same poly), so the wall test, the side rays and the look-back rays hit.
        const _sColGridPSX &grid = *gridPtr;
        for (int tries = 0; tries < 8; ++tries)
        {
            const uint32_t cellIndex = static_cast<uint32_t>(rng.below(grid.numCellsX * grid.numCellsZ));
            const _sColGridCellPSX &cell = at<_sColGridCellPSX>(grid.pCell.address + 16u * cellIndex);
            const uint32_t count = cell.list.num[0] + cell.list.num[1];
            if (count == 0 || count > 4096)
                continue;
            const uint32_t id = at<uint32_t>(grid.pIds.address + 4u * (cell.list.offset + static_cast<uint32_t>(rng.below(static_cast<int>(count)))));
            const _sColPolyPSX &poly = at<_sColPolyPSX>(grid.pPoly.address + 0x60u * id);
            const _sColCellVertPSX &a = poly.v[0], &b = poly.v[1 + rng.below(2)];
            const float blend = rng.range(0.0f, 1.0f), push = rng.range(-160.0f, 160.0f);
            const float px = a.x.v + (b.x.v - a.x.v) * blend + poly.n.x.v * push;
            const float py = a.y.v + (b.y.v - a.y.v) * blend + poly.n.y.v * push + rng.range(-40.0f, 40.0f);
            const float pz = a.z.v + (b.z.v - a.z.v) * blend + poly.n.z.v * push;
            c.oldPos.x = static_cast<int32>(px * 256.0f);
            c.oldPos.y = static_cast<int32>(py * 256.0f);
            c.oldPos.z = static_cast<int32>(pz * 256.0f);
            if (rng.chance(30))
            {
                const float off = rng.range(10.0f, 140.0f);
                physics.pos.x = (px + poly.n.x.v * off) * 256.0f;
                physics.pos.y = (py + rng.range(-60.0f, 20.0f)) * 256.0f;
                physics.pos.z = (pz + poly.n.z.v * off) * 256.0f;
            }
            else
            {
                const float turn = rng.range(-3.1416f, 3.1416f), dist = rng.range(80.0f, 700.0f);
                physics.pos.x = (px + std::cos(turn) * dist) * 256.0f;
                physics.pos.y = (py + rng.range(-200.0f, 40.0f)) * 256.0f;
                physics.pos.z = (pz + std::sin(turn) * dist) * 256.0f;
            }
            break;
        }
    }
    c.timeCounter = rng.chance(40) ? 0 : rng.chance(90) ? rng.between(0, 70) : rng.between(-5, 0x200);
    c.collided = rng.chance(85) ? rng.between(0, 16) : rng.between(-40, 60);
    c.rayCollided = rng.chance(90) ? rng.below(2) : static_cast<int32>(rng.next());
    c.backwardsCount = rng.chance(85) ? rng.between(0, 12) : rng.between(-3, 30);
    c.spinCounter = rng.chance(85) ? rng.between(0, 70) : rng.between(-5, 200);
    c.lookBackwards = rng.chance(70) ? 0 : rng.chance(90) ? 1 : static_cast<bool_>(rng.next());
    if (rng.chance(40))
        physics.angVel.y = rng.range(-6000.0f, 6000.0f);
    if (rng.chance(10))
        physics.angVel.y = rng.chance(50) ? 3000.0f : -3000.0f;
    if (rng.chance(40))
    {
        physics.vel.x = rng.range(-30000.0f, 30000.0f);
        physics.vel.z = rng.range(-30000.0f, 30000.0f);
        physics.speed = rng.range(0.0f, 16000.0f);
    }
    if (rng.chance(10))
        physics.speed = 8000.0f;
    physics.inputBrake = static_cast<int16>(rng.chance(70) ? 0 : rng.chance(80) ? 1 : rng.between(-3, 3));
    car.weaponAffect = static_cast<int16>(rng.chance(30) ? 9 : rng.below(12));
    car.shakeCameraStrength = rng.chance(80) ? rng.range(0.0f, 4.0f) : rng.range(-100.0f, 100.0f);
    setArg(ctx, 0, camera);
    setArg(ctx, 1, rng.chance(70) ? 0u : rng.chance(85) ? 1u : rng.next());
    setArg(ctx, 2, rng.chance(25) ? 0u : rng.chance(85) ? 1u : rng.next());
}

} // namespace rc::fuzz
