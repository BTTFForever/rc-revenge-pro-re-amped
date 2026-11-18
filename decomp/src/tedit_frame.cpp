// Decompiled from TEDIT.c: the grid editor's frame (the pad read into the editor's input, the control step, the
// display step) and the camera that follows the cursor. See tedit.h.

#include "tedit.h"
#include "tedit_fuzz.h"

namespace rc {

using namespace tedit;

// The pad as the editor reads it: response bits for the keys (1 left, 2 right, 4 up, 8 down from keys 7, 5, 4, 6;
// 0x10-0x200 for keys 14, 12, 13, 15, 0 and 3), and the left stick's direction (past 0x50) added when it changes.
ptr32<_sTrackInput> TEditGetInput(tTime)
{
    GuestFrame frame(0x30);
    sTEditClass &t = te();
    std::memcpy(guest_host(kTE + 0x8cu), guest_host(kTE + 0x7cu), 0x10u);
    t.input.response = 0;
    const auto hit = [](uint8_t key) { return guest_call<bool_>(sym::InputHit, key) != 0; };
    if (hit(7))
        t.input.response |= 1u;
    else if (hit(5))
        t.input.response |= 2u;
    if (hit(4))
        t.input.response |= 4u;
    else if (hit(6))
        t.input.response |= 8u;
    static constexpr uint8_t kKeys[] = {14, 12, 13, 15, 0, 3};
    for (uint32_t i = 0; i < 6u; ++i)
        if (hit(kKeys[i]))
            t.input.response |= 0x10u << i;
    at<uint32_t>(kTE + 0x84u) = at<uint32_t>(sym::inputVarG + 0xbcu); // the left and right sticks
    uint32_t stick = 0;
    const int32_t x = t.input.x0, y = t.input.y0;
    if ((x < 0 ? -x : x) > 0x50)
        stick = x > 0 ? 2u : 1u;
    if ((y < 0 ? -y : y) > 0x50)
        stick |= y > 0 ? 8u : 4u;
    if (t.input.analog1LastInput != stick)
    {
        t.input.analog1LastInput = stick;
        t.input.response |= stick;
    }
    return ptr32<_sTrackInput>(kTE + 0x7cu);
}

// The grid editor's control step (not while a warning or message is up): the 2D map while key 10 is held, the view
// turned with keys 8 and 9, the main menu (0x200), the far view (key 11), else the edit state's handler.
void TEditControlGridEdit(tTime time)
{
    GuestFrame frame(0x70);
    sTEditClass &t = te();
    if (static_cast<uint16_t>(t.trackState.editState - 3) < 2u)
        return;
    const ptr32<_sTrackInput> input = TEditGetInput(time);
    t.display2DGridMap = 0;
    if (guest_call<bool_>(sym::InputHeld, 10) != 0)
        t.display2DGridMap = 1;
    if (t.cameraTurning == 1)
        return;
    if (guest_call<bool_>(sym::InputHit, 8) != 0)
        t.newPlacementViewDir = static_cast<eDirection>((t.placementViewDir + 1) & 3);
    else if (guest_call<bool_>(sym::InputHit, 9) != 0)
        t.newPlacementViewDir = t.placementViewDir == NORTH ? WEST : static_cast<eDirection>(t.placementViewDir - 1);
    const bool_ turning = t.cameraTurning;
    t.placementViewDir = t.newPlacementViewDir;
    if (turning == 1 || t.display2DGridMap == 1)
        return;
    if ((input->response & 0x200u) != 0)
    {
        guest_call<int16_t>(sym::SfxPlay, 0x45, 0x80, 0xf, 0xf, 0x100, 0, 0);
        const uint32_t sound = t.trackDesc.soundTrackNdx, theme = t.trackDesc.themeTypeNdx;
        t.trackState.currentMode = 1;
        G(pMenuSystemG)->pActiveMenu = t.pTrackMainMenu;
        G(oldThemeTypeNdxG) = theme;
        G(oldSoundTrackNdxG) = sound;
        t.trackState.menuCurrentState = 0;
        return;
    }
    if (guest_call<bool_>(sym::InputHit, 11) != 0)
    {
        t.newCameraMode = 1;
        t.trackState.currentMode = 3;
        return;
    }
    switch (t.trackState.editState)
    {
    case 0:
        TEditControlPlaceStartGrid(input, time);
        break;
    case 1:
        TEditControlRotateStartGrid(input, time);
        break;
    case 2:
        TEditControlEditing(input, time);
        break;
    default:
        break;
    }
}

// The grid editor's display step: the 2D map while asked for, the grid, then by edit state the key bar and the cursor
// (blinking while placing the start grid or editing), the track, the edit menu and the meter; while a warning or
// message is up its menu is processed, and its OK goes back to editing.
void TEditDisplayGridEdit(tTime time)
{
    GuestFrame frame(0x40);
    const uint32_t temp = frame.local<uint8_t>(0).address;
    sTEditClass &t = te();
    guest_call(sym::memset, temp, 0, 0xc);
    if (t.display2DGridMap == 1)
    {
        TEditDisplay2DGridMap();
        TEditDisplay2DGridLines();
    }
    guest_call(sym::CellRenderInit, temp);
    guest_call(sym::TEditDisplay3DGridLines);
    const bool blink = time % 1000 < 500;
    switch (t.trackState.editState)
    {
    case 0:
        guest_call(sym::AMenuDisplayOptions, t.pTrackMenuBar[0]);
        if (blink)
            TEditRenderCursor();
        break;
    case 1:
        guest_call(sym::AMenuDisplayOptions, t.pTrackMenuBar[1]);
        TEditRenderCursor();
        break;
    case 2:
        guest_call(sym::AMenuDisplayOptions, t.pTrackMenuBar[2]);
        guest_call(sym::TEditDEBUGDisplayCursor);
        if (blink)
            TEditRenderCursor();
        guest_call(sym::TEditRenderTrack);
        guest_call(sym::TEditRenderEditMenu, time);
        TEditRenderResourceMeter();
        break;
    case 3:
    case 4:
        guest_call(sym::AMenuProcessAll, G(pMenuSystemG)->pActiveMenu);
        guest_call(sym::TEditDEBUGDisplayCursor);
        if (G(pMenuSystemG)->executeCode == 0x7fff0001)
        {
            guest_call<int16_t>(sym::SfxPlay, 0x45, 0x80, 0xf, 0xf, 0x100, 0, 0);
            t.trackState.editState = 2;
            G(pMenuSystemG)->executeCode = 0;
            guest_call(sym::InputResetAllHit);
        }
        TEditRenderCursor();
        guest_call(sym::TEditRenderTrack);
        guest_call(sym::TEditRenderEditMenu, time);
        TEditRenderResourceMeter();
        break;
    default:
        break;
    }
}

// The camera looks down on the cursor from behind, turned to the placement view (far view, mode 3: on the grid's
// middle from higher up); a new camera mode starts it from its default distance.
void TEditCameraFollowCursor()
{
    GuestFrame frame(0x30);
    const uint32_t sp = frame.local<uint8_t>(0).address; // cursor position 0x0, offset 0x10, angles 0x20
    sTEditClass &t = te();
    const uint32_t tiles = G(tileVarG).pTrackTiles.address;
    const bool far = t.trackState.currentMode == 3;
    if (far)
    {
        const uint32_t cell = at<uint16_t>(tiles + 2u);
        at<int32_t>(sp + 4u) = 0;
        at<int32_t>(sp) = static_cast<int32_t>((cell << 3) - 0x400u) << 8;
        at<int32_t>(sp + 8u) = static_cast<int32_t>((cell << 3) - 0x400u) << 8;
    }
    else
    {
        const uint32_t cell = at<uint16_t>(tiles + 2u);
        at<int32_t>(sp + 4u) = t.cursor.elevation << 8;
        at<int32_t>(sp) = static_cast<int32_t>(static_cast<uint32_t>(t.cursor.xPos) * cell) << 8;
        at<int32_t>(sp + 8u) = static_cast<int32_t>(static_cast<uint32_t>(t.cursor.yPos) * cell) << 8;
    }
    switch (t.placementViewDir)
    {
    case EAST:
        at<int16_t>(sp + 0x22u) = -0x4000;
        break;
    case NORTH:
        at<int16_t>(sp + 0x22u) = 0;
        break;
    case SOUTH:
        at<int16_t>(sp + 0x22u) = 0x7fff;
        break;
    case WEST:
        at<int16_t>(sp + 0x22u) = 0x4000;
        break;
    default:
        break; // the heading stays as the stack held it
    }
    uint32_t camera;
    if (t.newCameraMode == 0)
    {
        camera = at<uint32_t>(sym::cameraVarG + 0xb70u);
        for (uint32_t i = 0; i < 12u; i += 4u)
            at<int32_t>(sp + 0x10u + i) = at<int32_t>(camera + 0x70u + i) - at<int32_t>(sp + i);
    }
    else
    {
        const uint32_t cell = at<uint16_t>(tiles + 2u);
        t.newCameraMode = 0;
        camera = at<uint32_t>(sym::cameraVarG + 0xb70u);
        at<int32_t>(sp + 0x10u) = 0;
        at<int32_t>(sp + 0x14u) = 0;
        at<int32_t>(sp + 0x18u) = 0;
        if (far)
        {
            at<int32_t>(camera + 0x5cu) = 0x100000;
            at<int32_t>(camera + 0x58u) = 0;
            at<int32_t>(camera + 0x60u) = -static_cast<int32_t>(cell * 0x1cu) << 8;
        }
        else
        {
            at<int32_t>(camera + 0x60u) = -static_cast<int32_t>(cell * 5u) << 8;
            at<int32_t>(camera + 0x58u) = 0;
            at<int32_t>(camera + 0x5cu) = 0;
        }
    }
    const uint16_t pitch = far ? 0x2000 : 0x1800;
    at<int16_t>(sp + 0x24u) = 0;
    at<uint16_t>(sp + 0x20u) = pitch;
    at<int32_t>(camera + 0x4cu) = at<int32_t>(sp);
    const uint16_t freePitch = static_cast<uint16_t>(at<uint16_t>(camera + 0x44u) - pitch);
    at<int32_t>(camera + 0x50u) = at<int32_t>(sp + 4u);
    at<uint16_t>(camera + 0xa0u) = freePitch;
    at<uint16_t>(camera + 0xa2u) = static_cast<uint16_t>(at<uint16_t>(camera + 0x46u) - at<uint16_t>(sp + 0x22u));
    at<int32_t>(camera + 0x54u) = at<int32_t>(sp + 8u);
    at<uint16_t>(camera + 0xa4u) = static_cast<uint16_t>(at<uint16_t>(camera + 0x48u) - at<uint16_t>(sp + 0x24u));
    at<int32_t>(camera + 0xb0u) = at<int32_t>(sp + 0x10u);
    at<uint16_t>(camera + 0xa6u) = at<uint16_t>(sp + 0x20u);
    at<int32_t>(camera + 0xb4u) = at<int32_t>(sp + 0x14u);
    at<uint16_t>(camera + 0xa8u) = at<uint16_t>(sp + 0x22u);
    at<int32_t>(camera + 0xb8u) = at<int32_t>(sp + 0x18u);
    at<uint16_t>(camera + 0xaau) = at<uint16_t>(sp + 0x24u);
}

} // namespace rc

RC_DECOMP_FN_SETUP(TEditGetInput, rc::fuzz::teditGridControl)
RC_DECOMP_FN_SETUP(TEditControlGridEdit, rc::fuzz::teditGridControl)
RC_DECOMP_FN_SETUP(TEditDisplayGridEdit, rc::fuzz::teditGridDisplay)
RC_DECOMP_FN_SETUP(TEditCameraFollowCursor, rc::fuzz::teditPlain)
