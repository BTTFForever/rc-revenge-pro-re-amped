// Builds the random-test inputs of FRONT.c, CAROUSEL.c and CREDITS.c. See front_fuzz.h.

#include "front_fuzz.h"

#include <cstdlib>
#include <algorithm>
#include <cstring>
#include <unistd.h>

namespace rc::fuzz {

namespace {

// Scratch layout.
constexpr uint32_t kFELevel = 0x0000u, kGroups = 0x0140u, kHeaders = 0x0400u, kItems = 0x0900u, kVars = 0x0ea0u;
constexpr uint32_t kTextGroups = 0x1100u, kStrings = 0x1600u, kExtra = 0x2200u, kCups = 0x23a0u, kStats = 0x24e0u;
constexpr uint32_t kCameras = 0x2580u, kPaths = 0x2600u, kCarDefs = 0x2640u, kBuffer = 0x2d00u, kQuestion = 0x3100u;
constexpr uint32_t kCredits = 0x3180u, kTsos = 0x3280u, kEffects = 0x3a00u, kLoadSave = 0x3e00u, kKeyTables = 0x3f00u;
constexpr uint32_t kCamera = 0x3f40u, kHandle = 0x3f80u;
constexpr int kHeaderCount = 10, kItemCount = 60, kVarCount = 20, kTextGroupCount = 80, kStringCount = 96, kTsoCount = 30;
constexpr uint32_t kEnv = kFuzzScratch + 0x7b00u, kLog = kFuzzScratch + 0x7c00u, kLogMax = 48u;
enum : uint32_t { eSeed = 0, eCalls = 4, eHash = 8, eTime = 12, eFrames = 16, eFrameLimit = 20, eTso = 24, eObject = 28, eVsync = 32 };

constexpr uint32_t kMain = sym::mainVarG, kFrontEnd = sym::frontEndVarG, kMenuSystem = sym::menuSystemG;

template <typename T>
T &mem(uint32_t address) { return *static_cast<T *>(guest_host(address)); }

struct Rng
{
    uint64_t &state;
    uint32_t next() { return nextFuzz(state); }
    uint32_t below(uint32_t n) { return n ? next() % n : 0u; }
    int32_t between(int32_t low, int32_t high) { return low + static_cast<int32_t>(below(static_cast<uint32_t>(high - low + 1))); }
    bool chance(uint32_t percent) { return below(100) < percent; }
    template <typename T, size_t N>
    T pick(const T (&values)[N]) { return values[below(static_cast<uint32_t>(N))]; }
};

void setArg(R5900Context *ctx, int index, uint32_t value) { SET_GPR_S32(ctx, 4 + index, static_cast<int32_t>(value)); }
uint32_t arg(R5900Context *ctx, int index) { return getRegU32(ctx, 4 + index); }
bool inRam(uint32_t address) { return address >= 0x100000u && address < 0x2000000u; }

uint32_t mix(uint32_t x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

// A value from the input's seed, the same sequence in both runs.
uint32_t draw()
{
    const uint32_t k = mem<uint32_t>(kEnv + eCalls);
    mem<uint32_t>(kEnv + eCalls) = k + 1u;
    return mix(mem<uint32_t>(kEnv + eSeed) + k * 0x9e3779b9u);
}

void logEntry(uint32_t what, uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
    static ::FILE *trace = std::getenv("RC_FRONTFUZZ_TRACE") ? std::fopen(std::getenv("RC_FRONTFUZZ_TRACE"), "a") : nullptr; // each logged call, appended to this file
    if (trace)
    {
        std::fprintf(trace, "[front_fuzz %d] %x %x %x %x %x\n", static_cast<int>(getpid()), what, a, b, c, d);
        std::fflush(trace);
    }
    uint32_t hash = mem<uint32_t>(kEnv + eHash);
    for (const uint32_t word : {what, a, b, c, d})
        hash = (hash ^ word) * 16777619u;
    mem<uint32_t>(kEnv + eHash) = hash;
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

// The object pool the stand-ins hand out (the dump's own car objects, with physics, a render object and wheels).
uint32_t poolObject(uint32_t n)
{
    static uint32_t objects[46];
    static uint32_t count = 0;
    static bool read = false;
    if (!read)
    {
        read = true;
        for (uint32_t i = 0; i < 46u; ++i)
        {
            const uint32_t o = mem<uint32_t>(sym::objectVarG + 4u * i);
            if (inRam(o) && inRam(mem<uint32_t>(o + 0x20u)) && inRam(mem<uint32_t>(o + 0x18u)) && inRam(mem<uint32_t>(o + 0xcu)) &&
                inRam(mem<uint32_t>(mem<uint32_t>(o + 0x18u))) && inRam(mem<uint32_t>(mem<uint32_t>(o + 0x20u) + 0x1e8u)))
                objects[count++] = o;
        }
    }
    return count ? objects[n % count] : 0u;
}

// The functions replaced offline: each logs its arguments and answers from the seed.
uint32_t answer(uint32_t address, R5900Context *ctx)
{
    const uint32_t s = kFuzzScratch;
    if (address == sym::TimerGet)
    {
        const uint32_t t = mem<uint32_t>(kEnv + eTime) + 8u + (draw() & 31u);
        mem<uint32_t>(kEnv + eTime) = t;
        return t;
    }
    if (address == sym::rand)
        return draw() & 0x7fffffffu;
    if (address == sym::Init3dFrame)
        return draw() % 100u < 97u ? 1u : 0u;
    if (address == sym::CheckPads)
        return draw() % 100u < 85u ? 0u : 1u;
    if (address == sym::InputTestAnyKeyHeld || address == sym::LoadSaveCheckIfCardInSlot1 || address == sym::LoadSaveAutoLoadSavedGame)
        return draw() & 1u;
    if (address == sym::LoadSaveProcess)
    {
        static const uint32_t kResults[] = {0xffffffffu, 0, 0, 1, 1};
        return kResults[draw() % 5u];
    }
    if (address == sym::LoadSaveCheckFreeSpaceOnCards || address == sym::LoadSaveKeepCheckingFreeSpaceOnCards)
        return draw() % 6u;
    if (address == sym::AMenuNewProcessInput)
    {
        static const uint32_t kKeys[] = {0, 0, 0, 0, 0, 0, 1, 2, 3, 5, 6, 11};
        return kKeys[draw() % 12u];
    }
    if (address == sym::GetCameraFromTag)
        return s + kCamera;
    if (address == sym::TSOFindByTag)
    {
        const uint32_t n = mem<uint32_t>(kEnv + eTso);
        mem<uint32_t>(kEnv + eTso) = n + 1u;
        return s + kTsos + (n % kTsoCount) * 0x3cu;
    }
    if (address == sym::ObjectCreate)
    {
        const uint32_t n = mem<uint32_t>(kEnv + eObject);
        mem<uint32_t>(kEnv + eObject) = n + 1u;
        return poolObject(n);
    }
    if (address == sym::AColDownCast)
    {
        for (uint32_t i = 0; i < 4u; ++i)
        {
            mem<uint32_t>(arg(ctx, 1) + 4u * i) = draw() & 0xffffu;
            mem<uint32_t>(arg(ctx, 2) + 4u * i) = draw() & 0x1fffu;
        }
        mem<uint8_t>(arg(ctx, 3)) = static_cast<uint8_t>(draw());
        return draw() & 1u;
    }
    if (address == sym::NodeCalcWorldPos)
    {
        for (uint32_t i = 0; i < 3u; ++i)
            mem<int32_t>(arg(ctx, 3) + 4u * i) = static_cast<int32_t>(draw() % 0x40000u) - 0x20000;
        return 0;
    }
    if (address == sym::RendObjRender)
    {
        // What is drawn: the render object's head and position (a stack copy for the question marks and wheels).
        const uint32_t r = arg(ctx, 0);
        logEntry(0x52454e44u, mem<uint32_t>(r), mem<uint32_t>(r + 4u), mem<uint32_t>(r + 8u), mem<uint32_t>(r + 0xcu));
        logEntry(0x4d415458u, mem<uint32_t>(r + 0x18u), mem<uint32_t>(r + 0x1cu), mem<uint32_t>(r + 0x20u), mem<uint32_t>(r + 0x24u));
        return draw() & 0x7fffu;
    }
    if (address == sym::MemBlockAllocLo)
    {
        mem<uint32_t>(arg(ctx, 0)) = s + kBuffer;
        return 0;
    }
    if (address == sym::MemBlockFree)
    {
        mem<uint32_t>(arg(ctx, 0)) = 0;
        return 0;
    }
    if (address == sym::Rdr_EndFrame)
    {
        // The frame loops (FrontEndProcess, CreditsDisplay) end after a few frames; the field counter runs on.
        const uint32_t frames = mem<uint32_t>(kEnv + eFrames) + 1u;
        mem<uint32_t>(kEnv + eFrames) = frames;
        mem<uint32_t>(sym::displayVarG + 0x64u) = mem<uint32_t>(sym::displayVarG + 0x64u) + mem<uint32_t>(kEnv + eVsync) + (draw() & 1u);
        if (frames >= mem<uint32_t>(kEnv + eFrameLimit))
        {
            mem<uint32_t>(kMain) = 1;
            mem<uint32_t>(kMenuSystem + 0x68u) = 0x11000000u;
        }
        return 0;
    }
    if (address == sym::RCMusicFadeControl)
    {
        if (draw() % 3u == 0)
            mem<uint32_t>(sym::rcmusicVarG + 0x14u) = 0;
        return 0;
    }
    return draw() & 0xffu;
}

// Logs the arguments the function takes (the others hold whatever the caller left).
template <uint32_t Address, int Args>
void stub(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    const auto a = [ctx](int i) { return i < Args ? arg(ctx, i) : 0u; };
    logEntry(Address, a(0), a(1), a(2), a(3) ^ a(4) << 8 ^ a(5) << 16 ^ a(6) << 24);
    SET_GPR_S32(ctx, 2, static_cast<int32_t>(answer(Address, ctx)));
    ctx->pc = getRegU32(ctx, 31);
}

struct Stub
{
    uint32_t address;
    RecompFn fn;
};

#define FRONT_STUB(name, args) Stub{sym::name, &stub<sym::name, args>}
const Stub kStubs[] = {
    FRONT_STUB(AColDownCast, 4), FRONT_STUB(AColLvlInit, 0), FRONT_STUB(AColTerm, 0), FRONT_STUB(AIInit, 1), FRONT_STUB(AMenuDisplayOptions, 1),
    FRONT_STUB(AMenuNewProcessInput, 3), FRONT_STUB(AMenuProcessAll, 1), FRONT_STUB(AMenuSnapsize, 1),
    FRONT_STUB(AnimReinit, 0), FRONT_STUB(AnimTerm, 0), FRONT_STUB(AnimUpdateAnims, 0), FRONT_STUB(BuildProgress, 3), FRONT_STUB(BuildTimeTrials, 1),
    FRONT_STUB(CameraLvlInit, 1), FRONT_STUB(CameraLvlTerm, 0), FRONT_STUB(CameraPitchFixup, 1), FRONT_STUB(CameraProcess, 2),
    FRONT_STUB(CameraSetMode, 2), FRONT_STUB(CameraTerm, 0), FRONT_STUB(CellRenderInit, 1), FRONT_STUB(CheckPads, 1),
    FRONT_STUB(DepthCueInit, 0), FRONT_STUB(DepthCueSet, 2), FRONT_STUB(DisplayRenderFrameProfile, 1), FRONT_STUB(FontCentre, 2), FRONT_STUB(GameTaskLvlInit, 0), FRONT_STUB(GameTaskLvlTerm, 0), FRONT_STUB(GetCameraFromTag, 2), FRONT_STUB(GfxP2_RenderDmaSend, 0),
    FRONT_STUB(GfxTex_Update, 0), FRONT_STUB(HudDrawLockedTracks, 0), FRONT_STUB(HudInit, 0), FRONT_STUB(HudTerm, 0), FRONT_STUB(Init3dChangeViewcone, 1),
    FRONT_STUB(Init3dFrame, 4), FRONT_STUB(InputResetAllHit, 0), FRONT_STUB(InputSnapshot, 0), FRONT_STUB(InputTestAnyKeyHeld, 0), FRONT_STUB(LevelInit, 0),
    FRONT_STUB(LevelLoad, 1), FRONT_STUB(LevelTerm, 0), FRONT_STUB(LoadingCallbackTerm, 0), FRONT_STUB(LoadingPlayFlyingLogo, 0),
    FRONT_STUB(LoadingShowFlyingLogo, 0), FRONT_STUB(LoadSaveAutoLoadSavedGame, 0), FRONT_STUB(LoadSaveCheckFreeSpaceOnCards, 0),
    FRONT_STUB(LoadSaveCheckIfCardInSlot1, 0), FRONT_STUB(LoadSaveInit, 1), FRONT_STUB(LoadSaveKeepCheckingFreeSpaceOnCards, 1),
    FRONT_STUB(LoadSaveProcess, 1), FRONT_STUB(LoadSaveSetupClass, 1), FRONT_STUB(LsFrameInit, 3), FRONT_STUB(LsRenderWorld, 0),
    FRONT_STUB(MemBlockAllocLo, 2), FRONT_STUB(MemBlockFree, 1), FRONT_STUB(MeshInitFrame, 0), FRONT_STUB(MoveUIbox, 1), FRONT_STUB(NodeCalcWorldPos, 4),
    FRONT_STUB(ObjectClearSfx, 1), FRONT_STUB(ObjectCreate, 3), FRONT_STUB(ObjectDeleteAll, 0), FRONT_STUB(ObjectInit, 0), FRONT_STUB(ObjectRenderAll, 0),
    FRONT_STUB(ObjectRenderLights, 1), FRONT_STUB(ObjectResetAllTint, 0), FRONT_STUB(ObjectSfxFrontEnd, 1), FRONT_STUB(ObjectTerm, 0),
    FRONT_STUB(OColLvlInit, 0), FRONT_STUB(OColTerm, 0), FRONT_STUB(PathCopy, 2), FRONT_STUB(PathInit, 0), FRONT_STUB(PathPilotEndCallback, 1),
    FRONT_STUB(PathTerm, 0), FRONT_STUB(PhysicsLevelInit, 0), FRONT_STUB(PhysicsLevelTerm, 0), FRONT_STUB(rand, 0),
    FRONT_STUB(RandomSetSeed, 1), FRONT_STUB(RCMusicFadeControl, 0), FRONT_STUB(RCMusicModifyVolume, 2),
    FRONT_STUB(RCMusicPause, 1), FRONT_STUB(RCMusicPlayLevel, 1), FRONT_STUB(RCMusicSwitchAlternate, 1),
    FRONT_STUB(RdrAlphaAddToDmaChain, 0), FRONT_STUB(Rdr_EndFrame, 0), FRONT_STUB(Rdr_ShapeInit, 0),
    FRONT_STUB(Rdr_ShapeTerm, 0), FRONT_STUB(Rdr_SkinInit, 0), FRONT_STUB(Rdr_SkinTerm, 0), FRONT_STUB(Rdr_StartFrame, 0),
    FRONT_STUB(RdrState_ApplyDefaultStateBlock, 0), FRONT_STUB(RdrZedsWriteOff, 1), FRONT_STUB(RendObjRender, 4), FRONT_STUB(ResLoad, 1),
    FRONT_STUB(RevSfxInitAmbient, 1),
    FRONT_STUB(RevSfxModifyVolume, 1), FRONT_STUB(RevSfxPauseFade, 1), FRONT_STUB(RevSfxStopAll, 1), FRONT_STUB(RevSfxStopPauseSfx, 0),
    FRONT_STUB(RevSfxUpdateAmbient, 1), FRONT_STUB(sceGsSyncV, 1), FRONT_STUB(sceMcInit, 0), FRONT_STUB(SfxDoSfx, 0),
    FRONT_STUB(SkinAnimInit, 0), FRONT_STUB(TaskDelete, 1),
    FRONT_STUB(TaskInit, 0), FRONT_STUB(TaskProcess, 1), FRONT_STUB(TaskSet, 4), FRONT_STUB(TaskTerm, 0), FRONT_STUB(TBoxInit, 0), FRONT_STUB(TBoxTerm, 0),
    FRONT_STUB(TColLvlInit, 0), FRONT_STUB(TColTerm, 0), FRONT_STUB(TimerGet, 1), FRONT_STUB(TimerPause, 1), FRONT_STUB(TimerResume, 1),
    FRONT_STUB(TimerSetup, 3), FRONT_STUB(TSOFindByTag, 1), FRONT_STUB(TSOInit, 0), FRONT_STUB(TSORender, 1), FRONT_STUB(TSOTerm, 0),
    FRONT_STUB(VersionDisplay, 0), FRONT_STUB(Vfx2PodiumInitialise, 0), FRONT_STUB(VfxAddNew, 4), FRONT_STUB(VfxAddNewPlace, 4),
    FRONT_STUB(VfxCallHandlers, 0), FRONT_STUB(VfxInit, 0), FRONT_STUB(VfxRender, 0), FRONT_STUB(VfxRenderShadows, 0), FRONT_STUB(VfxSetupCameraMatrix, 0),
    FRONT_STUB(VfxTerm, 0), FRONT_STUB(WheelInit, 0), FRONT_STUB(WheelTerm, 0),
};
#undef FRONT_STUB

// Installs the stand-ins (once per test process). Not replaced: the functions some function of the units (or one they
// call) reaches by a tail jump (AMenuDrawBox, CameraSetNewPath, FontSet, RCMusicChain, RCMusicSetVol,
// RdrPolyFlush, ResTypeSet/UnSetConstructDaemon, SetCameraToThisTag, SfxPlay, SkinAnimTerm, TaskCachedInsert, printf):
// recompiled code jumps straight to those, past the function table, and the original's callees are the recompiled ones
// in the test, so a stand-in would see only the native's calls. ObjectDeleteAll is replaced but kept by the setups
// whose function reaches it by CarouselTerm's tail jump.
void installStubs(std::initializer_list<uint32_t> keep)
{
    if (!g_offline)
        return;
    static bool installed = false;
    if (installed)
        return;
    installed = true;
    for (const Stub &s : kStubs)
        if (std::find(keep.begin(), keep.end(), s.address) == keep.end())
            g_runtime->replaceFunction(s.address, s.fn);
}

// The menu drawing that runs for real where a function draws a menu through the native AMenuDisplayAll, whose callees
// are native calls (not through the function table) there.
#define FRONT_MENU_DRAWING sym::AMenuDisplayAll, sym::AMenuSnapsize, sym::AMenuDisplayOptions, sym::AMenuNewProcessInput, sym::GfxP2_RenderDmaSend

// The world of one input: the front end's level data, UI groups, menus, items, texts, tracks, cups, cars and the
// global state, all in scratch or in the globals.
class World
{
public:
    World(R5900Context *ctx, Rng &rng, uint32_t scratch, std::initializer_list<uint32_t> keep = {}) : rng(rng), s(scratch)
    {
        installStubs(keep);
        logEntry(0xffffffffu, 0, 0, 0, 0); // an input begins (hashed before the env is cleared below; shows in the trace)
        for (uint32_t offset = 0; offset < 0x40u; offset += 4u)
            mem<uint32_t>(kEnv + offset) = 0;
        mem<uint32_t>(kLog) = 0;
        mem<uint32_t>(kEnv + eSeed) = rng.next();
        mem<uint32_t>(kEnv + eTime) = rng.next() & 0xfffffu;
        mem<uint32_t>(kEnv + eFrameLimit) = static_cast<uint32_t>(rng.between(1, 6));
        mem<uint32_t>(kEnv + eVsync) = static_cast<uint32_t>(rng.between(1, 40));
        mem<uint32_t>(kEnv + eObject) = rng.below(8);
        std::memset(guest_host(0), 0, 0x100); // null pointers are followed in places; low memory reads as zero
        // Stale stack words show through partly built sprite and rectangle parameters.
        const uint32_t sp = getRegU32(ctx, 29);
        for (uint32_t offset = 0x400; offset >= 4; offset -= 4)
            mem<uint32_t>(sp - offset) = rng.next() & 0x3f3f3f3fu;
        language = rng.between(0, 3);
        build();
    }

    Rng &rng;
    const uint32_t s;
    int32_t language = 0;
    int groups = 0, headers = 0, items = 0, vars = 0, textGroups = 0, strings = 0;
    uint32_t firstMenu = 0, cupMenu = 0, carMenu = 0, statsMenu = 0, optionsMenu = 0;

    uint32_t header(int n) const { return s + kHeaders + static_cast<uint32_t>(n) * 0x80u; }
    uint32_t group(int n) const { return s + kGroups + static_cast<uint32_t>(n) * 0x14u; }

    uint32_t string(const char *text)
    {
        const uint32_t at = s + kStrings + static_cast<uint32_t>(strings++ % kStringCount) * 0x20u;
        std::strncpy(static_cast<char *>(guest_host(at)), text, 0x1f);
        mem<char>(at + 0x1fu) = 0;
        return at;
    }

    uint32_t word()
    {
        char text[12];
        const int n = rng.between(1, 10);
        for (int i = 0; i < n; ++i)
            text[i] = static_cast<char>('a' + rng.below(26));
        text[n] = 0;
        if (rng.chance(10))
            text[0] = rng.chance(50) ? '*' : '@';
        return string(text);
    }

    uint32_t textGroup()
    {
        const uint32_t at = s + kTextGroups + static_cast<uint32_t>(textGroups++ % kTextGroupCount) * 0x10u;
        const uint32_t base = word();
        for (uint32_t l = 0; l < 4; ++l)
            mem<uint32_t>(at + 4u * l) = base;
        if (rng.chance(50))
            mem<uint32_t>(at + 4u * static_cast<uint32_t>(language)) = word();
        return at;
    }

    // A car-selection or other tag of the kinds the code tests for.
    uint32_t carTag()
    {
        static const uint16_t kTags[] = {0x38, 0x39, 0x3a, 0x3b, 0x3c, 0x3f, 0x40, 0x42, 0x43, 0x45, 0x46, 0x48, 0x4d, 0x4e, 0x51,
                                         0x52, 0x58, 0x59, 0x5b, 0x5c, 0x5f, 0x60, 0x62, 0x63, 0x68, 0x69, 0x70, 0x71, 0x72, 0x73,
                                         0x291, 0x292, 0x293, 0x294, 0x295, 0x296, 0x297, 0x298};
        return 0x10000000u | rng.pick(kTags);
    }

    uint32_t anyTag()
    {
        static const uint16_t kOther[] = {0x3d, 0x3e, 0x41, 0x44, 0x47, 0x49, 0x4a, 0x4b, 0x4c, 0x4f, 0x53, 0x56, 0x5d, 0x5e, 0x65,
                                          0x66, 0x67, 0x6a, 0x6b, 0x78, 0x79, 0x7a, 0x7b, 0x1f2, 0x1f8, 0x1f9, 0x1fa, 0x1fc, 0x1fe,
                                          0x200, 0x203, 0x204, 0x205, 0x206, 0x207, 0x208, 0x209, 0x210, 0x22f, 0x285, 0x286, 0x287,
                                          0x288, 0x289, 0x28a, 0x28b, 0x28c, 0x28d, 0x28e, 0x28f, 0x290, 0x299, 0x61a, 0x50, 0x54};
        if (rng.chance(40))
            return carTag();
        if (rng.chance(8))
            return rng.chance(50) ? 0x10101010u : 0x11000000u;
        if (rng.chance(5))
            return rng.next();
        return 0x10000000u | rng.pick(kOther);
    }

    // An item list (count items) for a header; returns the first item.
    uint32_t itemList(int count, bool carMenuItems)
    {
        uint32_t first = 0, previous = 0;
        for (int i = 0; i < count && items < kItemCount; ++i)
        {
            const uint32_t it = s + kItems + static_cast<uint32_t>(items++) * 0x18u;
            std::memset(guest_host(it), 0, 0x18);
            auto &mi = mem<sMenuItemStd>(it);
            mi.pTextGroup = ptr32<sTextGroup>(textGroup());
            mi.selectionTag = carMenuItems && rng.chance(85) ? carTag() : anyTag();
            mi.flags = static_cast<uint16>(rng.next() & 0x7fu);
            mi.statusFlags = static_cast<uint8>(rng.next() & 0x1fu);
            if (vars < kVarCount && rng.chance(50))
            {
                const uint32_t v = s + kVars + static_cast<uint32_t>(vars++) * 0x1cu;
                std::memset(guest_host(v), 0, 0x1c);
                static const uint32_t kVarTags[] = {0x1000001fu, 0x10000026u, 0x10000020u, 0};
                mem<uint32_t>(v + 4u) = rng.pick(kVarTags);
                mem<int32_t>(v + 0xcu) = rng.between(0, 12);
                mi.pMenuVar = ptr32<sMenuItemVar>(v);
            }
            else if (vars > 0)
                mi.pMenuVar = ptr32<sMenuItemVar>(s + kVars + static_cast<uint32_t>(rng.below(static_cast<uint32_t>(vars))) * 0x1cu);
            if (previous)
                mem<uint32_t>(previous + 8u) = it;
            else
                first = it;
            previous = it;
        }
        return first;
    }

    uint32_t menu(int count, bool carMenuItems = false)
    {
        const uint32_t h = header(headers++);
        std::memset(guest_host(h), 0, 0x80);
        auto &hd = mem<sAMenuHeader>(h);
        hd.pMenuStart = ptr32<sMenuItemStd>(itemList(count, carMenuItems));
        hd.moreFlags = static_cast<uint16>(rng.next() & 0x821u);
        hd.evenMoreFlags = static_cast<uint16>(rng.next() & 0x5eu);
        hd.selection = static_cast<int8>(rng.between(0, count > 0 ? count - 1 : 0));
        hd.selectionTag = rng.chance(20) ? 0x10000283u : anyTag();
        hd.x = static_cast<int16>(rng.between(0, 200));
        hd.y = static_cast<int16>(rng.chance(10) ? 100 : rng.between(0, 200));
        hd.width = static_cast<uint16>(rng.between(40, 300));
        hd.height = static_cast<uint16>(rng.between(10, 120));
        // The dump's own border and colour sets (the pause menu's): a wild pointer here would reach beyond RAM, which
        // recompiled code sends to the hardware registers and native code wraps into RAM.
        hd.pBorderSet = ptr32<sBorderSet>(borderSet());
        hd.pColourSet = ptr32<sUIcolourSet>(colourSet());
        hd.pTitleText = ptr32<sTextGroup>(textGroup());
        hd.pCharList = ptr32<sTextGroup>(textGroup());
        hd.pAdditionalText = ptr32<sTextGroup>(textGroup());
        return h;
    }

    void build()
    {
        // Menus: the first menu, the cup menu (eight cups), the car menu, the stats menu and a few more.
        firstMenu = menu(rng.between(1, 5));
        cupMenu = menu(8);
        carMenu = menu(rng.between(3, 12), true);
        statsMenu = menu(2);
        optionsMenu = menu(4);
        for (int i = headers; i < 8; ++i)
            menu(rng.between(0, 6), rng.chance(40));
        // Links between the menus.
        for (int i = 0; i < headers; ++i)
        {
            auto &hd = mem<sAMenuHeader>(header(i));
            hd.pLinkMenuReturn = ptr32<sAMenuHeader>(rng.chance(30) ? 0u : header(rng.below(static_cast<uint32_t>(headers))));
            hd.pGlobalDestMenu = ptr32<sAMenuHeader>(rng.chance(30) ? 0u : header(rng.below(static_cast<uint32_t>(headers))));
            hd.pSpecialMenu = ptr32<sAMenuHeader>(rng.chance(30) ? 0u : header(rng.below(static_cast<uint32_t>(headers))));
        }
        // UI groups: one per menu (the first menu viewed from 0x100001f1, the cup menu tagged 0x1000020b, the car menu
        // viewed from 0x1000020a), then extra ones, and the end mark (a header of -1, the slider sprite in its tag).
        static const uint32_t kGroupTags[] = {0x10000208u, 0x10000213u, 0x10000214u, 0x10000215u, 0x10000216u, 0x1000021du,
                                              0x1000021eu, 0x10000210u, 0, 0x10000301u};
        static const uint32_t kViewTags[] = {0x1000020au, 0x100000e6u, 0x100000efu, 0x10000301u, 0, 0x100001f1u};
        const int count = rng.between(headers, 22);
        for (int i = 0; i < count; ++i)
        {
            const uint32_t g = group(i);
            const int h = i < headers ? i : rng.below(static_cast<uint32_t>(headers));
            mem<uint32_t>(g) = header(h);
            mem<uint32_t>(g + 4u) = header(rng.below(static_cast<uint32_t>(headers)));
            mem<uint32_t>(g + 8u) = h == 1 && i == 1 ? 0x1000020bu : h == 3 && rng.chance(70) ? 0x10000213u : rng.pick(kGroupTags);
            mem<uint32_t>(g + 0xcu) = i == 0 && rng.chance(50) ? 0x10000300u : h == 0 ? kTagFirstMenuValue() : h == 2 ? 0x1000020au : rng.pick(kViewTags);
            mem<int8_t>(g + 0x10u) = static_cast<int8_t>(rng.between(-1, 6));
            mem<uint8_t>(g + 0x11u) = static_cast<uint8_t>(rng.below(3));
            mem<uint8_t>(g + 0x12u) = static_cast<uint8_t>(rng.below(30));
            mem<uint8_t>(g + 0x13u) = 0;
            mem<sAMenuHeader>(header(h)).pUIgroup = ptr32<sUIgroup>(g);
        }
        // The first menu's group (0x100001f1) must exist for the walks that search it without an end mark.
        mem<uint32_t>(group(count - 1) + 0xcu) = kTagFirstMenuValue();
        mem<uint32_t>(group(count - 1)) = firstMenu;
        mem<uint32_t>(group(count)) = 0xffffffffu;
        mem<uint32_t>(group(count) + 8u) = sliderSprite();
        mem<uint32_t>(group(count) + 0xcu) = kTagFirstMenuValue();
        groups = count;

        // Level data.
        const uint32_t fe = s + kFELevel;
        std::memset(guest_host(fe), 0, 0x140);
        mem<uint32_t>(fe + 0x18u) = group(0);
        mem<uint32_t>(fe + 0x4u) = borderSet();
        mem<uint32_t>(fe + 0x8u) = colourSet();
        mem<uint32_t>(fe + 0x1cu) = mem<uint32_t>(sym::fontVarG);
        mem<uint32_t>(fe + 0x24u) = s + kLoadSave;
        mem<uint32_t>(fe + 0x28u) = s + kStats;
        mem<uint32_t>(fe + 0x30u) = s + kCameras;
        mem<uint32_t>(fe + 0x34u) = s + kPaths;
        for (uint32_t i = 0; i < 19u; ++i)
            mem<uint32_t>(fe + 0x38u + 4u * i) = s + kCarDefs + i * 0x58u;
        for (uint32_t i = 0; i < 8u; ++i)
            mem<uint32_t>(fe + 0xd0u + 4u * i) = s + kCups + i * 0x28u;
        mem<uint32_t>(fe + 0xfcu) = rng.chance(5) ? 0u : s + kExtra;
        mem<uint32_t>(fe + 0x10cu) = s + kCredits;
        mem<uint32_t>(fe + 0x11cu) = s + kQuestion;
        for (uint32_t i = 0; i < 4u; ++i)
            mem<uint32_t>(fe + 0x120u + 4u * i) = sliderSprite();
        mem<uint32_t>(kFrontEnd) = fe;
        // Tracks: names and time-trial targets (some beaten, some not).
        for (uint32_t i = 0; i < 25u; ++i)
        {
            const uint32_t e = s + kExtra + i * 0x10u;
            mem<uint32_t>(e) = textGroup();
            mem<uint32_t>(e + 4u) = sliderSprite();
            for (uint32_t k = 8; k < 0x10u; ++k)
                mem<uint8_t>(e + k) = static_cast<uint8_t>(rng.below(k == 0xau || k == 0xdu ? 60 : 10));
        }
        for (uint32_t i = 0; i < 8u; ++i)
        {
            const uint32_t c = s + kCups + i * 0x28u;
            for (uint32_t k = 0; k < 6u; ++k)
                mem<uint8_t>(c + k) = static_cast<uint8_t>(rng.between(rng.chance(5) ? 0 : 1, 24));
            for (uint32_t k = 0; k < 8u; ++k)
                mem<uint32_t>(c + 8u + 4u * k) = rng.chance(90) ? carTag() : anyTag();
        }
        // Car stats.
        mem<uint32_t>(s + kStats) = s + kTextGroups;
        for (uint32_t i = 0; i < 19u; ++i)
        {
            mem<uint32_t>(s + kStats + 4u + 4u * i) = s + kStats + 0x50u + 4u * i;
            mem<uint32_t>(s + kStats + 0x50u + 4u * i) = rng.next() & 0x7f7f7fu;
        }
        // Cameras (a camera list with a few tags) and paths.
        const uint32_t cams = static_cast<uint32_t>(rng.between(1, 5));
        mem<uint32_t>(s + kCameras) = cams;
        static const uint32_t kCamTags[] = {0x100000e6u, 0x100000efu, 0x100000b4u, 0x100000bbu, 0x100000b7u};
        for (uint32_t i = 0; i < cams; ++i)
        {
            const uint32_t cam = s + kCameras + 4u + i * 0x14u;
            mem<uint32_t>(cam) = rng.pick(kCamTags);
            for (uint32_t k = 4; k < 0x14u; k += 4u)
                mem<uint32_t>(cam + k) = rng.next() & 0x7fff7fffu;
        }
        mem<int32_t>(s + kCamera + 8u) = static_cast<int32_t>(rng.next() % 4000u) - 2000;
        mem<int32_t>(s + kCamera + 0x10u) = static_cast<int32_t>(rng.next() % 4000u) - 2000;
        for (uint32_t i = 0; i < 3u; ++i)
            mem<uint32_t>(s + kPaths + 4u * i) = s + kPaths + 0x10u + 0x10u * i;
        // Credits: names, now and then a blank or a coloured line, and the end.
        const int credits = rng.between(1, 60);
        for (int i = 0; i < 64; ++i)
            mem<uint32_t>(s + kCredits + 4u * static_cast<uint32_t>(i)) = i < credits ? word() : 0u;
        // Question mark render head, TSOs, load-save data.
        std::memset(guest_host(s + kQuestion), 0, 0x80);
        for (uint32_t i = 0; i < static_cast<uint32_t>(kTsoCount); ++i)
        {
            const uint32_t t = s + kTsos + i * 0x3cu;
            for (uint32_t k = 0; k < 0x3cu; k += 4u)
                mem<uint32_t>(t + k) = rng.next() & 0xffffu;
            mem<uint16_t>(t + 0x22u) = static_cast<uint16_t>(rng.next()); // obj.flags: bit 0 drawn, 0x20 stopped
        }
        globals();
    }

    static uint32_t kTagFirstMenuValue() { return 0x100001f1u; }

    // A sprite the dump has (the tick sheet of the menu system), for the pictures.
    static uint32_t sliderSprite()
    {
        const uint32_t sprite = mem<uint32_t>(kMenuSystem + 0x28u);
        return inRam(sprite) ? sprite : 0u;
    }

    void globals()
    {
        auto &f = mem<sFrontEndClass>(kFrontEnd);
        auto &m = mem<mainClass>(kMain);
        auto &ms = mem<sMenuSystem>(kMenuSystem);
        mem<uint32_t>(sym::pMenuSystemG) = kMenuSystem;
        m.workingLanguage = static_cast<eLanguages>(language);
        m.activeLanguage = static_cast<eLanguages>(rng.chance(20) ? -1 : rng.between(0, 3));
        static const uint32_t kRaceTypes[] = {0x100001fau, 0x100001fcu, 0x100001feu, 0x10000200u, 0, 0x10000201u};
        m.raceType = rng.pick(kRaceTypes);
        m.lastRaceType = rng.chance(60) ? 0x100001feu : rng.pick(kRaceTypes);
        m.reverseModeOnOff = rng.chance(70) ? static_cast<int32>(rng.below(2)) : rng.between(-1, 3);
        m.mirrorModeOnOff = rng.chance(70) ? static_cast<int32>(rng.below(2)) : rng.between(-1, 3);
        m.timeTrialBeaten = static_cast<uint8>(rng.below(2));
        m.p1Car = static_cast<uint8>(rng.between(0, 20));
        m.p2Car = static_cast<uint8>(rng.between(0, 20));
        m.car1CarMenuPos = static_cast<uint8>(rng.below(8));
        m.car2CarMenuPos = static_cast<uint8>(rng.below(8));
        m.level = static_cast<uint8>(rng.between(0, 25));
        m.status = static_cast<eMain>(rng.chance(70) ? 0 : 0x40);
        m.musicVolume = rng.between(0, 10);
        // The whole reward table: the cars' bytes too (the level tests read past the levels into them).
        for (uint32_t i = 0; i < sizeof(rewardTable); ++i)
            reinterpret_cast<uint8 *>(&m.rewards)[i] = static_cast<uint8>(rng.chance(60) ? rng.below(2) : rng.below(16));
        for (uint32_t i = 0; i < 6u; ++i)
            m.trackList[i] = static_cast<uint8>(rng.between(1, 24));
        static const uint32_t kWorlds[] = {0x10000056u, 0x1000004fu, 0x10000041u, 0x10000053u, 0x1000006bu, 0x10000065u, 0x10000299u};
        f.world = static_cast<int32>(rng.chance(92) ? rng.pick(kWorlds) : anyTag());
        f.track = static_cast<int32>(rng.chance(92) ? 0x1000006cu + rng.below(4) : anyTag());
        f.cup = rng.chance(50) ? 0 : rng.between(1, 8);
        f.carTag[0] = static_cast<eObjectType>(carTag());
        f.carTag[1] = static_cast<eObjectType>(carTag());
        f.active = static_cast<uint8>(rng.chance(60) ? 2 : rng.between(0, 5));
        f.flagPTT = static_cast<int8>(rng.chance(60) ? 0 : rng.between(0, 3));
        f.cupsWonMessage = static_cast<uint8>(rng.chance(60) ? 0 : rng.between(1, 0x19));
        f.frontEndStatus = static_cast<eFront>(rng.between(0, 7));
        f.loadSaveFlag = rng.chance(70) ? 0u : rng.chance(50) ? 0x1000ffffu : rng.chance(50) ? 0x10000206u : 0x10000204u;
        static const uint32_t kWaits[] = {999, 1000, 1001, 1002};
        f.uiDemoLoopWait = rng.chance(50) ? rng.below(1000) : rng.chance(60) ? rng.pick(kWaits) : 990u + rng.below(30);
        f.viewTag = rng.chance(20) ? 0u : anyTag();
        f.flyingLogo = rng.below(2);
        f.pCupSelectMenu = ptr32<sAMenuHeader>(cupMenu);
        f.pFrontEndStringsOLD = ptr32<char>(s + kBuffer);
        f.xaSwitch = rng.between(0, 6);
        for (uint32_t i = 0; i < 2u; ++i)
            f.gameStateData.playerConfig[i].controllerConfig = rng.chance(85) ? rng.between(0, 2) : rng.between(-1, 4);
        auto &c = f.gameStateData.gameConfig;
        c.cupsWon = rng.between(0, 9);
        c.iNumberOfLaps = rng.between(1, 9);
        c.pickupsOnOff = rng.between(0, 1);
        c.singleCarCount = rng.between(1, 8);
        c.multiCarCount = rng.between(2, 8);
        for (int32 &t : c.timeTrials.times)
            t = rng.chance(40) ? -1 : static_cast<int32>(rng.below(700000));
        // The reward messages and the stats menu: the pause menu where the dump has one, else the world's options
        // menu. The menu the game was in before a memory card page: one of the world's.
        const uint32_t drawable = pauseMenu() ? pauseMenu() : optionsMenu;
        for (uint32_t i = 0; i < 30u; ++i)
            f.pRewardMessages[i] = ptr32<sAMenuHeader>(drawable);
        f.pStoredMenu = ptr32<sAMenuHeader>(header(rng.below(static_cast<uint32_t>(headers))));
        ms.pActiveMenu = ptr32<sAMenuHeader>(header(rng.below(static_cast<uint32_t>(headers))));
        ms.pLastActiveMenu = ptr32<sAMenuHeader>(header(rng.below(static_cast<uint32_t>(headers))));
        ms.executeCode = anyTag();
        ms.scrollSelector = static_cast<int8>(rng.chance(80) ? 0 : rng.between(-1, 2));
        ms.newMenuFlag = static_cast<int8>(rng.below(2));
        ms.status = static_cast<int16>(rng.chance(80) ? 0 : 2);
        ms.specialStatsHoldoff = static_cast<uint8>(rng.chance(70) ? 0 : 3);
        mem<uint32_t>(sym::pCarSelectMenu) = carMenu;
        mem<uint32_t>(sym::pStatsMenuG) = rng.chance(80) ? drawable : statsMenu;
        mem<int32_t>(sym::memoryCardDontCareG) = rng.chance(60) ? 1 : 0;
        mem<int32_t>(sym::haveGotLanguageRecently_561) = static_cast<int32_t>(rng.below(2));
        mem<int32_t>(sym::flyTheBleedinCameraG) = rng.chance(85) ? 0 : 1;
        mem<int32_t>(sym::gameLoadedG) = static_cast<int32_t>(rng.below(2));
        mem<uint32_t>(sym::loadSaveAutoStatusG) = rng.below(7);
        mem<int32_t>(sym::bannerXG) = rng.between(0xa0, 0xc4);
        mem<int32_t>(sym::bannerYG) = rng.between(-0x50, 0x30);
        mem<int32_t>(sym::camCarSel3xG) = mem<int32_t>(mem<uint32_t>(sym::cameraVarG + 0xb70u) + 0xcu) + rng.between(-60, 60);
        mem<int32_t>(sym::camCarSel3zG) = mem<int32_t>(mem<uint32_t>(sym::cameraVarG + 0xb70u) + 0x14u) + rng.between(-60, 60);
        mem<int32_t>(sym::firstEntryG) = rng.chance(80) ? 1 : 0;
        mem<int32_t>(sym::autoLoadDoneG) = static_cast<int32_t>(rng.below(2));
        mem<int32_t>(sym::dontDrawCarouselCarsFixG) = static_cast<int32_t>(rng.below(2));
        mem<int32_t>(sym::carouselAvailableG) = static_cast<int32_t>(rng.below(2));
        mem<uint32_t>(sym::tEditVarG + 0x250u) = rng.next() & 7u;
        mem<uint32_t>(sym::rcmusicVarG + 0x14u) = rng.chance(50) ? 0u : 1u;
        carousel();
    }

    // The pause menu of the dump: a menu that can be drawn (in the race dumps; the menu dumps hold none, or a stale
    // pointer, so 0 there).
    static uint32_t pauseMenu()
    {
        const uint32_t menu = mem<uint32_t>(kMenuSystem + 0x14u);
        return inRam(menu) && inRam(mem<uint32_t>(menu + 8u)) && inRam(mem<uint32_t>(menu + 0xcu)) ? menu : 0u;
    }

    // A border and a colour set that can be drawn: the pause menu's, else the menu system's last ones (the front
    // end's, live in the menu dumps).
    static uint32_t borderSet() { return pauseMenu() ? mem<uint32_t>(pauseMenu() + 8u) : mem<uint32_t>(kMenuSystem + 0xcu); }
    static uint32_t colourSet() { return pauseMenu() ? mem<uint32_t>(pauseMenu() + 0xcu) : mem<uint32_t>(kMenuSystem + 0x8u); }

    // The turntable: 19 places with the dump's car objects, its types (mostly the cars' own) and flags.
    void carousel()
    {
        auto &c = mem<sCarouselClass>(sym::carouselVarG);
        c.pTSO = ptr32<_sTSO>(s + kTsos);
        c.numSegments = rng.chance(85) ? 19u : static_cast<uint32_t>(rng.between(0, 19));
        c.segment = rng.below(19);
        c.separation = static_cast<int32>(0x10000u / 19u);
        c.angle = static_cast<int32>(rng.chance(30) ? rng.next() : rng.next() & 0xffffu);
        c.tgtAngle = rng.chance(30) ? c.angle : static_cast<int32>(rng.next() & 0x1ffffu);
        c.moveMode = rng.between(0, 3);
        c.lastUpdated = static_cast<tTime>(mem<uint32_t>(kEnv + eTime) - rng.below(100));
        for (uint32_t i = 0; i < 19u; ++i)
        {
            sCarousel &e = c.carousel[i];
            e.type = static_cast<eObjectType>(rng.chance(90) ? static_cast<int32_t>(i) + 1 : rng.between(0, 21));
            e.flag = static_cast<uint16>(rng.chance(85) ? rng.below(2) : rng.below(4));
            e.flag2 = static_cast<uint16>(rng.chance(85) ? rng.below(2) : rng.below(4));
            e.bBoat = rng.below(2);
            e.angle = static_cast<::rc::angle>(rng.next());
            e.pObject = ptr32<_sObject>(poolObject(i + rng.below(3)));
        }
        mem<int32_t>(sym::carouselRadiusG) = rng.between(0x100, 0x4000);
        mem<int32_t>(sym::carouselRotateRateG) = rng.between(-40, 40);
    }
};

} // namespace

// Rewards, cheats and time trials.
void frontSfx(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    World w(ctx, rng, scratch, {sym::SfxPlay});
    setArg(ctx, 0, rng.below(8));
}

void frontRewardTable(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    World w(ctx, rng, scratch);
    for (const uint32_t cheat : {sym::cheatData1G, sym::cheatData2G, sym::cheatData3G})
    {
        auto &c = mem<sCheatKeyData>(cheat);
        c.activated = static_cast<uint8>(rng.chance(50) ? 1 : rng.below(3));
        // Cheat 3 (a cup won, which sets the rewards and so clears the secrets again) only in the third slot, as in
        // the game: in the first two, ClearAllSecrets would recurse without end.
        static const uint8 kCheats[] = {0, 1, 2, 4, 5, 6};
        c.cheatNo = cheat == sym::cheatData3G ? static_cast<uint8>(rng.between(0, 6)) : rng.pick(kCheats);
        c.keyCount = static_cast<uint8>(rng.below(8));
    }
    setArg(ctx, 0, rng.chance(80) ? sym::mainVarG + 0x3bcu : scratch + kBuffer);
}

void frontCheatResults(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    frontRewardTable(ctx, state, scratch);
    Rng rng{state};
    static const uint32_t kCheats[] = {sym::cheatData1G, sym::cheatData2G, sym::cheatData3G};
    setArg(ctx, 0, rng.pick(kCheats));
}

void frontCheatKeys(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    World w(ctx, rng, scratch);
    const uint32_t cheat = scratch + kKeyTables;
    auto &c = mem<sCheatKeyData>(cheat);
    std::memset(&c, 0, sizeof(sCheatKeyData));
    c.activated = static_cast<uint8>(rng.chance(15) ? 1 : 0);
    c.keyMax = static_cast<uint8>(rng.between(2, 6));
    c.keyCount = static_cast<uint8>(rng.between(0, c.keyMax));
    c.keyHeld = static_cast<uint8>(rng.below(2));
    c.cheatNo = static_cast<uint8>(rng.between(1, 5));
    const tTime time = static_cast<tTime>(rng.next() & 0xffffffu);
    static const int32_t kGaps[] = {1999, 2000, 2001, 2002};
    const int32_t gap = rng.chance(25) ? rng.pick(kGaps) : static_cast<int32_t>(rng.below(rng.chance(80) ? 2000 : 3000));
    c.lastKeyTime = rng.chance(30) ? 0u : static_cast<uint32>(time - gap);
    c.pCheatKeyTable = ptr32<uint8>(cheat + 0x20u);
    for (uint32_t i = 0; i < 8u; ++i)
        mem<uint8_t>(cheat + 0x20u + i) = static_cast<uint8_t>(rng.below(32));
    // The pads: the expected key held mostly, another one now and then.
    inputClass &in = G(inputVarG);
    for (uint8 &k : in.inactiveBuffer)
        k = static_cast<uint8>(k & 0xfeu);
    const uint8_t key = mem<uint8_t>(cheat + 0x20u + c.keyCount);
    if (rng.chance(60))
        in.inactiveBuffer[key & 31u] = static_cast<uint8>(in.inactiveBuffer[key & 31u] | 1u);
    if (rng.chance(20))
        in.inactiveBuffer[rng.below(32)] |= 1u;
    setArg(ctx, 0, cheat);
    setArg(ctx, 1, static_cast<uint32_t>(time));
}

void frontSetRewards(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    World w(ctx, rng, scratch);
    setArg(ctx, 0, sym::mainVarG + 0x3bcu);
    setArg(ctx, 1, sym::frontEndVarG + 0x138u);
    setArg(ctx, 2, static_cast<uint32_t>(rng.between(-1, 9)));
}

void frontCupAvailability(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    World w(ctx, rng, scratch);
    setArg(ctx, 0, rng.chance(70) ? 0u : rng.chance(80) ? 1u : rng.next());
}

void frontWorld(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    World w(ctx, rng, scratch);
    SET_GPR_S32(ctx, 8, static_cast<int32_t>(rng.below(6))); // $t0, WorldTrackToLevel's fallback
}

void frontTimeTrialExit(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    World w(ctx, rng, scratch);
    auto &m = mem<mainClass>(kMain);
    m.rewards.carConcept2000 = static_cast<uint8>(rng.chance(70) ? 0 : 1);
    m.rewards.carUFO = static_cast<uint8>(rng.chance(70) ? 0 : 1);
    // Beat every target now and then, to reach the car messages.
    if (rng.chance(30))
        for (int32 &t : mem<sFrontEndClass>(kFrontEnd).gameStateData.gameConfig.timeTrials.times)
            t = static_cast<int32>(rng.below(100));
}

void frontTimeTrialTime(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    World w(ctx, rng, scratch);
    setArg(ctx, 0, rng.chance(80) ? scratch + kBuffer : 0u);
    setArg(ctx, 1, sym::frontEndVarG + 0x138u);
    setArg(ctx, 2, static_cast<uint32_t>(rng.between(1, 24)));
    setArg(ctx, 3, rng.below(2));
}

void frontLevelAvailability(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    World w(ctx, rng, scratch);
    setArg(ctx, 0, sym::mainVarG + 0x3bcu);
    setArg(ctx, 1, static_cast<uint32_t>(rng.between(1, 96)));
}

void frontCarType(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    World w(ctx, rng, scratch);
    setArg(ctx, 0, static_cast<uint32_t>(rng.chance(90) ? rng.between(0, 20) : static_cast<int32_t>(rng.next())));
}

void frontCarousel(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    World w(ctx, rng, scratch);
    setArg(ctx, 0, rng.chance(50) ? mem<uint32_t>(kEnv + eTime) + rng.below(60) : rng.below(19));
}

// The front end's setup, frame and teardown.
void frontSetupClass(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    World w(ctx, rng, scratch, {sym::FontSet, sym::SetCameraToThisTag});
    setArg(ctx, 0, scratch + kHandle);
}

void frontEndInit(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    World w(ctx, rng, scratch);
}

void frontEndProcess(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    World w(ctx, rng, scratch, {FRONT_MENU_DRAWING});
    // No key-definition pages and no unknown world or track: BuildKeyDefinitions and WorldTrackToLevel fall back on
    // registers their caller left for those, which differ between the original's frame loop and the native one (they
    // are tested with them on their own). The game never has them there.
    for (int i = 0; i < w.headers; ++i)
        mem<sAMenuHeader>(w.header(i)).moreFlags = static_cast<uint16>(mem<sAMenuHeader>(w.header(i)).moreFlags & ~0x800u);
    auto &f = mem<sFrontEndClass>(kFrontEnd);
    static const uint32_t kWorlds[] = {0x10000056u, 0x1000004fu, 0x10000041u, 0x10000053u, 0x1000006bu, 0x10000065u, 0x10000299u};
    f.world = static_cast<int32>(rng.pick(kWorlds));
    f.track = static_cast<int32>(0x1000006cu + rng.below(4));
    // Left alone long enough on a menu that allows a demo, now and then.
    if (rng.chance(40))
    {
        f.uiDemoLoopWait = 1000u + rng.below(2);
        f.frontEndStatus = static_cast<eFront>(rng.below(3));
        f.flagPTT = 0;
        f.cupsWonMessage = 0;
        auto &menu = mem<sAMenuHeader>(mem<uint32_t>(kMenuSystem));
        menu.evenMoreFlags = static_cast<uint16>(menu.evenMoreFlags | 0x10u);
    }
}

void frontEndTerm(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    World w(ctx, rng, scratch, {sym::ObjectDeleteAll, sym::MemBlockFree}); // CarouselTerm's tail jump, as above
}

void frontTask(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    World w(ctx, rng, scratch, {sym::TaskCachedInsert});
    // A task of its own, out of the task list (the menu dumps have it in), due near the list's insertion point.
    auto &task = mem<sFrontEndClass>(kFrontEnd).task;
    const uint32_t self = guest_addr(&task);
    if (inRam(task.pPrev.address) && inRam(task.pNext.address) && task.pPrev->pNext.address == self && task.pNext->pPrev.address == self)
    {
        task.pPrev->pNext = task.pNext;
        task.pNext->pPrev = task.pPrev;
        if (G(taskVarG).pInsert.address == self)
            G(taskVarG).pInsert = task.pPrev;
    }
    std::memset(&task, 0, sizeof(task));
    const ptr32<_sTask> pInsert = G(taskVarG).pInsert;
    setArg(ctx, 0, 0);
    setArg(ctx, 1, static_cast<uint32_t>(pInsert->time + rng.between(-200, 200)));
}

void frontCameraDestination(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    World w(ctx, rng, scratch);
    static const uint32_t kTags[] = {0x1000020eu, 0x1000020fu, 0x10000212u, 0x1000020au};
    // A destination table: some entries matching the tags given.
    const uint32_t table = sym::camDest;
    const int count = rng.between(0, 12);
    uint32_t tag1 = rng.chance(50) ? rng.pick(kTags) : w.anyTag(), tag2 = w.anyTag();
    for (int i = 0; i < count; ++i)
    {
        const uint32_t d = table + 16u * static_cast<uint32_t>(i);
        const bool match = rng.chance(20);
        mem<uint32_t>(d) = match ? tag1 : (rng.chance(30) ? tag1 : w.anyTag()) | 1u;
        mem<uint32_t>(d + 4u) = match ? tag2 : w.anyTag();
        mem<uint32_t>(d + 8u) = rng.chance(30) ? rng.between(1, 5) : w.anyTag();
        mem<uint8_t>(d + 0xcu) = static_cast<uint8_t>(rng.next());
        mem<uint8_t>(d + 0xdu) = static_cast<uint8_t>(rng.next());
        mem<uint16_t>(d + 0xeu) = static_cast<uint16_t>(rng.next());
    }
    mem<uint32_t>(table + 16u * static_cast<uint32_t>(count)) = 0;
    // The world and track menus' selections, each one often.
    mem<sAMenuHeader>(mem<uint32_t>(kMenuSystem)).selection = static_cast<int8>(rng.between(-1, 7));
    setArg(ctx, 0, tag1);
    setArg(ctx, 1, tag2);
    SET_GPR_S32(ctx, 8, static_cast<int32_t>(rng.below(6)));
}

void frontRemapDestTags(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    World w(ctx, rng, scratch);
    setArg(ctx, 0, rng.chance(90) ? (rng.chance(80) ? 2u + rng.below(3) : rng.below(6)) : w.anyTag());
}

void frontTag(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    World w(ctx, rng, scratch);
    static const uint32_t kCups[] = {0x1000003du, 0x1000003eu, 0x1000004bu, 0x1000004cu, 0x1000005du, 0x1000005eu, 0x10000066u, 0x10000067u};
    // Car tags and their neighbours mostly (every edge of TagToCar's ranges), cup tags, anything.
    const uint32_t tag = rng.chance(50) ? w.carTag() + static_cast<uint32_t>(rng.between(-1, 1))
                         : rng.chance(30) ? rng.pick(kCups) : rng.chance(70) ? 0x10000000u + rng.below(0x2a0) : w.anyTag();
    setArg(ctx, 0, tag);
    setArg(ctx, 1, rng.next());
}

void frontCarToTag(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    World w(ctx, rng, scratch);
    setArg(ctx, 0, static_cast<uint32_t>(rng.between(-1, 22)));
    setArg(ctx, 1, static_cast<uint32_t>(rng.between(0, 3)));
    setArg(ctx, 2, rng.next());
    setArg(ctx, 3, rng.next());
}

void frontDraw(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    World w(ctx, rng, scratch, {sym::AMenuDrawBox});
}

void frontStubbed(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    // ObjectDeleteAll (CarouselTerm's tail jump) frees through native calls of MemBlockFree.
    World w(ctx, rng, scratch, {sym::ObjectDeleteAll, sym::MemBlockFree});
    setArg(ctx, 0, rng.chance(70) ? 0x10000000u + rng.below(0x300) : rng.next());
}

void frontCarouselInit(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    World w(ctx, rng, scratch);
    // The scenery list: tags and groups (1-4 mostly).
    for (uint32_t i = 0; i < 29u; ++i)
    {
        mem<uint32_t>(sym::frontTSOTagList + 12u * i) = 0x10000000u + rng.below(0x300);
        mem<int32_t>(sym::frontTSOTagList + 12u * i + 4u) = rng.between(0, 5);
    }
    setArg(ctx, 0, mem<uint32_t>(kEnv + eTime));
}

void frontCarouselSetUp(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    World w(ctx, rng, scratch);
    setArg(ctx, 0, static_cast<uint32_t>(rng.chance(80) ? 19 : rng.between(1, 19)));
    setArg(ctx, 1, mem<uint32_t>(kEnv + eTime));
}

void frontAnimGroup(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    World w(ctx, rng, scratch);
    for (uint32_t i = 0; i < 29u; ++i)
    {
        mem<int32_t>(sym::frontTSOTagList + 12u * i + 4u) = rng.between(0, 5);
        mem<uint32_t>(sym::frontTSOTagList + 12u * i + 8u) = scratch + kTsos + (i % kTsoCount) * 0x3cu;
    }
    setArg(ctx, 0, static_cast<uint32_t>(rng.between(0, 5)));
}

void frontSetUpVehicle(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    World w(ctx, rng, scratch);
    setArg(ctx, 0, static_cast<uint32_t>(rng.between(0, 2)));
    setArg(ctx, 1, static_cast<uint32_t>(rng.between(0, 20)));
}

// Menus.
void frontParse(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    World w(ctx, rng, scratch);
    auto &ms = mem<sMenuSystem>(kMenuSystem);
    // The active menu is one of the groups' (the view search ends there); a car menu now and then.
    ms.pActiveMenu = ptr32<sAMenuHeader>(w.header(rng.chance(30) ? 2 : rng.between(1, w.headers - 1)));
    auto &h = mem<sAMenuHeader>(ms.pActiveMenu.address);
    if (rng.chance(30))
    {
        h.moreFlags = static_cast<uint16>(h.moreFlags | 0x20u);
        static const uint32_t kSelections[] = {0, 1, 2, 1, 2};
        const uint32_t which = rng.pick(kSelections);
        h.pSelection = ptr32<uint8>(which == 0 && rng.chance(50) ? scratch + kBuffer : which);
        h.pValueStore = ptr32<uint8>(rng.chance(70) ? scratch + kBuffer + 8u : 0u);
        mem<uint8_t>(scratch + kBuffer) = static_cast<uint8_t>(rng.below(4));
    }
    else
        h.moreFlags = static_cast<uint16>(h.moreFlags & ~0x20u);
    // Option pages' items need variables.
    for (ptr32<sMenuItemStd> p = h.pMenuStart; p; p = p->pNext)
        if (!p->pMenuVar)
            p->pMenuVar = ptr32<sMenuItemVar>(scratch + kVars);
    if (w.vars == 0)
        std::memset(guest_host(scratch + kVars), 0, 0x1c);
    if (!h.pMenuStart || !h.pMenuStart->pNext || !h.pMenuStart->pNext->pNext || !h.pMenuStart->pNext->pNext->pNext)
        h.pMenuStart = mem<sAMenuHeader>(w.optionsMenu).pMenuStart; // AMenuGetMIT 1-3 must find items
    ms.pLastActiveMenu = ptr32<sAMenuHeader>(w.optionsMenu);
    mem<int32_t>(sym::rcmusicVarG + 0x24u) = rng.between(0, 12);
    mem<int32_t>(sym::revSfxVarG + 0x1148u) = rng.between(0, 12);
    // Each code the function acts on, often.
    static const uint32_t kCodes[] = {0x10000044u, 0x10000047u, 0x10000049u, 0x1000004au, 0x1000006au, 0x1000007bu, 0x100001f2u,
                                      0x10000207u, 0x100001f8u, 0x100001fau, 0x100001fcu, 0x100001feu, 0x10000200u, 0x10000204u,
                                      0x10000205u, 0x10000206u, 0x10000210u, 0x10000285u, 0x10000286u, 0x1000028au, 0x1000028cu,
                                      0x10101010u, 0x11000000u, 0x1000006bu, 0x1000004fu, 0x1000003du, 0x10000066u};
    if (rng.chance(50))
        ms.executeCode = rng.pick(kCodes);
    else if (rng.chance(10))
        ms.executeCode = rng.next();
}

void frontDirectionMenu(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    World w(ctx, rng, scratch);
    const uint32_t h = w.header(rng.between(0, w.headers - 1));
    auto &hd = mem<sAMenuHeader>(h);
    hd.evenMoreFlags = static_cast<uint16>(rng.chance(80) ? hd.evenMoreFlags | 4u : hd.evenMoreFlags & ~4u);
    if (!hd.pMenuStart)
        hd.pMenuStart = mem<sAMenuHeader>(w.optionsMenu).pMenuStart;
    if (!hd.pGlobalDestMenu)
        hd.pGlobalDestMenu = ptr32<sAMenuHeader>(w.optionsMenu);
    setArg(ctx, 0, h);
    SET_GPR_S32(ctx, 8, static_cast<int32_t>(rng.below(6)));
}

void frontConfirm(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    World w(ctx, rng, scratch);
    auto &f = mem<sFrontEndClass>(kFrontEnd);
    if (f.cup == 0 && rng.chance(50))
        f.cup = rng.between(1, 8);
    // The chosen cars are on the car menu mostly.
    for (int i = 0; i < 2; ++i)
        if (rng.chance(80))
        {
            ptr32<sMenuItemStd> p = mem<sAMenuHeader>(w.carMenu).pMenuStart;
            for (uint32_t k = rng.below(6); k > 0 && p->pNext; --k)
                p = p->pNext;
            f.carTag[i] = static_cast<eObjectType>(p->selectionTag);
        }
    setArg(ctx, 0, rng.below(2));
    setArg(ctx, 1, rng.next());
    SET_GPR_S32(ctx, 8, static_cast<int32_t>(rng.below(6)));
    if (rng.chance(30))
        setArg(ctx, 0, w.string("rev"));
}

void frontStatsBar(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    World w(ctx, rng, scratch);
    setArg(ctx, 0, static_cast<uint32_t>(rng.between(0, 500)));
    setArg(ctx, 1, static_cast<uint32_t>(rng.between(0, 400)));
    setArg(ctx, 2, static_cast<uint32_t>(rng.between(0, 100)));
    setArg(ctx, 3, static_cast<uint32_t>(rng.between(0, 30)));
    setArg(ctx, 4, static_cast<uint32_t>(rng.between(0, 100)));
}

void frontVehicleStats(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    World w(ctx, rng, scratch, {FRONT_MENU_DRAWING});
    const uint32_t h = rng.chance(70) ? w.carMenu : w.header(rng.below(static_cast<uint32_t>(w.headers)));
    auto &hd = mem<sAMenuHeader>(h);
    hd.moreFlags = static_cast<uint16>(rng.chance(85) ? hd.moreFlags | 1u : hd.moreFlags);
    if (!hd.pMenuStart)
        hd.pMenuStart = mem<sAMenuHeader>(w.carMenu).pMenuStart;
    mem<uint32_t>(mem<uint32_t>(sym::cameraVarG + 0xb70u) + 0xd8u) = rng.chance(85) ? 0xffffffffu : 0u;
    setArg(ctx, 0, h);
}

void frontSelectionTest(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    World w(ctx, rng, scratch);
    auto &ms = mem<sMenuSystem>(kMenuSystem);
    ms.pActiveMenu = ptr32<sAMenuHeader>(rng.chance(60) ? w.carMenu : w.header(rng.below(static_cast<uint32_t>(w.headers))));
    // Links only to the world's own menus.
    for (int i = 0; i < w.items; ++i)
    {
        auto &mi = mem<sMenuItemStd>(scratch + kItems + static_cast<uint32_t>(i) * 0x18u);
        mi.pLinkMenu = ptr32<sAMenuHeader>(rng.chance(50) ? 0u : w.header(rng.below(static_cast<uint32_t>(w.headers))));
        mi.linkType = static_cast<uint8>(rng.below(3));
    }
    if (rng.chance(20))
        ms.executeCode = 0x100001f9u;
}

void frontKeyDefinitions(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    World w(ctx, rng, scratch);
    const uint32_t h = rng.chance(10) ? 0u : w.header(rng.below(static_cast<uint32_t>(w.headers)));
    if (h)
    {
        auto &hd = mem<sAMenuHeader>(h);
        hd.moreFlags = static_cast<uint16>(rng.chance(85) ? hd.moreFlags | 0x800u : hd.moreFlags & ~0x800u);
        hd.pMenuStart = ptr32<sMenuItemStd>(w.itemList(rng.between(2, 14), false));
        if (!hd.pMenuStart || !hd.pMenuStart->pNext)
            hd.pMenuStart = mem<sAMenuHeader>(w.cupMenu).pMenuStart;
        if (!hd.pUIgroup)
            hd.pUIgroup = ptr32<sUIgroup>(w.group(0));
        mem<uint32_t>(hd.pUIgroup.address + 8u) = rng.chance(70) ? 0x10000214u + rng.below(2) : w.anyTag();
    }
    // The three line sets: ascending line numbers, the third set of each list ending the walk.
    for (const uint32_t set : {sym::set1_643, sym::set2_644, sym::set3_645})
    {
        uint8_t line = 0;
        for (uint32_t i = 0; i < 9u; ++i)
        {
            line = static_cast<uint8_t>(line + rng.between(1, 3));
            mem<uint8_t>(set + i) = line;
        }
    }
    setArg(ctx, 0, h);
    setArg(ctx, 2, rng.below(3));
    setArg(ctx, 6, sym::set2_644);
}

void frontRewardMessage(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    World w(ctx, rng, scratch, {FRONT_MENU_DRAWING});
    auto &f = mem<sFrontEndClass>(kFrontEnd);
    f.cupsWonMessage = static_cast<uint8>(rng.chance(30) ? rng.between(0x16, 0x19) : rng.chance(20) ? 0x0e : rng.between(0, 0x19));
    static const uint32_t kCodes[] = {0x10000625u, 0x10000626u, 0x10000627u, 0x10000628u, 0x10000629u, 0x1000062au, 0x100006bbu, 0};
    mem<uint32_t>(kMenuSystem + 0x68u) = rng.pick(kCodes);
    for (uint8 &k : G(inputVarG).inactiveBuffer)
        k = static_cast<uint8>(rng.chance(10) ? 0x21u : 0u);
    mem<sMenuSystem>(kMenuSystem).pActiveMenu = ptr32<sAMenuHeader>(World::pauseMenu() ? World::pauseMenu() : w.optionsMenu);
}

void frontCarMenu(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    World w(ctx, rng, scratch);
    auto &ms = mem<sMenuSystem>(kMenuSystem);
    ms.pActiveMenu = ptr32<sAMenuHeader>(w.carMenu);
    auto &h = mem<sAMenuHeader>(w.carMenu);
    h.moreFlags = static_cast<uint16>(rng.chance(85) ? h.moreFlags | 0x20u : h.moreFlags & ~0x20u);
    setArg(ctx, 2, rng.next());
    setArg(ctx, 3, rng.next());
}

void frontBanner(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    World w(ctx, rng, scratch);
    // The main menu (the logo comes in) half the time, and the logo near its limits.
    if (rng.chance(50))
        mem<sAMenuHeader>(mem<uint32_t>(kMenuSystem)).selectionTag = 0x10000283u;
    if (rng.chance(50))
        mem<int32_t>(sym::bannerYG) = rng.between(0x1a, 0x28);
    mem<int32_t>(sym::memoryCardDontCareG) = rng.chance(85) ? 1 : 0;
    if (rng.chance(10))
        mem<sMenuSystem>(kMenuSystem).pActiveMenu = nullptr;
}

void frontMemoryCard(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    World w(ctx, rng, scratch, {sym::RCMusicSetVol});
    mem<int32_t>(sym::memoryCardDontCareG) = rng.chance(50) ? 0 : 1;
    mem<uint32_t>(sym::loadSaveAutoStatusG) = rng.chance(80) ? rng.below(4) : rng.below(8);
}

void frontMenuScan(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    World w(ctx, rng, scratch);
    setArg(ctx, 0, w.group(0));
    setArg(ctx, 1, rng.below(3));
    if (rng.chance(10))
        mem<uint32_t>(w.group(0)) = 0xffffffffu;
}

void frontItemDisabler(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    World w(ctx, rng, scratch);
    const uint32_t h = w.header(rng.below(static_cast<uint32_t>(w.headers)));
    static const uint32_t kTags[] = {0x10000200u, 0x1000022fu, 0x1000061au, 0x10000078u, 0x1000006bu, 0x10000201u};
    for (ptr32<sMenuItemStd> p = mem<sAMenuHeader>(h).pMenuStart; p; p = p->pNext)
        if (rng.chance(60))
            p->selectionTag = rng.pick(kTags);
    setArg(ctx, 0, h);
}

void frontChopItem(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    World w(ctx, rng, scratch);
    setArg(ctx, 0, w.cupMenu);
    setArg(ctx, 1, static_cast<uint32_t>(rng.between(0, 7)));
}

void frontStatsDaemon(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    World w(ctx, rng, scratch);
    // A handle to a block whose first word is a list of resource names, ending with 0.
    const uint32_t handle = scratch + kHandle, block = scratch + kHandle + 8u, list = scratch + kBuffer;
    mem<uint32_t>(handle) = block;
    mem<uint32_t>(block) = list;
    const int count = rng.between(0, 20);
    for (int i = 0; i < count; ++i)
        mem<uint32_t>(list + 4u * static_cast<uint32_t>(i)) = rng.next() | 1u;
    mem<uint32_t>(list + 4u * static_cast<uint32_t>(count)) = 0;
    setArg(ctx, 0, handle);
}

// Credits and fireworks.
void frontEffects(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    World w(ctx, rng, scratch);
    // An effect list of its own: fireworks and others, some run out.
    auto &v = mem<_visFxClass>(sym::visFxVarG);
    const int count = rng.between(0, 12);
    uint32_t previous = 0;
    v.vfxHeader = nullptr;
    for (int i = 0; i < count; ++i)
    {
        const uint32_t e = scratch + kEffects + static_cast<uint32_t>(i) * 0x40u;
        std::memset(guest_host(e), 0, 0x40);
        auto &fx = mem<_sVfx_Object>(e);
        fx.Prev = ptr32<_sVfx_Object>(previous);
        fx.Renderer = ee_fnptr(sym::VersionDisplay);
        fx.iLifeSpan = static_cast<uint16>(rng.chance(40) ? 0 : rng.chance(40) ? rng.between(1, 3) : rng.between(1, 100));
        fx.iType = static_cast<uint16>(rng.chance(50) ? 0x23 : rng.below(0x30));
        if (previous)
            mem<uint32_t>(previous) = e;
        else
            v.vfxHeader = ptr32<_sVfx_Object>(e);
        previous = e;
    }
    v.vfxFooter = ptr32<_sVfx_Object>(rng.chance(90) ? previous : 0u);
}

void frontCredits(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    frontEffects(ctx, state, scratch);
    Rng rng{state};
    mem<uint32_t>(sym::displayVarG + 0x64u) = rng.next() & 0xffffu;
    mem<uint32_t>(kEnv + eFrameLimit) = static_cast<uint32_t>(rng.between(1, 30));
    setArg(ctx, 0, 0);
}

} // namespace rc::fuzz
