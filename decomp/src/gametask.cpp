// Decompiled from GAMETASK.c: the race's physics task. Each step (GameTaskProcess, every gameTaskTimeStep) holds the
// cars at the start and finish, runs the physics, moves the cars' collision positions, grows the metamorphosing
// cars' middle spheres, resolves collisions and runs the animations and the camera. After the collision response
// GameTaskGetShadowMaterialInfo finds each wheel's shadow height (once a car has gone unrendered for three frames,
// the fast method skips it).
//
// Written from the instructions.

#include "physics_fuzz.h"
#include "weapon.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace rc {

bool_ AColDownCast(ptr32<_sColCellVecPSX> pPos, ptr32<_sColCellVecPSX> pPoi, ptr32<_sColNormalPSX> pNormal,
                   ptr32<uint8> pMatId);                                   // collision.cpp
bool_ AColIsPositionOutsideWorld(ptr32<_sColCellVecPSX> pPos, ee_float tol); // collision.cpp
ptr32<_sVfx_Object> VfxAddNew(ptr32<_sObject> pPlayerObj, int32 iY, int32 iZ, int32 iType); // visfx12.cpp
uint16 VfxShortestLastDistRendered(ptr32<_sObject> pObj);                  // visfx.cpp
void AIRespawnFurther(ptr32<_sObject> pObj, int32 times);                  // ai.cpp
void OColProcess(tTime);                                                   // ocol.cpp
void FPhysLibUpdateCarSpheres(ptr32<_sFPhysLibCar> pCar, int32_t bFullUpdate); // physics.cpp
void CarColColDetCars();                                                   // collision.cpp
void CameraProcess(int32 time, int32 split);                               // camera.cpp
int32_t PhysicsMotion();                                                   // physics2.cpp
int32_t PhysicsCollisionResponseUpdate();

void GameTaskProcess(ptr32<void> pData, tTime time);
void GameTaskSpecialPhysicsCases();
void GameTaskUpdateCarOcols();
void GameTaskShadowCalc(ptr32<_sObject> pObject);

namespace {

using weapon::at;

constexpr uint32_t kTask = sym::GameTaskGlobals, kTimeStep = sym::gameTaskTimeStep, kGameTaskProcess = sym::GameTaskProcess, kAnimUpdateAnims = sym::AnimUpdateAnims;
constexpr uint32_t kObjectList = sym::objectVarG, kNoObjects = kObjectList + 0x170u, kMainVars = sym::mainVarG;
constexpr uint32_t kReadySteadyGo = (sym::hudVarG + 0x70u), kResultTrigger = (sym::visFxVarG + 0x2730u), kDoApplyAIBoost = sym::doApplyAIBoost_534;
constexpr uint32_t kCameraMode = sym::cameraVarG;
constexpr uint32_t kMorphScaleProcess = sym::lit4_32762c, kShadowDefault = sym::lit4_327630, kMorphScale = sym::lit4_327634;
const ee_float kToCell = ee_float(0.00390625f); // 1 / 256

ee_float fat(uint32_t address) { return at<ee_float>(address); }
uint32_t objectAt(int32_t i) { return at<uint32_t>(kObjectList + 4u * static_cast<uint32_t>(i)); }

// A metamorphosing car's middle sphere grows from its radius when it started to the collision object's over 400 ms.
void metamorphose(uint32_t obj, int32_t time, uint32_t scaleAddress)
{
    if (at<uint8_t>(obj + 0x176u) == 0)
    {
        at<int32_t>(obj + 0x1d0u) = 0;
        return;
    }
    int32_t started = at<int32_t>(obj + 0x1d0u);
    if (started < 0)
        return;
    const uint32_t phys = at<uint32_t>(obj + 0x20u);
    if (started == 0)
    {
        at<int32_t>(obj + 0x1d0u) = time;
        at<int32_t>(obj + 0x1d4u) = ee_ftoi(fat(at<uint32_t>(phys + 0x1e0u) + 0x698u));
        started = time;
    }
    const int32_t t = time - started;
    if (t >= 400)
    {
        at<int32_t>(obj + 0x1d0u) = -1;
        return;
    }
    const ee_float grown = ee_itof(t) * ee_float(256.0f);
    const ee_float target = fat(at<uint32_t>(at<uint32_t>(at<uint32_t>(obj + 0x24u) + 4u) + 0x28u)); // *pCarCol->pOcol->pRad
    const ee_float start = ee_itof(static_cast<int32_t>(static_cast<uint32_t>(at<int32_t>(obj + 0x1d4u)) * static_cast<uint32_t>(400 - t)));
    at<ee_float>(at<uint32_t>(phys + 0x1e0u) + 0x698u) = (start + target * grown) * fat(scaleAddress);
}

} // namespace

