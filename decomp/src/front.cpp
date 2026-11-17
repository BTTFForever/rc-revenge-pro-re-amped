// Decompiled from FRONT.c: the front end's setup, frame loop and teardown, and the tags its menus and camera paths
// are linked by. The front end is a 3D world (level 98) with the car carousel in it; each menu choice flies the camera
// to the place for the next menu (CameraDestination, RemapDestTags), and ParseSelectionTags acts on what was chosen.
//
// Written from the instructions (Ghidra's C as a first reading). Several functions return a register the caller left
// set for tags they do not know (TagToCar, CarToTag, TagToCup, WorldTrackToLevel): read from the running context.
//
// Europe (SLES-50077): FrontSetupClass, PathPilotEndCallbackHOP and FEStartPos leave the menu alone until a language
// has been chosen (activeLanguage -1: the language menu comes first), and ScreenAdjust's box is 16 lines taller (PAL).
// FrontEndInit, FrontEndProcess and CameraDestination differ only in register use and store order: one body.

#include "front.h"
#include "front_fuzz.h"

#include <optional>

RC_VERSION_VARIANT(FrontSetupClass)
RC_VERSION_VARIANT(FrontEndInit)
RC_VERSION_VARIANT(FrontEndProcess)
RC_VERSION_VARIANT(CameraDestination)
RC_VERSION_VARIANT(ScreenAdjust)
RC_VERSION_VARIANT(PathPilotEndCallbackHOP)
RC_VERSION_VARIANT(FEStartPos)

