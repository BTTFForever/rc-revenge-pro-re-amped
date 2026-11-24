// Enhancement: the front end at the high frame rate (rc_front_interp.h, FRONTEND_INTERP_DESIGN.md option A).
#include "rc_front_interp.h"

#include "rc_decomp.h"
#include "rc_frame_tick.h"
#include "rc_settings.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <type_traits>
#include <vector>

namespace rc {
void AngleAxisToMatrix(ptr32<_matPsx> pMat, ptr32<vec16_> pAxis, angle ang); // maths.cpp
}

namespace rc::frontinterp {

namespace {

template <typename T>
T &at(uint32_t address) { return *static_cast<T *>(guest_host(address)); }
uint8_t *host(uint32_t address) { return static_cast<uint8_t *>(guest_host(address)); }

template <typename T>
T get(const uint8_t *block, uint32_t offset)
{
    T v;
    std::memcpy(&v, block + offset, sizeof(T));
    return v;
}
template <typename T>
void put(uint8_t *block, uint32_t offset, T v) { std::memcpy(block + offset, &v, sizeof(T)); }

float lerp(float a, float b, float t) { return a + (b - a) * t; }

// The blends write into a host copy of the block (dst), from prev and cur copies.
void blendInts(uint8_t *dst, const uint8_t *prev, const uint8_t *cur, uint32_t offset, uint32_t count, float t)
{
    for (uint32_t k = 0; k < count; ++k)
    {
        const int32_t a = get<int32_t>(prev, offset + 4u * k), b = get<int32_t>(cur, offset + 4u * k);
        put<int32_t>(dst, offset + 4u * k, a + static_cast<int32_t>(std::lround((static_cast<double>(b) - a) * t)));
    }
}
void blendFloats(uint8_t *dst, const uint8_t *prev, const uint8_t *cur, uint32_t offset, uint32_t count, float t)
{
    for (uint32_t k = 0; k < count; ++k)
        put<float>(dst, offset + 4u * k, lerp(get<float>(prev, offset + 4u * k), get<float>(cur, offset + 4u * k), t));
}
// A 3x3 rotation of shorts (4096 = 1): blended per element, each row scaled back to the current row's length.
void blendShortMatrix(uint8_t *dst, const uint8_t *prev, const uint8_t *cur, uint32_t offset, float t)
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
            put<int16_t>(dst, offset + 6u * r + 2u * c, static_cast<int16_t>(std::clamp<long>(std::lround(v[c] * scale), -32768, 32767)));
    }
}
void blendFloatMatrix(uint8_t *dst, const uint8_t *prev, const uint8_t *cur, uint32_t offset, float t)
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
            put<float>(dst, offset + 12u * r + 4u * c, v[c] * scale);
    }
}
double distanceInts(const uint8_t *a, const uint8_t *b, uint32_t offset)
{
    double d = 0.0;
    for (uint32_t k = 0; k < 3; ++k)
    {
        const double x = static_cast<double>(get<int32_t>(a, offset + 4u * k)) - get<int32_t>(b, offset + 4u * k);
        d += x * x;
    }
    return std::sqrt(d);
}

float envFloat(const char *name, float fallback)
{
    const char *v = std::getenv(name);
    return v && *v ? static_cast<float>(std::atof(v)) : fallback;
}
int logLevel()
{
    static const int level = [] { const char *v = std::getenv("RC_FE_INTERP_LOG"); return v && *v ? std::atoi(v) : 0; }();
    return level;
}

// ---- camera 0: pos (+0xc, 3 ints) .. matrix (+0x24, 9 shorts) and its translation (+0x38, 3 ints) ----
constexpr uint32_t kCamBase = sym::cameraVarG + 0xcu, kCamBytes = 0x38u, kCamMatrix = 0x18u, kCamTrans = 0x2cu;
struct Camera
{
    bool havePrev = false, haveCur = false;
    uint8_t prev[kCamBytes]{}, cur[kCamBytes]{}, saved[kCamBytes]{};
    bool applied = false;
} g_cam;
// A fly-by speeds up and slows down along its path: up to ~27,500 camera units a tick, each step within a few
// thousand of the last. A cut (a new place) is a step above RC_FE_INTERP_JUMP (default 40,000) or far above the last
// one; it is drawn without a blend.
float camJump() { static const float v = envFloat("RC_FE_INTERP_JUMP", 40000.0f); return v; }
double g_camLastStep = 0.0, g_camStep = 0.0;
bool g_camCut = false;

