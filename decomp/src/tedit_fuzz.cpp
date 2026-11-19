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
constexpr uint32_t kHudBits = 0x5c00u; // the spread tile pictures (pHudMapBitsBPP), 0x400: in the arena's top, for the
constexpr uint32_t kHudMap = 0x6200u;  // setups that draw on the hud map; the byte map (pHudMapBPP), 0x1000, with room
                                       // either side for units off the map
enum : uint32_t
{
    eSeed = 0x0,
    eCalls = 0x4,
    eHash = 0x8,
    eArena = 0xc,  // the next free block
    eKeys = 0x10,  // percent of InputHit calls that answer true
    eArenaLo = 0x14, // the heap stand-ins' area (the scratch arena, or the searches' large one)
    eArenaHi = 0x18,
    eFree = 0x1c,  // freed blocks, linked through their first word (each reused as a block of up to 16 bytes)
};

// The searches' heap: unused RAM past the runtime's own guest heap (their stack alone is 0x3cf0 bytes). Outside the
// scratch area, so those functions compare all of RAM.
constexpr uint32_t kBigArena = 0x4d0000u, kBigArenaEnd = 0x4f8000u;

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

// CellRenderInit: its vector (the callers clear it on their stack).
void cellRenderInit(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    const uint32_t p = arg(ctx, 0);
    const bool ok = inRam(p);
    logEntry(sym::CellRenderInit, p, ok ? mem<uint32_t>(p) : 0u, ok ? mem<uint32_t>(p + 4u) : 0u, ok ? mem<uint32_t>(p + 8u) : 0u);
    returnWith(ctx, 0);
}

// TColBuildLvlCollision: the count, the four arrays and the bounds (two vectors on the caller's stack).
void tcolBuild(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    uint32_t hash = 2166136261u;
    for (const int reg : {9, 10})
        for (uint32_t i = 0; i < 6u && inRam(getRegU32(ctx, reg)); ++i)
            hash = (hash ^ mem<uint8_t>(getRegU32(ctx, reg) + i)) * 16777619u;
    logEntry(sym::TColBuildLvlCollision, arg(ctx, 0), arg(ctx, 1) ^ arg(ctx, 2) * 31u, arg(ctx, 3) ^ arg(ctx, 4) * 31u, hash);
    logEntry(sym::TColBuildLvlCollision, getRegU32(ctx, 9), getRegU32(ctx, 10), 0, 0);
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
    const uint32_t lo = env(eArenaLo), hi = env(eArenaHi);
    uint32_t at = env(eArena), take;
    if (size <= 16u && env(eFree) != 0)
    {
        // A freed block again (the searches allocate and free thousands of list nodes).
        at = env(eFree);
        setEnv(eFree, mem<uint32_t>(at));
        take = 16u;
    }
    else
    {
        if (at + 0x100u > hi || size > hi - lo)
            at = lo;
        const uint32_t room = hi - at;
        take = ((size < room ? size : room) + 15u) & ~15u;
        setEnv(eArena, at + take);
    }
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
    const uint32_t block = inRam(handle) ? mem<uint32_t>(handle) : 0u;
    logEntry(sym::MemBlockFree, handle, block, 0, 0);
    if (inRam(handle))
        mem<uint32_t>(handle) = 0;
    // Only the large arena recycles: its blocks are the stand-in's own, never the setup's lists.
    if (env(eArenaLo) == kBigArena && block >= kBigArena && block + 16u <= kBigArenaEnd && (block & 15u) == 0)
    {
        mem<uint32_t>(block) = env(eFree);
        setEnv(eFree, block);
    }
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
        {sym::LineRender3D, &drawn<sym::LineRender3D, 0x17, 0x17>},
        {sym::TileRenderGameROB, &logged<sym::TileRenderGameROB, 6>},
        {sym::TColBuildLvlCollision, &tcolBuild},
        {sym::sceGsSetDefLoadImage, &logged<sym::sceGsSetDefLoadImage, 7>},
        {sym::FlushCache, &logged<sym::FlushCache, 1>},
        {sym::sceGsExecLoadImage, &logged<sym::sceGsExecLoadImage, 2>},
        {sym::sceGsSyncPath, &logged<sym::sceGsSyncPath, 2>},
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

// A stand-in for one setup's callees (offline only). Every call installs its list: a single "installed" flag made the
// second list of a setup that calls two (teditMainMenus: searchStandIns, then its own) a no-op, so LoadSaveProcess and
// BuildTrackEditorHelp ran for real and a random menu state could send the help's string wrapping off the end of RAM.
void standIn(std::initializer_list<Stub> stubs)
{
    if (!g_offline)
        return;
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
        setEnv(eArenaLo, s + kArena);
        setEnv(eArenaHi, s + kArenaEnd);
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

    // The hud map's buffers in scratch (the heap stand-ins' arena ends before them).
    void hudBuffers()
    {
        for (uint32_t i = 0; i < 0x400u; ++i)
            mem<uint8_t>(s + kHudBits + i) = static_cast<uint8_t>(rng.chance(30) ? 0u : rng.below(16));
        te().pHudMapBitsBPP = ptr32<int32>(s + kHudBits);
        te().pHudMapBPP = ptr32<int32>(s + kHudMap);
        setEnv(eArenaHi, s + kHudBits);
    }

    // Every slope row of the edit menu offers at least one module, as in the game. (With none, the edit menu's
    // selection ends on column 7 or -1, TEditSetNewCursorModule takes the cell past the row for a module and the
    // pieces to its "slope" come from registers TEditBuildTransitionCursorModules never set.)
    void openRows()
    {
        for (int r = 0; r < 5; ++r)
        {
            sEditGridSlopeInstance &g = te().editMenuGridSlopes[r];
            bool open = false;
            for (int j = 0; j < 7; ++j)
                open = open || (g.avail[j] != 0 && g.restrictAvail[j] == 0);
            if (!open)
            {
                const uint32_t j = rng.below(7);
                g.avail[j] = 1;
                g.restrictAvail[j] = 0;
            }
        }
    }

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
             {sym::AMenuDisplayOptions, &logged<sym::AMenuDisplayOptions, 1>},
             {sym::AMenuProcessAll, &logged<sym::AMenuProcessAll, 1>},
             {sym::CellRenderInit, &cellRenderInit},
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
    standIn({{sym::CellRenderInit, &cellRenderInit},
             {sym::BuildTrackEditorHelp, &logged<sym::BuildTrackEditorHelp, 1>},});
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
    w.hudBuffers();
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
    // As in the game: nothing on the cursor only once the track is complete. (Key 4 would else store a register for the
    // cursor's slope, and the pieces to an unknown slope are taken from registers in turn.)
    if (!w.te().cursor.pCursorModuleList)
        w.te().trackIsComplete = 1;
    w.openRows();
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
             {sym::CellRenderInit, &cellRenderInit},});
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

