// Decompiled from LEVEL.c: loading a level. LevelLoad frees the render buffers, makes the resource area (0x1194000
// bytes at the top of memory), loads the level's BB file (the front end 0x62, the track editor 0x63, a built track
// 0x61, or a race level, numbers above 48 wrapped) and points the game's systems at its data, then gives what memory is
// left to the render buffers. The *SetupClass functions copy the level's pointers (pLvlG) into each system.
//
// Written from the instructions. Everything that loads runs once per level and can wait on the disc: LevelLoad and its
// four loaders are BLOCKING (live checks only). LevelInit and LevelTerm set pLvlG to -1 (Ghidra shows &_heap_size).

#include "physics_fuzz.h"
#include "weapon.h"

#include "rc_decomp.h"

// Europe variant: LevelLoadGameLevel differs only in register use and instruction order (the same body;
// tools/asm_listing.py NAME --diff).
RC_VERSION_VARIANT(LevelLoadGameLevel)

namespace rc {

void TBoxSetupClass(); // tbox.cpp

void LevelA3DSetupClass();
void LevelA3DSetupTrackLevelClass();
void LevelAColSetupClass();
void LevelPauseMenuSetupClass();
void LevelLoadFrontEnd();
void LevelLoadTrackEditor();
void LevelLoadGameLevel(int32_t levelNum);
void LevelLoadTrackLevel();

namespace {

using weapon::at;

constexpr uint32_t kLevelPtr = sym::pLvlG, kMenuSystemPtr = sym::pMenuSystemG, kLsVars = sym::lsVarG, kVisVars = sym::visVarG;
constexpr uint32_t kSvf = sym::svfVarG, kTso = sym::tsoVarG, kAColGrid = sym::acolG, kMainVars = sym::mainVarG;
constexpr uint32_t kResVars = sym::resVarG, kNextTexAddr = sym::nextTexAddr, kFrontEnd = sym::frontEndVarG, kCarList = sym::carListVarG;
constexpr uint32_t kVfxRes = (sym::visFxVarG + 0x6cu), kTEditVars = sym::tEditVarG;
// The level's resource types (strings at 0x327e30 to 0x327e78, eight bytes apart).
constexpr uint32_t kLevelTypes = (sym::sec_sdata + 0x230u);

// Guest functions still recompiled (or reached through the dispatcher).
constexpr uint32_t kResTypeSetConstructDaemon = sym::ResTypeSetConstructDaemon, kResTypeUnSetConstructDaemon = sym::ResTypeUnSetConstructDaemon;
constexpr uint32_t kVertAnimInit = sym::VertAnimInit, kVertAnimTerm = sym::VertAnimTerm, kGfxTexReset = sym::GfxTex_Reset;
constexpr uint32_t kRenderFreeBuffers = sym::RenderFreeBuffers, kRenderAllocateBuffers = sym::RenderAllocateBuffers, kHeapTerm = sym::HeapTerm;
constexpr uint32_t kHeapInit = sym::HeapInit, kDisplayDisable = sym::DisplayDisable, kDisplayEnable = sym::DisplayEnable;
constexpr uint32_t kMemBlockAllocHi = sym::MemBlockAllocHi, kMemBlockCompress = sym::MemBlockCompress, kMemGetFree = sym::MemGetFree;
constexpr uint32_t kFMPrepare = sym::FMPrepareHi, kFMPrepareHi = sym::FMPrepare, kFMUnPrepare = sym::FMUnPrepare, kFMSetCacheMode = sym::FMSetCacheMode;
constexpr uint32_t kResLoadInit = sym::ResLoadInit, kResLoadTerm = sym::ResLoadTerm, kResLoad = sym::ResLoad, kTexReset = sym::TexReset;
constexpr uint32_t kTexFreeIntermediate = sym::TexFreeIntermediateResData, kRevSfxReset = sym::RevSfxReset, kRCMenuMVarLinkup = sym::RCMenuMVarLinkup;
constexpr uint32_t kLoadingTerm = sym::LoadingTerm, kPrintf = sym::printf, kBBLoad = sym::BBLoad;
constexpr uint32_t kVfxSetup = sym::VfxSetup, kAnimSetupClass = sym::AnimSetupClass, kFrontSetupClass = sym::FrontSetupClass;
constexpr uint32_t kSFXSetupClass = sym::SFXSetupClass, kPickupSetupClass = sym::PickupSetupClass, kHudSetupClass = sym::HudSetupClass;
constexpr uint32_t kAINodeSetupClass = sym::AINodeSetupClass;
constexpr uint32_t kTEditSetupTELvLClass = sym::TEditSetupTELvLClass, kTEditLevelInit = sym::TEditLevelInit, kTEditSetupTCLvLClass = sym::TEditSetupTCLvLClass;
constexpr uint32_t kTEditGenerateTCol = sym::TEditGenerateTCol, kTEditSetupTALvLClass = sym::TEditSetupTALvLClass, kTEditGenerateTrackAINodes = sym::TEditGenerateTrackAINodes;
constexpr uint32_t kTEditGenerateHudMap = sym::TEditGenerateHudMapForTrackLevel, kTEditConvertTrackToALF = sym::TEditConvertTrackToALF, kTEditGetThemeType = sym::TEditGetThemeTypeForTrack;
constexpr uint32_t kTEditSetTileList = sym::TEditSetTileListForThemeType, kTEditUpdatePickups = sym::TEditUpdatePickupNumAndPositions, kTEditUpdateStartGrid = sym::TEditUpdateStartGridPositions;
constexpr uint32_t kTEditStartGridCameraPaths = sym::TEditConstructStartGridCameraPaths, kTEditCopyHudMap = sym::TEditCopyHudMapForTrackLevelToVRAM;
// printf formats.
constexpr uint32_t kTexOverflow = (sym::sec_rodata + 0x9d8u), kGpuBuffers = (sym::sec_rodata + 0xa00u), kBBLoadStarted = (sym::sec_rodata + 0xa48u);
constexpr uint32_t kBBLoadCompleted = (sym::sec_rodata + 0xa58u), kDynamicStarted = (sym::sec_rodata + 0xa70u), kDynamicCompleted = (sym::sec_rodata + 0xa88u);

constexpr int32_t kTrackLevel = 0x61, kFrontEndLevel = 0x62, kTrackEditor = 0x63;

uint32_t level() { return at<uint32_t>(kLevelPtr); }
uint32_t menu() { return at<uint32_t>(kMenuSystemPtr); }

// The menus' tick graphic and key sprites from a UI group (its selection tag and destination header).
void menuGraphics(uint32_t group)
{
    const uint32_t m = menu();
    at<uint32_t>(m + 0x28u) = at<uint32_t>(group + 8u); // pTickGfx
    at<uint32_t>(m + 0x24u) = at<uint32_t>(group + 4u); // pPsxKeys
}

} // namespace

void LevelA3DSetupClass()
{
    const uint32_t lvl = level();
    at<uint32_t>(kLsVars) = at<uint32_t>(lvl + 0u);   // lsVarG.pParent = pALFData
    at<uint32_t>(kVisVars) = at<uint32_t>(lvl + 0xcu); // visVarG.pVISHdr = pVISData
    const int32_t cellSize = at<int32_t>(at<uint32_t>(lvl + 0u) + 0x68u);
    const uint32_t vis = at<uint32_t>(lvl + 0xcu);
    const uint32_t tso = at<uint32_t>(lvl + 4u);
    const uint32_t svf = at<uint32_t>(lvl + 0x10u);
    at<int32_t>(kLsVars + 0xcu) = cellSize >> 1; // cellMid
    at<uint32_t>(kVisVars + 4u) = vis + 4u;      // pVISBoxes
    at<uint32_t>(kSvf) = svf;
    at<uint32_t>(kTso) = tso;
    at<int32_t>(kLsVars + 8u) = cellSize;
}

void LevelA3DSetupTrackLevelClass()
{
    const uint32_t lvl = level();
    const uint32_t tso = at<uint32_t>(lvl + 4u);
    at<uint32_t>(kVisVars) = at<uint32_t>(lvl + 0xcu);
    at<uint32_t>(kSvf) = at<uint32_t>(lvl + 0x10u);
    const uint32_t vis = at<uint32_t>(lvl + 0xcu);
    at<uint32_t>(kTso) = tso;
    at<uint32_t>(kVisVars + 4u) = vis + 4u;
}

void LevelAColSetupClass()
{
    at<uint32_t>(kAColGrid) = at<uint32_t>(level() + 0x14u);
}

void LevelPauseMenuSetupClass()
{
    const uint32_t m = menu();
    const uint32_t group = at<uint32_t>(level() + 0x44u);
    at<uint32_t>(m + 0x14u) = at<uint32_t>(group);      // pPauseMenu = pSourceHeader
    at<uint32_t>(m + 0x24u) = at<uint32_t>(group + 4u); // pPsxKeys = pDestHeader
}

void LevelInit()
{
    at<uint32_t>(kLevelPtr) = 0xffffffffu;
    for (uint32_t i = 0; i < 10; ++i)
        guest_call(kResTypeSetConstructDaemon, kLevelTypes + 8u * i, 0, 4);
    guest_call(kVertAnimInit); // a tail call
}

void LevelTerm()
{
    at<uint32_t>(kLevelPtr) = 0xffffffffu;
    for (uint32_t i = 0; i < 10; ++i)
        guest_call(kResTypeUnSetConstructDaemon, kLevelTypes + 8u * i);
    guest_call(kVertAnimTerm);
    guest_call(kGfxTexReset); // a tail call
}

static void levelLoadBody(int32_t levelNum)
{
    GuestFrame frame(0x60);
    const uint32_t resHandle = kMainVars + 0x164u;
    guest_call(kRenderFreeBuffers);
    if (levelNum != kTrackLevel)
    {
        guest_call(kHeapTerm);
        guest_call(kDisplayDisable);
    }
    guest_call(kMemBlockAllocHi, resHandle, 0x1194000u);
    at<uint32_t>(kResVars + 0x9cu) = 0x1194000u; // resVarG.size
    at<uint32_t>(kResVars + 0xa0u) = at<uint32_t>(resHandle); // resVarG.pTop
    guest_call(levelNum == kTrackLevel ? kFMPrepareHi : kFMPrepare, 0x1f4000u);
    guest_call(kResLoadInit);
    guest_call(kTexReset);
    guest_call(kRevSfxReset);
    if (levelNum == kFrontEndLevel)
        LevelLoadFrontEnd();
    else if (levelNum == kTrackEditor)
        LevelLoadTrackEditor();
    else if (levelNum == kTrackLevel)
        LevelLoadTrackLevel();
    else
        LevelLoadGameLevel(levelNum);
    const uint32_t texTop = at<uint32_t>(kNextTexAddr);
    if (texTop > 0x400000u)
        guest_call(kPrintf, kTexOverflow, texTop - 0x400000u);
    guest_call(kRCMenuMVarLinkup);
    guest_call(kResLoadTerm);
    guest_call(kFMUnPrepare);
    guest_call(kTexFreeIntermediate);
    guest_call(kMemBlockCompress, resHandle, at<uint32_t>(kResVars + 0xa4u) - at<uint32_t>(kResVars + 0x38cu) + 1u);
    if (levelNum != kTrackLevel)
    {
        guest_call(kHeapInit, 0x3436);
        guest_call(kDisplayEnable);
    }
    uint32_t size = (((guest_call<uint32_t>(kMemGetFree) - 0x19000u) >> 1) + 3u) & 0xfffffffcu;
    if (size > 0x32000u)
        size = 0x32000u;
    guest_call(kRenderAllocateBuffers, size);
    const uint32_t levelNo = at<uint8_t>(kMainVars + 0x34au);
    const uint32_t left = guest_call<uint32_t>(kMemGetFree);
    guest_call(kPrintf, kGpuBuffers, size, levelNo, left);
}

void LevelLoad(int32_t levelNum)
{
    levelLoadBody(levelNum);
    guest_call(kLoadingTerm); // a tail call, from the caller's frame
}

void LevelLoadFrontEnd()
{
    {
        GuestFrame frame(0x40);
        guest_call(kFMSetCacheMode, 0);
        guest_call(kPrintf, kBBLoadStarted);
        at<uint32_t>(kFrontEnd) = guest_call<uint32_t>(kBBLoad, kFrontEndLevel, 0x20) - 0x140u; // pFELevelData
        guest_call(kPrintf, kBBLoadCompleted);
        const uint32_t fe = at<uint32_t>(kFrontEnd);
        const uint32_t group = at<uint32_t>(fe + 0x18u);
        const uint32_t m = menu();
        at<uint32_t>(m + 0x28u) = at<uint32_t>(group + 8u);
        at<uint32_t>(kLevelPtr) = at<uint32_t>(fe);
        at<uint32_t>(m + 0x24u) = at<uint32_t>(group + 4u);
        guest_call(kVfxSetup);
        guest_call(kAnimSetupClass);
        LevelA3DSetupClass();
        LevelAColSetupClass();
        guest_call(kFrontSetupClass);
        guest_call(kSFXSetupClass);
        for (uint32_t off : {0x1e4u, 0x1e8u}) // carListVarG.altRendObjResource, altRendObjResourceBoat
            if (const uint32_t id = at<uint32_t>(kCarList + off); id != 0)
                at<uint32_t>(kCarList + off) = guest_call<uint32_t>(kResLoad, id);
    }
    at<uint8_t>(kMainVars + 0x368u) = 0;  // splitScreen
    at<uint32_t>(kMainVars + 0x398u) = 0; // mirrorModeOnOff
    const uint32_t fe = at<uint32_t>(kFrontEnd);
    const uint32_t res = at<uint32_t>(kVfxRes);
    at<uint32_t>(res + 0x5cu) = at<uint32_t>(fe + 0x134u); // vfxTex[0x17] = pHeadLightFlareTex
    at<uint32_t>(res + 4u) = at<uint32_t>(fe + 0x130u);    // vfxTex[1] = pHeadLightGlowTex
}

void LevelLoadTrackEditor()
{
    uint32_t data;
    {
        GuestFrame frame(0x20);
        guest_call(kFMSetCacheMode, 0);
        guest_call(kPrintf, kBBLoadStarted);
        data = guest_call<uint32_t>(kBBLoad, kTrackEditor, 0x10) - 0xb0u;
        guest_call(kPrintf, kBBLoadCompleted);
        menuGraphics(at<uint32_t>(data + 0x58u));
    }
    guest_call(kTEditSetupTELvLClass, data); // a tail call
}

void LevelLoadGameLevel(int32_t levelNum)
{
    GuestFrame frame(0x20);
    guest_call(kFMSetCacheMode, 0);
    guest_call(kPrintf, kBBLoadStarted);
    const uint32_t n = static_cast<uint32_t>(levelNum);
    const uint32_t file = n < 0x31u ? n : n % 0x31u + 1u;
    at<uint32_t>(kLevelPtr) = guest_call<uint32_t>(kBBLoad, file, 0x10) - 0x70u;
    guest_call(kPrintf, kBBLoadCompleted);
    guest_call(kAnimSetupClass);
    LevelA3DSetupClass();
    LevelAColSetupClass();
    LevelPauseMenuSetupClass();
    guest_call(kVfxSetup);
    guest_call(kPickupSetupClass);
    guest_call(kHudSetupClass);
    guest_call(kAINodeSetupClass);
    guest_call(kSFXSetupClass);
    TBoxSetupClass();
    guest_call(kFMSetCacheMode, 1);
    guest_call(kPrintf, kDynamicStarted);
    guest_call(kResLoad, 0x269a);
    guest_call(kPrintf, kDynamicCompleted);
    const uint32_t m = menu();
    const uint32_t group = at<uint32_t>(level() + 0x44u);
    at<uint32_t>(m) = at<uint32_t>(m + 0x14u); // pActiveMenu = pPauseMenu
    menuGraphics(group);
}

void LevelLoadTrackLevel()
{
    GuestFrame frame(0x50);
    guest_call(kTEditLevelInit);
    guest_call(kFMSetCacheMode, 0);
    guest_call(kTEditSetupTCLvLClass, guest_call<uint32_t>(kBBLoad, 0x65, 1) - 0x20u);
    guest_call(kTEditGenerateTCol);
    guest_call(kResLoadInit);
    guest_call(kRevSfxReset);
    guest_call(kTEditSetupTALvLClass, guest_call<uint32_t>(kBBLoad, 100, 1) - 0x30u);
    guest_call(kTEditGenerateTrackAINodes);
    guest_call(kTEditGenerateHudMap);
    guest_call(kTEditConvertTrackToALF);
    guest_call(kResLoadInit);
    guest_call(kTexReset);
    guest_call(kRevSfxReset);
    at<uint32_t>(kLevelPtr) = guest_call<uint32_t>(kBBLoad, kTrackLevel, 1) - 0x70u;
    const uint32_t theme = guest_call<uint32_t>(kTEditGetThemeType);
    guest_call(kTEditSetTileList, at<uint32_t>(level()), theme);
    at<uint32_t>(level()) = 0; // pALFData
    guest_call(kAnimSetupClass);
    LevelA3DSetupTrackLevelClass();
    LevelPauseMenuSetupClass();
    const uint32_t lvl = level();
    const uint32_t vista = at<uint32_t>(lvl + 0x4cu) + theme * 4u;
    at<uint32_t>(lvl + 0x4cu) = at<uint32_t>(vista); // pSkyVista
    at<uint32_t>(lvl + 0x50u) = at<uint32_t>(vista); // pToSkyVista
    guest_call(kVfxSetup);
    guest_call(kPickupSetupClass);
    guest_call(kTEditUpdatePickups);
    guest_call(kHudSetupClass);
    guest_call(kAnimSetupClass);
    TBoxSetupClass();
    guest_call(kTEditUpdateStartGrid);
    guest_call(kTEditStartGridCameraPaths);
    guest_call(kTEditCopyHudMap);
    guest_call(kFMSetCacheMode, 1);
    guest_call(kResLoad, 0x269a);
    const uint32_t m = menu();
    const uint32_t group = at<uint32_t>(level() + 0x44u);
    at<uint32_t>(kLsVars + 0xcu) = at<uint32_t>(kTEditVars + 0x278u); // cellMid
    at<uint32_t>(kLsVars + 8u) = at<uint32_t>(kTEditVars + 0x274u);   // cellSize
    at<uint32_t>(m) = at<uint32_t>(m + 0x14u);
    at<uint32_t>(kLsVars) = at<uint32_t>(kTEditVars + 0x26cu);        // pParent
    menuGraphics(group);
}

RC_DECOMP_FN_SETUP(LevelA3DSetupClass, rc::fuzz::levelData)
RC_DECOMP_FN_SETUP(LevelA3DSetupTrackLevelClass, rc::fuzz::levelData)
RC_DECOMP_FN_SETUP(LevelAColSetupClass, rc::fuzz::levelData)
RC_DECOMP_FN_SETUP(LevelPauseMenuSetupClass, rc::fuzz::levelData)
RC_DECOMP_FN_SETUP_HEAP(LevelInit, rc::fuzz::levelInitTerm)
RC_DECOMP_FN_SETUP_HEAP(LevelTerm, rc::fuzz::levelInitTerm)
RC_DECOMP_FN_BLOCKING(LevelLoad)
RC_DECOMP_FN_BLOCKING(LevelLoadFrontEnd)
RC_DECOMP_FN_BLOCKING(LevelLoadTrackEditor)
RC_DECOMP_FN_BLOCKING(LevelLoadGameLevel)
RC_DECOMP_FN_BLOCKING(LevelLoadTrackLevel)

} // namespace rc
