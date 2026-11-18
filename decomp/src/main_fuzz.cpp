// Builds MAIN.c's random-test inputs. See main_fuzz.h.

#include "main_fuzz.h"

#include <cstring>
#include <initializer_list>
#include <vector>

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

// --- Scripted callees: the boot (main), the mode loop, the track editor's set-up, frame loop and tear-down, and the
// shut-down. Offline each callee named by the setup becomes a fake that logs its call (the argument registers it reads)
// into scratch with a running hash, answers from the script and, after its call, applies the script's next change to
// mainVarG.status and toDo. After a chosen number of calls everything ends: status 8 (quit), every polled answer true
// and the music faded, so each loop the functions run is left. A callee the original reaches by a tail jump is not
// replaced (recompiled code calls a tail jump's target directly, past any replacement): it runs for real. ---

namespace {

constexpr uint32_t kEnv = kFuzzScratch + 0x7a00u, kScript = kFuzzScratch + 0x7800u, kLog = kFuzzScratch + 0x7c00u, kLogMax = 50u;
constexpr uint32_t kScriptEntries = 64u, kLeave = 0xffffffffu;
enum : uint32_t
{
    eCalls = 0x0,
    eLimit = 0x4,    // from this call on: status 8, polled answers true, the music faded
    eHash = 0x8,     // a hash of every logged call, so calls past the log's end are compared too
    eTrueMask = 0xc, // bit (call % 32): the answer of a polled call (sceSifRebootIop, sceSifSyncIop, Init3dFrame)
    eTime = 0x10,    // TimerGet's first answer (then 0x11 more a call)
    eFadeMask = 0x14, // bit (call % 32): RCMusicFadeControl ends the fade
    eAnswer = 0x18,  // every other fake's answer
};
constexpr uint32_t kMusicFading = sym::rcmusicVarG + 0x14u;

uint32_t env(uint32_t offset) { return mem<uint32_t>(kEnv + offset); }
void setEnv(uint32_t offset, uint32_t value) { mem<uint32_t>(kEnv + offset) = value; }

void logCall(uint32_t what, uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
    uint32_t hash = env(eHash);
    for (const uint32_t word : {what, a, b, c, d})
        hash = (hash ^ word) * 16777619u;
    setEnv(eHash, hash);
    const uint32_t count = mem<uint32_t>(kLog);
    if (count < kLogMax)
    {
        const uint32_t at = kLog + 4u + count * 20u;
        mem<uint32_t>(at) = what;
        mem<uint32_t>(at + 4u) = a;
        mem<uint32_t>(at + 8u) = b;
        mem<uint32_t>(at + 12u) = c;
        mem<uint32_t>(at + 16u) = d;
    }
    mem<uint32_t>(kLog) = count + 1u;
}

template <uint32_t Address, int Args>
void scripted(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    const auto arg = [ctx](int i) { return i < Args ? getRegU32(ctx, 4 + i) : 0u; };
    logCall(Address, arg(0), arg(1), arg(2), arg(3));
    const uint32_t n = env(eCalls);
    setEnv(eCalls, n + 1u);
    const bool over = n + 1u >= env(eLimit);
    const bool yes = over || ((env(eTrueMask) >> (n % 32u)) & 1u) != 0u;
    uint32_t answer = env(eAnswer);
    if constexpr (Address == sym::sceSifRebootIop || Address == sym::sceSifSyncIop || Address == sym::Init3dFrame)
        answer = yes ? 1u : 0u;
    else if constexpr (Address == sym::TimerGet)
        answer = env(eTime) + n * 0x11u;
    else if constexpr (Address == sym::RCMusicFadeControl)
    {
        if (over || ((env(eFadeMask) >> (n % 32u)) & 1u) != 0u)
            mem<uint32_t>(kMusicFading) = 0;
    }
    if (n < kScriptEntries)
    {
        const uint32_t newStatus = mem<uint32_t>(kScript + 8u * n), newToDo = mem<uint32_t>(kScript + 8u * n + 4u);
        if (newStatus != kLeave)
            mem<uint32_t>(kMain) = newStatus;
        if (newToDo != kLeave)
            mem<uint32_t>(kMain + 4u) = newToDo;
    }
    if (over)
    {
        mem<uint32_t>(kMain) = 8u;
        mem<uint32_t>(kMusicFading) = 0;
    }
    SET_GPR_S32(ctx, 2, static_cast<int32_t>(answer));
    ctx->pc = getRegU32(ctx, 31);
}

struct Fake
{
    uint32_t address;
    void (*fake)(uint8_t *, R5900Context *, PS2Runtime *);
};
#define RC_SCRIPTED(name, args) Fake{sym::name, &scripted<sym::name, args>}

// Installs the fakes (once per process: the oracle runs each function in its own) and starts a script: status values
// from `statuses`, modes 1-4 only (a mode outside them with no quit would spin with no call), the limit from limitRange.
void startScript(uint64_t &state, std::initializer_list<Fake> fakes, std::initializer_list<uint32_t> statuses, uint32_t limitLow, uint32_t limitRange)
{
    if (g_offline)
    {
        static bool installed = false;
        if (!installed)
        {
            installed = true;
            for (const Fake &f : fakes)
                g_runtime->replaceFunction(f.address, f.fake);
        }
    }
    soundOff();
    for (uint32_t offset = 0; offset < 0x40u; offset += 4u)
        setEnv(offset, 0);
    mem<uint32_t>(kLog) = 0;
    setEnv(eLimit, limitLow + rnd(state, limitRange));
    setEnv(eTrueMask, chance(state, 30) ? nextFuzz(state) | 0x80000000u : nextFuzz(state) | nextFuzz(state) | 0x80000000u);
    setEnv(eTime, nextFuzz(state));
    setEnv(eFadeMask, nextFuzz(state) & nextFuzz(state));
    setEnv(eAnswer, chance(state, 50) ? 0u : nextFuzz(state));
    const std::vector<uint32_t> statusList(statuses);
    for (uint32_t i = 0; i < kScriptEntries; ++i)
    {
        mem<uint32_t>(kScript + 8u * i) = chance(state, 70) ? kLeave : statusList[rnd(state, static_cast<uint32_t>(statusList.size()))];
        mem<uint32_t>(kScript + 8u * i + 4u) = chance(state, 70) ? kLeave : 1u + rnd(state, 4);
    }
    mem<uint32_t>(kMain) = statusList[rnd(state, static_cast<uint32_t>(statusList.size()))];
    mem<uint32_t>(kMain + 4u) = 1u + rnd(state, 4);
    mem<uint32_t>(kMusicFading) = rnd(state, 2);
}

} // namespace