namespace rc {

using namespace front;

namespace {

constexpr int32_t kNoLanguage = -1;

// Makes the first menu (the UI group viewed from tag 0x100001f1) the active one, with no menu to return to.
void activateFirstMenu()
{
    const ptr32<sAMenuHeader> pHeader = groupWithTag(0xcu, kTagFirstMenu)->pSourceHeader;
    menuSystem().pActiveMenu = pHeader;
    pHeader->pLinkMenuReturn = nullptr;
}

} // namespace

// The front end's level data is loaded: the menu system gets its first menu, borders, colours and font.
void FrontSetupClass()
{
    std::optional<GuestFrame> frame(std::in_place, 0x20u);
    const ptr32<_sFELevelData> pFELevelData = feLevel();
#if !RC_VERSION_EU
    mainVars().activeLanguage = static_cast<eLanguages>(0);
#endif
    sMenuSystem &ms = menuSystem();
    ms.pActiveMenu = pFELevelData->pUIGroup->pSourceHeader;
    ms.pLastBorderSet = pFELevelData->pBorderSet;
    ms.pLastColourSet = pFELevelData->pColourSet;
    ms.pLastActiveMenu = nullptr;
#if RC_VERSION_EU
    if (static_cast<int32_t>(mainVars().activeLanguage) != kNoLanguage) // else the language menu (the first group's)
#endif
        activateFirstMenu();
    guest_call<void>(sym::LoadSaveSetupClass, pFELevelData->pLoadSaveData);
    frame.reset(); // the original leaves its frame before this tail jump
    guest_call<void>(sym::FontSet, pFELevelData->pGamePropFont);
}

// Loads the front-end level and sets up everything it runs: the camera on its path (to the podium after a race), the
// menus, the reward messages, the stats and missing-pad menus, the carousel.
void FrontEndInit()
{
    sFrontEndClass &f = fe();
    mainClass &m = mainVars();
    std::optional<GuestFrame> frame(std::in_place, 0x90u); // the callees' frames sit where the original's do
    G(carouselAvailableG) = 0;
    f.uiDemoLoopWait = 0;
    f.pFrontEndStringsOLD = nullptr;
    f.pPlayTEitem = nullptr;
    guest_call<void>(sym::PathInit);
    StatsInit();
    guest_call<void>(sym::ResTypeSetConstructDaemon, sym::waterLevelsG + 0x8u, 0, 6);
    guest_call<void>(sym::ResTypeSetConstructDaemon, sym::waterLevelsG + 0x10u, 0, 4);
    guest_call<void>(sym::TaskInit);
    guest_call<void>(sym::HudInit);
    guest_call<void>(sym::LevelInit);
    guest_call<int32_t>(sym::printf, sym::sec_rodata + 0x4b0u); // "Doing Skinned Anim Init...\n"
    guest_call<void>(sym::SkinAnimInit);
    guest_call<void>(sym::ObjectInit);
    guest_call<void>(sym::TSOInit);
    guest_call<void>(sym::AIInit, 0);
    guest_call<void>(sym::WheelInit);
    guest_call<void>(sym::VfxInit);
    guest_call<void>(sym::TBoxInit);
    guest_call<int32_t>(sym::printf, sym::sec_rodata + 0x4d0u); // "Doing Task level init...\n"
    guest_call<void>(sym::GameTaskLvlInit);
    guest_call<int32_t>(sym::printf, sym::sec_rodata + 0x4f0u); // "Doing Physics init...\n"
    guest_call<void>(sym::PhysicsLevelInit);
    guest_call<void>(sym::RevSfxInitAmbient, 0);
    guest_call<void>(sym::LevelLoad, 0x62);
    guest_call<void>(sym::Rdr_ShapeInit);
    guest_call<void>(sym::Rdr_SkinInit);
    TimeTrialExitHandler();
    guest_call<void>(sym::DepthCueInit);
    guest_call<void>(sym::AColLvlInit);
    guest_call<void>(sym::OColLvlInit);
    guest_call<void>(sym::TColLvlInit);
    guest_call<void>(sym::AnimReinit);
    guest_call<void>(sym::CameraLvlInit, 0);
    guest_call<void>(sym::RandomSetSeed, 0);
    const uint32_t timer = guest_addr(&m.gameTimer);
    guest_call<void>(sym::TimerSetup, timer, 0, 0);
    m.pauseMenu = 0;
    guest_call<void>(sym::DepthCueSet, static_cast<uint32_t>(at<uint16_t>(sym::visVarG + 0x10u)), 0);
    menuSystem().pGamePropFont = feLevel()->pGamePropFont;
    FECarouselInit(guest_call<int32_t>(sym::TimerGet, timer));
    G(carouselAvailableG) = 1;
    f.handyNavTag = 0;
    guest_call<void>(sym::CameraSetMode, sym::cameraVarG, 1);
    sPathDetails &path = G(pathDetailsG);
    const uint32_t constructedPath = guest_addr(&path.constructedPath);
    if (static_cast<uint32_t>(m.status) == 0x40u) // back from a race: the podium
    {
        G(dontDrawCarouselCarsFixG) = 1;
        f.active = 3;
        guest_call<void>(sym::Vfx2PodiumInitialise);
        f.raceResult = 1;
        at<int32_t>(sym::visFxVarG + 0x2730u) = 0x1000; // iResultTrigger
        f.cameraPilotMode = 3;
        f.frontEndStatus = static_cast<eFront>(3);
        guest_call<void>(sym::PathCopy, feLevel()->pPaths[1], 3);
        path.pActivePath = ptr32<sCameraPath>(constructedPath);
        path.finalDestTag = 0x100000b7u;
        guest_call<void>(sym::SetCameraToThisTag, 0x100000bbu, feLevel()->pCameras);
        if (static_cast<int32_t>(m.activeLanguage) == kNoLanguage)
        {
            m.activeLanguage = static_cast<eLanguages>(0);
            m.workingLanguage = static_cast<eLanguages>(0);
        }
    }
    else
    {
        f.raceResult = 0;
        f.active = 2;
        f.frontEndStatus = static_cast<eFront>(2);
        f.cameraPilotMode = 3;
        guest_call<void>(sym::PathCopy, feLevel()->pPaths[0], 3);
        path.pActivePath = nullptr;
        if (static_cast<int32_t>(m.activeLanguage) == kNoLanguage)
            guest_call<void>(sym::SetCameraToThisTag, 0x100000efu, feLevel()->pCameras);
        else
        {
            path.pActivePath = ptr32<sCameraPath>(constructedPath);
            guest_call<void>(sym::SetCameraToThisTag, 0x100000e6u, feLevel()->pCameras);
        }
    }
    m.level = 0x62;
    if (path.pActivePath)
        guest_call<void>(sym::CameraSetNewPath, sym::cameraVarG, sym::PathPilotEndCallback, sym::PathPilotNodeCallback, path.pActivePath);
    const uint32_t task = guest_addr(&f.task);
    guest_call<void>(sym::TaskSet, task, sym::FrontEndTaskProcess, 0, 0);
    guest_call<void>(sym::TaskCachedInsert, task);
    SetCupAvailability(0);
    FrontEndMenuScan();
    MenuInitScan(feLevel()->pUIGroup, 0);
    f.loadSaveFlag = 0;
    guest_call<void>(sym::CameraPitchFixup, feLevel()->pCameras);
    menuSystem().EnabledSelectionCallBack = ee_fnptr(sym::SelectionTest);
    m.noHuman = 0;
    guest_call<void>(sym::MemBlockAllocLo, guest_addr(&f.pFrontEndStringsOLD), 1000);
    // The reward messages are the groups tagged 0x10000216; the group list ends with a header of -1, whose tag word
    // holds the slider's sprite.
    sMenuSystem &ms = menuSystem();
    ptr32<sUIgroup> g = feLevel()->pUIGroup;
    for (int32_t count = 0; g->pSourceHeader.address != 0xffffffffu; g = g + 1)
        if (g->selectionTag == 0x10000216u)
            f.pRewardMessages[count++] = g->pSourceHeader;
    f.flyingLogo = 0;
    ms.pSliderSprite = ptr32<_sSprite>(g->selectionTag);
    f.flagPTT = 0;
    guest_call<void>(sym::InputResetAllHit);
    // The stats and missing-pad menus, until all three are found.
    ptr32<sAMenuHeader> stats = G(pStatsMenuG), pad1 = G(pP1ContMissingG), pad2 = G(pP2ContMissingG);
    int32_t found = 0;
    for (g = feLevel()->pUIGroup; found != 3 && g->pSourceHeader.address != 0xffffffffu; g = g + 1)
    {
        const uint32_t tag = g->selectionTag;
        if (tag == 0x10000213u)
        {
            stats = g->pSourceHeader;
            ++found;
        }
        if (tag == 0x1000021du)
        {
            pad1 = g->pSourceHeader;
            ++found;
        }
        if (tag == 0x1000021eu)
        {
            pad2 = g->pSourceHeader;
            ++found;
        }
    }
    G(pP2ContMissingG) = pad2;
    G(pP1ContMissingG) = pad1;
    G(pStatsMenuG) = stats;
    G(bannerXG) = 0xbe;
    G(bannerYG) = -0x46;
    const ptr32<_sSingleCamera> pCamera = guest_call<ptr32<_sSingleCamera>>(sym::GetCameraFromTag, 0x100000b4u, feLevel()->pCameras);
    G(camCarSel3zG) = pCamera->z;
    G(camCarSel3xG) = pCamera->x;
    frame.reset(); // the original leaves its frame before this tail jump
    CarouselSetAvailability();
}

// The front end: one frame per pass, until a choice sets mainVarG.status (a race, the Track Editor, a demo), then the
// music fades out.
void FrontEndProcess()
{
    mainClass &m = mainVars();
    sFrontEndClass &f = fe();
    GuestFrame frame(0xc0);
    const uint32_t timer = guest_addr(&m.gameTimer);
    uint32_t frameCnt = 0;
    m.status = static_cast<eMain>(0);
    m.frameCount = 0;
    m.lastFrameTime = guest_call<int32_t>(sym::TimerGet, timer);
    guest_call<void>(sym::LoadingCallbackTerm);
    bool doCredits = false;
    do
    {
        ++frameCnt;
        guest_call<void>(sym::Rdr_StartFrame);
        bool menuDone = false;
        guest_call<void>(sym::RdrState_ApplyDefaultStateBlock);
        guest_call<void>(sym::InputSnapshot);
        G(extraFrameCntG) = frameCnt;
        if (guest_call<int32_t>(sym::InputTestAnyKeyHeld) != 0)
            f.uiDemoLoopWait = 0;
        const tTime time = guest_call<int32_t>(sym::TimerGet, timer);
        guest_call<void>(sym::CameraProcess, time, 0);
        m.time = time;
        guest_call<void>(sym::TaskProcess, time);
        guest_call<void>(sym::AnimUpdateAnims);
        guest_call<void>(sym::GfxTex_Update);
        if (const ptr32<sAMenuHeader> pMenu = menuSystem().pActiveMenu; pMenu && (pMenu->evenMoreFlags & 2u) != 0)
        {
            CheckCheatKeys(ptr32<sCheatKeyData>(sym::cheatData1G), time);
            CheckCheatKeys(ptr32<sCheatKeyData>(sym::cheatData2G), time);
            CheckCheatKeys(ptr32<sCheatKeyData>(sym::cheatData3G), time);
        }
        if (G(carouselAvailableG) == 1)
            guest_call<void>(sym::ObjectSfxFrontEnd, carousel().segment);
        guest_call<void>(sym::RevSfxUpdateAmbient, 0);
        guest_call<void>(sym::SfxDoSfx);
        guest_call<void>(sym::RCMusicFadeControl);
        at<uint32_t>(sym::cameraVarG + 0xb70u) = sym::cameraVarG; // pCamera = camera
        if (guest_call<int32_t>(sym::Init3dFrame, sym::mainVarG + 0x20u, sym::cameraVarG + 0x24u, m.time, 0) == 0)
            return;
        guest_call<void>(sym::MeshInitFrame);
        guest_call<void>(sym::LsFrameInit, m.time, 0, 0);
        const ptr32<int32_t> temp = frame.local<int32_t>(0);
        temp[0] = 0;
        temp[1] = 0;
        temp[2] = 0;
        m.iWhichSplitScreen = 0;
        guest_call<void>(sym::CellRenderInit, temp);
        guest_call<void>(sym::Init3dChangeViewcone, static_cast<uint32_t>(at<uint16_t>(at<uint32_t>(sym::cameraVarG + 0xb70u) + 0x574u)));
        guest_call<void>(sym::LsRenderWorld);
        guest_call<void>(sym::TSORender, m.time);
        if (static_cast<uint32_t>(f.active - 3) < 2u) // the podium
        {
            if (G(dontDrawCarouselCarsFixG) == 1)
            {
                guest_call<void>(sym::VfxRenderShadows);
                guest_call<void>(sym::ObjectRenderAll);
            }
        }
        else
        {
            CarouselRender(m.time);
            guest_call<void>(sym::VfxRenderShadows);
            CarouselDraw(m.time);
        }
        guest_call<void>(sym::RdrAlphaAddToDmaChain);
        BuildLockedTracksList();
        guest_call<void>(sym::HudDrawLockedTracks);
        if (G(firstEntryG) == 0) // the first frame: load the saved game from the memory card
        {
            bool checkSpace = false;
            guest_call<void>(sym::TimerPause, timer);
            G(pauseReasonG) = G(pauseReasonG) | 1;
            guest_call<int32_t>(sym::sceMcInit);
            if (guest_call<int32_t>(sym::LoadSaveCheckIfCardInSlot1) == 0)
            {
                G(firstEntryCountDownG) = 0;
                checkSpace = true;
            }
            else
            {
                guest_call<void>(sym::LoadSaveInit, 4);
                const int32_t loaded = guest_call<int32_t>(sym::LoadSaveAutoLoadSavedGame);
                G(gameLoadedG) = loaded;
                if (loaded == 0)
                    checkSpace = true;
                else
                {
                    guest_call<void>(sym::RCMusicPlayLevel, 0x69);
                    guest_call<void>(sym::RCMusicSetVol, m.musicVolume);
                }
            }
            if (checkSpace)
                G(loadSaveAutoStatusG) = static_cast<eLoadSaveFreeSpace>(guest_call<int32_t>(sym::LoadSaveCheckFreeSpaceOnCards));
            G(pauseReasonG) = G(pauseReasonG) & 0xfffffffeu;
            if (G(pauseReasonG) == 0)
                guest_call<void>(sym::TimerResume, timer);
            G(firstEntryG) = 1;
        }
        G(firstEntryCountDownG) = 0;
        G(firstEntryG) = 1;
        if (static_cast<int32_t>(m.activeLanguage) != kNoLanguage && G(autoLoadDoneG) == 0)
        {
            GameAutoLoadSetup();
            GameAutoLoadSpaceCheck();
            G(autoLoadDoneG) = 1;
        }
        BuildKeyDefinitions(menuSystem().pActiveMenu);
        if (menuSystem().status == 2)
            f.frontEndStatus = static_cast<eFront>(6);
        if (f.loadSaveFlag == 0)
        {
            const ptr32<sAMenuHeader> pMenu = menuSystem().pActiveMenu;
            const int32_t pads = pMenu && (pMenu->evenMoreFlags & 8u) != 0 ? 2 : 1;
            if (guest_call<int32_t>(sym::CheckPads, pads) == 0)
            {
                switch (static_cast<uint32_t>(f.frontEndStatus))
                {
                case 0:
                case 1:
                case 2:
                    DisplayRCRevengeBanner();
                    if (G(dontDrawCarouselCarsFixG) == 1 && at<int32_t>(at<uint32_t>(sym::cameraVarG + 0xb70u) + 0xd8u) != -1)
                        G(dontDrawCarouselCarsFixG) = 0;
                    break;
                case 3:
                    menuDone = true;
                    if (menuSystem().pActiveMenu)
                        guest_call<void>(sym::AMenuDisplayOptions, menuSystem().pActiveMenu);
                    break;
                case 6:
                    menuDone = true;
                    guest_call<void>(sym::AMenuProcessAll, menuSystem().pActiveMenu);
                    ScreenAdjust();
                    break;
                default:
                    break;
                }
                if (!menuDone && f.active == 2)
                {
                    sMenuSystem &ms = menuSystem();
                    const ptr32<sAMenuHeader> pActive = ms.pActiveMenu;
                    ms.pLastActiveMenu = pActive;
                    if (f.flagPTT == 0)
                    {
                        if (f.cupsWonMessage != 0)
                            ProcessRewardMessage();
                        else
                        {
                            if (pActive->selectionTag == 0x10000283u)
                            {
                                if (f.flyingLogo == 0)
                                    f.flyingLogo = 1;
                                guest_call<void>(sym::LoadingShowFlyingLogo);
                            }
                            guest_call<void>(sym::LoadingPlayFlyingLogo);
                            DisplayVehicleStats(menuSystem().pActiveMenu);
                            if (G(firstEntryCountDownG) == 0)
                                guest_call<void>(sym::AMenuProcessAll, menuSystem().pActiveMenu);
                            else
                                ShowMessage();
                            MenuItemDisabler(menuSystem().pActiveMenu);
                        }
                    }
                    else if (f.flagPTT == 2)
                        guest_call<void>(sym::BuildTimeTrials, pActive);
                    else if (f.flagPTT == 1)
                        guest_call<void>(sym::BuildProgress, pActive, guest_addr(&f.gameStateData), -1);
                    else if (f.flagPTT == 3)
                        doCredits = true;
                    ParseSelectionTags();
                    guest_call<void>(sym::MoveUIbox, menuSystem().pActiveMenu);
                }
            }
        }
        else
        {
            ShowMessage();
            f.uiDemoLoopWait = 0;
            if (f.loadSaveFlag == 0x1000ffffu)
            {
                if (guest_call<int32_t>(sym::LoadSaveAutoLoadSavedGame) == 1)
                {
                    SetCupAvailability(0);
                    CarouselSetAvailability();
                    f.cupsWonMessage = 0x0e;
                }
                else
                    f.cupsWonMessage = 0;
                f.loadSaveFlag = 0;
                guest_call<void>(sym::InputResetAllHit);
                guest_call<void>(sym::TimerResume, timer);
            }
            else
            {
                const int32_t result = guest_call<int32_t>(sym::LoadSaveProcess, frameCnt);
                if (result == -1 || result == 1)
                {
                    if (result == -1)
                        amenu::playSfx(0x43, 0x100);
                    else
                    {
                        if (f.loadSaveFlag == 0x10000206u)
                            at<uint32_t>(sym::tEditVarG + 0x250u) = 6; // loadSaveFlags
                        SetCupAvailability(0);
                        CarouselSetAvailability();
                    }
                    f.loadSaveFlag = 0;
                    menuSystem().pActiveMenu = f.pStoredMenu;
                    guest_call<void>(sym::InputResetAllHit);
                    guest_call<void>(sym::TimerResume, timer);
                }
            }
        }
        if (static_cast<uint32_t>(m.time - m.lastFrameTime) >= 1000u)
        {
            m.lastFrameTime = m.time;
            m.framesPerSecond = m.frameCount;
            m.frameCount = 0;
        }
        guest_call<void>(sym::VersionDisplay);
        guest_call<void>(sym::GfxP2_RenderDmaSend);
        guest_call<void>(sym::DisplayRenderFrameProfile, 0);
        m.frameCount = m.frameCount + 1;
        if (f.uiDemoLoopWait > 1000u)
        {
            // Left alone long enough on a menu that allows it: an attract-mode demo race.
            if (static_cast<uint32_t>(f.frontEndStatus) - 3u < 4u || f.flagPTT != 0 || f.cupsWonMessage != 0)
                f.uiDemoLoopWait = 0;
            else if ((menuSystem().pActiveMenu->evenMoreFlags & 0x10u) == 0)
                f.uiDemoLoopWait = 0;
            if (f.uiDemoLoopWait > 1000u)
            {
                f.cup = 0;
                const ptr32<sExtraLevelData> pExtra = feLevel()->pExtraLevelData;
                m.raceType = 0x100001fcu;
                m.toDo = static_cast<eMainToDo>(4);
                m.status = static_cast<eMain>(0x10);
                m.cupRaceNo = 0;
                const int32_t first = guest_call<int32_t>(sym::rand);
                m.noAI = 8;
                m.splitScreen = 0;
                m.noHuman = 0;
                m.level = static_cast<uint8>(first % 24 + 1);
                if ((at<uint32_t>(sym::tEditVarG + 0x250u) & 4u) != 0 && guest_call<int32_t>(sym::rand) % 5 == 0)
                {
                    m.level = 0x61;
                    guest_call<int32_t>(sym::printf, sym::sec_rodata + 0x508u); // "Demoing a track editor track\n"
                }
                else if ((guest_call<int32_t>(sym::rand) & 1) != 0)
                {
                    m.level = G(waterLevelsG)[guest_call<int32_t>(sym::rand) % 8];
                    guest_call<int32_t>(sym::printf, sym::sec_rodata + 0x528u); // "Demoing a water based level\n"
                }
                else
                {
                    m.level = G(nonWaterLevelsG)[guest_call<int32_t>(sym::rand) % 12];
                    guest_call<int32_t>(sym::printf, sym::sec_rodata + 0x548u); // "Demoing a NON-water based level\n"
                }
                if (guest_call<int32_t>(sym::rand) % 5 == 0)
                {
                    m.splitScreen = 1;
                    m.noAI = 4;
                    guest_call<int32_t>(sym::printf, sym::sec_rodata + 0x570u); // "Demoing in split screen\n"
                }
                const ptr32<char> pName = m.level == 0x61 ? GetGeneralString(8) : AMenuGetText(pExtra[m.level - 1].pTextGroup);
                guest_call<uint32_t>(sym::strcpy, guest_addr(m.trackNames[0]), pName);
            }
        }
        guest_call<void>(sym::VfxRender);
        guest_call<void>(sym::ObjectResetAllTint);
        guest_call<void>(sym::Rdr_EndFrame);
        if (doCredits)
        {
            doCredits = false;
            CreditsDisplay(menuSystem().pActiveMenu);
        }
    } while (m.status == 0);
    guest_call<void>(sym::SfxDoSfx);
    const uint32_t fade = sym::rcmusicVarG + 0x14u;
    while (at<int32_t>(fade) != 0)
    {
        guest_call<void>(sym::RevSfxPauseFade, 0);
        guest_call<void>(sym::SfxDoSfx);
        guest_call<int32_t>(sym::sceGsSyncV, 0);
        guest_call<void>(sym::RCMusicFadeControl);
    }
    guest_call<void>(sym::RevSfxStopAll, 1);
    guest_call<void>(sym::ObjectDeleteAll);
}

void FrontEndTerm()
{
    std::optional<GuestFrame> frame(std::in_place, 0x20u);
    sFrontEndClass &f = fe();
    f.active = 5;
    guest_call<void>(sym::Rdr_ShapeTerm);
    guest_call<void>(sym::Rdr_SkinTerm);
    CarouselTerm();
    guest_call<void>(sym::AColTerm);
    guest_call<void>(sym::OColTerm);
    guest_call<void>(sym::TColTerm);
    guest_call<void>(sym::PhysicsLevelTerm);
    guest_call<void>(sym::CameraTerm);
    guest_call<void>(sym::AnimTerm);
    guest_call<void>(sym::HudTerm);
    guest_call<void>(sym::LevelTerm);
    guest_call<void>(sym::TaskDelete, guest_addr(&f.task));
    guest_call<void>(sym::PathTerm);
    guest_call<void>(sym::ResTypeUnSetConstructDaemon, sym::waterLevelsG + 0x10u);
    guest_call<void>(sym::ResTypeUnSetConstructDaemon, sym::waterLevelsG + 0x8u);
    StatsTerm();
    guest_call<void>(sym::MemBlockFree, guest_addr(&f.pFrontEndStringsOLD));
    guest_call<void>(sym::TSOTerm);
    guest_call<void>(sym::TBoxTerm);
    guest_call<void>(sym::WheelTerm);
    guest_call<void>(sym::GameTaskLvlTerm);
    guest_call<void>(sym::CameraLvlTerm);
    guest_call<void>(sym::ObjectTerm);
    guest_call<void>(sym::VfxTerm);
    guest_call<void>(sym::TaskTerm);
    frame.reset(); // the original leaves its frame before this tail jump
    guest_call<void>(sym::SkinAnimTerm);
}

// The front end's task: counts the idle time before a demo while the menus are up, every 30 ms.
void FrontEndTaskProcess(ptr32<void>, tTime time)
{
    sFrontEndClass &f = fe();
    if (f.active == 2)
        f.uiDemoLoopWait = f.uiDemoLoopWait + 1;
    f.task.time = time + 0x1e;
    guest_call<void>(sym::TaskCachedInsert, guest_addr(&f.task));
}

// Where the camera goes for a menu choice (tag1 the menu's, tag2 the choice's): sets the path's final tag and returns
// the destination's extra data (-1 for none). World, track and cup choices pick the music and the track's name.
int32_t CameraDestination(uint32 tag1, uint32 tag2)
{
    GuestFrame frame(0x70);
    sFrontEndClass &f = fe();
    sPathDetails &path = G(pathDetailsG);
    const ptr32<sExtraLevelData> pExtra = f.pFELevelData->pExtraLevelData;
    int32_t result = -1;
    if (tag1 == 0x1000020eu) // a world
    {
        guest_call<void>(sym::RCMusicSwitchAlternate, 0);
        static const struct
        {
            int32_t xa;
            uint32_t world;
        } kWorlds[7] = {{0, 0x1000006bu}, {2, 0x1000004fu}, {5, 0x10000065u}, {3, 0x10000053u},
                        {1, 0x10000041u}, {4, 0x10000056u}, {6, 0x10000299u}};
        const uint32_t selection = static_cast<uint32_t>(static_cast<int32_t>(menuSystem().pActiveMenu->selection));
        if (selection < 7u)
        {
            f.xaSwitch = kWorlds[selection].xa;
            f.world = static_cast<int32>(kWorlds[selection].world);
        }
        G(debugCameraTagG) = 1;
        path.finalDestTag = 2;
    }
    else if (tag1 == 0x1000020fu) // a track
    {
        guest_call<void>(sym::RCMusicSwitchAlternate, f.xaSwitch);
        const int32_t selection = menuSystem().pActiveMenu->selection;
        if (selection >= 0 && selection <= 3)
            f.track = static_cast<int32>(0x1000006cu + static_cast<uint32_t>(selection));
        mainClass &m = mainVars();
        m.mirrorModeOnOff = 0;
        m.reverseModeOnOff = 0;
        uint32_t t0 = incoming(8);
        int32_t level = worldTrackToLevel(t0);
        if (level == 0x61)
            level = 0x19;
        const ptr32<sAMenuHeader> pMenu = menuSystem().pActiveMenu;
        pMenu->dlgLines = -1;
        path.finalDestTag = 3;
        pMenu->pDialogTxt = pExtra[(level - 1) % 24].pTextGroup;
        G(debugCameraTagG) = 3;
    }
    else if (tag1 == 0x10000212u) // a cup
    {
        if (f.cup == 0)
        {
            G(debugCameraTagG) = 2;
            path.finalDestTag = 4;
        }
        guest_call<void>(sym::RCMusicSwitchAlternate, f.xaSwitch);
    }
    else
    {
        if (tag1 == 0x1000020au)
            guest_call<void>(sym::RCMusicSwitchAlternate, 0);
        for (ptr32<sCameraDestinations> d(sym::camDest); d->sourceTag1 != 0; d = d + 1)
            if (d->sourceTag1 == tag1 && d->sourceTag2 == tag2)
            {
                G(debugCameraTagG) = 4;
                path.finalDestTag = d->destTag;
                f.cameraPilotMode = d->direction;
                result = d->miscData;
                break;
            }
    }
    path.finalDestTag = RemapDestTags(path.finalDestTag);
    return result;
}

// Destination tags 2 to 4 stand for the chosen world's view (2) and the chosen track's view (3) or its locked
// picture (4). A track or world not known resets both to the first.
uint32 RemapDestTags(uint32 tag)
{
    sFrontEndClass &f = fe();
    static const uint32_t kWorlds[7] = {0x10000056u, 0x1000004fu, 0x10000041u, 0x10000053u, 0x1000006bu, 0x10000065u, 0x10000299u};
    int32_t world = -1;
    for (int32_t i = 0; i < 7; ++i)
        if (static_cast<uint32_t>(f.world) == kWorlds[i])
            world = i;
    if (tag == 2)
    {
        static const uint32_t kViews[7] = {0x100000c2u, 0x100000ecu, 0x100000c5u, 0x100000ccu, 0x100000e2u, 0x100000d3u, 0x100002a4u};
        return world < 0 ? tag : kViews[world];
    }
    if (tag < 2 || tag > 4)
        return tag;
    // Per track (0x1000006c to 0x1000006f) and world: the track's view and its locked picture.
    static const uint32_t kTrackViews[4][7][2] = {
        {{0xbf, 0x9a}, {0xea, 0xaa}, {0xc6, 0x9e}, {0xcd, 0xa2}, {0xe2, 0xe2}, {0xd4, 0xa6}, {0x2a1, 0x29d}},
        {{0xc1, 0x99}, {0xe9, 0xa9}, {0xcb, 0x9d}, {0xcf, 0xa1}, {0xe2, 0xe2}, {0xd9, 0xa5}, {0x2a0, 0x29c}},
        {{0xc3, 0x98}, {0xe8, 0xa8}, {0xc8, 0x9c}, {0xd0, 0xa0}, {0xe2, 0xe2}, {0xd6, 0xa4}, {0x29f, 0x29b}},
        {{0xbe, 0x97}, {0xe7, 0xa7}, {0xc7, 0x9b}, {0xce, 0x9f}, {0xe2, 0xe2}, {0xd5, 0xa3}, {0x29e, 0x29a}}};
    const uint32_t track = static_cast<uint32_t>(f.track) - 0x1000006cu;
    const uint32_t *views = nullptr;
    if (track < 4u && world >= 0)
        views = kTrackViews[track][world];
    else
    {
        f.track = 0x1000006c;
        f.world = 0x1000004f;
        views = kTrackViews[0][1];
    }
    return 0x10000000u | views[tag == 4 ? 1 : 0];
}

// The car type of a car-selection tag (two tags per car: player 1's and player 2's). The original builds the result
// in $a1, so an unknown tag returns the caller's $a1: the type a previous call found, in a caller's loop.
eObjectType front::tagToCar(uint32 tag, uint32 &a1)
{
    static const struct
    {
        uint32_t low, high;
        int32_t type;
    } kRanges[] = {{0x38, 0x38, 9},   {0x39, 0x3a, 1},   {0x3b, 0x3c, 13},  {0x3f, 0x40, 12},  {0x42, 0x43, 11},
                   {0x45, 0x46, 10},  {0x48, 0x48, 9},   {0x4d, 0x4e, 8},   {0x51, 0x52, 7},   {0x58, 0x59, 6},
                   {0x5b, 0x5c, 5},   {0x5f, 0x60, 4},   {0x62, 0x63, 3},   {0x68, 0x69, 2},   {0x70, 0x71, 15},
                   {0x72, 0x73, 14},  {0x79, 0x7a, 1},   {0x291, 0x291, 16}, {0x292, 0x292, 17}, {0x293, 0x293, 18},
                   {0x294, 0x294, 19}, {0x295, 0x295, 16}, {0x296, 0x296, 17}, {0x297, 0x297, 18}, {0x298, 0x298, 19}};
    if ((tag & 0xffff0000u) == 0x10000000u)
        for (const auto &r : kRanges)
            if ((tag & 0xffffu) >= r.low && (tag & 0xffffu) <= r.high)
            {
                a1 = static_cast<uint32_t>(r.type);
                return static_cast<eObjectType>(r.type);
            }
    return static_cast<eObjectType>(a1);
}

eObjectType TagToCar(uint32 tag)
{
    uint32_t a1 = incoming(5);
    return tagToCar(tag, a1);
}

// A car's selection tag for player 1 or 2. An unknown car returns the caller's $a3 (player 1) or $a2.
uint32 front::carToTag(eObjectType car, int32_t player, uint32 a2, uint32 a3)
{
    static const uint16_t kTags[20][2] = {{0x39, 0x3a}, {0x68, 0x69}, {0x62, 0x63}, {0x5f, 0x60}, {0x5b, 0x5c},
                                          {0x58, 0x59}, {0x51, 0x52}, {0x4d, 0x4e}, {0x38, 0x48}, {0x45, 0x46},
                                          {0x42, 0x43}, {0x3f, 0x40}, {0x3b, 0x3c}, {0x72, 0x73}, {0x70, 0x71},
                                          {0x291, 0x295}, {0x292, 0x296}, {0x293, 0x297}, {0x294, 0x298}, {0x39, 0x3a}};
    uint32_t tag1 = a3, tag2 = a2;
    const uint32_t index = static_cast<uint32_t>(car) - 1u; // 1-19 and the first boat (20)
    if (index < 20u)
    {
        tag1 = 0x10000000u | kTags[index][0];
        tag2 = 0x10000000u | kTags[index][1];
    }
    return player != 1 ? tag2 : tag1;
}

uint32 CarToTag(eObjectType car, int32_t player)
{
    return carToTag(car, player, incoming(6), incoming(7));
}

// The level of the chosen world and track (1-24), plus 24 reversed and 48 mirrored; picks the world's music. The
// Track Editor's world is level 0x61. An unknown world adds the caller's $t0.
int32_t front::worldTrackToLevel(uint32 &t0)
{
    sFrontEndClass &f = fe();
    int32_t track = 0;
    const uint32_t t = static_cast<uint32_t>(f.track);
    if (t == 0x1000006du)
        track = 1;
    else if (t == 0x1000006eu)
        track = 2;
    else if (t == 0x1000006fu)
        track = 3;
    // The world's index is built in $t0: an unknown world adds what the caller left there.
    switch (static_cast<uint32_t>(f.world))
    {
    case 0x10000056u: t0 = 4; f.xaSwitch = 4; break;
    case 0x1000004fu: t0 = 0; f.xaSwitch = 2; break;
    case 0x10000041u: t0 = 3; f.xaSwitch = 1; break;
    case 0x10000053u: t0 = 2; f.xaSwitch = 3; break;
    case 0x10000065u: t0 = 1; f.xaSwitch = 5; break;
    case 0x10000299u: t0 = 5; f.xaSwitch = 6; break;
    case 0x1000006bu: f.xaSwitch = 0; return 0x61;
    default: break;
    }
    const int32_t world = static_cast<int32_t>(t0);
    const mainClass &m = mainVars();
    return track * 6 + 1 + m.reverseModeOnOff * 24 + world + m.mirrorModeOnOff * 48;
}

int32_t WorldTrackToLevel()
{
    uint32_t t0 = incoming(8);
    return worldTrackToLevel(t0);
}

// The cup (1-8) of a cup-selection tag. An unknown tag returns the caller's $a1.
int32_t TagToCup(uint32 tag)
{
    switch (tag)
    {
    case 0x1000003du: return 1;
    case 0x1000003eu: return 5;
    case 0x1000004bu: return 3;
    case 0x1000004cu: return 7;
    case 0x1000005du: return 4;
    case 0x1000005eu: return 8;
    case 0x10000066u: return 2;
    case 0x10000067u: return 6;
    default: return static_cast<int32_t>(incoming(5));
    }
}

// The screen position menu: a box 288 wide, 208 high (224 in PAL).
void ScreenAdjust()
{
    std::optional<GuestFrame> frame(std::in_place, 0x20u);
    const ptr32<sAMenuHeader> pHeader(sym::frontEndVarG + 0xa8u); // scrollPTTheader
    AMenuInitHeader(pHeader, 0x20);
    pHeader->y = 0x10;
    pHeader->width = 0x120;
#if RC_VERSION_EU
    pHeader->height = 0xe0;
#else
    pHeader->height = 0xd0;
#endif
    pHeader->x = 0x10;
    guest_call<void>(sym::AMenuSnapsize, pHeader);
    frame.reset(); // the original leaves its frame before this tail jump
    guest_call<void>(sym::AMenuDrawBox, pHeader, 0);
}

// Leaves the podium: its cars' sounds stop and the front end's music comes back.
void MainQuitPodium()
{
    std::optional<GuestFrame> frame(std::in_place, 0x20u);
    const uint32_t list = sym::objectVarG + 0x4cu;
    for (uint32_t i = 0; i < 4u; ++i)
        guest_call<void>(sym::ObjectClearSfx, at<uint32_t>(list + i * 4u));
    guest_call<void>(sym::RevSfxStopAll, 0);
    frame.reset(); // the original leaves its frame before this tail jump
    guest_call<void>(sym::RCMusicChain, 0x69, 0xa);
}

// The camera has flown from the podium back to the menus.
void PathPilotEndCallbackHOP(ptr32<void> pData)
{
    std::optional<GuestFrame> frame(std::in_place, 0x50u);
    sFrontEndClass &f = fe();
    const ptr32<_sFELevelData> pFELevelData = f.pFELevelData;
    guest_call<int32_t>(sym::TimerGet, guest_addr(&mainVars().gameTimer));
    f.active = 2;
    f.frontEndStatus = static_cast<eFront>(2);
    f.raceResult = 0;
    MainQuitPodium();
    at<int32_t>(sym::visFxVarG + 0x2730u) = 0; // iResultTrigger
    guest_call<void>(sym::PathPilotEndCallback, pData);
    menuSystem().pActiveMenu = pFELevelData->pUIGroup->pSourceHeader;
#if RC_VERSION_EU
    if (static_cast<int32_t>(mainVars().activeLanguage) != kNoLanguage)
        activateFirstMenu();
#else
    mainVars().activeLanguage = static_cast<eLanguages>(0);
    activateFirstMenu();
#endif
    frame.reset(); // the original leaves its frame before this tail jump
    guest_call<void>(sym::SetCameraToThisTag, 0x100000e6u, feLevel()->pCameras);
}

// Back from a race to the front end: the camera flies from the podium to the menus.
void HandoverToFE()
{
    std::optional<GuestFrame> frame(std::in_place, 0x30u);
    sFrontEndClass &f = fe();
    f.active = 4;
    f.raceResult = 0;
    f.cameraPilotMode = 3;
    f.frontEndStatus = static_cast<eFront>(2);
    guest_call<void>(sym::PathCopy, feLevel()->pPaths[2], 3);
    sPathDetails &path = G(pathDetailsG);
    path.pActivePath = ptr32<sCameraPath>(guest_addr(&path.constructedPath));
    guest_call<void>(sym::SetCameraToThisTag, 0x100000b7u, feLevel()->pCameras);
    path.finalDestTag = 0x100000dau;
    frame.reset(); // the original leaves its frame before this tail jump
    guest_call<void>(sym::CameraSetNewPath, sym::cameraVarG, sym::PathPilotEndCallbackHOP, sym::PathPilotNodeCallback, path.pActivePath);
}

void FECarouselInit(tTime time)
{
    std::optional<GuestFrame> frame(std::in_place, 0x20u); // the callees' frames sit where the original's do
    CarouselInit();
    CarouselSetUp(0x13, time);
    frame.reset(); // the original leaves its frame before this tail jump
    CarouselSetAvailability();
}

// The menus start at the first menu (Europe: once a language has been chosen).
void FEStartPos()
{
    mainClass &m = mainVars();
#if RC_VERSION_EU
    if (static_cast<int32_t>(m.activeLanguage) == kNoLanguage)
        return;
#else
    m.activeLanguage = static_cast<eLanguages>(0);
#endif
    activateFirstMenu();
}

// A player's car choice: its tag (the execute code), its type in the car list and the player's car.
int32_t SetUpVehicle(int32_t carNo, int32_t carType)
{
    sFrontEndClass &f = fe();
    f.carTag[carNo] = static_cast<eObjectType>(menuSystem().executeCode);
    f.lastWorldSel = 0xffffffffu;
    G(carListVarG).car[carNo].type = static_cast<eObjectType>(carType);
    if (carNo == 0)
        mainVars().p1Car = static_cast<uint8>(carType);
    else if (carNo == 1)
        mainVars().p2Car = static_cast<uint8>(carType);
    return carType;
}

void xxxDummyCallback() {}

ptr32<_sSingleCamera> xxxGetFECameraFromTag(uint32 tag)
{
    GuestFrame frame(0x10);
    return guest_call<ptr32<_sSingleCamera>>(sym::GetCameraFromTag, tag, feLevel()->pCameras);
}

} // namespace rc