// ---- the scenery objects (tsoVarG: count, then 0x3c-byte entries: pos +0x10 (1/256 units), matrix +0x24) ----
constexpr uint32_t kTsoStride = 0x3cu, kTsoPos = 0x10u, kTsoMatrix = 0x24u;
struct Tso
{
    uint8_t prev[30]{}, cur[30]{}, saved[30]{}; // pos (12) + matrix (18)
    bool havePrev = false, haveCur = false, blended = false;
};
std::vector<Tso> g_tsos;
uint32_t g_tsoHeader = 0;
float tsoJump() { static const float v = envFloat("RC_FE_INTERP_TSO_JUMP", 256.0f * 400.0f); return v; }

// ---- the turntable's cars ----
constexpr uint32_t kPhysBytes = 0x23cu, kRendBytes = 0x2cu, kIntBytes = 0x50u;
struct Car
{
    uint32_t obj = 0, phys = 0, rend = 0, integer = 0;
    bool havePrev = false, haveCur = false, blended = false;
    uint8_t prevP[kPhysBytes]{}, curP[kPhysBytes]{}, savedP[kPhysBytes]{};
    uint8_t prevR[kRendBytes]{}, curR[kRendBytes]{}, savedR[kRendBytes]{};
    uint8_t prevI[kIntBytes]{}, curI[kIntBytes]{}, savedI[kIntBytes]{};
};
std::vector<Car> g_cars;
float carJump() { static const float v = envFloat("RC_FE_INTERP_CAR_JUMP", 1500.0f); return v; } // ~470 a tick turning

// ---- objects (render-only passes) ----
constexpr uint32_t kObjectBytes = 0x42cu;
std::vector<uint8_t> g_objects;
uint32_t g_objectCount = 0;

// ---- K1 (slot 0 the front end's menus, 1 the race's pause menu) ----
struct K1Part
{
    std::vector<uint8_t> bytes;
    int32_t start = 0, tagEnd = -1;
    uint32_t lastTex = 0, zWrite = 0;
    bool kept = false;
} g_k1[2];

// ---- diagnostics ----
struct Stats
{
    uint64_t passes = 0, ticks = 0, renderOnly = 0, camJumps = 0, tsoBlended = 0, carJumps = 0;
    double maxCam = 0.0, maxTso = 0.0, maxCar = 0.0;
    size_t k1Bytes = 0;
    uint32_t k1Chain = 0, k1Polys = 0; // 3D work the kept 2D part added (lost on the passes between ticks if not 0)
} g_stats;
uint32_t g_k1ChainAt = 0, g_k1PolysAt = 0;
uint32_t polyCount()
{
    uint32_t n = 0;
    for (const sRdrPolyChannel &c : G(gRdrPolyClass).channels)
        n += c.polysToRender + c.vifPktInUse;
    return n;
}

std::atomic<const std::atomic<uint64_t> *> g_inputClock{nullptr};

} // namespace

void setInputClock(const void *clock) { g_inputClock.store(static_cast<const std::atomic<uint64_t> *>(clock)); }

bool enabled()
{
    static const bool diagnostics = [] {
        const char *rec = std::getenv("RC_FRAME_RECORD"), *survey = std::getenv("RC_GS_SURVEY"), *off = std::getenv("RC_FE_INTERP");
        return (rec && *rec) || (survey && *survey && *survey != '0') || (off && *off == '0');
    }();
    return !diagnostics && settings::smoothMotion() && frametick::raceHz() != 0u;
}

// ---------------------------------------------------------------------------------------------------------------------