void mainBoot(R5900Context *, uint64_t &state, uint32_t)
{
    startScript(state,
                {RC_SCRIPTED(__main, 0), RC_SCRIPTED(sceSifInitRpc, 1), RC_SCRIPTED(sceCdInit, 1), RC_SCRIPTED(sceSifRebootIop, 1),
                 RC_SCRIPTED(sceSifSyncIop, 0), RC_SCRIPTED(sceCdMmode, 1), RC_SCRIPTED(sceFsReset, 0), RC_SCRIPTED(sceSifLoadModule, 3),
                 RC_SCRIPTED(sceCdDiskReady, 1), RC_SCRIPTED(MainLoop, 0)},
                {1u}, 40u, 20u);
}

void mainLoop(R5900Context *, uint64_t &state, uint32_t)
{
    startScript(state,
                {RC_SCRIPTED(MainInit, 0), RC_SCRIPTED(MainFrontInit, 0), RC_SCRIPTED(MainFrontProcess, 0), RC_SCRIPTED(MainSetCars, 1),
                 RC_SCRIPTED(MainFrontTerm, 0), RC_SCRIPTED(LoadingInit, 1), RC_SCRIPTED(MainTrackEdInit, 0), RC_SCRIPTED(MainTrackEdProcess, 0),
                 RC_SCRIPTED(MainTrackEdTerm, 0), RC_SCRIPTED(MainGameInit, 0), RC_SCRIPTED(MainGameLevelInit, 0), RC_SCRIPTED(MainGameProcess, 0),
                 RC_SCRIPTED(MainGameLevelTerm, 0), RC_SCRIPTED(MainGameTerm, 0), RC_SCRIPTED(TaskSet, 4), RC_SCRIPTED(TaskCachedInsert, 1),
                 RC_SCRIPTED(MainTerm, 0)},
                {1u, 2u, 2u, 4u, 0x40u, 8u, 0u, 0x10u}, 10u, 60u);
}

