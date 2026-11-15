// Decompiled from MAIN.c: start-up and shut-down, each mode's set-up and tear-down (front end, track editor, race),
// the race's and track editor's frame loops and the split-screen windows. The end of a race (results, ranking, cup
// points), the pause menu and the computer's cars are in main2.cpp. main, MainLoop (the boot and the mode loop: they
// never return) and printf stay recompiled.
//
// Most of these run once per mode or level, or are frame loops that wait for the next frame, so they are registered
// as blocking (checked by the whole game only); SetFrameWindow and MainScreenGameInit have random inputs. The
// overrides' MainFrontProcess (RC_BOOT_LEVEL) and MainGameLevelInit wrappers reach these through decompOr.

#include "main.h"
#include "main_fuzz.h"

namespace rc {

using namespace mainunit;

namespace {

// Other guest functions these call (native ones too: the frame loops reach them through the dispatcher).
constexpr uint32_t kMemBlockInit = 0x194238u, kMemBlockAllocHi = 0x1942d0u, kMemBlockFree = 0x193a00u, kMemBlockTerm = 0x1942a0u;
constexpr uint32_t kDisplayInit = 0x182088u, kDisplaySetBackground = 0x1820d8u, kDisplayDisable = 0x1820a0u, kDisplayEnable = 0x182098u;
constexpr uint32_t kDisplayTerm = 0x182090u, kDisplayRenderFrameProfile = 0x1820f0u, kSysTimerInit = 0x1aeef8u, kSysTimerTerm = 0x1aef80u;
constexpr uint32_t kInputInit = 0x190908u, kInputTerm = 0x190a88u, kCardConstruct = 0x192420u, kCardInit = 0x192430u, kCardUnInit = 0x1924f8u;
constexpr uint32_t kFMInit = 0x1a0b18u, kFMPrepareHi = 0x1a0bb0u, kFMUnPrepare = 0x1a0540u, kBackdropInit = 0x112010u;
constexpr uint32_t kBackdropDoInitialScreens = 0x111138u, kBackdropTerm = 0x112018u, kSfxInitFirst = 0x1ac408u, kExit = 0x1b4a18u;
constexpr uint32_t kLoadErrorInit = 0x129428u, kLoadErrorTerm = 0x129438u, kResInit = 0x1aa708u, kResTerm = 0x1aadb8u;
constexpr uint32_t kTEditInitPermTrackStructures = 0x1541e0u, kTEditAllocPermTrackStructures = 0x146870u;
constexpr uint32_t kTEditFreePermTrackStructures = 0x1468a8u, kPhysicsInit = 0x13e368u, kPhysicsTerm = 0x13e3a0u, kSfxInit = 0x1ac1b8u;
constexpr uint32_t kRevSfxInit = 0x142300u, kSfxOn = 0x1ae520u, kRCMusicInit = 0x1416a8u, kSfxInitReverb = 0x1ac4c0u;
constexpr uint32_t kTSOSetSFXCallBack = 0x18ccb0u, kRevSfxTSO = 0x143ba8u, kInit3d = 0x1826c8u, kInit3dClean = 0x182900u;
constexpr uint32_t kView3dDefine3dDeviceWindow = 0x184d80u, kInit3dWindows = 0x182698u, kUserInterfaceInit = 0x10a470u;
constexpr uint32_t kUserInterfaceTerm = 0x10a508u, kCarListInit = 0x11a070u, kCarListTerm = 0x119cd0u, kClearAllSecrets = 0x1214b8u;
constexpr uint32_t kLoadingInit = 0x129448u, kMtlInit = 0x1331d8u, kAColInit = 0x18ccd0u, kOColInit = 0x13b890u, kTColInit = 0x144ed0u;
constexpr uint32_t kCarColInit = 0x119c08u, kGameTaskInit = 0x124028u, kCameraInit = 0x117b88u, kAnimInit = 0x110908u;
constexpr uint32_t kFrontEndInit = 0x11c188u, kFrontEndProcess = 0x11c740u, kFrontEndTerm = 0x11d1f0u, kRdrPolyInit = 0x1a7518u;
constexpr uint32_t kRdrAlphaInit = 0x1a15a0u, kVfxLvlInit = 0x156cc0u, kVfx2TSFXLvlInit = 0x16d080u, kMeshSetQuality = 0x182948u;
constexpr uint32_t kMirrorSet = 0x182950u, kMpegOnInterruptTERM = 0x194ce0u, kAnimTerm = 0x10f130u, kCameraTerm = 0x117b90u;
constexpr uint32_t kAColTerm = 0x18cce8u, kOColTerm = 0x13b898u, kTColTerm = 0x144ef0u, kCarColTerm = 0x119c10u, kGameTaskTerm = 0x124030u;
constexpr uint32_t kVfxLvlTerm = 0x16a030u, kVfx2TSFXLvlTerm = 0x179160u, kRdrPolyTerm = 0x1a90e0u, kRdrAlphaTerm = 0x1a16d8u;
constexpr uint32_t kLevelInit = 0x128b70u, kLevelLoad = 0x128cf8u, kLevelTerm = 0x128c58u, kTEditInit = 0x146758u, kTEditTerm = 0x1468d8u;
constexpr uint32_t kCameraLvlInit = 0x117b98u, kCameraSetMode = 0x117518u, kCameraProcess = 0x115b60u, kRdrStartFrame = 0x1a3638u;
constexpr uint32_t kApplyDefaultState = 0x1a1040u, kGfxP2ZWriteEnable = 0x190058u, kTEditControl = 0x1469a8u, kInit3dFrame = 0x1827f8u;
constexpr uint32_t kInit3dFrame2 = 0x1828a8u, kTileFrameInit = 0x183f20u, kTEditDisplay = 0x146a50u, kFontPrint = 0x189808u;
constexpr uint32_t kRdrAlphaAddToDmaChain = 0x1a1f50u, kRdrEndFrame = 0x1a38b8u, kSkinAnimInit = 0x1a49c0u, kObjectInit = 0x139338u;
constexpr uint32_t kPlayerInit = 0x13f628u, kAIInit = 0x1014b0u, kAINodeInit = 0x103b78u, kWheelInit = 0x181d50u, kVfxInit = 0x156bb8u;
constexpr uint32_t kHudInit = 0x124438u, kPhysicsLevelInit = 0x13e3c8u, kTSOInit = 0x18ca30u, kTBoxInit = 0x144c00u, kPickupInit = 0x13e840u;
constexpr uint32_t kPathInit = 0x13c5f8u, kRdrShapeInit = 0x1a4ad0u, kRdrSkinInit = 0x1a33f0u, kRdrWaterInit = 0x1a9310u;
constexpr uint32_t kVSyncCallback = 0x19f260u, kTaskInit = 0x1af880u, kAColLvlInit = 0x18e1c8u, kOColLvlInit = 0x13b8a0u;
constexpr uint32_t kTColLvlInit = 0x146318u, kCarColLvlInit = 0x119c18u, kRdrParticleInit = 0x1aa358u, kDepthCueInit = 0x181f70u;
constexpr uint32_t kObjectCreateCars = 0x1368a0u, kAISetupCars = 0x1004a0u, kGameTaskLvlInit = 0x123708u, kWeaponInit = 0x1811f8u;
constexpr uint32_t kDepthCueSet = 0x181f90u, kAnimReinit = 0x10f4f8u, kRandomSetSeed = 0x1a0f28u, kPickupAllSpawn = 0x13e8a0u;
constexpr uint32_t kHudCreate = 0x128aa8u, kMenuInitScan = 0x122948u, kFontSet = 0x18a3c0u, kTEditGetSoundTrackNdxForTrack = 0x154568u;
constexpr uint32_t kVertAnimFrameUpdate = 0x18b008u, kTaskProcess = 0x1af8f8u, kTBoxProcess = 0x1443d0u, kRdrParticleProcess = 0x1aa5d8u;
constexpr uint32_t kMeshInitFrame = 0x182940u, kInit3dChangeViewcone = 0x1826a0u, kLsFrameInit = 0x182578u, kGfxP2Tex = 0x18ea98u;
constexpr uint32_t kGfxP2RenderDmaSend = 0x18fcd0u, kAIDoRaceOrder = 0x1005d8u, kMemset = 0x1b7d10u, kLsRenderWorld = 0x182500u;
constexpr uint32_t kTEditRenderGameTrack = 0x1537c0u, kVfxRenderShadows = 0x1697e8u, kObjectRenderAll = 0x1394f8u;
constexpr uint32_t kVertAnimRender = 0x18b010u, kTSORender = 0x18b598u, kPickupRenderAll = 0x13eb88u, kHudPanel = 0x125550u;
constexpr uint32_t kWeaponRenderAll = 0x17c098u, kVfxRender = 0x157be0u, kObjectResetAllTint = 0x139620u, kObjectSfxDoAll = 0x139708u;
constexpr uint32_t kPlayerSetVibrate = 0x140588u, kRdrStateSetScissor = 0x1a14a8u, kAnimLvlTerm = 0x10f128u, kHudDelete = 0x124480u;
constexpr uint32_t kWeaponTerm = 0x17b6d0u, kObjectDeleteAll = 0x1356a0u, kPickupDeleteAll = 0x13f0f8u, kRdrParticleTerm = 0x1aa380u;
constexpr uint32_t kGameTaskLvlTerm = 0x123748u, kTaskTerm = 0x1af8e0u, kCameraLvlTerm = 0x117cc0u, kAColLvlTerm = 0x18e1d0u;
constexpr uint32_t kOColLvlTerm = 0x13b8b0u, kTColLvlTerm = 0x146320u, kCarColLvlTerm = 0x119c48u, kRdrShapeTerm = 0x1a4bf8u;
constexpr uint32_t kRdrSkinTerm = 0x1a2910u, kRdrWaterTerm = 0x1a9470u, kAINodeTerm = 0x1016b8u, kWheelTerm = 0x181d90u;
constexpr uint32_t kPlayerTerm = 0x13f918u, kObjectTerm = 0x133aa0u, kVfxTerm = 0x156db0u, kHudTerm = 0x124470u;
constexpr uint32_t kPhysicsLevelTerm = 0x13e3d8u, kTBoxTerm = 0x144c90u, kPickupTerm = 0x13f0f0u, kTSOTerm = 0x18b018u;
constexpr uint32_t kTEditFreeTrackAINodes = 0x152928u, kTEditFreeTrackALF = 0x1542d8u, kTEditLevelTerm = 0x1465a8u, kPathTerm = 0x13c610u;
constexpr uint32_t kSkinAnimTerm = 0x1a44d0u, kTexFreeIntermediateResData = 0x183ee8u;

constexpr uint32_t kTimer = kMain + 0x10u, kWindow0 = kMain + 0x20u, kWindow1 = kMain + 0x8cu, kWindow2 = kMain + 0xf8u;
constexpr uint32_t kResHandle = kMain + 0x164u, kCurrentCamera = kCamera + 0xb70u, kFrameMode = 0x328390u, kFrameSplit = 0x328394u;

void call(uint32_t address) { guest_call(address); }

uint8_t splitScreen() { return at<uint8_t>(kMain + 0x368u); }
uint8_t level() { return at<uint8_t>(kMain + 0x34au); }
uint32_t &frames() { return at<uint32_t>(kMain + 0x354u); }
uint32_t &frameTime() { return at<uint32_t>(kMain + 0x358u); }
uint32_t &gameTime() { return at<uint32_t>(kMain + 0x1cu); }

// View3dDefine3dDeviceWindow(window, -160, -120, 160, 120, 160, 120, -160, then five on the stack): the
// window's place on the screen (its last five arguments, as words in the stack's 8-byte slots).
void defineWindow(const GuestFrame &frame, uint32_t window, int32_t s0, int32_t s8, int32_t s10, int32_t s18, int32_t s20)
{
    at<int32_t>(frame.local<uint8_t>(0).address) = s0;
    at<int32_t>(frame.local<uint8_t>(8).address) = s8;
    at<int32_t>(frame.local<uint8_t>(0x10).address) = s10;
    at<int32_t>(frame.local<uint8_t>(0x18).address) = s18;
    at<int32_t>(frame.local<uint8_t>(0x20).address) = s20;
    guest_call(kView3dDefine3dDeviceWindow, window, -0xa0, -0x78, 0xa0, 0x78, 0xa0, 0x78, -0xa0);
}

// Waits for the music to fade out.
void fadeMusicOut()
{
    while (at<uint32_t>(kMusic + 0x14u) != 0)
    {
        guest_call(kRevSfxPauseFade, 0);
        call(kSfxDoSfx);
        guest_call(kSceGsSyncV, 0);
        call(kRCMusicFadeControl);
    }
}

// Once a second: the frame rate.
void countFrames(uint32_t time, bool print)
{
    if (time - frameTime() < 1000u)
        return;
    const uint32_t count = frames();
    frameTime() = time;
    at<uint32_t>(kMain + 0x35cu) = count;
    if (print)
        guest_call(kPrintf, 0x323628u, count);
    frames() = 0;
}

} // namespace

void MainInit()
{
    GuestFrame frame(0x80);
    guest_call(kPrintf, 0x3233a0u);
    guest_call(kMemBlockInit, 0x500000, 0x1af0000);
    call(kDisplayInit);
    guest_call(kDisplaySetBackground, 0, 0, 0);
    call(kSysTimerInit);
    call(kInputInit);
    call(kCardConstruct);
    call(kCardInit);
    guest_call(kInputSetPadMode, 0, 0x16);
    guest_call(kInputSetPadMode, 4, 0x16);
    guest_call(kInputSetVibrationFlags, 0, 0);
    guest_call(kInputSetVibrationFlags, 4, 0);
    call(kFMInit);
    call(kBackdropInit);
    call(kSfxInitFirst);
    call(kBackdropDoInitialScreens);
    call(kDisplayDisable);
    if (guest_call<uint32_t>(kMemBlockAllocHi, kResHandle, 0x1194000) == 0)
        guest_call(kExit, 1);
    guest_call(kFMPrepareHi, 0x9c40);
    call(kLoadErrorInit);
    guest_call(kResInit, 0x327ef0u, at<uint32_t>(kResHandle), 0x1194000);
    call(kFMUnPrepare);
    guest_call(kMemBlockFree, kResHandle);
    call(kTEditInitPermTrackStructures);
    call(kTEditAllocPermTrackStructures);
    call(kDisplayEnable);
    call(kPhysicsInit);
    guest_call(kSfxInit, 1);
    call(kRevSfxInit);
    call(kSfxOn);
    guest_call(kRCMusicInit, 0x24);
    call(kSfxInitReverb);
    guest_call(kTSOSetSFXCallBack, kRevSfxTSO);
    if (guest_call<uint32_t>(kInit3d, 0x3436, 0x111, 5, 0x2000, 0x7d0, 0x2328, 0x25800) == 0)
    {
        call(kInit3dClean);
        guest_call(kExit, 1);
    }
    defineWindow(frame, kWindow0, -0x78, 0xa0, 0x78, 0xa0, 0x78);
    at<uint8_t>(kMain + 0x369u) = 1;     // noHuman
    at<uint32_t>(kMain + 0x3a8u) = 1;    // pickupsOnOff
    at<uint32_t>(kMain + 0x3b0u) = 0xc4; // sfxVolume
    at<uint32_t>(kMain + 0x3b4u) = 3;    // iNumberOfLaps
    at<uint8_t>(kFrontEnd + 0x2d9u) = 5; // active
    at<uint8_t>(kMain + 0x368u) = 0;     // splitScreen
    at<uint32_t>(kMain + 0x3acu) = 0xc4; // musicVolume
    at<uint32_t>(kMain + 0x350u) = 0;    // lastRaceType
    at<uint32_t>(kFrontEnd + 0x25cu) = 0;
    guest_call(kInit3dWindows, kWindow0, 0);
    call(kUserInterfaceInit);
    call(kCarListInit);
    for (uint32_t off = 0x19cu; off <= 0x258u; off += 4u) // the time trial records
        at<int32_t>(kFrontEnd + off) = -1;
    toDo() = 1;
    at<uint8_t>(kFrontEnd + 0x2d9u) = 5;
    at<uint32_t>(kMain + 0xcu) = 0; // trackEdRace and the bytes after it
    guest_call(kClearAllSecrets, kMain + 0x3bcu);
    at<uint32_t>(kFrontEnd + 0x188u) = 1;
    at<uint32_t>(kFrontEnd + 0x18cu) = 3;
    at<uint32_t>(kFrontEnd + 0x190u) = 8;
    at<uint8_t>(kMain + 0x3e0u) = 4;
    at<uint8_t>(kMain + 0x3e2u) = 10;
    at<uint8_t>(kMain + 0x3e4u) = 0;
    at<uint8_t>(kFrontEnd + 0x2dau) = 0;
    at<uint32_t>(kFrontEnd + 0x158u) = 0;
    at<uint32_t>(kFrontEnd + 0x178u) = 0;
    at<uint32_t>(kFrontEnd + 0x194u) = 4;
    at<uint16_t>(kFrontEnd + 0x264u) = 0;
    at<uint16_t>(kFrontEnd + 0x266u) = 0;
    at<uint8_t>(kMain + 0x3dfu) = 4;
    at<uint8_t>(kMain + 0x3e1u) = 10;
    at<uint8_t>(kMain + 0x3e3u) = 0;
}

void MainFrontInit()
{
    {
        GuestFrame frame(0x30);
        guest_call(kLoadingInit, 1);
        for (const uint32_t f : {kMtlInit, kAColInit, kOColInit, kTColInit, kCarColInit, kGameTaskInit, kCameraInit, kAnimInit,
                                 kFrontEndInit, kRdrPolyInit, kRdrAlphaInit, kVfxLvlInit, kVfx2TSFXLvlInit})
            call(f);
        guest_call(kMeshSetQuality, 2);
        guest_call(kMirrorSet, at<uint32_t>(kMain + 0x398u));
        at<uint32_t>(kMain + 0x174u) = 0; // gameEndPos
        MainSetCars(static_cast<eCarSet>(0));
        if (status() == 0x40)
        {
            // Back from a race's results: the winner's or loser's tune.
            int32_t tune;
            if (noHuman() == 2)
                tune = at<uint16_t>(kAi + 0x64u) == 1 || at<uint16_t>(kAi + 0x66u) == 1 ? 0x65 : 0x64;
            else
                tune = at<uint16_t>(kAi + 0x64u) == 1 ? 0x65 : 0x64;
            guest_call(kRCMusicPlayLevel, tune);
            guest_call(kRCMusicSetVol, at<uint32_t>(kMain + 0x3acu));
        }
        else if (at<uint32_t>(0x327d54u) != 0 && at<uint32_t>(0x327d64u) == 1) // firstEntryG, memoryCardDontCareG
        {
            guest_call(kRCMusicPlayLevel, 0x69);
            guest_call(kRCMusicSetVol, at<uint32_t>(kMain + 0x3acu));
        }
    }
    guest_call(kMpegOnInterruptTERM, 1);
}

void MainFrontProcess() { call(kFrontEndProcess); }

void MainFrontTerm()
{
    {
        GuestFrame frame(0x10);
        for (const uint32_t f : {kAnimTerm, kCameraTerm, kAColTerm, kOColTerm, kTColTerm, kCarColTerm, kGameTaskTerm, kVfxLvlTerm,
                                 kVfx2TSFXLvlTerm, kRdrPolyTerm, kRdrAlphaTerm, kFrontEndTerm})
            call(f);
    }
    guest_call(kMemBlockFree, kResHandle);
}

void MainTrackEdInit()
{
    {
        GuestFrame frame(0x20);
        at<uint32_t>(kCarList + 0x1e8u) = 0;
        at<uint32_t>(kCarList + 0x1e4u) = 0;
        call(kRCMusicStop);
        call(kLevelInit);
        call(kTEditInit);
        at<uint8_t>(kMain + 0x34au) = 0x63;
        guest_call(kCameraLvlInit, 0);
        guest_call(kTimerSetup, kTimer, 0, 0);
        guest_call(kMpegOnInterruptTERM, 1);
        guest_call(kRCMusicPlayLevel, 0x68);
        guest_call(kRCMusicSetVol, at<uint32_t>(kMain + 0x3acu));
        guest_call(kDisplaySetBackground, 0, 0, 0x64);
    }
    call(kRdrAlphaInit);
}

// The track editor's frame loop, until it is left.
void MainTrackEdProcess()
{
    GuestFrame frame(0xe0);
    status() = 1;
    frames() = 0;
    frameTime() = guest_call<uint32_t>(kTimerGet, kTimer);
    guest_call(kCameraSetMode, kCamera, 0xc);
    call(kInputReset);
    uint32_t frameCount = 0;
    do
    {
        call(kRdrStartFrame);
        call(kApplyDefaultState);
        call(kGfxP2ZWriteEnable);
        call(kInputSnapshot);
        const uint32_t time = guest_call<uint32_t>(kTimerGet, kTimer);
        gameTime() = time;
        guest_call(kTEditControl, time);
        guest_call(kCameraProcess, time, 0);
        call(kSfxDoSfx);
        if (guest_call<uint32_t>(kInit3dFrame, kWindow0, at<uint32_t>(kCurrentCamera) + 0x24u, time, 0) == 0)
            return;
        call(kTileFrameInit);
        guest_call(kTEditDisplay, time, ++frameCount);
        countFrames(time, false);
        guest_call(kFontPrint, 0x294, 0x2e, 0x327ef8u);
        guest_call(kDisplayRenderFrameProfile, 0);
        call(kRdrAlphaAddToDmaChain);
        call(kRdrEndFrame);
        frames() = frames() + 1;
    } while (status() == 1);
    call(kSfxDoSfx);
    fadeMusicOut();
}

void MainTrackEdTerm()
{
    {
        GuestFrame frame(0x10);
        call(kTEditTerm);
        call(kRdrAlphaTerm);
        call(kLevelTerm);
    }
    guest_call(kMemBlockFree, kResHandle);
}

void MainGameInit()
{
    {
        GuestFrame frame(0x20);
        for (const uint32_t f : {kMtlInit, kAColInit, kOColInit, kTColInit, kCarColInit, kGameTaskInit, kCameraInit, kAnimInit, kRCMusicStop})
            call(f);
        guest_call(kPrintf, 0x3233b8u);
        call(kSkinAnimInit);
        guest_call(kPrintf, 0x3233d8u);
        call(kObjectInit);
        guest_call(kPrintf, 0x3233f0u);
        guest_call(kPlayerInit, noHuman());
        guest_call(kPrintf, 0x323408u);
        guest_call(kAIInit, noAI());
        guest_call(kPrintf, 0x323420u);
        call(kAINodeInit);
        guest_call(kPrintf, 0x323438u);
        call(kWheelInit);
        guest_call(kPrintf, 0x323450u);
        call(kVfxInit);
        guest_call(kPrintf, 0x323468u);
        call(kHudInit);
        guest_call(kPrintf, 0x323480u);
        call(kPhysicsLevelInit);
        guest_call(kPrintf, 0x323498u);
        call(kLevelInit);
        guest_call(kPrintf, 0x3234b8u);
        call(kTSOInit);
        guest_call(kPrintf, 0x3234e0u);
        call(kTBoxInit);
        guest_call(kPrintf, 0x323500u);
        call(kPickupInit);
        guest_call(kMirrorSet, at<uint32_t>(kMain + 0x398u));
        guest_call(kPrintf, 0x323518u);
        guest_call(kRevSfxInitAmbient, 1);
        call(kPathInit);
        guest_call(kPrintf, 0x323530u);
        guest_call(kLevelLoad, level());
        call(kRdrShapeInit);
        if (level() != 0x61) // not a track editor track
        {
            call(kRdrSkinInit);
            call(kRdrWaterInit);
        }
        MainScreenGameInit(splitScreen());
        // The level's first message (the list ends with -1): its value for the pause menu.
        uint32_t p = at<uint32_t>(at<uint32_t>(kLevel) + 0x44u);
        const uint32_t target = at<uint32_t>(kPauseMenu);
        while (at<int32_t>(p) != -1)
            p += 0x14u;
        at<uint32_t>(target + 0x2cu) = at<uint32_t>(p + 8u);
        at<uint8_t>(kMain + 0x36bu) = 0;
    }
    call(kInputReset);
}

void MainGameLevelInit()
{
    {
        GuestFrame frame(0x40);
        at<uint32_t>(kTimerResumed) = 0;
        guest_call(kMpegOnInterruptTERM, 1);
        guest_call(kVSyncCallback, 0x182680u);
        call(kTaskInit);
        guest_call(kPrintf, 0x323548u);
        guest_call(kTimerSetup, kTimer, 0, 1);
        gameTime() = 0;
        at<uint32_t>(kMain + 0x39cu) = 1;
        at<uint16_t>(kMain + 0x366u) = 0;
        for (const uint32_t f : {kAColLvlInit, kOColLvlInit, kTColLvlInit, kCarColLvlInit, kRdrParticleInit, kRdrPolyInit, kRdrAlphaInit,
                                 kVfxLvlInit, kDepthCueInit})
            call(f);
        guest_call(kObjectCreateCars, gameTime());
        guest_call(kPrintf, 0x323560u);
        call(kAISetupCars);
        guest_call(kPrintf, 0x323578u);
        guest_call(kCameraLvlInit, splitScreen());
        guest_call(kPrintf, 0x323598u);
        call(kGameTaskLvlInit);
        guest_call(kPrintf, 0x3235b8u);
        call(kWeaponInit);
        guest_call(kPrintf, 0x3235d0u);
        guest_call(kDepthCueSet, 0x61a8, splitScreen());
        guest_call(kMeshSetQuality, splitScreen() == 0 ? 2 : 0);
        call(kAnimReinit);
        guest_call(kRandomSetSeed, at<uint32_t>(0x31ff40u));
        if (at<uint32_t>(kMain + 0x3a8u) != 0)
        {
            guest_call(kPrintf, 0x3235f0u);
            guest_call(kPickupAllSpawn, 0);
        }
        call(kHudCreate);
        call(kVfx2TSFXLvlInit);
        PauseMenuScan();
        guest_call(kMenuInitScan, at<uint32_t>(at<uint32_t>(kLevel) + 0x44u), 1);
        BuildResults(0);
        const uint32_t cupNo = cup();
        at<uint32_t>(kMain + 0x174u) = 0;
        if (cupNo != 0 && level() == at<uint8_t>(kMain + 0x340u))
        {
            // The cup's first race.
            ClearRanking();
            ClearChampionshipPoints();
        }
        SetChampionshipPoints();
        ClearRankingDisplay();
        guest_call(kPrintf, 0x323608u);
        guest_call(kInputSetVibrationFlags, 0, 0);
    }
    guest_call(kInputSetVibrationFlags, 4, 0);
}

void MainGameProcess()
{
    GuestFrame frame(0xf0);
    const uint32_t temp = frame.local<uint8_t>(0x40).address;
    frames() = 0;
    frameTime() = guest_call<uint32_t>(kTimerGet, kTimer);
    guest_call(kFontSet, at<uint32_t>(at<uint32_t>(at<uint32_t>(kLevel) + 0x40u) + 0x4cu));
    call(kInputReset);
    const auto playTune = [&]
    {
        if (level() < 0x61)
            guest_call(kRCMusicPlayLevel, level());
        else
            guest_call(kRCMusicPlayTrackEdit, guest_call<uint32_t>(kTEditGetSoundTrackNdxForTrack));
    };
    if (status() == 0x10)
    {
        playTune();
        guest_call(kRCMusicSetVol, at<uint32_t>(kMain + 0x3acu));
    }
    else
    {
        at<uint32_t>(kMusic + 0x150u) = 1;
        playTune();
        guest_call(kRCMusicSetVol, 0);
    }
    do
    {
        call(kRdrStartFrame);
        call(kApplyDefaultState);
        call(kInputSnapshot);
        const uint32_t time = guest_call<uint32_t>(kTimerGet, kTimer);
        guest_call(kVertAnimFrameUpdate, time);
        gameTime() = time;
        guest_call(kTaskProcess, time);
        SetFrameWindow(0, static_cast<tTime>(gameTime()));
        call(kTBoxProcess);
        call(kRdrParticleProcess);
        call(kMeshInitFrame);
        guest_call(kInit3dChangeViewcone, at<uint16_t>(kCamera + 0x574u));
        guest_call(kLsFrameInit, gameTime(), splitScreen(), 0);
        guest_call(kGfxP2Tex, at<uint32_t>(at<uint32_t>(0x1f106cu) + 0x60u));
        call(kGfxP2RenderDmaSend);
        call(kAIDoRaceOrder);
        guest_call(kMemset, temp, 0, 0xc);
        {
            const uint32_t t = gameTime();
            at<uint32_t>(kMain + 0x378u) = 0;
            SetFrameWindow(0, static_cast<tTime>(t));
        }
        if (level() != 0x61)
            call(kLsRenderWorld);
        else
            call(kTEditRenderGameTrack);
        call(kVfxRenderShadows);
        call(kObjectRenderAll);
        call(kVertAnimRender);
        guest_call(kTSORender, gameTime());
        guest_call(kPickupRenderAll, gameTime());
        guest_call(kHudPanel, at<uint32_t>(at<uint32_t>(kCurrentCamera) + 0xacu), 0);
        guest_call(kWeaponRenderAll, 0);
        call(kRdrAlphaAddToDmaChain);
        call(kApplyDefaultState);
        call(kVfxRender);
        call(kApplyDefaultState);
        call(kGfxP2RenderDmaSend);
        if (splitScreen() != 0)
        {
            SetFrameWindow(1, static_cast<tTime>(gameTime()));
            guest_call(kInit3dChangeViewcone, at<uint16_t>(kCamera + 0xb2cu));
            guest_call(kLsFrameInit, gameTime(), splitScreen(), 1);
            guest_call(kMemset, temp, 0, 0xc);
            {
                const uint32_t t = gameTime();
                at<uint32_t>(kMain + 0x378u) = 1;
                SetFrameWindow(1, static_cast<tTime>(t));
            }
            if (level() != 0x61)
                call(kLsRenderWorld);
            else
                call(kTEditRenderGameTrack);
            call(kVertAnimRender);
            call(kObjectRenderAll);
            call(kVfxRenderShadows);
            guest_call(kTSORender, gameTime());
            call(kApplyDefaultState);
            guest_call(kPickupRenderAll, gameTime());
            guest_call(kHudPanel, at<uint32_t>(at<uint32_t>(kCurrentCamera) + 0xacu), 1);
            guest_call(kWeaponRenderAll, 1);
            call(kRdrAlphaAddToDmaChain);
            call(kApplyDefaultState);
            call(kVfxRender);
            call(kApplyDefaultState);
        }
        call(kObjectResetAllTint);
        SetFrameWindow(2, static_cast<tTime>(gameTime()));
        guest_call(kObjectSfxDoAll, splitScreen(), gameTime());
        guest_call(kRevSfxUpdateAmbient, splitScreen());
        guest_call(kRevSfxPauseFade, 0);
        call(kSfxDoSfx);
        call(kRCMusicFadeControl);
        if (splitScreen() != 0)
            SetFrameWindow(2, static_cast<tTime>(gameTime()));
        if (status() != 0x10)
        {
            if (CheckPads(noHuman()) == 0)
                PauseMenuProcess(ptr32<mainClass>(kMain));
        }
        if (status() == 0x10 && guest_call<uint32_t>(kInputTestAnyKeyHeld) != 0)
            MainDemoEndTask(ptr32<void>(at<uint32_t>(kMain + 0x388u)), static_cast<tTime>(at<uint32_t>(kMain + 0x380u)));
        countFrames(time, true);
        guest_call(kDisplayRenderFrameProfile, splitScreen());
        if (at<uint32_t>(kTimerResumed) == 0)
        {
            at<uint32_t>(kTimerResumed) = 1;
            if ((at<uint32_t>(kTimer) & 1u) != 0 && status() != 4)
                guest_call(kTimerResume, kTimer);
        }
        call(kGfxP2RenderDmaSend);
        if (at<uint32_t>(0x1ee7e0u + 0xcu) & 0x800u)
            status() = 8;
        call(kRdrEndFrame);
        frames() = frames() + 1;
    } while (status() == 1 || status() == 0x20 || status() == 0x10);
    call(kSfxDoSfx);
    if (status() != 2)
        MainScreenGameInit(0);
    call(kInputClearVibrationVals);
    guest_call(kPlayerSetVibrate, 0, 0, 0);
    guest_call(kPlayerSetVibrate, 0, 0, 1);
    fadeMusicOut();
    guest_call(kRevSfxStopAll, status() == 2 ? 0 : 1);
    // Up to 30 frames for both pads to report in.
    for (int32_t count = 0; (at<uint32_t>(0x1f4c80u + 0x100u) == 0 || at<uint32_t>(0x1f4c80u + 0x104u) == 0) && count < 0x1e; ++count)
    {
        guest_call(kSceGsSyncV, 0);
        call(kInputSnapshot);
    }
}

void MainGameLevelTerm()
{
    {
        GuestFrame frame(0x10);
        call(kAnimLvlTerm);
        SaveChampionshipPoints();
        for (const uint32_t f : {kHudDelete, kWeaponTerm, kObjectDeleteAll, kPickupDeleteAll, kVfxLvlTerm, kVfx2TSFXLvlTerm, kRdrParticleTerm,
                                 kRdrPolyTerm, kRdrAlphaTerm, kGameTaskLvlTerm, kTaskTerm, kCameraLvlTerm, kAColLvlTerm, kOColLvlTerm,
                                 kTColLvlTerm})
            call(f);
    }
    call(kCarColLvlTerm);
}

void MainGameTerm()
{
    {
        GuestFrame frame(0x30);
        call(kRdrShapeTerm);
        if (level() != 0x61)
        {
            call(kRdrSkinTerm);
            call(kRdrWaterTerm);
        }
        for (const uint32_t f : {kAINodeTerm, kWheelTerm, kPlayerTerm, kObjectTerm, kVfxTerm, kHudTerm, kPhysicsLevelTerm, kTBoxTerm,
                                 kPickupTerm, kTSOTerm, kLevelTerm})
            call(f);
        if (level() == 0x61)
        {
            call(kTEditFreeTrackAINodes);
            call(kTEditFreeTrackALF);
            call(kTEditLevelTerm);
        }
        for (const uint32_t f : {kAnimTerm, kCameraTerm, kPathTerm, kAColTerm, kOColTerm, kTColTerm, kCarColTerm, kSkinAnimTerm, kGameTaskTerm})
            call(f);
    }
    guest_call(kMemBlockFree, kResHandle);
}

void MainTerm()
{
    {
        GuestFrame frame(0x20);
        for (const uint32_t f : {kCarListTerm, kUserInterfaceTerm, kTEditFreePermTrackStructures, kTexFreeIntermediateResData, kPhysicsTerm})
            call(f);
        guest_call(kMemBlockFree, kResHandle);
        for (const uint32_t f : {kResTerm, kLoadErrorTerm, kCardUnInit, kInputTerm, kSysTimerTerm, kInit3dClean, kDisplayTerm, kBackdropTerm,
                                 kMemBlockTerm})
            call(f);
    }
    guest_call(kPrintf, 0x323638u);
}

// The three windows: player 1's (full screen, or the top half when split), player 2's (the bottom half) and the
// overlay's.
void MainScreenGameInit(int32_t i)
{
    {
        GuestFrame frame(0xa0);
        if (i == 0)
            defineWindow(frame, kWindow0, -0x78, 0xa0, 0x78, 0xa0, 0x78);
        else
        {
            defineWindow(frame, kWindow0, -0x3c, 0xa0, 0x3c, 0xa0, 0x3c);
            defineWindow(frame, kWindow1, -0x3c, 0xa0, 0x3c, 0xa0, 0xb4);
            defineWindow(frame, kWindow2, -0x78, 0xa0, 0x78, 0xa0, 0x78);
            guest_call(kInit3dWindows, kWindow0, 0);
            guest_call(kInit3dWindows, kWindow1, 1);
        }
    }
    guest_call(kInit3dWindows, i == 0 ? kWindow0 : kWindow2, i == 0 ? 0 : 2);
}

// Makes a window current: its camera, the 3D frame, the scissor for its half of the screen and the 2D offset.
void SetFrameWindow(int32_t windowId, tTime time)
{
    {
        GuestFrame frame(0x20);
        if (windowId != 2)
        {
            at<uint32_t>(kFrameMode) = 0;
            at<uint32_t>(kFrameSplit) = 0;
            at<uint32_t>(kCurrentCamera) = kCamera + static_cast<uint32_t>(windowId) * 0x5b8u;
        }
        const auto send = [&](uint32_t mode)
        {
            at<uint32_t>(kFrameMode) = mode;
            call(kGfxP2RenderDmaSend);
        };
        if (windowId == 0)
        {
            guest_call(kInit3dFrame, kWindow0, at<uint32_t>(kCurrentCamera) + 0x24u, time, splitScreen());
            if (splitScreen() != 0)
            {
                guest_call(kRdrStateSetScissor, 0, 3, 3, 0x27d, 0x6f);
                at<uint32_t>(kFrameSplit) = 1;
                send(0xc2700000u); // -60.0f
            }
            else
            {
                guest_call(kRdrStateSetScissor, 0, 3, 3, 0x27d, 0xed);
                at<uint32_t>(kFrameMode) = 0;
                at<uint32_t>(kFrameSplit) = 0;
                call(kGfxP2RenderDmaSend);
            }
        }
        else if (windowId == 1)
        {
            guest_call(kInit3dFrame2, kWindow1, at<uint32_t>(kCurrentCamera) + 0x24u, time, 0);
            guest_call(kRdrStateSetScissor, 0, 3, 0x70, 0x27d, 0xed);
            at<uint32_t>(kFrameSplit) = 2;
            send(0x42700000u); // 60.0f
        }
        else
        {
            guest_call(kRdrStateSetScissor, 0, 3, 3, 0x27d, 0xed);
            at<uint32_t>(kFrameMode) = 0;
            at<uint32_t>(kFrameSplit) = 0;
            call(kGfxP2RenderDmaSend);
        }
    }
    call(kApplyDefaultState);
}

} // namespace rc