void tickCapture()
{
    if (!enabled())
    {
        g_cam.haveCur = g_cam.havePrev = false;
        g_tsos.clear();
        g_tsoHeader = 0;
        return;
    }
    // the camera
    g_cam.havePrev = g_cam.haveCur;
    std::memcpy(g_cam.prev, g_cam.cur, kCamBytes);
    std::memcpy(g_cam.cur, host(kCamBase), kCamBytes);
    g_cam.haveCur = true;
    g_camLastStep = g_camStep;
    g_camStep = g_cam.havePrev ? distanceInts(g_cam.prev, g_cam.cur, 0u) : 0.0;
    g_camCut = g_camStep > camJump() || g_camStep > 4.0 * g_camLastStep + 8000.0;
    if (g_camCut)
        ++g_stats.camJumps;
    g_stats.maxCam = std::max(g_stats.maxCam, g_camStep);
    if (logLevel() >= 2 && g_cam.havePrev)
        std::fprintf(stderr, "[fe-interp-step] camera %.1f%s\n", g_camStep, g_camCut ? " cut" : "");

    // the scenery: the matrices TSORender would build this frame, built now so the current state is complete
    const uint32_t header = at<uint32_t>(sym::tsoVarG);
    const uint32_t count = header ? at<uint32_t>(header) : 0u;
    if (header != g_tsoHeader || count != g_tsos.size())
    {
        g_tsoHeader = header;
        g_tsos.assign(count, Tso{});
    }
    for (uint32_t i = 0; i < count; ++i)
    {
        const uint32_t tso = header + 4u + i * kTsoStride;
        const uint16_t flags = at<uint16_t>(tso + 0x22u);
        if ((flags & 0x20u) == 0 && (flags & 2u) != 0 && (at<uint32_t>(tso + 0x20u) & 0xc00000u) == 0)
        {
            if ((flags & 4u) != 0)
                AngleAxisToMatrix(ptr32<_matPsx>(tso + kTsoMatrix), ptr32<vec16_>(tso + 0x1cu), at<int16_t>(tso + 0x36u));
            else if ((flags & 8u) != 0)
                guest_call(sym::Mat3nBuild, tso + kTsoMatrix, tso + 0x1cu);
            at<uint16_t>(tso + 0x22u) = static_cast<uint16_t>(at<uint16_t>(tso + 0x22u) & 0xfffdu);
        }
        Tso &s = g_tsos[i];
        std::memcpy(s.prev, s.cur, sizeof(s.cur));
        s.havePrev = s.haveCur;
        s.haveCur = true;
        std::memcpy(s.cur, host(tso + kTsoPos), 12);
        std::memcpy(s.cur + 12, host(tso + kTsoMatrix), 18);
    }
}

void applyCamera()
{
    g_cam.applied = false;
    if (!frametick::fast() || !g_cam.havePrev)
        return;
    uint8_t *base = host(kCamBase);
    if (std::memcmp(base, g_cam.cur, kCamBytes) != 0) // moved since the tick (a cut by the menu logic): drawn as it is
        return;
    if (g_camCut)
        return;
    std::memcpy(g_cam.saved, base, kCamBytes);
    uint8_t blended[kCamBytes];
    std::memcpy(blended, g_cam.cur, kCamBytes);
    const float t = frametick::blend();
    blendInts(blended, g_cam.prev, g_cam.cur, 0u, 3, t);
    blendShortMatrix(blended, g_cam.prev, g_cam.cur, kCamMatrix, t);
    blendInts(blended, g_cam.prev, g_cam.cur, kCamTrans, 3, t);
    std::memcpy(base, blended, kCamBytes);
    g_cam.applied = true;
}

void logDrawnCamera()
{
    if (logLevel() < 2)
        return;
    using Clock = std::chrono::steady_clock;
    const double ms = std::chrono::duration<double, std::milli>(Clock::now().time_since_epoch()).count();
    std::fprintf(stderr, "[fe-interp-cam] %.2f %s t %.3f pos %d %d %d%s\n", ms, frametick::logicPass() ? "tick" : "draw", frametick::blend(),
                 at<int32_t>(kCamBase), at<int32_t>(kCamBase + 4u), at<int32_t>(kCamBase + 8u), g_cam.applied ? "" : " (unblended)");
}