// --- Part 2: the cursor engine, auto-complete and random tracks. ---

namespace {

constexpr uint32_t kEntryAt = kArgs + 0x40u; // an sAutoCompStackData (0x9c bytes)

// An auto-complete stack entry: the cells sorted or not, a few scored -1.
void autoCompEntry(World &w, uint32_t entry)
{
    Rng &rng = w.rng;
    for (uint32_t i = 0; i < 0x9cu; i += 4u)
        mem<uint32_t>(entry + i) = rng.next();
    mem<uint8_t>(entry) = static_cast<uint8_t>(rng.below(35)); // the callers place a piece only below 35
    for (uint32_t i = 0; i < 35u; ++i)
    {
        const uint32_t piece = entry + 0x10u + i * 4u;
        mem<uint8_t>(piece) = static_cast<uint8_t>(rng.below(7));
        mem<uint8_t>(piece + 1u) = static_cast<uint8_t>(rng.below(5));
        mem<int16_t>(piece + 2u) = static_cast<int16_t>(rng.chance(30) ? -1 : rng.chance(70) ? rng.between(0, 120) : rng.between(-3, 300));
    }
}

// Every cell of the edit menu holds a module (the functions that try the cells follow the pointers).
void fillMenu(World &w)
{
    sTEditClass &t = w.te();
    for (int r = 0; r < 5; ++r)
        for (int j = 0; j < 7; ++j)
            if (!t.editMenuGridSlopes[r].pEditModuleDesc[j])
                t.editMenuGridSlopes[r].pEditModuleDesc[j] = ptr32<_sEditModuleDesc>(w.anyDesc());
    for (int i = 0; i < 2; ++i)
        for (int j = 0; j < 7; ++j)
            if (!t.editMenu.pEditModuleSpecial[i][j])
                t.editMenu.pEditModuleSpecial[i][j] = ptr32<_sEditModuleDesc>(w.anyDesc());
}

// The registers a function can be left to read: a slope where one is taken for the cursor's slope ($s5, $s6, $fp; a
// slope the transition table does not know would then make TEditBuildTransitionCursorModules read registers in turn),
// small values elsewhere.
void leftovers(R5900Context *ctx, Rng &rng)
{
    for (const int reg : {8, 9, 10, 11, 16, 19, 20, 23})
        setReg(ctx, reg, static_cast<uint32_t>(rng.chance(70) ? rng.between(-2, 2) : rng.between(0, 49)));
    for (const int reg : {21, 22, 30})
        setReg(ctx, reg, static_cast<uint32_t>(rng.between(-2, 2)));
}

void seededAnswer(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    logEntry(0x5eed0000u, arg(ctx, 0), 0, 0, 0);
    returnWith(ctx, draw());
}

void init3dFrame(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    logEntry(sym::Init3dFrame, arg(ctx, 0), arg(ctx, 1), arg(ctx, 2), arg(ctx, 3));
    returnWith(ctx, drawChance(80) ? 1u : 0u);
}

// AMenuProcessAll during Complete Track: the player stops it now and then.
void menuProcessBreak(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    logEntry(sym::AMenuProcessAll, arg(ctx, 0), 0, 0, 0);
    if (drawChance(env(eKeys) / 4u))
        mem<uint32_t>(G(pMenuSystemG).address + 0x68u) = 0x11000000u;
    returnWith(ctx, 0);
}

// A walked world the searches can run in: every menu cell filled, the start near the cursor.
void searchWorld(World &w)
{
    fillMenu(w);
    w.openRows();
    sTEditClass &t = w.te();
    t.trackDesc.startOrientation = static_cast<eDirection>(w.rng.below(4));
    if (w.rng.chance(60))
    {
        t.trackDesc.startXPos = static_cast<uint8_t>(t.cursor.xPos + w.rng.between(-2, 2));
        t.trackDesc.startYPos = static_cast<uint8_t>(t.cursor.yPos + w.rng.between(-2, 2));
    }
    t.resourceMemoryUsed = w.rng.chance(85) ? w.rng.below(0x19000) : 0x19000u - w.rng.below(3) * 1000u;
}

} // namespace

