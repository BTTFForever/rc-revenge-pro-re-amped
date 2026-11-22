// exp/quick-dirty: widescreen, HUD anchoring and frame interpolation (rc_exp.h, EXPERIMENT.md).
#include "rc_decomp.h"
#include "rc_exp.h"

#include "runtime/ps2_display_options.h"
#include "runtime/ps2_exp_stats.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

namespace rc {
void GfxP2_EndScene(); // rdr.cpp
}

namespace rc::exp {

namespace {

bool flag(const char *name, bool fallback = false)
{
    const char *v = std::getenv(name);
    if (!v || !*v)
        return fallback;
    return *v != '0';
}

float parseAspect(const char *text)
{
    if (!text || !*text)
        return 16.0f / 9.0f;
    float w = 0, h = 0;
    if (std::sscanf(text, "%f:%f", &w, &h) == 2 && w > 0 && h > 0)
        return w / h;
    const float r = std::strtof(text, nullptr);
    return r > 0.5f && r < 5.0f ? r : 16.0f / 9.0f;
}

Config load()
{
    Config c;
    c.widescreen = flag("RC_WIDESCREEN");
    if (c.widescreen)
    {
        c.aspect = parseAspect(std::getenv("RC_ASPECT"));
        c.hscale = (4.0f / 3.0f) / c.aspect;
        c.hudAnchor = flag("RC_HUD_ANCHOR");
    }
    c.interp = flag("RC_INTERP");
    if (c.interp)
    {
        const char *fps = std::getenv("RC_FPS");
        const int want = fps && *fps ? std::atoi(fps) : 120;
        // The tick rate is the field rate (59.94 or 50); pictures per tick rounded, at least 2, at most 8.
        const bool pal = RC_VERSION_EU != 0;
        const float rate = pal ? 50.0f : 59.94f;
        c.perTick = std::clamp(static_cast<int>(want / rate + 0.5f), 2, 8);
        c.pace = flag("RC_INTERP_PACE", true);
        c.force = flag("RC_INTERP_FORCE");
        c.objects = flag("RC_INTERP_OBJECTS", true);
    }
    c.log = flag("RC_EXP_LOG");
    c.uiPillarbox = c.widescreen && flag("RC_UI_PILLARBOX");
    c.dips = flag("RC_EXP_DIPS");
    return c;
}

const Config g_config = load();

// The presentation side: the window's aspect and size, set before the runtime opens the window.
const bool g_presentation = []
{
    if (g_config.widescreen)
    {
        ps2_host_display::setAspect(g_config.aspect);
        const int h = 720;
        ps2_host_display::setInitialWindowSize(static_cast<int>(h * g_config.aspect + 0.5f), h);
    }
    if (g_config.interp)
        ps2_host_display::setPresentOnFlip(true);
    if (g_config.widescreen || g_config.interp || g_config.dips)
        std::fprintf(stderr, "[exp] widescreen=%d aspect=%.3f hscale=%.4f hud=%d ui=%d interp=%d perTick=%d pace=%d objects=%d\n",
                     g_config.widescreen, g_config.aspect, g_config.hscale, g_config.hudAnchor, g_config.uiPillarbox,
                     g_config.interp, g_config.perTick, g_config.pace, g_config.objects);
    return true;
}();

template <typename T>
T &at(uint32_t address) { return *static_cast<T *>(guest_host(address)); }

} // namespace

const Config &config() { return g_config; }

float widenHalfAngle(float half)
{
    return std::atan(std::tan(half) / g_config.hscale);
}

// ---------------------------------------------------------------------------------------------------------------------
// HUD anchoring

namespace {

// GS x in 12.4 fixed point: the screen's left edge, centre and right edge (the 2D offset is 2048 - 320).
constexpr uint32_t kLeftX = 0x6c0u << 4, kCentreX = 0x800u << 4, kRightX = (0x6c0u + 640u) << 4;

uint32_t anchorX(uint32_t anchor)
{
    switch (anchor)
    {
    case 1: case 7: case 9: return kLeftX;    // left, top left, bottom left
    case 2: case 8: case 10: return kRightX;  // right, top right, bottom right
    case 3: case 4: case 5: return kCentreX;  // top, bottom, centre
    default: return 0;                        // stretch, none
    }
}

struct Walker
{
    uint32_t buffer, anchor;
    int32_t end;
    float scale;