void restoreCamera()
{
    if (!g_cam.applied)
        return;
    std::memcpy(host(kCamBase), g_cam.saved, kCamBytes);
    g_cam.applied = false;
}

void applyScenery()
{
    if (!frametick::fast() || g_tsoHeader == 0u || at<uint32_t>(sym::tsoVarG) != g_tsoHeader)
        return;
    const float t = frametick::blend();
    for (uint32_t i = 0; i < g_tsos.size(); ++i)
    {
        Tso &s = g_tsos[i];
        s.blended = false;
        if (!s.havePrev || std::memcmp(s.prev, s.cur, sizeof(s.cur)) == 0)
            continue;
        const uint32_t tso = g_tsoHeader + 4u + i * kTsoStride;
        uint8_t now[30];
        std::memcpy(now, host(tso + kTsoPos), 12);
        std::memcpy(now + 12, host(tso + kTsoMatrix), 18);
        if (std::memcmp(now, s.cur, sizeof(now)) != 0)
            continue;
        const double moved = distanceInts(s.prev, s.cur, 0u);
        g_stats.maxTso = std::max(g_stats.maxTso, moved);
        if (moved > tsoJump())
            continue;
        std::memcpy(s.saved, now, sizeof(now));
        uint8_t blended[30];
        std::memcpy(blended, s.cur, sizeof(blended));
        blendInts(blended, s.prev, s.cur, 0u, 3, t);
        blendShortMatrix(blended, s.prev, s.cur, 12u, t);
        std::memcpy(host(tso + kTsoPos), blended, 12);
        std::memcpy(host(tso + kTsoMatrix), blended + 12, 18);
        s.blended = true;
        ++g_stats.tsoBlended;
    }
}

// TSORender writes each object's flags (0x10 drawn; 0x11 cleared on hidden ones, among them the turntable's "in view"
// bit, which CarouselRender sets again on ticks): a pass between ticks keeps them as the tick left them.
std::vector<uint16_t> g_tsoFlags;
void sceneryFlagsSave()
{
    const uint32_t header = at<uint32_t>(sym::tsoVarG);
    const uint32_t count = header ? at<uint32_t>(header) : 0u;
    g_tsoFlags.resize(count);
    for (uint32_t i = 0; i < count; ++i)
        g_tsoFlags[i] = at<uint16_t>(header + 4u + i * kTsoStride + 0x22u);
}
void sceneryFlagsRestore()
{
    const uint32_t header = at<uint32_t>(sym::tsoVarG);
    const uint32_t count = header ? at<uint32_t>(header) : 0u;
    if (count != g_tsoFlags.size())
        return;
    for (uint32_t i = 0; i < count; ++i)
        at<uint16_t>(header + 4u + i * kTsoStride + 0x22u) = g_tsoFlags[i];
}

void restoreScenery()
{
    for (uint32_t i = 0; i < g_tsos.size(); ++i)
    {
        Tso &s = g_tsos[i];
        if (!s.blended)
            continue;
        const uint32_t tso = g_tsoHeader + 4u + i * kTsoStride;
        std::memcpy(host(tso + kTsoPos), s.saved, 12);
        std::memcpy(host(tso + kTsoMatrix), s.saved + 12, 18);
        s.blended = false;
    }
}

// ---------------------------------------------------------------------------------------------------------------------