// TEditAdjustCursorModule: a0 a cursor module, a1-a2 a position, a3 a direction, t0 a height, t1 what to do.
void teditAdjust(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    World w(ctx, rng, scratch, true);
    w.hudBuffers();
    sTEditClass &t = w.te();
    const uint32_t args = scratch + kArgs;
    mem<int16_t>(args) = static_cast<int16_t>(rng.between(4, 11));
    mem<int16_t>(args + 2u) = static_cast<int16_t>(rng.between(4, 11));
    mem<int16_t>(args + 4u) = static_cast<int16_t>(rng.chance(60) ? rng.pick({0, 0x300, 0x600, -0x300, 0x900}) : rng.between(-0x1000, 0x1000));
    mem<uint32_t>(args + 8u) = rng.chance(92) ? rng.below(4) : rng.chance(50) ? rng.below(8) : rng.next();
    mem<uint32_t>(args + 0x20u) = rng.next();
    mem<uint32_t>(args + 0x24u) = w.anyDesc();
    t.placementErrorChecking = rng.chance(50) ? 0 : 1;
    t.resourceErrorCheckMemoryUsed = rng.chance(50) ? 0x19000u - rng.below(3) * 1000u : rng.next();
    setArg(ctx, 0, args + 0x20u);
    setArg(ctx, 1, args);
    setArg(ctx, 2, args + 2u);
    setArg(ctx, 3, args + 8u);
    setReg(ctx, 8, args + 4u);
    setReg(ctx, 9, rng.chance(90) ? rng.pick({1u, 2u, 4u, 8u, 2u, 1u}) : rng.chance(50) ? rng.pick({0u, 3u, 16u, 0x102u}) : rng.next());
}

// TEditBuildTransitionCursorModules: a0-a1 two slopes (the same one or an unknown one now and then: the id is then a
// leftover register).
void teditTransition(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    World w(ctx, rng, scratch);
    leftovers(ctx, rng);
    for (const int reg : {8, 9, 10, 11, 16, 19, 20})
        setReg(ctx, reg, rng.chance(80) ? rng.below(50) : rng.next());
    setArg(ctx, 0, static_cast<uint32_t>(rng.chance(92) ? rng.between(-2, 2) : rng.between(-4, 4)));
    setArg(ctx, 1, static_cast<uint32_t>(rng.chance(92) ? rng.between(-2, 2) : rng.between(-4, 4)));
}

// The auto-complete functions: a walked track and cursor, every menu cell filled, a0 a stack entry.
void teditAutoComp(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    World w(ctx, rng, scratch, true);
    searchWorld(w);
    leftovers(ctx, rng);
    sTEditClass &t = w.te();
    if (rng.chance(8)) // TEditGetAutoCompPieceScore: a start orientation it does not know
        t.trackDesc.startOrientation = static_cast<eDirection>(rng.between(4, 6));
    if (!t.trackDesc.pTrackModuleList)
    {
        mem<uint32_t>(scratch + kTrackNodes) = 0;
        mem<uint32_t>(scratch + kTrackNodes + 4u) = rng.below(42);
        t.trackDesc.pTrackModuleList = ptr32<_sTrackModule>(scratch + kTrackNodes);
    }
    uint32_t length = 0;
    for (uint32_t node = t.trackDesc.pTrackModuleList.address; node != 0; node = mem<uint32_t>(node))
        ++length;
    const uint32_t entry = scratch + kEntryAt;
    autoCompEntry(w, entry);
    // TEditAutoCompRemoveLastPiece takes that many modules off: all of them now and then (the cursor's slope is then
    // the register TEditClearTrack leaves).
    mem<uint8_t>(entry + 1u) = static_cast<uint8_t>(rng.chance(15) ? length : rng.below(length));
    setArg(ctx, 0, entry);
}

