// Decompiled from FRONT.c: the cheats and the rewards. What a player has won (cups, time trials beaten) unlocks cars
// and tracks in mainVarG.rewards; the carousel shows the unlocked cars. Three cheat key sequences (cheatData1-3G) unlock
// everything at once.
//
// Written from the instructions (Ghidra's C as a first reading). The original has the carousel search
// (GetCarouselPointer's loop, unrolled by four) inline in several functions: front::carouselEntry.

#include "front.h"
#include "front_fuzz.h"

#include <optional>

namespace rc {

using namespace front;

// The cheat sound.
void SetCheat(uint8)
{
    amenu::playSfx(0x44, 0xf6);
}

// Follows one cheat's key sequence: each key of the table in turn, held and let go, with no other key and no more
// than 2 s between them. Returns 1 once the whole sequence has been entered (then or before).
int32_t CheckCheatKeys(ptr32<sCheatKeyData> pCheatKeyData, tTime time)
{
    GuestFrame frame(0x40);
    sCheatKeyData &c = *pCheatKeyData;
    if (c.activated != 0)
        return 1;
    bool reset = false;
    if (c.lastKeyTime != 0 && static_cast<uint32_t>(time - c.lastKeyTime) > 2000u)
    {
        c.keyHeld = 0;
        reset = true;
    }
    else
    {
        uint8 key;
        bool waitForKey = c.keyHeld == 0;
        if (!waitForKey)
        {
            key = c.pCheatKeyTable[c.keyCount];
            if (!InputHeld(key))
            {
                c.keyHeld = 0;
                c.keyCount = static_cast<uint8>(c.keyCount + 1);
            }
            waitForKey = c.keyHeld == 0;
        }
        if (waitForKey)
        {
            key = c.pCheatKeyTable[c.keyCount];
            if (InputHeld(key))
            {
                c.lastKeyTime = time;
                c.keyHeld = 1;
            }
        }
        // Any other key held breaks the sequence.
        for (uint32_t other = 0; other < 0x20u && !reset; ++other)
            if (other != key && InputHeld(static_cast<uint8>(other)))
            {
                c.keyHeld = 0;
                reset = true;
            }
        if (!reset)
        {
            if (c.keyCount != c.keyMax)
                return 0;
            c.activated = 1;
            SetCheat(c.cheatNo);
            SetCheatResults(pCheatKeyData);
            return 1;
        }
    }
    c.lastKeyTime = 0;
    c.keyCount = 0;
    return 0;
}

void SetAllCarsAvailable(ptr32<rewardTable> pRewards)
{
    rewardTable &r = *pRewards;
    r.carRocket = 1;
    r.carUFO = 1;
    r.carConcept2000 = 1;
    r.carJungleRanger = 1;
    r.carYella = 1;
    r.carSarge = 1;
    r.carSkullduggery = 1;
    r.carArmoured = 1;
    r.carDumper = 1;
    r.carMoonrover = 1;
    CarouselSetAvailability();
}

// What an activated cheat gives: 1 every car, 2 every track in every mode and mirror mode, 3 one more cup won (until
// all 8), 4 the next language, 5 every time trial at its target. Cheats 3 to 5 can be entered again.
void SetCheatResults(ptr32<sCheatKeyData> pCheatKeyData)
{
    std::optional<GuestFrame> frame(std::in_place, 0x70u);
    sCheatKeyData &c = *pCheatKeyData;
    if (c.activated != 1)
        return;
    mainClass &m = mainVars();
    sFrontEndClass &f = fe();
    switch (c.cheatNo)
    {
    case 1:
        frame.reset(); // the original leaves its frame before this tail jump
        SetAllCarsAvailable(ptr32<rewardTable>(guest_addr(&m.rewards)));
        return;
    case 2:
        m.rewards.mirrorMode = 1;
        for (uint8 &level : m.rewards.lvlAvailability)
            level = 7;
        return;
    case 3:
        f.gameStateData.gameConfig.cupsWon = f.gameStateData.gameConfig.cupsWon + 1;
        if (f.gameStateData.gameConfig.cupsWon < 8)
        {
            c.keyCount = 0;
            c.activated = 0;
        }
        SetCupAvailability(0);
        frame.reset(); // the original leaves its frame before this tail jump
        CarouselSetAvailability();
        return;
    case 4:
        m.workingLanguage = static_cast<eLanguages>(static_cast<uint32_t>(m.workingLanguage) + 1u);
        if (static_cast<uint32_t>(m.workingLanguage) >= 4u)
            m.workingLanguage = static_cast<eLanguages>(0);
        break;
    case 5:
    {
        // The targets are written through the pointer read once; the times read it again for every level.
        const ptr32<sExtraLevelData> pWrite = f.pFELevelData->pExtraLevelData;
        int32 *times = f.gameStateData.gameConfig.timeTrials.times;
        for (int32_t i = 0; i < 24; ++i)
        {
            sExtraLevelData &e = pWrite[i];
            e.tt2Tenths = 9;
            e.tt1Mins = 9;
            e.tt1Secs = 59;
            e.tt1Tenths = 9;
            e.tt2Mins = 9;
            e.tt2Secs = 59;
            times[i] = -1;
            times[i + 24] = -1;
            if (const ptr32<sExtraLevelData> pExtra = f.pFELevelData->pExtraLevelData)
                keepBest(times[i], targetTime(pExtra, i, false));
            if (const ptr32<sExtraLevelData> pExtra = f.pFELevelData->pExtraLevelData)
                keepBest(times[i + 24], targetTime(pExtra, i, true));
        }
        break;
    }
    default:
        return;
    }
    c.keyCount = 0;
    c.activated = 0;
}

// Locks every secret: the cars, mirror mode and all modes of every track but the first six's normal one. The cheats
// already entered unlock theirs again.
void ClearAllSecrets(ptr32<rewardTable> pRewards)
{
    std::optional<GuestFrame> frame(std::in_place, 0x20u);
    rewardTable &r = *pRewards;
    r.carUFO = 0;
    r.carConcept2000 = 0;
    r.carJungleRanger = 0;
    r.carYella = 0;
    r.carSarge = 0;
    r.carSkullduggery = 0;
    r.carArmoured = 0;
    r.carDumper = 0;
    r.carMoonrover = 0;
    r.carRocket = 0;
    r.mirrorMode = 0;
    for (int32_t i = 23; i >= 7; --i)
        r.lvlAvailability[i] = 0;
    for (int32_t i = 0; i < 6; ++i)
        r.lvlAvailability[i] = 1;
    r.lvlAvailability[6] = 0;
    SetCheatResults(ptr32<sCheatKeyData>(sym::cheatData1G));
    frame.reset(); // the original leaves its frame before this tail jump
    SetCheatResults(ptr32<sCheatKeyData>(sym::cheatData2G));
}

namespace {

// Bit 'mode' (1 normal, 2 reversed, 4 mirrored, 8 both) of levels first to last (1-based).
void unlockLevels(rewardTable &r, int32_t first, int32_t last, uint8 mode)
{
    for (int32_t level = first; level <= last; ++level)
        r.lvlAvailability[level - 1] = static_cast<uint8>(r.lvlAvailability[level - 1] | mode);
}

// The time-trial targets of one direction against the game's best times: each target the player has not beaten yet
// becomes the best time; returns how many levels have a better time. Each time is formatted into a buffer of the
// caller's frame, as GetTimeTrialTime (inline here) does.
int32_t countBeaten(ptr32<_sFrontGameStateData> pGameStateData, bool reversed, ptr32<char> buffer)
{
    int32_t beaten = 0;
    int32 *times = pGameStateData->gameConfig.timeTrials.times + (reversed ? 24 : 0);
    for (int32_t level0 = 0; level0 < 24; ++level0)
    {
        const ptr32<sExtraLevelData> pExtra = feLevel()->pExtraLevelData;
        if (!pExtra)
            continue;
        const int32_t target = targetTime(pExtra, level0, reversed);
        const int32_t better = keepBest(times[level0], target);
        beaten += better;
        HudFormatLapTime(buffer, better ? times[level0] : target);
    }
    return beaten;
}

} // namespace

// The rewards for the cups won (0 to 8) and the time trials beaten.
void SetRewards(ptr32<rewardTable> pRewards, ptr32<_sFrontGameStateData> pGameStateData, int32_t cup)
{
    GuestFrame frame(0x190);
    ClearAllSecrets(pRewards);
    rewardTable &r = *pRewards;
    if (cup > 0)
    {
        r.carArmoured = 1;
        unlockLevels(r, 7, 12, 1);
    }
    if (cup > 1)
    {
        r.carJungleRanger = 1;
        unlockLevels(r, 13, 18, 1);
    }
    if (cup > 2)
    {
        r.carDumper = 1;
        unlockLevels(r, 19, 24, 1);
    }
    if (cup > 3)
    {
        r.carYella = 1;
        unlockLevels(r, 1, 24, 4);
        unlockLevels(r, 1, 6, 2);
    }
    if (cup > 4)
    {
        r.carMoonrover = 1;
        unlockLevels(r, 7, 12, 2);
    }
    if (cup > 5)
    {
        r.carSarge = 1;
        unlockLevels(r, 13, 18, 2);
    }
    if (cup > 6)
    {
        r.carRocket = 1;
        unlockLevels(r, 19, 24, 2);
    }
    if (cup > 7)
    {
        r.mirrorMode = 1;
        r.carSkullduggery = 1;
        unlockLevels(r, 1, 24, 8);
    }
    if (countBeaten(pGameStateData, false, frame.local<char>(0)) == 24)
        r.carUFO = 1;
    if (countBeaten(pGameStateData, true, frame.local<char>(0x70)) == 24)
        r.carConcept2000 = 1;
}

// Shows cups 2 to 8 of the cup menu up to the cups won (all for flag 1, which also gives the time-trial cars).
void SetCupAvailability(int32_t flag)
{
    GuestFrame frame(0x30);
    sFrontEndClass &f = fe();
    const ptr32<sAMenuHeader> pCupMenu = groupWithTag(8u, 0x1000020bu)->pSourceHeader;
    ptr32<sMenuItemStd> p = pCupMenu->pMenuStart->pNext;
    ptr32<sMenuItemStd> q = p;
    for (int32_t i = 0; i < 7; ++i, q = q->pNext)
        q->flags = static_cast<uint16>(q->flags | 0x60u);
    if (flag == 1)
        f.gameStateData.gameConfig.cupsWon = 8;
    for (int32_t i = 1; i <= 7 && i <= f.gameStateData.gameConfig.cupsWon; ++i)
    {
        p->flags = static_cast<uint16>(p->flags & 0xff9fu);
        p = p->pNext;
    }
    SetRewards(ptr32<rewardTable>(guest_addr(&mainVars().rewards)), ptr32<_sFrontGameStateData>(guest_addr(&f.gameStateData)),
               f.gameStateData.gameConfig.cupsWon);
    if (flag == 1)
    {
        mainVars().rewards.carConcept2000 = 1;
        mainVars().rewards.carUFO = 1;
    }
}

// The camera tags of the current world's three locked track pictures (0 for a track already available).
void BuildLockedTracksList()
{
    sFrontEndClass &f = fe();
    uint32 *tags = f.lockedTrackCamTags;
    int32_t world = 0;
    switch (static_cast<uint32_t>(f.world))
    {
    case 0x10000053u: tags[1] = 0x100000a0u; tags[2] = 0x1000009fu; tags[0] = 0x100000a1u; world = 3; break;
    case 0x10000041u: tags[1] = 0x1000009cu; tags[2] = 0x1000009bu; tags[0] = 0x1000009du; world = 4; break;
    case 0x1000004fu: tags[1] = 0x100000a8u; tags[2] = 0x100000a7u; tags[0] = 0x100000a9u; world = 1; break;
    case 0x10000065u: tags[1] = 0x100000a4u; tags[2] = 0x100000a3u; tags[0] = 0x100000a5u; world = 2; break;
    case 0x10000056u: tags[1] = 0x10000098u; tags[2] = 0x10000097u; tags[0] = 0x10000099u; world = 5; break;
    case 0x10000299u: tags[1] = 0x1000029bu; tags[2] = 0x1000029au; tags[0] = 0x1000029cu; world = 6; break;
    default: tags[0] = tags[1] = tags[2] = 0; break;
    }
    // The world's tracks are levels world, world + 6 and world + 12 (each step taken only past a picture).
    const uint8 *available = mainVars().rewards.lvlAvailability;
    int32_t level = world + 6;
    if (tags[0] != 0)
    {
        if ((available[level - 1] & 1u) != 0)
            tags[0] = 0;
        level += 6;
    }
    if (tags[1] != 0)
    {
        if ((available[level - 1] & 1u) != 0)
            tags[1] = 0;
        level += 6;
    }
    if (tags[2] != 0 && (available[level - 1] & 1u) != 0)
        tags[2] = 0;
}

// After a time trial that beat its target: the message for the first time-trial car unlocked, and the car's message
// once every target of that direction is beaten. Mirrored runs count for neither.
void TimeTrialExitHandler()
{
    mainClass &m = mainVars();
    sFrontEndClass &f = fe();
    if (m.lastRaceType == 0x100001feu && static_cast<uint32_t>(f.world) != 0x1000006bu && m.timeTrialBeaten != 0)
    {
        uint32_t mode = m.reverseModeOnOff == 1 ? 1u : 0u;
        if (m.mirrorModeOnOff == 1)
            mode |= 2u;
        const bool reversed = mode == 1;
        if ((mode == 1 && m.rewards.carConcept2000 == 0) || (mode == 0 && m.rewards.carUFO == 0))
        {
            f.cupsWonMessage = reversed ? 0x10 : 0x0f;
            const ptr32<sExtraLevelData> pExtra = f.pFELevelData->pExtraLevelData;
            int32 *times = f.gameStateData.gameConfig.timeTrials.times + (reversed ? 24 : 0);
            int32_t beaten = 0;
            for (int32_t level0 = 0; level0 < 24; ++level0)
                if (pExtra)
                    beaten += keepBest(times[level0], targetTime(pExtra, level0, reversed));
            if (beaten == 24)
                f.cupsWonMessage = reversed ? 0x0a : 0x09;
        }
    }
    m.lastRaceType = 0;
}

// A level's time-trial best into the buffer (if any): the target if not beaten yet, which becomes the best time.
// Returns 1 if the best time beats the target.
int32_t GetTimeTrialTime(ptr32<char> pDestBuffer, ptr32<_sFrontGameStateData> pGameStateData, int32_t level, int32_t reverseFlag)
{
    GuestFrame frame(0x20);
    const ptr32<sExtraLevelData> pExtra = feLevel()->pExtraLevelData;
    if (!pExtra)
        return 0;
    int32 &slot = pGameStateData->gameConfig.timeTrials.times[reverseFlag * 24 + level - 1];
    const sExtraLevelData &e = pExtra[level - 1];
    const int32_t target = reverseFlag == 0 ? static_cast<int32_t>(e.tt1Mins * 60000u + e.tt1Secs * 1000u + e.tt1Tenths * 100u)
                                            : static_cast<int32_t>(e.tt2Mins * 60000u + e.tt2Secs * 1000u + e.tt2Tenths * 100u);
    const int32_t better = keepBest(slot, target);
    if (pDestBuffer)
        HudFormatLapTime(pDestBuffer, better ? slot : target);
    return better;
}

// Whether a level is unlocked in its mode: levels above 48 are mirrored, above 24 (after that) reversed.
int32_t CheckLevelAvailability(ptr32<rewardTable> pRewards, int32_t level)
{
    uint32_t mode = 0;
    if (level > 48)
    {
        mode = 1;
        level -= 48;
    }
    const uint8 available = pRewards->lvlAvailability[level - 1];
    if (level > 24)
        mode |= 2u;
    static const uint8 kBits[4] = {1, 2, 4, 8};
    return (available & kBits[mode]) != 0 ? 1 : 0;
}

void SetAllTracksAvailable(ptr32<rewardTable> pRewards)
{
    pRewards->mirrorMode = 1;
    for (uint8 &level : pRewards->lvlAvailability)
        level = 7;
}

// Whether a car type is unlocked (the secret cars by their reward, the others always).
int32_t CheckSingleCarAvailability(int32_t carType)
{
    const rewardTable &r = mainVars().rewards;
    switch (carType)
    {
    case 2: return r.carSkullduggery != 0;
    case 3: return r.carSarge != 0;
    case 7: return r.carJungleRanger != 0;
    case 0xb: return r.carConcept2000 != 0;
    case 0xe: return r.carYella != 0;
    case 0xf: return r.carUFO != 0;
    case 0x10: return r.carArmoured != 0;
    case 0x11: return r.carDumper != 0;
    case 0x12: return r.carMoonrover != 0;
    case 0x13: return r.carRocket != 0;
    default: return 1;
    }
}

// Marks the carousel entries of the unlocked secret cars available (writing through a null entry, at address 8, for a
// car not on the carousel, as the original does).
void CarouselSetAvailability()
{
    const rewardTable &r = mainVars().rewards;
    const struct
    {
        uint8 rewardTable::*reward;
        int32_t type;
    } kCars[] = {{&rewardTable::carUFO, 15},       {&rewardTable::carConcept2000, 11}, {&rewardTable::carJungleRanger, 7},
                 {&rewardTable::carYella, 14},     {&rewardTable::carSarge, 3},        {&rewardTable::carSkullduggery, 2},
                 {&rewardTable::carArmoured, 16},  {&rewardTable::carDumper, 17},      {&rewardTable::carMoonrover, 18},
                 {&rewardTable::carRocket, 19}};
    for (const auto &car : kCars)
        if (r.*car.reward != 0)
            carouselEntry(car.type)->flag = 1;
}

// The carousel entry's availability flag (read from address 8 for a car not on the carousel).
int32_t CheckCarAvailability(int32_t carType)
{
    return carouselEntry(carType)->flag;
}

ptr32<_sCarousel> GetCarouselPointer(int32_t carType)
{
    return carouselEntry(carType);
}

void EnableSingleCar(int32_t carType)
{
    carouselEntry(carType)->flag = 1;
}

} // namespace rc

