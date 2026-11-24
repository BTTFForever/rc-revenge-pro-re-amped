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
#include "../rc_frame_tick.h"
#include "../rc_front_interp.h"
#include "../rc_split.h"
#include <cstdlib>
#include "../rc_cars.h"

RC_VERSION_VARIANT(printf) // Europe also stores $f12-$f18
// Europe's MainGameProcess is 8 bytes longer only from code generation: the end-of-frame "timer resumed" block stores
// iDrawDownG = 1 on each path instead of in one delay slot (same effect) and an alignment nop moved. The native race
// loop (and with it the race frame rate and smooth motion hooks) runs in both versions; its callees that differ in
// Europe (SetFrameWindow, MainScreenGameInit, CheckPads) divert to Europe's own functions.
RC_VERSION_VARIANT(MainGameProcess)
// The PAL screen (256 lines; NOTES.md "Europe version"): MainInit (the 3D window and Init3d's second argument 0xe6
// instead of 0x111), MainScreenGameInit (the windows; the split screen's bottom half 0x3f high), SetFrameWindow (the
// scissors and the split screen's offsets 64 instead of 60).
RC_VERSION_VARIANT(MainInit)
RC_VERSION_VARIANT(MainScreenGameInit)
RC_VERSION_VARIANT(SetFrameWindow)

