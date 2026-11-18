// Builds the random-test inputs of TEDIT.c. See tedit_fuzz.h.

#include "tedit_fuzz.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <unistd.h>

namespace rc::fuzz {

namespace {

// Scratch layout. The grid has eight rows' room on either side: a track that leaves the grid writes units there.
constexpr uint32_t kUnits = 0x0400u;        // the grid: 0x300 track units of 8 bytes
constexpr uint32_t kModuleHeader = 0x2080u; // the edit module resource: a count, then 42 modules of 0x18 bytes
constexpr uint32_t kPosArrays = 0x2480u;    // the modules' covered units (3 bytes each), shared and overlapping
constexpr uint32_t kTileArrays = 0x2500u;   // the modules' tiles, 9 words each
constexpr uint32_t kTrackTiles = 0x2b00u;   // an sTrackTiles: counts, then 0xb0 cell pointers
constexpr uint32_t kTileShapes = 0x2dd0u;   // 8 tile shapes (their base height at 0x6a)
constexpr uint32_t kSaved = 0x3150u;        // the permanent save buffer (0x200)
constexpr uint32_t kSaveGame = 0x3350u;     // the memory card's save buffer (0x404 used)
constexpr uint32_t kTrackNodes = 0x3770u;   // the track's module list, 48 nodes of 8
constexpr uint32_t kCursorNodes = 0x38f0u;  // the cursor's module list, 16 nodes of 8
constexpr uint32_t kArgs = 0x3970u;         // argument buffers, 0x180
constexpr uint32_t kAINodes = 0x3af0u;      // 24 AI nodes of 0x6c
constexpr uint32_t kMenuSys = 0x4520u;     // an sMenuSystem (pMenuSystemG), 0x100
constexpr uint32_t kLevel = 0x4620u;       // a level's paths and cameras (pLvlG), 0x100
constexpr uint32_t kSprites = 0x4720u;     // the editor's sprite table (pTrackSprites), 0xc0
constexpr uint32_t kArena = 0x47e0u, kArenaEnd = 0x7800u; // the heap stand-ins' blocks
constexpr uint32_t kEnv = kFuzzScratch + 0x7800u, kLog = kFuzzScratch + 0x7900u, kLogMax = 44u;
constexpr uint32_t kModules = 42u, kTiles = 0xb0u;
enum : uint32_t
{
    eSeed = 0x0,
    eCalls = 0x4,
    eHash = 0x8,
    eArena = 0xc,  // the next free block
    eKeys = 0x10,  // percent of InputHit calls that answer true
};

constexpr uint32_t kTE = sym::tEditVarG, kDescs = sym::editModuleListG;

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

void setReg(R5900Context *ctx, int reg, uint32_t value) { SET_GPR_S32(ctx, reg, static_cast<int32_t>(value)); }
void setArg(R5900Context *ctx, int index, uint32_t value) { setReg(ctx, 4 + index, value); }
uint32_t arg(R5900Context *ctx, int index) { return getRegU32(ctx, 4 + index); }
uint32_t env(uint32_t offset) { return mem<uint32_t>(kEnv + offset); }
void setEnv(uint32_t offset, uint32_t value) { mem<uint32_t>(kEnv + offset) = value; }

uint32_t mix(uint32_t x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

// A value from the input's seed: the same sequence in both runs, as long as the calls are the same.
uint32_t draw()
{
    const uint32_t k = env(eCalls);
    setEnv(eCalls, k + 1u);
    return mix(env(eSeed) + k * 0x9e3779b9u);
}
uint32_t drawBelow(uint32_t n) { return n ? draw() % n : 0u; }
bool drawChance(uint32_t percent) { return drawBelow(100) < percent; }

bool inRam(uint32_t address) { return address >= 0x10000u && address < 0x2000000u; }

void logEntry(uint32_t what, uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
    static ::FILE *trace = std::getenv("RC_TEDITFUZZ_TRACE") ? std::fopen(std::getenv("RC_TEDITFUZZ_TRACE"), "a") : nullptr;
    if (trace)
    {
        std::fprintf(trace, "[tedit_fuzz %d] %x %x %x %x %x\n", static_cast<int>(getpid()), what, a, b, c, d);
        std::fflush(trace);
    }
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

void returnWith(R5900Context *ctx, uint32_t value)
{
    SET_GPR_S32(ctx, 2, static_cast<int32_t>(value));
    ctx->pc = getRegU32(ctx, 31);
}

// --- Stand-ins. ---

template <uint32_t Address, int Args>
void logged(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    const auto a = [ctx](int i) { return i < Args ? arg(ctx, i) : 0u; };
    logEntry(Address, a(0), a(1), a(2), a(3) ^ a(4) << 8 ^ a(5) << 16 ^ a(6) << 24);
    returnWith(ctx, 0);
}

void inputHeld(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    logEntry(sym::InputHeld, arg(ctx, 0), 0, 0, 0);
    returnWith(ctx, drawChance(env(eKeys)) ? 1u : 0u);
}

// TileRender (TEditAdjustCursorModule's 3D pieces): its position, the unit and the theme.
void tileRender(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    const uint32_t unit = arg(ctx, 2);
    logEntry(sym::TileRender, arg(ctx, 0) & 0xffu, arg(ctx, 1) & 0xffu,
             inRam(unit) ? mem<uint32_t>(unit) ^ mem<uint32_t>(unit + 4u) * 31u : unit, arg(ctx, 3));
    returnWith(ctx, 0);
}

void inputHit(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    logEntry(sym::InputHit, arg(ctx, 0), 0, 0, 0);
    returnWith(ctx, drawChance(env(eKeys)) ? 1u : 0u);
}

void memAllocLo(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    const uint32_t handle = arg(ctx, 0), size = arg(ctx, 1);
    uint32_t at = env(eArena);
    if (at + 0x100u > kFuzzScratch + kArenaEnd || size > kArenaEnd - kArena)
        at = kFuzzScratch + kArena;
    const uint32_t room = kFuzzScratch + kArenaEnd - at, take = ((size < room ? size : room) + 15u) & ~15u;
    setEnv(eArena, at + take);
    // A new block holds whatever was there: a word from the seed.
    for (uint32_t i = 0; i < take; i += 4u)
        mem<uint32_t>(at + i) = draw();
    logEntry(sym::MemBlockAllocLo, handle, size, at, 0);
    if (inRam(handle))
        mem<uint32_t>(handle) = at;
    returnWith(ctx, 1);
}

void memFree(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    const uint32_t handle = arg(ctx, 0);
    logEntry(sym::MemBlockFree, handle, inRam(handle) ? mem<uint32_t>(handle) : 0u, 0, 0);
    if (inRam(handle))
        mem<uint32_t>(handle) = 0;
    returnWith(ctx, 0);
}

// The sprite's parameters as the callers write them (flags to blue; code and the scales are left as the stack held them
// and SpriteRenderNew does not read them).
void spriteRender(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    const uint32_t p = arg(ctx, 1);
    uint32_t hash = 2166136261u;
    for (uint32_t i = 0; i < 0x13u && inRam(p); ++i)
        hash = (hash ^ mem<uint8_t>(p + i)) * 16777619u;
    logEntry(sym::SpriteRenderNew, arg(ctx, 0), hash, arg(ctx, 2), 0);
    returnWith(ctx, 0);
}

// A drawing call: its first argument's bytes hashed (skipping the ones the callers leave as the stack held them) and
// its depth (in $f12).
template <uint32_t Address, uint32_t Size, uint32_t Skip>
void drawn(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    const uint32_t p = arg(ctx, 0);
    uint32_t hash = 2166136261u;
    for (uint32_t i = 0; i < Size && inRam(p); ++i)
        if (i != Skip)
            hash = (hash ^ mem<uint8_t>(p + i)) * 16777619u;
    uint32_t otz;
    std::memcpy(&otz, &ctx->f[12], 4);
    logEntry(Address, p, hash, otz, 0);
    returnWith(ctx, 0);
}

void spriteScaled(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    const uint32_t p = arg(ctx, 1);
    uint32_t hash = 2166136261u;
    for (uint32_t i = 0; i < 0x18u && inRam(p); ++i)
        if (i != 0x13u)
            hash = (hash ^ mem<uint8_t>(p + i)) * 16777619u;
    uint32_t otz;
    std::memcpy(&otz, &ctx->f[12], 4);
    logEntry(sym::SpriteRenderScaled, arg(ctx, 0), hash, otz, 0);
    returnWith(ctx, 0);
}

// GetCameraFromTag: one of four cameras in the level's scratch area, by the tag.
void cameraFromTag(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    logEntry(sym::GetCameraFromTag, arg(ctx, 0), arg(ctx, 1), 0, 0);
    returnWith(ctx, kFuzzScratch + kLevel + 0x80u + (arg(ctx, 0) & 3u) * 0x18u);
}

// TEditGetInput's stand-in: the editor's input block.
void getInput(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    logEntry(sym::TEditGetInput, arg(ctx, 0), 0, 0, 0);
    returnWith(ctx, kTE + 0x7cu);
}

struct Stub
{
    uint32_t address;
    RecompFn fn;
};

// Installs the fakes once per test process (each function's test runs in its own process). With heap false the real
// heap stays (for the functions that tail-jump into it).
void install(bool heap)
{
    static bool installed = false;
    if (!g_offline || installed)
        return;
    installed = true;
    const Stub stubs[] = {
        {sym::InputHit, &inputHit},
        {sym::InputHeld, &inputHeld},
        {sym::TileRender, &tileRender},
        {sym::LevelLoad, &logged<sym::LevelLoad, 1>},
        {sym::MainSetCars, &logged<sym::MainSetCars, 1>},
        {sym::FontSet, &logged<sym::FontSet, 1>},
        {sym::LoadSaveSetupClass, &logged<sym::LoadSaveSetupClass, 1>},
        {sym::Rdr_ShapeInit, &logged<sym::Rdr_ShapeInit, 0>},
        {sym::Rdr_ShapeTerm, &logged<sym::Rdr_ShapeTerm, 0>},
        {sym::DepthCueTrackEditorSet, &logged<sym::DepthCueTrackEditorSet, 1>},
        {sym::MVARLinkup, &logged<sym::MVARLinkup, 0>},
        {sym::RCMusicStop, &logged<sym::RCMusicStop, 0>},
        {sym::AMenuLinkItems, &logged<sym::AMenuLinkItems, 5>},
        {sym::SfxPlay, &logged<sym::SfxPlay, 7>},
        {sym::SpriteRenderNew, &spriteRender},
        {sym::SpriteRenderScaled, &spriteScaled},
        {sym::RectRender, &drawn<sym::RectRender, 0x17, 0x17>},
        {sym::RectRenderGouraud, &drawn<sym::RectRenderGouraud, 0x24, 0x17>},
        {sym::LineRender2D, &drawn<sym::LineRender2D, 0xf, 0xf>},
        {sym::TEditRenderTileToHudMap, &logged<sym::TEditRenderTileToHudMap, 5>},
        {sym::GetCameraFromTag, &cameraFromTag},
        {sym::ResTypeSetConstructDaemon, &logged<sym::ResTypeSetConstructDaemon, 3>},
        {sym::ResTypeUnSetConstructDaemon, &logged<sym::ResTypeUnSetConstructDaemon, 1>},
    };
    for (const Stub &s : stubs)
        g_runtime->replaceFunction(s.address, s.fn);
    if (heap)
    {
        g_runtime->replaceFunction(sym::MemBlockAllocLo, &memAllocLo);
        g_runtime->replaceFunction(sym::MemBlockFree, &memFree);
    }
}

// A stand-in for one setup's callees (installed once in its test process).
void standIn(std::initializer_list<Stub> stubs)
{
    static bool installed = false;
    if (!g_offline || installed)
        return;
    installed = true;
    for (const Stub &s : stubs)
        g_runtime->replaceFunction(s.address, s.fn);
}

// --- The world of one input. ---

class World
{
public:
    // walk: the track and the cursor's pieces are walked over the grid (a few modules from near its middle, so they
    // stay within the room around it).
    World(R5900Context *ctx, Rng &rng, uint32_t scratch, bool walk = false, bool heapFakes = true)
        : ctx(ctx), rng(rng), s(scratch), walk(walk)
    {
        install(heapFakes);
        for (uint32_t offset = 0; offset < 0x40u; offset += 4u)
            setEnv(offset, 0);
        mem<uint32_t>(kLog) = 0;
        setEnv(eSeed, rng.next());
        setEnv(eArena, s + kArena);
        setEnv(eKeys, rng.pick({0u, 10u, 30u, 60u, 90u}));
        std::memset(guest_host(0), 0, 0x100);
        // Stale stack words: the leftovers below $sp.
        const uint32_t sp = getRegU32(ctx, 29);
        for (uint32_t offset = 0x400; offset >= 4; offset -= 4)
            mem<uint32_t>(sp - offset) = rng.chance(50) ? rng.pick({0u, 1u, 2u, 0xffffffffu}) : rng.next();
        realModules = mem<uint32_t>(kTE + 0x38u) != 0 && inRam(mem<uint32_t>(kTE + 0x38u)) &&
                      mem<uint32_t>(mem<uint32_t>(kTE + 0x38u)) == kModules;
        tiles();
        modules();
        descs();
        grid();
        state();
        trackList();
        cursorList();
        menu();
        saves();
        menuSystem();
        setReg(ctx, 7, static_cast<uint32_t>(rng.between(-2, 2))); // a leftover slope in $a3
    }

    R5900Context *ctx;
    Rng &rng;
    const uint32_t s;
    const bool walk;
    bool realModules = false;

    int16_t position() { return static_cast<int16_t>(walk ? rng.between(4, 11) : rng.chance(85) ? rng.below(16) : rng.between(-2, 18)); }
    uint32_t walkLength(uint32_t longest) { return walk ? rng.below(4) : longest; }

    sTEditClass &te() { return mem<sTEditClass>(kTE); }

    uint32_t desc(uint32_t id) { return kDescs + id * 4u; }
    uint32_t anyDesc() { return desc(rng.chance(90) ? rng.below(42) : rng.below(50)); }

    int16_t tileId() { return static_cast<int16_t>(rng.below(kTiles)); }

    // Tiles: every cell pointer leads to one of eight shapes, each with its base height.
    void tiles()
    {
        mem<uint16_t>(s + kTrackTiles) = static_cast<uint16_t>(kTiles);
        mem<uint16_t>(s + kTrackTiles + 2u) = 0x400;
        for (uint32_t i = 0; i < kTiles; ++i)
            mem<uint32_t>(s + kTrackTiles + 4u + i * 4u) = s + kTileShapes + rng.below(8) * 0x70u;
        for (uint32_t i = 0; i < 8u; ++i)
            mem<int16_t>(s + kTileShapes + i * 0x70u + 0x6au) =
                static_cast<int16_t>(rng.chance(60) ? rng.pick({0, 0x100, -0x100, 0x200, 0x300}) : rng.between(-0x800, 0x800));
        G(tileVarG).pTrackTiles = ptr32<_sTrackTiles>(s + kTrackTiles);
    }

    // The edit modules, shaped as the game's (or, on the Track Editor dump, half the time the level's own).
    void modules()
    {
        const uint32_t header = s + kModuleHeader;
        if (realModules && rng.chance(50))
        {
            const uint32_t real = mem<uint32_t>(kTE + 0x38u);
            std::memcpy(guest_host(header), guest_host(real), 4u + kModules * 0x18u);
        }
        else
        {
            mem<uint32_t>(header) = kModules;
            for (uint32_t i = 0; i < 0x80u; ++i)
                mem<uint8_t>(s + kPosArrays + i) = rng.chance(70) ? rng.pick({uint8_t(1), uint8_t(0), uint8_t(205), uint8_t(2), uint8_t(67)}) : static_cast<uint8_t>(rng.next());
            for (uint32_t m = 0; m < kModules; ++m)
            {
                const uint32_t at = header + 4u + m * 0x18u;
                const uint16_t size = static_cast<uint16_t>(rng.pick({1, 1, 2, 3, 3}));
                mem<uint16_t>(at) = size;
                mem<uint16_t>(at + 2u) = rng.chance(90) ? size : static_cast<uint16_t>(rng.between(1, 3));
                mem<int16_t>(at + 4u) = static_cast<int16_t>(rng.pick({0, 0, 1, 3, 2}));
                mem<int16_t>(at + 6u) = static_cast<int16_t>(rng.chance(50) ? 0 : rng.pick({1, 2, 3, 4, 6, -3, -6}));
                const int32_t in = rng.chance(60) ? 0 : rng.between(-2, 2);
                mem<int32_t>(at + 8u) = in;
                mem<int32_t>(at + 0xcu) = rng.chance(60) ? in : rng.between(-2, 2);
                mem<uint32_t>(at + 0x10u) = s + kPosArrays + rng.below(0x50);
                mem<uint32_t>(at + 0x14u) = s + kTileArrays + m * 0x24u;
                for (uint32_t t = 0; t < 9u; ++t)
                    mem<uint32_t>(s + kTileArrays + m * 0x24u + t * 4u) = rng.chance(10) ? 0u : rng.below(kTiles);
            }
        }
        te().pEditModuleResourceHeader = ptr32<_sEditModuleResourceHeader>(header);
        te().pEditModuleList = ptr32<_sEditModule>(header + 4u);
    }

    // editModuleListG: the game's table, a few entries changed (ids below 42, the mirror flag).
    void descs()
    {
        const uint32_t changes = rng.chance(60) ? 0u : rng.below(6);
        for (uint32_t i = 0; i < changes; ++i)
        {
            const uint32_t at = desc(rng.below(50));
            mem<uint16_t>(at) = static_cast<uint16_t>(rng.below(kModules));
            mem<uint16_t>(at + 2u) = static_cast<uint16_t>(rng.chance(70) ? rng.below(2) : rng.below(0x10000));
        }
    }

    // The grid: a scattering of tiles in the first layer, fewer above, the rest empty.
    void grid()
    {
        const uint32_t units = s + kUnits;
        const uint32_t density = rng.pick({0u, 10u, 30u, 60u, 95u});
        for (uint32_t i = 0; i < 0x300u; ++i)
        {
            const uint32_t layer = i / 0x100u;
            // A walked track adds its own units: with the upper layers empty it never needs a fourth (which would be
            // written past the grid's three, as in the game).
            const bool filled = (!walk || layer == 0) && rng.chance(layer == 0 ? density : density / (layer * 2u + 1u));
            mem<int16_t>(units + i * 8u) = filled ? tileId() : -1;
            mem<int16_t>(units + i * 8u + 2u) = static_cast<int16_t>(rng.chance(60) ? rng.pick({0, 0x100, 0x300, 0x600, -0x300, 0x900}) : rng.between(-0x1000, 0x1000));
            mem<uint32_t>(units + i * 8u + 4u) = rng.chance(85) ? rng.below(4) : rng.next();
        }
        te().track.pTrackUnits = ptr32<_sTrackUnit>(units);
    }

    void state()
    {
        sTEditClass &t = te();
        t.cursor.xPos = position();
        t.cursor.yPos = position();
        t.cursor.xMax = 0x10;
        t.cursor.yMax = 0x10;
        t.cursor.xSize = static_cast<int16_t>(rng.between(1, 3));
        t.cursor.ySize = static_cast<int16_t>(rng.between(1, 3));
        t.cursor.currentSlope = static_cast<eSlope>(rng.chance(50) ? 0 : rng.between(-2, 2));
        t.cursor.direction = static_cast<eDirection>(rng.below(4));
        t.cursor.elevation = static_cast<int16_t>(rng.chance(40) ? 0 : rng.chance(50) ? rng.pick({0x100, 0x300, 0x600, -0x300, 0x900, 0xc00})
                                                        // near the editor's top and bottom heights (elevation / 256 above 22, below -14)
                                                        : rng.pick({22, 23, 24, 25, 26, 27, 28, 29, -14, -15, -16, -17, -18, -19, -20, -21}) * 0x100 + static_cast<int32_t>(rng.below(0x100)));
        t.trackDesc.startOrientation = static_cast<eDirection>(rng.chance(50) ? t.cursor.direction : rng.below(4));
        t.trackDesc.themeTypeNdx = rng.below(6);
        t.trackDesc.soundTrackNdx = rng.below(8);
        t.trackDesc.bestLapTime = rng.chance(50) ? 0x9275cu : rng.below(0x100000);
        t.trackDesc.startElevation = rng.chance(60) ? t.cursor.elevation : static_cast<int16_t>(rng.pick({0, 0x300, 0x600}));
        t.trackDesc.startXPos = static_cast<uint8_t>(rng.chance(50) ? t.cursor.xPos : position());
        t.trackDesc.startYPos = static_cast<uint8_t>(rng.chance(50) ? t.cursor.yPos : position());
        for (int i = 0; i < 6; ++i)
            t.pTrackTiles[i] = ptr32<_sTrackTiles>(s + kTrackTiles);
        t.trackState.currentMode = static_cast<uint16_t>(rng.chance(85) ? rng.below(4) : rng.below(6));
        t.trackState.editState = static_cast<uint16_t>(rng.below(6));
        t.trackState.menuCurrentState = static_cast<uint16_t>(rng.below(12));
        t.trackState.menuNextState = static_cast<uint16_t>(rng.below(12));
        t.status = rng.below(4);
        t.placementViewDir = static_cast<eDirection>(rng.below(4));
        t.newPlacementViewDir = static_cast<eDirection>(rng.below(4));
        t.cameraTurning = rng.chance(70) ? 0 : 1;
        t.newCameraMode = rng.below(2);
        t.display2DGridMap = rng.below(2);
        t.trackIsComplete = rng.below(2);
        t.errorFlags = rng.chance(50) ? 0 : rng.below(0x40);
        t.placementErrorChecking = rng.below(2);
        t.loadSaveFlags = rng.below(8);
        t.resourceMemoryUsed = rng.chance(50) ? rng.below(1000) * 1000u : rng.next();
        t.resourceErrorCheckMemoryUsed = rng.next();
        t.pSavedTrackData = ptr32<int32>(s + kSaved);
        G(mainVarG).trackEdRace = rng.chance(85) ? 0 : 1;
        G(bLoadingG) = 1; // LoadingInit (TEditTerm's tail jump) plays no movie
        for (uint32_t i = 0; i < 4u; ++i) // the sticks (inputVarG.analog1A...)
            mem<int8_t>(sym::inputVarG + 0xbcu + i) = static_cast<int8_t>(rng.chance(50) ? rng.pick({0, 0x50, 0x51, -0x50, -0x51, 0x7f, -0x80}) : rng.next());
    }

    // The track: up to 47 modules, the list ending with a null link.
    void trackList()
    {
        const uint32_t n = walkLength(rng.chance(15) ? 0u : rng.chance(80) ? rng.between(1, 12) : rng.between(13, 47));
        for (uint32_t i = 0; i < n; ++i)
        {
            const uint32_t node = s + kTrackNodes + i * 8u;
            mem<uint32_t>(node) = i + 1u < n ? node + 8u : 0u;
            mem<uint32_t>(node + 4u) = rng.chance(95) ? rng.below(42) : rng.below(50);
        }
        te().trackDesc.pTrackModuleList = ptr32<_sTrackModule>(n ? s + kTrackNodes : 0u);
    }

    void cursorList()
    {
        const uint32_t n = walkLength(rng.chance(20) ? 0u : rng.between(1, 6));
        for (uint32_t i = 0; i < n; ++i)
        {
            const uint32_t node = s + kCursorNodes + i * 8u;
            mem<uint32_t>(node) = i + 1u < n ? node + 8u : 0u;
            mem<uint32_t>(node + 4u) = anyDesc();
        }
        te().cursor.pCursorModuleList = ptr32<_sCursorModule>(n ? s + kCursorNodes : 0u);
    }

    // The edit menu: each slope row's seven modules (or none), what is available and restricted.
    void menu()
    {
        sTEditClass &t = te();
        sEditMenuState &e = t.editMenu;
        e.screenXPos = static_cast<uint16_t>(rng.below(0x200));
        e.screenYPos = static_cast<uint16_t>(rng.below(0x100));
        e.rowNdx = static_cast<uint8_t>(rng.below(4));
        e.moduleNdx = static_cast<uint8_t>(rng.below(7));
        e.slopeNdx = static_cast<uint8_t>(rng.below(5));
        e.specialNdx = static_cast<uint8_t>(rng.below(7));
        for (int r = 0; r < 5; ++r)
            e.slopeRestrict[r] = static_cast<uint8_t>(rng.chance(70) ? 0 : 1);
        for (int i = 0; i < 2; ++i)
            for (int j = 0; j < 7; ++j)
            {
                e.pEditModuleSpecial[i][j] = ptr32<_sEditModuleDesc>(rng.chance(85) ? anyDesc() : 0u);
                e.specialRestrict[i][j] = static_cast<int8_t>(rng.chance(70) ? 0 : 1);
            }
        e.noSpecialAvail = static_cast<int8_t>(rng.chance(80) ? 0 : 1);
        for (int r = 0; r < 5; ++r)
        {
            sEditGridSlopeInstance &g = t.editMenuGridSlopes[r];
            for (int j = 0; j < 7; ++j)
            {
                g.pEditModuleDesc[j] = ptr32<_sEditModuleDesc>(rng.chance(75) ? anyDesc() : 0u);
                g.avail[j] = static_cast<int8_t>(rng.chance(30) ? 0 : 1);
                g.restrictAvail[j] = static_cast<int8_t>(rng.chance(70) ? 0 : 1);
            }
        }
    }

    // The menu system in scratch (pMenuSystemG), its active menu the editor's main menu half the time; the editor's
    // menus are scratch addresses.
    void menuSystem()
    {
        sTEditClass &t = te();
        const uint32_t sys = s + kMenuSys;
        for (uint32_t i = 0; i < 0x100u; i += 4u)
            mem<uint32_t>(sys + i) = rng.next();
        for (uint32_t i = 0; i < 4; ++i)
            t.pTrackWarnings[i] = ptr32<sAMenuHeader>(s + kMenuSys + 0x40u + i * 8u);
        t.pTrackMainMenu = ptr32<sAMenuHeader>(s + kMenuSys + 0x80u);
        mem<uint32_t>(sys) = rng.chance(50) ? t.pTrackMainMenu.address : s + kMenuSys + 0x90u;
        G(pMenuSystemG) = ptr32<sMenuSystem>(sys);
        for (uint32_t i = 0; i < 0xc0u; i += 4u)
            mem<uint32_t>(s + kSprites + i) = rng.next();
        t.pTrackSprites = ptr32<_sTrackSprites>(s + kSprites);
        G(frontEndVarG).flagPTT = static_cast<int8_t>(rng.chance(50) ? 0 : 1);
    }

    // The permanent buffer: a description and up to 40 module ids, ending -1 (rarely at once); the memory card buffer:
    // the same with its checksum right most of the time.
    void saves()
    {
        const uint32_t saved = s + kSaved;
        for (uint32_t i = 0; i < 0x200u; i += 4u)
            mem<uint32_t>(saved + i) = rng.next();
        mem<uint32_t>(saved) = 0x76543210u;
        mem<uint32_t>(saved + 4u) = rng.below(4);
        mem<uint32_t>(saved + 8u) = rng.below(6);
        mem<uint32_t>(saved + 0xcu) = rng.below(8);
        mem<uint32_t>(saved + 0x10u) = rng.chance(50) ? 0x9275cu : rng.below(0x100000);
        mem<int16_t>(saved + 0x14u) = static_cast<int16_t>(rng.pick({0, 0x300, 0x600}));
        mem<uint8_t>(saved + 0x16u) = static_cast<uint8_t>(position());
        mem<uint8_t>(saved + 0x17u) = static_cast<uint8_t>(position());
        const uint32_t n = walk ? rng.between(1, 3) : rng.chance(5) ? 0u : rng.between(1, 40); // an empty list gives module -1
        for (uint32_t i = 0; i < n; ++i)
            mem<uint32_t>(saved + 0x18u + i * 4u) = rng.chance(95) ? rng.below(42) : rng.below(50);
        mem<uint32_t>(saved + 0x18u + n * 4u) = 0xffffffffu;

        const uint32_t buffer = s + kSaveGame;
        for (uint32_t i = 0; i < 0x404u; i += 4u)
            mem<uint32_t>(buffer + i) = rng.next();
        std::memcpy(guest_host(buffer + 0x204u), guest_host(saved), 0x200u);
        if (rng.chance(60)) // a loaded track other than the kept one (its last words too)
            for (uint32_t i = 0x1c0u; i < 0x200u; i += 4u)
                mem<uint32_t>(buffer + 0x204u + i) = rng.next();
        if (rng.chance(70))
        {
            uint32_t sum = 0;
            for (uint32_t i = 0; i < 0x200u; i += 4u)
                sum ^= mem<uint32_t>(buffer + 0x204u + i);
            mem<uint32_t>(buffer + 0x200u) = sum;
        }
        G(loadSaveVarG).pSaveGameBuffer = ptr32<void>(buffer);
    }
};

} // namespace

void teditPlain(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    World w(ctx, rng, scratch);
}

// The real heap: the dump's track and save blocks (allocated at boot), or none.
void teditWalk(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    World w(ctx, rng, scratch, true);
}

// TEditDisplay and TEditRenderSkyDome: a walked track and cursor, the drawing and menus stood in but the sky (the
// tail jump) drawn for real, a level's sky on the Track Editor dump, else the effects' shape (visfx6's) for every theme.
void teditDisplay(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    uint32_t skies[6];
    std::memcpy(skies, guest_host(kTE + 0x5cu), sizeof(skies));
    World w(ctx, rng, scratch, true);
    standIn({{sym::TEditDisplayMainMenus, &logged<sym::TEditDisplayMainMenus, 2>},
             {sym::TEditDisplay3DGridLines, &logged<sym::TEditDisplay3DGridLines, 0>},
             {sym::TEditRenderTrack, &logged<sym::TEditRenderTrack, 0>},
             {sym::AMenuDisplayOptions, &logged<sym::AMenuDisplayOptions, 1>},
             {sym::AMenuProcessAll, &logged<sym::AMenuProcessAll, 1>},
             {sym::CellRenderInit, &logged<sym::CellRenderInit, 1>},
             {sym::TEditDEBUGDisplayCursor, &logged<sym::TEditDEBUGDisplayCursor, 0>},
             {sym::TEditRenderEditMenu, &logged<sym::TEditRenderEditMenu, 1>},
             {sym::BuildTrackEditorHelp, &logged<sym::BuildTrackEditorHelp, 1>}});
    const uint32_t vfx = G(visFxVarG).pVfxRes.address;
    for (uint32_t i = 0; i < 6u; ++i)
        mem<uint32_t>(kTE + 0x5cu + i * 4u) = w.realModules ? skies[i] : mem<uint32_t>(vfx + 0x88u);
    w.te().trackState.editState = static_cast<uint16_t>(rng.below(5));
    setArg(ctx, 0, rng.next());
    setArg(ctx, 1, rng.next());
}

// Functions whose callees draw or build menus: those are stand-ins.
void teditStandIns(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    World w(ctx, rng, scratch);
    standIn({{sym::CellRenderInit, &logged<sym::CellRenderInit, 1>},
             {sym::TEditDisplay3DGridLines, &logged<sym::TEditDisplay3DGridLines, 0>},
             {sym::BuildTrackEditorHelp, &logged<sym::BuildTrackEditorHelp, 1>},
             {sym::TEditRenderTrack, &logged<sym::TEditRenderTrack, 0>}});
    setArg(ctx, 0, scratch + kArgs);
    setArg(ctx, 1, rng.next());
}

// A level whose first path holds camera tags (and others), the AI nodes a ring linked back through prev[0] with their
// centres spread so some are more than 0x400 apart; a0 a camera and a1 a node handle (TEditCopyAINodePosToCameraNode).
void teditCameraPaths(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    World w(ctx, rng, scratch);
    const uint32_t nodes = scratch + kAINodes, count = static_cast<uint32_t>(rng.between(3, 24));
    const int32_t spread = rng.pick({0x200, 0x600, 0x1000, 0x4000});
    for (uint32_t i = 0; i < 24u; ++i)
    {
        const uint32_t node = nodes + i * 0x6cu;
        for (uint32_t o = 0; o < 0x6cu; o += 4u)
            mem<uint32_t>(node + o) = rng.next();
        mem<uint32_t>(node + 8u) = nodes + ((i + count - 1u) % count) * 0x6cu;
        for (uint32_t c = 0; c < 3u; ++c)
            mem<int16_t>(node + 0x28u + c * 2u) = static_cast<int16_t>(rng.between(-spread, spread));
    }
    const uint32_t start = rng.below(count);
    G(aiNodeVarG).pNode = ptr32<_sAINode>(nodes + start * 0x6cu);
    if (rng.chance(40)) // the node behind about 0x400 away (VecLength rounds)
    {
        const uint32_t from = nodes + start * 0x6cu, behind = mem<uint32_t>(from + 8u);
        if (behind != from)
            for (uint32_t c = 0; c < 3u; ++c)
                mem<int16_t>(behind + 0x28u + c * 2u) = static_cast<int16_t>(mem<int16_t>(from + 0x28u + c * 2u) + (c == 0 ? rng.between(0x3fc, 0x404) : 0));
    }
    // At least one node of the ring far from every other (the walk back ends there): not the start or the one behind.
    mem<int16_t>(nodes + ((start + 1u) % count) * 0x6cu + 0x28u) = static_cast<int16_t>(rng.pick({0x3000, -0x3000}));
    const uint32_t level = scratch + kLevel;
    for (uint32_t i = 0; i < 0x100u; i += 4u)
        mem<uint32_t>(level + i) = rng.next();
    mem<uint32_t>(level + 0x20u) = level + 0x30u;
    mem<uint32_t>(level + 0x30u) = level + 0x40u;
    const uint32_t tags = rng.below(8);
    mem<uint8_t>(level + 0x40u) = static_cast<uint8_t>(tags);
    for (uint32_t i = 0; i < tags; ++i)
        mem<uint32_t>(level + 0x48u + i * 4u) = rng.chance(70) ? 0x10000000u + rng.below(0x100) : rng.pick({0u, 0x0fffffffu, 0x80000000u, 0x123u});
    G(pLvlG).address = level;
    setArg(ctx, 0, level + 0x80u + rng.below(4) * 0x18u);
    mem<uint32_t>(scratch + kArgs) = G(aiNodeVarG).pNode.address;
    setArg(ctx, 1, scratch + kArgs);
}

// A unit on a map: a0-a1 a position, a2 a tile picture, a3 a height, t0 an orientation (0x80 mirrored).
void teditMapUnit(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    World w(ctx, rng, scratch);
    setArg(ctx, 0, static_cast<uint32_t>(rng.chance(90) ? rng.below(16) : rng.between(-4, 20)));
    setArg(ctx, 1, static_cast<uint32_t>(rng.chance(90) ? rng.below(16) : rng.between(-4, 20)));
    setArg(ctx, 2, static_cast<uint32_t>(static_cast<int8_t>(rng.chance(90) ? rng.below(0x32) : rng.next())));
    setArg(ctx, 3, rng.next());
    // The orientations TEditAdjustCursorModule passes: a direction, 0x80 for a mirrored piece. (A larger one would read
    // the flag table past its four bytes, into the original's saved registers.)
    setReg(ctx, 8, rng.below(4) | (rng.chance(30) ? 0x80u : 0u));
}

// The edit menu's keys: a0 the input bits.
void teditMenuInput(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    World w(ctx, rng, scratch);
    sEditMenuState &e = w.te().editMenu;
    e.rowNdx = static_cast<uint8_t>(rng.below(4));
    if (rng.chance(40)) // the second special row closed up to a column and beyond it, open only past it
    {
        const uint32_t open = rng.below(7);
        for (uint32_t j = 0; j < 7u; ++j)
            e.specialRestrict[1][j] = static_cast<int8_t>(j == open ? 0 : 1);
        e.specialNdx = static_cast<uint8_t>(rng.below(open + 1u));
    }
    setArg(ctx, 0, rng.chance(85) ? rng.below(0x10) : rng.next());
}

// TEditPlaceModule: a0-a1 a position, a2 a tile, a3 an elevation, t0 an orientation; checking for errors half the time.
void teditPlace(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    World w(ctx, rng, scratch);
    sTEditClass &t = w.te();
    t.placementErrorChecking = rng.chance(50) ? 1 : rng.below(3);
    t.resourceErrorCheckMemoryUsed = rng.chance(50) ? 0x19000u - rng.below(3) * 1000u : rng.next();
    setArg(ctx, 0, rng.below(16));
    setArg(ctx, 1, rng.below(16));
    setArg(ctx, 2, static_cast<uint32_t>(w.tileId()));
    setArg(ctx, 3, static_cast<uint32_t>(rng.chance(60) ? rng.pick({0, 0x300, 0x600, -0x300, 0x900}) : rng.between(-0x1000, 0x1000)));
    setReg(ctx, 8, rng.chance(85) ? rng.below(4) : rng.next());
}

// DeleteTrackModuleFromTrackModuleList: a walked track of at least one module.
void teditDelete(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    World w(ctx, rng, scratch, true);
    sTEditClass &t = w.te();
    if (!t.trackDesc.pTrackModuleList)
    {
        mem<uint32_t>(scratch + kTrackNodes) = 0;
        mem<uint32_t>(scratch + kTrackNodes + 4u) = rng.below(42);
        t.trackDesc.pTrackModuleList = ptr32<_sTrackModule>(scratch + kTrackNodes);
    }
}

// The grid editor's control step and the pad: the edit state's handlers run.
void teditGridControl(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    World w(ctx, rng, scratch);
    w.te().trackState.editState = static_cast<uint16_t>(rng.chance(90) ? rng.below(5) : rng.below(0x10000));
    w.te().input.analog1LastInput = rng.chance(70) ? rng.pick({0u, 1u, 2u, 4u, 8u, 5u, 10u}) : rng.next();
    setArg(ctx, 0, rng.next());
}

// The grid editor's display step: a walked track and cursor, the 3D drawing and menus stood in.
void teditGridDisplay(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    World w(ctx, rng, scratch, true);
    standIn({{sym::AMenuDisplayOptions, &logged<sym::AMenuDisplayOptions, 1>},
             {sym::AMenuProcessAll, &logged<sym::AMenuProcessAll, 1>},
             {sym::CellRenderInit, &logged<sym::CellRenderInit, 1>},
             {sym::TEditDisplay3DGridLines, &logged<sym::TEditDisplay3DGridLines, 0>},
             {sym::TEditDEBUGDisplayCursor, &logged<sym::TEditDEBUGDisplayCursor, 0>},
             {sym::TEditRenderTrack, &logged<sym::TEditRenderTrack, 0>},
             {sym::TEditRenderEditMenu, &logged<sym::TEditRenderEditMenu, 1>}});
    w.te().trackState.editState = static_cast<uint16_t>(rng.chance(90) ? rng.below(5) : rng.below(0x10000));
    mem<uint32_t>(scratch + kMenuSys + 0x68u) = rng.chance(50) ? 0x7fff0001u : rng.below(3);
    // Times at the cursor's blink (half of each second) and anywhere.
    setArg(ctx, 0, static_cast<uint32_t>(rng.chance(40) ? static_cast<int32_t>(rng.below(100)) * 1000 + rng.between(499, 501)
                                         : rng.chance(80) ? rng.between(0, 100000) : static_cast<int32_t>(rng.next())));
}

void teditHeap(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    const uint32_t units = mem<uint32_t>(kTE + 0x1cu), saved = mem<uint32_t>(kTE + 0x3cu);
    World w(ctx, rng, scratch, false, false);
    sTEditClass &t = w.te();
    t.track.pTrackUnits = ptr32<_sTrackUnit>(rng.chance(80) ? units : 0u);
    t.pSavedTrackData = ptr32<int32>(rng.chance(80) ? saved : 0u);
    t.cursor.pCursorModuleList = nullptr;
}

void teditLevelData(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    World w(ctx, rng, scratch);
    const uint32_t data = scratch + kArgs;
    for (uint32_t i = 0; i < 0xa8u; i += 4u)
        mem<uint32_t>(data + i) = rng.chance(80) ? scratch + rng.below(0x7000) : rng.next();
    mem<uint32_t>(data) = scratch + kModuleHeader;
    // TEditSetupTCLvLClass and TEditSetupTALvLClass index tables by the theme.
    w.te().trackDesc.themeTypeNdx = rng.below(6);
    setArg(ctx, 0, data);
}

void teditUnitCount(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    World w(ctx, rng, scratch);
    setArg(ctx, 0, static_cast<uint32_t>(rng.chance(90) ? rng.below(16) : rng.between(-3, 18)));
    setArg(ctx, 1, static_cast<uint32_t>(rng.chance(90) ? rng.below(16) : rng.between(-3, 18)));
    setArg(ctx, 2, scratch + kArgs + rng.below(0x10));
    setArg(ctx, 3, scratch + kArgs + 0x20u + rng.below(8) * 2u);
}

void teditSetUnit(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    World w(ctx, rng, scratch);
    setArg(ctx, 0, rng.chance(90) ? rng.below(16) : rng.below(0x10000));
    setArg(ctx, 1, rng.chance(90) ? rng.below(16) : rng.below(0x40));
    const uint32_t unit = rng.chance(10) ? scratch + kUnits + rng.below(0x300) * 8u : scratch + kArgs + 0x40u;
    if (unit == scratch + kArgs + 0x40u)
    {
        mem<int16_t>(unit) = w.tileId();
        mem<int16_t>(unit + 2u) = static_cast<int16_t>(rng.next());
        mem<uint32_t>(unit + 4u) = rng.chance(80) ? rng.below(4) : rng.next();
    }
    setArg(ctx, 2, unit);
    setArg(ctx, 3, rng.chance(90) ? rng.below(3) : rng.below(0x100));
    // Out of the grid's three layers (a layer above 2 or a position past 16 x 16): kept inside scratch.
    const int32_t layer = (arg(ctx, 3) & 0xffu) != 0 ? static_cast<int8_t>(arg(ctx, 3)) * 256 : 0;
    const int32_t index = static_cast<int16_t>(layer + static_cast<int32_t>(arg(ctx, 0) & 0xffffu) + static_cast<int32_t>(arg(ctx, 1) & 0xffffu) * 16);
    if (index < 0 || index >= 0xf00)
        setArg(ctx, 3, 0);
}

void teditMeter(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    World w(ctx, rng, scratch);
    const uint32_t counter = rng.chance(50) ? kTE + 0x254u : rng.chance(50) ? kTE + 0x258u : scratch + kArgs + 0x60u;
    setArg(ctx, 0, counter);
    setArg(ctx, 1, static_cast<uint32_t>(static_cast<int32_t>(w.tileId())));
}

void teditGridBlocked(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    World w(ctx, rng, scratch);
    setArg(ctx, 0, static_cast<uint32_t>(rng.chance(90) ? rng.below(16) : rng.between(-3, 18)));
    setArg(ctx, 1, static_cast<uint32_t>(rng.chance(90) ? rng.below(16) : rng.between(-3, 18)));
}

void teditSlopeModule(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    World w(ctx, rng, scratch);
    // a0: a slope (-2..2), a module id (0..49) or anything; a1: an editModuleListG id or a flag.
    setArg(ctx, 0, static_cast<uint32_t>(rng.chance(40) ? rng.between(-2, 2) : rng.chance(80) ? rng.between(0, 49) : static_cast<int32_t>(rng.below(0x10000))));
    setArg(ctx, 1, rng.chance(85) ? rng.below(50) : rng.below(2) );
    setArg(ctx, 2, scratch + kArgs + 0x80u);
}

void teditMenuCell(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    World w(ctx, rng, scratch);
    sTEditClass &t = w.te();
    const uint32_t row = rng.below(5), column = rng.below(7);
    if (!t.editMenuGridSlopes[row].pEditModuleDesc[column])
        t.editMenuGridSlopes[row].pEditModuleDesc[column] = ptr32<_sEditModuleDesc>(w.anyDesc());
    setArg(ctx, 0, row);
    setArg(ctx, 1, column);
}

void teditRestrict(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    World w(ctx, rng, scratch);
    setArg(ctx, 0, rng.chance(70) ? rng.below(0x20) : rng.next());
    setArg(ctx, 1, rng.chance(70) ? rng.below(0x20) : rng.next());
}

// A chain of AI nodes linked back through prev[0], with their link distances, from node 3 of the array.
void teditAINodes(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    World w(ctx, rng, scratch);
    const uint32_t nodes = scratch + kAINodes;
    for (uint32_t i = 0; i < 24u; ++i)
    {
        const uint32_t node = nodes + i * 0x6cu;
        for (uint32_t o = 0; o < 0x6cu; o += 4u)
            mem<uint32_t>(node + o) = rng.next();
        mem<uint32_t>(node + 8u) = nodes + (rng.chance(80) ? (i + 23u) % 24u : rng.below(24)) * 0x6cu;
        mem<int32_t>(node + 0x60u) = rng.chance(80) ? static_cast<int32_t>(rng.below(0x2000)) : static_cast<int32_t>(rng.next());
    }
    sAINodeClass &ai = G(aiNodeVarG);
    ai.pNode = ptr32<_sAINode>(nodes);
    ai.noNodesSingle = static_cast<int16_t>(rng.chance(85) ? rng.between(0, 40) : rng.between(-3, 200));
    ai.nodeTotalDist = static_cast<int32_t>(rng.next());
}

} // namespace rc::fuzz