void GameTaskLvlInit()
{
    guest_call(weapon::kTaskSet, kTask, kGameTaskProcess, 0, 0);
    guest_call(weapon::kTaskCachedInsert, kTask); // a tail call
}

void GameTaskLvlTerm()
{
    guest_call(weapon::kTaskDelete, kTask); // a tail call
}

void GameTaskInit() {}

void GameTaskTerm() {}

// Each car's collision position from its render position, and a car off the world or being respawned put back.
void GameTaskUpdateCarOcols()
{
    for (int32_t i = at<int32_t>(kNoObjects) - 1; i >= 0; --i)
    {
        const uint32_t obj = objectAt(i);
        const uint32_t phys = at<uint32_t>(obj + 0x20u);
        at<ee_float>(obj + 0xbcu) = fat(phys + 8u) * kToCell;
        at<ee_float>(obj + 0xc0u) = fat(phys + 0xcu) * kToCell;
        at<ee_float>(obj + 0xc4u) = fat(phys + 0x10u) * kToCell;
        if (AColIsPositionOutsideWorld(ptr32<_sColCellVecPSX>(obj + 0xbcu), ee_float(100.0f)) && at<int8_t>(obj + 0x18au) == 0)
            VfxAddNew(ptr32<_sObject>(obj), 0, 0, 0x24);
        else if (at<int8_t>(obj + 0x18au) == 2)
        {
            const uint32_t p = at<uint32_t>(obj + 0x20u);
            at<uint8_t>(p + 0x1cdu) = at<uint8_t>(p + 0x1cdu) & 0xfdu;
            at<int32_t>(obj + 0x6cu) = 1; // oCol.enabled
            at<int8_t>(obj + 0x18au) = 1;
            AIRespawnFurther(ptr32<_sObject>(obj), 2);
        }
        at<uint32_t>(obj + 0x64u) = ((at<uint8_t>(phys + 0x1cdu) >> 1) ^ 1u) & 1u; // oCol.updated
    }
}