void carsCapture()
{
    if (!enabled())
    {
        g_cars.clear();
        return;
    }
    const sCarouselClass &c = G(carouselVarG);
    const uint32_t n = std::min<uint32_t>(c.numSegments, static_cast<uint32_t>(std::size(c.carousel)));
    if (g_cars.size() != n)
        g_cars.assign(n, Car{});
    for (uint32_t i = 0; i < n; ++i)
    {
        Car &car = g_cars[i];
        const ptr32<_sObject> pObject = c.carousel[i].pObject;
        const uint32_t obj = pObject.address;
        const uint32_t phys = obj ? pObject->pPhysics.address : 0u;
        const uint32_t rend = obj ? pObject->pRendObj.address : 0u;
        const uint32_t integer = phys ? at<uint32_t>(phys + 0x1e8u) : 0u;
        const bool same = car.obj == obj && car.phys == phys && car.rend == rend && car.integer == integer;
        car.obj = obj;
        car.phys = phys;
        car.rend = rend;
        car.integer = integer;
        if (!obj || !phys || !rend || !integer)
        {
            car.havePrev = car.haveCur = false;
            continue;
        }
        car.havePrev = same && car.haveCur;
        std::memcpy(car.prevP, car.curP, kPhysBytes);
        std::memcpy(car.prevR, car.curR, kRendBytes);
        std::memcpy(car.prevI, car.curI, kIntBytes);
        std::memcpy(car.curP, host(phys), kPhysBytes);
        std::memcpy(car.curR, host(rend), kRendBytes);
        std::memcpy(car.curI, host(integer), kIntBytes);
        car.haveCur = true;
    }
}

void carsApply()
{
    if (!frametick::fast())
        return;
    const float t = frametick::blend();
    for (Car &car : g_cars)
    {
        car.blended = false;
        if (!car.havePrev || !car.haveCur)
            continue;
        if (std::memcmp(host(car.rend), car.curR, kRendBytes) != 0 || std::memcmp(host(car.integer), car.curI, kIntBytes) != 0)
            continue; // changed since the tick
        const double moved = distanceInts(car.prevR, car.curR, 4u);
        g_stats.maxCar = std::max(g_stats.maxCar, moved);
        if (moved > carJump())
        {
            ++g_stats.carJumps;
            continue;
        }
        std::memcpy(car.savedP, host(car.phys), kPhysBytes);
        std::memcpy(car.savedR, host(car.rend), kRendBytes);
        std::memcpy(car.savedI, host(car.integer), kIntBytes);
        uint8_t *p = host(car.phys), *r = host(car.rend), *in = host(car.integer);
        blendInts(r, car.prevR, car.curR, 4u, 3, t);         // render object: pos
        blendShortMatrix(r, car.prevR, car.curR, 0x18u, t);  // matrix
        blendFloats(p, car.prevP, car.curP, 0x8u, 3, t);     // physics: render position
        blendFloats(p, car.prevP, car.curP, 0x14u, 3, t);    // position
        blendFloatMatrix(p, car.prevP, car.curP, 0x4cu, t);  // matrix
        if (car.integer < car.phys || car.integer >= car.phys + kPhysBytes) // the integer copy outside the block
        {
            blendInts(in, car.prevI, car.curI, 4u, 3, t);
            blendInts(in, car.prevI, car.curI, 0x10u, 3, t);
            blendShortMatrix(in, car.prevI, car.curI, 0x28u, t);
        }
        else
        {
            const uint32_t o = car.integer - car.phys;
            blendInts(p, car.prevP, car.curP, o + 4u, 3, t);
            blendInts(p, car.prevP, car.curP, o + 0x10u, 3, t);
            blendShortMatrix(p, car.prevP, car.curP, o + 0x28u, t);
        }
        car.blended = true;
    }
    if (logLevel() >= 3)
    {
        const sCarouselClass &c = G(carouselVarG);
        if (c.segment < g_cars.size())
        {
            const Car &car = g_cars[c.segment];
            std::fprintf(stderr, "[fe-interp-car] %s t %.3f blended %d have %d%d rend %d %d %d prev %d %d %d cur %d %d %d phys %.0f %.0f %.0f\n",
                         frametick::logicPass() ? "tick" : "draw", t, car.blended, car.havePrev, car.haveCur, at<int32_t>(car.rend + 4u),
                         at<int32_t>(car.rend + 8u), at<int32_t>(car.rend + 12u), get<int32_t>(car.prevR, 4), get<int32_t>(car.prevR, 8),
                         get<int32_t>(car.prevR, 12), get<int32_t>(car.curR, 4), get<int32_t>(car.curR, 8), get<int32_t>(car.curR, 12),
                         at<float>(car.phys + 8u), at<float>(car.phys + 12u), at<float>(car.phys + 16u));
        }
    }
}