RC_DECOMP_FN_SETUP(SetCheat, rc::fuzz::frontSfx)
RC_DECOMP_FN_SETUP(CheckCheatKeys, rc::fuzz::frontCheatKeys)
RC_DECOMP_FN_SETUP(SetAllCarsAvailable, rc::fuzz::frontRewardTable)
RC_DECOMP_FN_SETUP(SetCheatResults, rc::fuzz::frontCheatResults)
RC_DECOMP_FN_SETUP(ClearAllSecrets, rc::fuzz::frontRewardTable)
RC_DECOMP_FN_SETUP(SetRewards, rc::fuzz::frontSetRewards)
RC_DECOMP_FN_SETUP(SetCupAvailability, rc::fuzz::frontCupAvailability)
RC_DECOMP_FN_SETUP(BuildLockedTracksList, rc::fuzz::frontWorld)
RC_DECOMP_FN_SETUP(TimeTrialExitHandler, rc::fuzz::frontTimeTrialExit)
RC_DECOMP_FN_SETUP(GetTimeTrialTime, rc::fuzz::frontTimeTrialTime)
RC_DECOMP_FN_SETUP(CheckLevelAvailability, rc::fuzz::frontLevelAvailability)
RC_DECOMP_FN_SETUP(SetAllTracksAvailable, rc::fuzz::frontRewardTable)
RC_DECOMP_FN_SETUP(CheckSingleCarAvailability, rc::fuzz::frontCarType)
RC_DECOMP_FN_SETUP(CarouselSetAvailability, rc::fuzz::frontCarousel)
RC_DECOMP_FN_SETUP(CheckCarAvailability, rc::fuzz::frontCarType)
RC_DECOMP_FN_SETUP(GetCarouselPointer, rc::fuzz::frontCarType)
RC_DECOMP_FN_SETUP(EnableSingleCar, rc::fuzz::frontCarType)
