// Builds MAIN.c's random-test inputs. See main_fuzz.h.

#include "main_fuzz.h"

#include <cstring>

namespace rc::fuzz {

namespace {

constexpr uint32_t kMain = sym::mainVarG, kFrontEnd = sym::frontEndVarG, kAi = sym::aiVarG, kObjectList = sym::objectVarG, kInput = sym::inputVarG;
constexpr uint32_t kCarousel = sym::carouselVarG + 0x20u, kSfxInitOk = sym::sfxVarG;

template <typename T>
T &mem(uint32_t address) { return *static_cast<T *>(guest_host(address)); }

uint32_t rnd(uint64_t &state, uint32_t n) { return n == 0 ? 0u : nextFuzz(state) % n; }
bool chance(uint64_t &state, uint32_t percent) { return rnd(state, 100) < percent; }
void setArg(R5900Context *ctx, int index, uint32_t value) { SET_GPR_S32(ctx, 4 + index, static_cast<int32_t>(value)); }
bool inRam(uint32_t address) { return address != 0 && address < 0x2000000u; }

void soundOff() { mem<uint8_t>(kSfxInitOk) = 0; }

// The race order's cars that are real objects: how many leading entries can be used.
uint32_t usableOrder()
{
    uint32_t n = 0;
    while (n < 8 && inRam(mem<uint32_t>(kAi + 4u + 12u * n)) && inRam(mem<uint32_t>(mem<uint32_t>(kAi + 4u + 12u * n) + 0xcu)))
        ++n;
    return n;
}

// Players and computer cars (no more than the race order holds), the cup, its points, the cars' finish flags.
void varyRace(uint64_t &state)
{
    const uint32_t usable = usableOrder();
    if (chance(state, 60) && usable > 0)
    {
        const uint32_t total = rnd(state, usable + 1);
        const uint32_t humans = total == 0 ? 0 : rnd(state, total < 2 ? total + 1 : 3);
        mem<uint8_t>(kMain + 0x369u) = static_cast<uint8_t>(humans);
        mem<uint8_t>(kMain + 0x36au) = static_cast<uint8_t>(total - humans);
    }
    else if (mem<uint8_t>(kMain + 0x369u) + mem<uint8_t>(kMain + 0x36au) > usable)
    {
        mem<uint8_t>(kMain + 0x369u) = static_cast<uint8_t>(usable > 0 ? 1 : 0);
        mem<uint8_t>(kMain + 0x36au) = static_cast<uint8_t>(usable > 0 ? usable - 1 : 0);
    }
    if (chance(state, 60))
        mem<uint32_t>(kFrontEnd + 0x298u) = chance(state, 30) ? 0 : 1 + rnd(state, 6); // cup
    for (uint32_t i = 0; i < 8; ++i)
    {
        const uint32_t order = kAi + 4u + 12u * i;
        if (chance(state, 50))
        {
            mem<uint8_t>(order + 6u) = static_cast<uint8_t>(rnd(state, 40));
            mem<uint8_t>(order + 7u) = static_cast<uint8_t>(chance(state, 30) ? mem<uint8_t>(order + 6u) : rnd(state, 60));
        }
        if (chance(state, 30))
            for (uint32_t b = 4; b < 6; ++b)
                mem<uint8_t>(order + b) = static_cast<uint8_t>(nextFuzz(state));
        if (i < usable && chance(state, 40))
            mem<uint32_t>(mem<uint32_t>(order) + 0xe8u) = chance(state, 70) ? 1 + rnd(state, 3) : 0; // finished
        if (i < usable && chance(state, 30))
            mem<uint8_t>(mem<uint32_t>(order) + 0x44u) = static_cast<uint8_t>(rnd(state, 3)); // playerType
        if (chance(state, 40))
            mem<uint8_t>(kMain - 0x4a0u + i) = static_cast<uint8_t>(rnd(state, 11)); // racePointsG (0x1ed460)
        if (chance(state, 40))
        {
            mem<uint32_t>(kMain + 0x198u + 4u * i) = chance(state, 70) ? 1 + rnd(state, 19) : nextFuzz(state);
            mem<uint8_t>(kMain + 0x1b8u + i) = static_cast<uint8_t>(rnd(state, 60));
        }
    }
    if (chance(state, 30) && usable >= 2) // shuffle the order a little
    {
        const uint32_t a = rnd(state, usable), b = rnd(state, usable);
        uint8_t tmp[12];
        std::memcpy(tmp, guest_host(kAi + 4u + 12u * a), 12);
        std::memcpy(guest_host(kAi + 4u + 12u * a), guest_host(kAi + 4u + 12u * b), 12);
        std::memcpy(guest_host(kAi + 4u + 12u * b), tmp, 12);
    }
    if (chance(state, 40))
        mem<uint8_t>(kMain + 0x349u) = static_cast<uint8_t>(rnd(state, 7)); // race of the cup
    if (chance(state, 30))
        for (uint32_t i = 0; i < 8; ++i)
            mem<uint8_t>(kMain + 0x340u + i) = static_cast<uint8_t>(rnd(state, 0x64));
    if (chance(state, 20))
        mem<uint32_t>(kMain + 0xcu) = rnd(state, 2); // trackEdRace
    if (chance(state, 30))
        mem<int32_t>(kFrontEnd + 0x25cu) = static_cast<int32_t>(rnd(state, 8)) - 1;
    if (chance(state, 30))
        mem<uint32_t>(sym::carListVarG) = 1 + rnd(state, 0x26); // carListVarG.car[0].type: player 1's
    if (chance(state, 30))
    {
        static const uint32_t kConfigs[] = {0x100001fcu, 0x100001feu, 0x10000200u, 0x10000201u};
        mem<uint32_t>(kMain + 0x34cu) = kConfigs[rnd(state, 4)];
    }
    if (chance(state, 20))
        mem<uint32_t>(kMain + 0x10u) ^= 1u; // timer paused
}

} // namespace

void mainNone(R5900Context *ctx, uint64_t &state, uint32_t)
{
    soundOff();
    setArg(ctx, 0, nextFuzz(state));
    setArg(ctx, 1, nextFuzz(state));
}

void mainScan(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    mainNone(ctx, state, scratch);
    // The pause menu's links (11, ending with key 0): now and then a key repeated, or the list cut short.
    const uint32_t links = sym::links_600;
    if (chance(state, 40))
    {
        const uint32_t a = rnd(state, 10), b = rnd(state, 10);
        mem<uint32_t>(links + 8u * b) = mem<uint32_t>(links + 8u * a);
    }
    if (chance(state, 20))
        mem<uint32_t>(links + 8u * rnd(state, 10)) = 0;
}

void mainResults(R5900Context *ctx, uint64_t &state, uint32_t)
{
    soundOff();
    varyRace(state);
    setArg(ctx, 0, chance(state, 10) ? nextFuzz(state) : rnd(state, 3));
}

void mainPause(R5900Context *ctx, uint64_t &state, uint32_t)
{
    soundOff();
    varyRace(state);
    static const uint32_t kStatus[] = {1, 1, 0x10, 0x20, 0x20, 0x20, 2, 4};
    mem<uint32_t>(kMain) = kStatus[rnd(state, 8)];
    if (chance(state, 70))
        mem<uint32_t>(kMain + 0x174u) = rnd(state, 10); // results step
    if (chance(state, 60))
        mem<uint16_t>(kMain + 0x366u) = static_cast<uint16_t>(rnd(state, 3)); // paused
    if (chance(state, 60))
        mem<uint16_t>(kMain + 0x364u) = static_cast<uint16_t>(rnd(state, 3)); // who paused
    if (chance(state, 50))
        mem<uint16_t>(sym::rePauseCountG) = static_cast<uint16_t>(rnd(state, 3)); // pause delay
    for (uint32_t key : {3u, 0x13u, 0xeu, 0x1eu})
        if (chance(state, 30))
            mem<uint8_t>(kInput + 0x5cu + key) |= 0x20; // a hit
    for (int k = 0; k < 2; ++k)
        if (chance(state, 30))
            mem<int32_t>(kInput + 4u * static_cast<uint32_t>(k)) = static_cast<int32_t>(rnd(state, 2)); // pad present
    if (chance(state, 40))
    {
        // The pause menu's current item and choice.
        const uint32_t menu = mem<uint32_t>(sym::pMenuSystemG);
        if (inRam(menu))
        {
            static const uint32_t kChoices[] = {0x10000223u, 0x10000217u, 0x10000224u, 0, 0x10000222u};
            mem<uint32_t>(menu + 0x68u) = kChoices[rnd(state, 5)];
        }
    }
    if (chance(state, 30))
    {
        mem<uint32_t>(sym::rcmusicVarG + 0x20u) = rnd(state, 0x100); // music volume and its last value
        mem<uint32_t>(sym::revSfxVarG + 0x1144u) = rnd(state, 0x100);
        mem<uint32_t>(sym::rcmusicVarG + 0x150u) = rnd(state, 2);
        mem<uint8_t>(sym::rcmusicVarG + 0x2bu) = static_cast<uint8_t>(rnd(state, 2));
    }
    setArg(ctx, 0, kMain);
}

void mainPads(R5900Context *ctx, uint64_t &state, uint32_t)
{
    soundOff();
    for (int k = 0; k < 2; ++k)
        if (chance(state, 50))
            mem<int32_t>(kInput + 4u * static_cast<uint32_t>(k)) = static_cast<int32_t>(rnd(state, 2));
    if (chance(state, 50))
        mem<uint32_t>(kMain + 0x10u) ^= 1u;
    if (chance(state, 50))
        mem<uint32_t>(sym::pauseReasonG) = rnd(state, 4);
    if (chance(state, 50))
        mem<uint16_t>(kMain + 0x366u) = static_cast<uint16_t>(rnd(state, 2));
    if (chance(state, 30))
        mem<uint8_t>(kMain + 0x34au) = static_cast<uint8_t>(chance(state, 50) ? 0x62 : rnd(state, 0x64));
    setArg(ctx, 0, chance(state, 10) ? nextFuzz(state) : rnd(state, 3));
}

void mainCars(R5900Context *ctx, uint64_t &state, uint32_t)
{
    soundOff();
    const uint32_t humans = rnd(state, 3), ai = rnd(state, 8 - humans);
    mem<uint8_t>(kMain + 0x369u) = static_cast<uint8_t>(humans);
    mem<uint8_t>(kMain + 0x36au) = static_cast<uint8_t>(ai);
    mem<uint8_t>(kMain + 0x3dfu) = static_cast<uint8_t>(1 + rnd(state, 19)); // p1Car
    mem<uint8_t>(kMain + 0x3e0u) = static_cast<uint8_t>(1 + rnd(state, 19)); // p2Car
    if (chance(state, 60))
        for (uint32_t i = 0; i < 8; ++i)
            mem<uint32_t>(kMain + 0x178u + 4u * i) = 1 + rnd(state, 19); // cupCarList
    if (chance(state, 50))
    {
        // The players' cars in the cup's list, often next to each other (either way round).
        const uint32_t at1 = rnd(state, 8), at2 = chance(state, 60) ? (at1 + (chance(state, 50) ? 1 : 7)) % 8 : rnd(state, 8);
        mem<uint32_t>(kMain + 0x178u + 4u * at2) = mem<uint8_t>(kMain + 0x3e0u);
        mem<uint32_t>(kMain + 0x178u + 4u * at1) = mem<uint8_t>(kMain + 0x3dfu);
    }
    mem<uint32_t>(kFrontEnd + 0x298u) = chance(state, 50) ? 0 : 1 + rnd(state, 6);
    // The carousel: at least eight cars available, so the cup's draw ends.
    uint32_t available = 0;
    for (uint32_t i = 0; i < 19; ++i)
    {
        const uint32_t c = kCarousel + i * 0x34u;
        mem<uint32_t>(c) = i + 1;
        const bool on = chance(state, 70) || 19 - i <= 8 - available;
        mem<uint16_t>(c + 8u) = static_cast<uint16_t>(on ? 1 : (chance(state, 50) ? 0 : 2));
        available += on ? 1 : 0;
    }
    setArg(ctx, 0, chance(state, 5) ? 4 + rnd(state, 4) : rnd(state, 4));
}

void mainScreen(R5900Context *ctx, uint64_t &state, uint32_t)
{
    soundOff();
    setArg(ctx, 0, chance(state, 20) ? nextFuzz(state) : rnd(state, 2));
}

void mainFrameWindow(R5900Context *ctx, uint64_t &state, uint32_t)
{
    soundOff();
    if (chance(state, 50))
        mem<uint8_t>(kMain + 0x368u) = static_cast<uint8_t>(rnd(state, 2)); // split screen
    setArg(ctx, 0, chance(state, 5) ? nextFuzz(state) % 6 : rnd(state, 4));
    setArg(ctx, 1, mem<uint32_t>(kMain + 0x1cu) + rnd(state, 0x4000));
}

} // namespace rc::fuzz
