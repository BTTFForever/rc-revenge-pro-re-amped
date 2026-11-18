// Decompiled from TEDIT.c: the grid editor's keys. Moving and turning the start grid before it is placed, then the
// edit menu: choosing a piece, adding the cursor's pieces to the track, deleting the last one. See tedit.h.

#include "tedit.h"
#include "tedit_fuzz.h"

namespace rc {

using namespace tedit;

namespace {

void sfx(uint32_t sound, uint32_t pitch) { guest_call<int16_t>(sym::SfxPlay, sound, 0x80, 0xf, 0xf, pitch, 0, 0); }

R5900Context &context() { return g_runtime->eeScheduler().runningContext(); }

// The start grid one unit along an axis, unless it is at the grid's edge already or the step would put it in a
// corner (on an edge with the other coordinate 0 or 15). True if it moved.
bool stepStartGrid(int16 &coordinate, int32_t delta, int16_t other)
{
    const int32_t from = coordinate;
    if (delta > 0)
    {
        if (!(from < 0xf))
            return false;
        if (from == 0xe && (other == 0 || other == 0xf))
            return false;
    }
    else
    {
        if (from == 0)
            return false;
        if (from == 1 && (other == 0 || other == 0xf))
            return false;
    }
    coordinate = static_cast<int16_t>(static_cast<uint16_t>(coordinate) + delta);
    sfx(0x44, 0xf6);
    return true;
}

// The cursor moved one unit on from the start grid just placed.
void stepOffStartGrid()
{
    sCursorDesc &c = te().cursor;
    switch (static_cast<uint32_t>(c.direction))
    {
    case 0:
        c.yPos = static_cast<int16_t>(c.yPos + 1);
        break;
    case 1:
        c.xPos = static_cast<int16_t>(c.xPos + 1);
        break;
    case 2:
        c.yPos = static_cast<int16_t>(c.yPos - 1);
        break;
    case 3:
        c.xPos = static_cast<int16_t>(c.xPos - 1);
        break;
    default:
        break;
    }
}

} // namespace

// The edit menu: the arrows change the selection (by the row the cursor is on), key 4 adds the cursor's pieces to the
// track (unless a warning comes up; a track that closes is complete), key 5 deletes the last module and puts it
// back on the cursor with the menu on its cell. With nothing on the cursor the slope stored is the caller's $s5.
void TEditProcessEditMenuKeys(ptr32<_sTrackInput> pInput, tTime time) { processEditMenuKeys(pInput, time, getRegU32(&context(), 21)); }

void tedit::processEditMenuKeys(ptr32<_sTrackInput> pInput, tTime, uint32_t slope)
{
    GuestFrame frame(0x90);
    const uint32_t sp = frame.local<uint8_t>(0).address; // x 0x0, y 0x2, elevation 0x4, direction 0x8, outputSlope 0xc
    sTEditClass &t = te();
    const uint32_t input = pInput.address;
    const uint32_t row = t.editMenu.rowNdx;
    bool updated = false;
    if (row == 1)
        updated = TEditProcessEditMenuSlopeSelection(at<uint32_t>(input)) != 0;
    else if (row == 0)
        updated = TEditProcessEditMenuModuleSelection(at<uint32_t>(input)) != 0;
    else if (row < 4)
        updated = TEditProcessEditMenuSpecialSelection(at<uint32_t>(input)) != 0;
    if (updated)
    {
        sfx(0x44, 0xf6);
        TEditGetEditMenuAvailModuleForSlope();
        if (t.trackIsComplete == 0)
            TEditSetNewCursorModule();
    }
    const uint32_t response = at<uint32_t>(input);
    if ((response & 0x10u) != 0 && t.trackIsComplete == 0)
    {
        sfx(0x45, 0x100);
        if (TEditCheckForEditWarnings() == 1)
            return;
        at<int32_t>(sp + 8u) = t.cursor.direction;
        at<int16_t>(sp + 4u) = t.cursor.elevation;
        at<int16_t>(sp) = t.cursor.xPos;
        at<int16_t>(sp + 2u) = t.cursor.yPos;
        for (uint32_t module = t.cursor.pCursorModuleList.address; module != 0; module = at<uint32_t>(module))
        {
            slope = static_cast<uint32_t>(TEditAdjustCursorModule(ptr32<_sCursorModule>(module), ptr32<int16>(sp), ptr32<int16>(sp + 2u),
                                                                  ptr32<eDirection>(sp + 8u), ptr32<int16>(sp + 4u), 2));
            AddTrackModuleToTrackModuleList(static_cast<int32_t>(at<uint32_t>(module + 4u) - kEditModuleList) >> 2, 1);
        }
        t.cursor.currentSlope = static_cast<eSlope>(slope);
        t.cursor.direction = static_cast<eDirection>(at<int32_t>(sp + 8u));
        t.cursor.elevation = at<int16_t>(sp + 4u);
        t.cursor.xPos = at<int16_t>(sp);
        t.cursor.yPos = at<int16_t>(sp + 2u);
        TEditClearAllRestricts();
        TEditCheckCursorDirectionConstraints();
        TEditCheckCursorMinMaxHeightConstraints();
        availSlopeAndModule(getRegU32(&context(), 5));
        TEditSetNewCursorModule();
        if (TEditCheckForCompleteTrack() != 1)
            return;
        t.trackIsComplete = 1;
        ClearCursorModuleList();
        const uint32_t menu = t.pTrackComplete.address;
        at<uint32_t>(G(pMenuSystemG).address) = menu;
        guest_call(sym::AMenuLinkItems, menu, 0, 0, 0, 0x7fff0001);
        t.trackState.editState = 4;
        return;
    }
    if ((response & 0x20u) == 0)
        return;
    sfx(0x43, 0x100);
    t.trackIsComplete = 0;
    const uint32_t id = DeleteTrackModuleFromTrackModuleList() & 0xffu;
    TEditClearTrack();
    TEditResetResourceMeter();
    if (id == 0)
    {
        // The start grid itself: back to turning it.
        t.trackState.currentMode = 0;
        t.trackState.editState = 1;
        ClearCursorModuleList();
        AddEditModuleToCursorModuleList(ptr32<_sEditModuleDesc>(kEditModuleList));
        return;
    }
    generateTrackFromList(0xffffffffu); // $a3 as the grid's clearing left it
    bool special = false;
    uint32_t menuSlope = 0;
    if (id - 1u < 0xcu)
    {
        t.editMenu.moduleNdx = 3;
        menuSlope = static_cast<uint32_t>(TEditGetEditModuleOutputSlope(id));
        at<uint32_t>(sp + 0xcu) = menuSlope;
    }
    else if (TEditCheckForFlatInOutSlopedCorners(static_cast<uint8_t>(id), ptr32<eSlope>(sp + 0xcu)) == 1)
    {
        t.editMenu.moduleNdx = GetEditMenuModuleIndex(static_cast<eSlope>(at<int32_t>(sp + 0xcu)), id);
        menuSlope = at<uint32_t>(sp + 0xcu);
    }
    else if (id < 0x24u)
    {
        t.editMenu.moduleNdx = GetEditMenuModuleIndex(t.cursor.currentSlope, id);
        menuSlope = static_cast<uint32_t>(t.cursor.currentSlope);
    }
    else
    {
        special = true;
        t.editMenu.rowNdx = static_cast<uint8_t>(id < 0x2bu ? 2 : 3);
        t.editMenu.specialNdx = static_cast<uint8_t>(id < 0x2bu ? id - 0x24u : id - 0x2bu);
        t.editMenu.slopeNdx = 2;
    }
    if (!special)
    {
        t.editMenu.slopeNdx = TEditGetSlopeNdxFromTrueSlope(static_cast<eSlope>(menuSlope));
        t.editMenu.rowNdx = 0;
    }
    TEditClearAllRestricts();
    TEditCheckCursorDirectionConstraints();
    TEditCheckCursorMinMaxHeightConstraints();
    availSlopeAndModule(getRegU32(&context(), 5));
    ClearCursorModuleList();
    AddEditModuleToCursorModuleList(ptr32<_sEditModuleDesc>(editModuleDesc(id)));
}

// The start grid before it is placed: the arrows move it over the grid as the view shows them (never into a corner),
// key 4 goes on to turning it, on an edge first turned to run along it.
void TEditProcessPlaceStartGrid(ptr32<_sTrackInput> pInput, tTime)
{
    GuestFrame frame(0x40);
    sTEditClass &t = te();
    sCursorDesc &c = t.cursor;
    const uint32_t input = pInput.address;
    const uint32_t view = static_cast<uint32_t>(t.placementViewDir);
    if (view <= 3u)
    {
        // Bits 0 and 1 move across the view, bits 2 and 3 into and out of it: the axis and the sign by the view.
        const bool acrossIsX = view == 0u || view == 2u;
        const int32_t bit0 = view == 0u || view == 3u ? -1 : 1;
        const int32_t bit2 = view == 0u || view == 1u ? 1 : -1;
        int16 &acrossCoordinate = acrossIsX ? c.xPos : c.yPos;
        int16 &intoCoordinate = acrossIsX ? c.yPos : c.xPos;
        uint32_t response = at<uint32_t>(input);
        if ((response & 1u) != 0)
            stepStartGrid(acrossCoordinate, bit0, intoCoordinate);
        else if ((response & 2u) != 0)
            stepStartGrid(acrossCoordinate, -bit0, intoCoordinate);
        response = at<uint32_t>(input);
        if ((response & 4u) != 0)
            stepStartGrid(intoCoordinate, bit2, acrossCoordinate);
        else if ((response & 8u) != 0)
            stepStartGrid(intoCoordinate, -bit2, acrossCoordinate);
    }
    if ((at<uint32_t>(input) & 0x10u) == 0)
        return;
    sfx(0x45, 0x100);
    if (c.yPos == 0xf || c.yPos == 0)
    {
        if (c.direction == NORTH || c.direction == SOUTH)
            c.direction = EAST;
    }
    else if ((c.xPos == 0xf || c.xPos == 0) && (c.direction == EAST || c.direction == WEST))
        c.direction = NORTH;
    t.trackState.editState = 1;
}

// The start grid being turned: left and right a quarter each (on an edge straight round to face the other way), key 4
// places it as the track's first module and opens the edit menu, key 5 goes back to moving it.
void TEditProcessRotateStartGrid(ptr32<_sTrackInput> pInput, tTime)
{
    GuestFrame frame(0x70);
    const uint32_t sp = frame.local<uint8_t>(0).address;
    sTEditClass &t = te();
    sCursorDesc &c = t.cursor;
    const uint32_t input = pInput.address;
    const bool edge = c.yPos == 0xf || c.xPos == 0xf || c.yPos == 0 || c.xPos == 0;
    uint32_t response = at<uint32_t>(input);
    const bool left = (response & 1u) != 0, right = !left && (response & 2u) != 0;
    if (left || right)
    {
        sfx(0x44, 0xf6);
        const uint32_t direction = static_cast<uint32_t>(c.direction);
        if (edge)
        {
            if (direction <= 3u)
                c.direction = static_cast<eDirection>(direction ^ 2u);
        }
        else if (left)
            c.direction = static_cast<eDirection>(direction == 0u ? 3u : direction - 1u);
        else
            c.direction = static_cast<eDirection>(direction == 3u ? 0u : direction + 1u);
        response = at<uint32_t>(input);
    }
    if ((response & 0x10u) != 0)
    {
        const uint32_t modules = t.pEditModuleList.address;
        sfx(0x45, 0x100);
        t.trackDesc.startOrientation = c.direction;
        t.trackDesc.startYPos = static_cast<uint8_t>(c.yPos);
        t.trackDesc.startElevation = c.elevation;
        t.trackDesc.startXPos = static_cast<uint8_t>(c.xPos);
        AddTrackModuleToTrackModuleList(0, 1);
        const uint32_t tiles = at<uint32_t>(modules + 0x14u);
        TEditPlaceModule(c.xPos, c.yPos, at<int16_t>(tiles), c.elevation, c.direction);
        stepOffStartGrid();
        t.editMenu.slopeNdx = 2;
        t.editMenu.specialNdx = 3;
        t.trackState.editState = 2;
        t.editMenu.rowNdx = 0;
        t.editMenu.moduleNdx = 3;
        ClearCursorModuleList();
        TEditClearAllRestricts();
        TEditCheckCursorDirectionConstraints();
        availSlopeAndModule(getRegU32(&context(), 5));
        AddEditModuleToCursorModuleList(ptr32<_sEditModuleDesc>(at<uint32_t>(kGridSlopes + t.editMenu.slopeNdx * 0x2cu + t.editMenu.moduleNdx * 4u)));
        return;
    }
    if ((response & 0x20u) == 0)
        return;
    sfx(0x43, 0x100);
    // The grid's unit under the start grid emptied.
    const uint32_t unit = unitAddress(static_cast<uint16_t>(c.xPos) + (static_cast<uint16_t>(c.yPos) << 4));
    at<uint16_t>(sp + 2u) = 0;
    at<int16_t>(unit) = -1;
    at<int32_t>(unit + 4u) = 0;
    at<uint16_t>(unit + 2u) = at<uint16_t>(sp + 2u);
    t.trackState.editState = 0;
}

} // namespace rc

RC_DECOMP_FN_SETUP(TEditProcessEditMenuKeys, rc::fuzz::teditKeys)
RC_DECOMP_FN_SETUP(TEditProcessPlaceStartGrid, rc::fuzz::teditStartGrid)
RC_DECOMP_FN_SETUP(TEditProcessRotateStartGrid, rc::fuzz::teditStartGrid)