void mainTrackEdInit(R5900Context *, uint64_t &state, uint32_t)
{
    startScript(state,
                {RC_SCRIPTED(RCMusicStop, 0), RC_SCRIPTED(LevelInit, 0), RC_SCRIPTED(TEditInit, 0), RC_SCRIPTED(CameraLvlInit, 1),
                 RC_SCRIPTED(TimerSetup, 3), RC_SCRIPTED(mpegOnInterruptTERM, 1), RC_SCRIPTED(RCMusicPlayLevel, 1), RC_SCRIPTED(RCMusicSetVol, 1),
                 RC_SCRIPTED(DisplaySetBackground, 3)},
                {1u}, 100u, 1u);
    mem<uint32_t>(kMain + 0x3acu) = nextFuzz(state) >> rnd(state, 32); // the music volume
    if (chance(state, 50))
        mem<uint8_t>(kMain + 0x34au) = static_cast<uint8_t>(rnd(state, 0x64));
}

void mainTrackEdProcess(R5900Context *, uint64_t &state, uint32_t)
{
    startScript(state,
                {RC_SCRIPTED(TimerGet, 1), RC_SCRIPTED(CameraSetMode, 2), RC_SCRIPTED(InputReset, 0), RC_SCRIPTED(Rdr_StartFrame, 0),
                 RC_SCRIPTED(RdrState_ApplyDefaultStateBlock, 0), RC_SCRIPTED(GfxP2_ZWriteEnable, 0), RC_SCRIPTED(InputSnapshot, 0),
                 RC_SCRIPTED(TEditControl, 1), RC_SCRIPTED(CameraProcess, 2), RC_SCRIPTED(SfxDoSfx, 0), RC_SCRIPTED(Init3dFrame, 4),
                 RC_SCRIPTED(TileFrameInit, 0), RC_SCRIPTED(TEditDisplay, 2), RC_SCRIPTED(FontPrint, 3), RC_SCRIPTED(DisplayRenderFrameProfile, 1),
                 RC_SCRIPTED(RdrAlphaAddToDmaChain, 0), RC_SCRIPTED(Rdr_EndFrame, 0), RC_SCRIPTED(RevSfxPauseFade, 1), RC_SCRIPTED(sceGsSyncV, 1),
                 RC_SCRIPTED(RCMusicFadeControl, 0)},
                {1u, 1u, 1u, 0u, 2u, 8u}, 20u, 120u);
    // The frame counter and the time it was last taken, so the once-a-second count runs now and then.
    mem<uint32_t>(kMain + 0x354u) = rnd(state, 100);
    mem<uint32_t>(kMain + 0x358u) = env(eTime) - rnd(state, 3000);
    mem<uint32_t>(sym::cameraVarG + 0xb70u) = sym::cameraVarG + 0x5b8u * rnd(state, 2); // the current camera
}

void mainTrackEdTerm(R5900Context *, uint64_t &state, uint32_t)
{
    startScript(state, {RC_SCRIPTED(TEditTerm, 0), RC_SCRIPTED(RdrAlphaTerm, 0), RC_SCRIPTED(LevelTerm, 0)}, {1u}, 100u, 1u);
}

void mainTerm(R5900Context *, uint64_t &state, uint32_t)
{
    startScript(state,
                {RC_SCRIPTED(CarListTerm, 0), RC_SCRIPTED(UserInterfaceTerm, 0), RC_SCRIPTED(TEditFreePermTrackStructures, 0),
                 RC_SCRIPTED(TexFreeIntermediateResData, 0), RC_SCRIPTED(PhysicsTerm, 0), RC_SCRIPTED(MemBlockFree, 1), RC_SCRIPTED(ResTerm, 0),
                 RC_SCRIPTED(LoadErrorTerm, 0), RC_SCRIPTED(CardUnInit, 0), RC_SCRIPTED(InputTerm, 0), RC_SCRIPTED(SysTimerTerm, 0),
                 RC_SCRIPTED(Init3dClean, 0), RC_SCRIPTED(DisplayTerm, 0), RC_SCRIPTED(BackdropTerm, 0), RC_SCRIPTED(MemBlockTerm, 0)},
                {1u}, 100u, 1u);
}

// The variable arguments, any 64-bit values, and $sp in scratch so the frame they are stored into is compared.
void mainPrintf(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    setArg(ctx, 0, scratch + 0x100u);
    for (int r = 5; r <= 11; ++r)
        SET_GPR_U64(ctx, r, (static_cast<uint64_t>(nextFuzz(state)) << 32) | nextFuzz(state));
    for (int f = 12; f < 20; ++f)
    {
        const uint32_t bits = nextFuzz(state);
        std::memcpy(&ctx->f[f], &bits, 4);
    }
    SET_GPR_U32(ctx, 29, scratch + 0x4000u + 16u * rnd(state, 0x100));
}

} // namespace rc::fuzz