RC_DECOMP_FN_BLOCKING(MainInit, 0x12f080)           // Once, at start-up.
RC_DECOMP_FN_BLOCKING(MainFrontInit, 0x12f3a8)      // Front end set-up: loads and plays music.
RC_DECOMP_FN_BLOCKING(MainFrontProcess, 0x12f500)   // The front end's frame (FrontEndProcess).
RC_DECOMP_FN_BLOCKING(MainFrontTerm, 0x12f508)
RC_DECOMP_FN_BLOCKING(MainGameInit, 0x12fa38)       // Loads the level.
RC_DECOMP_FN_BLOCKING(MainGameLevelInit, 0x12fc58)
RC_DECOMP_FN_BLOCKING(MainGameProcess, 0x12fe78)    // The race's frame loop.
RC_DECOMP_FN_BLOCKING(MainGameLevelTerm, 0x130708)
RC_DECOMP_FN_BLOCKING(MainGameTerm, 0x1307a8)
// Not registered: MainTrackEdInit, MainTrackEdProcess and MainTrackEdTerm (the track editor, reached only through the
// menus, so no run checks them) and MainTerm (shut-down; MainLoop never ends). Their code above is unchecked.
RC_DECOMP_FN_SETUP_HEAP(MainScreenGameInit, 0x130960, rc::fuzz::mainScreen)
RC_DECOMP_FN_SETUP_HEAP(SetFrameWindow, 0x130b10, rc::fuzz::mainFrameWindow)
