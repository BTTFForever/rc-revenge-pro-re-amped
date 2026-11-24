// Decompiled from MAIN.c: the race's end (results, cup ranking and championship points), the pause menu, the pads
// check and the choice of the computer's cars. See main.cpp for the start-up, level and frame loops.

#include "main.h"
#include "main_fuzz.h"
#include "../rc_widescreen.h"
#include <cstdio>
#include <cstdlib>

// Europe variant: CheckPads stores pauseReasonG in another order (the same body).
RC_VERSION_VARIANT(CheckPads)

namespace rc {

using namespace mainunit;

namespace {

constexpr int8_t kMark = -0x56; // 0xaa: the menu text's field marker

// Blanks a menu item's text after its first marker (the fields a results line fills in).
void blankAfterMark(uint32_t item)
{
    const uint32_t text = menuText(item);
    bool found = false;
    for (uint32_t i = 0; i < glen(text); ++i)
    {
        const int8_t c = at<int8_t>(text + i);
        if (c == kMark)
            found = true;
        if (found && c != kMark)
            at<uint8_t>(text + i) = ' ';
    }
}

uint32_t firstLine(uint32_t menuGlobal) { return at<uint32_t>(at<uint32_t>(at<uint32_t>(menuGlobal) + 4u) + 8u); }

// The human players' lines: blue for player 1, red for player 2.
void markPlayer(uint32_t item, uint32_t car)
{
    const uint8_t type = at<uint8_t>(car + 0x44u);
    if (type == 0)
        at<uint8_t>(item + 0x17u) |= 0x10;
    else if (type == 1)
        at<uint8_t>(item + 0x17u) |= 0x20;
}

} // namespace

// The results screen: each line gets its car's name (and, in a cup, the race points) after the markers. Returns 0 if
// a car has not finished yet.
int32_t BuildResults(int32_t flag)
{
    GuestFrame frame(0xc0);
    const uint32_t sp = frame.local<uint8_t>(0).address; // racePointsStr at +0, unfinishedCar at +0x10
    const int32_t total = carCount();
    at<uint32_t>(sp + 0x10) = 0;
    std::memcpy(guest_host(sp), guest_host(kRacePointsText), 5);
    uint32_t item = at<uint32_t>(at<uint32_t>(kResultsMenu) + 4u);
    if (cup() == 0)
    {
        // The title: blank between the second and third '*'.
        const uint32_t text = menuText(item);
        int32_t stars = 0;
        for (uint32_t i = 0; i < glen(text); ++i)
        {
            const int8_t c = at<int8_t>(text + i);
            if (c == '*')
                ++stars;
            if (stars == 2 && c != '*')
                at<uint8_t>(text + i) = ' ';
        }
    }
    item = at<uint32_t>(item + 8u);
    uint32_t result = 1;
    if (flag == 0)
    {
        for (; item; item = at<uint32_t>(item + 8u))
            blankAfterMark(item);
        at<uint8_t>(kMain + 0x34bu) = 0; // racePos
    }
    else if (total != 0)
    {
        for (int32_t i = 0; i < total; ++i)
        {
            const uint32_t car = orderCar(i);
            if (at<uint32_t>(car + 0xe8u) == 0)
                at<uint32_t>(sp + 0x10) = 1;
            else
            {
                uint32_t k = 0;
                int32_t found = 0;
                const uint32_t name = guest_call<uint32_t>(kGetCarText, at<uint32_t>(at<uint32_t>(car + 0xcu) + 4u));
                const uint32_t text = menuText(item);
                for (uint32_t j = 0; j < glen(text); ++j)
                {
                    if (found == 1 && at<int8_t>(name + k) != 0)
                    {
                        at<uint8_t>(text + j) = at<uint8_t>(name + k);
                        ++k;
                    }
                    const uint32_t p = text + j;
                    if (cup() != 0 && found == 2 && at<int8_t>(sp + k) != 0)
                    {
                        at<uint8_t>(p) = at<uint8_t>(sp + k);
                        ++k;
                    }
                    if (at<int8_t>(p) == kMark && ++found == 2)
                    {
                        k = 0;
                        const uint32_t n = glen(sp);
                        at<uint8_t>(sp + n - 1u) = static_cast<uint8_t>(at<uint8_t>(kRacePoints + static_cast<uint32_t>(i)) + 0x30);
                    }
                }
            }
            at<uint8_t>(item + 0x17u) &= 0xcf;
            markPlayer(item, orderCar(i));
            item = at<uint32_t>(item + 8u);
        }
        result = at<uint32_t>(sp + 0x10) ^ 1u;
    }
    int32_t i = 0;
    for (uint32_t line = firstLine(kResultsMenu); line; line = at<uint32_t>(line + 8u), ++i)
    {
        uint16_t &flags = at<uint16_t>(line + 0x14u);
        flags = i < total ? static_cast<uint16_t>(flags & 0xffdf) : static_cast<uint16_t>(flags | 0x20);
    }
    return result != 0 ? 1 : 0;
}

void ClearRanking()
{
    GuestFrame frame(0xa0);
    const int32_t total = carCount();
    for (uint32_t item = firstLine(kRankingMenu); item; item = at<uint32_t>(item + 8u))
        blankAfterMark(item);
    for (int32_t i = 0; i < total; ++i)
    {
        at<uint8_t>(orderAt(i) + 6u) = 0;
        at<uint8_t>(orderAt(i) + 7u) = 0;
    }
}

// Bubble sort of the race order by cup points (+7), player 1's car first among equals.
void SortRanking()
{
    GuestFrame frame(0x90);
    const uint32_t sp = frame.local<uint8_t>(0).address; // swapOrder
    const int32_t n = carCount();
    // $a1 holds the computer cars' count at the call, as in the original: TagToCar returns it for an unknown tag (0 in
    // a demo race), which then decides ties.
    const uint32_t p1 = guest_call<uint32_t>(kTagToCar, at<uint32_t>((sym::frontEndVarG + 0x280u)),
                                             static_cast<uint32_t>(at<uint8_t>(sym::mainVarG + 0x36au)));
    bool change;
    do
    {
        change = false;
        if (n - 1 <= 0)
            break;
        uint32_t held = at<uint32_t>(sp);
        for (int32_t k = 0; k + 1 < n; ++k)
        {
            const uint32_t a = orderAt(k), b = orderAt(k + 1);
            const uint8_t pa = at<uint8_t>(a + 7u), pb = at<uint8_t>(b + 7u);
            bool swap = pa < pb;
            if (!swap && pa == pb)
            {
                const uint32_t tb = at<uint32_t>(at<uint32_t>(at<uint32_t>(b) + 0xcu) + 4u);
                swap = tb == p1 && at<uint32_t>(at<uint32_t>(at<uint32_t>(a) + 0xcu) + 4u) != tb;
            }
            if (!swap)
                continue;
            change = true;
            held = at<uint32_t>(a);
            const uint8_t a4 = at<uint8_t>(a + 4u), a5 = at<uint8_t>(a + 5u), a6 = at<uint8_t>(a + 6u);
            at<uint8_t>(sp + 7) = pa;
            at<uint32_t>(a) = at<uint32_t>(b);
            at<uint8_t>(sp + 4) = a4;
            at<uint8_t>(sp + 5) = a5;
            at<uint8_t>(sp + 6) = a6;
            for (uint32_t f = 4; f < 8; ++f)
                at<uint8_t>(a + f) = at<uint8_t>(b + f);
            at<uint32_t>(b) = held;
            at<uint8_t>(b + 7u) = pa;
            at<uint8_t>(b + 4u) = a4;
            at<uint8_t>(b + 5u) = a5;
            at<uint8_t>(b + 6u) = a6;
        }
        at<uint32_t>(sp) = held;
    } while (change);
}

// The cup ranking: lines filled from the last place up, the points counting up one per call (flag 0) until each
// car's race points reach its total. Returns 0 while still counting.
int32_t BuildRanking(int32_t flag)
{
    GuestFrame frame(0xe0);
    const uint32_t sp = frame.local<uint8_t>(0).address; // pMenuItems at +0, racePointsStr at +0x20
    std::memcpy(guest_host(sp + 0x20), guest_host(kRacePointsText), 5);
    uint32_t item = firstLine(kRankingMenu);
    at<int32_t>(sp + 0x30) = flag;
    at<uint32_t>(sp + 0x38) = 0; // change
    const int32_t n = carCount();
    for (int32_t k = 0; k < n; ++k)
    {
        at<uint32_t>(sp + 4u * static_cast<uint32_t>(k)) = item;
        item = at<uint32_t>(item + 8u);
    }
    SortRanking();
    for (int32_t i = n - 1; i >= 0 && at<uint32_t>(sp + 0x38) != 1; --i)
    {
        const uint32_t slot = sp + 4u * static_cast<uint32_t>(i);
        markPlayer(at<uint32_t>(slot), orderCar(i));
        const uint32_t text = menuText(at<uint32_t>(slot));
        const uint32_t name = guest_call<uint32_t>(kGetCarText, at<uint32_t>(at<uint32_t>(orderCar(i) + 0xcu) + 4u));
        at<uint32_t>(sp + 0x34) = name;
        const uint32_t points = orderAt(i) + 6u;
        if (at<int32_t>(sp + 0x30) == 0)
        {
            if (at<uint8_t>(points) < at<uint8_t>(points + 1u))
            {
                at<uint8_t>(points) = static_cast<uint8_t>(at<uint8_t>(points) + 1);
                at<uint32_t>(sp + 0x38) = 1;
            }
        }
        else
            at<uint8_t>(points) = at<uint8_t>(points + 1u);
        uint32_t k = 0;
        int32_t found = 0;
        for (uint32_t j = 0; j < glen(text); ++j)
        {
            if (found == 1 && at<int8_t>(name + k) != 0)
            {
                at<uint8_t>(text + j) = at<uint8_t>(name + k);
                ++k;
            }
            const uint32_t p = text + j;
            if (found == 2 && at<int8_t>(sp + 0x20 + k) != 0)
            {
                at<uint8_t>(p) = at<uint8_t>(sp + 0x20 + k);
                ++k;
            }
            if (at<int8_t>(p) == kMark && ++found == 2)
            {
                k = 0;
                const uint32_t v = at<uint8_t>(points);
                const uint32_t tens = (v / 10u) & 0xffu, units = (v % 10u) & 0xffu;
                at<uint8_t>(sp + 0x20 + glen(sp + 0x20) - 2u) = static_cast<uint8_t>(tens > 0 ? tens + 0x30 : 0x20);
                at<uint8_t>(sp + 0x20 + glen(sp + 0x20) - 1u) = static_cast<uint8_t>(units + 0x30);
            }
        }
    }
    return static_cast<int32_t>(at<uint32_t>(sp + 0x38) ^ 1u);
}

namespace enhance {
void poll(); // rc_enhance_menu.cpp
}

namespace {

// The pause menu was left: music and sounds back.
void pauseMenuMusic()
{
    const uint32_t music = kMusic;
    if (at<uint32_t>(music + 0x14u) == 0)
        guest_call(kRCMusicPause, 0);
    guest_call(kRevSfxStopPauseSfx);
    if (at<uint32_t>(music + 0x14u) == 0)
        at<uint32_t>(music + 0x18u) = 0;
    if (at<int8_t>(music + 0x2bu) != 0)
    {
        at<uint8_t>(music + 0x2bu) = 0;
        guest_call(kRCMusicSetVol, 0);
    }
}

} // namespace

// The pause menu (start pressed during a race) and, after the race (status 0x20), the steps of the results, cup
// ranking and race-over screens.
namespace {

// PauseMenuProcess's body, in the original's frame. Returns the options menu to draw (a tail call made outside the
// frame), or 0 for none.
uint32_t pauseMenuStep(uint32_t pm)
{
    GuestFrame frame(0x70);
    const auto pmWord = [&](uint32_t off) -> uint32_t & { return at<uint32_t>(pm + off); };
    const auto pmHalf = [&](uint32_t off) -> uint16_t & { return at<uint16_t>(pm + off); };
    if (at<uint16_t>(kPauseDelay) != 0)
        at<uint16_t>(kPauseDelay) = static_cast<uint16_t>(at<uint16_t>(kPauseDelay) - 1);
    if (pmWord(0) != 0x20)
    {
        const uint32_t car0 = at<uint32_t>(kObjectList);
        if (car0 && (at<uint32_t>(car0) & 0x8000000u) == 0)
            guest_call(kInputSetVibrationFlags, 0, at<uint32_t>((sym::frontEndVarG + 0x154u)) == 0 ? 1 : 0);
        if (guest_call<uint32_t>(kInputPadPresent, 5) != 0 && at<uint32_t>(kMain + 0x34cu) == 0x10000200u)
        {
            const uint32_t car1 = at<uint32_t>(kObjectList + 4u);
            if (car1 && (at<uint32_t>(car1) & 0x8000000u) == 0)
                guest_call(kInputSetVibrationFlags, 4, at<uint32_t>((sym::frontEndVarG + 0x174u)) == 0 ? 1 : 0);
        }
    }
    int32_t showOptions = 0;
    if (pmWord(0x174) < 2)
    {
        uint32_t hit1 = guest_call<uint32_t>(kInputHit, 3), hit2 = 0;
        if (at<uint32_t>(kMain + 0x34cu) == 0x10000200u)
            hit2 = guest_call<uint32_t>(kInputHit, 0x13);
        uint32_t paused = pmHalf(0x366);
        if (at<uint16_t>(kPauseDelay) != 0 && paused == 0)
            hit1 = hit2 = 0;
        if (status() != 0x10 && (hit1 != 0 || hit2 != 0))
        {
            const uint32_t menu = at<uint32_t>(kPauseMenu);
            if (paused == 0)
            {
                const uint32_t first = at<uint32_t>(at<uint32_t>(menu + 0x14u) + 4u);
                at<uint16_t>(first + 0x14u) &= 0xfffa;
                if (hit1 != 0)
                {
                    at<uint16_t>(kMain + 0x364u) = 1;
                    at<uint16_t>(first + 0x14u) |= 1;
                }
                else
                {
                    at<uint16_t>(kMain + 0x364u) = 2;
                    at<uint16_t>(first + 0x14u) |= 4;
                }
                const uint32_t options = at<uint32_t>(menu + 0x14u);
                pmHalf(0x366) = 1;
                at<uint32_t>(menu) = options;
                guest_call(kInputResetAllHit);
                guest_call(kTimerPause, pm + 0x10u);
                guest_call(kRevSfxPauseFade, 1);
                guest_call(kRCMusicPause, 0xf);
                paused = pmHalf(0x366);
            }
            else if ((hit1 != 0 && at<int16_t>(kMain + 0x364u) == 1) || (hit2 != 0 && at<int16_t>(kMain + 0x364u) == 2))
            {
                // Start again by the player who paused: back to the race.
                guest_call(kAMenuProcessAll, at<uint32_t>(at<uint32_t>(kPauseMenu)));
                guest_call(kMenuItemDisabler, at<uint32_t>(at<uint32_t>(kPauseMenu)));
                if (pmWord(0x10) & 1u)
                {
                    guest_call(kTimerResume, pm + 0x10u);
                    pmHalf(0x366) = 0;
                }
                guest_call(kRCMusicResume, 0xf);
                guest_call(kRevSfxPauseFade, 2);
                guest_call(kRevSfxStopPauseSfx);
                pmHalf(0x366) = 0;
                paused = 0;
            }
        }
        if (paused == 1)
        {
            at<uint16_t>(kPauseDelay) = 10;
            guest_call(kAMenuProcessAll, at<uint32_t>(at<uint32_t>(kPauseMenu)));
            guest_call(kMenuItemDisabler, at<uint32_t>(at<uint32_t>(kPauseMenu)));
            enhance::poll(); // enhanced recomp: Pause Menu > Game Options pages
            const uint32_t menu0 = at<uint32_t>(at<uint32_t>(kPauseMenu));
            const uint32_t mit = guest_call<uint32_t>(kAMenuGetMIT, menu0, static_cast<int32_t>(at<int8_t>(menu0 + 0x63u)));
            const uint32_t current = at<uint32_t>(mit + 4u);
            const uint32_t tag = current ? at<uint32_t>(current + 4u) : 0;
            if (current && (tag == 0x1000001fu || tag == 0x10000026u))
            {
                // The music or effects volume slider.
                uint32_t changed = 0;
                if (tag == 0x1000001fu)
                    at<uint32_t>(kMusic + 0x20u) = at<uint32_t>(current + 0xcu);
                if (at<uint32_t>(current + 4u) == 0x10000026u)
                    at<uint32_t>(kRevSfx + 0x1144u) = at<uint32_t>(current + 0xcu);
                if (at<uint32_t>(kMusic + 0x20u) != at<uint32_t>(kMusic + 0x24u) ||
                    at<uint32_t>(kRevSfx + 0x1144u) != at<uint32_t>(kRevSfx + 0x1148u))
                {
                    changed = 1;
                    if (at<uint32_t>(kMusic + 0x150u) != 0)
                        at<uint8_t>(kMusic + 0x2bu) = 1;
                }
                if (changed && std::getenv("RC_ENHANCE_DEBUG")) // enhanced recomp: the bars moved to Game Options > Audio Setup
                    std::fprintf(stderr, "[enhance-debug] pause volumes: music %u effects %u\n", at<uint32_t>(kMusic + 0x20u),
                                 at<uint32_t>(kRevSfx + 0x1144u));
                guest_call(kRCMusicModifyVolume, 0xf, changed);
                if (at<uint32_t>(kRevSfx + 0x1144u) != at<uint32_t>(kRevSfx + 0x1148u))
                    guest_call(kRevSfxModifyVolume, changed);
            }
            else
                pauseMenuMusic();
            const uint32_t chosen = at<uint32_t>(at<uint32_t>(kPauseMenu) + 0x68u);
            if (chosen == 0x10000223u)
            {
                // Quit.
                pmHalf(0x366) = 0;
                pmWord(0) = 2;
                guest_call(kRCMusicStop);
                guest_call(kRevSfxStopAll, 0);
            }
            else if (chosen == 0x10000217u)
            {
                // Continue.
                guest_call(kRCMusicResume, 0x19);
                if (pmWord(0x10) & 1u)
                    guest_call(kTimerResume, pm + 0x10u);
                pmHalf(0x366) = 0;
            }
            else if (chosen == 0x10000224u)
            {
                // Restart.
                if (at<uint32_t>(kMain + 0xcu) == 1)
                    toDo() = 2;
                else
                {
                    pmWord(4) = 1;
                    pmWord(0x350) = pmWord(0x34c);
                }
                pmHalf(0x366) = 0;
                pmWord(0) = 4;
                guest_call(kRevSfxStopAll, 1);
            }
        }
    }
    if (status() != 0x20)
        return 0;
    const int32_t total = carCount();
    if (guest_call<uint32_t>(kInputHit, 0xe) != 0 || guest_call<uint32_t>(kInputHit, 0x1e) != 0)
    {
        pmWord(0x174) = pmWord(0x174) + 1;
        guest_call(kInputResetAllHit);
    }
    const uint32_t step = pmWord(0x174);
    switch (step)
    {
    case 1:
        if (at<uint32_t>(kMain + 0x34cu) == 0x100001fcu || at<uint32_t>(kMain + 0x34cu) == 0x10000200u)
            showOptions = 1;
        break;
    case 2:
        if (at<uint32_t>(kMain + 0x34cu) == 0x100001feu)
            MainRaceOver(0);
        else
        {
            if ((at<uint32_t>(kMain + 0x10u) & 1u) == 0)
                guest_call(kRevSfxPauseFade, 1);
            pmWord(0x174) = 3;
        }
        break;
    case 3:
        if (BuildResults(1) == 0)
            for (int32_t i = 0; i < total; ++i)
                at<uint32_t>(orderCar(i) + 0xe8u) += 1;
        guest_call(kAMenuDisplayAll, at<uint32_t>(kResultsMenu));
        break;
    case 4:
        for (int32_t i = 0; i < total; ++i)
            at<uint8_t>(orderAt(i) + 7u) =
                static_cast<uint8_t>(at<uint8_t>(orderAt(i) + 6u) + at<uint8_t>(kRacePoints + static_cast<uint32_t>(i)));
        pmWord(0x174) = pmWord(0x174) + 1;
        break;
    case 5:
        if (cup() != 0)
        {
            if (BuildRanking(0) == 1)
                pmWord(0x174) = 6;
            guest_call(kAMenuDisplayAll, at<uint32_t>(kRankingMenu));
        }
        else
            MainRaceOver(0);
        break;
    case 6:
        BuildRanking(1);
        guest_call(kAMenuDisplayAll, at<uint32_t>(kRankingMenu));
        break;
    case 7:
        guest_call(kTimerPause, pm + 0x10u);
        MainRaceOver(0);
        break;
    default:
        break;
    }
    if (pmHalf(0x366) == 1)
        return 0;
    return showOptions ? kPauseOptions2 : kPauseOptions;
}

} // namespace

void PauseMenuProcess(ptr32<mainClass> pMainVar)
{
    const widescreen::RaceMenuScope menus; // widescreen: these menus pillarboxed in split screen too
    const uint32_t options = pauseMenuStep(pMainVar.address);
    if (options != 0)
        guest_call(kAMenuDisplayOptions, at<uint32_t>(options));
}

// The computer's cars must differ from the players': the cup's list without the players' cars, in order.
void MainSetCarsRemoveDuplicates()
{
    const int32_t n = carCount();
    int32_t p1Skip = -1, p2Skip = -1;
    const auto cupCar = [](int32_t i) { return at<uint32_t>(kMain + 0x178u + 4u * static_cast<uint32_t>(i)); };
    if (noHuman() != 0)
    {
        const uint32_t p1 = at<uint8_t>(kMain + 0x3dfu);
        for (int32_t i = 0; i < n; ++i)
            if (cupCar(i) == p1)
                p1Skip = i;
    }
    if (noHuman() >= 2)
    {
        const uint32_t p2 = at<uint8_t>(kMain + 0x3e0u);
        for (int32_t i = 0; i < n; ++i)
            if (cupCar(i) == p2)
                p2Skip = i;
    }
    int32_t j = 0;
    for (int32_t k = noHuman(); k < n; ++k)
    {
        if (j == p1Skip)
            ++j;
        if (j == p2Skip)
            ++j;
        if (j == p1Skip)
            ++j;
        at<uint32_t>(kCarList + 12u * static_cast<uint32_t>(k)) = cupCar(j);
        ++j;
    }
}

// The computer's cars: the front end's line-up (0), a random set without repeats (1, 2), or a cup's eight from the
// cars available (3).
void MainSetCars(eCarSet carSet)
{
    const uint32_t set = static_cast<uint32_t>(carSet);
    {
        GuestFrame frame(0x70);
        if (set >= 3)
        {
            if (set != 3)
                return;
            if (cup() == 0)
            {
                const uint32_t list = kMain + 0x178u;
                for (int32_t i = 0; i < 8; ++i)
                {
                    const int32_t r = guest_call<int32_t>(kRand);
                    const uint32_t car = static_cast<uint32_t>(r % 0x13 + 1);
                    at<uint32_t>(list + 4u * static_cast<uint32_t>(i)) = car;
                    if (guest_call<int32_t>(kCheckCarAvailability, car) != 1)
                    {
                        --i;
                        continue;
                    }
                    for (int32_t j = 0; j < i; ++j)
                        if (at<uint32_t>(list + 4u * static_cast<uint32_t>(i)) == at<uint32_t>(list + 4u * static_cast<uint32_t>(j)))
                        {
                            --i;
                            break;
                        }
                }
            }
        }
        else if (set == 0)
        {
            static constexpr uint32_t kLineUp[8] = {0xc, 0xa, 1, 0xd, 5, 6, 7, 3};
            for (uint32_t i = 0; i < 8; ++i)
                at<uint32_t>(kCarList + 12u * i) = kLineUp[i];
            return;
        }
        else
        {
            for (int32_t i = 0; i < noAI(); ++i)
            {
                const int32_t r = guest_call<int32_t>(kRand);
                at<uint32_t>(kCarList + 12u * static_cast<uint32_t>(i)) = static_cast<uint32_t>(r % 0x13 + 1);
                for (int32_t j = 0; j < i; ++j)
                    if (at<uint32_t>(kCarList + 12u * static_cast<uint32_t>(i)) == at<uint32_t>(kCarList + 12u * static_cast<uint32_t>(j)))
                    {
                        --i;
                        break;
                    }
            }
            for (int32_t i = 0; i < noAI(); ++i)
                guest_call(kPrintf, (sym::sec_rodata + 0x1070u), at<uint32_t>(kCarList + 12u * static_cast<uint32_t>(i)));
            return;
        }
    }
    MainSetCarsRemoveDuplicates();
}

void MainDemoEndTask(ptr32<void>, tTime)
{
    {
        GuestFrame frame(0x20);
        toDo() = 1;
        at<uint16_t>(kMain + 0x366u) = 0;
        status() = 4;
        guest_call(kRevSfxStopAll, 1);
    }
    guest_call(kTaskDelete, kMain + 0x37cu);
}

// The race is over: back to the front end, or (in a cup) on to the next race, or after the sixth to the ranking
// (and the cup won if player 1 came first).
void MainRaceOver(int32_t flag)
{
    {
        GuestFrame frame(0x90);
        if (at<uint32_t>(kMain + 0xcu) == 1)
        {
            status() = 4;
            toDo() = 2;
            at<uint16_t>(kMain + 0x366u) = 0;
            at<uint32_t>(kResultTrigger) = 0x1000;
            return;
        }
        const int32_t n = carCount();
        guest_call(kInputResetHit, 0xe);
        guest_call(kInputResetHit, 0x1e);
        const uint32_t cupNo = cup();
        if (cupNo == 0 || flag == 1)
        {
            toDo() = 1;
            if (flag == 0)
                status() = 4;
            else
            {
                at<uint32_t>(kResultTrigger) = 0x1000;
                status() = 0x40;
            }
            at<uint16_t>(kMain + 0x366u) = 0;
            at<uint32_t>(kMain + 0x350u) = at<uint32_t>(kMain + 0x34cu);
        }
        else
        {
            const uint32_t p1 = at<uint32_t>(kCarList);
            for (int32_t i = 0; i < n && carType(orderCar(i)) != p1; ++i)
            {
            }
            const uint8_t race = static_cast<uint8_t>(at<uint8_t>(kMain + 0x349u) + 1);
            at<uint8_t>(kMain + 0x349u) = race;
            if (race == 6)
            {
                if (at<int32_t>(kFrontEnd + 0x25cu) < static_cast<int32_t>(cupNo))
                {
                    const uint32_t p1Car = at<uint32_t>(kCarList);
                    int32_t best = 0, p1Score = 0;
                    for (int32_t i = 0; i < n; ++i)
                    {
                        const int32_t score = at<uint8_t>(orderAt(i) + 6u);
                        if (carType(orderCar(i)) == p1Car)
                            p1Score = score;
                        if (best < score)
                            best = score;
                    }
                    if (!(p1Score < best))
                    {
                        at<uint32_t>(kFrontEnd + 0x25cu) = cupNo;
                        at<uint8_t>(kFrontEnd + 0x2dau) = at<uint8_t>(kFrontEnd + 0x298u);
                    }
                }
                MainRaceOver(1);
            }
            else
            {
                toDo() = 3;
                at<uint16_t>(kMain + 0x366u) = 0;
                status() = 4;
                at<uint8_t>(kMain + 0x34au) = at<uint8_t>(kMain + 0x340u + at<uint8_t>(kMain + 0x349u));
            }
        }
        guest_call(kRCMusicPause, 0xa);
    }
    guest_call(kRevSfxPauseFade, 1);
}

// Keeps each car's cup points (by car type) and player 1's place.
void SaveChampionshipPoints()
{
    const int32_t n = carCount();
    for (int32_t i = 0; i < n; ++i)
    {
        const uint8_t points = at<uint8_t>(orderAt(i) + 6u);
        at<uint32_t>(kMain + 0x198u + 4u * static_cast<uint32_t>(i)) = carType(orderCar(i));
        at<uint8_t>(kMain + 0x1b8u + static_cast<uint32_t>(i)) = points;
    }
    for (uint32_t i = 0; i < at<uint32_t>(kObjectList + 0x170u); ++i)
        if (at<uint8_t>(orderCar(static_cast<int32_t>(i)) + 0x44u) == 0)
            at<uint16_t>(kAi + 0x64u) = static_cast<uint16_t>(i + 1);
}

// A missing pad pauses the game with a "connect the controller" box; returns which player's (1, 2) or 0.
int32_t CheckPads(int32_t numHumans)
{
    GuestFrame frame(0x40);
    int32_t missing = guest_call<uint32_t>(kInputPadPresent, 0) == 0 ? 1 : 0;
    if (missing == 0 && numHumans == 2)
        missing = guest_call<uint32_t>(kInputPadPresent, 5) == 0 ? numHumans : 0;
    if (missing != 0)
    {
        at<uint16_t>(kMain + 0x366u) = 1;
        guest_call(kInputResetAllHit);
        if ((at<uint32_t>(kMain + 0x10u) & 1u) == 0)
        {
            guest_call(kTimerPause, kMain + 0x10u);
            at<uint32_t>(kPadFlags) |= 2u;
        }
        guest_call(kRevSfxPauseFade, 1);
        if (missing == 1)
            guest_call(kAMenuDisplay, at<uint32_t>(kPadMenu1));
        else if (missing == 2)
            guest_call(kAMenuDisplay, at<uint32_t>(kPadMenu2));
        return missing;
    }
    const uint32_t flags = at<uint32_t>(kPadFlags) & 0xfffffffdu;
    at<uint32_t>(kPadFlags) = flags;
    if ((at<uint32_t>(kMain + 0x10u) & 1u) != 0 && flags == 0 &&
        (at<uint16_t>(kMain + 0x366u) == 0 || at<uint8_t>(kMain + 0x34au) == 0x62))
        guest_call(kTimerResume, kMain + 0x10u);
    return 0;
}

// Points the pause menu's message links at the level's messages with their tags. Returns how many it found.
int32_t PauseMenuScan()
{
    int32_t found = 0;
    for (uint32_t p = at<uint32_t>(at<uint32_t>(kLevel) + 0x44u); at<int32_t>(p) != -1; p += 0x14u)
    {
        const uint32_t message = at<uint32_t>(p);
        const uint32_t tag = at<uint32_t>(p + 8u);
        for (uint32_t j = 0; at<uint32_t>(kPauseLinks + 8u * j) != 0; ++j)
            if (at<uint32_t>(kPauseLinks + 8u * j) == tag)
            {
                at<uint32_t>(at<uint32_t>(kPauseLinks + 8u * j + 4u)) = message;
                ++found;
                break;
            }
    }
    return found;
}

void ClearRankingDisplay()
{
    GuestFrame frame(0x80);
    const int32_t n = carCount();
    uint32_t item = firstLine(kRankingMenu);
    for (int32_t k = 0; k < n; ++k)
    {
        const uint32_t text = menuText(item);
        for (uint32_t j = 0; j < glen(text); ++j)
            if (at<int8_t>(text + j) != kMark)
                at<uint8_t>(text + j) = ' ';
        item = at<uint32_t>(item + 8u);
    }
}

void ClearChampionshipPoints()
{
    const int32_t n = carCount();
    for (int32_t i = 0; i < n; ++i)
        at<uint8_t>(kMain + 0x1b8u + static_cast<uint32_t>(i)) = 0;
}

// Gives each car back the cup points kept for its type.
void SetChampionshipPoints()
{
    const int32_t n = carCount();
    for (int32_t i = 0; i < n; ++i)
    {
        const uint32_t car = orderCar(i);
        for (int32_t j = 0; j < n; ++j)
            if (carType(car) == at<uint32_t>(kMain + 0x198u + 4u * static_cast<uint32_t>(j)))
            {
                at<uint8_t>(orderAt(i) + 7u) = 0;
                at<uint8_t>(orderAt(i) + 6u) = at<uint8_t>(kMain + 0x1b8u + static_cast<uint32_t>(j));
            }
    }
}

} // namespace rc

