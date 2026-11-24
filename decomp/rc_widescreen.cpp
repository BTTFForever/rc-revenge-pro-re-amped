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
// GS coordinates in 12.4 fixed point: the screen's left edge, centre and right edge (the 2D offset is 2048 - 320), and
// its top (the menus' 2D origin and the frame's first line: the HUD's origin is 8 lines lower in USA), vertical centre
// (the line between a split screen's halves: the 3D scissors end the top half at line 0x70, Europe 0x80) and half height.
constexpr int32_t kLeftX = 0x6c0 << 4, kCentreX = 0x800 << 4, kRightX = (0x6c0 + 640) << 4;
constexpr int32_t kScreenTopY = 0x780 << 4, kHalfLines = (kScreenLines / 2) << 4, kCentreY = kScreenTopY + kHalfLines;
constexpr uint32_t kAuto = 11; // AnchorScope::kAuto

int32_t anchorX(uint32_t anchor)
{
    switch (anchor)
    {
    case 1: case 7: case 9: return kLeftX;    // left, top left, bottom left
    case 2: case 8: case 10: return kRightX;  // right, top right, bottom right
    case 3: case 4: case 5: case kAuto: return kCentreX; // top, bottom, centre (auto: widescreen about the centre)
    default: return 0;                        // stretch, none
    }
}

// -1 top, 0 middle, 1 bottom of the region.
int anchorRow(uint32_t anchor)
{
    switch (anchor)
    {
    case 3: case 7: case 8: return -1;
    case 4: case 9: case 10: return 1;
    default: return 0;
    }
}

struct Walker
{
    uint32_t buffer;
    int32_t end;
    float kx = 1.0f, bx = 0.0f, ky = 1.0f, by = 0.0f; // x' = kx x + bx, y' = ky y + by

    uint64_t *qw(int32_t i) const { return static_cast<uint64_t *>(guest_host(buffer + static_cast<uint32_t>(i) * 16u)); }

    bool measure = false;
    mutable int32_t minX = 0x10000, maxX = -1, minY = 0x10000, maxY = -1;