namespace {

void searchStandIns()
{
    standIn({{sym::Rdr_EndFrame, &logged<sym::Rdr_EndFrame, 0>},
             {sym::Rdr_StartFrame, &logged<sym::Rdr_StartFrame, 0>},
             {sym::RdrState_ApplyDefaultStateBlock, &logged<sym::RdrState_ApplyDefaultStateBlock, 0>},
             {sym::GfxP2_ZWriteEnable, &logged<sym::GfxP2_ZWriteEnable, 0>},
             {sym::Init3dFrame, &init3dFrame},
             {sym::TileFrameInit, &logged<sym::TileFrameInit, 0>},
             {sym::CellRenderInit, &cellRenderInit},
             {sym::InputSnapshot, &logged<sym::InputSnapshot, 0>},
             {sym::AMenuProcessAll, &menuProcessBreak},
             {sym::DisplayRenderFrameProfile, &logged<sym::DisplayRenderFrameProfile, 1>},
             {sym::TimerGet, &seededAnswer},
             {sym::CameraProcess, &logged<sym::CameraProcess, 2>},
             {sym::SfxDoSfx, &logged<sym::SfxDoSfx, 0>},
             {sym::LineRender3D, &logged<sym::LineRender3D, 1>}});
}

// The searches' heap, and their grid moved out of scratch into the spare RAM above the heap, with empty units all
// round it: a search can stack a special piece a fourth layer high or step off the grid, which in scratch would write
// units over the edit modules (and a module 200 units wide never ends).
void bigArena(World &w)
{
    setEnv(eArena, kBigArena);
    setEnv(eArenaLo, kBigArena);
    setEnv(eArenaHi, kBigArenaEnd);
    constexpr uint32_t kRoom = kBigArenaEnd, kRoomEnd = 0x500000u, kGrid = kRoom + 0x1000u;
    for (uint32_t unit = kRoom; unit < kRoomEnd; unit += 8u)
    {
        mem<int16_t>(unit) = -1;
        mem<int16_t>(unit + 2u) = 0;
        mem<uint32_t>(unit + 4u) = 0;
    }
    std::memcpy(guest_host(kGrid), guest_host(w.s + kUnits), 0x1800u);
    w.te().track.pTrackUnits = ptr32<_sTrackUnit>(kGrid);
}

} // namespace

// Complete Track and one random track: the frame's calls stood in (answers from the seed), the large heap.
void teditSearch(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    uint32_t skies[6];
    std::memcpy(skies, guest_host(kTE + 0x5cu), sizeof(skies));
    World w(ctx, rng, scratch, true);
    // The sky is drawn for real (TEditRenderSkyDome's tail jump): the level's, else the effects' shape, as teditDisplay.
    for (uint32_t i = 0; i < 6u; ++i)
        mem<uint32_t>(kTE + 0x5cu + i * 4u) = w.realModules ? skies[i] : mem<uint32_t>(G(visFxVarG).pVfxRes.address + 0x88u);
    searchStandIns();
    searchWorld(w);
    leftovers(ctx, rng);
    bigArena(w);
    sTEditClass &t = w.te();
    if (!t.trackDesc.pTrackModuleList)
    {
        mem<uint32_t>(scratch + kTrackNodes) = 0;
        mem<uint32_t>(scratch + kTrackNodes + 4u) = 0;
        t.trackDesc.pTrackModuleList = ptr32<_sTrackModule>(scratch + kTrackNodes);
    }
    t.pTrackAutoCompActive = ptr32<sAMenuHeader>(scratch + kMenuSys + 0xa0u);
    mem<uint32_t>(scratch + kMenuSys + 0x68u) = rng.chance(5) ? 0x11000000u : rng.below(3);
}

