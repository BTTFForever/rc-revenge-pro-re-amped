// Decompiled from FRONT.c: what the front end does with its menus. ParseSelectionTags acts on the code of the item just
// selected (a car, a world, a track, a cup, a race type, an option page, a language...); the others show and hide
// items, build the confirmation text, the vehicle stats and the key-definition page, and show the reward messages.
//
// Written from the instructions (Ghidra's C as a first reading). The functions that test a car tag call TagToCar,
// which returns its caller's $a1 for a tag it does not know: the natives pass what the original holds there.
//
// Europe: BuildKeyDefinitions leaves the page's y alone, ProcessRewardMessage flies the camera to the menus when the
// "game loaded" message (14) is dismissed, and DisplayRCRevengeBanner's logo comes 6 lines further down (PAL).

#include "front.h"
#include "front_fuzz.h"

#include <optional>
#include <string>
#include <cstdio>
#include <cstdlib>

RC_VERSION_VARIANT(ParseSelectionTags)
RC_VERSION_VARIANT(BuildKeyDefinitions)
RC_VERSION_VARIANT(ProcessRewardMessage)
RC_VERSION_VARIANT(DisplayRCRevengeBanner)

namespace rc {

namespace enhance {
void install(ptr32<sUIgroup> pUIGroup); // rc_enhance_menu.cpp
}

using namespace front;

namespace {

constexpr uint32_t kTEditFlags = sym::tEditVarG + 0x250u; // tEditVarG.loadSaveFlags: bit 2, a Track Editor track exists

ptr32<sAMenuHeader> activeMenu() { return menuSystem().pActiveMenu; }

void selectSound() { amenu::playSfx(0x45, 0x100); }
void startSound() { amenu::playSfx(0x46, 0x128); }

// A choice that starts the game: the start sound, the music fades, and the main loop goes on to the race.
void startRace()
{
    startSound();
    guest_call<void>(sym::RCMusicPause, 0xf);
}

// A level's name (1-based, wrapping every 24).
ptr32<char> levelName(int32_t level)
{
    return AMenuGetText(feLevel()->pExtraLevelData[(level - 1) % 24].pTextGroup);
}

// The car-selection menu's item for a car type (null if none): the item's name is the car's.
ptr32<sMenuItemStd> carItem(eObjectType type, uint32_t &a1)
{
    ptr32<sMenuItemStd> p = G(pCarSelectMenu)->pMenuStart;
    while (p && tagToCar(p->selectionTag, a1) != type)
        p = p->pNext;
    return p;
}

// The current value of item n of a menu's variable.
int32_t itemValue(ptr32<sAMenuHeader> pHeader, int32_t n)
{
    return AMenuGetMIT(pHeader, n)->pMenuVar->value;
}

// Back to the menu this one was entered from (an option page's OK).
ptr32<sAMenuHeader> leaveMenu()
{
    sMenuSystem &ms = menuSystem();
    const ptr32<sAMenuHeader> pMenu = ms.pActiveMenu;
    ms.pLastActiveMenu = pMenu;
    ms.pActiveMenu = pMenu->pLinkMenuReturn;
    return pMenu;
}

// What a selection code does.
enum class Code
{
    None, Player1Car, Player2Car, Cup, World, StartRace, Language0, Language1, Language2, Language3, Progress,
    TrackEditor, Arcade, SingleRace, TimeTrial, TwoPlayer, GameLoad, GameSave, TrackLoad, Confirm, GameConfig,
    SfxMode, ConfigP1, ConfigP2, LockedLevel, Back,
};

Code classify(uint32_t code)
{
    switch (code)
    {
    case 0x10000038u: case 0x10000039u: case 0x1000003bu: case 0x1000003fu: case 0x10000042u: case 0x10000045u:
    case 0x1000004du: case 0x10000051u: case 0x10000058u: case 0x1000005bu: case 0x1000005fu: case 0x10000062u:
    case 0x10000068u: case 0x10000070u: case 0x10000072u: case 0x10000291u: case 0x10000292u: case 0x10000293u:
    case 0x10000294u:
        return Code::Player1Car;
    case 0x1000003au: case 0x1000003cu: case 0x10000040u: case 0x10000043u: case 0x10000046u: case 0x10000048u:
    case 0x1000004eu: case 0x10000052u: case 0x10000059u: case 0x1000005cu: case 0x10000060u: case 0x10000063u:
    case 0x10000069u: case 0x10000071u: case 0x10000073u: case 0x10000295u: case 0x10000296u: case 0x10000297u:
    case 0x10000298u:
        return Code::Player2Car;
    case 0x1000003du: case 0x1000003eu: case 0x1000004bu: case 0x1000004cu: case 0x1000005du: case 0x1000005eu:
    case 0x10000066u: case 0x10000067u:
        return Code::Cup;
    case 0x10000041u: case 0x1000004fu: case 0x10000053u: case 0x10000056u: case 0x10000065u: case 0x1000006bu:
    case 0x10000299u:
        return Code::World;
    case 0x10000044u: return Code::StartRace;
    case 0x10000047u: return Code::Language0;
    case 0x10000049u: return Code::Language1;
    case 0x1000004au: return Code::Language2;
    case 0x1000006au: return Code::Language3;
    case 0x1000007bu: case 0x100001f2u: case 0x10000207u: return Code::Progress;
    case 0x100001f8u: return Code::TrackEditor;
    case 0x100001fau: return Code::Arcade;
    case 0x100001fcu: return Code::SingleRace;
    case 0x100001feu: return Code::TimeTrial;
    case 0x10000200u: return Code::TwoPlayer;
    case 0x10000204u: return Code::GameLoad;
    case 0x10000205u: return Code::GameSave;
    case 0x10000206u: return Code::TrackLoad;
    case 0x10000210u: return Code::Confirm;
    case 0x10000285u: case 0x10000287u: case 0x1000028du: case 0x1000028eu: return Code::GameConfig;
    case 0x10000286u: case 0x10000288u: case 0x10000289u: return Code::SfxMode;
    case 0x1000028au: return Code::ConfigP1;
    case 0x1000028cu: return Code::ConfigP2;
    case 0x10101010u: return Code::LockedLevel;
    case 0x11000000u: return Code::Back;
    default: return Code::None;
    }
}

} // namespace

// Acts on the menu system's execute code (the item just selected), then points the camera at the active menu's
// place. Also keeps the sound volumes in step with the options menu's sliders and the carousel on the highlighted
// car. Returns 1 if the camera was sent on its way to the first menu.
int32_t ParseSelectionTags()
{
    GuestFrame frame(0xb0);
    mainClass &m = mainVars();
    sFrontEndClass &f = fe();
    const ptr32<_sFELevelData> pFELevelData = f.pFELevelData;
    const uint32_t pCamera = at<uint32_t>(sym::cameraVarG + 0xb70u);
    int32_t language = -1;
    int32_t retVal = 0;
    // The options menu's volume sliders.
    const ptr32<sMenuItemStd> pItem = AMenuGetMIT(activeMenu(), activeMenu()->selection);
    if (pItem && pItem->pMenuVar)
    {
        const int32_t tag = pItem->pMenuVar->tagMVar;
        if (static_cast<uint32_t>(tag) == 0x1000001fu || static_cast<uint32_t>(tag) == 0x10000026u)
        {
            const int32_t value = pItem->pMenuVar->value;
            int32_t flag = 0;
            if (value != at<int32_t>(sym::rcmusicVarG + 0x24u) && static_cast<uint32_t>(tag) == 0x1000001fu)
                flag = 2;
            else if (value != at<int32_t>(sym::revSfxVarG + 0x1148u) && static_cast<uint32_t>(tag) == 0x10000026u)
                flag = 2;
            at<int32_t>(sym::rcmusicVarG + 0x20u) = itemValue(activeMenu(), 1);
            at<int32_t>(sym::revSfxVarG + 0x1144u) = itemValue(activeMenu(), 2);
            if (pItem->pMenuVar->value != at<int32_t>(sym::revSfxVarG + 0x1148u) &&
                static_cast<uint32_t>(pItem->pMenuVar->tagMVar) == 0x10000026u)
                guest_call<void>(sym::RevSfxModifyVolume, flag);
            guest_call<void>(sym::RCMusicModifyVolume, 0x19, flag);
        }
        else
            guest_call<void>(sym::RevSfxStopPauseSfx);
    }
    // A car menu: its selection and stored car are player 1's (1) or player 2's (2); the carousel turns to the car.
    const ptr32<sUIgroup> pGroups = pFELevelData->pUIGroup;
    if (const ptr32<sAMenuHeader> pMenu = activeMenu(); (pMenu->moreFlags & 0x20u) != 0)
    {
        const uint32_t which = pMenu->pSelection.address;
        if (which == 1)
        {
            pMenu->pSelection = ptr32<uint8>(guest_addr(&m.car1CarMenuPos));
            pMenu->pValueStore = ptr32<uint8>(guest_addr(&m.p1Car));
            pMenu->selection = static_cast<int8>(m.car1CarMenuPos);
        }
        else if (which != 0)
        {
            if (which == 2)
            {
                pMenu->pSelection = ptr32<uint8>(guest_addr(&m.car2CarMenuPos));
                pMenu->pValueStore = ptr32<uint8>(guest_addr(&m.p2Car));
            }
            activeMenu()->selection = static_cast<int8>(*activeMenu()->pSelection);
        }
        uint32_t entry = *activeMenu()->pSelection;
        const eObjectType type = tagToCar(AMenuGetMIT(activeMenu(), static_cast<int32_t>(entry))->selectionTag, entry);
        CarouselMoveTo(type);
        if (const ptr32<uint8> pStore = activeMenu()->pValueStore)
            *pStore = static_cast<uint8>(type);
    }
    sMenuSystem &ms = menuSystem();
    const uint32_t code = ms.executeCode;
    switch (classify(code))
    {
    case Code::Player1Car:
    case Code::Player2Car:
    {
        const int32_t player = classify(code) == Code::Player1Car ? 0 : 1;
        uint32_t a1 = 0; // the code is a car's (any value would do)
        const eObjectType type = tagToCar(code, a1);
        G(carListVarG).car[player].type = type;
        f.carTag[player] = static_cast<eObjectType>(menuSystem().executeCode);
        f.lastWorldSel = 0xffffffffu;
        (player == 0 ? m.p1Car : m.p2Car) = static_cast<uint8>(type);
        break;
    }
    case Code::Cup:
    {
        f.cup = TagToCup(code);
        m.cupRaceNo = 0;
        const ptr32<cupDetails> pCup = feLevel()->pCups[f.cup - 1];
        const uint8 tracks[6] = {pCup->track1, pCup->track2, pCup->track3, pCup->track4, pCup->track5, pCup->track6};
        for (int32_t i = 0; i < 6; ++i)
            m.trackList[i] = tracks[i];
        // TagToCar's fallback register: mainVarG's address for the first car, its upper half after.
        uint32_t a1 = sym::mainVarG;
        for (int32_t i = 0; i < 8; ++i)
        {
            m.cupCarList[i] = tagToCar(feLevel()->pCups[f.cup - 1]->cars[i], a1);
            a1 = (sym::mainVarG + 0x8000u) & 0xffff0000u; // set again after the call (lui)
        }
        f.pCupSelectMenu = menuSystem().pLastActiveMenu;
        buildConfirmText(sym::frontEndVarG); // $t0 as the original leaves it
        break;
    }
    case Code::World:
        f.world = static_cast<int32>(code);
        f.pWorldSelectMenu = ms.pLastActiveMenu;
        f.cup = 0;
        if (static_cast<uint32_t>(f.world) == 0x1000006bu) // the Track Editor's world: its track if there is one
        {
            if ((at<uint32_t>(kTEditFlags) & 4u) == 0)
            {
                ms.selectDisabled = 1;
                ms.pActiveMenu = ms.pLastActiveMenu;
            }
            else
            {
                startRace();
                m.level = 0x61;
                m.toDo = static_cast<eMainToDo>(3);
                m.status = static_cast<eMain>(1);
                guest_call<uint32_t>(sym::strcpy, guest_addr(m.trackNames[0]), GetGeneralString(8));
            }
        }
        break;
    case Code::StartRace:
        if (f.cup == 0)
        {
            if (static_cast<uint32_t>(f.world) == 0x1000006bu)
            {
                if ((at<uint32_t>(kTEditFlags) & 4u) != 0)
                {
                    startRace();
                    m.toDo = static_cast<eMainToDo>(3);
                    m.level = 0x61;
                    m.status = static_cast<eMain>(1);
                }
            }
            else
            {
                uint32_t t0 = sym::frontEndVarG;
                m.level = static_cast<uint8>(worldTrackToLevel(t0));
                startRace();
                m.toDo = static_cast<eMainToDo>(3);
                m.status = static_cast<eMain>(1);
                guest_call<uint32_t>(sym::strcpy, guest_addr(m.trackNames[0]), levelName(m.level));
            }
        }
        else
        {
            // A cup: its first track, and the names of all six.
            startRace();
            m.status = static_cast<eMain>(1);
            m.toDo = static_cast<eMainToDo>(3);
            m.level = pFELevelData->pCups[f.cup - 1]->track1;
            const ptr32<sExtraLevelData> pExtra = feLevel()->pExtraLevelData;
            for (int32_t i = 0; i < 6; ++i)
                guest_call<uint32_t>(sym::strcpy, guest_addr(m.trackNames[i]),
                                     AMenuGetText(pExtra[(m.trackList[i] - 1) % 24].pTextGroup));
        }
        break;
    case Code::Language0: language = 0; break;
    case Code::Language1: language = 1; break;
    case Code::Language2: language = 2; break;
    case Code::Language3: language = 3; break;
    case Code::Progress: // the progress, time-trial and credits pages
        if (f.flagPTT == 0)
        {
            f.scrollPTTitemHeight = -1;
            f.scrollPTTheaderHeight = -1;
            f.scrollPTT = 0x14;
            f.scrollPTTspeed = 1;
            f.scrollPTTHeight = 0;
            if (code == 0x100001f2u)
            {
                selectSound();
                f.fisrtDrawFlagPTT = 1;
                f.flagPTT = 2;
                guest_call<void>(sym::BuildTimeTrials, menuSystem().pActiveMenu);
            }
            else if (code == 0x1000007bu)
            {
                selectSound();
                f.fisrtDrawFlagPTT = 1;
                f.flagPTT = 1;
                guest_call<void>(sym::BuildProgress, menuSystem().pActiveMenu, guest_addr(&f.gameStateData), -1);
            }
            else if (code == 0x10000207u)
            {
                selectSound();
                f.flagPTT = 3;
            }
        }
        break;
    case Code::TrackEditor:
        amenu::playSfx(0x46, 0x100);
        m.status = static_cast<eMain>(1);
        m.toDo = static_cast<eMainToDo>(2);
        break;
    case Code::Arcade:
        m.noHuman = 1;
        m.noAI = 7;
        m.iNumberOfLaps = 3;
        m.pickupsOnOff = f.gameStateData.gameConfig.pickupsOnOff;
        f.gameStateData.gameConfig.trackDirHints = 0;
        m.livesLeft = 3;
        m.splitScreen = 0;
        m.raceType = ms.executeCode;
        break;
    case Code::SingleRace:
        m.noHuman = 1;
        m.noAI = static_cast<uint8>(f.gameStateData.gameConfig.singleCarCount - 1);
        m.iNumberOfLaps = f.gameStateData.gameConfig.iNumberOfLaps;
        m.pickupsOnOff = f.gameStateData.gameConfig.pickupsOnOff;
        m.splitScreen = 0;
        m.raceType = ms.executeCode;
        break;
    case Code::TimeTrial:
        m.noHuman = 1;
        m.iNumberOfLaps = 9;
        m.splitScreen = 0;
        m.noAI = 0;
        m.pickupsOnOff = 0;
        m.raceType = ms.executeCode;
        break;
    case Code::TwoPlayer:
        m.splitScreen = 1;
        m.noHuman = 2;
        m.noAI = static_cast<uint8>(f.gameStateData.gameConfig.multiCarCount - 2);
        m.iNumberOfLaps = f.gameStateData.gameConfig.iNumberOfLaps;
        m.pickupsOnOff = f.gameStateData.gameConfig.pickupsOnOff;
        m.raceType = ms.executeCode;
        break;
    case Code::GameLoad:
    case Code::GameSave:
    case Code::TrackLoad:
    {
        selectSound();
        const Code c = classify(code);
        guest_call<void>(sym::LoadSaveInit, c == Code::GameLoad ? 1 : c == Code::GameSave ? 0 : 3);
        f.pStoredMenu = menuSystem().pActiveMenu;
        f.loadSaveFlag = menuSystem().executeCode;
        guest_call<void>(sym::TimerPause, guest_addr(&m.gameTimer));
        break;
    }
    case Code::Confirm:
        buildConfirmText(G(pMenuSystemG).address); // $t0 as the original leaves it
        break;
    case Code::GameConfig:
    {
        const ptr32<sAMenuHeader> pMenu = leaveMenu();
        sFrontGameConfig &c = f.gameStateData.gameConfig;
        c.iNumberOfLaps = itemValue(pMenu, 0);
        c.pickupsOnOff = itemValue(menuSystem().pLastActiveMenu, 1);
        c.singleCarCount = itemValue(menuSystem().pLastActiveMenu, 2);
        c.multiCarCount = itemValue(menuSystem().pLastActiveMenu, 3);
        break;
    }
    case Code::SfxMode:
        at<int32_t>(sym::sfxVarG + 0x8u) = itemValue(leaveMenu(), 0); // sfxVarG.mode
        break;
    case Code::ConfigP1:
        m.lastConfigP1 = static_cast<uint8>(itemValue(leaveMenu(), 0));
        break;
    case Code::ConfigP2:
        m.lastConfigP2 = static_cast<uint8>(itemValue(leaveMenu(), 0));
        break;
    case Code::LockedLevel:
    {
        uint32_t t0 = G(pMenuSystemG).address;
        const int32_t level = worldTrackToLevel(t0);
        if ((m.rewards.lvlAvailability[(level - 1) % 24] & 1u) != 1u)
        {
            f.cupsWonMessage = 0x0c;
            leaveMenu();
        }
        break;
    }
    case Code::Back:
        ms.pActiveMenu = ms.pLastActiveMenu;
        break;
    case Code::None:
        break;
    }
    // A language chosen: the menus start at the first menu. Otherwise the camera's next view is the active menu's.
    const uint32_t memoryCardDontCare = static_cast<uint32_t>(G(memoryCardDontCareG));
    if (language != -1)
    {
        m.activeLanguage = static_cast<eLanguages>(language);
        m.workingLanguage = static_cast<eLanguages>(language);
        ptr32<sUIgroup> g = pGroups;
        while (g->viewPosTag != kTagFirstMenu)
            g = g + 1;
        const ptr32<sAMenuHeader> pFirst = g->pSourceHeader;
        if (pFirst->pLinkMenuReturn)
        {
            pFirst->pLinkMenuReturn = nullptr;
            G(haveGotLanguageRecently) = 1;
        }
    }
    else if (f.flagPTT == 0 && f.cupsWonMessage == 0)
    {
        ptr32<sUIgroup> g = pGroups + 1;
        while (g->pSourceHeader != menuSystem().pActiveMenu)
            g = g + 1;
        f.viewTag = g->viewPosTag;
    }
    if ((memoryCardDontCare == 1 && G(haveGotLanguageRecently) == 1) || G(flyTheBleedinCameraG) != 0)
    {
        sPathDetails &path = G(pathDetailsG);
        path.finalDestTag = 0x100000e6u;
        path.pActivePath = ptr32<sCameraPath>(guest_addr(&path.constructedPath));
        retVal = 1;
        guest_call<void>(sym::CameraSetNewPath, sym::cameraVarG, sym::PathPilotEndCallback, sym::PathPilotNodeCallback, path.pActivePath);
        G(haveGotLanguageRecently) = 0;
        G(flyTheBleedinCameraG) = 0;
    }
    if (retVal == 0 && f.viewTag != 0 && static_cast<int32_t>(m.activeLanguage) != -1 && at<int32_t>(pCamera + 0xd8u) == -1)
    {
        CameraDestination(f.viewTag, 0xffffffffu);
        sPathDetails &path = G(pathDetailsG);
        if (path.finalDestTag != path.currentTag)
        {
            path.pActivePath = ptr32<sCameraPath>(guest_addr(&path.constructedPath));
            guest_call<void>(sym::PathPilotEndCallback, 0);
        }
    }
    return retVal;
}

// The direction menu (normal, reversed) of a track: the modes not unlocked are hidden. With only one left it is
// chosen at once (mirrored for a hidden normal mode, else straight to the confirmation). Returns 0 if the menu was
// left that way.
int32_t HideEmptyMenu(ptr32<sAMenuHeader> pHeader)
{
    GuestFrame frame(0x50);
    if ((pHeader->evenMoreFlags & 4u) == 0)
        return 1;
    uint32_t t0 = incoming(8);
    const int32_t level = worldTrackToLevel(t0);
    const uint8 *pAvailable = &mainVars().rewards.lvlAvailability[(level - 1) % 24];
    int32_t hidden = 0;
    const ptr32<sMenuItemStd> pFirst = AMenuGetMIT(pHeader, 0);
    pFirst->flags = static_cast<uint16>(pFirst->flags & 0xff9fu);
    if ((*pAvailable & 4u) == 0)
    {
        pFirst->flags = static_cast<uint16>(pFirst->flags | 0x60u);
        hidden = 1;
    }
    if (const ptr32<sMenuItemStd> pSecond = AMenuGetMIT(pHeader, 1))
    {
        pSecond->flags = static_cast<uint16>(pSecond->flags & 0xff9fu);
        if ((*pAvailable & 2u) == 0)
        {
            pSecond->flags = static_cast<uint16>(pSecond->flags | 0x60u);
            ++hidden;
        }
    }
    sMenuSystem &ms = menuSystem();
    if (hidden == 1)
    {
        if (const ptr32<sAMenuHeader> pSpecial = pHeader->pSpecialMenu)
        {
            pSpecial->pLinkMenuReturn = pHeader->pLinkMenuReturn;
            ms.pLastActiveMenu = pHeader;
            ms.pActiveMenu = pSpecial;
            pSpecial->selection = 0;
            mainVars().mirrorModeOnOff = 1;
            mainVars().reverseModeOnOff = 0;
        }
        return 0;
    }
    if (hidden != 2)
        return 1;
    pHeader->pGlobalDestMenu->pLinkMenuReturn = pHeader->pLinkMenuReturn;
    ms.pLastActiveMenu = pHeader;
    ms.pActiveMenu = pHeader->pGlobalDestMenu;
    ms.pActiveMenu->selection = 0;
    mainVars().mirrorModeOnOff = 0;
    mainVars().reverseModeOnOff = 0;
    buildConfirmText(t0);
    return 0;
}

// The confirmation text of the active menu: the cup and car (arcade), or the track, direction and car(s), written
// into the front end's string buffer, which becomes the menu's dialog text in every language.
void BuildConfirmText()
{
    buildConfirmText(incoming(8));
}

void front::buildConfirmText(uint32 t0)
{
    GuestFrame frame(0xc0);
    sFrontEndClass &f = fe();
    mainClass &m = mainVars();
    const ptr32<char> str = f.pFrontEndStringsOLD;
    const ptr32<sAMenuHeader> pHeader = activeMenu();
    *str = 0;
    const uint32_t raceType = m.raceType;
    const auto sprintf = [str](ptr32<char> format, auto... args) { guest_call<int32_t>(sym::sprintf, str, format, args...); };
    if (raceType == 0x100001fau)
    {
        ptr32<sMenuItemStd> pCupItem = f.pCupSelectMenu->pMenuStart;
        for (int32_t i = 0; i < f.cup - 1; ++i)
            pCupItem = pCupItem->pNext;
        const ptr32<sTextGroup> pCup = pCupItem->pTextGroup;
        const ptr32<char> pCupName(at<uint32_t>(pCup.address + static_cast<uint32_t>(m.workingLanguage) * 4u));
        uint32_t a1 = pCup.address + static_cast<uint32_t>(m.workingLanguage) * 4u;
        const eObjectType type = tagToCar(f.carTag[0], a1);
        const ptr32<char> pCar = AMenuGetText(carItem(type, a1)->pTextGroup);
        sprintf(GetGeneralString(0x16), pCupName, pCar);
    }
    else if (raceType == 0x100001fcu || raceType == 0x100001feu || raceType == 0x10000200u)
    {
        const ptr32<sExtraLevelData> pExtra = feLevel()->pExtraLevelData;
        m.level = static_cast<uint8>(worldTrackToLevel(t0));
        const ptr32<char> pTrack = AMenuGetText(pExtra[(m.level - 1) % 24].pTextGroup);
        const ptr32<char> pReverseText = GetGeneralString(0x17);
        const ptr32<char> pReverse = m.reverseModeOnOff == 1 ? pReverseText : ptr32<char>(sym::nullStringG);
        const ptr32<char> pMirrorText = GetGeneralString(0x18);
        const ptr32<char> pMirror = m.mirrorModeOnOff == 1 ? pMirrorText : ptr32<char>(sym::nullStringG);
        uint32_t a1 = G(pCarSelectMenu).address;
        const eObjectType type1 = tagToCar(f.carTag[0], a1);
        const ptr32<char> pCar1 = AMenuGetText(carItem(type1, a1)->pTextGroup);
        const bool plain = std::strlen(pReverse.get()) + std::strlen(pMirror.get()) == 0;
        if (raceType != 0x10000200u)
        {
            if (plain)
                sprintf(GetGeneralString(0x19), pTrack, pCar1);
            else
                sprintf(GetGeneralString(0x1a), pTrack, pReverse, pMirror, pCar1);
        }
        else
        {
            a1 = G(pCarSelectMenu).address;
            const eObjectType type2 = tagToCar(f.carTag[1], a1);
            const ptr32<char> pCar2 = AMenuGetText(carItem(type2, a1)->pTextGroup);
            if (plain)
                sprintf(GetGeneralString(0x1b), pTrack, pCar1, pCar2);
            else
                sprintf(GetGeneralString(0x1c), pTrack, pReverse, pMirror, pCar1, pCar2);
        }
    }
    sTextGroup &confirm = G(confirmTextGroup);
    for (int32_t i = 3; i >= 0; --i)
        confirm.pText[i] = str;
    pHeader->pDialogTxt = ptr32<sTextGroup>(sym::confirmTextGroup);
    pHeader->dlgLines = -1;
    pHeader->flags = static_cast<uint16>(pHeader->flags | 0x8000u);
}

// A stats bar: a black frame (width wide) and the value in a red-to-cyan bar, from y + 6 to y + height. Returns the
// next line's y.
int32_t DisplayStatsBar(int32_t x, int32_t y, int32_t width, int32_t height, int32_t value)
{
    const amenu::Locals f(0x90);
    const int32_t top = y + 6, bottom = y + height;
    f.sh(0x6, top);
    f.sh(0xa, top);
    f.sh(0xe, bottom);
    f.sh(0x12, bottom);
    f.sh(0x4, x);
    f.sh(0x10, x);
    f.sh(0xc, x + width);
    f.sw(0x0, 0);
    for (const uint32_t c : {0x14u, 0x15u, 0x16u, 0x20u, 0x21u, 0x22u, 0x18u, 0x19u, 0x1au, 0x1cu, 0x1du, 0x1eu})
        f.sb(c, 0);
    f.sh(0x8, x + width);
    RectRenderGouraud(f.ptr<_sGRect>(0), amenu::constant(sym::lit4_327614));
    f.sh(0xa, top);
    f.sh(0xc, x + value);
    f.sh(0x10, x);
    f.sb(0x1e, 0xff);
    f.sh(0x6, top);
    f.sh(0xe, bottom);
    f.sh(0x12, bottom);
    f.sh(0x4, x);
    f.sh(0x8, x + value);
    for (const uint32_t c : {0x14u, 0x20u, 0x19u, 0x1au, 0x1du})
        f.sb(c, 0xff);
    f.sw(0x0, 0);
    for (const uint32_t c : {0x15u, 0x16u, 0x21u, 0x22u, 0x18u, 0x1cu})
        f.sb(c, 0);
    RectRenderGouraud(f.ptr<_sGRect>(0), amenu::constant(sym::lit4_327618));
    return bottom;
}

// The stats box under the car menu while the camera is at the car-selection view: the highlighted car's speed,
// acceleration and weight, or the stats menu alone for a car still locked.
void DisplayVehicleStats(ptr32<sAMenuHeader> pHeader)
{
    std::optional<GuestFrame> frame(std::in_place, 0x90u);
    sFrontEndClass &f = fe();
    sMenuSystem &ms = menuSystem();
    if ((pHeader->moreFlags & 1u) == 0 || f.cupsWonMessage != 0)
        return;
    const uint32_t pCamera = at<uint32_t>(sym::cameraVarG + 0xb70u);
    if (at<int32_t>(pCamera + 0xd8u) != -1 || ms.specialStatsHoldoff != 0)
        return;
    const int32_t camX = at<int32_t>(pCamera + 0xcu), camZ = at<int32_t>(pCamera + 0x14u);
    const int32_t selX = G(camCarSel3xG), selZ = G(camCarSel3zG);
    if (!(selX - 0x32 < camX && camX < selX + 0x32 && selZ - 0x32 < camZ && camZ < selZ + 0x32))
    {
        ms.specialStatsHoldoff = 0x14;
        return;
    }
    if (pHeader->y == 100)
        return;
    const ptr32<sAMenuHeader> pStats = G(pStatsMenuG);
    pStats->y = static_cast<int16>(static_cast<uint16_t>(pHeader->y) + pHeader->height + 0x14);
    const int32_t entry = pHeader->selection + ms.scrollSelector;
    uint32_t a1 = static_cast<uint32_t>(entry);
    const eObjectType type = tagToCar(AMenuGetMIT(pHeader, entry)->selectionTag, a1);
    GetCarouselPointer(type);
    const ptr32<sCarBoatStats> pCarStats = feLevel()->pCarStats;
    if (CheckCarAvailability(type) != 1)
    {
        pStats->width = 0xa0;
        menuSystem().screenAdjust = 0;
        frame.reset(); // the original leaves its frame before this tail jump
        AMenuDisplayAll(G(pStatsMenuG));
        return;
    }
    const ptr32<font> pFont = G(fontVarG).pFont;
    const int32_t chHeight = (pFont->flags & 2u) != 0 ? pFont->chHeight - 2 : pFont->chHeight;
    const int32_t line = chHeight / 2;
    // A box of the front end's own, a copy of the car menu's header made to fit the stats.
    sAMenuHeader &box = f.scrollPTTheader;
    std::memmove(&box, pHeader.get(), sizeof(sAMenuHeader));
    box.x = 0;
    box.flags = 0x824;
    box.width = 0xa0;
    box.y = pStats->y;
    box.pTitleText = pHeader->pCharList;
    box.pDialogTxt = nullptr;
    box.pAdditionalText = nullptr;
    box.pSpareText = nullptr;
    box.height = pStats->height;
    box.pLinkMenuReturn = nullptr;
    box.pOptionsList = pHeader->pAdditionalText;
    box.pMenuStart = nullptr;
    box.pGlobalDestMenu = nullptr;
    box.selectionTag = 0;
    box.pCharList = nullptr;
    box.pUIgroup = nullptr;
    if (box.height < 0x2c)
        box.height = 0x2c;
    box.changeX = 0;
    box.moreFlags = 4;
    box.changeY = 0;
    box.dlgLines = 0;
    box.scrollItems = 0;
    box.scrollStart = 0;
    box.colSelect = 0;
    box.rowSelect = 0;
    box.recurseFlag = 0;
    box.tofspEnglish = box.tofspFrench = box.tofspGerman = box.tofspSpanish = 0;
    box.tofsnEnglish = box.tofsnFrench = box.tofsnGerman = box.tofsnSpanish = 0;
    const ptr32<sAMenuHeader> pBox(guest_addr(&box));
    guest_call<void>(sym::AMenuSnapsize, pBox);
    guest_call<void>(sym::AMenuDrawBox, pBox, 0);
    int32_t y = box.y;
    FontSetColour(0x32, 0x7f, 0x32);
    const ptr32<char> pTitle = AMenuGetText(box.pTitleText);
    FontPrint(amenu::i16(AMenuStringCentre(pBox, pTitle, -1)), amenu::i16(y), pTitle);
    y += line;
    const ptr32<sCarBoatStat> pStat = pCarStats->pCarBoatStatList[type - 1];
    const uint8 values[3] = {pStat->speed, pStat->accel, pStat->mass};
    static const int32_t kNames[3] = {0x13, 0x15, 0x14};
    for (int32_t i = 0; i < 3; ++i)
    {
        FontPrint(box.x, amenu::i16(y), GetGeneralString(kNames[i]));
        if (i == 2)
            frame.reset(); // the last bar is a tail jump, after the original leaves its frame
        y = DisplayStatsBar(box.width + box.x - 0x32, y, 0x46, line, static_cast<int32_t>(values[i]) * 0x46 / 100);
    }
}

// The stats resource is loaded: its text group list's entries become pointers to the loaded groups.
void StatsConstructDaemon(ptr32<memHandle> pHandle)
{
    GuestFrame frame(0x20);
    const uint32_t list = at<uint32_t>(at<uint32_t>(pHandle.address));
    for (uint32_t p = list; at<uint32_t>(p) != 0; p += 4u)
        at<uint32_t>(p) = guest_call<uint32_t>(sym::ResLoad, at<uint32_t>(p));
}

void StatsInit()
{
    guest_call<void>(sym::ResTypeSetConstructDaemon, sym::haveGotLanguageRecently_561 + 0xcu, sym::StatsConstructDaemon, 2);
}

void StatsTerm()
{
    guest_call<void>(sym::ResTypeUnSetConstructDaemon, sym::haveGotLanguageRecently_561 + 0xcu);
}

// The menu system's selection callback: 0 if the item can be selected. A car is refused (1) while locked on the
// carousel.
int32_t SelectionTest()
{
    GuestFrame frame(0x30);
    sMenuSystem &ms = menuSystem();
    const ptr32<sAMenuHeader> pHeader = ms.pActiveMenu;
    const ptr32<sMenuItemStd> pItem = AMenuGetMIT(pHeader, pHeader->selection + ms.scrollSelector);
    if (!pItem || (pItem->flags & 0x10u) != 0)
        return 0;
    const uint32_t before = ms.executeCode;
    const int32_t selected = AMenuSelectMenuItem(pHeader, pItem);
    uint32_t tag = menuSystem().executeCode;
    menuSystem().executeCode = before;
    if ((selected & 1) == 0 && pHeader->pGlobalDestMenu && before != 0x100001f9u && (selected & 2) == 0)
        tag = pHeader->selectionTag;
    // The car tags (those of TagToCar but 0x10000079/7a).
    const int32_t t = static_cast<int32_t>(tag);
    const auto in = [t](uint32_t low, uint32_t high) { return t >= static_cast<int32_t>(low) && t <= static_cast<int32_t>(high); };
    const bool car = in(0x10000038u, 0x1000003cu) || in(0x1000003fu, 0x10000040u) || in(0x10000042u, 0x10000043u) ||
                     in(0x10000045u, 0x10000046u) || t == 0x10000048 || in(0x1000004du, 0x1000004eu) ||
                     in(0x10000051u, 0x10000052u) || in(0x10000058u, 0x10000059u) || in(0x1000005bu, 0x1000005cu) ||
                     in(0x1000005fu, 0x10000060u) || in(0x10000062u, 0x10000063u) || in(0x10000068u, 0x10000069u) ||
                     in(0x10000070u, 0x10000073u) || in(0x10000291u, 0x10000298u);
    if (!car)
        return 0;
    uint32_t a1 = static_cast<uint32_t>(selected);
    return carouselEntry(tagToCar(tag, a1))->flag != 1 ? 1 : 0;
}

// The key-definition page of a player's pad configuration: only the lines of the configuration's set are shown.
void BuildKeyDefinitions(ptr32<sAMenuHeader> pHeader)
{
    if (!pHeader || (pHeader->moreFlags & 0x800u) == 0)
        return;
    const ptr32<sMenuItemStd> pFirst = pHeader->pMenuStart;
    pHeader->width = 0x1c2;
#if !RC_VERSION_EU
    pHeader->y = 0x13;
#endif
    ptr32<sMenuItemStd> p = pFirst->pNext;
    p->flags = static_cast<uint16>(p->flags | 0x40u);
    for (p = p->pNext; p; p = p->pNext)
        p->flags = static_cast<uint16>(p->flags | 0x60u);
    const uint32_t groupTag = pHeader->pUIgroup->selectionTag;
    // Outside a pad configuration group the configuration is a register the caller left: $a2 (USA), $a3 (Europe).
    int32_t config = static_cast<int32_t>(incoming(RC_VERSION_EU ? 7 : 6));
    if (groupTag == 0x10000214u)
        config = fe().gameStateData.playerConfig[0].controllerConfig;
    else if (groupTag == 0x10000215u)
        config = fe().gameStateData.playerConfig[1].controllerConfig;
    uint32_t set = incoming(10);
    if (config == 0)
        set = sym::set1_643;
    else if (config == 1)
        set = sym::set2_644;
    else if (config == 2)
        set = sym::set3_645;
    // Line k (from 1) is shown if it is the set's next entry; line 3 takes two items.
    int32_t next = 0;
    p = pFirst->pNext->pNext;
    for (uint32_t k = 1; p; ++k)
    {
        if (k == at<uint8_t>(set + static_cast<uint32_t>(next)))
        {
            p->flags = static_cast<uint16>(p->flags & 0xffdfu);
            if (k == 3)
            {
                p = p->pNext;
                p->flags = static_cast<uint16>(p->flags & 0xffdfu);
            }
            ++next;
        }
        else if (k == 3)
            p = p->pNext;
        p = p->pNext;
    }
    pHeader->selection = 0;
}

void ShowMessage()
{
    sFrontEndClass &f = fe();
    if (f.cupsWonMessage != 0)
        AMenuDisplayAll(f.pRewardMessages[f.cupsWonMessage - 1]);
}

// A reward or memory card message over the menus, until dismissed. The memory card messages (22 on) are menus of
// their own, whose choice says whether the game goes on without saving.
void ProcessRewardMessage()
{
    GuestFrame frame(0x60);
    sFrontEndClass &f = fe();
    bool dismissed = false;
    if (f.cupsWonMessage >= 0x16)
    {
        PollingMemoryCard();
        if (f.cupsWonMessage == 0)
            return;
        const ptr32<sAMenuHeader> pKeep = menuSystem().pActiveMenu;
        guest_call<void>(sym::AMenuProcessAll, f.pRewardMessages[f.cupsWonMessage - 1]);
        const uint32_t code = menuSystem().executeCode;
        menuSystem().pActiveMenu = pKeep;
        if (code == 0x10000625u || code == 0x10000627u || code == 0x10000629u || code == 0x100006bbu)
        {
            f.cupsWonMessage = 0;
            G(memoryCardDontCareG) = 1;
        }
        guest_call<void>(sym::InputResetAllHit);
        menuSystem().executeCode = 0;
        if (G(memoryCardDontCareG) == 1)
        {
            guest_call<void>(sym::RCMusicPlayLevel, 0x69);
            guest_call<void>(sym::RCMusicSetVol, mainVars().musicVolume);
        }
        if (f.cupsWonMessage == 0)
            return;
    }
    AMenuDisplayAll(f.pRewardMessages[f.cupsWonMessage - 1]);
    guest_call<void>(sym::AMenuDisplayOptions, f.pRewardMessages[f.cupsWonMessage - 1]);
    if (f.cupsWonMessage == 0x0c)
        AMenuDisplayAll(menuSystem().pActiveMenu);
    for (const uint8 key : {uint8(0xe), uint8(0xc)})
        if (InputHit(key) || InputHit(static_cast<uint8>(key + 0x10)))
        {
#if RC_VERSION_EU
            if (f.cupsWonMessage == 0x0e) // the saved game loaded: on to the menus
                G(flyTheBleedinCameraG) = 1;
#endif
            dismissed = true;
            f.cupsWonMessage = 0;
            menuSystem().executeCode = 0;
        }
    menuSystem().executeCode = 0;
    f.uiDemoLoopWait = 0;
    if (dismissed)
        selectSound();
}

// On a car menu: the carousel turns to the highlighted car, or (just entered) to the player's car, which becomes the
// menu's selection.
void HandlePersistentCars()
{
    std::optional<GuestFrame> frame(std::in_place, 0x60u);
    sMenuSystem &ms = menuSystem();
    if ((ms.pActiveMenu->moreFlags & 0x20u) == 0)
        return;
    mainClass &m = mainVars();
    eObjectType type;
    uint32_t tag;
    if (ms.newMenuFlag == 1)
    {
        type = static_cast<eObjectType>((ms.pActiveMenu->evenMoreFlags & 8u) != 0 ? m.p2Car : m.p1Car);
        const uint32_t a2 = incoming(6), a3 = incoming(7);
        tag = carToTag(type, 1, a2, a3);
        const uint32_t tag2 = carToTag(type, 2, a2, a3);
        for (ptr32<sMenuItemStd> p = AMenuGetMIT(menuSystem().pActiveMenu, 0); p; p = p->pNext)
            if (p->selectionTag == tag || p->selectionTag == tag2)
            {
                menuSystem().pActiveMenu->selection = 0;
                menuSystem().scrollSelector = 0;
                break;
            }
    }
    else
    {
        const int32_t entry = ms.pActiveMenu->selection + ms.scrollSelector;
        const ptr32<sMenuItemStd> p = AMenuGetMIT(ms.pActiveMenu, entry);
        uint32_t a1 = static_cast<uint32_t>(entry);
        type = tagToCar(p->selectionTag, a1);
        tag = p->selectionTag;
    }
    if ((menuSystem().pActiveMenu->evenMoreFlags & 8u) != 0)
        m.p2Car = static_cast<uint8>(type);
    else
        m.p1Car = static_cast<uint8>(type);
    frame.reset(); // the original leaves its frame before this tail jump
    if (tag != 0)
        CarouselMoveTo(type);
}

// The game's logo slides in at the top while the main menu (0x10000283) is up, and out again after.
void DisplayRCRevengeBanner()
{
#if RC_VERSION_EU
    constexpr int32_t kBannerLow = 0x24;
#else
    constexpr int32_t kBannerLow = 0x1e;
#endif
    const ptr32<sAMenuHeader> pMenu = menuSystem().pActiveMenu;
    if (!pMenu || G(memoryCardDontCareG) != 1)
        return;
    int32 &x = G(bannerXG);
    int32 &y = G(bannerYG);
    if (pMenu->selectionTag == 0x10000283u)
    {
        x = x < 0xbf ? 0xbe : x - 5;
        y = y < kBannerLow ? y + 3 : kBannerLow;
    }
    else
    {
        x = x < 0xbe ? x + 5 : 0xbe;
        y = y < -0x45 ? -0x46 : y - 3;
    }
    if (!(x < 0xbe || y >= -0x45))
    {
        x = 0xbe;
        y = -0x46;
        return;
    }
    const amenu::Locals f(0x40);
    f.sb(0x12, 0x7f);
    f.sb(0x10, 0x7f);
    f.sb(0x11, 0x7f);
    f.sh(0x16, 2);
    f.sw(0x0, 0x20);
    f.sh(0xe, 0x40);
    f.sh(0x4, static_cast<uint16_t>(x));
    f.sh(0x6, static_cast<uint16_t>(y));
    f.sh(0x14, 2);
    f.sh(0xc, 0x40);
    f.sh(0x8, 0);
    f.sh(0xa, 0);
    const ptr32<_sFELevelData> pLogos = feLevel();
    static const uint32_t kDepths[4] = {sym::lit4_32761c, sym::lit4_327620, sym::lit4_327624, sym::lit4_327628};
    for (int32_t i = 0; i < 4; ++i)
    {
        SpriteRender(pLogos->pLogoGraphic[i], f.ptr<_sSpriteParams>(0), amenu::constant(kDepths[i]));
        f.sh(0x4, f.lhu(0x4) + f.lhu(0xc));
    }
}

// The space check's answer as a message: a card full or none (22-25), or on with the music.
void GameAutoLoadSpaceCheck()
{
    sFrontEndClass &f = fe();
    switch (static_cast<uint32_t>(G(loadSaveAutoStatusG)))
    {
    case 0: f.cupsWonMessage = 0x16; break;
    case 1: f.cupsWonMessage = 0x17; break;
    case 2: f.cupsWonMessage = 0x18; break;
    case 3: f.cupsWonMessage = 0x19; break;
    case 4:
    {
        G(memoryCardDontCareG) = 1;
        f.cupsWonMessage = 0;
        {
            GuestFrame frame(0x10);
            guest_call<void>(sym::RCMusicPlayLevel, 0x69);
        }
        guest_call<void>(sym::RCMusicSetVol, mainVars().musicVolume); // a tail jump, after the frame is left
    }
        break;
    default: break;
    }
}

void PollingMemoryCard()
{
    if (G(memoryCardDontCareG) != 0)
        return;
    {
        GuestFrame frame(0x10);
        G(loadSaveAutoStatusG) = static_cast<eLoadSaveFreeSpace>(guest_call<int32_t>(sym::LoadSaveKeepCheckingFreeSpaceOnCards, G(extraFrameCntG)));
    }
    GameAutoLoadSpaceCheck(); // a tail jump
}

// Hides the items the UI groups tagged 0x10000208 point at; for the pause menu (flag 1) also the two-player items
// outside a two-player race, and in a cup the pause menu is the first group's.
namespace {
// RC_MENU_DUMP=1 or 2 (tool): the front end's menus as the level file builds them, and the first race's pause menu, once each, to stderr.
// RC_MENU_DUMP=2: each text in all four languages, separated by |.
std::string menuText(ptr32<sTextGroup> group)
{
    if (!group || !group->pText[0])
        return "-";
    static const bool all = std::getenv("RC_MENU_DUMP") && std::string(std::getenv("RC_MENU_DUMP")) == "2";
    std::string out;
    for (int i = 0; i < (all ? 4 : 1); ++i)
    {
        if (i)
            out += '|';
        if (group->pText[i])
            out += std::string(static_cast<const char *>(guest_host(group->pText[i].address))).substr(0, 40);
    }
    return out;
}
void dumpMenus(ptr32<sUIgroup> pUIGroup, int32_t flag)
{
    static bool done[2] = {false, false}; // the front end's menus, the pause menu's
    bool &once = done[flag == 1 ? 1 : 0];
    if (once || !std::getenv("RC_MENU_DUMP"))
        return;
    once = true;
    int n = 0;
    for (ptr32<sUIgroup> g = pUIGroup; g->pSourceHeader.address != 0xffffffffu && n < 200; g = g + 1, ++n)
    {
        const ptr32<sAMenuHeader> h = g->pSourceHeader;
        std::fprintf(stderr, "[menu] group %d src %08x dest %08x tag %08x view %08x linkPos %d linkType %u\n", n, h.address,
                     g->pDestHeader.address, g->selectionTag, g->viewPosTag, g->linkPos, g->linkType);
        if (!h)
            continue;
        std::fprintf(stderr, "[menu]   header %08x \"%s\" flags %04x more %04x even %04x items %u xywh %d %d %u %u colour %08x border %08x ui %08x\n",
                     h.address, menuText(h->pTitleText).c_str(), h->flags, h->moreFlags, h->evenMoreFlags, h->menuItems, h->x, h->y, h->width,
                     h->height, h->pColourSet.address, h->pBorderSet.address, h->pUIgroup.address);
        int k = 0;
        for (ptr32<sMenuItemStd> p = h->pMenuStart; p && k < 40; p = p->pNext, ++k)
        {
            std::fprintf(stderr, "[menu]     item %08x \"%s\" tag %08x flags %04x linkType %u status %02x link %08x var %08x", p.address,
                         menuText(p->pTextGroup).c_str(), p->selectionTag, p->flags, p->linkType, p->statusFlags, p->pLinkMenu.address,
                         p->pMenuVar.address);
            if (const ptr32<sMenuItemVar> v = p->pMenuVar)
                std::fprintf(stderr, " [var tag %08x value %d min %d max %d flags %04x feedback %08x choices \"%s\"]", v->tagMVar, v->value,
                             v->minValue, v->maxValue, v->flags, v->pFeedbackVar.address, menuText(v->pTextGroup).c_str());
            std::fprintf(stderr, "\n");
        }
    }
}
} // namespace

void MenuInitScan(ptr32<sUIgroup> pUIGroup, int32_t flag)
{
    dumpMenus(pUIGroup, flag);
    GuestFrame frame(0x70);
    for (ptr32<sUIgroup> g = pUIGroup; g->pSourceHeader.address != 0xffffffffu; g = g + 1)
        if (g->selectionTag == 0x10000208u)
        {
            const ptr32<sMenuItemStd> p = AMenuGetMIT(g->pSourceHeader, g->linkPos);
            p->flags = static_cast<uint16>(p->flags | 0x60u);
        }
    enhance::install(pUIGroup); // enhanced recomp: Game Options > Enhancements, and Debug Menus shown
    const int32_t cup = fe().cup;
    if (flag != 1)
        return;
    if (mainVars().raceType != 0x10000200u)
        for (ptr32<sUIgroup> g = pUIGroup + 1; g->pSourceHeader.address != 0xffffffffu; g = g + 1)
            for (ptr32<sMenuItemStd> p = g->pSourceHeader->pMenuStart; p; p = p->pNext)
                if (p->selectionTag == 0x10000078u)
                    p->flags = static_cast<uint16>(p->flags | 0x60u);
    if (cup != 0)
        menuSystem().pPauseMenu = ptr32<sAMenuHeader>(pUIGroup->viewPosTag);
}

// Each frame: the two-player items need the second pad, the Track Editor's world a track.
void MenuItemDisabler(ptr32<sAMenuHeader> pHeader)
{
    GuestFrame frame(0xa0);
    if ((pHeader->evenMoreFlags & 0x40u) != 0)
        return;
    int32_t index = 0;
    for (ptr32<sMenuItemStd> p = pHeader->pMenuStart; p; p = p->pNext, ++index)
    {
        const uint32_t tag = p->selectionTag;
        if (tag == 0x10000200u || tag == 0x1000022fu || tag == 0x1000061au || tag == 0x10000078u)
        {
            if (InputPadPresent(static_cast<eInputPad>(5)))
                p->statusFlags = static_cast<uint8>(p->statusFlags & 0xfeu);
            else
            {
                p->statusFlags = static_cast<uint8>(p->statusFlags | 1u);
                if (pHeader->selection == index)
                    pHeader->selection = 0;
            }
        }
        else if (tag == 0x1000006bu)
            p->statusFlags = static_cast<uint8>((at<uint32_t>(kTEditFlags) & 4u) == 0 ? p->statusFlags | 8u : p->statusFlags & 0xf7u);
    }
}

// The car-selection menu: the last UI group viewed from tag 0x1000020a. Returns how many there are.
int32_t FrontEndMenuScan()
{
    int32_t count = 0;
    ptr32<sUIgroup> g = feLevel()->pUIGroup;
    if (g->pSourceHeader.address == 0xffffffffu)
        return 0;
    ptr32<sAMenuHeader> found = G(pCarSelectMenu);
    for (; g->pSourceHeader.address != 0xffffffffu; g = g + 1)
        if (g->viewPosTag == 0x1000020au)
        {
            found = g->pSourceHeader;
            ++count;
        }
    G(pCarSelectMenu) = found;
    return count;
}

// A saved game was loaded: its cups and cars, and the "game loaded" message.
void GameAutoLoadSetup()
{
    GuestFrame frame(0x10);
    if (G(gameLoadedG) == 0)
        return;
    SetCupAvailability(0);
    CarouselSetAvailability();
    fe().cupsWonMessage = 0x0e;
}

void ChopOutMenuItem(ptr32<sAMenuHeader> pHeader, int32_t item)
{
    GuestFrame frame(0x10);
    const ptr32<sMenuItemStd> p = AMenuGetMIT(pHeader, item);
    p->flags = static_cast<uint16>(p->flags | 0x60u);
}

ptr32<char> BuildConfirmTextCar(int32_t car)
{
    GuestFrame frame(0x30);
    ptr32<sMenuItemStd> p = G(pCarSelectMenu)->pMenuStart;
    uint32_t a1 = incoming(5);
    const eObjectType type = tagToCar(fe().carTag[car], a1);
    while (p && tagToCar(p->selectionTag, a1) != type)
        p = p->pNext;
    return AMenuGetText(p->pTextGroup);
}

ptr32<char> BuildConfirmTextWorldTrack()
{
    GuestFrame frame(0x40);
    const ptr32<sExtraLevelData> pExtra = feLevel()->pExtraLevelData;
    uint32_t t0 = incoming(8);
    mainVars().level = static_cast<uint8>(worldTrackToLevel(t0));
    return AMenuGetText(pExtra[(mainVars().level - 1) % 24].pTextGroup);
}

// The chosen cup's name in the working language.
ptr32<char> BuildConfirmTextCup()
{
    sFrontEndClass &f = fe();
    ptr32<sMenuItemStd> p = f.pCupSelectMenu->pMenuStart;
    for (int32_t i = 0; i < f.cup - 1; ++i)
        p = p->pNext;
    return ptr32<char>(at<uint32_t>(p->pTextGroup.address + static_cast<uint32_t>(mainVars().workingLanguage) * 4u));
}

ptr32<char> BuildConfirmTextRev(ptr32<char> pReverse)
{
    return mainVars().reverseModeOnOff == 1 ? pReverse : ptr32<char>(sym::nullStringG);
}

ptr32<char> BuildConfirmTextMirror(ptr32<char> pMirror)
{
    return mainVars().mirrorModeOnOff == 1 ? pMirror : ptr32<char>(sym::nullStringG);
}

} // namespace rc