void carsRestore()
{
    for (Car &car : g_cars)
    {
        if (!car.blended)
            continue;
        std::memcpy(host(car.phys), car.savedP, kPhysBytes);
        std::memcpy(host(car.rend), car.savedR, kRendBytes);
        std::memcpy(host(car.integer), car.savedI, kIntBytes);
        car.blended = false;
    }
}

void objectsSave()
{

    const uint32_t list = sym::objectVarG;
    g_objectCount = std::min<uint32_t>(at<uint32_t>(list + 0x170u), 64u);
    g_objects.resize(static_cast<size_t>(g_objectCount) * (kObjectBytes + 4u));
    for (uint32_t i = 0; i < g_objectCount; ++i)
    {
        const uint32_t obj = at<uint32_t>(list + 4u * i);
        std::memcpy(g_objects.data() + i * (kObjectBytes + 4u), &obj, 4u);
        if (obj)
            std::memcpy(g_objects.data() + i * (kObjectBytes + 4u) + 4u, host(obj), kObjectBytes);
    }
}

void objectsRestore()
{
    const uint32_t list = sym::objectVarG;
    if (at<uint32_t>(list + 0x170u) < g_objectCount)
        return; // objects went away during the pass (not expected): leave them
    for (uint32_t i = 0; i < g_objectCount; ++i)
    {
        uint32_t obj;
        std::memcpy(&obj, g_objects.data() + i * (kObjectBytes + 4u), 4u);
        if (obj && at<uint32_t>(list + 4u * i) == obj)
            std::memcpy(host(obj), g_objects.data() + i * (kObjectBytes + 4u) + 4u, kObjectBytes);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// K1, the GfxP2 packet (quadwords from gGfxP2.pDmaBuff; buffIdx the next free one, gifTagIdx the open GIF tag or -1).

void k1Forget(int slot) { g_k1[slot & 1].kept = false; }

void k1Begin(int slot)
{
    sGfxP2 &gfx = G(gGfxP2);
    gfx.gifTagIdx = -1;     // the kept part starts with its own GIF tag
    G(gLastTex) = nullptr;  // and sets its own texture
    g_k1[slot & 1].start = gfx.buffIdx;
    g_k1ChainAt = static_cast<uint32_t>(G(gRdr).currDmaTag);
    g_k1PolysAt = polyCount();
}

void k1End(int slot)
{
    K1Part &k = g_k1[slot & 1];
    const sGfxP2 &gfx = G(gGfxP2);
    const int32_t n = gfx.buffIdx - k.start;
    if (n < 0)
    {
        k.kept = false;
        return;
    }
    k.bytes.resize(static_cast<size_t>(n) * 16u);
    if (n > 0)
        std::memcpy(k.bytes.data(), host(gfx.pDmaBuff.address + static_cast<uint32_t>(k.start) * 16u), k.bytes.size());
    k.tagEnd = gfx.gifTagIdx >= k.start ? gfx.gifTagIdx - k.start : -1;
    k.lastTex = G(gLastTex).address;
    k.zWrite = static_cast<uint32_t>(G(zWriteEnabled));
    k.kept = true;
    if (slot == 0)
        g_stats.k1Bytes = k.bytes.size();
    g_stats.k1Chain = std::max(g_stats.k1Chain, static_cast<uint32_t>(G(gRdr).currDmaTag) - g_k1ChainAt);
    g_stats.k1Polys = std::max(g_stats.k1Polys, polyCount() - g_k1PolysAt);
}

void k1Replay(int slot)
{
    const K1Part &k = g_k1[slot & 1];
    if (!k.kept)
        return;
    sGfxP2 &gfx = G(gGfxP2);
    const int32_t start = gfx.buffIdx;
    const int32_t n = static_cast<int32_t>(k.bytes.size() / 16u);
    if (start + n > 0x50000 / 16 - 16) // no room (not expected: front-end frames are ~140 KB of 320)
        return;
    gfx.gifTagIdx = -1;
    if (n > 0)
        std::memcpy(host(gfx.pDmaBuff.address + static_cast<uint32_t>(start) * 16u), k.bytes.data(), k.bytes.size());
    gfx.buffIdx = start + n;
    gfx.gifTagIdx = k.tagEnd >= 0 ? start + k.tagEnd : -1;
    G(gLastTex).address = k.lastTex;
    G(zWriteEnabled) = static_cast<std::remove_reference_t<decltype(G(zWriteEnabled))>>(k.zWrite);
}

// ---------------------------------------------------------------------------------------------------------------------

void logPass(bool tick)
{
    ++g_stats.passes;
    if (tick)
        ++g_stats.ticks;
    else
        ++g_stats.renderOnly;
    if (logLevel() < 1)
        return;
    using Clock = std::chrono::steady_clock;
    static auto last = Clock::now();
    const auto now = Clock::now();
    if (now - last < std::chrono::seconds(1))
        return;
    const double s = std::chrono::duration<double>(now - last).count();
    last = now;
    std::fprintf(stderr,
                 "[fe-interp] %.1f passes/s, %.1f ticks/s, %.1f render-only/s; largest step: camera %.1f (cuts %llu), scenery %.0f "
                 "(%llu blended), turntable car %.1f (jumps %llu); 2D packet %zu bytes (3D in it: %u tags, %u polys); fast %d\n",
                 g_stats.passes / s, g_stats.ticks / s, g_stats.renderOnly / s, g_stats.maxCam, static_cast<unsigned long long>(g_stats.camJumps),
                 g_stats.maxTso, static_cast<unsigned long long>(g_stats.tsoBlended), g_stats.maxCar, static_cast<unsigned long long>(g_stats.carJumps),
                 g_stats.k1Bytes, g_stats.k1Chain, g_stats.k1Polys, frametick::fast() ? 1 : 0);
    g_stats = Stats{};
}

void trace(uint32_t tickNo)
{
    static std::FILE *const file = []() -> std::FILE * {
        const char *path = std::getenv("RC_FE_TRACE");
        return path && *path ? std::fopen(path, "w") : nullptr;
    }();
    if (!file)
        return;
    const std::atomic<uint64_t> *clockPtr = g_inputClock.load();
    const uint64_t clock = clockPtr ? clockPtr->load(std::memory_order_relaxed) : 0u;
    const sMenuSystem &ms = *G(pMenuSystemG);
    const uint32_t menu = ms.pActiveMenu.address;
    const _sCamera &cam = *reinterpret_cast<const _sCamera *>(guest_host(sym::cameraVarG));
    const sCarouselClass &c = G(carouselVarG);
    const sFrontEndClass &f = G(frontEndVarG);
    std::fprintf(file, "%llu:%llu tick %u menu %08x sel %d status %d fe %d active %d cam %d %d %d path %d/%d seg %u angle %d mode %d demo %u music %d/%d/%d split %u\n",
                 static_cast<unsigned long long>(clock >> 32), static_cast<unsigned long long>(clock & 0xffffffffull), tickNo,
                 menu ? at<uint32_t>(menu + 0x14u) : 0u, menu ? at<int8_t>(menu + 0x63u) : -1, static_cast<int>(G(mainVarG).status),
                 static_cast<int>(f.frontEndStatus), f.active, cam.pos.x, cam.pos.y, cam.pos.z, cam.path.curNodeOnPath, cam.path.interp,
                 static_cast<unsigned>(c.segment), c.angle, c.moveMode, f.uiDemoLoopWait, at<int32_t>(sym::rcmusicVarG + 0x18u),
                 at<int32_t>(sym::rcmusicVarG + 0x14u), at<int32_t>(sym::rcmusicVarG + 0x20u), at<uint8_t>(sym::mainVarG + 0x368u));
    std::fflush(file);
}

} // namespace rc::frontinterp
