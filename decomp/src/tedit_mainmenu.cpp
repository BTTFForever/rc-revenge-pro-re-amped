// Decompiled from TEDIT.c: the editor's menu (start): its display step runs the menu and acts on what was chosen: New
// Track, Load and Save (through the memory card screens), Random Track, Complete Track, Help, Options, the test races
// with their car menus, Quit. See tedit.h.

#include "tedit.h"
#include "tedit_fuzz.h"
#include "../rc_cars.h"

namespace rc {

using namespace tedit;

namespace {

void sfx(uint32_t sound) { guest_call<int16_t>(sym::SfxPlay, sound, 0x80, 0xf, 0xf, 0x100, 0, 0); }

uint32_t menuSystem() { return G(pMenuSystemG).address; }
uint32_t activeMenu() { return at<uint32_t>(menuSystem()); }
void setActiveMenu(uint32_t menu) { at<uint32_t>(menuSystem()) = menu; }
uint32_t &executeCode() { return at<uint32_t>(menuSystem() + 0x68u); }

bool carAvailable(uint32_t car) { return guest_call<int32_t>(sym::CheckSingleCarAvailability, car) == 1; }

void linkItems(uint32_t menu, uint32_t item, uint32_t code) { guest_call(sym::AMenuLinkItems, menu, item, 0, 0, code); }

// The scrolling car menu: the names of the cars that are unlocked.
void fillCarMenu(uint32_t header)
{
    const uint32_t groups = sym::scrollMenuTGRPsG;
    for (uint32_t i = 0; i < 16u; ++i)
        guest_call(sym::AMenuAddTextToTGRP, groups + i * 0x10u, 0);
    uint32_t count = 0, next = groups;
    for (uint32_t car = 1; car < 0x14u; ++car)
        if (carAvailable(car))
        {
            ++count;
            const uint32_t name = guest_call<uint32_t>(sym::GetCarText, car);
            guest_call(sym::AMenuAddTextToTGRP, next, name);
            next += 0x10u;
        }
    at<uint16_t>(header + 0x32u) = static_cast<uint16_t>(at<uint16_t>(header + 0x32u) | 0x40u); // evenMoreFlags
    at<int8_t>(header + 0x70u) = -1;                                                           // scrollStart
    at<uint16_t>(header + 0x6au) = 0x80;                                                       // height
    at<uint8_t>(header + 0x62u) = static_cast<uint8_t>(count);                                 // menuItems
    at<uint8_t>(header + 0x63u) = 0;                                                           // selection
    at<uint32_t>(header + 4u) = groups;                                                        // pMenuStart
    at<uint16_t>(sym::frontEndVarG + 0x2cau) = 0;                                              // scrollPTT
}

// A car's place in that menu (0 if it is not in it).
uint32_t carPosition(uint32_t current)
{
    uint32_t position = 0;
    for (uint32_t car = 1; car < 0x14u; ++car)
        if (carAvailable(car))
        {
            if (car == current)
                return position;
            ++position;
        }
    return 0;
}

// The car at a place in that menu (the first car's type if there is none).
uint32_t carOfSelection(int32_t selection)
{
    int32_t position = 0;
    for (uint32_t car = 1; car < 0x14u; ++car)
        if (carAvailable(car))
        {
            if (position == selection)
                return car;
            ++position;
        }
    return 1;
}

// CarToTag as the original calls it: its second argument is what CheckSingleCarAvailability left in $a1 (its reward
// table), so the tag is always the second player's form.
uint32_t carTag(uint32_t car) { return guest_call<uint32_t>(sym::CarToTag, car & 0xffu, sym::mainVarG + 0x3bcu); }

// Changing the theme or the music in Options counts as a change to the track.
void noteOptionChanges()
{
    sTEditClass &t = te();
    if (G(oldThemeTypeNdxG) != t.trackDesc.themeTypeNdx || G(oldSoundTrackNdxG) != t.trackDesc.soundTrackNdx)
        t.loadSaveFlags = 1;
}

// "The track has not been saved": Save (0x7fff2000) or go on to `next` (0x7fff2001).
void askUnsaved(uint32_t next)
{
    sTEditClass &t = te();
    t.trackState.menuNextState = static_cast<uint16_t>(next);
    at<uint32_t>(t.pTrackUnsaved.address) = t.pTrackMainMenu.address;
    const uint32_t menu = t.pTrackUnsaved.address;
    setActiveMenu(menu);
    linkItems(menu, 0, 0x7fff2000u);
    linkItems(activeMenu(), 1, 0x7fff2001u);
}

void emptyTrack()
{
    TEditClearTrack();
    ClearCursorModuleList();
    ClearTrackModuleList();
    TEditResetResourceMeter();
    TEditInitialiseCursor();
    TEditInitialiseEditMenu();
}

// Save: to the memory card screens if the track is complete, else "the track is incomplete".
void saveOrIncomplete()
{
    sTEditClass &t = te();
    if (TEditCheckForCompleteTrack() != 0)
    {
        t.trackState.menuCurrentState = 3;
        guest_call(sym::LoadSaveInit, 2);
        return;
    }
    const uint32_t menu = t.pTrackIncomplete.address;
    setActiveMenu(menu);
    linkItems(menu, 0, 0x7fff3000u);
}

// Leaves the editor for a test race on the track.
void startRace()
{
    at<uint32_t>(sym::mainVarG) = 4; // MAIN_QUIT_GAME
    at<uint32_t>(sym::mainVarG + 0xcu) = 1; // trackEdRace
    sfx(0x46);
}

// The menu's own state (menuCurrentState 0): what the menu's items and the message boxes' buttons ask for. True if
// the menu's code is to be cleared.
bool mainMenuCode(uint32_t code)
{
    sTEditClass &t = te();
    const uint32_t main = sym::mainVarG, front = sym::frontEndVarG;
    switch (code)
    {
    case 0x1000022cu: // Random Track
        sfx(0x45);
        if ((t.loadSaveFlags & 1u) != 0)
        {
            askUnsaved(5);
            return true;
        }
        TEditMenuDoRandomTrack();
        t.loadSaveFlags = 1;
        t.trackState.editState = 2;
        t.trackState.currentMode = 0;
        return true;
    case 0x10000227u: // Load Track
        sfx(0x45);
        noteOptionChanges();
        setActiveMenu(t.pTrackMainMenu.address);
        if ((t.loadSaveFlags & 1u) != 0)
        {
            askUnsaved(2);
            return true;
        }
        t.trackState.menuCurrentState = 2;
        guest_call(sym::LoadSaveInit, 3);
        return true;
    case 0x1000007du: // Quit
        sfx(0x45);
        noteOptionChanges();
        if ((t.loadSaveFlags & 1u) != 0)
        {
            askUnsaved(4);
            return true;
        }
        at<uint32_t>(main) = 4;
        return true;
    case 0x10000225u: // a race on a track that is not complete
        if (TEditCheckForCompleteTrack() == 0)
        {
            const uint32_t menu = t.pTrackUnPlayable.address;
            setActiveMenu(menu);
            linkItems(menu, 0, 0x7fff3000u);
            executeCode() = 0;
        }
        return false;
    case 0x10000226u: // New Track
        sfx(0x45);
        noteOptionChanges();
        if ((t.loadSaveFlags & 1u) != 0)
        {
            askUnsaved(1);
            return true;
        }
        emptyTrack();
        t.newCameraMode = 1;
        t.trackDesc.bestLapTime = kUnsavedLapTime;
        t.trackIsComplete = 0;
        t.trackState.currentMode = 0;
        t.placementViewDir = NORTH;
        t.loadSaveFlags = 0;
        t.trackState.editState = 0;
        return true;
    case 0x10000229u: // Help
        sfx(0x45);
        executeCode() = 0;
        setActiveMenu(t.pTrackComplete.address);
        at<uint8_t>(front + 0x2d4u) = 1;
        at<uint8_t>(front + 0x2d0u) = 1;
        at<int8_t>(front + 0x2d3u) = -1;
        at<int8_t>(front + 0x2d2u) = -1;
        t.trackState.currentMode = 2;
        at<uint16_t>(front + 0x2cau) = 0x32;
        at<uint16_t>(front + 0x2c8u) = 1;
        at<uint16_t>(front + 0x2ccu) = 0;
        guest_call(sym::BuildTrackEditorHelp, activeMenu());
        return false;
    case 0x10000228u: // Save Track
    case 0x7fff2000u: // "not saved": Save
        sfx(0x45);
        saveOrIncomplete();
        return true;
    case 0x1000022au:
        at<uint8_t>(activeMenu() + 0x63u) = 1;
        return false;
    case 0x1000022bu: // Complete Track
    {
        sfx(0x45);
        uint32_t menu = t.pTrackWarnings[3].address;
        if (t.trackState.editState == 2)
            menu = at<uint32_t>(kTE + 0x224u + static_cast<uint32_t>(completeTrack(kTE)) * 4u); // $s5: tEditVarG
        setActiveMenu(menu);
        linkItems(menu, 0, 0x7fff5000u);
        return true;
    }
    case 0x7fff1002u: // Options
        executeCode() = 0;
        setActiveMenu(t.pTrackOptionsMenu.address);
        return false;
    case 0x1000022du: // a single race, a time trial, a two-player race: the first player's car menu
    case 0x1000022eu:
    case 0x1000022fu:
    {
        fillCarMenu(activeMenu());
        const uint32_t position = carPosition(at<uint8_t>(main + 0x3dfu));
        at<uint8_t>(activeMenu() + 0x63u) = static_cast<uint8_t>(position);
        at<uint32_t>(front + 0x29cu) = 0x1000006bu; // the Track Editor's world
        at<uint32_t>(front + 0x298u) = 0;
        if (code == 0x1000022du)
        {
            at<uint32_t>(main + 0x34cu) = 0x100001fcu;
            at<uint8_t>(main + 0x369u) = 1;
            at<uint8_t>(main + 0x36au) = static_cast<uint8_t>(cars::singleCount(at<uint8_t>(front + 0x190u)) - 1); // enhancement: rc_cars.h
            at<uint32_t>(main + 0x3b4u) = at<uint32_t>(front + 0x18cu);
            at<uint32_t>(main + 0x3a8u) = at<uint32_t>(front + 0x188u);
            at<uint8_t>(main + 0x368u) = 0;
        }
        else if (code == 0x1000022eu)
        {
            at<uint32_t>(main + 0x34cu) = 0x100001feu;
            at<uint8_t>(main + 0x369u) = 1;
            at<uint32_t>(main + 0x3b4u) = 8;
            at<uint8_t>(main + 0x368u) = 0;
            at<uint8_t>(main + 0x36au) = 0;
            at<uint32_t>(main + 0x3a8u) = 0;
        }
        else
        {
            // Two players: the code stays for the second car menu's first step.
            t.trackState.menuCurrentState = 7;
            at<uint32_t>(main + 0x34cu) = 0x10000200u;
            at<uint8_t>(main + 0x368u) = 1;
            at<uint8_t>(main + 0x369u) = 2;
            at<uint8_t>(main + 0x36au) = static_cast<uint8_t>(cars::multiCount(at<uint8_t>(front + 0x194u)) - 2); // enhancement: rc_cars.h
            at<uint32_t>(main + 0x3b4u) = at<uint32_t>(front + 0x18cu);
            at<uint32_t>(main + 0x3a8u) = at<uint32_t>(front + 0x188u);
            return false;
        }
        t.trackState.menuCurrentState = 6;
        return true;
    }
    case 0x11000000u: // back
        sfx(0x43);
        noteOptionChanges();
        t.trackState.currentMode = 0;
        executeCode() = 0;
        guest_call(sym::InputResetAllHit);
        return false;
    case 0x7fff2001u: // "not saved": go on
        sfx(0x45);
        t.trackState.menuCurrentState = t.trackState.menuNextState;
        if (t.trackState.menuNextState == 2)
            guest_call(sym::LoadSaveInit, 3);
        return true;
    case 0x7fff3000u: // a message's OK: back to the grid
    case 0x7fff4000u:
    case 0x7fff5000u:
    case 0x1000007cu:
        sfx(0x45);
        t.trackState.currentMode = 0;
        executeCode() = 0;
        return false;
    default:
        return false;
    }
}

} // namespace

void TEditDisplayMainMenus(tTime, uint32 frameCnt)
{
    GuestFrame frame(0xb0);
    const uint32_t temp = frame.local<uint8_t>(0).address;
    sTEditClass &t = te();
    const uint32_t main = sym::mainVarG, front = sym::frontEndVarG;
    guest_call(sym::memset, temp, 0, 0xc);
    guest_call(sym::CellRenderInit, temp);
    TEditDisplay3DGridLines();
    const auto finish = [] {
        TEditRenderCursor();
        TEditRenderTrack();
    };
    // A car menu's step: false while nothing is chosen (back returns to `back`).
    const auto carMenuChosen = [&](uint32_t back)
    {
        guest_call(sym::AMenuProcessAll, activeMenu());
        const uint32_t code = executeCode();
        if (code == 0x11000001u)
        {
            t.trackState.menuCurrentState = static_cast<uint16_t>(back);
            return false;
        }
        return code != 0;
    };
    switch (t.trackState.menuCurrentState)
    {
    case 0:
        guest_call(sym::AMenuProcessAll, activeMenu());
        guest_call(sym::MenuItemDisabler, activeMenu());
        if (mainMenuCode(executeCode()))
            executeCode() = 0;
        break;
    case 1: // New Track, after "not saved"
        emptyTrack();
        t.newCameraMode = 1;
        t.trackState.currentMode = 0;
        t.trackDesc.bestLapTime = kUnsavedLapTime;
        t.placementViewDir = NORTH;
        t.trackIsComplete = 0;
        t.loadSaveFlags = 0;
        t.trackState.editState = 0;
        break;
    case 5: // Random Track, after "not saved"
        TEditMenuDoRandomTrack();
        t.loadSaveFlags = 1;
        t.trackState.editState = 2;
        t.trackState.currentMode = 0;
        break;
    case 2: // the load screens
    case 3: // the save screens
    {
        at<uint8_t>(front + 0x2d4u) = 0;
        const int32_t result = guest_call<int32_t>(sym::LoadSaveProcess, frameCnt);
        if (result == -1)
        {
            const uint32_t system = menuSystem();
            t.trackState.menuCurrentState = 0;
            at<uint32_t>(system) = t.pTrackMainMenu.address;
            break;
        }
        if (result != 1)
            break;
        if (t.trackState.menuCurrentState == 2)
        {
            emptyTrack();
            t.newCameraMode = 1;
            t.placementViewDir = NORTH;
            t.trackDesc.bestLapTime = kUnsavedLapTime;
            t.trackIsComplete = 0;
            TEditGenerateTrackFromPermSaveBuffer();
        }
        else
            TEditSaveTrackDescToPermSaveBuffer();
        t.trackState.currentMode = 0;
        t.loadSaveFlags = 6;
        break;
    }
    case 4: // Quit, after "not saved": the changed track is kept for the next visit
        if ((t.loadSaveFlags & 1u) == 0)
            break;
        TEditSaveTrackDescToPermSaveBuffer();
        t.loadSaveFlags |= 2u;
        if (TEditCheckForCompleteTrack() != 0)
            t.loadSaveFlags |= 4u;
        else
            t.loadSaveFlags &= 0xfffffffbu;
        at<uint32_t>(main) = 4;
        sfx(0x46);
        break;
    case 6: // the car menu of a one-player race
        if (!carMenuChosen(0))
            break;
        TEditSaveTrackDescToPermSaveBuffer();
        t.loadSaveFlags |= 6u;
        {
            const uint32_t car = carOfSelection(at<int8_t>(activeMenu() + 0x63u));
            at<uint8_t>(main + 0x3dfu) = static_cast<uint8_t>(car);
            at<uint32_t>(front + 0x280u) = carTag(car);
            at<uint32_t>(sym::carListVarG) = at<uint8_t>(main + 0x3dfu);
        }
        startRace();
        break;
    case 7: // the first player's car menu of a two-player race: on to the second's
    {
        if (!carMenuChosen(0))
            break;
        sfx(0x45);
        const uint32_t car = carOfSelection(at<int8_t>(activeMenu() + 0x63u));
        at<uint8_t>(main + 0x3dfu) = static_cast<uint8_t>(car);
        const uint32_t tag = carTag(car);
        const uint32_t second = t.pTrackPlayer2CarSelectMenu.address, first = activeMenu();
        setActiveMenu(second);
        at<uint32_t>(second) = first;
        at<uint32_t>(front + 0x280u) = tag;
        const uint32_t header = activeMenu();
        at<uint32_t>(sym::carListVarG) = at<uint8_t>(main + 0x3dfu);
        fillCarMenu(header);
        const uint32_t position = carPosition(at<uint8_t>(main + 0x3e0u));
        at<uint8_t>(activeMenu() + 0x63u) = static_cast<uint8_t>(position);
        t.trackState.menuCurrentState = 8;
        executeCode() = 0;
        break;
    }
    case 8: // the second player's car menu
        if (!carMenuChosen(7))
            break;
        TEditSaveTrackDescToPermSaveBuffer();
        t.loadSaveFlags |= 6u;
        {
            const uint32_t car = carOfSelection(at<int8_t>(activeMenu() + 0x63u));
            at<uint8_t>(main + 0x3e0u) = static_cast<uint8_t>(car);
            at<uint32_t>(front + 0x284u) = carTag(car);
            at<uint32_t>(sym::carListVarG + 0xcu) = at<uint8_t>(main + 0x3e0u);
        }
        startRace();
        break;
    default:
        break;
    }
    finish();
}

} // namespace rc

RC_DECOMP_FN_BLOCKING_SETUP_HEAP(TEditDisplayMainMenus, rc::fuzz::teditMainMenus) // the memory card screens; Complete Track draws frames
