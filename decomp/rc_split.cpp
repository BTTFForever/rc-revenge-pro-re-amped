// Vertical split screen (rc_split.h).
#include "rc_decomp.h"
#include "rc_settings.h"
#include "rc_split.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace rc::split {

namespace {

template <typename T>
T &at(uint32_t address) { return *static_cast<T *>(guest_host(address)); }

bool splitByte() { return at<uint8_t>(sym::mainVarG + 0x368u) != 0; }

// The halves side by side, in pixels: x 3..0x13d and 0x142..0x27d (the PS2's 3-pixel border kept outside, a 4-pixel gap
// between them), centred 160 pixels either side of the screen's centre.
constexpr int kLeftEnd = 0x13d, kRightStart = 0x142;
constexpr float kHalfShift = 160.0f;

int g_lastVertical = -1;

bool logOn()
{
    static const bool on = [] { const char *v = std::getenv("RC_SPLIT_LOG"); return v && *v && *v != '0'; }();
    return on;
}

} // namespace

int view()
{
    const int32_t player = G(gRdrPolyPlayer);
    return player == 1 || player == 2 ? player : 0;
}

bool sideBySide() { return settings::splitVertical() && view() != 0 && splitByte(); }

bool raceSideBySide()
{
    return settings::splitVertical() && splitByte() && static_cast<uint32_t>(G(mainVarG).status) != 0u;
}

float zoom()
{
    const float half = settings::aspectRatio() * 0.5f;
    return std::min(1.0f, std::sqrt(half / (4.0f / 3.0f)));
}

float centreX()
{
    if (!sideBySide())
        return 2048.0f;
    return view() == 1 ? 2048.0f - kHalfShift : 2048.0f + kHalfShift;
}

void scaleProjection(float &dValX, float &dValY)
{
    const float z = zoom();
    dValX *= settings::hscale() * z;
    dValY *= z;
}

void halfAngles(float dValX, float dValY, float psHalf, float &sideHalf, float &topHalf)
{
    // A little over the half's 160 pixels and the screen's half height, so nothing at the edges is culled.
    const float tanSide = (kHalfShift + 4.0f) / dValX;
    const float tanTop = (static_cast<float>(kScreenLines / 2) + 4.0f) / dValY;
    sideHalf = std::atan(tanSide);
    topHalf = std::max(psHalf, std::atan(tanTop));
}

void viewCone(int16_t windowWidth, int16_t &halfX, int16_t &halfY)
{
    // The window is the PS2's top half (both halves are drawn with window 0's cone): its width is the screen's, its
    // height half of it. Side by side a half shows 1 / (2 hscale zoom) of the 4:3 screen's width and 1 / zoom of its
    // height; 5% more each way for the cells' edges.
    const float z = zoom();
    halfX = static_cast<int16_t>(-std::lround(static_cast<float>(windowWidth >> 1) / (2.0f * settings::hscale() * z) * 1.05f));
    halfY = static_cast<int16_t>(-std::lround(static_cast<float>(kScreenLines / 2) / z * 1.05f));
}

bool scissor(int window, int &x1, int &y1, int &x2, int &y2)
{
    if (!settings::splitVertical() || !splitByte() || window == 2)
        return false;
    y1 = 3;
    y2 = kScreenLines - 3;
    x1 = window == 0 ? 3 : kRightStart;
    x2 = window == 0 ? kLeftEnd : 0x27d;
    return true;
}

void frameWindow(int window)
{
    if (window == 2 || !splitByte())
        return;
    const int vertical = settings::splitVertical() ? 1 : 0;
    if (vertical != g_lastVertical)
    {
        // The view cone (View3dInitViewconeReject) is rebuilt only when the window or the projection distance changes.
        G(init3dVarG).winStore = nullptr;
        if (logOn())
            std::fprintf(stderr, "[split] %s, zoom %.3f, aspect %.3f\n", vertical ? "side by side" : "one above the other", zoom(), settings::aspectRatio());
        g_lastVertical = vertical;
    }
}

} // namespace rc::split