// A world a random track can close in (TEditMenuDoRandomTrack tries until one does): on the Track Editor dump the
// level's own modules, editModuleListG and edit menu half the time; else one-unit flat pieces (every module a straight,
// a right or a left turn by its number) with the flat row's cells left turns, a straight, right turns.
namespace {

struct Closable
{
    uint8_t menu[0x134], descs[200];
    uint32_t real;
    Closable()
    {
        std::memcpy(menu, guest_host(kTE + 0xa4u), sizeof(menu));
        std::memcpy(descs, guest_host(kDescs), sizeof(descs));
        real = mem<uint32_t>(kTE + 0x38u);
    }
    void apply(World &w) const
    {
        Rng &rng = w.rng;
        sTEditClass &t = w.te();
        const uint32_t header = w.s + kModuleHeader;
        if (w.realModules && rng.chance(50))
        {
            std::memcpy(guest_host(header), guest_host(real), 4u + kModules * 0x18u);
            std::memcpy(guest_host(kTE + 0xa4u), menu, sizeof(menu));
            std::memcpy(guest_host(kDescs), descs, sizeof(descs));
            return;
        }
        mem<uint8_t>(w.s + kPosArrays) = 1;
        for (uint32_t m = 0; m < kModules; ++m)
        {
            const uint32_t at = header + 4u + m * 0x18u;
            mem<uint16_t>(at) = 1;
            mem<uint16_t>(at + 2u) = 1;
            mem<int16_t>(at + 4u) = static_cast<int16_t>(m == 0 ? 0 : m % 3u == 0 ? 0 : m % 3u == 1 ? 1 : 3);
            mem<int16_t>(at + 6u) = 0;
            mem<int32_t>(at + 8u) = 0;
            mem<int32_t>(at + 0xcu) = 0;
            mem<uint32_t>(at + 0x10u) = w.s + kPosArrays;
        }
        std::memcpy(guest_host(kDescs), descs, sizeof(descs));
        const auto cell = [&](uint32_t turn) // an editModuleListG entry whose module turns so (0 straight, 1 right, 3 left)
        {
            for (;;)
            {
                const uint32_t d = w.desc(1u + rng.below(49));
                if (static_cast<uint32_t>(static_cast<uint16_t>(mem<int16_t>(header + 4u + mem<uint16_t>(d) * 0x18u + 4u))) == turn && mem<uint16_t>(d) != 0)
                    return d;
            }
        };
        for (int r = 0; r < 5; ++r)
            for (int j = 0; j < 7; ++j)
            {
                sEditGridSlopeInstance &g = t.editMenuGridSlopes[r];
                g.pEditModuleDesc[j] = ptr32<_sEditModuleDesc>(cell(j < 3 ? 3u : j == 3 ? 0u : 1u));
                g.avail[j] = static_cast<int8_t>(r == 2 ? 1 : 0);
            }
        for (int i = 0; i < 2; ++i)
            for (int j = 0; j < 7; ++j)
                t.editMenu.pEditModuleSpecial[i][j] = ptr32<_sEditModuleDesc>(cell(rng.pick({0u, 0u, 1u, 3u})));
    }
};

// LoadSaveProcess in the editor's menu: -1 (left), 1 (done) or still going.
void loadSaveProcess(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    logEntry(sym::LoadSaveProcess, arg(ctx, 0), 0, 0, 0);
    const uint32_t answer = drawBelow(4);
    returnWith(ctx, answer == 0 ? 0xffffffffu : answer == 1 ? 1u : answer == 2 ? 0u : 2u);
}

} // namespace

void teditRandomTrack(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    const Closable closable;
    World w(ctx, rng, scratch, true);
    searchStandIns();
    leftovers(ctx, rng);
    bigArena(w);
    closable.apply(w);
}

