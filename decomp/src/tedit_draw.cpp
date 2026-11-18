// Decompiled from TEDIT.c: the grid editor's drawing: the cursor's slope and height panel, the track's tiles around
// the cursor, the 3D grid lines and the edit menu's three rows of pictures. See tedit.h.

#include "tedit.h"
#include "tedit_fuzz.h"

RC_VERSION_VARIANT(TEditDEBUGDisplayCursor)

namespace rc {

using namespace tedit;

namespace {

// sSpriteParams and sRect in a frame.
constexpr uint32_t kFlags = 0x0u, kX = 0x4u, kY = 0x6u, kXOffset = 0x8u, kYOffset = 0xau, kWidth = 0xcu, kHeight = 0xeu;
constexpr uint32_t kRed = 0x10u, kGreen = 0x11u, kBlue = 0x12u, kXScale = 0x14u, kYScale = 0x16u;
constexpr uint32_t kX0 = 0x4u, kY0 = 0x6u, kX1 = 0x8u, kY1 = 0xau, kX2 = 0xcu, kY2 = 0xeu, kX3 = 0x10u, kY3 = 0x12u;
constexpr uint32_t kRectRed = 0x14u, kRectGreen = 0x15u, kRectBlue = 0x16u;

void set16(uint32_t address, int32_t value) { at<uint16_t>(address) = static_cast<uint16_t>(value); }

void rectRender(uint32_t rect, uint32_t depth) { guest_call(sym::RectRender, rect, at<ee_float>(depth)); }

void spriteScaled(uint32_t picture, uint32_t sprite, uint32_t depth)
{
    guest_call(sym::SpriteRenderScaled, at<uint32_t>(te().pTrackSprites.address + picture), sprite, at<ee_float>(depth));
}

void setColour(uint32_t sprite, uint32_t red, uint32_t green, uint32_t blue)
{
    at<uint8_t>(sprite + kRed) = static_cast<uint8_t>(red);
    at<uint8_t>(sprite + kGreen) = static_cast<uint8_t>(green);
    at<uint8_t>(sprite + kBlue) = static_cast<uint8_t>(blue);
}

// The part of the grid drawn: three units around the cursor, or all of it in the far view.
struct Window
{
    int32_t xStart, xStop, yStart, yStop;
};

Window window()
{
    const sTEditClass &t = te();
    const uint32_t xb = static_cast<uint8_t>(t.cursor.xPos), yb = static_cast<uint8_t>(t.cursor.yPos);
    Window w;
    w.xStart = static_cast<int8_t>(xb - 3u);
    w.xStop = static_cast<int8_t>(xb + 3u);
    if (!(-1 < w.xStart))
        w.xStart = 0;
    if (!(w.xStop < t.cursor.xMax))
        w.xStop = static_cast<int8_t>(static_cast<uint8_t>(t.cursor.xMax) - 1u);
    w.yStart = static_cast<int8_t>(yb - 3u);
    w.yStop = static_cast<int8_t>(yb + 3u);
    if (!(-1 < w.yStart))
        w.yStart = 0;
    if (!(w.yStop < t.cursor.yMax))
        w.yStop = static_cast<int8_t>(static_cast<uint8_t>(t.cursor.yMax) - 1u);
    if (t.trackState.currentMode == 3)
    {
        w.xStart = 0;
        w.yStart = 0;
        w.xStop = static_cast<int8_t>(static_cast<uint8_t>(t.cursor.xMax) - 1u);
        w.yStop = static_cast<int8_t>(static_cast<uint8_t>(t.cursor.yMax) - 1u);
    }
    return w;
}

} // namespace

// The panel at the top left: the cursor's slope, whether it is above or below ground, and its height in levels.
void TEditDEBUGDisplayCursor()
{
#if RC_VERSION_EU
    constexpr int32_t kTop = 0x1c, kBottom = 0x2c, kSignTop = 0x1e, kSignBottom = 0x2a;
#else
    constexpr int32_t kTop = 0x26, kBottom = 0x36, kSignTop = 0x28, kSignBottom = 0x34;
#endif
    GuestFrame frame(0xc0);
    const uint32_t sprite = frame.local<uint8_t>(0).address, rect = sprite + 0x20u;
    const sTEditClass &t = te();
    const auto columns = [&](int32_t left, int32_t right)
    {
        set16(rect + kX0, left);
        set16(rect + kX1, right);
        set16(sprite + kX, left);
        set16(rect + kX3, left);
        set16(rect + kX2, right);
    };
    const auto rows = [&](int32_t top, int32_t bottom)
    {
        set16(rect + kY0, top);
        set16(rect + kY2, bottom);
        set16(sprite + kY, top);
        set16(rect + kY1, top);
        set16(rect + kY3, bottom);
    };
    set16(sprite + kXScale, 0x100);
    set16(sprite + kYScale, 0x80);
    set16(sprite + kHeight, 0x20);
    at<uint32_t>(rect) = 1;
    columns(0x14, 0x34);
    rows(kTop, kBottom);
    set16(sprite + kXOffset, 0);
    set16(sprite + kYOffset, 0);
    set16(sprite + kWidth, 0x20);
    setColour(sprite, 0x48, 0x48, 0x48);
    at<uint32_t>(sprite + kFlags) = 1;
    at<uint8_t>(rect + kRectBlue) = 0;
    at<uint8_t>(rect + kRectGreen) = 0;
    at<uint8_t>(rect + kRectRed) = 0;
    rectRender(rect, sym::lit4_3276f0);
    const int32_t slope = t.cursor.currentSlope;
    if (slope < 0)
        spriteScaled(0xb0, sprite, sym::lit4_3276f4);
    else if (slope > 0)
        spriteScaled(0xa8, sprite, sym::lit4_3276f8);
    else
        spriteScaled(0xac, sprite, sym::lit4_3276fc);
    columns(0x94, 0xb4);
    rectRender(rect, sym::lit4_327700);
    if (t.cursor.elevation == 0)
        spriteScaled(0xac, sprite, sym::lit4_327704);
    else if (t.cursor.elevation > 0)
        spriteScaled(0xb4, sprite, sym::lit4_327708);
    else
        spriteScaled(0xb8, sprite, sym::lit4_32770c);
    columns(0x50, 0x90);
    rectRender(rect, sym::lit4_327710);
    spriteScaled(0x9c, sprite, sym::lit4_327714);
    set16(sprite + kX, 0x70);
    spriteScaled(0xa4, sprite, sym::lit4_327714);
    const int32_t elevation = t.cursor.elevation;
    set16(sprite + kHeight, 0x18);
    columns(0x5c, 0x68);
    rows(kSignTop, kSignBottom);
    set16(sprite + kWidth, 0xc);
    if (elevation > 0)
    {
        rectRender(rect, sym::lit4_327718);
        spriteScaled(0xbc, sprite, sym::lit4_32771c);
    }
    else if (elevation < 0)
    {
        rectRender(rect, sym::lit4_327720);
        spriteScaled(0xc0, sprite, sym::lit4_327724);
    }
    // The height in levels of 256: its last digit at 0x7c, then what is left of it (not reduced to a digit) before.
    int32_t levels = t.cursor.elevation >> 8;
    if (levels < 0)
        levels = -levels;
    const uint32_t value = static_cast<uint32_t>(levels) & 0xffffu;
    set16(sprite + kX, 0x7c);
    set16(sprite + kWidth, 0x14);
    uint32_t digit = (value % 10u) & 0xffffu;
    for (uint32_t count = 0;;)
    {
        const uint32_t x = at<uint16_t>(sprite + kX);
        set16(rect + kX3, static_cast<int32_t>(x));
        set16(rect + kX1, static_cast<int32_t>(x + 0x14u));
        set16(rect + kX0, static_cast<int32_t>(x));
        set16(rect + kX2, static_cast<int32_t>(x + 0x14u));
        rectRender(rect, sym::lit4_327728);
        spriteScaled((digit + 0x1du) * 4u, sprite, sym::lit4_32772c);
        digit = (value / 10u) & 0xffffu;
        const uint32_t right = at<uint16_t>(sprite + kX), left = right - 0x14u;
        set16(sprite + kX, static_cast<int32_t>(left));
        if (digit == 0)
            return;
        count = (count + 1u) & 0xffffu;
        if (count >= 2u)
            return;
        set16(rect + kX0, static_cast<int32_t>(left));
        set16(rect + kX1, static_cast<int32_t>(right));
        set16(rect + kX3, static_cast<int32_t>(left));
        set16(rect + kX2, static_cast<int32_t>(right));
        rectRender(rect, sym::lit4_327730);
        spriteScaled((digit + 0x1du) * 4u, sprite, sym::lit4_327734);
        set16(sprite + kX, static_cast<int32_t>(at<uint16_t>(sprite + kX) - 0x14u));
        count = (count + 1u) & 0xffffu;
        if (count >= 2u)
            return;
    }
}

// The track's tiles: every unit of the three layers inside the window.
void TEditRenderTrack()
{
    GuestFrame frame(0xf0);
    sTEditClass &t = te();
    const uint32_t theme = t.trackDesc.themeTypeNdx;
    const uint32_t tiles = at<uint32_t>(kTE + 0x44u + theme * 4u);
    const Window w = window();
    G(tileVarG).pTrackTiles = ptr32<_sTrackTiles>(tiles);
    uint32_t unit = t.track.pTrackUnits.address;
    for (int32_t layer = 0; layer < 3; ++layer)
        for (int32_t y = 0; y < 16; ++y)
            for (int32_t x = 0; x < 16; ++x, unit += 8u)
                if (at<int16_t>(unit) != -1 && !(x < w.xStart) && !(w.xStop < x) && !(y < w.yStart) && !(w.yStop < y))
                    guest_call(sym::TileRender, x, y, unit, theme);
}

// The grid's lines on the ground around the cursor (in scratchPadG, an sLine3D): blue, the grid's edges red.
void TEditDisplay3DGridLines()
{
    GuestFrame frame(0xe0);
    const sTEditClass &t = te();
    const Window w = window();
    const int32_t across = static_cast<int8_t>(w.xStop - w.xStart + 1), down = static_cast<int8_t>(w.yStop - w.yStart + 1);
    const uint32_t line = sym::scratchPadG;
    const int32_t x0 = (w.xStart << 11) - 0x400, x1 = ((w.xStart + 1) << 11) - 0x400;
    const int32_t z0 = (w.yStart << 11) - 0x400, z1 = ((w.yStart + 1) << 11) - 0x400;
    const auto colour = [&](bool edge)
    {
        at<uint8_t>(line + 0x14u) = edge ? 0x80 : 0;
        at<uint8_t>(line + 0x15u) = edge ? 0 : 0x20;
        at<uint8_t>(line + 0x16u) = edge ? 0 : 0x80;
    };
    const auto step = [&](uint32_t offset) { at<uint16_t>(line + offset) = static_cast<uint16_t>(at<uint16_t>(line + offset) + 0x800u); };
    at<uint32_t>(line) = 4;
    set16(line + 0x8u, z0);
    set16(line + 0xcu, x0);
    set16(line + 0x10u, z1);
    set16(line + 0x4u, x0);
    set16(line + 0x6u, 0);
    set16(line + 0xeu, 0);
    if (across >= 0)
        for (int8_t i = 0; i <= across; i = static_cast<int8_t>(i + 1))
        {
            colour((w.xStart == 0 && i == 0) || (w.xStop == t.cursor.xMax - 1 && i == across));
            for (int8_t j = 0; j < down; j = static_cast<int8_t>(j + 1))
            {
                guest_call(sym::LineRender3D, line);
                step(0x8u);
                step(0x10u);
            }
            step(0x4u);
            step(0xcu);
            set16(line + 0x10u, z1);
            set16(line + 0x8u, z0);
        }
    set16(line + 0x6u, 0);
    set16(line + 0x4u, x0);
    set16(line + 0xcu, x1);
    set16(line + 0xeu, 0);
    set16(line + 0x10u, z0);
    set16(line + 0x8u, z0);
    if (down < 0)
        return;
    for (int8_t i = 0; i <= down; i = static_cast<int8_t>(i + 1))
    {
        colour((w.yStart == 0 && i == 0) || (w.yStop == t.cursor.yMax - 1 && i == down));
        for (int8_t j = 0; j < across; j = static_cast<int8_t>(j + 1))
        {
            guest_call(sym::LineRender3D, line);
            step(0x4u);
            step(0xcu);
        }
        step(0x8u);
        set16(line + 0x4u, x0);
        step(0x10u);
        set16(line + 0xcu, x1);
    }
}

// The edit menu: the selected slope's seven modules, the five slopes, the two rows of specials. A cell that cannot be
// chosen is dark on black, the others grey on black; the chosen one of each row has no box, and pulses orange in the
// row the cursor is on (a 1.2 s cycle).
void TEditRenderEditMenu(tTime time)
{
    GuestFrame frame(0x100);
    const uint32_t sprite = frame.local<uint8_t>(0).address, rect = sprite + 0x20u;
    const sTEditClass &t = te();
    const uint32_t startX = t.editMenu.screenXPos;
    const uint32_t phase = static_cast<uint32_t>(static_cast<int32_t>(time) % 0x4b0);
    const uint32_t pulse = phase < 600u ? 0x8cu * phase / 600u + 0x5au : 0xe4u - 0x8cu * (phase - 600u) / 600u;
    const uint32_t startY = t.editMenu.screenYPos;
    const uint32_t cells = kGridSlopes + t.editMenu.slopeNdx * 0x2cu;
    set16(sprite + kYScale, 0x80);
    set16(sprite + kXScale, 0x100);
    at<uint32_t>(rect) = 1;
    set16(sprite + kHeight, 0x20);
    set16(sprite + kWidth, 0x20);
    set16(sprite + kXOffset, 0);
    set16(sprite + kYOffset, 0);
    set16(sprite + kY, static_cast<int32_t>(startY));
    at<uint8_t>(rect + kRectBlue) = 0;
    at<uint8_t>(rect + kRectGreen) = 0;
    at<uint8_t>(rect + kRectRed) = 0;
    const auto place = [&](uint32_t column)
    {
        const uint32_t x = startX + column * 0x24u, y = at<uint16_t>(sprite + kY);
        set16(rect + kY2, static_cast<int32_t>(y + 0x10u));
        set16(rect + kY1, static_cast<int32_t>(y));
        set16(sprite + kX, static_cast<int32_t>(x));
        set16(rect + kX1, static_cast<int32_t>(x + 0x20u));
        set16(rect + kX3, static_cast<int32_t>(x));
        set16(rect + kX0, static_cast<int32_t>(x));
        set16(rect + kX2, static_cast<int32_t>(x + 0x20u));
        set16(rect + kY0, static_cast<int32_t>(y));
        set16(rect + kY3, static_cast<int32_t>(y + 0x10u));
    };
    // A cell with its box: `shade` 0x15 if it cannot be chosen, 0x48 if it can.
    const auto boxed = [&](uint32_t picture, uint32_t shade, uint32_t boxDepth, uint32_t depth)
    {
        rectRender(rect, boxDepth);
        setColour(sprite, shade, shade, shade);
        at<uint32_t>(sprite + kFlags) = 1;
        spriteScaled(picture * 4u, sprite, depth);
    };
    const auto pulsing = [&] { setColour(sprite, pulse, pulse >> 1, 0); };
    for (uint32_t i = 0; i < 7u; ++i)
    {
        place(i);
        if (at<int8_t>(cells + 0x1cu + i) == 0 || at<int8_t>(cells + 0x23u + i) != 0)
            boxed(i + 3u, 0x15, sym::lit4_32775c, sym::lit4_327760);
        else if (t.editMenu.moduleNdx == i)
        {
            at<uint32_t>(sprite + kFlags) = 0;
            setColour(sprite, 0x80, 0x80, 0x80);
            if (t.editMenu.rowNdx == 0)
                pulsing();
            spriteScaled((i + 3u) * 4u, sprite, sym::lit4_327764);
        }
        else
            boxed(i + 3u, 0x48, sym::lit4_327768, sym::lit4_32776c);
    }
    uint32_t y = (startY + 0x12u) & 0xffffu;
    set16(sprite + kY, static_cast<int32_t>(y));
    for (uint32_t i = 0; i < 5u; ++i)
    {
        place(i + 1u);
        if (t.editMenu.slopeRestrict[i] != 0)
            boxed(i + 10u, 0x15, sym::lit4_327770, sym::lit4_327774);
        else if (t.editMenu.slopeNdx == i)
        {
            at<uint32_t>(sprite + kFlags) = 0;
            setColour(sprite, 0x80, 0x80, 0x80);
            if (t.editMenu.rowNdx == 1)
                pulsing();
            spriteScaled((i + 10u) * 4u, sprite, sym::lit4_327778);
        }
        else
            boxed(i + 10u, 0x48, sym::lit4_32777c, sym::lit4_327780);
    }
    y = (y + 0x18u) & 0xffffu;
    for (uint32_t row = 0; row < 2u; ++row)
    {
        set16(sprite + kY, static_cast<int32_t>(y));
        for (uint32_t column = 0; column < 7u; ++column)
        {
            const uint32_t index = row * 7u + column;
            place(column);
            if (at<int8_t>(kTE + 0xecu + index) != 0 || t.editMenu.noSpecialAvail != 0)
                boxed(index + 0xfu, 0x15, sym::lit4_327784, sym::lit4_327788);
            else if (t.editMenu.specialNdx == column && t.editMenu.rowNdx == row + 2u)
            {
                pulsing();
                at<uint32_t>(sprite + kFlags) = 0;
                spriteScaled((index + 0xfu) * 4u, sprite, sym::lit4_32778c);
            }
            else
                boxed(index + 0xfu, 0x48, sym::lit4_327790, sym::lit4_327794);
        }
        y = (y + 0x12u) & 0xffffu;
    }
}

} // namespace rc

RC_DECOMP_FN_SETUP(TEditDEBUGDisplayCursor, rc::fuzz::teditPlain)
RC_DECOMP_FN_SETUP(TEditRenderTrack, rc::fuzz::teditPlain)
RC_DECOMP_FN_SETUP(TEditDisplay3DGridLines, rc::fuzz::teditPlain)
RC_DECOMP_FN_SETUP(TEditRenderEditMenu, rc::fuzz::teditMenuTime)