namespace rc {

void RcInterpBeginDraw(int32_t time); // gametask.cpp
void RcInterpEndDraw();
void RcInterpBeginCamera(int32_t time);
void RcRefreshRace(bool racing);


using namespace mainunit;

namespace {

// Other guest functions these call (native ones too: the frame loops reach them through the dispatcher).
constexpr uint32_t kMemBlockInit = sym::MemBlockInit, kMemBlockAllocHi = sym::MemBlockAllocHi, kMemBlockFree = sym::MemBlockFree, kMemBlockTerm = sym::MemBlockTerm;
constexpr uint32_t kDisplayInit = sym::DisplayInit, kDisplaySetBackground = sym::DisplaySetBackground, kDisplayDisable = sym::DisplayDisable, kDisplayEnable = sym::DisplayEnable;
constexpr uint32_t kDisplayTerm = sym::DisplayTerm, kDisplayRenderFrameProfile = sym::DisplayRenderFrameProfile, kSysTimerInit = sym::SysTimerInit, kSysTimerTerm = sym::SysTimerTerm;
constexpr uint32_t kInputInit = sym::InputInit, kInputTerm = sym::InputTerm, kCardConstruct = sym::CardConstruct, kCardInit = sym::CardInit, kCardUnInit = sym::CardUnInit;
constexpr uint32_t kFMInit = sym::FMInit, kFMPrepareHi = sym::FMPrepareHi, kFMUnPrepare = sym::FMUnPrepare, kBackdropInit = sym::BackdropInit;
constexpr uint32_t kBackdropDoInitialScreens = sym::BackdropDoInitialScreens, kBackdropTerm = sym::BackdropTerm, kSfxInitFirst = sym::SfxInitFirst, kExit = sym::exit;
constexpr uint32_t kLoadErrorInit = sym::LoadErrorInit, kLoadErrorTerm = sym::LoadErrorTerm, kResInit = sym::ResInit, kResTerm = sym::ResTerm;
constexpr uint32_t kTEditInitPermTrackStructures = sym::TEditInitPermTrackStructures, kTEditAllocPermTrackStructures = sym::TEditAllocPermTrackStructures;
constexpr uint32_t kTEditFreePermTrackStructures = sym::TEditFreePermTrackStructures, kPhysicsInit = sym::PhysicsInit, kPhysicsTerm = sym::PhysicsTerm, kSfxInit = sym::SfxInit;
constexpr uint32_t kRevSfxInit = sym::RevSfxInit, kSfxOn = sym::SfxOn, kRCMusicInit = sym::RCMusicInit, kSfxInitReverb = sym::SfxInitReverb;
constexpr uint32_t kTSOSetSFXCallBack = sym::TSOSetSFXCallBack, kRevSfxTSO = sym::RevSfxTSO, kInit3d = sym::Init3d, kInit3dClean = sym::Init3dClean;
constexpr uint32_t kView3dDefine3dDeviceWindow = sym::View3dDefine3dDeviceWindow, kInit3dWindows = sym::Init3dWindows, kUserInterfaceInit = sym::UserInterfaceInit;
constexpr uint32_t kUserInterfaceTerm = sym::UserInterfaceTerm, kCarListInit = sym::CarListInit, kCarListTerm = sym::CarListTerm, kClearAllSecrets = sym::ClearAllSecrets;
constexpr uint32_t kLoadingInit = sym::LoadingInit, kMtlInit = sym::MtlInit, kAColInit = sym::AColInit, kOColInit = sym::OColInit, kTColInit = sym::TColInit;
constexpr uint32_t kCarColInit = sym::CarColInit, kGameTaskInit = sym::GameTaskInit, kCameraInit = sym::CameraInit, kAnimInit = sym::AnimInit;
constexpr uint32_t kFrontEndInit = sym::FrontEndInit, kFrontEndProcess = sym::FrontEndProcess, kFrontEndTerm = sym::FrontEndTerm, kRdrPolyInit = sym::RdrPolyInit;
constexpr uint32_t kRdrAlphaInit = sym::RdrAlphaInit, kVfxLvlInit = sym::VfxLvlInit, kVfx2TSFXLvlInit = sym::Vfx2TSFXLvlInit, kMeshSetQuality = sym::MeshSetQuality;
constexpr uint32_t kMirrorSet = sym::MirrorSet, kMpegOnInterruptTERM = sym::mpegOnInterruptTERM, kAnimTerm = sym::AnimTerm, kCameraTerm = sym::CameraTerm;
constexpr uint32_t kAColTerm = sym::AColTerm, kOColTerm = sym::OColTerm, kTColTerm = sym::TColTerm, kCarColTerm = sym::CarColTerm, kGameTaskTerm = sym::GameTaskTerm;
constexpr uint32_t kVfxLvlTerm = sym::VfxLvlTerm, kVfx2TSFXLvlTerm = sym::Vfx2TSFXLvlTerm, kRdrPolyTerm = sym::RdrPolyTerm, kRdrAlphaTerm = sym::RdrAlphaTerm;
constexpr uint32_t kLevelInit = sym::LevelInit, kLevelLoad = sym::LevelLoad, kLevelTerm = sym::LevelTerm, kTEditInit = sym::TEditInit, kTEditTerm = sym::TEditTerm;
constexpr uint32_t kCameraLvlInit = sym::CameraLvlInit, kCameraSetMode = sym::CameraSetMode, kCameraProcess = sym::CameraProcess, kRdrStartFrame = sym::Rdr_StartFrame;
constexpr uint32_t kApplyDefaultState = sym::RdrState_ApplyDefaultStateBlock, kGfxP2ZWriteEnable = sym::GfxP2_ZWriteEnable, kTEditControl = sym::TEditControl, kInit3dFrame = sym::Init3dFrame;
constexpr uint32_t kInit3dFrame2 = sym::Init3dFrame2, kTileFrameInit = sym::TileFrameInit, kTEditDisplay = sym::TEditDisplay, kFontPrint = sym::FontPrint;
constexpr uint32_t kRdrAlphaAddToDmaChain = sym::RdrAlphaAddToDmaChain, kRdrEndFrame = sym::Rdr_EndFrame, kSkinAnimInit = sym::SkinAnimInit, kObjectInit = sym::ObjectInit;
constexpr uint32_t kPlayerInit = sym::PlayerInit, kAIInit = sym::AIInit, kAINodeInit = sym::AINodeInit, kWheelInit = sym::WheelInit, kVfxInit = sym::VfxInit;
constexpr uint32_t kHudInit = sym::HudInit, kPhysicsLevelInit = sym::PhysicsLevelInit, kTSOInit = sym::TSOInit, kTBoxInit = sym::TBoxInit, kPickupInit = sym::PickupInit;
constexpr uint32_t kPathInit = sym::PathInit, kRdrShapeInit = sym::Rdr_ShapeInit, kRdrSkinInit = sym::Rdr_SkinInit, kRdrWaterInit = sym::Rdr_WaterInit;
constexpr uint32_t kVSyncCallback = sym::VSyncCallback, kTaskInit = sym::TaskInit, kAColLvlInit = sym::AColLvlInit, kOColLvlInit = sym::OColLvlInit;
constexpr uint32_t kTColLvlInit = sym::TColLvlInit, kCarColLvlInit = sym::CarColLvlInit, kRdrParticleInit = sym::RdrParticleInit, kDepthCueInit = sym::DepthCueInit;
constexpr uint32_t kObjectCreateCars = sym::ObjectCreateCars, kAISetupCars = sym::AISetupCars, kGameTaskLvlInit = sym::GameTaskLvlInit, kWeaponInit = sym::WeaponInit;
constexpr uint32_t kDepthCueSet = sym::DepthCueSet, kAnimReinit = sym::AnimReinit, kRandomSetSeed = sym::RandomSetSeed, kPickupAllSpawn = sym::PickupAllSpawn;
constexpr uint32_t kHudCreate = sym::HudCreate, kMenuInitScan = sym::MenuInitScan, kFontSet = sym::FontSet, kTEditGetSoundTrackNdxForTrack = sym::TEditGetSoundTrackNdxForTrack;
constexpr uint32_t kVertAnimFrameUpdate = sym::VertAnimFrameUpdate, kTaskProcess = sym::TaskProcess, kTBoxProcess = sym::TBoxProcess, kRdrParticleProcess = sym::RdrParticleProcess;
constexpr uint32_t kMeshInitFrame = sym::MeshInitFrame, kInit3dChangeViewcone = sym::Init3dChangeViewcone, kLsFrameInit = sym::LsFrameInit, kGfxP2Tex = sym::GfxP2_Tex;
constexpr uint32_t kGfxP2RenderDmaSend = sym::GfxP2_RenderDmaSend, kAIDoRaceOrder = sym::AIDoRaceOrder, kMemset = sym::memset, kLsRenderWorld = sym::LsRenderWorld;
constexpr uint32_t kTEditRenderGameTrack = sym::TEditRenderGameTrack, kVfxRenderShadows = sym::VfxRenderShadows, kObjectRenderAll = sym::ObjectRenderAll;
constexpr uint32_t kVertAnimRender = sym::VertAnimRender, kTSORender = sym::TSORender, kPickupRenderAll = sym::PickupRenderAll, kHudPanel = sym::HudPanel;
constexpr uint32_t kWeaponRenderAll = sym::WeaponRenderAll, kVfxRender = sym::VfxRender, kObjectResetAllTint = sym::ObjectResetAllTint, kObjectSfxDoAll = sym::ObjectSfxDoAll;
constexpr uint32_t kPlayerSetVibrate = sym::PlayerSetVibrate, kRdrStateSetScissor = sym::RdrState_SetScissor, kAnimLvlTerm = sym::AnimLvlTerm, kHudDelete = sym::HudDelete;
constexpr uint32_t kWeaponTerm = sym::WeaponTerm, kObjectDeleteAll = sym::ObjectDeleteAll, kPickupDeleteAll = sym::PickupDeleteAll, kRdrParticleTerm = sym::RdrParticleTerm;
constexpr uint32_t kGameTaskLvlTerm = sym::GameTaskLvlTerm, kTaskTerm = sym::TaskTerm, kCameraLvlTerm = sym::CameraLvlTerm, kAColLvlTerm = sym::AColLvlTerm;
constexpr uint32_t kOColLvlTerm = sym::OColLvlTerm, kTColLvlTerm = sym::TColLvlTerm, kCarColLvlTerm = sym::CarColLvlTerm, kRdrShapeTerm = sym::Rdr_ShapeTerm;
constexpr uint32_t kRdrSkinTerm = sym::Rdr_SkinTerm, kRdrWaterTerm = sym::Rdr_WaterTerm, kAINodeTerm = sym::AINodeTerm, kWheelTerm = sym::WheelTerm;
constexpr uint32_t kPlayerTerm = sym::PlayerTerm, kObjectTerm = sym::ObjectTerm, kVfxTerm = sym::VfxTerm, kHudTerm = sym::HudTerm;
constexpr uint32_t kPhysicsLevelTerm = sym::PhysicsLevelTerm, kTBoxTerm = sym::TBoxTerm, kPickupTerm = sym::PickupTerm, kTSOTerm = sym::TSOTerm;
constexpr uint32_t kTEditFreeTrackAINodes = sym::TEditFreeTrackAINodes, kTEditFreeTrackALF = sym::TEditFreeTrackALF, kTEditLevelTerm = sym::TEditLevelTerm, kPathTerm = sym::PathTerm;
constexpr uint32_t kSkinAnimTerm = sym::SkinAnimTerm, kTexFreeIntermediateResData = sym::TexFreeIntermediateResData;

constexpr uint32_t kTimer = kMain + 0x10u, kWindow0 = kMain + 0x20u, kWindow1 = kMain + 0x8cu, kWindow2 = kMain + 0xf8u;
constexpr uint32_t kResHandle = kMain + 0x164u, kCurrentCamera = kCamera + 0xb70u, kFrameMode = sym::gRdrPolyOffset, kFrameSplit = sym::gRdrPolyPlayer;

void call(uint32_t address) { guest_call(address); }

uint8_t splitScreen() { return at<uint8_t>(kMain + 0x368u); }
uint8_t level() { return at<uint8_t>(kMain + 0x34au); }
uint32_t &frames() { return at<uint32_t>(kMain + 0x354u); }
uint32_t &frameTime() { return at<uint32_t>(kMain + 0x358u); }
uint32_t &gameTime() { return at<uint32_t>(kMain + 0x1cu); }

// Side tag: the view the frame loop renders next, right after SetFrameWindow made it current (its camera is
// gRdr.pCamera; the scissor is the one SetFrameWindow set, the vertical offset gRdrPolyOffset). Reads only.
void viewTag(uint32_t index)
{
    if (!rc::gfx::live())
        return;
    rc::gfx::ViewTag tag{};
    tag.index = index;
    const int32_t isSplit = splitScreen() != 0 ? 1 : 0;
    tag.scissor[0] = 3;
    tag.scissor[1] = isSplit && index == 1 ? 0x70 : 3;
    tag.scissor[2] = 0x27d;
    tag.scissor[3] = isSplit && index == 0 ? 0x6f : 0xed;
    int x1, y1, x2, y2;
    if (isSplit && split::scissor(static_cast<int>(index), x1, y1, x2, y2)) // vertical split screen
    {
        tag.scissor[0] = x1;
        tag.scissor[1] = y1;
        tag.scissor[2] = x2;
        tag.scissor[3] = y2;
    }
    std::memcpy(&tag.yOffset, guest_host(kFrameMode), 4);
    const uint32_t camera = G(gRdr).pCamera.address;
    tag.camera = camera;
    if (camera)
    {
        const _sRdrCamera &c = *G(gRdr).pCamera;
        std::memcpy(tag.position, &c.pos, sizeof(tag.position));
        std::memcpy(tag.rotation, &c.mat, sizeof(tag.rotation));
        std::memcpy(&tag.dVal, &c.dVal, 4);
        std::memcpy(&tag.dValX, &c.dValX, 4);
        std::memcpy(&tag.dValY, &c.dValY, 4);
        std::memcpy(&tag.minZ, &c.minZ, 4);
        std::memcpy(&tag.maxZ, &c.maxZ, 4);
        tag.viewAngle = c.viewAngle;
    }
    rc::gfx::view(tag);
}

// View3dDefine3dDeviceWindow(window, -160, -120, 160, 120, 160, 120, -160, then five on the stack): the
// window's place on the screen (its last five arguments, as words in the stack's 8-byte slots).
void defineWindow(const GuestFrame &frame, uint32_t window, int32_t s0, int32_t s8, int32_t s10, int32_t s18, int32_t s20)
{
    at<int32_t>(frame.local<uint8_t>(0).address) = s0;
    at<int32_t>(frame.local<uint8_t>(8).address) = s8;
    at<int32_t>(frame.local<uint8_t>(0x10).address) = s10;
    at<int32_t>(frame.local<uint8_t>(0x18).address) = s18;
    at<int32_t>(frame.local<uint8_t>(0x20).address) = s20;
    constexpr int32_t half = kScreenLines / 2; // 0x78, in Europe 0x80
    guest_call(kView3dDefine3dDeviceWindow, window, -0xa0, -half, 0xa0, half, 0xa0, half, -0xa0);
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
        guest_call(kPrintf, (sym::sec_rodata + 0x1028u), count);
    frames() = 0;
}

} // namespace

void MainInit()
{
    GuestFrame frame(0x80);
    guest_call(kPrintf, (sym::sec_rodata + 0xda0u));
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
    guest_call(kResInit, (sym::sec_sdata + 0x2f0u), at<uint32_t>(kResHandle), 0x1194000);
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
    if (guest_call<uint32_t>(kInit3d, 0x3436, RC_VERSION_EU ? 0xe6 : 0x111, 5, 0x2000, 0x7d0, 0x2328, 0x25800) == 0)
    {
        call(kInit3dClean);
        guest_call(kExit, 1);
    }
    defineWindow(frame, kWindow0, -kScreenLines / 2, 0xa0, kScreenLines / 2, 0xa0, kScreenLines / 2);
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
    cars::forgetCount();
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
        else if (at<uint32_t>(sym::firstEntryG) != 0 && at<uint32_t>(sym::memoryCardDontCareG) == 1) // firstEntryG, memoryCardDontCareG
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
    cars::forgetCount();
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
        guest_call(kFontPrint, 0x294, 0x2e, (sym::sec_sdata + 0x2f8u));
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
    cars::restoreCount(); // enhancement: a race that dropped cars for GS memory (rc_cars.h) gives the next level its count back
    {
        GuestFrame frame(0x20);
        for (const uint32_t f : {kMtlInit, kAColInit, kOColInit, kTColInit, kCarColInit, kGameTaskInit, kCameraInit, kAnimInit, kRCMusicStop})
            call(f);
        guest_call(kPrintf, (sym::sec_rodata + 0xdb8u));
        call(kSkinAnimInit);
        guest_call(kPrintf, (sym::sec_rodata + 0xdd8u));
        call(kObjectInit);
        guest_call(kPrintf, (sym::sec_rodata + 0xdf0u));
        guest_call(kPlayerInit, noHuman());
        guest_call(kPrintf, (sym::sec_rodata + 0xe08u));
        guest_call(kAIInit, noAI());
        guest_call(kPrintf, (sym::sec_rodata + 0xe20u));
        call(kAINodeInit);
        guest_call(kPrintf, (sym::sec_rodata + 0xe38u));
        call(kWheelInit);
        guest_call(kPrintf, (sym::sec_rodata + 0xe50u));
        call(kVfxInit);
        guest_call(kPrintf, (sym::sec_rodata + 0xe68u));
        call(kHudInit);
        guest_call(kPrintf, (sym::sec_rodata + 0xe80u));
        call(kPhysicsLevelInit);
        guest_call(kPrintf, (sym::sec_rodata + 0xe98u));
        call(kLevelInit);
        guest_call(kPrintf, (sym::sec_rodata + 0xeb8u));
        call(kTSOInit);
        guest_call(kPrintf, (sym::sec_rodata + 0xee0u));
        call(kTBoxInit);
        guest_call(kPrintf, (sym::sec_rodata + 0xf00u));
        call(kPickupInit);
        guest_call(kMirrorSet, at<uint32_t>(kMain + 0x398u));
        guest_call(kPrintf, (sym::sec_rodata + 0xf18u));
        guest_call(kRevSfxInitAmbient, 1);
        call(kPathInit);
        guest_call(kPrintf, (sym::sec_rodata + 0xf30u));
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
        guest_call(kVSyncCallback, sym::VSyncCallbackFunction);
        call(kTaskInit);
        guest_call(kPrintf, (sym::sec_rodata + 0xf48u));
        guest_call(kTimerSetup, kTimer, 0, 1);
        gameTime() = 0;
        at<uint32_t>(kMain + 0x39cu) = 1;
        at<uint16_t>(kMain + 0x366u) = 0;
        for (const uint32_t f : {kAColLvlInit, kOColLvlInit, kTColLvlInit, kCarColLvlInit, kRdrParticleInit, kRdrPolyInit, kRdrAlphaInit,
                                 kVfxLvlInit, kDepthCueInit})
            call(f);
        guest_call(kObjectCreateCars, gameTime());
        guest_call(kPrintf, (sym::sec_rodata + 0xf60u));
        call(kAISetupCars);
        guest_call(kPrintf, (sym::sec_rodata + 0xf78u));
        guest_call(kCameraLvlInit, splitScreen());
        guest_call(kPrintf, (sym::sec_rodata + 0xf98u));
        call(kGameTaskLvlInit);
        guest_call(kPrintf, (sym::sec_rodata + 0xfb8u));
        call(kWeaponInit);
        guest_call(kPrintf, (sym::sec_rodata + 0xfd0u));
        guest_call(kDepthCueSet, 0x61a8, splitScreen());
        guest_call(kMeshSetQuality, splitScreen() == 0 ? 2 : 0);
        call(kAnimReinit);
        guest_call(kRandomSetSeed, at<uint32_t>((sym::sysTimerVarG + 0x8u)));
        if (at<uint32_t>(kMain + 0x3a8u) != 0)
        {
            guest_call(kPrintf, (sym::sec_rodata + 0xff0u));
            guest_call(kPickupAllSpawn, 0);
        }
        call(kHudCreate);
        call(kVfx2TSFXLvlInit);
        PauseMenuScan();
        guest_call(kMenuInitScan, at<uint32_t>(at<uint32_t>(kLevel) + 0x44u), 1);
        cars::extendResultMenus(); // enhancement: lines for 9th and on (rc_cars.h)
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
        guest_call(kPrintf, (sym::sec_rodata + 0x1008u));
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
    RcRefreshRace(true); // enhancement: the frame rate setting (Game Options > Display, PS2X_REFRESH_HZ)
    frontinterp::k1Forget(1);
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
    // Enhancement: above the video rate, the loop's per-frame logic (input, sound and music steps, the pause and results
    // menus, rain and glint ageing, tint steps, animated textures, the frame counter) runs on video-rate ticks
    // (rc_frame_tick.h); the passes between them draw the race (blended by smooth motion) and send the tick's pause
    // menu again. The physics keeps its own 20 ms clock either way. RC_FRAME_TICK=0: every pass steps, as before.
    std::vector<uint32_t> tintHold; // the objects' current tints, kept through a pass between ticks
    const auto holdTints = [&tintHold]()
    {
        tintHold.clear();
        for (uint32_t i = 0; i < at<uint32_t>(sym::objectVarG + 0x170u); ++i)
            if (const uint32_t obj = at<uint32_t>(sym::objectVarG + 4u * i))
            {
                tintHold.push_back(obj);
                tintHold.push_back(at<uint32_t>(obj + 4u));
            }
    };
    const auto releaseTints = [&tintHold]()
    {
        for (size_t k = 0; k + 1 < tintHold.size(); k += 2)
            at<uint32_t>(tintHold[k] + 4u) = tintHold[k + 1];
        tintHold.clear();
    };
    do
    {
        const bool logic = frametick::beginPass();
        frametick::logSecond();
        call(kRdrStartFrame);
        call(kApplyDefaultState);
        if (logic)
            call(kInputSnapshot);
        const uint32_t time = guest_call<uint32_t>(kTimerGet, kTimer);
        guest_call(kVertAnimFrameUpdate, time);
        gameTime() = time;
        guest_call(kTaskProcess, time);
        RcInterpBeginCamera(static_cast<int32_t>(time)); // enhancement: the frame's cameras between physics steps
        SetFrameWindow(0, static_cast<tTime>(gameTime()));
        call(kTBoxProcess);
        if (logic)
            call(kRdrParticleProcess);
        call(kMeshInitFrame);
        guest_call(kInit3dChangeViewcone, at<uint16_t>(kCamera + 0x574u));
        guest_call(kLsFrameInit, gameTime(), splitScreen(), 0);
        guest_call(kGfxP2Tex, at<uint32_t>(at<uint32_t>((sym::visFxVarG + 0x6cu)) + 0x60u));
        call(kGfxP2RenderDmaSend);
        call(kAIDoRaceOrder);
        RcInterpBeginDraw(static_cast<int32_t>(time)); // enhancement: cars and cameras drawn between physics steps
        if (!logic)
            holdTints(); // the draws step each object's tint towards its target: once per tick
        guest_call(kMemset, temp, 0, 0xc);
        {
            const uint32_t t = gameTime();
            at<uint32_t>(kMain + 0x378u) = 0;
            SetFrameWindow(0, static_cast<tTime>(t));
        }
        viewTag(0);
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
            viewTag(1);
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
        if (logic)
            call(kObjectResetAllTint);
        else
            releaseTints();
        RcInterpEndDraw();
        SetFrameWindow(2, static_cast<tTime>(gameTime()));
        if (logic)
        {
            guest_call(kObjectSfxDoAll, splitScreen(), gameTime());
            guest_call(kRevSfxUpdateAmbient, splitScreen());
            guest_call(kRevSfxPauseFade, 0);
            call(kSfxDoSfx);
            call(kRCMusicFadeControl);
        }
        if (splitScreen() != 0)
            SetFrameWindow(2, static_cast<tTime>(gameTime()));
        if (status() != 0x10)
        {
            if (logic)
            {
                const bool keep2d = frametick::fast();
                if (keep2d)
                    frontinterp::k1Begin(1); // kept for the passes between ticks
                if (CheckPads(noHuman()) == 0)
                    PauseMenuProcess(ptr32<mainClass>(kMain));
                if (keep2d)
                    frontinterp::k1End(1);
            }
            else
                frontinterp::k1Replay(1);
        }
        if (logic && status() == 0x10 && guest_call<uint32_t>(kInputTestAnyKeyHeld) != 0)
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
        if (at<uint32_t>(sym::playerVarG + 0xcu) & 0x800u)
            status() = 8;
        call(kRdrEndFrame);
        frames() = frames() + 1;
    } while (status() == 1 || status() == 0x20 || status() == 0x10);
    RcRefreshRace(false);
    call(kSfxDoSfx);
    if (status() != 2)
        MainScreenGameInit(0);
    call(kInputClearVibrationVals);
    guest_call(kPlayerSetVibrate, 0, 0, 0);
    guest_call(kPlayerSetVibrate, 0, 0, 1);
    fadeMusicOut();
    guest_call(kRevSfxStopAll, status() == 2 ? 0 : 1);
    // Up to 30 frames for both pads to report in.
    for (int32_t count = 0; (at<uint32_t>(sym::inputVarG + 0x100u) == 0 || at<uint32_t>(sym::inputVarG + 0x104u) == 0) && count < 0x1e; ++count)
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
    guest_call(kPrintf, (sym::sec_rodata + 0x1038u));
}

// The three windows: player 1's (full screen, or the top half when split), player 2's (the bottom half) and the
// overlay's.
void MainScreenGameInit(int32_t i)
{
    {
        GuestFrame frame(RC_VERSION_EU ? 0x90 : 0xa0);
        if (i == 0)
            defineWindow(frame, kWindow0, -kScreenLines / 2, 0xa0, kScreenLines / 2, 0xa0, kScreenLines / 2);
        else
        {
#if RC_VERSION_EU
            // Europe: halves of a 256-line screen; the bottom one's height is 0x3f, not 0x40.
            defineWindow(frame, kWindow0, -0x40, 0xa0, 0x40, 0xa0, 0x40);
            defineWindow(frame, kWindow1, -0x40, 0xa0, 0x3f, 0xa0, 0xc0);
            defineWindow(frame, kWindow2, -0x80, 0xa0, 0x80, 0xa0, 0x80);
#else
            defineWindow(frame, kWindow0, -0x3c, 0xa0, 0x3c, 0xa0, 0x3c);
            defineWindow(frame, kWindow1, -0x3c, 0xa0, 0x3c, 0xa0, 0xb4);
            defineWindow(frame, kWindow2, -0x78, 0xa0, 0x78, 0xa0, 0x78);
#endif
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
        split::frameWindow(windowId); // enhancement: vertical split screen (rc_split.h)
        int sx1, sy1, sx2, sy2;
        const bool sideBySide = split::scissor(windowId, sx1, sy1, sx2, sy2);
        if (windowId == 0)
        {
            guest_call(kInit3dFrame, kWindow0, at<uint32_t>(kCurrentCamera) + 0x24u, time, splitScreen());
            if (splitScreen() != 0)
            {
                if (sideBySide) // the left half, no vertical offset
                    guest_call(kRdrStateSetScissor, 0, sx1, sy1, sx2, sy2);
                else
                    guest_call(kRdrStateSetScissor, 0, 3, 3, 0x27d, RC_VERSION_EU ? 0x7f : 0x6f);
                at<uint32_t>(kFrameSplit) = 1;
                send(sideBySide ? 0u : RC_VERSION_EU ? 0xc2800000u : 0xc2700000u); // -64.0f, USA -60.0f
            }
            else
            {
                guest_call(kRdrStateSetScissor, 0, 3, 3, 0x27d, kScreenLines - 3);
                at<uint32_t>(kFrameMode) = 0;
                at<uint32_t>(kFrameSplit) = 0;
                call(kGfxP2RenderDmaSend);
            }
        }
        else if (windowId == 1)
        {
            guest_call(kInit3dFrame2, kWindow1, at<uint32_t>(kCurrentCamera) + 0x24u, time, 0);
            if (sideBySide) // the right half
                guest_call(kRdrStateSetScissor, 0, sx1, sy1, sx2, sy2);
            else
                guest_call(kRdrStateSetScissor, 0, 3, RC_VERSION_EU ? 0x80 : 0x70, 0x27d, kScreenLines - 3);
            at<uint32_t>(kFrameSplit) = 2;
            send(sideBySide ? 0u : RC_VERSION_EU ? 0x42800000u : 0x42700000u); // 64.0f, USA 60.0f
        }
        else
        {
            guest_call(kRdrStateSetScissor, 0, 3, 3, 0x27d, kScreenLines - 3);
            at<uint32_t>(kFrameMode) = 0;
            at<uint32_t>(kFrameSplit) = 0;
            call(kGfxP2RenderDmaSend);
        }
    }
    call(kApplyDefaultState);
}

// The game's own printf (MAIN.c) prints nothing: its body was compiled out, leaving a variadic function that stores its
// variable arguments ($a1-$t3 as doublewords, and in the Europe version also $f12, $f14, $f16 and $f18) into its 0x80-byte
// frame and returns ($v0 untouched). The recompiled build replaces it with the runtime's printf, which formats the text
// (shown only in logging builds) and returns its length; the native one does what the game's does.
void printf()
{
    const R5900Context &ctx = g_runtime->eeScheduler().runningContext();
    const uint32_t frame = getRegU32(&ctx, 29) - 0x80u;
    for (int r = 5; r <= 11; ++r)
    {
        const uint64_t value = GPR_U64((&ctx), r);
        std::memcpy(guest_host(frame + 0x48u + 8u * static_cast<uint32_t>(r - 5)), &value, 8);
    }
#if RC_VERSION_EU
    for (uint32_t i = 0; i < 4; ++i)
        std::memcpy(guest_host(frame + 0x38u + 4u * i), &ctx.f[12 + 2 * i], 4);
#endif
}

// The boot: the IOP rebooted with the game's image and its modules loaded, then the mode loop. MainLoop returns only
// when a mode asks to quit (status 8), which no menu does.
int32_t ps2_main()
{
    GuestFrame frame(0x20);
    guest_call(sym::__main);
    guest_call(sym::sceSifInitRpc, 0);
    guest_call(sym::sceCdInit, 0);
    const uint32_t iopImage = sym::sec_rodata + 0xcb0u; // the IOP's image on the disc
    while (guest_call<int32_t>(sym::sceSifRebootIop, iopImage) == 0)
        ;
    while (guest_call<int32_t>(sym::sceSifSyncIop) == 0)
        ;
    guest_call(sym::sceSifInitRpc, 0);
    guest_call(sym::sceCdInit, 0);
    guest_call(sym::sceCdMmode, 1);
    guest_call(sym::sceFsReset);
    for (const uint32_t module : {0xcd0u, 0xcf0u, 0xd10u, 0xd28u, 0xd48u, 0xd60u, 0xd80u}) // the IOP modules' names
        guest_call(sym::sceSifLoadModule, sym::sec_rodata + module, 0, 0);
    guest_call(sym::sceCdDiskReady, 0);
    at<uint32_t>(sym::stackStartG) = frame.local<uint8_t>(0).address; // main's $sp
    guest_call(sym::MainLoop);
    return 0;
}

// The mode loop. mainVarG.toDo picks the next mode (1 front end, 2 track editor, 3 a race or a cup, 4 the attract demo)
// and mainVarG.status what a mode ended with (2 restart the level, 4 and 0x40 back to the menus, 8 quit). Every call goes
// through the dispatcher, so the overrides' wrappers (RC_BOOT_LEVEL's MainFrontProcess, MainGameLevelInit) still apply.
void MainLoop()
{
    GuestFrame frame(0xa0);
    at<uint8_t>(kMain + 0x34au) = 3;
    do
    {
        call(sym::MainInit);
        uint32_t mode = toDo();
        for (;;)
        {
            at<uint32_t>(kMain + 8u) = mode; // the mode before
            if (mode == 2)
            {
                call(sym::MainTrackEdInit);
                call(sym::MainTrackEdProcess);
                call(sym::MainTrackEdTerm);
            }
            else if (mode == 1)
            {
                call(sym::MainFrontInit);
                call(sym::MainFrontProcess);
                const uint32_t next = toDo();
                if (next != 2)
                    guest_call(sym::MainSetCars, next == 4 ? 2 : 3);
                call(sym::MainFrontTerm);
                guest_call(kLoadingInit, toDo());
            }
            else if (mode == 3 || mode == 4)
            {
                // A race (each level of a cup, each restart) or the demo, until the menus or a quit.
                uint32_t ended;
                do
                {
                    call(sym::MainGameInit);
                    if (mode == 3)
                    {
                        do
                        {
                            status() = 1;
                            call(sym::MainGameLevelInit);
                            call(sym::MainGameProcess);
                            call(sym::MainGameLevelTerm);
                        } while (status() == 2);
                        call(sym::MainGameTerm);
                        guest_call(kLoadingInit, toDo());
                    }
                    else
                    {
                        call(sym::MainGameLevelInit);
                        // Ends the demo after 30 s. RC_DEMO_SECS=<n> lengthens it (whole-lap test runs with RC_BOOT_LEVEL).
                        static const uint32_t demoMs = [] {
                            const char *v = std::getenv("RC_DEMO_SECS");
                            const int secs = v ? std::atoi(v) : 0;
                            return secs > 0 ? static_cast<uint32_t>(secs) * 1000u : 30000u;
                        }();
                        guest_call(sym::TaskSet, kMain + 0x37cu, sym::MainDemoEndTask, 1, demoMs);
                        guest_call(sym::TaskCachedInsert, kMain + 0x37cu);
                        call(sym::MainGameProcess);
                        call(sym::MainGameLevelTerm);
                        call(sym::MainGameTerm);
                    }
                    ended = status();
                } while (ended != 8 && ended != 4 && ended != 0x40);
            }
            else
                guest_checkpoint(); // no mode: the original spins here (its backward branch is a scheduling point)
            if (status() == 8)
                break;
            mode = toDo();
        }
        call(sym::MainTerm);
    } while (status() != 8);
}

} // namespace rc

RC_DECOMP_FN_BLOCKING(MainInit)           // Once, at start-up.
RC_DECOMP_FN_BLOCKING(MainFrontInit)      // Front end set-up: loads and plays music.
RC_DECOMP_FN_BLOCKING(MainFrontProcess)   // The front end's frame (FrontEndProcess).
RC_DECOMP_FN_BLOCKING(MainFrontTerm)
RC_DECOMP_FN_BLOCKING(MainGameInit)       // Loads the level.
RC_DECOMP_FN_BLOCKING(MainGameLevelInit)
RC_DECOMP_FN_BLOCKING(MainGameProcess)    // The race's frame loop.
RC_DECOMP_FN_BLOCKING(MainGameLevelTerm)
RC_DECOMP_FN_BLOCKING(MainGameTerm)
// The boot and the mode loop, the track editor's set-up, frame loop and tear-down, and the shut-down: offline their
// callees are scripted fakes that log each call and change the mode and status (main_fuzz.cpp).
RC_DECOMP_FN_BLOCKING_SETUP(ps2_main, rc::fuzz::mainBoot)
RC_DECOMP_FN_BLOCKING_SETUP(MainLoop, rc::fuzz::mainLoop)
RC_DECOMP_FN_BLOCKING_SETUP_HEAP(MainTrackEdInit, rc::fuzz::mainTrackEdInit)
RC_DECOMP_FN_BLOCKING_SETUP(MainTrackEdProcess, rc::fuzz::mainTrackEdProcess)
RC_DECOMP_FN_BLOCKING_SETUP_HEAP(MainTrackEdTerm, rc::fuzz::mainTrackEdTerm)
RC_DECOMP_FN_BLOCKING_SETUP(MainTerm, rc::fuzz::mainTerm)

namespace {
// The game's printf as its instructions are (the recompiler made it the runtime's printf): the variable arguments
// stored into the frame below $sp, nothing else.
void printfInstructions(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    const uint32_t sp = getRegU32(ctx, 29) - 0x80u;
    for (int r = 5; r <= 11; ++r)
    {
        const uint64_t value = GPR_U64(ctx, r);
        std::memcpy(rc::guest_host(sp + 0x48u + 8u * static_cast<uint32_t>(r - 5)), &value, 8); // sd $a1-$t3, 0x48-0x78
    }
#if RC_VERSION_EU
    for (int i = 0; i < 4; ++i)
        std::memcpy(rc::guest_host(sp + 0x38u + 4u * static_cast<uint32_t>(i)), &ctx->f[12 + 2 * i], 4); // swc1 $f12-$f18, 0x38-0x44
#endif
    ctx->pc = getRegU32(ctx, 31);
}
} // namespace
RC_DECOMP_FN_SETUP_REFERENCE(printf, rc::fuzz::mainPrintf, printfInstructions)
RC_DECOMP_FN_SETUP_HEAP(MainScreenGameInit, rc::fuzz::mainScreen)
RC_DECOMP_FN_SETUP_HEAP(SetFrameWindow, rc::fuzz::mainFrameWindow)