// TEditDisplayMainMenus: a menu state and a code the menu "chose", in a world a random track closes in and Complete
// Track can run in; the menu, the memory card screens and the help page stood in, the cars' availability random.
void teditMainMenus(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    const Closable closable;
    uint32_t skies[6];
    std::memcpy(skies, guest_host(kTE + 0x5cu), sizeof(skies));
    World w(ctx, rng, scratch, true);
    for (uint32_t i = 0; i < 6u; ++i)
        mem<uint32_t>(kTE + 0x5cu + i * 4u) = w.realModules ? skies[i] : mem<uint32_t>(G(visFxVarG).pVfxRes.address + 0x88u);
    searchStandIns();
    standIn({{sym::LoadSaveProcess, &loadSaveProcess},
             {sym::LoadSaveInit, &logged<sym::LoadSaveInit, 1>},
             {sym::MenuItemDisabler, &logged<sym::MenuItemDisabler, 1>},
             {sym::BuildTrackEditorHelp, &logged<sym::BuildTrackEditorHelp, 1>},
             {sym::InputResetAllHit, &logged<sym::InputResetAllHit, 0>},
             {sym::AMenuProcessAll, &logged<sym::AMenuProcessAll, 1>}});
    leftovers(ctx, rng);
    bigArena(w);
    closable.apply(w);
    sTEditClass &t = w.te();
    if (!t.trackDesc.pTrackModuleList)
    {
        mem<uint32_t>(scratch + kTrackNodes) = 0;
        mem<uint32_t>(scratch + kTrackNodes + 4u) = 0;
        t.trackDesc.pTrackModuleList = ptr32<_sTrackModule>(scratch + kTrackNodes);
    }
    // The menus: headers of their own for the active menu and the second player's car menu, the others a word each.
    const uint32_t sys = scratch + kMenuSys, menus = scratch + kArgs;
    const auto menu = [&](uint32_t i) { return ptr32<sAMenuHeader>(menus + 0x100u + i * 8u); };
    t.pTrackMainMenu = ptr32<sAMenuHeader>(sys + 0x80u);
    t.pTrackPlayer2CarSelectMenu = ptr32<sAMenuHeader>(menus);
    t.pTrackUnsaved = menu(0);
    t.pTrackIncomplete = menu(1);
    t.pTrackComplete = menu(2);
    t.pTrackOptionsMenu = menu(3);
    t.pTrackWarnings[3] = menu(4);
    for (uint32_t i = 0; i < 4u; ++i)
        t.pTrackAutoCompMsg[i] = menu(5u + i);
    t.pTrackUnPlayable = menu(9);
    t.pTrackAutoCompActive = menu(10);
    mem<uint32_t>(sys) = rng.chance(70) ? sys + 0x80u : rng.chance(50) ? menus : menus + 0x80u;
    mem<int8_t>(mem<uint32_t>(sys) + 0x63u) = static_cast<int8_t>(rng.chance(85) ? rng.below(12) : rng.between(-2, 25));
    static constexpr uint32_t kCodes[] = {0x1000022cu, 0x10000227u, 0x1000007du, 0x1000007cu, 0x10000225u, 0x10000226u, 0x10000229u,
                                          0x10000228u, 0x1000022au, 0x1000022bu, 0x7fff1002u, 0x1000022du, 0x1000022eu, 0x1000022fu,
                                          0x11000000u, 0x7fff2000u, 0x7fff2001u, 0x7fff3000u, 0x7fff4000u, 0x7fff5000u, 0x11000001u, 0u};
    mem<uint32_t>(sys + 0x68u) = rng.chance(92) ? rng.pick(kCodes) : rng.chance(50) ? rng.pick(kCodes) + rng.pick({1u, 0xffffffffu}) : rng.next();
    t.trackState.menuCurrentState = static_cast<uint16_t>(rng.chance(45) ? 0u : rng.chance(92) ? rng.between(1, 8) : rng.below(12));
    t.trackState.menuNextState = static_cast<uint16_t>(rng.pick({1u, 2u, 4u, 5u, 2u, 0u}));
    t.trackState.editState = static_cast<uint16_t>(rng.chance(60) ? 2u : rng.below(5));
    t.loadSaveFlags = rng.below(8);
    G(oldThemeTypeNdxG) = rng.chance(70) ? t.trackDesc.themeTypeNdx : rng.below(6);
    G(oldSoundTrackNdxG) = rng.chance(70) ? t.trackDesc.soundTrackNdx : rng.below(8);
    // The cars: the secret ones unlocked or not, the players' cars any of the 19 (or none of them).
    for (const uint32_t reward : {0x3bcu, 0x3bdu, 0x3beu, 0x3bfu, 0x3c0u, 0x3c1u, 0x3dbu, 0x3dcu, 0x3ddu, 0x3deu})
        mem<uint8_t>(sym::mainVarG + reward) = static_cast<uint8_t>(rng.chance(50) ? 0 : rng.chance(80) ? 1 : rng.next());
    mem<uint8_t>(sym::mainVarG + 0x3dfu) = static_cast<uint8_t>(rng.chance(90) ? rng.between(1, 19) : rng.below(30));
    mem<uint8_t>(sym::mainVarG + 0x3e0u) = static_cast<uint8_t>(rng.chance(90) ? rng.between(1, 19) : rng.below(30));
    for (const uint32_t config : {0x188u, 0x18cu, 0x190u, 0x194u})
        mem<uint32_t>(sym::frontEndVarG + config) = rng.chance(80) ? rng.below(9) : rng.next();
    // GetCarText: the Track Editor's names (no level's): 20 strings' addresses in scratch.
    G(pLvlG).address = scratch + kLevel;
    mem<uint32_t>(scratch + kLevel + 0x58u) = scratch + kLevel + 0x80u;
    for (uint32_t i = 0; i < 20u; ++i)
        mem<uint32_t>(scratch + kLevel + 0x80u + i * 4u) = scratch + kSaved + rng.below(0x100);
    mem<uint32_t>(scratch + kLevel + 0x4cu) = mem<uint32_t>(kTE + 0x5cu);
    setArg(ctx, 0, rng.next());
    setArg(ctx, 1, rng.next());
}

// The edit menu's keys: a0 the pad's block (the editor's own or one in scratch) with a response of the six keys; a
// walked track and cursor, every menu cell filled.
void teditKeys(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    World w(ctx, rng, scratch, true);
    fillMenu(w);
    w.openRows();
    leftovers(ctx, rng);
    sTEditClass &t = w.te();
    if (!t.trackDesc.pTrackModuleList)
    {
        mem<uint32_t>(scratch + kTrackNodes) = 0;
        mem<uint32_t>(scratch + kTrackNodes + 4u) = rng.chance(50) ? 0u : rng.below(50);
        t.trackDesc.pTrackModuleList = ptr32<_sTrackModule>(scratch + kTrackNodes);
    }
    // The last module decides where the menu goes after a delete: any id now and then.
    if (rng.chance(40))
    {
        uint32_t last = t.trackDesc.pTrackModuleList.address;
        while (mem<uint32_t>(last) != 0)
            last = mem<uint32_t>(last);
        mem<uint32_t>(last + 4u) = rng.below(50);
    }
    t.trackIsComplete = rng.chance(80) ? 0 : 1;
    t.trackDesc.startOrientation = static_cast<eDirection>(rng.below(4));
    t.pTrackComplete = ptr32<sAMenuHeader>(scratch + kMenuSys + 0xb0u);
    const uint32_t input = rng.chance(50) ? kTE + 0x7cu : scratch + kArgs;
    mem<uint32_t>(input) = rng.chance(90) ? rng.pick({0u, 1u, 2u, 4u, 8u, 0x10u, 0x20u, 0x10u, 0x20u, 0x11u, 0x30u, 0x24u}) : rng.next();
    setArg(ctx, 0, input);
    setArg(ctx, 1, rng.next());
}

