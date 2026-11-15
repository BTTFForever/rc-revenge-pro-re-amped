// Shared by main.cpp and main2.cpp (MAIN.c): the unit's functions, guest addresses and small helpers.
#pragma once

#include "rc_decomp.h"

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
constexpr uint32_t kMain = 0x1ed900u, kFrontEnd = 0x1eca58u, kAi = 0x1e9400u, kRaceOrder = 0x1e9404u, kCarList = 0x1eb7a0u;
constexpr uint32_t kCamera = 0x1e9fb0u, kMusic = 0x1ee8c0u, kRevSfx = 0x1ef3b0u, kObjectList = 0x1ee390u;
constexpr uint32_t kResultTrigger = 0x1f3730u, kRacePoints = 0x1ed460u, kPauseLinks = 0x1ed468u, kRacePointsText = 0x327f00u;
constexpr uint32_t kResultsMenu = 0x327ec0u, kRankingMenu = 0x327ec4u, kPauseOptions = 0x327ec8u, kPauseOptions2 = 0x327eccu;
constexpr uint32_t kPadMenu1 = 0x327ed0u, kPadMenu2 = 0x327ed4u, kPauseDelay = 0x327ee8u, kPauseMenu = 0x327c20u;
constexpr uint32_t kTimerResumed = 0x327e90u, kPadFlags = 0x327d68u, kLevel = 0x327e80u;

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
constexpr uint32_t kPrintf = 0x12e9e8u, kAMenuGetText = 0x10ddf0u, kGetCarText = 0x10df30u, kAMenuDisplay = 0x104f80u;
constexpr uint32_t kAMenuDisplayAll = 0x103f08u, kAMenuDisplayOptions = 0x108cd8u, kAMenuGetMIT = 0x10dc80u, kAMenuProcessAll = 0x108ef8u;
constexpr uint32_t kMenuItemDisabler = 0x122c00u, kTagToCar = 0x11dc30u, kCheckCarAvailability = 0x122ec0u;
constexpr uint32_t kInputHit = 0x191598u, kInputPadPresent = 0x1918f8u, kInputResetAllHit = 0x1916c8u, kInputResetHit = 0x1917b8u;
constexpr uint32_t kInputReset = 0x191600u, kInputSnapshot = 0x191810u, kInputTestAnyKeyHeld = 0x191c78u;
constexpr uint32_t kInputClearVibrationVals = 0x191c40u, kInputSetVibrationFlags = 0x191c20u, kInputSetPadMode = 0x191c00u;
constexpr uint32_t kTimerGet = 0x1b4188u, kTimerPause = 0x1b4068u, kTimerResume = 0x1b40e8u, kTimerSetup = 0x1b4038u;
constexpr uint32_t kRevSfxPauseFade = 0x142e20u, kRevSfxStopAll = 0x141fc8u, kRevSfxStopPauseSfx = 0x144150u;
constexpr uint32_t kRevSfxModifyVolume = 0x144098u, kRevSfxUpdateAmbient = 0x1423b0u, kRevSfxInitAmbient = 0x143d50u;
constexpr uint32_t kRCMusicPause = 0x141790u, kRCMusicResume = 0x140e20u, kRCMusicSetVol = 0x1414e0u, kRCMusicStop = 0x1415b8u;
constexpr uint32_t kRCMusicModifyVolume = 0x1413f0u, kRCMusicPlayLevel = 0x1415d0u, kRCMusicPlayTrackEdit = 0x140cf8u;
constexpr uint32_t kRCMusicFadeControl = 0x141080u, kSfxDoSfx = 0x1ac9e0u, kSceGsSyncV = 0x1c3d90u, kTaskDelete = 0x1afc48u;
constexpr uint32_t kRand = 0x1b8818u;

inline uint32_t menuText(uint32_t item) { return guest_call<uint32_t>(kAMenuGetText, at<uint32_t>(item)); }

} // namespace mainunit

} // namespace rc
