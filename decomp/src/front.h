// Shared by the FRONT.c, CAROUSEL.c and CREDITS.c sources (front.cpp, front_menu.cpp, front_rewards.cpp, carousel.cpp,
// credits.cpp): the units' functions, guest addresses and small helpers.
#pragma once

#include "amenu.h"

namespace rc {

// Other units, native.
void HudFormatLapTime(ptr32<char> pString, int32 lapTime); // hud.cpp

// FRONT.c.
void SetCheat(uint8 cheatNo);
int32_t CheckCheatKeys(ptr32<sCheatKeyData> pCheatKeyData, tTime time);
void FrontSetupClass();
void FrontEndInit();
void FrontEndProcess();
void FrontEndTerm();
int32_t CameraDestination(uint32 tag1, uint32 tag2);
uint32 RemapDestTags(uint32 tag);
eObjectType TagToCar(uint32 tag);
uint32 CarToTag(eObjectType car, int32_t player);
int32_t WorldTrackToLevel();
int32_t HideEmptyMenu(ptr32<sAMenuHeader> pHeader);
int32_t ParseSelectionTags();
void ScreenAdjust();
void BuildConfirmText();
int32_t DisplayStatsBar(int32_t x, int32_t y, int32_t width, int32_t height, int32_t value);
void DisplayVehicleStats(ptr32<sAMenuHeader> pHeader);
void StatsInit();
void StatsTerm();
void FrontEndTaskProcess(ptr32<void> pData, tTime time);
int32_t CheckSingleCarAvailability(int32_t carType);
void CarouselSetAvailability();
int32_t SelectionTest();
void MainQuitPodium();
void PathPilotEndCallbackHOP(ptr32<void> pData);
void HandoverToFE();
void FECarouselInit(tTime time);
void BuildKeyDefinitions(ptr32<sAMenuHeader> pHeader);
void SetAllCarsAvailable(ptr32<rewardTable> pRewards);
void SetCheatResults(ptr32<sCheatKeyData> pCheatKeyData);
void ClearAllSecrets(ptr32<rewardTable> pRewards);
void SetRewards(ptr32<rewardTable> pRewards, ptr32<_sFrontGameStateData> pGameStateData, int32_t cup);
void SetCupAvailability(int32_t flag);
void ShowMessage();
void ProcessRewardMessage();
void HandlePersistentCars();
void BuildLockedTracksList();
void TimeTrialExitHandler();
void DisplayRCRevengeBanner();
void GameAutoLoadSpaceCheck();
void PollingMemoryCard();
void MenuInitScan(ptr32<sUIgroup> pUIGroup, int32_t flag);
int32_t GetTimeTrialTime(ptr32<char> pDestBuffer, ptr32<_sFrontGameStateData> pGameStateData, int32_t level, int32_t reverseFlag);
void MenuItemDisabler(ptr32<sAMenuHeader> pHeader);
void StatsConstructDaemon(ptr32<memHandle> pHandle);
int32_t CheckCarAvailability(int32_t carType);
ptr32<_sCarousel> GetCarouselPointer(int32_t carType);
int32_t FrontEndMenuScan();
int32_t CheckLevelAvailability(ptr32<rewardTable> pRewards, int32_t level);
void SetAllTracksAvailable(ptr32<rewardTable> pRewards);
void GameAutoLoadSetup();
void FEStartPos();
int32_t SetUpVehicle(int32_t carNo, int32_t carType);
int32_t TagToCup(uint32 tag);
ptr32<char> BuildConfirmTextCar(int32_t car);
ptr32<char> BuildConfirmTextWorldTrack();
ptr32<char> BuildConfirmTextCup();
ptr32<char> BuildConfirmTextRev(ptr32<char> pReverse);
ptr32<char> BuildConfirmTextMirror(ptr32<char> pMirror);
void xxxDummyCallback();
ptr32<_sSingleCamera> xxxGetFECameraFromTag(uint32 tag);
void EnableSingleCar(int32_t carType);
void ChopOutMenuItem(ptr32<sAMenuHeader> pHeader, int32_t item);

// CAROUSEL.c.
void CarouselInit();
void CarouselTerm();
void CarouselSetUp(uint32 numSegments, tTime time);
uint32 CarouselRender(tTime time);
uint32 CarouselDraw(tTime time);
void CarouselMoveTo(eObjectType type);
void CarouselMove(uint32 segment);
void CarouselMode(bool_ bBoat);
void CarouselDisableAnimGroup(int32 group);
void CarouselEnableAnimGroup(int32 group);

// CREDITS.c.
void CreditsDisplay(ptr32<sAMenuHeader> pSourceHeader);
void killDeadFX();
void VfxCallFireWorkRenderers();
void VfxDeleteFireWorks();
void CreditsInit(int32 iType);
void CreditsTerm();

namespace front {

using amenu::at;

// The menu tags the front end's menus and camera paths are linked by.
constexpr uint32_t kTagFirstMenu = 0x100001f1u;

inline sFrontEndClass &fe() { return G(frontEndVarG); }
inline mainClass &mainVars() { return G(mainVarG); }
inline sMenuSystem &menuSystem() { return *G(pMenuSystemG); }
inline ptr32<_sFELevelData> feLevel() { return G(frontEndVarG).pFELevelData; }
inline sCarouselClass &carousel() { return G(carouselVarG); }

// The carousel entry of a car type, or null (the original's search, unrolled by four, inline in several functions).
inline ptr32<_sCarousel> carouselEntry(int32_t carType)
{
    for (uint32_t i = 0; i < 19u; ++i)
        if (static_cast<int32_t>(carousel().carousel[i].type) == carType)
            return ptr32<_sCarousel>(guest_addr(&carousel().carousel[i]));
    return ptr32<_sCarousel>(nullptr);
}

// A register the caller left set (the original returns it for a value it does not know).
inline uint32_t incoming(int reg) { return getRegU32(&g_runtime->eeScheduler().runningContext(), reg); }

// TagToCar, CarToTag and WorldTrackToLevel with the registers they fall back on for an unknown tag, car or world
// given explicitly (the value the original's caller leaves in them), for native callers. TagToCar leaves a known
// tag's type in $a1 (a1 here) as the original does.
eObjectType tagToCar(uint32 tag, uint32 &a1);
uint32 carToTag(eObjectType car, int32_t player, uint32 a2, uint32 a3);
int32_t worldTrackToLevel(uint32 &t0);   // leaves a known world's index in t0, as the original does
void buildConfirmText(uint32 t0);         // BuildConfirmText with the $t0 its WorldTrackToLevel falls back on

// The first UI group whose word at the given offset (selectionTag 8, viewPosTag 0xc) holds the tag, searched without
// an end mark as the original does.
inline ptr32<sUIgroup> groupWithTag(uint32_t offset, uint32_t tag)
{
    uint32_t g = feLevel()->pUIGroup.address;
    while (at<uint32_t>(g + offset) != tag)
        g += 0x14u;
    return ptr32<sUIgroup>(g);
}

// A time trial's target in thousandths: the level's (0-based) first or second (reversed) time.
inline int32_t targetTime(ptr32<sExtraLevelData> pExtra, int32_t level0, bool reversed)
{
    const sExtraLevelData &e = pExtra[level0];
    if (reversed)
        return static_cast<int32_t>(e.tt2Mins * 60000u + e.tt2Secs * 1000u + e.tt2Tenths * 100u);
    return static_cast<int32_t>(e.tt1Mins * 60000u + e.tt1Secs * 1000u + e.tt1Tenths * 100u);
}

// Stores the target time in the slot if the slot is empty (-1) or not better; returns 1 if the slot's time is better.
inline int32_t keepBest(int32_t &slot, int32_t target)
{
    if (slot == -1 || target <= slot)
    {
        slot = target;
        return 0;
    }
    return 1;
}

} // namespace front
} // namespace rc