// The start grid's keys: a0 the pad's block, the cursor often on or beside the grid's edges.
void teditStartGrid(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    World w(ctx, rng, scratch);
    fillMenu(w);
    w.openRows();
    sTEditClass &t = w.te();
    const auto place = [&] { return static_cast<int16_t>(rng.chance(70) ? rng.pick({0, 1, 14, 15, 0, 15}) : rng.chance(90) ? rng.below(16) : rng.between(-2, 18)); };
    t.cursor.xPos = place();
    t.cursor.yPos = place();
    t.cursor.direction = static_cast<eDirection>(rng.chance(92) ? rng.below(4) : rng.below(8));
    t.placementViewDir = static_cast<eDirection>(rng.chance(92) ? rng.below(4) : rng.below(8));
    const uint32_t input = rng.chance(50) ? kTE + 0x7cu : scratch + kArgs;
    mem<uint32_t>(input) = rng.chance(90) ? rng.below(0x40) : rng.next();
    setArg(ctx, 0, input);
    setArg(ctx, 1, rng.next());
}

// a0 a time: the edit menu's pulse (1.2 s), its turning points and negative times.
void teditMenuTime(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    World w(ctx, rng, scratch);
    setArg(ctx, 0, static_cast<uint32_t>(rng.chance(40) ? static_cast<int32_t>(rng.below(100)) * 600 + rng.between(-2, 2)
                                         : rng.chance(80) ? rng.between(0, 200000) : static_cast<int32_t>(rng.next())));
}

// --- Part 2: the track as a race level. ---

namespace {

constexpr uint32_t kAISets = kBigArena + 0x10000u; // the AI node sets (pAINode): a word, then 32 sets of 0x288 bytes

// A level in scratch: eight start positions, a hud resource whose first map sprite leads to a texture handle, a sky.
void levelData(World &w, uint32_t sky)
{
    Rng &rng = w.rng;
    const uint32_t level = w.s + kLevel;
    for (uint32_t i = 0; i < 0x100u; i += 4u)
        mem<uint32_t>(level + i) = rng.next();
    mem<uint32_t>(level + 0x30u) = level + 0x80u; // pStartData: startPos[8]
    for (uint32_t i = 0; i < 0x40u; i += 2u)
        mem<int16_t>(level + 0x80u + i) = static_cast<int16_t>(rng.chance(70) ? rng.between(-0x400, 0x400) : rng.between(-0x8000, 0x7fff));
    mem<uint32_t>(level + 0x40u) = level + 0xc0u;  // pHudRes
    mem<uint32_t>(level + 0xc8u) = level + 0xd0u;  // pMapSpr[0]
    mem<uint32_t>(level + 0xd0u) = w.s + kArgs + 0x100u; // its texture handle (idxList at 0x2c, pBitmapList at 0x48)
    mem<uint32_t>(level + 0x4cu) = sky;
    G(pLvlG).address = level;
}

} // namespace

// The track-to-level functions: a walked track, the AI node sets (in the spare RAM the searches' heap uses: read only),
// a level with its start positions, the pickups' positions, a ring of AI nodes; a0 a 0x80-byte bitmap.
void teditLevel(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    World w(ctx, rng, scratch, true);
    sTEditClass &t = w.te();
    leftovers(ctx, rng);
    setReg(ctx, 8, static_cast<uint32_t>(rng.between(-2, 2))); // TEditConvertTrackToALF's slope with no modules
    for (uint32_t i = 0; i < 4u + 32u * 0x288u; i += 4u)
        mem<uint32_t>(kAISets + i) = rng.chance(60) ? rng.next() & 0x0fff0fffu : rng.next();
    t.pAINode = ptr32<uint32>(kAISets);
    levelData(w, 0);
    t.trackDesc.startOrientation = static_cast<eDirection>(rng.chance(92) ? rng.below(4) : rng.below(8));
    // The AI nodes TEditUpdatePickupNumAndPositions reads: a stretch of 15 a pair.
    const uint32_t nodes = scratch + kAINodes;
    for (uint32_t i = 0; i < 24u * 0x6cu; i += 4u)
        mem<uint32_t>(nodes + i) = rng.next();
    sAINodeClass &ai = G(aiNodeVarG);
    ai.pNode = ptr32<_sAINode>(nodes);
    ai.noNodesSingle = static_cast<int16_t>(rng.chance(90) ? rng.between(0, 65) : rng.between(-40, 0));
    mem<uint32_t>(sym::pickupVarG + 0x48u) = rng.chance(90) ? rng.below(12) : rng.below(40); // a pair a stretch of 15 nodes
    mem<uint32_t>(sym::pickupVarG + 0x44u) = scratch + kArgs + 0x40u;
    // The grid for the icon: a few start grid tiles, some stacked.
    if (rng.chance(50))
        for (uint32_t i = rng.below(12); i-- > 0;)
            mem<int16_t>(scratch + kUnits + rng.below(0x300) * 8u) = static_cast<int16_t>(rng.chance(60) ? 0x43 : w.tileId());
    setArg(ctx, 0, scratch + kArgs + 0x40u);
}