    uint64_t *qw(int32_t i) const { return static_cast<uint64_t *>(guest_host(buffer + static_cast<uint32_t>(i) * 16u)); }

    bool measure = false;
    mutable int32_t minX = 0x10000, maxX = -1, minY = 0x10000, maxY = -1;

    void fix(uint64_t &low) const
    {
        if (measure)
        {
            const int32_t x = static_cast<int32_t>(low & 0xffffu);
            const int32_t y = static_cast<int32_t>((low >> 32) & 0xffffu);
            minX = std::min(minX, x);
            maxX = std::max(maxX, x);
            minY = std::min(minY, y);
            maxY = std::max(maxY, y);
            return;
        }
        const int32_t x = static_cast<int32_t>(low & 0xffffu);
        int32_t nx = static_cast<int32_t>(static_cast<float>(anchor) + static_cast<float>(x - static_cast<int32_t>(anchor)) * scale + 0.5f);
        nx = std::clamp(nx, 0, 0xffff);
        low = (low & ~0xffffull) | static_cast<uint64_t>(nx);
    }

    // Returns the quadword after the tag's data.
    int32_t tag(int32_t index, int32_t first) const
    {
        const uint64_t t0 = qw(index)[0], t1 = qw(index)[1];
        const int32_t nloop = static_cast<int32_t>(t0 & 0x7fffu);
        const uint32_t flg = static_cast<uint32_t>(t0 >> 58) & 3u;
        int32_t nreg = static_cast<int32_t>(t0 >> 60) & 0xf;
        if (nreg == 0)
            nreg = 16;
        const int32_t data = index + 1;
        if (flg == 1)
            return data + (nloop * nreg + 1) / 2;
        if (flg >= 2)
            return data + nloop;
        const int32_t total = nloop * nreg;
        for (int32_t q = std::max(first, data); q < data + total && q < end; ++q)
        {
            const uint32_t r = static_cast<uint32_t>(t1 >> (4 * ((q - data) % nreg))) & 0xfu;
            uint64_t *p = qw(q);
            if (r == 4 || r == 5 || r == 0xc || r == 0xd)
                fix(p[0]);
            else if (r == 0xe)
            {
                const uint32_t a = static_cast<uint32_t>(p[1] & 0xffu);
                if (a == 4 || a == 5 || a == 0xc || a == 0xd)
                    fix(p[0]);
            }
        }
        return data + total;
    }
};

} // namespace

namespace {
int g_scopeDepth = 0; // active or blocking anchor scopes open
}

HudAnchorScope::HudAnchorScope(uint32_t anchor, Kind kind) : m_kind(kind)
{
    if (!g_config.widescreen || g_scopeDepth > 0)
        return;
    if (anchor == kKeep)
    {
        m_counted = true;
        ++g_scopeDepth;
        return;
    }
    if (!(kind == Hud ? g_config.hudAnchor : g_config.uiPillarbox))
        return;
    if (at<uint8_t>(sym::mainVarG + 0x368u) != 0) // not in split screen
        return;
    m_anchorX = anchorX(anchor);
    if (m_anchorX == 0)
        return;
    const sGfxP2 &gfx = G(gGfxP2);
    m_on = m_counted = true;
    ++g_scopeDepth;
    m_buffer = gfx.pDmaBuff.address;
    m_tag = gfx.gifTagIdx;
    m_start = gfx.buffIdx;
}

HudAnchorScope::~HudAnchorScope()
{
    if (m_counted)
        --g_scopeDepth;
    if (!m_on)
        return;
    const sGfxP2 &gfx = G(gGfxP2);
    if (gfx.pDmaBuff.address != m_buffer || gfx.buffIdx < m_start)
        return; // the packet was sent inside the scope
    Walker w{m_buffer, m_anchorX, gfx.buffIdx, g_config.hscale};
    const auto walk = [&]
    {
        int32_t pos = m_start;
        if (m_tag != -1 && m_tag < m_start)
            pos = w.tag(m_tag, m_start);
        for (int guard = 0; pos < w.end && guard < 100000; ++guard)
            pos = w.tag(pos, pos + 1);
    };
    if (m_kind == Ui)
    {
        w.measure = true;
        walk();
        w.measure = false;
        // A full-screen background or fade (edge to edge, most of the height): keep it filling. A wide menu border
        // or panel is pillarboxed with its text.
        if (w.maxX >= 0 && w.maxX - w.minX >= (624 << 4) && w.maxY - w.minY >= (200 << 4))
            return;
    }
    walk();
}

// ---------------------------------------------------------------------------------------------------------------------
// Frame-time dips

namespace {
using Clock = std::chrono::steady_clock;
Clock::time_point g_swap[3], g_lastStart, g_lastEnd, g_secondStart;
uint64_t g_swaps = 0, g_dips = 0;
uint64_t g_waitAtEnd[ps2x::expstats::kSites]{};
// Per-second sums for the summary line.
double g_secFrames = 0, g_secInterval = 0, g_secGame = 0, g_secGs = 0, g_secVblank = 0, g_secWait[ps2x::expstats::kSites]{};
}

void swapMark(int stage)
{
    if (!g_config.dips)
        return;
    const auto now = Clock::now();
    g_swap[stage] = now;
    if (stage != 2)
        return;
    ++g_swaps;
    const auto ms = [](Clock::duration d) { return std::chrono::duration<double, std::milli>(d).count(); };
    const double field = RC_VERSION_EU ? 20.0 : 1001.0 / 60.0;
    double wait[ps2x::expstats::kSites];
    for (int i = 0; i < ps2x::expstats::kSites; ++i)
    {
        const uint64_t v = ps2x::expstats::g_ns[i].load(std::memory_order_relaxed);
        wait[i] = static_cast<double>(v - g_waitAtEnd[i]) / 1e6;
        g_waitAtEnd[i] = v;
    }
    if (g_lastStart != Clock::time_point{})
    {
        const double interval = ms(g_swap[0] - g_lastStart);
        const double game = ms(g_swap[0] - g_lastEnd), gs = ms(g_swap[1] - g_swap[0]), vblank = ms(g_swap[2] - g_swap[1]);
        g_secFrames += 1;
        g_secInterval += interval;
        g_secGame += game;
        g_secGs += gs;
        g_secVblank += vblank;
        for (int i = 0; i < ps2x::expstats::kSites; ++i)
            g_secWait[i] += wait[i];
        if (interval > 1.5 * field && interval < 1000.0)
        {
            ++g_dips;
            // game: from the last flip to this swap (EE side, including its waits on the worker threads: vif queue full,
            // vif sync (register reads drain VU1 and GIF), gs queue full, gs drain); gs: the swap's own wait for the GS
            // path; vblank: the wait for the vertical blank. The waits cover the whole time since the last flip.
            std::fprintf(stderr, "[exp-dip] swap %llu (dip %llu) interval %.1f ms: game %.1f (waits: vifq %.1f vifsync %.1f gsq %.1f gsdrain %.1f), gs wait %.1f, vblank wait %.1f, field %llu\n",
                         static_cast<unsigned long long>(g_swaps), static_cast<unsigned long long>(g_dips), interval, game,
                         wait[0], wait[1], wait[2], wait[3], gs, vblank,
                         static_cast<unsigned long long>(g_runtime->eeScheduler().currentVSyncTick()));
        }
    }
    if (g_secondStart == Clock::time_point{})
        g_secondStart = now;
    if (now - g_secondStart >= std::chrono::seconds(1) && g_secFrames > 0)
    {
        const double n = g_secFrames;
        std::fprintf(stderr, "[exp-frame] %.0f frames, per frame ms: interval %.1f game %.1f (vifq %.1f vifsync %.1f gsq %.1f gsdrain %.1f) gs %.1f vblank %.1f\n",
                     n, g_secInterval / n, g_secGame / n, g_secWait[0] / n, g_secWait[1] / n, g_secWait[2] / n, g_secWait[3] / n,
                     g_secGs / n, g_secVblank / n);
        g_secFrames = g_secInterval = g_secGame = g_secGs = g_secVblank = 0;
        for (double &w : g_secWait)
            w = 0;
        g_secondStart = now;
    }
    g_lastStart = g_swap[0];
    g_lastEnd = now;
}

// ---------------------------------------------------------------------------------------------------------------------
// Frame interpolation

namespace interp {

namespace {

constexpr uint32_t kCameraPtr = sym::cameraVarG + 0xb70u; // cameraVarG.pCamera (the window's camera)
constexpr int kMaxObjects = 46;

struct CameraSnap
{
    uint32_t address = 0;
    int32_t pos[3];   // sCamera.pos (0xc)
    uint8_t mat[0x20]; // sCamera.matrix (0x24): 9 shorts (4.12), pad, 3 ints
};

struct ObjectSnap
{
    uint32_t object = 0, physics = 0, integer = 0;
    int32_t iPos[3];     // integer state renderPos (0x4)
    int16_t iMat[9];     // integer state mat (0x28)
    int16_t iWheelMat[9]; // integer state frontWheelMat (0x3c)
    float fPos[3];       // physics renderPos (0x8)
    float fMat[9];       // physics mat (0x4c)
    float wheel[4][3];   // physics wheel[i].pos (0xac + 0x48 i)
};

struct Snapshot
{
    bool valid = false;
    CameraSnap camera;
    int objects = 0;
    ObjectSnap object[kMaxObjects];
};

Snapshot g_last, g_current;
std::chrono::steady_clock::time_point g_vblank{};
uint64_t g_lastTick = 0;
int g_skipTicks = 0, g_failures = 0;
bool g_activeThisTick = false;
// Counters for RC_EXP_LOG.
uint64_t g_ticks = 0, g_drawn = 0, g_missed = 0, g_skipped = 0;
std::chrono::steady_clock::time_point g_logTime{};
// The 2D packet and chain the tick wrote before its in-between pictures.
std::vector<uint8_t> g_saved2D, g_savedChain;
int32_t g_saved2DTag = -1;

void copyIn(void *dst, uint32_t address, size_t n) { std::memcpy(dst, guest_host(address), n); }
void copyOut(uint32_t address, const void *src, size_t n) { std::memcpy(guest_host(address), src, n); }

void takeSnapshot(Snapshot &s)
{
    s.valid = true;
    const uint32_t cam = at<uint32_t>(kCameraPtr);
    s.camera.address = cam;
    if (cam)
    {
        copyIn(s.camera.pos, cam + 0xcu, 12);
        copyIn(s.camera.mat, cam + 0x24u, 0x20);
    }
    s.objects = 0;
    const uint32_t count = std::min<uint32_t>(at<uint32_t>(sym::objectVarG + 0x170u), kMaxObjects);
    for (uint32_t i = 0; i < count; ++i)
    {
        const uint32_t obj = at<uint32_t>(sym::objectVarG + 4u * i);
        if (!obj)
            continue;
        const uint32_t physics = at<uint32_t>(obj + 0x20u);
        if (!physics)
            continue;
        const uint32_t integer = at<uint32_t>(physics + 0x1e8u);
        if (!integer)
            continue;
        ObjectSnap &o = s.object[s.objects++];
        o.object = obj;
        o.physics = physics;
        o.integer = integer;
        copyIn(o.iPos, integer + 0x4u, 12);
        copyIn(o.iMat, integer + 0x28u, 18);
        copyIn(o.iWheelMat, integer + 0x3cu, 18);
        copyIn(o.fPos, physics + 0x8u, 12);
        copyIn(o.fMat, physics + 0x4cu, 36);
        for (int w = 0; w < 4; ++w)
            copyIn(o.wheel[w], physics + 0xacu + 0x48u * static_cast<uint32_t>(w), 12);
    }
}

void writeSnapshot(const Snapshot &s)
{
    if (s.camera.address && s.camera.address == at<uint32_t>(kCameraPtr))
    {
        copyOut(s.camera.address + 0xcu, s.camera.pos, 12);
        copyOut(s.camera.address + 0x24u, s.camera.mat, 0x20);
    }
    for (int i = 0; i < s.objects; ++i)
    {
        const ObjectSnap &o = s.object[i];
        copyOut(o.integer + 0x4u, o.iPos, 12);
        copyOut(o.integer + 0x28u, o.iMat, 18);
        copyOut(o.integer + 0x3cu, o.iWheelMat, 18);
        copyOut(o.physics + 0x8u, o.fPos, 12);
        copyOut(o.physics + 0x4cu, o.fMat, 36);
        for (int w = 0; w < 4; ++w)
            copyOut(o.physics + 0xacu + 0x48u * static_cast<uint32_t>(w), o.wheel[w], 12);
    }
}

int32_t lerpi(int32_t a, int32_t b, float t) { return a + static_cast<int32_t>(std::lround(static_cast<double>(b - a) * t)); }
int16_t lerps(int16_t a, int16_t b, float t) { return static_cast<int16_t>(lerpi(a, b, t)); }
float lerpf(float a, float b, float t) { return a + (b - a) * t; }

// A cut (respawn, camera change, teleport): a jump of more than `limit` in any axis.
template <typename T>
bool jumped(const T *a, const T *b, double limit)
{
    for (int i = 0; i < 3; ++i)
        if (std::abs(static_cast<double>(b[i]) - static_cast<double>(a[i])) > limit)
            return true;
    return false;
}

// The rows of a 4.12 rotation: more than ~45 degrees apart is a cut.
bool turned(const int16_t *a, const int16_t *b)
{
    double dot = 0;
    for (int i = 0; i < 9; ++i)
        dot += static_cast<double>(a[i]) * b[i];
    return dot < 3.0 * 4096.0 * 4096.0 * 0.7;
}

const ObjectSnap *find(const Snapshot &s, uint32_t object)
{
    for (int i = 0; i < s.objects; ++i)
        if (s.object[i].object == object)
            return &s.object[i];
    return nullptr;
}

} // namespace

void reset()
{
    g_last.valid = g_current.valid = false;
    g_lastTick = 0;
    g_skipTicks = 0;
    g_activeThisTick = false;
}

void capture()
{
    if (!g_config.interp)
        return;
    g_last = g_current;
    takeSnapshot(g_current);
}

bool active(bool paused)
{
    g_activeThisTick = false;
    if (!g_config.interp)
        return false;
    ++g_ticks;
    if (paused || !g_last.valid || !g_current.valid)
        return false;
    if (g_skipTicks > 0)
    {
        --g_skipTicks;
        ++g_skipped;
        return false;
    }
    g_activeThisTick = true;
    return true;
}

int passes() { return g_activeThisTick ? g_config.perTick - 1 : 0; }

void apply(float t)
{
    // Camera.
    const CameraSnap &a = g_last.camera, &b = g_current.camera;
    if (a.address && a.address == b.address && b.address == at<uint32_t>(kCameraPtr))
    {
        int16_t ma[9], mb[9], m[9];
        std::memcpy(ma, a.mat, 18);
        std::memcpy(mb, b.mat, 18);
        int32_t ta[3], tb[3], tr[3], p[3];
        std::memcpy(ta, a.mat + 0x14, 12);
        std::memcpy(tb, b.mat + 0x14, 12);
        if (!turned(ma, mb) && !jumped(a.pos, b.pos, 0x40000) && !jumped(ta, tb, 0x40000))
        {
            for (int i = 0; i < 9; ++i)
                m[i] = lerps(ma[i], mb[i], t);
            for (int i = 0; i < 3; ++i)
            {
                tr[i] = lerpi(ta[i], tb[i], t);
                p[i] = lerpi(a.pos[i], b.pos[i], t);
            }
            copyOut(b.address + 0x24u, m, 18);
            copyOut(b.address + 0x24u + 0x14u, tr, 12);
            copyOut(b.address + 0xcu, p, 12);
        }
    }
    if (!g_config.objects)
        return;
    for (int i = 0; i < g_current.objects; ++i)
    {
        const ObjectSnap &o = g_current.object[i];
        const ObjectSnap *l = find(g_last, o.object);
        if (!l || l->integer != o.integer || l->physics != o.physics)
            continue;
        if (jumped(l->iPos, o.iPos, 0x40000) || turned(l->iMat, o.iMat))
            continue;
        int32_t ip[3];
        int16_t im[9], iw[9];
        float fp[3], fm[9], wh[3];
        for (int k = 0; k < 3; ++k)
        {
            ip[k] = lerpi(l->iPos[k], o.iPos[k], t);
            fp[k] = lerpf(l->fPos[k], o.fPos[k], t);
        }
        for (int k = 0; k < 9; ++k)
        {
            im[k] = lerps(l->iMat[k], o.iMat[k], t);
            iw[k] = lerps(l->iWheelMat[k], o.iWheelMat[k], t);
            fm[k] = lerpf(l->fMat[k], o.fMat[k], t);
        }
        copyOut(o.integer + 0x4u, ip, 12);
        copyOut(o.integer + 0x28u, im, 18);
        copyOut(o.integer + 0x3cu, iw, 18);
        copyOut(o.physics + 0x8u, fp, 12);
        copyOut(o.physics + 0x4cu, fm, 36);
        for (int w = 0; w < 4; ++w)
        {
            for (int k = 0; k < 3; ++k)
                wh[k] = lerpf(l->wheel[w][k], o.wheel[w][k], t);
            copyOut(o.physics + 0xacu + 0x48u * static_cast<uint32_t>(w), wh, 12);
        }
    }
}

void restore() { writeSnapshot(g_current); }

void save2D()
{
    const sGfxP2 &gfx = G(gGfxP2);
    g_saved2D.resize(static_cast<size_t>(std::max(gfx.buffIdx, 0)) * 16u);
    if (!g_saved2D.empty())
        copyIn(g_saved2D.data(), gfx.pDmaBuff.address, g_saved2D.size());
    g_saved2DTag = gfx.gifTagIdx;
    const sRdr &rdr = G(gRdr);
    const int32_t base = (rdr.frameNumber & 1u) == 0 ? 0 : 1000;
    const int32_t n = std::max(rdr.currDmaTag - base, 0);
    g_savedChain.resize(static_cast<size_t>(n) * 16u);
    if (n)
        copyIn(g_savedChain.data(), rdr.pDmaTag.address + static_cast<uint32_t>(base) * 16u, g_savedChain.size());
}

void replay2D()
{
    sGfxP2 &gfx = G(gGfxP2);
    if (!g_saved2D.empty() && gfx.buffIdx == 0)
    {
        copyOut(gfx.pDmaBuff.address, g_saved2D.data(), g_saved2D.size());
        gfx.buffIdx = static_cast<int32_t>(g_saved2D.size() / 16u);
        gfx.gifTagIdx = g_saved2DTag;
    }
    sRdr &rdr = G(gRdr);
    if (!g_savedChain.empty())
    {
        copyOut(rdr.pDmaTag.address + static_cast<uint32_t>(rdr.currDmaTag) * 16u, g_savedChain.data(), g_savedChain.size());
        rdr.currDmaTag += static_cast<int32_t>(g_savedChain.size() / 16u);
    }
}

void paceWait(int k)
{
    if (!g_config.pace || g_vblank == std::chrono::steady_clock::time_point{})
        return;
    const double field = RC_VERSION_EU ? 1.0 / 50.0 : 1001.0 / 60000.0;
    const auto target = g_vblank + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                       std::chrono::duration<double>(field * k / g_config.perTick));
    // Short sleeps with a scheduling point between them, so interrupts and other EE threads due meanwhile still run.
    while (std::chrono::steady_clock::now() < target)
    {
        guest_checkpoint();
        const auto left = target - std::chrono::steady_clock::now();
        std::this_thread::sleep_for(std::min<std::chrono::steady_clock::duration>(left, std::chrono::microseconds(500)));
    }
}

void endFrameNoVSync()
{
    sRdr &rdr = G(gRdr);
    GfxP2_EndScene();
    // GfxP2_SwapBuffers without sceGsSyncV (and without counting a field in uiVSyncInc).
    guest_call(sym::sceGsSyncPath, 0, 0);
    guest_call(sym::GfxP2_RenderDmaSendActual);
    guest_call(sym::sceGsSyncPath, 0, 0);
    sceGsDBuff &dbuff = G(gGfxP2_DBuff);
    const uint32_t draw = guest_addr((rdr.frameNumber & 1u) != 0u ? &dbuff.draw1 : &dbuff.draw0);
    guest_call(sym::sceGsSetHalfOffset, draw, 0x800, 0x800, 0);
    guest_call(sym::sceGsSwapDBuff, guest_addr(&dbuff), rdr.frameNumber);
    ps2_host_display::notifyFlip();
    // The rest of Rdr_EndFrame, without GfxTex_Update (animated textures stay at the tick rate).
    const int32_t n = rdr.currDmaTag;
    const uint32_t tags = rdr.pDmaTag.address;
    rdr.currDmaTag = n + 1;
    sDmaTag &end = *ptr32<sDmaTag>(tags + static_cast<uint32_t>(n) * 16u);
    end.id = 0x70;
    end.qwc = 0;
    end.mark = 0;
    end.next = nullptr;
    guest_call(sym::sceGsSyncPath, 0, 0);
    const uint32_t chain = (rdr.frameNumber & 1u) == 0 ? rdr.pDmaTag.address : rdr.pDmaTag.address + 1000u * 16u;
    hw_write32(0x10003c20u, 2);          // VIF1_ERR
    hw_write32(0x10009020u, 0);          // D1_QWC
    hw_write32(0x10009030u, chain & 0x0fffffffu); // D1_TADR
    hw_write32(0x1000e010u, 2);          // D_STAT
    guest_call(sym::FlushCache, 0);
    hw_write32(0x10009000u, 0x145);      // D1_CHCR
    rdr.frameNumber = rdr.frameNumber + 1u;
    ++g_drawn;
}

void afterRealFrame()
{
    if (!g_config.interp)
        return;
    const auto now = std::chrono::steady_clock::now();
    g_vblank = now;
    const uint64_t tick = g_runtime->eeScheduler().currentVSyncTick();
    if (g_lastTick != 0 && tick - g_lastTick > 1)
    {
        ++g_missed;
        if (g_activeThisTick && !g_config.force)
        {
            g_failures = std::min(g_failures + 1, 6);
            g_skipTicks = 60 << g_failures; // back off: 2 s, 4 s, ... up to ~64 s
        }
    }
    else if (g_activeThisTick)
        g_failures = 0; // a good tick with in-between pictures: start the back-off over
    g_lastTick = tick;
    if (g_config.log && now - g_logTime > std::chrono::seconds(1))
    {
        std::fprintf(stderr, "[exp] ticks=%llu inbetween=%llu missed=%llu skipped=%llu skipNow=%d\n",
                     static_cast<unsigned long long>(g_ticks), static_cast<unsigned long long>(g_drawn),
                     static_cast<unsigned long long>(g_missed), static_cast<unsigned long long>(g_skipped), g_skipTicks);
        g_logTime = now;
    }
}

} // namespace interp

} // namespace rc::exp
