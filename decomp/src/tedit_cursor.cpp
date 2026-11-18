// Decompiled from TEDIT.c: the cursor (its restrictions at the grid's edges and heights, the pieces it carries), the
// edit menu's selection and what it offers, placing a module's tiles, deleting the last module, the error check before
// placing, the 2D map's pieces, the start grid's camera paths and the editor's smaller frame functions. See tedit.h.

#include "tedit.h"
#include "tedit_fuzz.h"

RC_VERSION_VARIANT(TEditRender2DModule)

namespace rc {

using namespace tedit;

namespace {

// An edit menu cell's flags: offered (avail) and not restricted.
bool cellOpen(uint32_t row, int32_t column)
{
    return at<int8_t>(row + 0x1cu + static_cast<uint32_t>(column)) != 0 && at<int8_t>(row + 0x23u + static_cast<uint32_t>(column)) == 0;
}

// TEditAdjustCursorModule (recompiled) on the locals of a frame: x 0x0/0x10, y, direction, elevation.
uint32_t adjust(uint32_t module, uint32_t x, uint32_t y, uint32_t direction, uint32_t elevation, uint32_t flag)
{
    return guest_call<uint32_t>(sym::TEditAdjustCursorModule, module, x, y, direction, elevation, flag);
}

} // namespace

void TEditControlPlaceStartGrid(ptr32<_sTrackInput> pInput, tTime time) { guest_call(sym::TEditProcessPlaceStartGrid, pInput, time); }

void TEditControlRotateStartGrid(ptr32<_sTrackInput> pInput, tTime time) { guest_call(sym::TEditProcessRotateStartGrid, pInput, time); }

void TEditFreeTrackAINodes() { guest_call<int32_t>(sym::MemBlockFree, sym::aiNodeVarG + 4u); }

// The theme's sky, drawn from the camera.
void TEditRenderSkyDome()
{
    uint32_t sky;
    {
        GuestFrame frame(0x30);
        sky = te().pTrackSkies[te().trackDesc.themeTypeNdx].address;
        guest_call(sym::LsCameraToRdr, sym::rdrCameraG);
    }
    guest_call(sym::TRdr_VUShape, sky, sym::rdrCameraG, 0);
}

// Key 3 on the main menu returns to the grid.
void TEditControlMenu(tTime)
{
    {
        GuestFrame frame(0x20);
        if (guest_call<bool_>(sym::InputHit, 3) == 0)
            return;
        if (G(pMenuSystemG)->pActiveMenu != te().pTrackMainMenu)
            return;
        guest_call<int16_t>(sym::SfxPlay, 0x43, 0x80, 0xf, 0xf, 0x100, 0, 0);
        te().trackState.currentMode = 0;
    }
    guest_call(sym::InputResetAllHit);
}

void TEditDisplayHelpScreen(tTime)
{
    GuestFrame frame(0x30);
    const uint32_t temp = frame.local<uint8_t>(0).address;
    guest_call(sym::memset, temp, 0, 0xc);
    guest_call(sym::CellRenderInit, temp);
    guest_call(sym::TEditDisplay3DGridLines);
    guest_call(sym::BuildTrackEditorHelp, G(pMenuSystemG)->pActiveMenu);
    if (G(frontEndVarG).flagPTT == 0)
    {
        te().trackState.currentMode = 1;
        G(pMenuSystemG)->pActiveMenu = te().pTrackMainMenu;
        te().trackState.menuCurrentState = 0;
    }
    guest_call(sym::TEditRenderTrack);
}

// The start grid's cameras (the level's first path, tags of 0x10000000 and up, last first) placed along the AI route.
void TEditConstructStartGridCameraPaths()
{
    GuestFrame frame(0x40);
    const uint32_t node = frame.local<uint32_t>(0).address;
    const uint32_t level = G(pLvlG).address;
    at<uint32_t>(node) = G(aiNodeVarG).pNode.address;
    const uint32_t path = at<uint32_t>(at<uint32_t>(level + 0x20u));
    for (int32_t i = at<uint8_t>(path) - 1; i >= 0; --i)
    {
        const int32_t tag = at<int32_t>(path + 8u + static_cast<uint32_t>(i) * 4u);
        if (tag > 0x0fffffff)
        {
            const uint32_t camera = guest_call<uint32_t>(sym::GetCameraFromTag, tag, at<uint32_t>(G(pLvlG).address + 0x1cu));
            TEditCopyAINodePosToCameraNode(ptr32<_sSingleCamera>(camera), ptr32<ptr32<_sAINode>>(node));
        }
    }
}

// Puts a camera 0x400 along the route behind a node: back through prev[0] to the first node more than 0x400 away,
// then 0x400 towards it, 300 up, looking at the node; the node becomes that one.
void TEditCopyAINodePosToCameraNode(ptr32<_sSingleCamera> pCam, ptr32<ptr32<_sAINode>> ppAINode)
{
    GuestFrame frame(0x80);
    const uint32_t vec = frame.local<uint8_t>(0).address;
    const uint32_t from = ppAINode->address;
    uint32_t to = at<uint32_t>(from + 8u);
    const auto difference = [&]
    {
        at<uint16_t>(vec) = static_cast<uint16_t>(at<uint16_t>(to + 0x28u) - at<uint16_t>(from + 0x28u));
        at<uint16_t>(vec + 2u) = static_cast<uint16_t>(at<uint16_t>(to + 0x2au) - at<uint16_t>(from + 0x2au));
        at<uint16_t>(vec + 4u) = static_cast<uint16_t>(at<uint16_t>(to + 0x2cu) - at<uint16_t>(from + 0x2cu));
        return guest_call<int32_t>(sym::VecLength, vec);
    };
    while (difference() < 0x401)
        to = at<uint32_t>(to + 8u);
    const int32_t length = difference();
    const uint32_t cam = pCam.address;
    const int16_t x = static_cast<int16_t>(at<uint16_t>(from + 0x28u) + ee_div(at<int16_t>(vec) << 10, length));
    at<int16_t>(vec) = x;
    const int16_t y = static_cast<int16_t>(at<uint16_t>(from + 0x2au) + ee_div(at<int16_t>(vec + 2u) * 1024, length));
    at<int16_t>(vec + 2u) = y;
    at<int32_t>(cam + 8u) = x << 8;
    at<int16_t>(cam + 6u) = 0;
    at<int32_t>(cam + 0xcu) = (y + 300) << 8;
    const int16_t z = static_cast<int16_t>(at<uint16_t>(from + 0x2cu) + ee_div(at<int16_t>(vec + 4u) * 1024, length));
    at<int16_t>(vec + 4u) = z;
    at<int32_t>(cam + 0x10u) = z << 8;
    const int16_t heading = guest_call<int16_t>(sym::ArcTan, at<int16_t>(from + 0x2cu) - z, at<int16_t>(from + 0x28u) - x);
    ppAINode->address = to;
    at<int16_t>(cam + 4u) = heading;
}

// The cursor's restrictions from its place on the grid, turned to its direction (as if facing north).
void TEditCheckCursorDirectionConstraints()
{
    const sCursorDesc &c = te().cursor;
    const uint8_t x = static_cast<uint8_t>(c.xPos), y = static_cast<uint8_t>(c.yPos);
    const uint8_t xMax = static_cast<uint8_t>(c.xMax), yMax = static_cast<uint8_t>(c.yMax);
    switch (c.direction)
    {
    case EAST:
        TEditSetCursorDirectionConstraints(static_cast<uint8_t>(yMax - 1 - y), x);
        break;
    case NORTH:
        TEditSetCursorDirectionConstraints(x, y);
        break;
    case SOUTH:
        TEditSetCursorDirectionConstraints(static_cast<uint8_t>(xMax - 1 - x), static_cast<uint8_t>(yMax - 1 - y));
        break;
    case WEST:
        TEditSetCursorDirectionConstraints(y, static_cast<uint8_t>(xMax - 1 - x));
        break;
    default:
        break;
    }
}

// Near the top or the bottom of the editor's heights (elevation / 256 above 22 or below -14), turns and slope changes
// that would leave them are restricted.
void TEditCheckCursorMinMaxHeightConstraints()
{
    GuestFrame frame(0x40);
    const int32_t level = te().cursor.elevation >> 8;
    const int32_t slope = te().cursor.currentSlope;
    if (static_cast<uint16_t>(level + 0xe) < 0x25u)
        return;
    if (level >= 0)
    {
        const int32_t room = 0x1c - level;
        TEditRestrictLeftTurn(1, 1);
        TEditRestrictRightTurn(4, 1);
        if (room < 2)
            TEditRestrictSlopeChange(3);
        else if (room == 2)
        {
            TEditRestrictSlopeChange(1);
            if (slope == SLOPE_FLAT)
            {
                TEditRestrictLeftTurn(3, 2);
                TEditRestrictRightTurn(6, 2);
            }
            else
                TEditRestrictSlopeChange(2);
        }
        else if (room == 3)
        {
            TEditRestrictSlopeChange(1);
            if (slope == SLOPE_INCLINE_2_1)
            {
                TEditRestrictLeftTurn(3, 2);
                TEditRestrictRightTurn(6, 2);
            }
            else if (slope == SLOPE_INCLINE_3_1)
                TEditRestrictSlopeChange(2);
        }
        else if (room == 4)
            TEditRestrictSlopeChange(1);
        return;
    }
    const int32_t room = level + 0x14;
    TEditRestrictLeftTurn(1, 0x10);
    TEditRestrictRightTurn(4, 0x10);
    if (room < 2)
        TEditRestrictSlopeChange(0x18);
    else if (room == 2)
    {
        TEditRestrictSlopeChange(0x10);
        if (slope == SLOPE_FLAT)
        {
            TEditRestrictLeftTurn(3, 8);
            TEditRestrictRightTurn(6, 8);
        }
        else
            TEditRestrictSlopeChange(8);
    }
    else if (room == 3)
    {
        TEditRestrictSlopeChange(0x10);
        if (slope == SLOPE_DECLINE_2_1)
        {
            TEditRestrictLeftTurn(3, 8);
            TEditRestrictRightTurn(6, 8);
        }
        else if (slope == SLOPE_DECLINE_3_1)
            TEditRestrictSlopeChange(8);
    }
    else if (room == 4)
        TEditRestrictSlopeChange(0x10);
}

// The restrictions for a cursor at (x, y) facing north: no turns or specials into the sides, nothing that runs off the
// far edge, no special pieces on a slope.
void TEditSetCursorDirectionConstraints(uint8 xPos, uint8 yPos)
{
    GuestFrame frame(0x40);
    const uint32_t x = xPos, y = yPos;
    if (te().cursor.currentSlope != SLOPE_FLAT)
        TEditRestrictSpecial(1);
    if (x == 0 || x == 1)
    {
        TEditRestrictLeftTurn(x == 0 ? 7 : 3, 0x1f);
        TEditRestrictSpecial(2);
    }
    else if (x == 2)
    {
        TEditRestrictLeftTurn(1, 0x1f);
        TEditRestrictSpecial(2);
    }
    const int32_t xMax = te().cursor.xMax;
    if (x == static_cast<uint32_t>(xMax - 1) || x == static_cast<uint32_t>(xMax - 2))
    {
        TEditRestrictRightTurn(x == static_cast<uint32_t>(xMax - 1) ? 7 : 6, 0x1f);
        TEditRestrictSpecial(4);
    }
    else if (x == static_cast<uint32_t>(xMax - 3))
    {
        TEditRestrictRightTurn(4, 0x1f);
        TEditRestrictSpecial(4);
    }
    const int32_t yMax = te().cursor.yMax;
    if (y == static_cast<uint32_t>(yMax - 1))
    {
        TEditRestrictSpecial(1);
        TEditRestrictStraight(0x1f);
        TEditRestrictSlopeChange(0x1b);
        TEditRestrictLeftTurn(3, 0x1f);
        TEditRestrictRightTurn(6, 0x1f);
    }
    else if (y == static_cast<uint32_t>(yMax - 2))
    {
        TEditRestrictSlopeChange(0x1b);
        TEditRestrictLeftTurn(1, 0x1f);
        TEditRestrictRightTurn(4, 0x1f);
        TEditRestrictSpecial(6);
    }
    else if (y == static_cast<uint32_t>(yMax - 3))
        TEditRestrictSlopeToSlopeChange(te().cursor.currentSlope);
}

// The cursor's pieces drawn at its place.
void TEditRenderCursor()
{
    GuestFrame frame(0x60);
    const uint32_t sp = frame.local<uint8_t>(0).address; // x 0x0, y 0x2, elevation 0x4, direction 0x8
    sTEditClass &t = te();
    G(tileVarG).pTrackTiles = t.pTrackTiles[t.trackDesc.themeTypeNdx];
    at<int32_t>(sp + 8u) = t.cursor.direction;
    at<int16_t>(sp + 4u) = t.cursor.elevation;
    at<int16_t>(sp) = t.cursor.xPos;
    at<int16_t>(sp + 2u) = t.cursor.yPos;
    for (uint32_t module = t.cursor.pCursorModuleList.address; module != 0; module = at<uint32_t>(module))
        adjust(module, sp, sp + 2u, sp + 8u, sp + 4u, 1);
}

// The cursor's pieces for the edit menu's choice: the module (or the transition pieces to its slope), or a special.
void TEditSetNewCursorModule()
{
    sTEditClass &t = te();
    {
        GuestFrame frame(0x20);
        ClearCursorModuleList();
        if (t.editMenu.rowNdx < 2)
        {
            const eSlope desired = TEditGetEditModuleInputSlope(t.editMenu.slopeNdx, t.editMenu.moduleNdx);
            if (t.cursor.currentSlope != desired)
            {
                guest_call(sym::TEditBuildTransitionCursorModules, t.cursor.currentSlope, desired);
                return;
            }
            AddEditModuleToCursorModuleList(ptr32<_sEditModuleDesc>(at<uint32_t>(kGridSlopes + t.editMenu.slopeNdx * 0x2cu + t.editMenu.moduleNdx * 4u)));
            return;
        }
    }
    const int32_t special = (static_cast<int32_t>(t.editMenu.rowNdx) - 2) * 0x1c + t.editMenu.specialNdx * 4;
    AddEditModuleToCursorModuleList(ptr32<_sEditModuleDesc>(at<uint32_t>(kTE + 0xb4u + static_cast<uint32_t>(special))));
}

// The 2D map: the track's modules from its start.
void TEditDisplay2DGridMap()
{
    GuestFrame frame(0x90);
    const uint32_t sp = frame.local<uint8_t>(0).address; // cursorModule 0x0, x 0x10, y 0x12, elevation 0x14, direction 0x18
    const sTEditClass &t = te();
    uint32_t node = t.trackDesc.pTrackModuleList.address;
    at<int32_t>(sp + 0x18u) = t.trackDesc.startOrientation;
    at<int16_t>(sp + 0x14u) = t.trackDesc.startElevation;
    at<int16_t>(sp + 0x10u) = t.trackDesc.startXPos;
    at<int16_t>(sp + 0x12u) = t.trackDesc.startYPos;
    for (; node != 0; node = at<uint32_t>(node))
    {
        at<uint32_t>(sp + 4u) = editModuleDesc(at<uint32_t>(node + 4u));
        adjust(sp, sp + 0x10u, sp + 0x12u, sp + 0x18u, sp + 0x14u, 4);
    }
}

// A unit's tile on the hud map (4 pixels a unit) from the tile pictures (5 a row, 5 pixels each).
void TEditRender2DHudMapModule(int16 xPos, int16 yPos, int8 tileId, int16, uint8 tileOrientation)
{
    GuestFrame frame(0x20);
    const uint32_t rots = frame.local<uint8_t>(0).address;
    at<uint32_t>(rots) = at<uint32_t>(kRots);
    uint32_t flags = at<uint8_t>(rots + (tileOrientation & 0x7fu));
    if ((tileOrientation & 0x80u) != 0)
        flags |= 0x20u;
    const uint32_t tile = tileId < 0x19 ? static_cast<uint16_t>(tileId) : static_cast<uint16_t>(tileId - 0x19);
    guest_call(sym::TEditRenderTileToHudMap, static_cast<int16_t>(xPos << 2), static_cast<int16_t>((15 - yPos) << 2),
               static_cast<int32_t>((tile % 5u) * 5u + 1u), static_cast<int16_t>((tile / 5u) * 5u + 1u), flags);
}

// A unit's tile on the 2D map (20 x 10 pixels a unit) from the tile sheets (25 tiles each, 5 a row, 11 pixels apart).
void TEditRender2DModule(int16 xPos, int16 yPos, int8 tileId, int16, uint8 tileOrientation)
{
    GuestFrame frame(0x40);
    const uint32_t sp = frame.local<uint8_t>(0).address; // the sprite's parameters at 0x0, the flag table at 0x20
    at<uint32_t>(sp + 0x20u) = at<uint32_t>(kRots);
    if (te().display2DGridMap == 0)
        return;
    uint32_t flags = at<uint8_t>(sp + 0x20u + (tileOrientation & 0x7fu));
    if ((tileOrientation & 0x80u) != 0)
        flags |= 0x20u;
#if RC_VERSION_EU
    constexpr int32_t kTop = 0x2d; // PAL: the map 12 lines lower
#else
    constexpr int32_t kTop = 0x21;
#endif
    at<uint32_t>(sp) = flags;
    at<int16_t>(sp + 4u) = static_cast<int16_t>(xPos * 20 + 0xa1);
    at<int16_t>(sp + 6u) = static_cast<int16_t>((15 - yPos) * 10 + kTop);
    uint32_t tile, sheet;
    if (tileId < 0x19)
    {
        tile = static_cast<uint16_t>(tileId);
        sheet = 1;
    }
    else
    {
        tile = static_cast<uint16_t>(tileId - 0x19);
        sheet = 2;
    }
    at<int16_t>(sp + 8u) = static_cast<int16_t>((tile % 5u) * 11u + 1u);
    at<uint8_t>(sp + 0x10u) = 0x80;
    at<uint8_t>(sp + 0x12u) = 0x80;
    at<uint8_t>(sp + 0x11u) = 0x80;
    at<int16_t>(sp + 0xau) = static_cast<int16_t>((tile / 5u) * 11u + 1u);
    at<int16_t>(sp + 0xcu) = 9;
    at<int16_t>(sp + 0xeu) = 9;
    guest_call(sym::SpriteRenderNew, at<uint32_t>(te().pTrackSprites.address + sheet * 4u), sp, 0);
}

// The edit menu's module row: left (1) and right (2) move to the next open module; 8 goes down a row.
bool_ TEditProcessEditMenuModuleSelection(uint32 input)
{
    sEditMenuState &e = te().editMenu;
    const uint32_t row = kGridSlopes + e.slopeNdx * 0x2cu;
    bool_ changed = 0;
    if ((input & 1u) != 0)
    {
        if (e.moduleNdx != 0)
        {
            int32_t i = static_cast<int8_t>(e.moduleNdx - 1);
            while (i >= 0 && !cellOpen(row, i))
                i = static_cast<int8_t>(i - 1);
            if (i >= 0)
            {
                e.moduleNdx = static_cast<uint8_t>(i);
                changed = 1;
            }
        }
    }
    else if ((input & 2u) != 0 && e.moduleNdx < 6)
    {
        int32_t i = static_cast<int8_t>(e.moduleNdx + 1);
        while (i < 7 && !cellOpen(row, i))
            i = static_cast<int8_t>(i + 1);
        if (i < 7)
        {
            e.moduleNdx = static_cast<uint8_t>(i);
            changed = 1;
        }
    }
    if ((input & 8u) != 0)
    {
        e.rowNdx = static_cast<uint8_t>(e.rowNdx + 1);
        changed = 1;
    }
    return changed;
}

// The special rows: left and right move to the next unrestricted special; 4 goes up a row; 8 goes down from the first
// special row to the second if one of its specials is open (the nearest at or left of the column, else right of it).
bool_ TEditProcessEditMenuSpecialSelection(uint32 input)
{
    sEditMenuState &e = te().editMenu;
    const auto restricted = [&](int32_t i)
    { return at<int8_t>(kTE + 0xecu + static_cast<uint32_t>((static_cast<int32_t>(e.rowNdx) - 2) * 7 + i)) != 0; };
    bool_ changed = 0;
    if ((input & 1u) != 0)
    {
        if (e.specialNdx != 0)
        {
            int32_t i = static_cast<int8_t>(e.specialNdx - 1);
            while (i >= 0 && restricted(i))
                i = static_cast<int8_t>(i - 1);
            if (i >= 0)
            {
                e.specialNdx = static_cast<uint8_t>(i);
                changed = 1;
            }
        }
    }
    else if ((input & 2u) != 0 && e.specialNdx < 6)
    {
        int32_t i = static_cast<int8_t>(e.specialNdx + 1);
        while (i < 7 && restricted(i))
            i = static_cast<int8_t>(i + 1);
        if (i < 7)
        {
            e.specialNdx = static_cast<uint8_t>(i);
            changed = 1;
        }
    }
    if ((input & 4u) != 0)
    {
        e.rowNdx = static_cast<uint8_t>(e.rowNdx - 1);
        return 1;
    }
    if ((input & 8u) == 0 || e.rowNdx >= 3)
        return changed;
    const uint32_t second = kTE + 0xf3u; // specialRestrict[1]
    if (at<int8_t>(second + e.specialNdx) == 0)
    {
        e.rowNdx = static_cast<uint8_t>(e.rowNdx + 1);
        return 1;
    }
    int32_t i = static_cast<int8_t>(e.specialNdx);
    while (i >= 0 && at<int8_t>(second + static_cast<uint32_t>(i)) != 0)
        i = static_cast<int8_t>(i - 1);
    if (i < 0)
    {
        i = static_cast<int8_t>(e.specialNdx);
        while (i < 7 && at<int8_t>(second + static_cast<uint32_t>(i)) != 0)
            i = static_cast<int8_t>(i + 1);
        if (i >= 7)
            return changed;
    }
    e.specialNdx = static_cast<uint8_t>(i);
    e.rowNdx = static_cast<uint8_t>(e.rowNdx + 1);
    return 1;
}

// The slope row: left and right move to the next unrestricted slope; 8 goes down to the specials (if any), 4 up.
bool_ TEditProcessEditMenuSlopeSelection(uint32 input)
{
    sEditMenuState &e = te().editMenu;
    bool_ changed = 0;
    if ((input & 1u) != 0)
    {
        if (e.slopeNdx != 0)
        {
            int32_t i = static_cast<int8_t>(e.slopeNdx - 1);
            while (i >= 0 && e.slopeRestrict[i] != 0)
                i = static_cast<int8_t>(i - 1);
            if (i >= 0)
            {
                e.slopeNdx = static_cast<uint8_t>(i);
                changed = 1;
            }
        }
    }
    else if ((input & 2u) != 0 && e.slopeNdx < 4)
    {
        int32_t i = static_cast<int8_t>(e.slopeNdx + 1);
        while (i < 5 && e.slopeRestrict[i] != 0)
            i = static_cast<int8_t>(i + 1);
        if (i < 5)
        {
            e.slopeNdx = static_cast<uint8_t>(i);
            changed = 1;
        }
    }
    if ((input & 8u) != 0 && e.noSpecialAvail == 0)
    {
        e.rowNdx = static_cast<uint8_t>(e.rowNdx + 1);
        return 1;
    }
    if ((input & 4u) == 0)
        return changed;
    e.rowNdx = static_cast<uint8_t>(e.rowNdx - 1);
    return 1;
}

// After the restrictions change: a special row with nothing left moves to the modules, a restricted special to an open
// one; a restricted slope or module moves to the nearest open module (straight, then left, then right) on the nearest
// open slope (flat, then down the inclines' side, then the declines'). With nothing open anywhere the slope becomes 5
// and the module the caller's $a1 (flat is never restricted in the game).
void tedit::availSlopeAndModule(uint32_t a1)
{
    sEditMenuState &e = te().editMenu;
    if (e.rowNdx >= 2)
    {
        if (e.noSpecialAvail != 0)
            e.rowNdx = 0;
        else if (e.rowNdx == 3)
        {
            if (e.specialNdx == 5 && e.specialRestrict[1][5] != 0)
                e.specialNdx = 4;
            else if (e.specialNdx == 6 && e.specialRestrict[1][6] != 0)
                e.specialNdx = e.specialRestrict[1][5] != 0 ? 4 : 5;
        }
    }
    const uint32_t current = kGridSlopes + e.slopeNdx * 0x2cu;
    if (e.slopeRestrict[e.slopeNdx] == 0 && cellOpen(current, e.moduleNdx))
        return;
    int32_t module = static_cast<int32_t>(a1);
    // One slope: straight on, else the nearest open turn to the left, else to the right.
    const auto search = [&](int32_t slope)
    {
        const uint32_t row = kGridSlopes + static_cast<uint32_t>(slope) * 0x2cu;
        module = 3;
        if (cellOpen(row, 3))
            return true;
        for (module = 2; module >= 0; --module)
            if (cellOpen(row, module))
                return true;
        module = 4;
        if (cellOpen(row, 4))
            return true;
        for (module = 5; module < 7; ++module)
            if (cellOpen(row, module))
                return true;
        return false;
    };
    int32_t slope;
    for (slope = 2; slope >= 0; --slope)
        if (e.slopeRestrict[slope] == 0 && search(slope))
            break;
    if (slope < 0)
        for (slope = 3; slope < 5; ++slope)
            if (e.slopeRestrict[slope] == 0 && search(slope))
                break;
    e.moduleNdx = static_cast<uint8_t>(module);
    e.slopeNdx = static_cast<uint8_t>(slope);
}

void TEditGetEditMenuAvailSlopeAndModule() { availSlopeAndModule(getRegU32(&g_runtime->eeScheduler().runningContext(), 5)); }

// A restricted module on the module row moves to an open one on the same slope: the left ones search left first, the
// right ones right first (7 or -1 if none).
void TEditGetEditMenuAvailModuleForSlope()
{
    sEditMenuState &e = te().editMenu;
    if (e.rowNdx >= 2)
        return;
    const uint32_t row = kGridSlopes + e.slopeNdx * 0x2cu;
    const uint32_t m = e.moduleNdx;
    if (cellOpen(row, static_cast<int32_t>(m)))
        return;
    int32_t i;
    if (m < 4)
    {
        for (i = static_cast<int8_t>(m); i >= 0; i = static_cast<int8_t>(i - 1))
            if (cellOpen(row, i))
                break;
        if (i < 0)
            for (i = static_cast<int8_t>(m + 1); i < 7; i = static_cast<int8_t>(i + 1))
                if (cellOpen(row, i))
                    break;
    }
    else
    {
        for (i = static_cast<int8_t>(m + 1); i < 7; i = static_cast<int8_t>(i + 1))
            if (cellOpen(row, i))
                break;
        if (i >= 7)
            for (i = static_cast<int8_t>(m); i >= 0; i = static_cast<int8_t>(i - 1))
                if (cellOpen(row, i))
                    break;
    }
    e.moduleNdx = static_cast<uint8_t>(i);
}

// One unit of a module: placed on the grid's next free layer, or, while checking for errors, only checked: no free
// layer (error 2), a tile within 0x600 of its height (1), the resources used up (4).
void TEditPlaceModule(int16 xPos, int16 yPos, int16 tileId, int16 elevation, eDirection tileOrientation)
{
    GuestFrame frame(0x90);
    const uint32_t unit = frame.local<uint8_t>(0).address;
    const ptr32<int16> heights = frame.local<int16>(0x10);
    const ptr32<uint8> layers = frame.local<uint8>(0x20);
    at<int16_t>(unit + 2u) = elevation;
    at<int32_t>(unit + 4u) = tileOrientation;
    at<int16_t>(unit) = tileId;
    const int16_t yStart = guest_call<int16_t>(sym::TileGetYStart, tileId);
    const int16_t height = static_cast<int16_t>(at<uint16_t>(unit + 2u) + yStart);
    TEditGetTrackUnitInstanceNum(xPos, yPos, layers, heights);
    sTEditClass &t = te();
    if (t.placementErrorChecking == 1)
    {
        if (*layers >= 3u)
            t.errorFlags |= 2u;
        else
            for (int32_t i = static_cast<int8_t>(*layers - 1); i >= 0; i = static_cast<int8_t>(i - 1))
            {
                const int16_t h = heights[i];
                const int16_t gap = static_cast<int16_t>(h - height > 0 ? h - height : height - h);
                if (gap < 0x600)
                {
                    t.errorFlags |= 1u;
                    break;
                }
            }
        TEditUpdateResourceMeter(ptr32<uint32>(kTE + 0x258u), at<int16_t>(unit));
        if (t.resourceErrorCheckMemoryUsed > 0x19000u)
            t.errorFlags |= 4u;
        return;
    }
    TEditSetTrackUnit(static_cast<uint16_t>(xPos), static_cast<uint16_t>(yPos), ptr32<_sTrackUnit>(unit), *layers);
    TEditUpdateResourceMeter(ptr32<uint32>(kTE + 0x254u), at<int16_t>(unit));
}

// Removes the track's last module (the list has at least one) and moves the cursor back to its start; returns its id.
uint32 DeleteTrackModuleFromTrackModuleList()
{
    GuestFrame frame(0xc0);
    const uint32_t sp = frame.local<uint8_t>(0).address; // cursorModule 0x0, x 0x10, y 0x12, elevation 0x14, direction 0x18
    const uint32_t handle = sp + 0x1cu;
    sTEditClass &t = te();
    uint32_t slope = SLOPE_FLAT, previous = 0;
    uint32_t last = t.trackDesc.pTrackModuleList.address;
    at<int32_t>(sp + 0x18u) = t.trackDesc.startOrientation;
    at<int16_t>(sp + 0x14u) = t.trackDesc.startElevation;
    at<int16_t>(sp + 0x10u) = t.trackDesc.startXPos;
    at<int16_t>(sp + 0x12u) = t.trackDesc.startYPos;
    at<uint32_t>(handle) = last;
    while (at<uint32_t>(last) != 0)
    {
        at<uint32_t>(sp + 4u) = editModuleDesc(at<uint32_t>(last + 4u));
        slope = adjust(sp, sp + 0x10u, sp + 0x12u, sp + 0x18u, sp + 0x14u, 0);
        previous = at<uint32_t>(handle);
        last = at<uint32_t>(previous);
        at<uint32_t>(handle) = last;
    }
    t.cursor.direction = static_cast<eDirection>(at<int32_t>(sp + 0x18u));
    t.cursor.elevation = at<int16_t>(sp + 0x14u);
    t.cursor.currentSlope = static_cast<eSlope>(slope);
    t.cursor.xPos = at<int16_t>(sp + 0x10u);
    t.cursor.yPos = at<int16_t>(sp + 0x12u);
    const uint32_t id = at<uint32_t>(last + 4u);
    if (last == t.trackDesc.pTrackModuleList.address)
    {
        guest_call<int32_t>(sym::MemBlockFree, handle);
        t.trackDesc.pTrackModuleList = nullptr;
    }
    else
    {
        guest_call<int32_t>(sym::MemBlockFree, handle);
        at<uint32_t>(previous) = 0;
    }
    t.trackDesc.bestLapTime = kUnsavedLapTime;
    t.loadSaveFlags |= 1u;
    return id;
}

// Places the cursor's pieces in checking mode; on an error shows its warning (resources, then layers, then height)
// and returns true.
bool_ TEditCheckForEditWarnings()
{
    GuestFrame frame(0x70);
    const uint32_t sp = frame.local<uint8_t>(0).address; // x 0x0, y 0x2, elevation 0x4, direction 0x8
    sTEditClass &t = te();
    at<int32_t>(sp + 8u) = t.cursor.direction;
    at<int16_t>(sp + 4u) = t.cursor.elevation;
    at<int16_t>(sp) = t.cursor.xPos;
    at<int16_t>(sp + 2u) = t.cursor.yPos;
    t.placementErrorChecking = 1;
    t.resourceErrorCheckMemoryUsed = t.resourceMemoryUsed;
    t.errorFlags = 0;
    for (uint32_t module = t.cursor.pCursorModuleList.address; module != 0; module = at<uint32_t>(module))
        adjust(module, sp, sp + 2u, sp + 8u, sp + 4u, 2);
    t.placementErrorChecking = 0;
    const uint32_t errors = t.errorFlags;
    if (errors == 0)
        return 0;
    int32_t warning = 2;
    uint32_t bit = 4;
    while ((errors & bit) == 0)
    {
        warning = static_cast<int8_t>(warning - 1);
        bit = static_cast<uint32_t>(static_cast<int32_t>(bit) >> 1);
        if (warning < 0)
            break;
    }
    const uint32_t menu = at<uint32_t>(kTE + 0x1fcu + static_cast<uint32_t>(warning * 4));
    G(pMenuSystemG)->pActiveMenu = ptr32<sAMenuHeader>(menu);
    guest_call(sym::AMenuLinkItems, menu, 0, 0, 0, 0x7fff0001);
    t.trackState.editState = 3;
    return 1;
}

} // namespace rc