RC_DECOMP_FN_SETUP_HEAP(BuildResults, rc::fuzz::mainResults)
RC_DECOMP_FN_SETUP_HEAP(ClearRanking, rc::fuzz::mainResults)
RC_DECOMP_FN_SETUP_HEAP(SortRanking, rc::fuzz::mainResults)
RC_DECOMP_FN_SETUP_HEAP(BuildRanking, rc::fuzz::mainResults)
RC_DECOMP_FN_SETUP_HEAP(PauseMenuProcess, rc::fuzz::mainPause)
RC_DECOMP_FN_SETUP_HEAP(MainSetCarsRemoveDuplicates, rc::fuzz::mainCars)
RC_DECOMP_FN_SETUP_HEAP(MainSetCars, rc::fuzz::mainCars)
RC_DECOMP_FN_SETUP_HEAP(MainDemoEndTask, rc::fuzz::mainNone)
RC_DECOMP_FN_SETUP_HEAP(MainRaceOver, rc::fuzz::mainResults)
RC_DECOMP_FN_SETUP_HEAP(SaveChampionshipPoints, rc::fuzz::mainResults)
RC_DECOMP_FN_SETUP_HEAP(CheckPads, rc::fuzz::mainPads)
RC_DECOMP_FN_SETUP_HEAP(PauseMenuScan, rc::fuzz::mainScan)
RC_DECOMP_FN_SETUP_HEAP(ClearRankingDisplay, rc::fuzz::mainResults)
RC_DECOMP_FN_SETUP_HEAP(ClearChampionshipPoints, rc::fuzz::mainResults)
RC_DECOMP_FN_SETUP_HEAP(SetChampionshipPoints, rc::fuzz::mainResults)
