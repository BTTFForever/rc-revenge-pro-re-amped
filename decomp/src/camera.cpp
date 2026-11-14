// Decompiled from CAMERA.c: the race cameras. cameraVarG holds two cameras (one per split-screen player); each has a
// mode (eCameraMode) that CameraProcess runs once a frame. Positions are 1/256 units (pos >> 8 is the matrix's
// translation), angles 12-bit, matrices 4.12 fixed point. The start-of-race fly-by is a path of camera nodes (level
// camera tags, or the follow camera's own view for a tag below 0x10000000) splined with a Catmull-Rom matrix
// (ComputeCoeffs, SplinePoint); the follow camera's state is saved in copyCamG while the path runs.

#include "ee_math.h"
#include "camera_fuzz.h"

#define F(value) ::rc::ee_float(value##f)

namespace rc {

void Vec32MulMatPsx(ptr32<vec32_> p, ptr32<_matPsx> mp, ptr32<vec32_> r);  // maths.cpp
void Vec32MulMatPsxT(ptr32<vec32_> p, ptr32<_matPsx> mp, ptr32<vec32_> r); // maths.cpp
angle ArcTan(int32 x, int32 y);                                            // maths.cpp
void MatPsxInit(ptr32<_matPsx> in);                                        // maths.cpp
void PS2C_Vec16CrossVec16(ptr32<_sVec16> pOut, ptr32<_sVec16> pVec1, ptr32<_sVec16> pVec2); // psxdefs.cpp
void MemBlockSet32(memHandle handle, uint32 value, uint32 size);           // mem.cpp
bool_ InputHeld(uint8 key);                                                // ai.cpp
void AColCellXZFromPos(ptr32<_sColGridPSX> pGrid, ee_float x, ee_float z, ptr32<int32> pX, ptr32<int32> pZ);
int32 AColGetCellIdFromXZ(ptr32<_sColGridPSX> pGrid, int32 x, int32 z);
bool_ AColRay_Poly_1Way(ptr32<_sColCellVecPSX> pS, ptr32<_sColCellVecPSX> pE, ptr32<_sColPolyPSX> pP, ptr32<_sColCellVecPSX> pPoi);

void CameraFollowPlayer(ptr32<_sCamera> pCamera, bool_ goStraightThere, bool_ doCollision); // below: still the original
void CameraSetModeDefaults(ptr32<_sCamera> pCamera);
void CameraSetMode(ptr32<_sCamera> pCamera, int32 mode);
void CameraSetMatrixFromLookAt(ptr32<_sCamera> pCamera, ptr32<vec32_> pLookAt);
void CameraSetZoomMode(ptr32<_sCamera> pCamera, uint32 distance);
void CameraLvlInitCamera(ptr32<_sCamera> pCamera, int32 num, int32 i);
void CameraFollowPath(ptr32<_sCamera> pCamera);
void CameraSetStartGridPath(ptr32<_sCamera> pCamera);
void CameraTagToPathNode(ptr32<sCameraPath> pPath, int32 pathPos, ptr32<_sCameraPathNode> pNode, ptr32<_sCamera> pCamera);

namespace {

void tagToPathNode(ptr32<sCameraPath> pPath, int32 pathPos, ptr32<_sCameraPathNode> pNode, ptr32<_sCamera> pCamera, uint32_t ra);

const uint32_t kMat3nBuild = 0x19e710, kMat3nBuildXYZ = 0x19e768, kVec32Normalise = 0x19fb78, kVec32Length = 0x19fae8;
const uint32_t kInputHit = 0x191598, kGetCameraFromTag = 0x13ca18, kFpToUi = 0x1b6d50;
const uint32_t kEndOfStartGridPathCallBack = 0x117cf8, kStartGridHitFirstNodeCallBack = 0x112f70;
const uint32_t kObjectList = 0x1ee390u, kCopyCam = 0x1eab28u, kCameras = 0x1e9fb0u, kOrbitAngles = 0x327d08u;
const uint32_t kCameraSize = 0x5b8u;

const ee_float kHeldStep = float_from_bits(0x3dcccccdu);    // 0.1, the six constants at 0x3275e0
const ee_float kFovScale = float_from_bits(0x3cb60b61u);    // 0x3275f8: 60 degrees of field over 2700 zoom units
const ee_float kCosineTurn = float_from_bits(0x3a490fdbu);  // 0x3275fc: pi / 4096

template <typename T>
T &at(uint32_t address) { return *static_cast<T *>(guest_host(address)); }

// objectVarG.list[n] and copyCamG[n] for any n, as the original indexes them (a guest address, wrapped into RAM).
ptr32<_sObject> objectAt(int32_t n) { return at<ptr32<_sObject>>(kObjectList + static_cast<uint32_t>(n) * 4u); }
ptr32<_sCamera> copyCamAt(int32_t n) { return ptr32<_sCamera>(kCopyCam + static_cast<uint32_t>(n) * kCameraSize); }

void copyVec(vec32_ &dst, const vec32_ &src)
{
    const int32 x = src.x, y = src.y, z = src.z;
    dst.x = x;
    dst.y = y;
    dst.z = z;
}

// A whole camera, copied as the original's doubleword loop does (each 8 bytes read before they are written).
void copyCamera(uint32_t dst, uint32_t src)
{
    for (uint32_t offset = 0; offset < kCameraSize; offset += 8)
    {
        uint64_t word;
        std::memcpy(&word, guest_host(src + offset), 8);
        std::memcpy(guest_host(dst + offset), &word, 8);
    }
}

// The standard follow offset and look-at offset for the camera's distance setting (near), written to any vectors,
// with the original's order of loads and stores (CameraSetOffsets, inlined four times in CameraSetNear).
void setOffset(_sCamera &camera, uint32_t offsetAddress)
{
    vec32_ &offset = at<vec32_>(offsetAddress);
    offset.x = 0;
    offset.y = 0x9600;
    const int32_t near = camera.near;
    if (near == 0)
        offset.z = -0x1a400;
    else if (near == 1)
        offset.z = -70000;
    else if (near == 2)
        offset.z = -0x24400;
    int32_t forwards = offset.z >> 8;
    if (forwards < 0)
        forwards = -forwards;
    camera.forwardsZ = forwards;
}

void setLookAtOffset(_sCamera &camera, uint32_t lookAtAddress)
{
    const int32 y = camera.near == 1 ? 0x6590 : 0x5a00;
    vec32_ &lookAt = at<vec32_>(lookAtAddress);
    lookAt.z = 0x1400;
    lookAt.y = y;
    lookAt.x = 0;
}

// angles cleared, offset back to the original one.
void resetAngles(_sCamera &camera)
{
    camera.angles.roll = 0;
    camera.hasChanged = 1;
    camera.angles.head = 0;
    camera.angles.pitch = 0;
    copyVec(camera.offset, camera.origOffset);
}

// The rotation looking from the camera's position along a vector: z along it, x level, y their cross product (the
// body of CameraSetMatrixFromLookAt, inlined in CameraPlaceAtAINode with its own frame). The locals are in the
// caller's frame at the offsets given.
void matrixAlong(_sCamera &camera, const GuestFrame &frame, uint32_t xAxisAt, uint32_t vectorAt, uint32_t xNormalAt,
                 uint32_t yNormalAt, uint32_t zNormalAt)
{
    const ptr32<_sVec32> xAxis = frame.local<_sVec32>(xAxisAt), vector = frame.local<_sVec32>(vectorAt);
    const ptr32<_sVec16> xNormal = frame.local<_sVec16>(xNormalAt), yNormal = frame.local<_sVec16>(yNormalAt),
                         zNormal = frame.local<_sVec16>(zNormalAt);
    guest_call(kVec32Normalise, zNormal, vector);
    if (zNormal->x == 0 && zNormal->z == 0)
        zNormal->x = 1;
    xAxis->x = zNormal->z;
    xAxis->z = -static_cast<int32_t>(zNormal->x);
    xAxis->y = 0;
    guest_call(kVec32Normalise, xNormal, xAxis);
    PS2C_Vec16CrossVec16(yNormal, zNormal, xNormal);
    _mat3n &m = camera.matrix.mat;
    m.Xx = xNormal->x;
    m.Zz = zNormal->z;
    m.Xy = yNormal->x;
    m.Xz = zNormal->x;
    m.Yx = xNormal->y;
    m.Yy = yNormal->y;
    m.Yz = zNormal->y;
    m.Zx = xNormal->z;
    m.Zy = yNormal->z;
}

// A car's physics position, each axis converted to an integer.
void targetPosition(const _sFPhysLibCar &physics, vec32_ &out)
{
    out.z = ee_ftoi(physics.pos.z);
    out.x = ee_ftoi(physics.pos.x);
    out.y = ee_ftoi(physics.pos.y);
}

// From the camera to a car, each axis in floats and then converted.
void towardsCar(const _sCamera &camera, const _sFPhysLibCar &physics, vec32_ &out)
{
    const ee_float cz = ee_itof(camera.pos.z), cx = ee_itof(camera.pos.x), cy = ee_itof(camera.pos.y);
    const ee_float dz = physics.pos.z - cz, dx = physics.pos.x - cx, dy = physics.pos.y - cy;
    out.z = ee_ftoi(dz);
    out.x = ee_ftoi(dx);
    out.y = ee_ftoi(dy);
}

// A held key's speed-up: its length grows by a tenth a frame up to 4.
ee_float &grow(uint32_t address)
{
    ee_float &length = at<ee_float>(address);
    ee_float grown = length + kHeldStep;
    length = grown;
    if (F(4.0) < grown)
        length = F(4.0);
    return length;
}

// An angle moved by (length + 1) * 100, through floats.
int16 turn(int16 value, ee_float length, bool add)
{
    const ee_float step = (length + F(1.0)) * F(100.0);
    const ee_float base = ee_itof(value);
    return static_cast<int16>(ee_ftoi(add ? base + step : base - step));
}

// x / 4096, rounding towards zero, in 64 bits.
int64_t q12(int64_t value) { return (value < 0 ? value + 0xfff : value) >> 12; }
int64_t mul64(int64_t a, int64_t b) { return static_cast<int64_t>(static_cast<uint64_t>(a) * static_cast<uint64_t>(b)); }

// One axis of the cubic a t^3 + b t^2 + c t + d (4.12 fixed point), halved.
int32_t splineAxis(int32_t a, int32_t b, int32_t c, int32_t d, int32_t t, int64_t t2, int64_t t3)
{
    const int64_t first = q12(mul64(a, t3)) + d;
    const int64_t rest = q12(static_cast<int64_t>(c) * t) + q12(mul64(b, t2));
    const uint64_t sum = static_cast<uint64_t>(first) + static_cast<uint64_t>(rest);
    return static_cast<int32_t>(static_cast<uint32_t>((sum + (sum >> 63)) >> 1));
}

// The Catmull-Rom basis (times two), rows for t^3, t^2, t, 1.
const int32_t kBasis[4][4] = {{-1, 3, -3, 1}, {2, -5, 4, -1}, {-1, 0, 1, 0}, {0, 2, 0, 0}};

// One axis of the four coefficients from the field at fieldOffset of nodes n-1 to n+2, into out (stride 12 bytes).
void coeffAxis(uint32_t nodes, int32_t n, uint32_t fieldOffset, uint32_t out)
{
    const uint32_t base = nodes + static_cast<uint32_t>(n * 0x24) + fieldOffset;
    const int32_t p3 = at<int32_t>(base + 0x48), p2 = at<int32_t>(base + 0x24), p0 = at<int32_t>(base - 0x24), p1 = at<int32_t>(base);
    for (int row = 3; row >= 0; --row)
        at<int32_t>(out + 12u * row) = kBasis[row][0] * p0 + kBasis[row][3] * p3 + kBasis[row][2] * p2 + kBasis[row][1] * p1;
}

} // namespace

void CameraInit() {}

void CameraTerm() {}

void CameraLvlTerm() {}

int32_t CameraDebugInfo(int32_t) { return 0; }

void CameraTerminatePath(ptr32<_sCamera> pCamera) { pCamera->path.interp = -1; }

void CameraSetTunnelMode(ptr32<_sCamera> pCamera) { pCamera->inTunnel = 1; }

void CameraUnSetTunnelMode(ptr32<_sCamera> pCamera) { pCamera->inTunnel = 0; }

void CameraReset(ptr32<_sCamera> pCamera) { resetAngles(*pCamera); }

void CameraModeDefaultsFollowPlayer(ptr32<_sCamera> pCamera)
{
    resetAngles(*pCamera);
    pCamera->pTarget = objectAt(pCamera->cameraNo);
}

int32 Linear_Interpolate(int32 a, int32 b, int32 x)
{
    const int32_t bs = (b < 0 ? b + 0xff : b) >> 8;
    const int32_t as = (a < 0 ? a + 0xff : a) >> 8;
    return as * (0x100 - x) + bs * x;
}

int32 Cosine_Interpolate(int32 a, int32 b, int32 x)
{
    const int32_t half = (x / 2) & 0xfff;
    ee_float angle = ee_itof(half);
    angle = (angle + angle) * kCosineTurn;
    const int32_t c = ee_ftoi(ee_cosf(angle) * F(4096.0));
    const int32_t as = (a < 0 ? a + 0xfff : a) >> 12;
    const int32_t bs = (b < 0 ? b + 0xfff : b) >> 12;
    const int32_t f = (0x1000 - c) / 2;
    return as * (0x1000 - f) + bs * f;
}

void CameraSetZoomMode(ptr32<_sCamera> pCamera, uint32 distance)
{
    if (distance <= 0xe600u)
        pCamera->dValue = 0xe6;
    else if (distance > 0x80000u)
        pCamera->dValue = 0x800;
    else
        pCamera->dValue = static_cast<uint16>(((distance - 0xe600u) >> 8) + 0xe6);
}

int32 CameraDampingProcess(int32 value)
{
    if (value < 0x401)
    {
        if (value >= -0x400)
            return 0;
        if (value >= -0x4000)
            return value + 0x400;
    }
    else if (value < 0x4001)
        return value - 0x400;
    return value - (value >> 3);
}

void CameraSetOffsetForTwoPlayerMode(ptr32<_sCamera> pCamera)
{
    vec32_ &offset = pCamera->offset;
    const ee_float scale = G(offset2player);
    const ee_float x = ee_itof(offset.x) * scale, y = ee_itof(offset.y) * scale, z = ee_itof(offset.z) * scale;
    offset.x = ee_ftoi(x);
    offset.y = ee_ftoi(y);
    offset.z = ee_ftoi(z);
}

// The tunnel blend: up by 125 a frame while in a tunnel (to 0x1000), down by 62.5 out of one (to 0). The original
// goes through the library's doubles; a value moved by 62.5 truncates towards zero.
void CameraHandleTunnels(ptr32<_sCamera> pCamera)
{
    _sCamera &camera = *pCamera;
    if (camera.inTunnel != 0 || camera.forceInTunnel != 0)
    {
        const int64_t up = static_cast<int64_t>(camera.inTunnelDelta) + 125;
        camera.inTunnelDelta = up > 0x1000 ? 0x1000 : static_cast<int32>(up);
    }
    else
    {
        const int32_t delta = camera.inTunnelDelta;
        camera.inTunnelDelta = delta > 62 ? delta - 63 : 0;
    }
}

void CameraDecelerateInterp(ptr32<vec32_> pSrc, ptr32<vec32_> pDest, int32 rate, bool_ x, bool_ y, bool_ z, ptr32<vec32_> pWriteTo)
{
    if (rate == 0)
    {
        copyVec(*pWriteTo, *pSrc);
        return;
    }
    if (x)
    {
        const int32_t d = pDest->x - pSrc->x;
        pWriteTo->x = pWriteTo->x + ee_div(d, rate);
    }
    if (y)
    {
        const int32_t d = pDest->y - pSrc->y;
        pWriteTo->y = pWriteTo->y + ee_div(d, rate);
    }
    if (z)
    {
        const int32_t d = pDest->z - pSrc->z;
        pWriteTo->z = pWriteTo->z + ee_div(d, rate);
    }
}

void CameraSetOffsets(ptr32<_sCamera> pCamera, ptr32<vec32_> pOffset, ptr32<vec32_> pLookAtOffset)
{
    if (pOffset)
        setOffset(*pCamera, pOffset.address);
    if (pLookAtOffset)
        setLookAtOffset(*pCamera, pLookAtOffset.address);
}

// CameraSetOffsets inlined for the offset and look-at offset and for their originals. The null tests are of the
// fields' addresses, so they always pass.
void CameraSetNear(ptr32<_sCamera> pCamera)
{
    _sCamera &camera = *pCamera;
    const uint32_t base = pCamera.address;
    if (base + 0x58u != 0)
        setOffset(camera, base + 0x58u);
    if (base + 0x70u != 0)
        setLookAtOffset(camera, base + 0x70u);
    if (base + 0x64u != 0)
        setOffset(camera, base + 0x64u);
    if (base + 0x7cu != 0)
        setLookAtOffset(camera, base + 0x7cu);
}

// The desired position: the offset turned by the car's heading (only), added to the car's position.
void CameraCalcDesiredGoto(ptr32<_sCamera> pCamera, ptr32<_matPsx> pMat)
{
    GuestFrame frame(0x50);
    const ptr32<_matAngles> angles = frame.local<_matAngles>(0);
    const ptr32<vec32_> transformed = frame.local<vec32_>(0x10);
    _sCamera &camera = *pCamera;
    {
        const _sFPhysLibCar &physics = *camera.pTarget->pPhysics;
        const ee_float zx = -physics.mat.Zx;
        angles->head = ArcTan(ee_ftoi(physics.mat.Zz), ee_ftoi(zx));
    }
    angles->roll = 0;
    angles->pitch = 0;
    guest_call(kMat3nBuildXYZ, pMat, angles);
    Vec32MulMatPsx(ptr32<vec32_>(pCamera.address + 0x58u), pMat, transformed);
    const _sFPhysLibCar &physics = *camera.pTarget->pPhysics;
    const ee_float tz = ee_itof(transformed->z), tx = ee_itof(transformed->x), ty = ee_itof(transformed->y);
    const ee_float z = physics.pos.z + tz, x = physics.pos.x + tx, y = physics.pos.y + ty;
    camera.desiredGoto.z = ee_ftoi(z);
    camera.desiredGoto.x = ee_ftoi(x);
    camera.desiredGoto.y = ee_ftoi(y);
}

void CameraSetMatrixFromLookAt(ptr32<_sCamera> pCamera, ptr32<vec32_> pLookAt)
{
    GuestFrame frame(0xa0);
    _sCamera &camera = *pCamera;
    vec32_ &vector = *frame.local<vec32_>(0x20);
    const int32 lz = pLookAt->z, lx = pLookAt->x, ly = pLookAt->y;
    vector.x = lx - camera.pos.x;
    vector.y = ly - camera.pos.y;
    vector.z = lz - camera.pos.z;
    matrixAlong(camera, frame, 0x00, 0x20, 0x30, 0x40, 0x50);
}

void CameraPlaceAtAINode(ptr32<_sCamera> pCamera, ptr32<_sAINode> pNode, int32 height)
{
    GuestFrame frame(0xf0);
    _sCamera &camera = *pCamera;
    const ptr32<_matPsx> mat = frame.local<_matPsx>(0x10);
    const ptr32<vec32_> vec = frame.local<vec32_>(0x30);
    const ptr32<_matAngles> angles = frame.local<_matAngles>(0x40);
    const ptr32<vec32_> transformed = frame.local<vec32_>(0x50);

    camera.hasChanged = 1;
    camera.pos.x = static_cast<int32>(pNode->centre.vx) << 8;
    camera.pos.y = (static_cast<int32>(pNode->centre.vy) + height) << 8;
    camera.pos.z = static_cast<int32>(pNode->centre.vz) << 8;
    copyVec(camera.oldPos, camera.pos);
    {
        const _sFPhysLibCar &physics = *camera.pTarget->pPhysics;
        const ee_float zx = -physics.mat.Zx;
        angles->head = ArcTan(ee_ftoi(physics.mat.Zz), ee_ftoi(zx));
    }
    angles->roll = 0;
    angles->pitch = 0;
    guest_call(kMat3nBuildXYZ, mat, angles);
    Vec32MulMatPsx(ptr32<vec32_>(pCamera.address + 0x58u), mat, transformed);
    {
        const int32_t lift = camera.origLookAtOffset.y * (0x1000 - camera.inTunnelDelta);
        const _sFPhysLibCar &physics = *camera.pTarget->pPhysics;
        const ee_float tx = ee_itof(transformed->x), ty = ee_itof(transformed->y), tz = ee_itof(transformed->z);
        const ee_float x = physics.pos.x + tx, z = physics.pos.z + tz, y = physics.pos.y + ty;
        camera.desiredGoto.x = ee_ftoi(x);
        camera.lookAtOffset.y = (lift < 0 ? lift + 0xfff : lift) >> 12;
        camera.desiredGoto.y = ee_ftoi(y);
        camera.desiredGoto.z = ee_ftoi(z);
    }
    Vec32MulMatPsx(ptr32<vec32_>(pCamera.address + 0x70u), mat, vec);
    if (G(mainVarG).splitScreen != 0 && camera.near == 0)
    {
        // Two thirds (0xaaa / 0x1000) of the look-at offset in split screen.
        const int32_t x = vec->x * 0xaaa, y = vec->y * 0xaaa, z = vec->z * 0xaaa;
        vec->x = (x < 0 ? x + 0xfff : x) >> 12;
        vec->y = (y < 0 ? y + 0xfff : y) >> 12;
        vec->z = (z < 0 ? z + 0xfff : z) >> 12;
    }
    const _sFPhysLibCar &physics = *camera.pTarget->pPhysics;
    const ee_float vx = ee_itof(vec->x), vz = ee_itof(vec->z), vy = ee_itof(vec->y);
    const ee_float x = physics.pos.x + vx, z = physics.pos.z + vz, y = physics.pos.y + vy;
    const int32 px = camera.pos.x, py = camera.pos.y, pz = camera.pos.z;
    const int32 tx = ee_ftoi(x);
    camera.targetPos.z = ee_ftoi(z);
    camera.targetPos.y = ee_ftoi(y);
    camera.targetPos.x = tx;
    vec32_ &vector = *frame.local<vec32_>(0x80);
    vector.x = tx - px;
    vector.y = camera.targetPos.y - py;
    vector.z = camera.targetPos.z - pz;
    matrixAlong(camera, frame, 0x60, 0x80, 0x90, 0xa0, 0xb0);
}

void CameraSetModeDefaults(ptr32<_sCamera> pCamera)
{
    _sCamera &camera = *pCamera;
    camera.dValue = 0xe6;
    camera.hasChanged = 1;
    const uint32_t mode = static_cast<uint32_t>(camera.mode);
    if (mode >= 15u)
        return;
    GuestFrame frame(0x40);
    switch (mode)
    {
    case 0: // CAMERA_MODE_GAME_FOLLOW_PLAYER
    case 4: // CAMERA_MODE_FOLLOW
    case 7: // CAMERA_MODE_SIDE
        camera.hasChanged = 1;
        resetAngles(camera);
        camera.pTarget = objectAt(camera.cameraNo);
        break;
    case 1: // CAMERA_MODE_FRONT_END
        camera.path.numNodesOnPath = 0;
        camera.path.interp = -1;
        camera.path.curNodeOnPath = 0;
        camera.pos.z = 0;
        camera.pos.y = 0;
        camera.pos.x = 0;
        camera.rotAngles.pitch = 0;
        camera.rotAngles.head = 0;
        camera.rotAngles.roll = 0;
        break;
    case 2: // CAMERA_MODE_FREE
        camera.freeangles = camera.angles;
        break;
    case 3: // CAMERA_MODE_TRACK
    {
        const ptr32<_sObject> target = objectAt(camera.cameraNo);
        camera.pTarget = target;
        const ptr32<vec32_> lookAt = frame.local<vec32_>(0x10);
        targetPosition(*target->pPhysics, *lookAt);
        CameraSetMatrixFromLookAt(pCamera, lookAt);
        break;
    }
    case 5: // CAMERA_MODE_ORBIT
    {
        resetAngles(camera);
        const _matAngles orbit = at<_matAngles>(kOrbitAngles);
        camera.pTarget = objectAt(camera.cameraNo);
        camera.rotAngles = orbit;
        break;
    }
    case 8: // CAMERA_MODE_TRACK_ZOOM
    {
        const ptr32<_sObject> target = objectAt(camera.cameraNo);
        camera.pTarget = target;
        const ptr32<vec32_> lookAt = frame.local<vec32_>(0);
        targetPosition(*target->pPhysics, *lookAt);
        CameraSetMatrixFromLookAt(pCamera, lookAt);
        lookAt->x = lookAt->x - camera.pos.x;
        lookAt->y = lookAt->y - camera.pos.y;
        lookAt->z = lookAt->z - camera.pos.z;
        CameraSetZoomMode(pCamera, guest_call<uint32>(kVec32Length, lookAt));
        break;
    }
    case 10: // CAMERA_MODE_STATIC_ZOOM
    {
        const ptr32<_sObject> target = objectAt(camera.cameraNo);
        camera.pTarget = target;
        const ptr32<vec32_> toCar = frame.local<vec32_>(0);
        towardsCar(camera, *target->pPhysics, *toCar);
        CameraSetZoomMode(pCamera, guest_call<uint32>(kVec32Length, toCar));
        break;
    }
    case 11: // CAMERA_MODE_START_GRID
        G(cameraVarG).readySteadyGo = 0;
        camera.pTarget = objectAt(camera.cameraNo);
        break;
    default: // 6, 9, 12, 13, 14
        break;
    }
}

void CameraSetMode(ptr32<_sCamera> pCamera, int32 mode)
{
    pCamera->hasChanged = 1;
    pCamera->mode = static_cast<eCameraMode>(mode);
    CameraSetModeDefaults(pCamera);
}

// The camera button: the next mode; after the last of the follow modes, back to the follow camera at the next distance
// (the middle one is skipped in split screen). Not during the start-grid fly-by.
void CameraNextMode(ptr32<_sCamera> pCamera)
{
    _sCamera &camera = *pCamera;
    camera.hasChanged = 1;
    const int32_t mode = static_cast<int32_t>(camera.mode);
    if (mode == 11)
        return;
    camera.mode = static_cast<eCameraMode>(mode + 1);
    if (mode + 1 == 1)
    {
        camera.near = camera.near + 1;
        if (camera.near >= 3)
            camera.near = 0;
        if (G(mainVarG).splitScreen != 0 && camera.near == 1)
            camera.near = 2;
        CameraSetNear(pCamera);
        camera.mode = static_cast<eCameraMode>(0);
    }
    CameraSetModeDefaults(pCamera);
}

void CameraLvlInitCamera(ptr32<_sCamera> pCamera, int32 num, int32 i)
{
    GuestFrame frame(0x80);
    _sCamera &camera = *pCamera;
    vec32_ &offset = *frame.local<vec32_>(0);
    vec32_ &lookAtOffset = *frame.local<vec32_>(0x10);
    const vec32_ &pos = *frame.local<vec32_>(0x20); // never set: whatever the stack held

    MemBlockSet32(pCamera.cast<void>(), 0, 0x5b8);
    camera.hasChanged = 1;
    camera.lookBackwards = 0;
    camera.lookBackwardsOldState = 0;
    camera.rayCollided = 0;
    camera.collided = 0;
    camera.isSplit = num;
    camera.near = 0;
    if (num != 0)
        camera.near = 2;
    CameraSetNear(pCamera);
    const int32_t near = camera.near;
    offset.y = 0x9600;
    offset.x = 0;
    if (near == 0)
        offset.z = -0x1a400;
    else if (near == 1)
        offset.z = -70000;
    else if (near == 2)
        offset.z = -0x24400;
    int32_t forwards = offset.z >> 8;
    if (forwards < 0)
        forwards = -forwards;
    camera.forwardsZ = forwards;
    lookAtOffset.x = 0;
    lookAtOffset.y = near == 1 ? 0x6590 : 0x5a00;
    lookAtOffset.z = 0x1400;
    camera.path.interp = -1;
    camera.path.pNodes = ptr32<_sCameraPathNode>(pCamera.address + 0xdcu);
    camera.forwardsZ = forwards;
    camera.mode = static_cast<eCameraMode>(11);
    camera.inTunnel = 0;
    camera.inTunnelDelta = 0;
    camera.feFunctionCallBackNode.address = 0;
    camera.feFunctionCallBack.address = 0;
    camera.pStartGridPath = nullptr;
    camera.path.numNodesOnPath = 0;
    camera.path.curNodeOnPath = 0;
    copyVec(camera.pos, pos);
    camera.cameraNo = i;
    camera.angles.roll = 0;
    camera.angles.head = 0;
    camera.angles.pitch = 0;
    copyVec(camera.lookAtOffset, lookAtOffset);
    copyVec(camera.origLookAtOffset, lookAtOffset);
    MatPsxInit(ptr32<_matPsx>(pCamera.address + 0x24u));
    guest_call(kMat3nBuild, ptr32<_mat3n>(pCamera.address + 0x24u), ptr32<_matAngles>(pCamera.address + 0x44u));
    camera.pTarget = nullptr;
    copyVec(camera.offset, offset);
    copyVec(camera.origOffset, camera.offset);
    camera.dValue = 0xe6;
    CameraSetModeDefaults(pCamera);
    camera.spinCounter = 0;
    camera.pStartGridPath = nullptr;
    camera.forceInTunnel = 0;
    camera.timeCounter = 0;
    camera.backwardsCount = 0;
}

void CameraLvlInit(int32 num)
{
    GuestFrame frame(0x70);
    sCameraClass &cameras = G(cameraVarG);
    cameras.readySteadyGo = 0;
    for (int32_t i = 0; i <= num; ++i)
    {
        const ptr32<_sCamera> pCamera(kCameras + static_cast<uint32_t>(i) * kCameraSize);
        cameras.pCamera = pCamera;
        CameraLvlInitCamera(pCamera, num, i);
        const mainClass &main = G(mainVarG);
        if (main.currentDo == 1 || main.level >= 0x62)
            continue;
        const ptr32<_sLevelData> level = G(pLvlG);
        ptr32<sCameraPath> path = nullptr;
        if (level && level->pPaths)
            path = *level->pPaths;
        if (path && G(mainVarG).status != 0x10)
        {
            pCamera->pStartGridPath = path;
            CameraSetStartGridPath(pCamera);
        }
        else
        {
            CameraSetMode(pCamera, 0);
            copyVec(pCamera->oldPos, pCamera->pos);
            CameraFollowPlayer(pCamera, 1, 0);
        }
    }
}

// The fly-by's end: the saved follow camera back, and the race may start.
void CameraEndOfStartGridPathCallBack(ptr32<_sCamera> pCamera)
{
    copyCamera(pCamera.address, copyCamAt(pCamera->cameraNo).address);
    pCamera->mode = static_cast<eCameraMode>(0);
    G(cameraVarG).readySteadyGo = 1;
    pCamera->pTarget = objectAt(pCamera->cameraNo);
}

// The fly-by's first node reached: the last four nodes (the path's end) become the follow camera's current view.
void CameraStartGridHitFirstNodeCallBack(ptr32<_sCamera> pCamera, uint32 nodeNum)
{
    (void)nodeNum;
    const uint32_t ra = getRegU32(&g_runtime->eeScheduler().runningContext(), 31);
    const ptr32<sCameraPath> path = pCamera->pStartGridPath;
    const auto call = [&](uint32_t k, uint32_t returnAddress)
    {
        const uint32_t length = path->pathLength;
        const uint32_t node = pCamera->path.pNodes.address + length * 0x24u + k * 0x24u;
        tagToPathNode(path, static_cast<int32>(path->pathLength) - 1, ptr32<_sCameraPathNode>(node), pCamera, returnAddress);
    };
    {
        GuestFrame frame(0x40);
        call(0, 0x112fb4);
        call(1, 0x112fdc);
        call(2, 0x113004);
    }
    call(3, ra); // a tail call, from the caller's frame
}

void CameraSetNewPath(ptr32<_sCamera> pCamera, uint32 fp, uint32 fpNode, ptr32<sCameraPath> pPath)
{
    static const uint32_t kLoopReturns[4] = {0x11643c, 0x11646c, 0x11649c, 0x1164cc};
    static const uint32_t kEndReturns[3] = {0x116508, 0x116530, 0x116558};
    _sCamera &camera = *pCamera;
    GuestFrame frame(0x50);
    camera.feFunctionCallBack.address = fp;
    camera.feFunctionCallBackNode.address = fpNode;
    camera.path.curNodeOnPath = 0;
    camera.path.numNodesOnPath = pPath->pathLength + 3;
    tagToPathNode(pPath, 0, camera.path.pNodes, pCamera, 0x116410);
    uint32_t length = pPath->pathLength;
    for (int32_t i = 0; i < static_cast<int32_t>(length); ++i)
    {
        tagToPathNode(pPath, static_cast<int16>(i), ptr32<_sCameraPathNode>(camera.path.pNodes.address + 0x24u * (i + 1)), pCamera,
                      kLoopReturns[i & 3]);
        length = pPath->pathLength;
    }
    for (uint32_t k = 1; k <= 3; ++k)
    {
        const uint32_t at = (k == 1 ? length : pPath->pathLength) + k;
        tagToPathNode(pPath, static_cast<int32>(pPath->pathLength) - 1, ptr32<_sCameraPathNode>(camera.path.pNodes.address + 0x24u * at),
                      pCamera, kEndReturns[k - 1]);
    }
    camera.path.interp = 0;
    camera.path.curNodeOnPath = 1;
}

// Saves the follow camera (copyCamG) and starts the start-grid fly-by.
void CameraSetStartGridPath(ptr32<_sCamera> pCamera)
{
    {
        GuestFrame frame(0x40);
        copyCamera(copyCamAt(pCamera->cameraNo).address, pCamera.address);
        const ptr32<_sCamera> copy = copyCamAt(pCamera->cameraNo);
        copy->mode = static_cast<eCameraMode>(0);
        CameraModeDefaultsFollowPlayer(copy);
        pCamera->mode = static_cast<eCameraMode>(11);
    }
    CameraSetNewPath(pCamera, kEndOfStartGridPathCallBack, kStartGridHitFirstNodeCallBack, pCamera->pStartGridPath);
}

// The fly-by: the saved follow camera keeps following (so the hand-over is smooth), the camera follows the path.
void CameraStartGrid(ptr32<_sCamera> pCamera)
{
    {
        GuestFrame frame(0x20);
        const ptr32<_sCamera> copy = copyCamAt(pCamera->cameraNo);
        copy->mode = static_cast<eCameraMode>(0);
        copyVec(copy->oldPos, copy->pos);
        CameraFollowPlayer(copyCamAt(pCamera->cameraNo), 1, 0);
    }
    CameraFollowPath(pCamera);
}

namespace {

// CameraTagToPathNode with the return address its original saves in its frame (sd $ra, 0x670): the word is stale data
// for the next frames there (CameraLvlInit's second camera reads it as its position), so the native one leaves it too.
void tagToPathNode(ptr32<sCameraPath> pPath, int32 pathPos, ptr32<_sCameraPathNode> pNode, ptr32<_sCamera> pCamera, uint32_t ra)
{
    GuestFrame frame(0x680);
    at<uint64_t>(frame.local<uint64_t>(0x670).address) = ra;
    const int16 speed = static_cast<int16>(pPath->pathSpeed);
    const int32_t index = static_cast<int16>(pathPos);
    const int32_t tag = at<int32_t>(pPath.address + 8u + static_cast<uint32_t>(index) * 4u);
    const bool fromPlayer = tag <= 0x0fffffff;
    const ptr32<_sCamera> cam = frame.local<_sCamera>(0x20);
    ptr32<_sSingleCamera> source = frame.local<_sSingleCamera>(0); // playerPos: only x, y, z are set
    if (!fromPlayer)
        source = guest_call<ptr32<_sSingleCamera>>(kGetCameraFromTag, tag, G(pLvlG)->pCameras);
    else
    {
        CameraLvlInitCamera(cam, G(mainVarG).splitScreen, pCamera->cameraNo);
        CameraSetMode(cam, 0);
        copyVec(pCamera->oldPos, pCamera->pos);
        CameraFollowPlayer(cam, 1, 0);
        source->x = cam->pos.x;
        source->y = cam->pos.y;
        source->z = cam->pos.z;
    }
    _sCameraPathNode &node = *pNode;
    const int32 x = source->x;
    node.pos.x = x;
    node.angles.pitch = static_cast<int16>(source->pitch);
    const int32 y = source->y;
    node.pos.y = y;
    const int16 heading = static_cast<int16>(source->heading);
    node.angles.head = heading;
    const int32 z = source->z;
    node.angles.roll = 0;
    node.pos.z = z;
    if (fromPlayer)
    {
        const _mat3n &m = cam->matrix.mat;
        node.lookAtPos.x = x + ((m.Xz * 0x78000) >> 12);
        node.lookAtPos.y = y + ((m.Yz * 0x78000) >> 12);
        node.lookAtPos.z = z + ((m.Zz * 0x78000) >> 12);
    }
    else
    {
        node.angles.head = static_cast<int16>(-heading);
        const ptr32<_mat3n> m = frame.local<_mat3n>(0x5e0);
        guest_call(kMat3nBuild, m, ptr32<_matAngles>(pNode.address + 0x18u));
        const int32 nx = node.pos.x, ny = node.pos.y, nz = node.pos.z;
        node.lookAtPos.x = nx + ((m->Zx * 0x78000) >> 12);
        node.lookAtPos.y = ny + ((m->Zy * 0x78000) >> 12);
        node.lookAtPos.z = nz + ((m->Zz * 0x78000) >> 12);
    }
    node.speed = speed;
}

} // namespace

void CameraTagToPathNode(ptr32<sCameraPath> pPath, int32 pathPos, ptr32<_sCameraPathNode> pNode, ptr32<_sCamera> pCamera)
{
    tagToPathNode(pPath, pathPos, pNode, pCamera, getRegU32(&g_runtime->eeScheduler().runningContext(), 31));
}

// The point at t (0 to 0x1000) of the current spline segment.
void SplinePoint(ptr32<_sCamera> pCamera, int32_t t, ptr32<vec32_> pPoint, bool_ angles)
{
    const uint32_t points = pCamera.address + (angles ? 0x544u : 0x514u);
    const int32_t square = t * t;
    const int64_t t2 = (square < 0 ? square + 0xfff : square) >> 12;
    const int64_t t3 = q12(mul64(t2, t));
    for (uint32_t axis = 0; axis < 3; ++axis)
    {
        const uint32_t p = points + 4u * axis;
        at<int32_t>(pPoint.address + 4u * axis) =
            splineAxis(at<int32_t>(p), at<int32_t>(p + 12), at<int32_t>(p + 24), at<int32_t>(p + 36), t, t2, t3);
    }
}

// The spline coefficients of segment n from the positions (or the look-at points) of nodes n-1 to n+2.
void ComputeCoeffs(ptr32<_sCamera> pCamera, int32_t n, bool_ angles)
{
    const uint32_t nodes = pCamera->path.pNodes.address;
    const uint32_t field = angles ? 0xcu : 0u;
    const uint32_t out = pCamera.address + (angles ? 0x544u : 0x514u);
    for (uint32_t axis = 0; axis < 3; ++axis)
        coeffAxis(nodes, n, field + 4u * axis, out + 4u * axis);
}

void CameraFollowPath(ptr32<_sCamera> pCamera)
{
    _sCamera &camera = *pCamera;
    if (camera.path.interp == -1)
        return;
    GuestFrame frame(0x50);
    const ptr32<vec32_> lookAt = frame.local<vec32_>(0);
    ComputeCoeffs(pCamera, camera.path.curNodeOnPath, 0);
    SplinePoint(pCamera, camera.path.interp, ptr32<vec32_>(pCamera.address + 0xcu), 0);
    ComputeCoeffs(pCamera, camera.path.curNodeOnPath, 1);
    SplinePoint(pCamera, camera.path.interp, lookAt, 1);
    CameraSetMatrixFromLookAt(pCamera, lookAt);
    int32_t current = camera.path.curNodeOnPath;
    const ptr32<_sCameraPathNode> node(camera.path.pNodes.address + static_cast<uint32_t>(current * 0x24));
    const int32_t interp = camera.path.interp + node->speed;
    camera.path.interp = interp;
    if (interp <= 0x1000)
        return;
    camera.path.interp = node->speed;
    if (camera.feFunctionCallBackNode.address != 0)
    {
        guest_call(camera.feFunctionCallBackNode.address, pCamera, current);
        current = camera.path.curNodeOnPath;
    }
    int32_t next = current + 1;
    if (!(current + 1 < camera.path.numNodesOnPath - 3) && camera.feFunctionCallBack.address != 0)
    {
        guest_call(camera.feFunctionCallBack.address, pCamera);
        camera.path.interp = -1;
        next = camera.path.curNodeOnPath + 1;
    }
    camera.path.curNodeOnPath = next;
}

void CameraSpin(ptr32<_sCamera> pCamera)
{
    _sCamera &camera = *pCamera;
    camera.angles.pitch = static_cast<int16>(camera.angles.pitch + camera.rotAngles.pitch);
    camera.angles.head = static_cast<int16>(camera.angles.head + camera.rotAngles.head);
    camera.angles.roll = static_cast<int16>(camera.angles.roll + camera.rotAngles.roll);
    guest_call(kMat3nBuildXYZ, ptr32<_mat3n>(pCamera.address + 0x24u), ptr32<_matAngles>(pCamera.address + 0x44u));
}

// The debug fly camera: the right stick and the d-pad turn it (faster the longer a key is held), the other keys move it.
void CameraFree(ptr32<_sCamera> pCamera)
{
    GuestFrame frame(0x80);
    _sCamera &camera = *pCamera;
    vec32_ &direction = *frame.local<vec32_>(0);
    direction.x = direction.y = direction.z = 0;
    bool angleChanged = false, positionChanged = false;
    const inputClass &input = G(inputVarG);
    const int32_t stickX = input.analog2A;
    if ((stickX < 0 ? -stickX : stickX) > 0x50)
        camera.freeangles.head = static_cast<int16>(camera.freeangles.head - stickX * 2);
    const int32_t stickY = G(inputVarG).analog2B;
    if ((stickY < 0 ? -stickY : stickY) > 0x50)
        camera.freeangles.pitch = static_cast<int16>(camera.freeangles.pitch + stickY * 2);
    uint32_t scale = 2;
    const bool strafe = InputHeld(0x10) != 0;
    if (!strafe)
    {
        if (InputHeld(0x14))
        {
            angleChanged = true;
            camera.freeangles.pitch = turn(camera.freeangles.pitch, grow(0x327cecu), true);
        }
        else
            G(lengthHeld1) = F(0.0);
        if (InputHeld(0x16))
        {
            angleChanged = true;
            camera.freeangles.pitch = turn(camera.freeangles.pitch, grow(0x327cf0u), false);
        }
        else
            G(lengthHeld2) = F(0.0);
        if (InputHeld(0x17))
        {
            angleChanged = true;
            camera.freeangles.head = turn(camera.freeangles.head, grow(0x327cf4u), true);
        }
        else
            G(lengthHeld3) = F(0.0);
        if (InputHeld(0x15))
        {
            angleChanged = true;
            camera.freeangles.head = turn(camera.freeangles.head, grow(0x327cf8u), false);
        }
        else
            G(lengthHeld4) = F(0.0);
    }
    if (InputHeld(0x1f))
    {
        const ee_float length = G(lengthHeld5) + kHeldStep;
        G(lengthHeld5) = length;
        scale = guest_call<uint32>(kFpToUi, length + F(5.0));
    }
    else
        G(lengthHeld5) = F(0.0);
    if (InputHeld(0x1d))
        scale = 1;
    if (InputHeld(0x19) || (strafe && InputHeld(0x15)))
    {
        positionChanged = true;
        direction.x = direction.x + static_cast<int32>(scale * 0xa00u);
    }
    if (InputHeld(0x18) || (strafe && InputHeld(0x17)))
    {
        positionChanged = true;
        direction.x = direction.x - static_cast<int32>(scale * 0xa00u);
    }
    if (InputHeld(0x1e))
    {
        ee_float &length = grow(0x327d00u);
        const ee_float step = (length + G(lengthHeld5)) * F(2560.0);
        direction.z = ee_ftoi(ee_itof(direction.z) + step);
        positionChanged = true;
    }
    else
        G(lengthHeld6) = F(1.0);
    if (InputHeld(0x1c))
    {
        positionChanged = true;
        direction.z = direction.z - static_cast<int32>(scale * 0xa00u);
    }
    if (InputHeld(0x1b) || (strafe && InputHeld(0x16)))
        camera.pos.y = camera.pos.y - static_cast<int32>(scale * 0xa00u);
    if (InputHeld(0x1a) || (strafe && InputHeld(0x14)))
        camera.pos.y = camera.pos.y + static_cast<int32>(scale * 0xa00u);
    if (angleChanged)
        guest_call(kMat3nBuildXYZ, ptr32<_mat3n>(pCamera.address + 0x24u), ptr32<_matAngles>(pCamera.address + 0xa0u));
    if (positionChanged)
    {
        const ptr32<vec32_> result = frame.local<vec32_>(0x10);
        Vec32MulMatPsxT(frame.local<vec32_>(0), ptr32<_matPsx>(pCamera.address + 0x24u), result);
        camera.pos.x = camera.pos.x + result->x;
        camera.pos.y = camera.pos.y + result->y;
        camera.pos.z = camera.pos.z + result->z;
    }
}

// The debug orbit camera: turns around the car with the right stick and the d-pad.
void CameraOrbit(ptr32<_sCamera> pCamera)
{
    GuestFrame frame(0x70);
    _sCamera &camera = *pCamera;
    const ptr32<_sObject> target = camera.pTarget;
    const ptr32<_sFPhysLibCar> physics = target->pPhysics;
    const int32_t stickX = G(inputVarG).analog2A;
    if ((stickX < 0 ? -stickX : stickX) > 0x50)
        camera.rotAngles.head = static_cast<int16>(camera.rotAngles.head - stickX);
    const int32_t stickY = G(inputVarG).analog2B;
    if ((stickY < 0 ? -stickY : stickY) > 0x50)
        camera.rotAngles.pitch = static_cast<int16>(camera.rotAngles.pitch + stickY);
    if (InputHeld(0x14))
        camera.rotAngles.pitch = static_cast<int16>(camera.rotAngles.pitch - 10);
    else if (InputHeld(0x16))
        camera.rotAngles.pitch = static_cast<int16>(camera.rotAngles.pitch + 10);
    if (InputHeld(0x17))
        camera.rotAngles.head = static_cast<int16>(camera.rotAngles.head - 10);
    else if (InputHeld(0x15))
        camera.rotAngles.head = static_cast<int16>(camera.rotAngles.head + 10);
    const int16 pitch = static_cast<int16>(camera.angles.pitch + camera.rotAngles.pitch);
    const int16 head = static_cast<int16>(camera.angles.head + camera.rotAngles.head);
    const int16 roll = static_cast<int16>(camera.angles.roll + camera.rotAngles.roll);
    camera.angles.pitch = pitch;
    camera.angles.head = head;
    camera.angles.roll = roll;
    const _matAngles &carAngles = target->pRendObj->angles;
    _matAngles &angles = *frame.local<_matAngles>(0);
    vec32_ &targetPos = *frame.local<vec32_>(0x10);
    targetPosition(*physics, targetPos);
    angles.pitch = static_cast<int16>(pitch - carAngles.pitch);
    angles.head = static_cast<int16>(head - carAngles.head);
    angles.roll = static_cast<int16>(roll - carAngles.roll);
    guest_call(kMat3nBuildXYZ, ptr32<_mat3n>(pCamera.address + 0x24u), frame.local<_matAngles>(0));
    Vec32MulMatPsxT(ptr32<vec32_>(pCamera.address + 0x58u), ptr32<_matPsx>(pCamera.address + 0x24u), ptr32<vec32_>(pCamera.address + 0x38u));
    camera.pos.x = camera.matrix.trans.x + targetPos.x;
    camera.pos.y = camera.matrix.trans.y + targetPos.y;
    camera.pos.z = camera.matrix.trans.z + targetPos.z;
}

// Runs each camera's mode once a frame (split + 1 cameras) and sets the field of view from the last one's zoom.
void CameraProcess(int32 time, int32 split)
{
    (void)time;
    R5900Context &ctx = g_runtime->eeScheduler().runningContext();
    ptr32<_sCamera> pCamera(getRegU32(&ctx, 18)); // with no camera to run, the caller's $s2
    {
        GuestFrame frame(0xc0);
        sCameraClass &cameras = G(cameraVarG);
        for (int32_t i = 0; i <= split; ++i)
        {
            pCamera = ptr32<_sCamera>(kCameras + static_cast<uint32_t>(i) * kCameraSize);
            cameras.pCamera = pCamera;
            _sCamera &camera = *pCamera;
            const ptr32<vec32_> targetPos = frame.local<vec32_>(0);
            switch (static_cast<uint32_t>(camera.mode))
            {
            case 0: // CAMERA_MODE_GAME_FOLLOW_PLAYER
            {
                const bool_ backwards = camera.lookBackwards;
                if (camera.lookBackwardsOldState != backwards)
                    camera.hasChanged = 1;
                camera.lookBackwardsOldState = backwards;
                CameraFollowPlayer(pCamera, 0, 1);
                break;
            }
            case 1: // CAMERA_MODE_FRONT_END
                CameraFollowPath(pCamera);
                break;
            case 2: // CAMERA_MODE_FREE
            case 13: // CAMERA_MODE_FREE_EDITOR
                if (camera.mode == 13 || split == 0)
                    CameraFree(pCamera);
                break;
            case 3: // CAMERA_MODE_TRACK
                targetPosition(*camera.pTarget->pPhysics, *targetPos);
                CameraSetMatrixFromLookAt(pCamera, targetPos);
                break;
            case 4: // CAMERA_MODE_FOLLOW: the car's own matrix (its integer state's, with the 12 bytes after it)
            {
                const _sFPhysLibCar &physics = *camera.pTarget->pPhysics;
                const uint32_t source = physics.pInteger.address + 0x28u;
                const uint32_t matrix = pCamera.address + 0x24u;
                targetPosition(physics, *targetPos);
                // sMat is row-major, mat3n column-major: the copy transposes.
                static const uint32_t kFrom[9] = {0x0, 0x6, 0xc, 0x2, 0x8, 0xe, 0x4, 0xa, 0x10};
                for (uint32_t k = 0; k < 9; ++k)
                    at<uint16_t>(matrix + 2 * k) = at<uint16_t>(source + kFrom[k]);
                for (uint32_t k = 0; k < 3; ++k)
                    at<uint32_t>(matrix + 0x14 + 4 * k) = at<uint32_t>(source + 0x14 + 4 * k);
                Vec32MulMatPsxT(ptr32<vec32_>(pCamera.address + 0x58u), ptr32<_matPsx>(matrix), ptr32<vec32_>(pCamera.address + 0x38u));
                camera.pos.x = camera.matrix.trans.x + targetPos->x;
                camera.pos.y = camera.matrix.trans.y + targetPos->y;
                camera.pos.z = camera.matrix.trans.z + targetPos->z;
                break;
            }
            case 5: // CAMERA_MODE_ORBIT
                if (split == 0)
                    CameraOrbit(pCamera);
                break;
            case 7: // CAMERA_MODE_SIDE: looking at the car's side
            {
                const _sFPhysLibCar &physics = *camera.pTarget->pPhysics;
                const _sFMat &m = physics.mat;
                _mat3n &r = camera.matrix.mat;
                r.Xx = static_cast<int16>(ee_ftoi(-m.Zx));
                r.Yx = static_cast<int16>(ee_ftoi(m.Yx));
                r.Zx = static_cast<int16>(ee_ftoi(m.Xx));
                r.Xy = static_cast<int16>(ee_ftoi(-m.Zy));
                r.Yy = static_cast<int16>(ee_ftoi(m.Yy));
                r.Zy = static_cast<int16>(ee_ftoi(m.Xy));
                r.Xz = static_cast<int16>(ee_ftoi(-m.Zz));
                r.Yz = static_cast<int16>(ee_ftoi(m.Yz));
                r.Zz = static_cast<int16>(ee_ftoi(m.Xz));
                targetPosition(physics, *targetPos);
                Vec32MulMatPsxT(ptr32<vec32_>(pCamera.address + 0x58u), ptr32<_matPsx>(pCamera.address + 0x24u), ptr32<vec32_>(pCamera.address + 0x38u));
                camera.pos.x = camera.matrix.trans.x + targetPos->x;
                camera.pos.y = camera.matrix.trans.y + targetPos->y;
                camera.pos.z = camera.matrix.trans.z + targetPos->z;
                break;
            }
            case 8: // CAMERA_MODE_TRACK_ZOOM
                targetPosition(*camera.pTarget->pPhysics, *targetPos);
                CameraSetMatrixFromLookAt(pCamera, targetPos);
                targetPos->x = targetPos->x - camera.pos.x;
                targetPos->y = targetPos->y - camera.pos.y;
                targetPos->z = targetPos->z - camera.pos.z;
                CameraSetZoomMode(pCamera, guest_call<uint32>(kVec32Length, targetPos));
                break;
            case 10: // CAMERA_MODE_STATIC_ZOOM
                towardsCar(camera, *camera.pTarget->pPhysics, *targetPos);
                CameraSetZoomMode(pCamera, guest_call<uint32>(kVec32Length, targetPos));
                break;
            case 11: // CAMERA_MODE_START_GRID: a button skips the fly-by once past its first node
            {
                const bool skip = guest_call<bool_>(kInputHit, 0xe) != 0 || guest_call<bool_>(kInputHit, 0x1e) != 0;
                if (skip && camera.path.curNodeOnPath >= 2)
                {
                    if (G(mainVarG).splitScreen == 1)
                    {
                        const ptr32<_sCamera> other(camera.cameraNo == 0 ? kCameras + kCameraSize : kCameras);
                        CameraSetMode(other, 0);
                        copyVec(other->oldPos, other->pos);
                        CameraFollowPlayer(other, 1, 0);
                    }
                    CameraSetMode(pCamera, 0);
                    copyVec(camera.oldPos, camera.pos);
                    CameraFollowPlayer(pCamera, 1, 0);
                    G(cameraVarG).readySteadyGo = 1;
                }
                else
                    CameraStartGrid(pCamera);
                break;
            }
            case 12: // CAMERA_MODE_TRACK_EDITOR
            {
                camera.freeangles.pitch = static_cast<int16>(CameraDampingProcess(camera.freeangles.pitch));
                camera.freeangles.roll = static_cast<int16>(CameraDampingProcess(camera.freeangles.roll));
                const int32 head = CameraDampingProcess(camera.freeangles.head);
                camera.angles.pitch = static_cast<int16>(camera.rotAngles.pitch + camera.freeangles.pitch);
                camera.angles.roll = static_cast<int16>(camera.rotAngles.roll + camera.freeangles.roll);
                camera.angles.head = static_cast<int16>(camera.rotAngles.head + head);
                camera.freeangles.head = static_cast<int16>(head);
                guest_call(kMat3nBuildXYZ, ptr32<_mat3n>(pCamera.address + 0x24u), ptr32<_matAngles>(pCamera.address + 0x44u));
                Vec32MulMatPsxT(ptr32<vec32_>(pCamera.address + 0x58u), ptr32<_matPsx>(pCamera.address + 0x24u), ptr32<vec32_>(pCamera.address + 0xcu));
                camera.targetOffset.x = CameraDampingProcess(camera.targetOffset.x);
                camera.targetOffset.y = CameraDampingProcess(camera.targetOffset.y);
                const int32 z = CameraDampingProcess(camera.targetOffset.z);
                const int32 lx = camera.desiredGoto.x + camera.targetOffset.x;
                const int32 ly = camera.desiredGoto.y + camera.targetOffset.y;
                const int32 lz = camera.desiredGoto.z + z;
                camera.pos.x = camera.pos.x + lx;
                camera.pos.y = camera.pos.y + ly;
                camera.pos.z = camera.pos.z + lz;
                camera.targetOffset.z = z;
                camera.lookAtOffset.x = lx;
                camera.lookAtOffset.y = ly;
                camera.lookAtOffset.z = lz;
                break;
            }
            case 16: // CAMERA_MODE_SPIN
                CameraSpin(pCamera);
                break;
            default: // 6, 9, 14, 15, 17 (the menu camera clears a vector it does not use) and beyond
                break;
            }
            camera.matrix.trans.x = camera.pos.x >> 8;
            camera.matrix.trans.y = camera.pos.y >> 8;
            camera.matrix.trans.z = camera.pos.z >> 8;
        }
    }
    _sCamera &last = *pCamera;
    if (last.mode != 2)
        last.freeangles = last.angles;
    G(lsVarG).fov = (F(60.0) - ee_itof(last.dValue) * kFovScale) + F(5.0);
}

// The follow camera (7,216 bytes, CAMERA's largest) is not decompiled yet: native callers reach the original through the
// dispatcher. Not registered.
void CameraFollowPlayer(ptr32<_sCamera> pCamera, bool_ goStraightThere, bool_ doCollision)
{
    guest_call(0x113818, pCamera, goStraightThere, doCollision);
}

// The camera's own ray cast (a copy of the collision code's, which also ignores the material 0x1a polys of the
// scifi level's door while its trigger is shut): the nearest hit of a segment on the polys of one cell.
bool_ CameraCellCastCell(ptr32<_sColGridCellPSX> pCell, ptr32<_sColCellVecPSX> pStart, ptr32<_sColCellVecPSX> pEnd,
                         ptr32<_sColCellVecPSX> pPoi, ptr32<_sColNormalPSX> pNormal)
{
    GuestFrame frame(0xf0);
    const _sColGridPSX &grid = *G(acolG).pGrid;
    const ptr32<_sColCellVecPSX> s = frame.local<_sColCellVecPSX>(0), e = frame.local<_sColCellVecPSX>(0x10);
    _sColCellVecPSX &min = *frame.local<_sColCellVecPSX>(0x20), &max = *frame.local<_sColCellVecPSX>(0x30);
    const ptr32<_sColCellVecPSX> poi = frame.local<_sColCellVecPSX>(0x40);
    const int32_t count = static_cast<int32_t>(pCell->list.num[0]);
    {
        _sColCellVecPSX start, end;
        std::memcpy(&start, guest_host(pStart.address), 16);
        std::memcpy(&end, guest_host(pEnd.address), 16);
        std::memcpy(s.get(), &start, 16);
        std::memcpy(e.get(), &end, 16);
    }
    const uint32_t ids = grid.pIds.address + pCell->list.offset * 4u;
    const auto bounds = [&]
    {
        const _sColCellVecPSX &a = *s, &b = *e;
        const ee_float maxX = std::max(b.x.v, a.x.v), minX = std::min(b.x.v, a.x.v);
        const ee_float maxY = std::max(b.y.v, a.y.v), minY = std::min(b.y.v, a.y.v);
        const ee_float maxZ = std::max(b.z.v, a.z.v), minZ = std::min(b.z.v, a.z.v);
        min.x = minX;
        min.y = minY;
        max.x = maxX;
        min.z = minZ;
        max.y = maxY;
        max.z = maxZ;
    };
    bounds();
    bool collided = false;
    for (int32_t i = 0; i < count;)
    {
        const uint32_t id = at<uint32_t>(ids + 4u * i);
        ++i;
        const ptr32<_sColPolyPSX> polyPtr(grid.pPoly.address + id * 0x60u);
        const _sColPolyPSX &poly = *polyPtr;
        if (poly.v[0].y < min.y)
            break; // sorted by height: the rest are lower still
        if (poly.v[2].pad < min.x || max.x < poly.v[1].pad || poly.bbMaxZ < min.z || max.z < poly.v[3].pad ||
            max.y < poly.v[0].pad)
            continue;
        if (poly.mat == 0x1a && G(carColG).triggerState[0] == 1)
            continue;
        if (!AColRay_Poly_1Way(s, e, polyPtr, poi))
            continue;
        std::memcpy(e.get(), poi.get(), 16);
        collided = true;
        bounds();
        std::memcpy(guest_host(pPoi.address), poi.get(), 16);
        uint8_t normal[16];
        std::memcpy(normal, &poly.n, 16);
        std::memcpy(guest_host(pNormal.address), normal, 16);
    }
    return collided ? 1 : 0;
}

// The nearest hit of a segment on the level, over the rectangle of cells it spans.
bool_ CameraGeneralRayCast(ptr32<_sColCellVecPSX> pS, ptr32<_sColCellVecPSX> pE, ptr32<_sColCellVecPSX> pPoi, ptr32<_sColNormalPSX> pNormal)
{
    GuestFrame frame(0x100);
    const ptr32<_sColGridPSX> gridPtr = G(acolG).pGrid;
    const ptr32<int32> cellX = frame.local<int32>(0), cellZ = frame.local<int32>(0x10);
    const ptr32<_sColCellVecPSX> s16 = frame.local<_sColCellVecPSX>(0x20), e16 = frame.local<_sColCellVecPSX>(0x30);
    const ptr32<_sColCellVecPSX> poi = frame.local<_sColCellVecPSX>(0x40);
    const ee_float sz = pS->z, sx = pS->x, sy = pS->y;
    const ee_float ex = pE->x, ey = pE->y, ez = pE->z;
    s16->y = sy;
    e16->x = ex;
    e16->y = ey;
    e16->z = ez;
    s16->x = sx;
    s16->z = sz;
    AColCellXZFromPos(gridPtr, sx, sz, cellX, cellZ);
    AColCellXZFromPos(gridPtr, e16->x, e16->z, cellX + 1, cellZ + 1);
    const int32_t xs = cellX[0], xe = cellX[1], zs = cellZ[0], ze = cellZ[1];
    int32_t lastX = xe < xs ? xs : xe, lastZ = ze < zs ? zs : ze;
    int32_t firstX = xs < xe ? xs : xe, firstZ = zs < ze ? zs : ze;
    const _sColGridPSX &grid = *gridPtr;
    const auto clamp = [](int32_t v, int32_t n) { return v < 0 ? 0 : (v < n ? v : n - 1); };
    firstX = clamp(firstX, grid.numCellsX);
    firstZ = clamp(firstZ, grid.numCellsZ);
    lastX = clamp(lastX, grid.numCellsX);
    lastZ = clamp(lastZ, grid.numCellsZ);
    bool collided = false;
    for (int32_t x = firstX; x <= lastX; ++x)
        for (int32_t z = firstZ; z <= lastZ; ++z)
        {
            const int32 id = AColGetCellIdFromXZ(gridPtr, x, z);
            const ptr32<_sColGridCellPSX> cell(gridPtr->pCell.address + static_cast<uint32_t>(id) * 16u);
            if (CameraCellCastCell(cell, s16, e16, poi, pNormal))
            {
                std::memcpy(e16.get(), poi.get(), 16);
                collided = true;
                std::memcpy(guest_host(pPoi.address), poi.get(), 16);
            }
        }
    return collided ? 1 : 0;
}

} // namespace rc