RC_DECOMP_FN_SETUP(TEditControlPlaceStartGrid, rc::fuzz::teditStandIns)
RC_DECOMP_FN_SETUP(TEditControlRotateStartGrid, rc::fuzz::teditStandIns)
RC_DECOMP_FN_SETUP_HEAP(TEditFreeTrackAINodes, rc::fuzz::teditHeap)
RC_DECOMP_FN_SETUP_HEAP(TEditRenderSkyDome, rc::fuzz::teditDisplay)
RC_DECOMP_FN_SETUP(TEditControlMenu, rc::fuzz::teditStandIns)
RC_DECOMP_FN_SETUP(TEditDisplayHelpScreen, rc::fuzz::teditStandIns)
RC_DECOMP_FN_SETUP(TEditConstructStartGridCameraPaths, rc::fuzz::teditCameraPaths)
RC_DECOMP_FN_SETUP(TEditCopyAINodePosToCameraNode, rc::fuzz::teditCameraPaths)
RC_DECOMP_FN_SETUP(TEditCheckCursorDirectionConstraints, rc::fuzz::teditPlain)
RC_DECOMP_FN_SETUP(TEditCheckCursorMinMaxHeightConstraints, rc::fuzz::teditPlain)
RC_DECOMP_FN_SETUP(TEditSetCursorDirectionConstraints, rc::fuzz::teditGridBlocked)
RC_DECOMP_FN_SETUP(TEditRenderCursor, rc::fuzz::teditWalk)
RC_DECOMP_FN_SETUP(TEditSetNewCursorModule, rc::fuzz::teditPlain)
RC_DECOMP_FN_SETUP(TEditDisplay2DGridMap, rc::fuzz::teditWalk)
RC_DECOMP_FN_SETUP(TEditRender2DHudMapModule, rc::fuzz::teditMapUnit)
RC_DECOMP_FN_SETUP(TEditRender2DModule, rc::fuzz::teditMapUnit)
RC_DECOMP_FN_SETUP(TEditProcessEditMenuModuleSelection, rc::fuzz::teditMenuInput)
RC_DECOMP_FN_SETUP(TEditProcessEditMenuSpecialSelection, rc::fuzz::teditMenuInput)
RC_DECOMP_FN_SETUP(TEditProcessEditMenuSlopeSelection, rc::fuzz::teditMenuInput)
RC_DECOMP_FN_SETUP(TEditGetEditMenuAvailSlopeAndModule, rc::fuzz::teditPlain)
RC_DECOMP_FN_SETUP(TEditGetEditMenuAvailModuleForSlope, rc::fuzz::teditPlain)
RC_DECOMP_FN_SETUP(TEditPlaceModule, rc::fuzz::teditPlace)
RC_DECOMP_FN_SETUP(DeleteTrackModuleFromTrackModuleList, rc::fuzz::teditDelete)
RC_DECOMP_FN_SETUP(TEditCheckForEditWarnings, rc::fuzz::teditWalk)