RC_DECOMP_FN_SETUP(FrontSetupClass, rc::fuzz::frontSetupClass)
RC_DECOMP_FN_BLOCKING_SETUP_HEAP(FrontEndInit, rc::fuzz::frontEndInit)
RC_DECOMP_FN_BLOCKING_SETUP_HEAP(FrontEndProcess, rc::fuzz::frontEndProcess)
RC_DECOMP_FN_SETUP_HEAP(FrontEndTerm, rc::fuzz::frontEndTerm)
RC_DECOMP_FN_SETUP_HEAP(FrontEndTaskProcess, rc::fuzz::frontTask)
RC_DECOMP_FN_SETUP(CameraDestination, rc::fuzz::frontCameraDestination)
RC_DECOMP_FN_SETUP(RemapDestTags, rc::fuzz::frontRemapDestTags)
RC_DECOMP_FN_SETUP(TagToCar, rc::fuzz::frontTag)
RC_DECOMP_FN_SETUP(CarToTag, rc::fuzz::frontCarToTag)
RC_DECOMP_FN_SETUP(WorldTrackToLevel, rc::fuzz::frontWorld)
RC_DECOMP_FN_SETUP(TagToCup, rc::fuzz::frontTag)
RC_DECOMP_FN_SETUP_HEAP(ScreenAdjust, rc::fuzz::frontDraw)
RC_DECOMP_FN_SETUP(MainQuitPodium, rc::fuzz::frontStubbed)
RC_DECOMP_FN_SETUP(PathPilotEndCallbackHOP, rc::fuzz::frontSetupClass)
RC_DECOMP_FN_SETUP(HandoverToFE, rc::fuzz::frontStubbed)
RC_DECOMP_FN_SETUP(FECarouselInit, rc::fuzz::frontCarouselInit)
RC_DECOMP_FN_SETUP(FEStartPos, rc::fuzz::frontSetupClass)
RC_DECOMP_FN_SETUP(SetUpVehicle, rc::fuzz::frontSetUpVehicle)
RC_DECOMP_FN(xxxDummyCallback)
RC_DECOMP_FN_SETUP(xxxGetFECameraFromTag, rc::fuzz::frontStubbed)