// Holds the cars still before the start (the players only until "go") and after the result; at "go" the computer's
// cars get a boost, shorter the further back they start. Then the hold flags are cleared again.
void GameTaskSpecialPhysicsCases()
{
    RC_DIVERT_IF_CHANGED(GameTaskSpecialPhysicsCases);
    const int32_t rsg = at<int32_t>(kReadySteadyGo);
    if (rsg != 0 || at<int32_t>(kResultTrigger) != 0)
    {
        for (int32_t i = at<int32_t>(kNoObjects) - 1; i >= 0; --i)
        {
            const uint32_t phys = at<uint32_t>(objectAt(i) + 0x20u);
            if (!(i < static_cast<int32_t>(at<uint8_t>(kMainVars + 0x369u)) && at<int32_t>(kReadySteadyGo) < 3))
            {
                at<int16_t>(phys + 0u) = 0;
                at<int16_t>(phys + 2u) = 0;
                at<int16_t>(phys + 4u) = 0;
            }
            at<uint8_t>(phys + 0x1cdu) = at<uint8_t>(phys + 0x1cdu) | 0x1cu;
        }
        if (at<int32_t>(kReadySteadyGo) == 1)
            at<int32_t>(kDoApplyAIBoost) = 1;
        return;
    }
    const uint32_t count = at<uint32_t>(kNoObjects);
    if (at<int32_t>(kDoApplyAIBoost) != 0)
    {
        at<int32_t>(kDoApplyAIBoost) = 0;
        for (uint32_t i = at<uint8_t>(kMainVars + 0x369u); i < count; ++i)
        {
            const int32_t back = static_cast<int32_t>(i - at<uint8_t>(kMainVars + 0x369u));
            const int32_t boost = ee_div(back * 600 + 600, at<uint8_t>(kMainVars + 0x36au)); // noAI
            at<int16_t>(at<uint32_t>(objectAt(static_cast<int32_t>(i)) + 0x20u) + 0x1d2u) = static_cast<int16_t>(boost);
        }
    }
    for (int32_t i = static_cast<int32_t>(at<uint32_t>(kNoObjects)) - 1; i >= 0; --i)
    {
        const uint32_t phys = at<uint32_t>(objectAt(i) + 0x20u);
        at<uint8_t>(phys + 0x1cdu) = at<uint8_t>(phys + 0x1cdu) & 0xe3u;
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Enhancement: drawing between physics steps (RC_INTERP, on by default; RC_INTERP=0 draws the latest step as the
// original does). The physics runs in fixed 20 ms steps (50 Hz) and the race loop draws once a loop with the latest
// step, so at 60 fps one picture in five repeats the cars and camera, and at higher refresh rates (PS2X_REFRESH_HZ)
// most do. Each step keeps the cars' and cameras' drawn state from before it; each frame then draws them blended
// between that and the current state by how far the frame's time is into the step (one step behind, so always between
// two real states) and puts the current state back before anything else runs. The simulation never sees a blend.

namespace interp {

constexpr uint32_t kPhysBytes = 0x23cu, kSphereBytes = 0x190u, kMaxObjects = 32u, kCameraBytes = 0x5b8u;
constexpr uint32_t kCameras = sym::cameraVarG;

struct Car
{
    uint32_t obj = 0, phys = 0;
    bool havePrev = false;
    uint8_t prev[kPhysBytes]{};
    uint8_t cur[kPhysBytes]{};
    std::vector<uint8_t> spheres;
    uint32_t sphereAt = 0;
};
struct Cam
{
    bool havePrev = false;
    uint8_t prev[0x38]{}; // pos (0xc) .. matrix (0x24 + 0x20)
    uint8_t cur[0x38]{};
};
// The drawn fields (offset, bytes) that are blended and, after drawing, put back: nothing else in the block is
// touched, because the draw itself writes some of it (each wheel's ground material and shadow height).
constexpr std::pair<uint32_t, uint32_t> kCarFields[] = {
    {0x8u, 12u}, {0x14u, 12u}, {0x4cu, 36u}, {0x70u, 36u},                         // render position, position, matrices
    {0xacu, 12u}, {0xacu + 0x48u, 12u}, {0xacu + 0x90u, 12u}, {0xacu + 0xd8u, 12u}, // wheel positions
    {0x1ecu + 4u, 12u}, {0x1ecu + 0x10u, 12u}, {0x1ecu + 0x28u, 18u}, {0x1ecu + 0x3cu, 18u}, // the integer copy
};
Car cars[kMaxObjects];
Cam cams[2];
bool blended[kMaxObjects]{}, camBlended[2]{};
// cars whose ground and shadow pass (GameTaskShadowCalc) ran during a blended draw: it is run again on the real state
// afterwards, since the physics reads the wheels' ground material from it (grip)
std::vector<uint32_t> shadowRedo;
// cars whose wheels were placed from the drawn state (FPhysLibUpdateCarSpheres(car, 0) in ObjectRender): placed again
// from the real state afterwards (the physics uses each wheel's local position)
std::vector<uint32_t> wheelRedo;
uint8_t blendedSnap[kMaxObjects][kPhysBytes]; // RC_INTERP_LOG=3: what the draw itself writes to the block
uint32_t carCount = 0;
int32_t stepTime = 0;
bool stepped = false, applied = false;

bool enabled()
{
    static const bool on = [] { const char *v = std::getenv("RC_INTERP"); return !(v && *v == '0'); }();
    return on;
}

// Before a physics step: what is drawn now becomes the previous state.
void stepBegin()
{
    if (!enabled())
        return;
    const uint32_t count = std::min<uint32_t>(at<uint32_t>(kNoObjects), kMaxObjects);
    for (uint32_t i = 0; i < count; ++i)
    {
        Car &c = cars[i];
        const uint32_t obj = objectAt(static_cast<int32_t>(i));
        const uint32_t phys = obj ? at<uint32_t>(obj + 0x20u) : 0u;
        const bool same = c.obj == obj && c.phys == phys;
        c.obj = obj;
        c.phys = phys;
        c.havePrev = same && phys != 0u && at<uint32_t>(phys + 0x1e8u) != 0u;
        if (phys != 0u)
            std::memcpy(c.prev, guest_host(phys), kPhysBytes);
    }
    for (uint32_t i = count; i < carCount; ++i)
        cars[i] = Car{};
    carCount = count;
    for (uint32_t k = 0; k < 2; ++k)
    {
        std::memcpy(cams[k].prev, guest_host(kCameras + k * kCameraBytes + 0xcu), sizeof(cams[k].prev));
        cams[k].havePrev = true;
    }
}

void stepEnd(int32_t time)
{
    stepTime = time;
    stepped = true;
}

template <typename T>
T get(const uint8_t *block, uint32_t offset)
{
    T v;
    std::memcpy(&v, block + offset, sizeof(T));
    return v;
}

float lerp(float a, float b, float t) { return a + (b - a) * t; }

void blendFloats(uint32_t address, const uint8_t *prev, const uint8_t *cur, uint32_t offset, uint32_t count, float t)
{
    for (uint32_t k = 0; k < count; ++k)
        at<ee_float>(address + offset + 4u * k) = ee_float(lerp(get<float>(prev, offset + 4u * k), get<float>(cur, offset + 4u * k), t));
}
void blendInts(uint32_t address, const uint8_t *prev, const uint8_t *cur, uint32_t offset, uint32_t count, float t)
{
    for (uint32_t k = 0; k < count; ++k)
    {
        const int32_t a = get<int32_t>(prev, offset + 4u * k), b = get<int32_t>(cur, offset + 4u * k);
        at<int32_t>(address + offset + 4u * k) = a + static_cast<int32_t>(std::lround(static_cast<double>(b - a) * t));
    }
}
// A 3x3 rotation: blended per element, each row then scaled back to the current row's length.
void blendFloatMatrix(uint32_t address, const uint8_t *prev, const uint8_t *cur, uint32_t offset, float t)
{
    for (uint32_t r = 0; r < 3; ++r)
    {
        float v[3], len = 0.0f, want = 0.0f;
        for (uint32_t c = 0; c < 3; ++c)
        {
            const float b = get<float>(cur, offset + 12u * r + 4u * c);
            v[c] = lerp(get<float>(prev, offset + 12u * r + 4u * c), b, t);
            len += v[c] * v[c];
            want += b * b;
        }
        const float scale = len > 1e-12f ? std::sqrt(want / len) : 1.0f;
        for (uint32_t c = 0; c < 3; ++c)
            at<ee_float>(address + offset + 12u * r + 4u * c) = ee_float(v[c] * scale);
    }
}
void blendShortMatrix(uint32_t address, const uint8_t *prev, const uint8_t *cur, uint32_t offset, float t)
{
    for (uint32_t r = 0; r < 3; ++r)
    {
        float v[3], len = 0.0f, want = 0.0f;
        for (uint32_t c = 0; c < 3; ++c)
        {
            const float b = get<int16_t>(cur, offset + 6u * r + 2u * c);
            v[c] = lerp(get<int16_t>(prev, offset + 6u * r + 2u * c), b, t);
            len += v[c] * v[c];
            want += b * b;
        }
        const float scale = len > 1e-6f ? std::sqrt(want / len) : 1.0f;
        for (uint32_t c = 0; c < 3; ++c)
            at<int16_t>(address + offset + 6u * r + 2u * c) =
                static_cast<int16_t>(std::clamp<long>(std::lround(v[c] * scale), -32768, 32767));
    }
}
float distance(const uint8_t *a, const uint8_t *b, uint32_t offset)
{
    float d = 0.0f;
    for (uint32_t k = 0; k < 3; ++k)
    {
        const float x = get<float>(a, offset + 4u * k) - get<float>(b, offset + 4u * k);
        d += x * x;
    }
    return std::sqrt(d);
}
float distanceInts(const uint8_t *a, const uint8_t *b, uint32_t offset)
{
    double d = 0.0;
    for (uint32_t k = 0; k < 3; ++k)
    {
        const double x = static_cast<double>(get<int32_t>(a, offset + 4u * k)) - get<int32_t>(b, offset + 4u * k);
        d += x * x;
    }
    return static_cast<float>(std::sqrt(d));
}

// A step that moves a car or camera further than this (a respawn, a camera cut) is drawn without blending. In the
// physics' units (world units x 256); RC_INTERP_JUMP overrides it.
float jumpLimit()
{
    static const float limit = [] { const char *v = std::getenv("RC_INTERP_JUMP"); return v ? static_cast<float>(std::atof(v)) : 65536.0f; }();
    return limit;
}

} // namespace interp

// PS2X_REFRESH_HZ=<n>: vblanks at n Hz while a race (or the demo) runs, so its loop draws up to n frames a second; the
// race keeps time on its own clock. The front end and other screens step their animations once a frame, so they keep
// the video mode's rate.
void RcRefreshRace(bool racing)
{
    static const uint32_t hz = [] { const char *v = std::getenv("PS2X_REFRESH_HZ"); return v ? static_cast<uint32_t>(std::atoi(v)) : 0u; }();
    static uint32_t normal = 0;
    if (hz < 30u || hz > 500u)
        return;
    EeScheduler &scheduler = g_runtime->eeScheduler();
    if (racing)
    {
        normal = static_cast<uint32_t>(scheduler.vblankPeriod().count());
        scheduler.setVBlankPeriodMicroseconds(1000000u / hz);
    }
    else if (normal != 0u)
        scheduler.setVBlankPeriodMicroseconds(normal);
}

namespace interp {
// RC_INTERP_LOG=2: the drawn camera 0 and its car each frame (frames 3000-3600), to check the motion is even
void logDrawn(int32_t time, float t)
{
    static const bool log2 = [] { const char *v = std::getenv("RC_INTERP_LOG"); return v && *v == '2'; }();
    static uint64_t drawn = 0;
    if (!log2 || ++drawn < 3000u || drawn >= 3600u)
        return;
    const uint32_t target = at<uint32_t>(kCameras + 0xacu);
    const uint32_t phys = target ? at<uint32_t>(target + 0x20u) : 0u;
    const uint32_t state = phys ? at<uint32_t>(phys + 0x1e8u) : 0u;
    std::fprintf(stderr, "[interp-cam] time %d cam %d %d %d car %d %d %d mat %d %d %d blend %.3f step %d\n", time,
                 at<int32_t>(kCameras + 0x38u), at<int32_t>(kCameras + 0x3cu), at<int32_t>(kCameras + 0x40u),
                 state ? at<int32_t>(state + 4u) : 0, state ? at<int32_t>(state + 8u) : 0, state ? at<int32_t>(state + 12u) : 0,
                 at<int16_t>(kCameras + 0x24u), at<int16_t>(kCameras + 0x24u + 2u), at<int16_t>(kCameras + 0x24u + 4u), t, stepTime);
}
} // namespace interp

// FPhysLibUpdateCarSpheres (physics.cpp) calls this: a wheel placement during a blended draw is redone afterwards.
void RcInterpNoteWheelPlacement(uint32_t phys)
{
    if (interp::applied)
        interp::wheelRedo.push_back(phys);
}

// Called by the race loop after TaskProcess, before drawing: puts the blended state in place.
void RcInterpBeginDraw(int32_t time)
{
    using namespace interp;
    applied = false;
    if (!enabled() || !stepped)
    {
        logDrawn(time, 0.0f);
        return;
    }
    const int32_t step = std::max<int32_t>(at<int32_t>(kTimeStep), 1);
    const float t = std::clamp(static_cast<float>(time - stepTime) / static_cast<float>(step), 0.0f, 1.0f);
    static const bool log = std::getenv("RC_INTERP_LOG") != nullptr;
    static float maxStep = 0.0f, maxCam = 0.0f;
    static uint32_t frames = 0;
    for (uint32_t i = 0; i < carCount; ++i)
    {
        Car &c = cars[i];
        if (!c.havePrev || c.phys == 0u || at<uint32_t>(c.obj + 0x20u) != c.phys || at<uint32_t>(c.phys + 0x1e8u) == 0u)
            continue;
        std::memcpy(c.cur, guest_host(c.phys), kPhysBytes);
        const float moved = distance(c.prev, c.cur, 0x14u);
        if (log)
            maxStep = std::max(maxStep, moved);
        if (moved > jumpLimit())
            continue;
        // the collision spheres: a draw may recompute them from the blended state, so they are put back as well
        const int32_t spheres = get<int8_t>(c.cur, 0x1ccu);
        c.sphereAt = get<uint32_t>(c.cur, 0x1e0u);
        c.spheres.clear();
        if (c.sphereAt != 0u && spheres > 0 && spheres <= 16)
        {
            c.spheres.resize(static_cast<size_t>(spheres) * kSphereBytes);
            std::memcpy(c.spheres.data(), guest_host(c.sphereAt), c.spheres.size());
        }
        const uint32_t p = c.phys;
        blendFloats(p, c.prev, c.cur, 0x8u, 3, t);  // renderPos
        blendFloats(p, c.prev, c.cur, 0x14u, 3, t); // pos
        blendFloatMatrix(p, c.prev, c.cur, 0x4cu, t);
        blendFloatMatrix(p, c.prev, c.cur, 0x70u, t);
        for (uint32_t w = 0; w < 4; ++w)
            blendFloats(p, c.prev, c.cur, 0xacu + w * 0x48u, 3, t); // the wheel's position
        if (get<uint32_t>(c.cur, 0x1e8u) == p + 0x1ecu && get<uint32_t>(c.prev, 0x1e8u) == p + 0x1ecu)
        {
            blendInts(p, c.prev, c.cur, 0x1ecu + 4u, 3, t);    // integer renderPos
            blendInts(p, c.prev, c.cur, 0x1ecu + 0x10u, 3, t); // integer pos
            blendShortMatrix(p, c.prev, c.cur, 0x1ecu + 0x28u, t);
            blendShortMatrix(p, c.prev, c.cur, 0x1ecu + 0x3cu, t);
        }
        applied = true;
        blended[i] = true;
        std::memcpy(blendedSnap[i], guest_host(p), kPhysBytes);
    }
    for (uint32_t k = 0; k < 2; ++k)
    {
        Cam &cam = cams[k];
        const uint32_t base = kCameras + k * kCameraBytes + 0xcu;
        std::memcpy(cam.cur, guest_host(base), sizeof(cam.cur));
        camBlended[k] = false;
        const float camMoved = distanceInts(cam.prev, cam.cur, 0x24u - 0xcu + 0x14u);
        if (log && k == 0u)
            maxCam = std::max(maxCam, camMoved);
        if (!cam.havePrev || camMoved > jumpLimit() / 256.0f) // the camera's units are whole world units
            continue;
        blendInts(base, cam.prev, cam.cur, 0u, 3, t);                  // pos
        blendShortMatrix(base, cam.prev, cam.cur, 0x24u - 0xcu, t);    // matrix
        blendInts(base, cam.prev, cam.cur, 0x24u - 0xcu + 0x14u, 3, t); // matrix translation
        camBlended[k] = true;
        applied = true;
    }
    logDrawn(time, t);
    if (log && ++frames % 600u == 0u)
    {
        std::fprintf(stderr, "[interp] largest step in the last 600 frames: car %.1f, camera %.1f (jump limit %.1f)\n", maxStep, maxCam, jumpLimit());
        maxStep = maxCam = 0.0f;
    }
}

// Called after drawing: the current state goes back exactly as the physics left it.
void RcInterpEndDraw()
{
    using namespace interp;
    if (!applied)
        return;
    for (uint32_t i = 0; i < carCount; ++i)
    {
        if (!blended[i])
            continue;
        blended[i] = false;
        Car &c = cars[i];
        uint8_t *block = static_cast<uint8_t *>(guest_host(c.phys));
        static const bool check = [] { const char *v = std::getenv("RC_INTERP_LOG"); return v && *v == '3'; }();
        if (check)
        {
            // which bytes the draw wrote (to see that the re-runs below cover them all)
            static uint64_t reported = 0;
            for (uint32_t o = 0; o < kPhysBytes; ++o)
                if (block[o] != blendedSnap[i][o] && (reported & (1ull << (o / 4u % 64u))) == 0u)
                {
                    reported |= 1ull << (o / 4u % 64u);
                    std::fprintf(stderr, "[interp-check] the draw wrote car block offset 0x%x\n", o);
                }
        }
        // the exact state the physics left, then what the original's draw does to it, from that state
        std::memcpy(block, c.cur, kPhysBytes);
    }
    applied = false;
    for (const uint32_t obj : shadowRedo)
        GameTaskShadowCalc(ptr32<_sObject>(obj));
    shadowRedo.clear();
    for (const uint32_t phys : wheelRedo)
        FPhysLibUpdateCarSpheres(ptr32<_sFPhysLibCar>(phys), 0);
    wheelRedo.clear();
    for (uint32_t k = 0; k < 2; ++k)
        if (camBlended[k])
        {
            uint8_t *base = static_cast<uint8_t *>(guest_host(kCameras + k * kCameraBytes + 0xcu));
            std::memcpy(base, cams[k].cur, 12u);                    // pos
            std::memcpy(base + 0x18u, cams[k].cur + 0x18u, 0x20u); // matrix (camera +0x24)
            camBlended[k] = false;
        }
    applied = false;
}

void GameTaskProcess(ptr32<void>, tTime time)
{
    interp::stepBegin();
    GameTaskSpecialPhysicsCases();
    PhysicsMotion();
    GameTaskUpdateCarOcols();
    for (int32_t i = at<int32_t>(kNoObjects) - 1; i >= 0; --i)
        metamorphose(objectAt(i), time, kMorphScaleProcess);
    OColProcess(time);
    CarColColDetCars();
    PhysicsCollisionResponseUpdate();
    guest_call(kAnimUpdateAnims);
    if (at<int32_t>(kCameraMode) != 1) // CAMERA_MODE_FRONT_END
        CameraProcess(time, at<uint8_t>(kMainVars + 0x368u));
    at<int32_t>(kTask + 4u) = time + at<int32_t>(kTimeStep);
    interp::stepEnd(time);
    guest_call(weapon::kTaskCachedInsert, kTask); // a tail call
}

void MetaMorphiseCarSpheres(int32_t time)
{
    for (int32_t i = at<int32_t>(kNoObjects) - 1; i >= 0; --i)
        metamorphose(objectAt(i), time, kMorphScale);
}

// The ground under the car (its normal kept, scaled to 4096) and each wheel's shadow height: the highest ground contact
// of its sphere, else (driving on material 0x46, none) a cast down from the sphere, else the constant at 0x327630;
// within 128 of the ground under the middle, or that.
void GameTaskShadowCalc(ptr32<_sObject> pObject)
{
    if (interp::applied)
        interp::shadowRedo.push_back(pObject.address);
    GuestFrame frame(0x110);
    const uint32_t sp = frame.local<uint8_t>(0).address;
    const uint32_t obj = pObject.address;
    const uint32_t phys = at<uint32_t>(obj + 0x20u);
    uint32_t wheel = phys + 0xacu;
    uint32_t sphere = at<uint32_t>(phys + 0x1e0u);
    at<ee_float>(sp + 0u) = fat(phys + 0x14u) * kToCell;
    at<ee_float>(sp + 4u) = fat(phys + 0x18u) * kToCell + ee_float(80.0f);
    at<ee_float>(sp + 8u) = fat(phys + 0x1cu) * kToCell;
    const bool_ hit = AColDownCast(ptr32<_sColCellVecPSX>(sp), ptr32<_sColCellVecPSX>(obj + 0x1ecu),
                                   ptr32<_sColNormalPSX>(obj + 0x1dcu), ptr32<uint8>(sp + 0x10u));
    at<uint32_t>(sp + 0x58u) = sp + 0x20u;
    at<uint32_t>(sp + 0x54u) = sp + 0x30u;
    at<int32_t>(obj + 0x1d8u) = hit;
    for (uint32_t k = 0; k < 3; ++k)
        at<ee_float>(obj + 0x1dcu + 4u * k) = fat(obj + 0x1dcu + 4u * k) * ee_float(4096.0f);
    for (int32_t w = 4; w > 0; --w, wheel += 0x48u, sphere += 0x190u)
    {
        at<uint8_t>(sp + 0x50u) = 0x46;
        ee_float height = fat(kShadowDefault);
        const int32_t first = static_cast<int32_t>(at<int8_t>(sphere + 0x64u)) + at<int8_t>(sphere + 0x65u);
        for (int32_t k = at<int8_t>(sphere + 0x67u) - 1; k >= first; --k)
        {
            if (at<int8_t>(sphere + 0x94u + static_cast<uint32_t>(k)) == 2)
                break;
            const uint32_t normal = sphere + 0xa0u + 16u * static_cast<uint32_t>(k);
            if (ee_float(10.0f) < fat(normal + 4u))
            {
                const ee_float y = fat(sphere + 4u);
                at<int16_t>(wheel + 0x3eu) = static_cast<int16_t>(at<uint8_t>(at<uint32_t>(sphere + 0x160u + 4u * static_cast<uint32_t>(k)) + 0x53u));
                height = ((y - fat(sphere + 0x58u)) + fat(normal + 0xcu)) * kToCell;
                break;
            }
        }
        if (at<int16_t>(wheel + 0x3eu) == 0x46)
        {
            at<ee_float>(sp + 0x20u) = fat(sphere + 0u) * kToCell;
            at<ee_float>(sp + 0x24u) = fat(sphere + 4u) * kToCell + ee_float(80.0f);
            at<ee_float>(sp + 0x28u) = fat(sphere + 8u) * kToCell;
            if (AColDownCast(ptr32<_sColCellVecPSX>(sp + 0x20u), ptr32<_sColCellVecPSX>(sp + 0x30u),
                             ptr32<_sColNormalPSX>(sp + 0x40u), ptr32<uint8>(sp + 0x50u)))
                height = fat(sp + 0x34u);
        }
        at<ee_float>(wheel + 0x40u) = height;
        if (at<int32_t>(obj + 0x1d8u) != 0)
        {
            const ee_float ground = fat(obj + 0x1f0u);
            if (ee_float(128.0f) < ee_abs(ground - height))
                at<ee_float>(wheel + 0x40u) = ground;
        }
    }
}

void GameTaskGetShadowMaterialInfo(bool_ fastMethod)
{
    for (int32_t i = at<int32_t>(kNoObjects) - 1; i >= 0; --i)
    {
        const uint32_t obj = objectAt(i);
        if (VfxShortestLastDistRendered(ptr32<_sObject>(obj)) < 0x2000)
            at<int8_t>(obj + 0x21cu) = 0;
        else
        {
            const int8_t frames = static_cast<int8_t>(at<int8_t>(obj + 0x21cu) + 1);
            at<int8_t>(obj + 0x21cu) = frames < 4 ? frames : int8_t{3};
        }
        if (fastMethod != 0 && at<int8_t>(obj + 0x21cu) >= 3)
            at<int32_t>(obj + 0x1d8u) = 0;
        else
            GameTaskShadowCalc(ptr32<_sObject>(obj));
    }
}

RC_DECOMP_FN_SETUP_HEAP(GameTaskLvlInit, rc::fuzz::physicsRace)
RC_DECOMP_FN_SETUP_HEAP(GameTaskLvlTerm, rc::fuzz::physicsRace)
RC_DECOMP_FN_SETUP_HEAP(GameTaskUpdateCarOcols, rc::fuzz::physicsRace)
RC_DECOMP_FN_SETUP_HEAP(GameTaskSpecialPhysicsCases, rc::fuzz::physicsRace)
RC_DECOMP_FN_SETUP_HEAP(GameTaskProcess, rc::fuzz::physicsTask)
RC_DECOMP_FN_SETUP_HEAP(GameTaskShadowCalc, rc::fuzz::physicsObject)
RC_DECOMP_FN(GameTaskInit)
RC_DECOMP_FN(GameTaskTerm)
RC_DECOMP_FN_SETUP_HEAP(GameTaskGetShadowMaterialInfo, rc::fuzz::physicsRace)
RC_DECOMP_FN_SETUP_HEAP(MetaMorphiseCarSpheres, rc::fuzz::physicsTask)

} // namespace rc
