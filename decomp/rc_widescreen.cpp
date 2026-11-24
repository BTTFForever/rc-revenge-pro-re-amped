// Widescreen (rc_widescreen.h), ported from the exp/quick-dirty experiment (EXPERIMENT.md sections 1, rounds 2-4).
#include "rc_decomp.h"
#include "rc_settings.h"
#include "rc_widescreen.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace rc::widescreen {

namespace {
template <typename T>
T &at(uint32_t address) { return *static_cast<T *>(guest_host(address)); }
} // namespace

float widenHalfAngle(float half)
{
    return std::atan(std::tan(half) / settings::hscale());
}

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
int g_raceMenu = 0;   // RaceMenuScopes open
}

RaceMenuScope::RaceMenuScope() { ++g_raceMenu; }
RaceMenuScope::~RaceMenuScope() { --g_raceMenu; }

AnchorScope::AnchorScope(uint32_t anchor, Kind kind) : m_kind(kind)
{
    if (!settings::widescreen() || g_scopeDepth > 0)
        return;
    if (!(kind == Hud ? settings::hudAnchor() : settings::uiPillarbox()))
        return;
    // Not in a split-screen race (mainVarG.splitScreen, +0x368): each half's HUD keeps the original layout, its
    // sprites and text included (drawn through the Ui paths). The byte is already set in the front end once two players
    // are chosen, where the menus (the two-player vehicle select) are pillarboxed like any other; so it counts for the
    // HUD, and for the 2D drawn while a race runs (status not 0: the front end's loop runs with status 0) except the
    // race's full-screen menus (RaceMenuScope).
    if (at<uint8_t>(sym::mainVarG + 0x368u) != 0 &&
        (kind == Hud || (static_cast<uint32_t>(G(mainVarG).status) != 0u && g_raceMenu == 0)))
        return;
    m_anchorX = anchorX(anchor);
    if (m_anchorX == 0)
        return;
    const sGfxP2 &gfx = G(gGfxP2);
    m_on = m_counted = true;
    m_scale = settings::hscale();
    ++g_scopeDepth;
    m_buffer = gfx.pDmaBuff.address;
    m_tag = gfx.gifTagIdx;
    m_start = gfx.buffIdx;
}

AnchorScope::~AnchorScope()
{
    if (m_counted)
        --g_scopeDepth;
    if (!m_on)
        return;
    const sGfxP2 &gfx = G(gGfxP2);
    if (gfx.pDmaBuff.address != m_buffer || gfx.buffIdx < m_start)
        return; // the packet was sent inside the scope
    Walker w{m_buffer, m_anchorX, gfx.buffIdx, m_scale};
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

// In a version where a HUD or 2D function's native is not registered (Europe: HudMenuBorder, HudSprite*, the HUD
// corners and more differ in a constant, so the recompiled ones run), the scope wraps the recompiled function instead.
namespace {
template <const FnInfo &F, uint32_t Anchor, int Kind>
void scopedOriginal(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const AnchorScope scope(Anchor, static_cast<AnchorScope::Kind>(Kind));
    F.original(rdram, ctx, runtime);
}

template <const FnInfo &F, uint32_t Anchor, int Kind>
int wrapIfRecompiled(PS2Runtime &runtime)
{
    if constexpr (!registeredHere<F>)
        return runtime.replaceFunction(F.address, &scopedOriginal<F, Anchor, Kind>) ? 1 : 0;
    else
        return 0;
}
} // namespace

void installScopeWrappers(PS2Runtime &runtime)
{
    constexpr int Ui = AnchorScope::Ui, Hud = AnchorScope::Hud;
    int n = 0;
    n += wrapIfRecompiled<fns::HudMenuBorder, 5, Ui>(runtime);
    n += wrapIfRecompiled<fns::HudSprite, 5, Ui>(runtime);
    n += wrapIfRecompiled<fns::HudSpriteMirror, 5, Ui>(runtime);
    n += wrapIfRecompiled<fns::HudSpriteHI, 5, Ui>(runtime);
    n += wrapIfRecompiled<fns::HudRotSpriteHI, 5, Ui>(runtime);
    n += wrapIfRecompiled<fns::HudWrongWayX, 5, Ui>(runtime);
    n += wrapIfRecompiled<fns::HudLost, 5, Ui>(runtime);
    n += wrapIfRecompiled<fns::HudTopRight, 8, Hud>(runtime);
    n += wrapIfRecompiled<fns::HudBottomRight, 10, Hud>(runtime);
    n += wrapIfRecompiled<fns::HudBottomLeft, 9, Hud>(runtime);
    n += wrapIfRecompiled<fns::HudMiddle, 5, Hud>(runtime);
    n += wrapIfRecompiled<fns::SpriteRenderFast, 5, Ui>(runtime);
    n += wrapIfRecompiled<fns::SpriteRender, 5, Ui>(runtime);
    n += wrapIfRecompiled<fns::SpriteRenderHalf, 5, Ui>(runtime);
    n += wrapIfRecompiled<fns::SpriteRenderScaled, 5, Ui>(runtime);
    n += wrapIfRecompiled<fns::SpriteRenderNew, 5, Ui>(runtime);
    n += wrapIfRecompiled<fns::SpriteRenderFont, 5, Ui>(runtime);
    n += wrapIfRecompiled<fns::RectRender, 5, Ui>(runtime);
    n += wrapIfRecompiled<fns::RectRenderGouraud, 5, Ui>(runtime);
    n += wrapIfRecompiled<fns::LineRender2D, 5, Ui>(runtime);
    if (n)
        std::fprintf(stderr, "[widescreen] %d recompiled HUD/2D functions wrapped in anchor scopes\n", n);
}


} // namespace rc::widescreen
