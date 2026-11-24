// Shared by main.cpp and main2.cpp (MAIN.c): the unit's functions, guest addresses and small helpers.
#pragma once

#include "rc_decomp.h"
#include "../rc_cars.h"

#include <cstring>

namespace rc {

// MAIN.c.
void MainInit();
void MainFrontInit();
void MainFrontProcess();
void MainFrontTerm();
void MainTrackEdInit();
void MainTrackEdProcess();
void MainTrackEdTerm();
void MainGameInit();
void MainGameLevelInit();
void MainGameProcess();
void MainGameLevelTerm();
void MainGameTerm();
void MainTerm();
void MainScreenGameInit(int32_t i);
void SetFrameWindow(int32_t windowId, tTime time);
int32_t BuildResults(int32_t flag);
void ClearRanking();
void SortRanking();
int32_t BuildRanking(int32_t flag);
void PauseMenuProcess(ptr32<mainClass> pMainVar);
void MainSetCarsRemoveDuplicates();
void MainSetCars(eCarSet carSet);
void MainDemoEndTask(ptr32<void> pData, tTime time);
void MainRaceOver(int32_t flag);
void SaveChampionshipPoints();
int32_t CheckPads(int32_t numHumans);
int32_t PauseMenuScan();
void ClearRankingDisplay();
void ClearChampionshipPoints();
void SetChampionshipPoints();

namespace mainunit {

template <typename T>
T &at(uint32_t address) { return *static_cast<T *>(guest_host(address)); }

// Globals.
constexpr uint32_t kMain = sym::mainVarG, kFrontEnd = sym::frontEndVarG, kAi = sym::aiVarG, kRaceOrder = cars::kRaceOrder /* enhancement: aiVarG.raceOrder moved for more than 8 cars (rc_cars.h) */, kCarList = sym::carListVarG;
constexpr uint32_t kCamera = sym::cameraVarG, kMusic = sym::rcmusicVarG, kRevSfx = sym::revSfxVarG, kObjectList = sym::objectVarG;
constexpr uint32_t kResultTrigger = (sym::visFxVarG + 0x2730u), kRacePoints = sym::racePointsG, kPauseLinks = sym::links_600, kRacePointsText = (sym::sec_sdata + 0x300u);
constexpr uint32_t kResultsMenu = sym::pResultsMenuG, kRankingMenu = sym::pRankingMenuG, kPauseOptions = sym::pXContMenuG, kPauseOptions2 = sym::pXContMenuPlusG;
constexpr uint32_t kPadMenu1 = sym::pP1ContMissingG, kPadMenu2 = sym::pP2ContMissingG, kPauseDelay = sym::rePauseCountG, kPauseMenu = sym::pMenuSystemG;
constexpr uint32_t kTimerResumed = sym::iDrawDownG, kPadFlags = sym::pauseReasonG, kLevel = sym::pLvlG;

// mainVarG fields by offset.
inline uint32_t &status() { return at<uint32_t>(kMain); }
inline uint32_t &toDo() { return at<uint32_t>(kMain + 4u); }
inline uint8_t noHuman() { return at<uint8_t>(kMain + 0x369u); }
inline uint8_t noAI() { return at<uint8_t>(kMain + 0x36au); }
inline int32_t carCount() { return static_cast<int32_t>(noHuman()) + static_cast<int32_t>(noAI()); }
inline uint32_t cup() { return at<uint32_t>(kFrontEnd + 0x298u); }

// raceOrder[i]: the car (+0) and its points bytes (+6 race points, +7 total).
inline uint32_t orderAt(int32_t i) { return kRaceOrder + static_cast<uint32_t>(i) * 12u; }
inline uint32_t orderCar(int32_t i) { return at<uint32_t>(orderAt(i)); }
// The car's type, a boat's as its car's.
inline uint32_t carType(uint32_t car)
{
    uint32_t type = at<uint32_t>(at<uint32_t>(car + 0xcu) + 4u);
    if (type >= 0x14u)
        type -= 0x13u;
    return type;
}

inline uint32_t glen(uint32_t text) { return static_cast<uint32_t>(std::strlen(static_cast<const char *>(guest_host(text)))); }

// Guest functions still recompiled (and native ones reached the same way from the frame loops).
constexpr uint32_t kPrintf = sym::printf, kAMenuGetText = sym::AMenuGetText, kGetCarText = sym::GetCarText, kAMenuDisplay = sym::AMenuDisplay;
constexpr uint32_t kAMenuDisplayAll = sym::AMenuDisplayAll, kAMenuDisplayOptions = sym::AMenuDisplayOptions, kAMenuGetMIT = sym::AMenuGetMIT, kAMenuProcessAll = sym::AMenuProcessAll;
constexpr uint32_t kMenuItemDisabler = sym::MenuItemDisabler, kTagToCar = sym::TagToCar, kCheckCarAvailability = sym::CheckCarAvailability;
constexpr uint32_t kInputHit = sym::InputHit, kInputPadPresent = sym::InputPadPresent, kInputResetAllHit = sym::InputResetAllHit, kInputResetHit = sym::InputResetHit;
constexpr uint32_t kInputReset = sym::InputReset, kInputSnapshot = sym::InputSnapshot, kInputTestAnyKeyHeld = sym::InputTestAnyKeyHeld;
constexpr uint32_t kInputClearVibrationVals = sym::InputClearVibrationVals, kInputSetVibrationFlags = sym::InputSetVibrationFlags, kInputSetPadMode = sym::InputSetPadMode;
constexpr uint32_t kTimerGet = sym::TimerGet, kTimerPause = sym::TimerPause, kTimerResume = sym::TimerResume, kTimerSetup = sym::TimerSetup;
constexpr uint32_t kRevSfxPauseFade = sym::RevSfxPauseFade, kRevSfxStopAll = sym::RevSfxStopAll, kRevSfxStopPauseSfx = sym::RevSfxStopPauseSfx;
constexpr uint32_t kRevSfxModifyVolume = sym::RevSfxModifyVolume, kRevSfxUpdateAmbient = sym::RevSfxUpdateAmbient, kRevSfxInitAmbient = sym::RevSfxInitAmbient;
constexpr uint32_t kRCMusicPause = sym::RCMusicPause, kRCMusicResume = sym::RCMusicResume, kRCMusicSetVol = sym::RCMusicSetVol, kRCMusicStop = sym::RCMusicStop;
constexpr uint32_t kRCMusicModifyVolume = sym::RCMusicModifyVolume, kRCMusicPlayLevel = sym::RCMusicPlayLevel, kRCMusicPlayTrackEdit = sym::RCMusicPlayTrackEdit;
constexpr uint32_t kRCMusicFadeControl = sym::RCMusicFadeControl, kSfxDoSfx = sym::SfxDoSfx, kSceGsSyncV = sym::sceGsSyncV, kTaskDelete = sym::TaskDelete;
constexpr uint32_t kRand = sym::rand;

inline uint32_t menuText(uint32_t item) { return guest_call<uint32_t>(kAMenuGetText, at<uint32_t>(item)); }

} // namespace mainunit

} // namespace rc