RC_DECOMP_FN(CameraInit, 0x117b88)
RC_DECOMP_FN(CameraTerm, 0x117b90)
RC_DECOMP_FN(CameraLvlTerm, 0x117cc0)
RC_DECOMP_FN(CameraDebugInfo, 0x117cc8)
RC_DECOMP_FN_SETUP(CameraTerminatePath, 0x117cd0, rc::fuzz::cameraOne)
RC_DECOMP_FN_SETUP(CameraSetTunnelMode, 0x117ce0, rc::fuzz::cameraOne)
RC_DECOMP_FN_SETUP(CameraUnSetTunnelMode, 0x117cf0, rc::fuzz::cameraOne)
RC_DECOMP_FN_SETUP(CameraReset, 0x118450, rc::fuzz::cameraOne)
RC_DECOMP_FN_SETUP(CameraModeDefaultsFollowPlayer, 0x117e28, rc::fuzz::cameraOne)
RC_DECOMP_FN(Linear_Interpolate, 0x117fa0)
RC_DECOMP_FN(Cosine_Interpolate, 0x117fe0)
RC_DECOMP_FN_SETUP(CameraSetZoomMode, 0x118098, rc::fuzz::cameraZoom)
RC_DECOMP_FN(CameraDampingProcess, 0x1180e0)
RC_DECOMP_FN_SETUP(CameraSetOffsetForTwoPlayerMode, 0x118130, rc::fuzz::cameraOne)
RC_DECOMP_FN_SETUP(CameraHandleTunnels, 0x118178, rc::fuzz::cameraOne)
RC_DECOMP_FN_SETUP(CameraDecelerateInterp, 0x1182e8, rc::fuzz::cameraDecelerate)
RC_DECOMP_FN_SETUP(CameraSetOffsets, 0x1183b8, rc::fuzz::cameraOffsets)
RC_DECOMP_FN_SETUP(CameraSetNear, 0x117350, rc::fuzz::cameraOne)
RC_DECOMP_FN_SETUP(CameraCalcDesiredGoto, 0x118218, rc::fuzz::cameraWithMatrix)
RC_DECOMP_FN_SETUP(CameraSetMatrixFromLookAt, 0x117e78, rc::fuzz::cameraLookAt)
RC_DECOMP_FN_SETUP(CameraPlaceAtAINode, 0x117888, rc::fuzz::cameraAINode)
RC_DECOMP_FN_SETUP(CameraSetModeDefaults, 0x117530, rc::fuzz::cameraOne)
RC_DECOMP_FN_SETUP(CameraSetMode, 0x117518, rc::fuzz::cameraMode)
RC_DECOMP_FN_SETUP(CameraNextMode, 0x117480, rc::fuzz::cameraOne)
RC_DECOMP_FN_SETUP(CameraLvlInitCamera, 0x117130, rc::fuzz::cameraLvlInit)
RC_DECOMP_FN_SETUP_HEAP(CameraLvlInit, 0x117b98, rc::fuzz::cameraLevel)
RC_DECOMP_FN_SETUP(CameraEndOfStartGridPathCallBack, 0x117cf8, rc::fuzz::cameraOne)
RC_DECOMP_FN_SETUP(CameraStartGridHitFirstNodeCallBack, 0x112f70, rc::fuzz::cameraPath)
RC_DECOMP_FN_SETUP(CameraSetNewPath, 0x1163c0, rc::fuzz::cameraNewPath)
RC_DECOMP_FN_SETUP(CameraSetStartGridPath, 0x113040, rc::fuzz::cameraPath)
RC_DECOMP_FN_SETUP(CameraStartGrid, 0x1131a8, rc::fuzz::cameraPath)
RC_DECOMP_FN_SETUP(CameraTagToPathNode, 0x1161e8, rc::fuzz::cameraTag)
RC_DECOMP_FN_SETUP(SplinePoint, 0x116580, rc::fuzz::cameraSpline)
RC_DECOMP_FN_SETUP(ComputeCoeffs, 0x1167c8, rc::fuzz::cameraCoeffs)
RC_DECOMP_FN_SETUP(CameraFollowPath, 0x117010, rc::fuzz::cameraPath)
RC_DECOMP_FN_SETUP(CameraSpin, 0x115b20, rc::fuzz::cameraOne)
RC_DECOMP_FN_SETUP(CameraFree, 0x115448, rc::fuzz::cameraPad)
RC_DECOMP_FN_SETUP(CameraOrbit, 0x115940, rc::fuzz::cameraPad)
RC_DECOMP_FN_SETUP_HEAP(CameraProcess, 0x115b60, rc::fuzz::cameraProcess)
RC_DECOMP_FN_SETUP(CameraCellCastCell, 0x113560, rc::fuzz::cameraCell)
RC_DECOMP_FN_SETUP(CameraGeneralRayCast, 0x113218, rc::fuzz::cameraRay)
