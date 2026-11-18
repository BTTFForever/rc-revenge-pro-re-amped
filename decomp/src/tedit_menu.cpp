// Decompiled from TEDIT.c: the edit menu's starting layout, the resource meter and the 2D map's grid lines, the three
// with Europe variants (the PAL screen is taller: the meter 10 lines higher, the map 12 lower, the menu 10 higher).
// See tedit.h.

#include "tedit.h"
#include "tedit_fuzz.h"

RC_VERSION_VARIANT(TEditInitialiseEditMenu)
RC_VERSION_VARIANT(TEditRenderResourceMeter)
RC_VERSION_VARIANT(TEditDisplay2DGridLines)

namespace rc {

using namespace tedit;

// The edit menu's grid of modules: editModuleListG 13 onwards in order, row by row (the steep slopes offer the
// straight and the tight turns, the gentle ones all but the 2 x 2 turns, flat all seven), then the 14 specials; the
// cursor on the flat straight.
void TEditInitialiseEditMenu()
{
    sEditMenuState &e = te().editMenu;
#if RC_VERSION_EU
    e.screenYPos = 0xa;
#else
    e.screenYPos = 0x14;
#endif
    e.screenXPos = 0x177;
    e.moduleNdx = 3;
    e.slopeNdx = 2;
    e.rowNdx = 0;
    e.specialNdx = 0;
    e.noSpecialAvail = 0;
    uint32_t next = 0xd;
    for (uint32_t r = 0; r < 5u; ++r)
    {
        sEditGridSlopeInstance &g = te().editMenuGridSlopes[r];
        e.slopeRestrict[r] = 0;
        for (uint32_t c = 0; c < 7u; ++c)
        {
            bool offered;
            if (r == 0 || r == 4)
                offered = c == 0 || c == 3 || c == 6;
            else if (r == 1 || r == 3)
                offered = c != 2 && c != 4;
            else
                offered = true;
            g.avail[c] = static_cast<int8_t>(offered ? 1 : 0);
            g.pEditModuleDesc[c] = ptr32<_sEditModuleDesc>(offered ? editModuleDesc(next++) : 0u);
        }
    }
    for (uint32_t i = 0; i < 2u; ++i)
        for (uint32_t j = 0; j < 7u; ++j)
        {
            e.specialRestrict[i][j] = 0;
            e.pEditModuleSpecial[i][j] = ptr32<_sEditModuleDesc>(editModuleDesc(next++));
        }
}

// The resource meter at the top left: a black box, the meter's five pictures, and a bar from red to orange growing with
// the resources used (0x19000 fills it).
void TEditRenderResourceMeter()
{
#if RC_VERSION_EU
    GuestFrame frame(0xf0);
    constexpr int16_t kTop = 0xa, kBottom = 0x1a, kBarTop = 0xb, kBarBottom = 0x19;
#else
    GuestFrame frame(0xe0);
    constexpr int16_t kTop = 0x14, kBottom = 0x24, kBarTop = 0x15, kBarBottom = 0x23;
#endif
    const uint32_t rect = frame.local<uint8_t>(0).address, grect = rect + 0x20u, sprite = rect + 0x50u;
    const uint32_t bar = te().resourceMemoryUsed * 0x9cu / 0x19000u + 0x16u;
    at<int16_t>(rect + 0x10u) = 0x14;
    at<int16_t>(rect + 4u) = 0x14;
    at<int16_t>(rect + 0xau) = kTop;
    at<int16_t>(rect + 6u) = kTop;
    at<int16_t>(rect + 8u) = 0xb4;
    at<int16_t>(rect + 0xeu) = kBottom;
    at<uint32_t>(rect) = 1;
    at<int16_t>(rect + 0xcu) = 0xb4;
    at<int16_t>(rect + 0x12u) = kBottom;
    at<uint8_t>(rect + 0x16u) = 0;
    at<uint8_t>(rect + 0x15u) = 0;
    at<uint8_t>(rect + 0x14u) = 0;
    const ee_float otz = at<ee_float>(sym::lit4_32779c);
    guest_call(sym::RectRender, rect, at<ee_float>(sym::lit4_327798));
    at<uint8_t>(sprite + 0x12u) = 0x48;
    at<uint8_t>(sprite + 0x10u) = 0x48;
    at<uint8_t>(sprite + 0x11u) = 0x48;
    at<int16_t>(sprite + 6u) = kTop;
    at<int16_t>(sprite + 4u) = 0x14;
    at<uint16_t>(sprite + 0x14u) = 0x100;
    at<uint16_t>(sprite + 0x16u) = 0x80;
    at<uint16_t>(sprite + 0xeu) = 0x20;
    at<uint32_t>(sprite) = 1;
    at<uint16_t>(sprite + 0xcu) = 0x20;
    at<uint16_t>(sprite + 8u) = 0;
    at<uint16_t>(sprite + 0xau) = 0;
    const uint32_t sprites = te().pTrackSprites.address;
    guest_call(sym::SpriteRenderScaled, at<uint32_t>(sprites + 0x9cu), sprite, otz);
    for (const uint32_t picture : {0xa0u, 0xa0u, 0xa0u, 0xa4u})
    {
        at<uint16_t>(sprite + 4u) = static_cast<uint16_t>(at<uint16_t>(sprite + 4u) + 0x20u);
        guest_call(sym::SpriteRenderScaled, at<uint32_t>(te().pTrackSprites.address + picture), sprite, otz);
    }
    at<int16_t>(grect + 8u) = static_cast<int16_t>(bar);
    at<int16_t>(grect + 0xcu) = static_cast<int16_t>(bar);
    at<int16_t>(grect + 4u) = 0x16;
    at<int16_t>(grect + 6u) = kBarTop;
    at<int16_t>(grect + 0xeu) = kBarBottom;
    at<uint8_t>(grect + 0x14u) = 0x64;
    at<uint8_t>(grect + 0x15u) = 0x32;
    at<uint8_t>(grect + 0x18u) = 0xe6;
    at<uint8_t>(grect + 0x19u) = 0x73;
    at<uint32_t>(grect) = 0;
    at<int16_t>(grect + 0x10u) = 0x16;
    at<int16_t>(grect + 0xau) = kBarTop;
    at<int16_t>(grect + 0x12u) = kBarBottom;
    at<uint8_t>(grect + 0x1au) = 0;
    at<uint8_t>(grect + 0x16u) = 0;
    at<uint8_t>(grect + 0x22u) = 0;
    at<uint8_t>(grect + 0x1eu) = 0;
    at<uint8_t>(grect + 0x20u) = 0x64;
    at<uint8_t>(grect + 0x21u) = 0x32;
    at<uint8_t>(grect + 0x1cu) = 0xe6;
    at<uint8_t>(grect + 0x1du) = 0x73;
    at<uint8_t>(grect + 0x1bu) = 0;
    at<uint8_t>(grect + 0x1fu) = 0;
    at<uint8_t>(grect + 0x23u) = 0;
    guest_call(sym::RectRenderGouraud, grect, at<ee_float>(sym::lit4_3277a0));
}

// The 2D map's background and its 17 x 17 grid lines (20 pixels across, 10 down), each line at its own depth.
void TEditDisplay2DGridLines()
{
#if RC_VERSION_EU
    constexpr int16_t kTop = 0x2c, kBottom = 0xcc;
#else
    constexpr int16_t kTop = 0x20, kBottom = 0xc0;
#endif
    GuestFrame frame(0xa0);
    const uint32_t line = frame.local<uint8_t>(0).address, rect = line + 0x10u;
    at<int16_t>(rect + 0xeu) = kBottom;
    at<int16_t>(rect + 0x10u) = 0xa0;
    at<int16_t>(rect + 4u) = 0xa0;
    at<int16_t>(rect + 0xcu) = 0x1e0;
    at<int16_t>(rect + 8u) = 0x1e0;
    at<int16_t>(rect + 0xau) = kTop;
    at<int16_t>(rect + 6u) = kTop;
    at<int16_t>(rect + 0x12u) = kBottom;
    at<uint32_t>(rect) = 0;
    at<uint8_t>(rect + 0x16u) = 0;
    at<uint8_t>(rect + 0x15u) = 0;
    at<uint8_t>(rect + 0x14u) = 0;
    guest_call(sym::RectRender, rect, at<ee_float>(sym::lit4_327738));
    at<uint8_t>(line + 0xdu) = 0x10;
    at<uint8_t>(line + 0xeu) = 0x40;
    at<int16_t>(line + 4u) = 0xa0;
    at<int16_t>(line + 8u) = 0x1e0;
    at<uint32_t>(line) = 0;
    at<uint8_t>(line + 0xcu) = 0;
    static constexpr uint32_t kAcross[4] = {sym::lit4_32773c, sym::lit4_327740, sym::lit4_327744, sym::lit4_327748};
    static constexpr uint32_t kDown[4] = {sym::lit4_32774c, sym::lit4_327750, sym::lit4_327754, sym::lit4_327758};
    for (int32_t i = 0; i < 17; ++i)
    {
        at<int16_t>(line + 6u) = static_cast<int16_t>(kTop + i * 10);
        at<int16_t>(line + 0xau) = static_cast<int16_t>(kTop + i * 10);
        guest_call(sym::LineRender2D, line, at<ee_float>(kAcross[i & 3]));
    }
    at<int16_t>(line + 6u) = kTop;
    at<int16_t>(line + 0xau) = kBottom;
    for (int32_t i = 0; i < 17; ++i)
    {
        at<int16_t>(line + 8u) = static_cast<int16_t>(0xa0 + i * 20);
        at<int16_t>(line + 4u) = static_cast<int16_t>(0xa0 + i * 20);
        guest_call(sym::LineRender2D, line, at<ee_float>(kDown[i & 3]));
    }
}

} // namespace rc

RC_DECOMP_FN_SETUP(TEditInitialiseEditMenu, rc::fuzz::teditPlain)
RC_DECOMP_FN_SETUP(TEditRenderResourceMeter, rc::fuzz::teditPlain)
RC_DECOMP_FN_SETUP(TEditDisplay2DGridLines, rc::fuzz::teditPlain)