    void fix(uint64_t &low) const
    {
        const int32_t x = static_cast<int32_t>(low & 0xffffu);
        const int32_t y = static_cast<int32_t>((low >> 32) & 0xffffu);
        if (measure)
        {
            minX = std::min(minX, x);
            maxX = std::max(maxX, x);
            minY = std::min(minY, y);
            maxY = std::max(maxY, y);
            return;
        }
        const int32_t nx = std::clamp(static_cast<int32_t>(std::floor(kx * static_cast<float>(x) + bx + 0.5f)), 0, 0xffff);
        const int32_t ny = std::clamp(static_cast<int32_t>(std::floor(ky * static_cast<float>(y) + by + 0.5f)), 0, 0xffff);
        low = (low & ~0x0000ffff0000ffffull) | static_cast<uint64_t>(nx) | (static_cast<uint64_t>(ny) << 32);
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

int g_scopeDepth = 0;              // active or blocking anchor scopes open
int g_raceMenu = 0;                // RaceMenuScopes open
int g_menuRow = 0;                 // MenuRowScope: where the menus scale toward (-1 top, 0 centre, 1 bottom)
AnchorScope *g_outermost = nullptr; // the scope that applies (for a packet sent inside it)

bool splitScreen() { return at<uint8_t>(sym::mainVarG + 0x368u) != 0; }
// A race's own 2D (not the front end, whose loop runs with status 0, nor a race's full-screen menus).
bool raceNonMenu() { return static_cast<uint32_t>(G(mainVarG).status) != 0u && g_raceMenu == 0; }

// A scale s about a point a after the widescreen scale h about w: x' = s (w + (x - w) h) + a (1 - s).
void compose(float &k, float &b, float h, float w, float s, float a)
{
    k = s * h;
    b = s * w * (1.0f - h) + a * (1.0f - s);
}

} // namespace

RaceMenuScope::RaceMenuScope() { ++g_raceMenu; }
RaceMenuScope::~RaceMenuScope() { --g_raceMenu; }
MenuRowScope::MenuRowScope(int row) : m_saved(g_menuRow) { g_menuRow = row; }
MenuRowScope::~MenuRowScope() { g_menuRow = m_saved; }

AnchorScope::AnchorScope(uint32_t anchor, Kind kind) : m_kind(kind), m_anchor(anchor)
{
    if (g_scopeDepth > 0)
        return;
    // Split screen (mainVarG.splitScreen, +0x368): each half's HUD keeps the original widescreen layout, its sprites and
    // text included (drawn through the Ui paths). The byte is already set in the front end once two players are chosen,
    // where the menus (the two-player vehicle select) are pillarboxed like any other; so it counts for the HUD, and for
    // the 2D drawn while a race runs except the race's full-screen menus (RaceMenuScope). The HUD and menu scales apply
    // in split screen too, each half's HUD toward its own half's edges.
    const bool split = splitScreen();
    if (kind == Hud)
    {
        m_widescreen = settings::hudAnchor() && !split;
        m_scale = settings::hudScale();
        m_split = split;
    }
    else
    {
        const bool race = raceNonMenu();
        m_widescreen = settings::uiPillarbox() && !(split && race);
        m_scale = race ? settings::hudScale() : settings::menuScale();
        m_split = split && race;
        m_menuRow = race ? 0 : g_menuRow;
    }
    if (!m_widescreen && m_scale >= 1.0f)
        return;
    if (anchorX(anchor) == 0)
        return;
    const sGfxP2 &gfx = G(gGfxP2);
    m_on = m_counted = true;
    ++g_scopeDepth;
    g_outermost = this;
    m_buffer = gfx.pDmaBuff.address;
    m_tag = gfx.gifTagIdx;
    m_start = gfx.buffIdx;
}

void AnchorScope::apply()
{
    const sGfxP2 &gfx = G(gGfxP2);
    if (gfx.pDmaBuff.address != m_buffer || gfx.buffIdx < m_start)
        return; // the packet was sent inside the scope (and not through packetSending)
    Walker w{m_buffer, gfx.buffIdx};
    const auto walk = [&]
    {
        int32_t pos = m_start;
        if (m_tag != -1 && m_tag < m_start)
            pos = w.tag(m_tag, m_start);
        for (int guard = 0; pos < w.end && guard < 100000; ++guard)
            pos = w.tag(pos, pos + 1);
    };
    w.measure = true;
    walk();
    w.measure = false;
    if (w.maxX < 0)
        return;
    // Outside the screen as drawn (the front end's version number "0.13R-NTSC" above the top, the HUD's "." that sets
    // up the font, panels before they slide in): left where it is, so no scale brings it into view.
    if (w.maxX <= kLeftX || w.minX >= kRightX || w.maxY <= kScreenTopY || w.minY >= kCentreY + kHalfLines)
        return;
    const bool wide = w.maxX - w.minX >= (620 << 4);
    // A full-screen background or fade (edge to edge, most of the height): kept filling. A wide menu border or panel
    // is pillarboxed with its text.
    if (m_kind == Ui && wide && w.maxY - w.minY >= (200 << 4))
        return;
    const float hs = m_widescreen ? settings::hscale() : 1.0f;
    // A bar across the whole screen (the split screen's divider, say) keeps its width; only the widescreen scale.
    const float s = m_kind == Ui && wide ? 1.0f : m_scale;
    if (hs >= 1.0f && s >= 1.0f)
        return;
    const int32_t wsAnchor = anchorX(m_anchor);
    // The region the element scales within: the screen, or in split screen the half it is in (for one across the
    // line between them, as the map is, toward that line).
    int32_t top = kCentreY - kHalfLines, bottom = kCentreY + kHalfLines;
    const int32_t midY = (w.minY + w.maxY) / 2;
    const bool across = m_split && w.minY < kCentreY - (8 << 4) && w.maxY > kCentreY + (8 << 4);
    if (m_split && !across)
        (midY < kCentreY ? bottom : top) = kCentreY;
    float ax, ay;
    if (m_kind == Ui)
    {
        ax = static_cast<float>(kCentreX);
        const int row = m_split ? 0 : m_menuRow;
        ay = row < 0 ? static_cast<float>(top) : row > 0 ? static_cast<float>(bottom) : static_cast<float>(top + bottom) * 0.5f;
    }
    else
    {
        int row = anchorRow(m_anchor);
        int32_t x = wsAnchor;
        if (m_anchor == kAuto)
        {
            // From where it is: the nearest edge in thirds of the screen and of the region.
            const float midX = static_cast<float>(wsAnchor) + (static_cast<float>(w.minX + w.maxX) * 0.5f - static_cast<float>(wsAnchor)) * hs;
            const float third = static_cast<float>(kRightX - kLeftX) / 3.0f;
            x = midX < static_cast<float>(kLeftX) + third ? kLeftX : midX > static_cast<float>(kRightX) - third ? kRightX : kCentreX;
            const float rowThird = static_cast<float>(bottom - top) / 3.0f;
            row = static_cast<float>(midY) < static_cast<float>(top) + rowThird ? -1 : static_cast<float>(midY) > static_cast<float>(bottom) - rowThird ? 1 : 0;
        }
        if (across)
            row = 0; // toward the line between the halves
        ax = static_cast<float>(x);
        ay = row < 0 ? static_cast<float>(top) : row > 0 ? static_cast<float>(bottom) : static_cast<float>(top + bottom) * 0.5f;
    }
    compose(w.kx, w.bx, hs, static_cast<float>(wsAnchor), s, ax);
    compose(w.ky, w.by, 1.0f, 0.0f, s, ay);
    walk();
}

AnchorScope::~AnchorScope()
{
    if (m_counted)
        --g_scopeDepth;
    if (!m_on)
        return;
    if (g_outermost == this)
        g_outermost = nullptr;
    apply();
}

void packetSending()
{
    if (g_outermost)
        g_outermost->apply();
}

void packetSent()
{
    if (AnchorScope *s = g_outermost)
    {
        const sGfxP2 &gfx = G(gGfxP2);
        s->m_buffer = gfx.pDmaBuff.address;
        s->m_start = gfx.buffIdx;
        s->m_tag = -1;
    }
}

bool menuScissor(int32_t &x1, int32_t &y1, int32_t &x2, int32_t &y2)
{
    if (g_scopeDepth > 0 || (splitScreen() && raceNonMenu()))
        return false;
    const float hs = settings::uiPillarbox() ? settings::hscale() : 1.0f;
    const float s = raceNonMenu() ? settings::hudScale() : settings::menuScale();
    if (hs >= 1.0f && s >= 1.0f)
        return false;
    // The scissor is in pixels from the menus' 2D origin (0x6c0, 0x780).
    float kx, bx, ky, by;
    compose(kx, bx, hs, static_cast<float>(kCentreX), s, static_cast<float>(kCentreX));
    const float ay = g_menuRow < 0 ? static_cast<float>(kCentreY - kHalfLines) : g_menuRow > 0 ? static_cast<float>(kCentreY + kHalfLines) : static_cast<float>(kCentreY);
    compose(ky, by, 1.0f, 0.0f, s, raceNonMenu() ? static_cast<float>(kCentreY) : ay);
    const auto mapX = [&](int32_t v) { return (kx * static_cast<float>((v + 0x6c0) << 4) + bx) / 16.0f - 0x6c0; };
    const auto mapY = [&](int32_t v) { return (ky * static_cast<float>((v + 0x780) << 4) + by) / 16.0f - 0x780; };
    x1 = static_cast<int32_t>(std::floor(mapX(x1)));
    y1 = static_cast<int32_t>(std::floor(mapY(y1)));
    x2 = static_cast<int32_t>(std::ceil(mapX(x2)));
    y2 = static_cast<int32_t>(std::ceil(mapY(y2)));
    return true;
}

} // namespace rc::widescreen