// TEditConstructAINode: a0 a set, a1 five nodes' room, a2 the last node's handle, a3 an offset, t0-t1 mirrored and
// run backwards, t2 a direction (another now and then: the turn's start is then what the stack held).
void teditAINodeSet(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    World w(ctx, rng, scratch);
    for (uint32_t i = 0; i < 4u + 32u * 0x288u; i += 4u)
        mem<uint32_t>(kAISets + i) = rng.chance(60) ? rng.next() & 0x0fff0fffu : rng.next();
    w.te().pAINode = ptr32<uint32>(kAISets);
    const uint32_t args = scratch + kArgs;
    for (uint32_t i = 0; i < 8u; i += 2u)
        mem<int16_t>(args + i) = static_cast<int16_t>(rng.chance(70) ? rng.between(-8, 8) * 0x800 : rng.between(-0x8000, 0x7fff));
    mem<uint32_t>(args + 0x20u) = rng.chance(30) ? 0u : scratch + kAINodes + rng.below(24) * 0x6cu;
    setArg(ctx, 0, rng.chance(90) ? rng.below(32) : 0x10000u + rng.below(32));
    setArg(ctx, 1, scratch + kArena + rng.below(8) * 4u);
    setArg(ctx, 2, args + 0x20u);
    setArg(ctx, 3, args);
    setReg(ctx, 8, rng.chance(80) ? rng.below(2) : rng.pick({0x10000u, 0x10001u, 2u, 0xffffu}));
    setReg(ctx, 9, rng.chance(80) ? rng.below(2) : rng.pick({0x10000u, 0x10001u, 2u, 0xffffu}));
    setReg(ctx, 10, rng.chance(92) ? rng.below(4) : rng.below(8));
}

// TEditRenderGameTrack: the grid's tiles (TileRenderGameROB stood in) and the level's sky, drawn for real.
void teditGameTrack(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    const uint32_t realSky = mem<uint32_t>(kTE + 0x5cu);
    World w(ctx, rng, scratch);
    levelData(w, w.realModules ? realSky : mem<uint32_t>(G(visFxVarG).pVfxRes.address + 0x88u));
}

// The hud map: the tile pictures' sprite (a 0x200-byte bitmap), the buffers TEditRenderTileToHudMap reads and draws
// on, a level whose map sprite names a texture; a0-a1 a place on the map, a2-a3 a picture's place, t0 the flags.
void teditHudMap(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    World w(ctx, rng, scratch, true);
    sTEditClass &t = w.te();
    w.hudBuffers();
    levelData(w, 0);
    const uint32_t sprite = scratch + kArgs + 0x80u, handle = scratch + kArgs + 0x100u;
    mem<uint32_t>(sprite) = handle;
    mem<uint32_t>(handle + 0x48u) = scratch + kSaveGame; // the bitmap: 0x200 bytes of the save buffer's random words
    mem<int16_t>(handle + 0x2cu) = static_cast<int16_t>(rng.next());
    t.pTrackMapBits = ptr32<_sSprite>(sprite);
    t.pHudMap = ptr32<int32>(rng.chance(80) ? scratch + kArena : 0u);
    setArg(ctx, 0, static_cast<uint32_t>(rng.chance(90) ? rng.between(0, 60) : rng.between(-16, 80)));
    setArg(ctx, 1, static_cast<uint32_t>(rng.chance(90) ? rng.between(0, 60) : rng.between(-16, 76)));
    setArg(ctx, 2, static_cast<uint32_t>(rng.chance(90) ? rng.between(0, 28) : rng.between(0, 31)));
    setArg(ctx, 3, static_cast<uint32_t>(rng.chance(90) ? rng.between(0, 28) : rng.between(0, 100)));
    setReg(ctx, 8, rng.chance(90) ? (rng.pick({0u, 2u, 4u, 8u}) | (rng.chance(40) ? 0x20u : 0u)) : rng.next());
}

// TEditGenerateHudMapForTrackLevel: a walked track and the tile pictures' sprite; its buffers come from the heap.
void teditHudMapBuild(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    World w(ctx, rng, scratch, true);
    sTEditClass &t = w.te();
    const uint32_t sprite = scratch + kArgs + 0x80u, handle = scratch + kArgs + 0x100u;
    mem<uint32_t>(sprite) = handle;
    mem<uint32_t>(handle + 0x48u) = scratch + kSaveGame;
    t.pTrackMapBits = ptr32<_sSprite>(sprite);
    t.pHudMap = ptr32<int32>(rng.next());
    t.pHudMapBitsBPP = ptr32<int32>(rng.next());
    t.pHudMapBPP = ptr32<int32>(rng.next());
}

} // namespace rc::fuzz