RC_DECOMP_FN_SETUP(ParseSelectionTags, rc::fuzz::frontParse)
RC_DECOMP_FN_SETUP(HideEmptyMenu, rc::fuzz::frontDirectionMenu)
RC_DECOMP_FN_SETUP(BuildConfirmText, rc::fuzz::frontConfirm)
RC_DECOMP_FN_SETUP_HEAP(DisplayStatsBar, rc::fuzz::frontStatsBar)
RC_DECOMP_FN_SETUP_HEAP(DisplayVehicleStats, rc::fuzz::frontVehicleStats)
RC_DECOMP_FN_SETUP(StatsConstructDaemon, rc::fuzz::frontStatsDaemon)
RC_DECOMP_FN_SETUP(StatsInit, rc::fuzz::frontStubbed)
RC_DECOMP_FN_SETUP(StatsTerm, rc::fuzz::frontStubbed)
RC_DECOMP_FN_SETUP(SelectionTest, rc::fuzz::frontSelectionTest)
RC_DECOMP_FN_SETUP(BuildKeyDefinitions, rc::fuzz::frontKeyDefinitions)
RC_DECOMP_FN_SETUP_HEAP(ShowMessage, rc::fuzz::frontRewardMessage)
RC_DECOMP_FN_SETUP_HEAP(ProcessRewardMessage, rc::fuzz::frontRewardMessage)
RC_DECOMP_FN_SETUP(HandlePersistentCars, rc::fuzz::frontCarMenu)
RC_DECOMP_FN_SETUP_HEAP(DisplayRCRevengeBanner, rc::fuzz::frontBanner)
RC_DECOMP_FN_SETUP(GameAutoLoadSpaceCheck, rc::fuzz::frontMemoryCard)
RC_DECOMP_FN_SETUP(PollingMemoryCard, rc::fuzz::frontMemoryCard)
RC_DECOMP_FN_SETUP(MenuInitScan, rc::fuzz::frontMenuScan)
RC_DECOMP_FN_SETUP(MenuItemDisabler, rc::fuzz::frontItemDisabler)
RC_DECOMP_FN_SETUP(FrontEndMenuScan, rc::fuzz::frontMenuScan)
RC_DECOMP_FN_SETUP(GameAutoLoadSetup, rc::fuzz::frontMemoryCard)
RC_DECOMP_FN_SETUP(ChopOutMenuItem, rc::fuzz::frontChopItem)
RC_DECOMP_FN_SETUP(BuildConfirmTextCar, rc::fuzz::frontConfirm)
RC_DECOMP_FN_SETUP(BuildConfirmTextWorldTrack, rc::fuzz::frontConfirm)
RC_DECOMP_FN_SETUP(BuildConfirmTextCup, rc::fuzz::frontConfirm)
RC_DECOMP_FN_SETUP(BuildConfirmTextRev, rc::fuzz::frontConfirm)
RC_DECOMP_FN_SETUP(BuildConfirmTextMirror, rc::fuzz::frontConfirm)
