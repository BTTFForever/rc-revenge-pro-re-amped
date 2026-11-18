// Builds the random-test inputs of CARD.c and LOADSAVE.c. See card_fuzz.h.

#include "card_fuzz.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>

namespace rc {
uint32 ResBinaryChecksum(uint32 checkSum, ptr32<uint32> pCs, uint32 len); // res.cpp
} // namespace rc

namespace rc::fuzz {

namespace {

// Scratch layout.
constexpr uint32_t kMsgs = 0x0000u;      // 45 menu headers, 0x80 bytes each
constexpr uint32_t kMenuSys = 0x1700u;   // an sMenuSystem (menuInputString at 0xc4)
constexpr uint32_t kStrings = 0x1800u;   // 16 strings of 0x20 bytes
constexpr uint32_t kVectors = 0x1a00u;   // an icon header's inputs: colours 0x40, lights 0x30 + 0x30, ambient 0x10
constexpr uint32_t kIconNames = 0x1ac0u; // three name pointers
constexpr uint32_t kTitle = 0x1ad0u;     // 0x40
constexpr uint32_t kIconOut = 0x1b20u;   // an icon.sys (0x3c4)
constexpr uint32_t kSprites = 0x1f00u;   // two sprite headers
constexpr uint32_t kLsData = 0x1f80u;    // an sLoadSaveData (0xd4)
constexpr uint32_t kResults = 0x2060u;   // result and exists words
constexpr uint32_t kArena = 0x2080u, kArenaEnd = 0x7800u; // the heap stand-ins' blocks
constexpr uint32_t kEnv = kFuzzScratch + 0x7800u, kLog = kFuzzScratch + 0x7900u, kLogMax = 44u;
enum : uint32_t
{
    eSeed = 0x0,
    eCalls = 0x4,
    eHash = 0x8,
    ePending = 0xc,    // a command is running
    ePendCmd = 0x10,
    ePendResult = 0x14,
    eArena = 0x18,     // the next free block
    eBusy = 0x1c,      // percent of mode-1 syncs that answer "running"
    eFail = 0x20,      // percent of commands that fail to start
    eKeys = 0x24,      // percent of InputHit calls that answer true
    eValidSave = 0x28, // percent of 0x2000-byte reads that give a good save
};

constexpr uint32_t kBack = 0x11000000u, kYes = 0x7fff0000u, kNo = 0x7fff0001u;
constexpr uint32_t kCardVars = sym::cardVarG, kLoadSave = sym::loadSaveVarG;

template <typename T>
T &mem(uint32_t address) { return *static_cast<T *>(guest_host(address)); }
char *cstr(uint32_t address) { return static_cast<char *>(guest_host(address)); }

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

bool inRam(uint32_t address) { return address >= 0x10000u && address < 0x2000000u; } // the stack is below 0x100000

uint32_t hashBytes(uint32_t at, uint32_t size)
{
    uint32_t hash = 2166136261u;
    if (!inRam(at))
        return at;
    for (uint32_t i = 0; i < size; ++i)
        hash = (hash ^ mem<uint8_t>(at + i)) * 16777619u;
    return hash;
}

uint32_t hashString(uint32_t at)
{
    if (!inRam(at))
        return at;
    return hashBytes(at, static_cast<uint32_t>(strnlen(cstr(at), 0x40)));
}

void logEntry(uint32_t what, uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
    static ::FILE *trace = std::getenv("RC_CARDFUZZ_TRACE") ? std::fopen(std::getenv("RC_CARDFUZZ_TRACE"), "a") : nullptr;
    if (trace)
    {
        std::fprintf(trace, "[card_fuzz %d] %x %x %x %x %x\n", static_cast<int>(getpid()), what, a, b, c, d);
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

// --- libmc. ---

// A command starts (0) or fails to start; a started one's result waits for sceMcSync.
uint32_t start(uint32_t cmd, int32_t result)
{
    if (drawChance(env(eFail)))
        return drawChance(50) ? 0xffffffffu : 0xfffffff6u;
    setEnv(ePending, 1);
    setEnv(ePendCmd, cmd);
    setEnv(ePendResult, static_cast<uint32_t>(result));
    return 0;
}

// A result for a command that returns 0 on success: mostly 0, else an error.
int32_t okOrError()
{
    static const int32_t kErrors[] = {-1, -2, -4, -5, -7, 1};
    return drawChance(75) ? 0 : kErrors[drawBelow(6)];
}

void mcGetInfo(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    logEntry(sym::sceMcGetInfo, arg(ctx, 0), arg(ctx, 1), arg(ctx, 2), arg(ctx, 3) ^ arg(ctx, 4) << 8);
    static const int32_t kTypes[] = {2, 2, 2, 2, 2, 2, 1, 3, 0, 5};
    static const int32_t kFree[] = {0, 0x4a, 0x4b, 0xcd, 0xce, 0x2000, 0x1f00, 0x10};
    static const int32_t kResults[] = {0, 0, 0, 0, 0, -1, -2, -10, 1};
    const uint32_t answer = start(1, kResults[drawBelow(9)]);
    if (answer == 0)
    {
        const int32_t values[] = {kTypes[drawBelow(10)], drawChance(80) ? kFree[drawBelow(8)] : static_cast<int32_t>(draw() & 0x3fff),
                                  drawChance(75) ? 1 : drawChance(70) ? 0 : 2};
        for (int i = 0; i < 3; ++i)
            if (inRam(arg(ctx, 2 + i)))
                mem<int32_t>(arg(ctx, 2 + i)) = values[i];
    }
    returnWith(ctx, answer);
}

void mcSync(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    const uint32_t mode = arg(ctx, 0), cmdPtr = arg(ctx, 1), resultPtr = arg(ctx, 2);
    logEntry(sym::sceMcSync, mode, cmdPtr, resultPtr, env(ePending) ? env(ePendResult) : 0xdeadu);
    if (env(ePending) == 0)
    {
        returnWith(ctx, 0xffffffffu);
        return;
    }
    if (mode != 0 && drawChance(env(eBusy)))
    {
        returnWith(ctx, 0);
        return;
    }
    setEnv(ePending, 0);
    if (cmdPtr != 0 && inRam(cmdPtr))
        mem<uint32_t>(cmdPtr) = env(ePendCmd);
    if (resultPtr != 0 && inRam(resultPtr))
        mem<uint32_t>(resultPtr) = env(ePendResult);
    returnWith(ctx, 1);
}

void mcOpen(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    logEntry(sym::sceMcOpen, arg(ctx, 0), arg(ctx, 1), hashString(arg(ctx, 2)), arg(ctx, 3));
    static const int32_t kResults[] = {0, 1, 2, 3, 5, -4, -5, -7, -2};
    returnWith(ctx, start(2, drawChance(70) ? kResults[drawBelow(5)] : kResults[5 + drawBelow(4)]));
}

// A close starts unless the descriptor is not one libmc handed out. (The game ignores whether its closes of a file
// it has just opened started; a close that did not would leave the next sceMcSync answering -1 without writing its
// result, and the caller reading a stale stack word.)
void mcClose(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    const uint32_t fd = arg(ctx, 0);
    logEntry(sym::sceMcClose, fd, 0, 0, 0);
    if (fd > 15u)
    {
        returnWith(ctx, 0xffffffffu);
        return;
    }
    setEnv(ePending, 1);
    setEnv(ePendCmd, 3);
    setEnv(ePendResult, static_cast<uint32_t>(okOrError()));
    returnWith(ctx, 0);
}

// A count a read or write answers: all of it mostly, else short or an error.
int32_t count(uint32_t size)
{
    const uint32_t kind = drawBelow(10);
    if (kind < 7)
        return static_cast<int32_t>(size);
    if (kind < 9)
        return static_cast<int32_t>(size - 1u - drawBelow(size ? size : 1u));
    return -4;
}

void mcRead(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    const uint32_t fd = arg(ctx, 0), buffer = arg(ctx, 1), size = arg(ctx, 2);
    logEntry(sym::sceMcRead, fd, buffer, size, 0);
    const uint32_t answer = start(5, count(size));
    if (answer == 0 && inRam(buffer))
    {
        // A whole save is a good one now and then: random words with their checksum.
        const uint32_t bytes = size == 0x2000u ? 0x2000u : (size < 0x400u ? size : 0x400u);
        const uint32_t seed = draw();
        for (uint32_t i = 0; i + 4u <= bytes; i += 4u)
            mem<uint32_t>(buffer + i) = mix(seed + i);
        if (size == 0x2000u && drawChance(env(eValidSave)))
            mem<uint32_t>(buffer + 0x200u) = ResBinaryChecksum(0, ptr32<uint32>(buffer + 0x204u), 0x130u);
    }
    returnWith(ctx, answer);
}

void mcWrite(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    const uint32_t size = arg(ctx, 2);
    logEntry(sym::sceMcWrite, arg(ctx, 0), arg(ctx, 1), size, hashBytes(arg(ctx, 1), size < 0x2000u ? size : 0x2000u));
    returnWith(ctx, start(6, count(size)));
}

// Mkdir, Chdir (path, pwd), Delete, Rename (old, new), Format, Unformat: port, slot, then Names names (a pwd may be 0).
template <uint32_t Address, uint32_t Cmd, int Names>
void mcNamed(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    logEntry(Address, arg(ctx, 0), arg(ctx, 1), Names > 0 ? hashString(arg(ctx, 2)) : 0u, Names > 1 ? hashString(arg(ctx, 3)) : 0u);
    returnWith(ctx, start(Cmd, okOrError()));
}

// Track and game save names as the card would list them, from a seed.
void entryName(uint32_t at, uint32_t seed)
{
    uint32_t k = 0;
    const auto drawBelow = [&](uint32_t n) { return n ? mix(seed + 0x9e3779b9u * ++k) % n : 0u; };
    char name[32] = {};
    const char *gameId = cstr(sym::revolt2GameId), *trackId = cstr(sym::revolt2TrackId), *productId = cstr(sym::revolt2ProductId);
    const uint32_t kind = drawBelow(10);
    if (kind < 3)
        std::snprintf(name, sizeof(name), "%s", gameId);
    else if (kind < 6)
        std::snprintf(name, sizeof(name), "%sTrack%u", trackId, 1u + drawBelow(16));
    else if (kind < 8)
    {
        static const char kChars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 ";
        std::snprintf(name, sizeof(name), "%s", trackId);
        const size_t n = std::strlen(name), extra = 1u + drawBelow(8);
        for (size_t i = 0; i < extra && n + i < 31u; ++i)
            name[n + i] = kChars[drawBelow(37)];
    }
    else if (kind < 9)
        std::snprintf(name, sizeof(name), "%.12sX", productId);
    else
        std::snprintf(name, sizeof(name), "BESCES-%05u", drawBelow(100000));
    std::memcpy(guest_host(at), name, sizeof(name));
}

void mcGetDir(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    const uint32_t maxEnt = arg(ctx, 4), table = arg(ctx, 5);
    logEntry(sym::sceMcGetDir, arg(ctx, 0), hashString(arg(ctx, 2)), arg(ctx, 3), maxEnt ^ table << 8);
    const uint32_t n = drawBelow(maxEnt < 12u ? maxEnt + 1u : 13u);
    const uint32_t answer = start(0xd, drawChance(85) ? static_cast<int32_t>(n) : -2);
    if (answer == 0 && inRam(table))
        for (uint32_t i = 0; i < n; ++i)
        {
            for (uint32_t w = 0; w < 0x20u; w += 4u)
                mem<uint32_t>(table + i * 0x40u + w) = draw();
            entryName(table + i * 0x40u + 0x20u, draw());
        }
    returnWith(ctx, answer);
}

void mcInit(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    logEntry(sym::sceMcInit, 0, 0, 0, 0);
    static const int32_t kResults[] = {0, 0, -0x78, -0x79, -0x65, -1, -0x7a, -0x80};
    returnWith(ctx, static_cast<uint32_t>(kResults[drawBelow(8)]));
}

// --- The game's other outside calls. ---

template <uint32_t Address, int Args>
void logged(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    const auto a = [ctx](int i) { return i < Args ? arg(ctx, i) : 0u; };
    logEntry(Address, a(0), a(1), a(2), a(3) ^ a(4) << 8 ^ a(5) << 16 ^ a(6) << 24);
    returnWith(ctx, 0);
}

void inputHit(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    logEntry(sym::InputHit, arg(ctx, 0), 0, 0, 0);
    returnWith(ctx, drawChance(env(eKeys)) ? 1u : 0u);
}

void extractTrack(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    logEntry(sym::TEditExtractTrackDescFromSaveBuffer, 0, 0, 0, 0);
    returnWith(ctx, drawChance(70) ? 1u : 0u);
}

void generalString(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    logEntry(sym::GetGeneralString, arg(ctx, 0), 0, 0, 0);
    returnWith(ctx, kFuzzScratch + kStrings + 0x1e0u);
}

uint32_t floatBits(R5900Context *ctx)
{
    uint32_t bits;
    std::memcpy(&bits, &ctx->f[12], 4);
    return bits;
}

// The parameters as the renderers read them: RectRender reads a rectangle's fix byte (0x17) only with flag 0x10 and
// SpriteRenderScaled never reads code (0x13), which RenderMemoryCards leaves as the stack held them.
uint32_t hashSkipping(uint32_t at, uint32_t size, uint32_t skip)
{
    return hashBytes(at, skip) ^ hashBytes(at + skip + 1u, size - skip - 1u) * 31u;
}

void rectRender(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    const uint32_t rect = arg(ctx, 0), flags = inRam(rect) ? mem<uint32_t>(rect) : 0u;
    logEntry(sym::RectRender, rect, (flags & 0x10u) ? hashBytes(rect, 0x18u) : hashSkipping(rect, 0x18u, 0x17u), floatBits(ctx), 0);
    returnWith(ctx, 0);
}

void spriteRender(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    logEntry(sym::SpriteRenderScaled, arg(ctx, 0), arg(ctx, 1), hashSkipping(arg(ctx, 1), 0x18u, 0x13u), floatBits(ctx));
    returnWith(ctx, 0);
}

// The heap: blocks from the scratch arena (a request larger than what is left gets the rest, at least 0x100 bytes
// that nothing reads beyond).
void memAllocLo(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    const uint32_t handle = arg(ctx, 0), size = arg(ctx, 1);
    uint32_t at = env(eArena);
    if (at + 0x100u > kFuzzScratch + kArenaEnd)
        at = kFuzzScratch + kArena;
    const uint32_t room = kFuzzScratch + kArenaEnd - at, take = ((size < room ? size : room) + 15u) & ~15u;
    setEnv(eArena, at + take);
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

void memSet32(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    const uint32_t block = arg(ctx, 0), value = arg(ctx, 1), size = arg(ctx, 2);
    logEntry(sym::MemBlockSet32, block, value, size, 0);
    if (block >= kFuzzScratch + kArena && block < kFuzzScratch + kArenaEnd)
        for (uint32_t i = 0; i + 4u <= size && block + i + 4u <= kFuzzScratch + kArenaEnd; i += 4u)
            mem<uint32_t>(block + i) = value;
    returnWith(ctx, 0);
}

struct Stub
{
    uint32_t address;
    RecompFn fn;
};

// Installs the fakes once per test process. Not replaced: MemBlockCopy32 and strcpy (tail jumps in the originals
// reach them past the function table), sprintf, ResBinaryChecksum, ASCIIStringToJIS, AMenuAddTextToTGRP (all
// real, on scratch and the globals).
void install()
{
    static bool installed = false;
    if (!g_offline || installed)
        return;
    installed = true;
    const Stub stubs[] = {
        {sym::sceMcGetInfo, &mcGetInfo},
        {sym::sceMcSync, &mcSync},
        {sym::sceMcOpen, &mcOpen},
        {sym::sceMcClose, &mcClose},
        {sym::sceMcRead, &mcRead},
        {sym::sceMcWrite, &mcWrite},
        {sym::sceMcMkdir, &mcNamed<sym::sceMcMkdir, 0xb, 1>},
        {sym::sceMcChdir, &mcNamed<sym::sceMcChdir, 0xc, 2>},
        {sym::sceMcDelete, &mcNamed<sym::sceMcDelete, 0xf, 1>},
        {sym::sceMcRename, &mcNamed<sym::sceMcRename, 0x13, 2>},
        {sym::sceMcFormat, &mcNamed<sym::sceMcFormat, 0x10, 0>},
        {sym::sceMcUnformat, &mcNamed<sym::sceMcUnformat, 0x11, 0>},
        {sym::sceMcGetDir, &mcGetDir},
        {sym::sceMcInit, &mcInit},
        {sym::sceGsSyncV, &logged<sym::sceGsSyncV, 1>},
        {sym::printf, &logged<sym::printf, 1>},
        {sym::InputHit, &inputHit},
        {sym::InputResetHit, &logged<sym::InputResetHit, 1>},
        {sym::InputResetAllHit, &logged<sym::InputResetAllHit, 0>},
        {sym::SfxPlay, &logged<sym::SfxPlay, 7>},
        {sym::SfxSetVolume, &logged<sym::SfxSetVolume, 1>},
        {sym::RCMusicModifyVolume, &logged<sym::RCMusicModifyVolume, 2>},
        {sym::SetCupAvailability, &logged<sym::SetCupAvailability, 1>},
        {sym::AMenuProcessAll, &logged<sym::AMenuProcessAll, 1>},
        {sym::BuildProgress, &logged<sym::BuildProgress, 3>},
        {sym::TEditInsertTrackDescToSaveBuffer, &logged<sym::TEditInsertTrackDescToSaveBuffer, 0>},
        {sym::TEditExtractTrackDescFromSaveBuffer, &extractTrack},
        {sym::GetGeneralString, &generalString},
        {sym::RectRender, &rectRender},
        {sym::SpriteRenderScaled, &spriteRender},
        {sym::MemBlockAllocLo, &memAllocLo},
        {sym::MemBlockFree, &memFree},
        {sym::MemBlockSet32, &memSet32},
    };
    for (const Stub &s : stubs)
        g_runtime->replaceFunction(s.address, s.fn);
}

// --- The world of one input. ---

class World
{
public:
    World(R5900Context *ctx, Rng &rng, uint32_t scratch) : ctx(ctx), rng(rng), s(scratch)
    {
        install();
        logEntry(0xffffffffu, 0, 0, 0, 0);
        for (uint32_t offset = 0; offset < 0x40u; offset += 4u)
            setEnv(offset, 0);
        mem<uint32_t>(kLog) = 0;
        setEnv(eSeed, rng.next());
        setEnv(eArena, s + kArena);
        setEnv(eBusy, rng.pick({0u, 20u, 40u, 70u}));
        setEnv(eFail, rng.pick({0u, 3u, 10u, 25u}));
        setEnv(eKeys, rng.pick({5u, 20u, 50u, 80u}));
        setEnv(eValidSave, rng.pick({0u, 50u, 90u}));
        if (rng.chance(30)) // a command already running
        {
            setEnv(ePending, 1);
            setEnv(ePendCmd, rng.below(0x14));
            setEnv(ePendResult, rng.pick({0u, 1u, 0x2000u, 0xffffffffu, 0xfffffffeu, 0xfffffffcu}));
        }
        std::memset(guest_host(0), 0, 0x100);
        // Stale stack words show through results libmc did not write.
        const uint32_t sp = getRegU32(ctx, 29);
        for (uint32_t offset = 0x800; offset >= 4; offset -= 4)
            mem<uint32_t>(sp - offset) = rng.chance(50) ? rng.pick({0u, 1u, 2u, 0xffffffffu, 0x2000u}) : rng.next();
        for (uint32_t i = 0; i < 45u; ++i)
        {
            const uint32_t h = s + kMsgs + i * 0x80u;
            for (uint32_t w = 0; w < 0x80u; w += 4u)
                mem<uint32_t>(h + w) = rng.next();
        }
        for (uint32_t i = 0; i < 16u; ++i)
            randomName(s + kStrings + i * 0x20u, 0x1f);
        cards();
        loadSave();
        menuSystem();
        frontEnd();
    }

    R5900Context *ctx;
    Rng &rng;
    const uint32_t s;

    uint32_t string(int i) const { return s + kStrings + static_cast<uint32_t>(i) * 0x20u; }

    void randomName(uint32_t at, uint32_t max)
    {
        static const char kChars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789   ";
        const uint32_t n = rng.below(max + 1u);
        for (uint32_t i = 0; i < n; ++i)
            mem<char>(at + i) = kChars[rng.below(39)];
        mem<char>(at + n) = 0;
    }

    // A name for the save being overwritten, an existing entry or a "Track<n>".
    void trackId(uint32_t at, uint32_t room)
    {
        char name[40];
        const uint32_t kind = rng.below(4);
        if (kind == 0)
            std::snprintf(name, sizeof(name), "%sTrack%u", cstr(sym::revolt2TrackId), 1u + rng.below(16));
        else if (kind == 1)
        {
            const int32_t card = rng.below(2), n = mem<int32_t>(dir(card) + 0x1b00u);
            std::snprintf(name, sizeof(name), "%.31s", n > 0 ? cstr(dir(card) + 0x260u + rng.below(static_cast<uint32_t>(n < 99 ? n : 99)) * 0x40u) : "");
        }
        else if (kind == 2)
            std::snprintf(name, sizeof(name), "%sTRACK%u", cstr(sym::revolt2TrackId), rng.below(4));
        else
            std::snprintf(name, sizeof(name), "%s%c%c", cstr(sym::revolt2TrackId), 'A' + rng.below(26), ' ' + rng.below(2) * ('B' - ' '));
        std::strncpy(cstr(at), name, room - 1u);
        mem<char>(at + room - 1u) = 0;
    }

    static uint32_t dir(int32_t card) { return kCardVars + 0x400u + static_cast<uint32_t>(card) * 0x1b40u; }

    void directory(int32_t card)
    {
        const uint32_t d = dir(card);
        static const int32_t kStatus[] = {0, 0, 0, 1, 2, 3, 4, 5};
        static const int32_t kFree[] = {0, 0x4a, 0x4b, 0xcd, 0xce, 0x2000, 0x60};
        mem<int32_t>(d) = rng.pick(kStatus);
        mem<int32_t>(d + 4u) = rng.below(2);
        mem<int32_t>(d + 8u) = rng.chance(80) ? rng.pick(kFree) : static_cast<int32_t>(rng.next() & 0xffff);
        const uint32_t n = rng.chance(10) ? 0u : rng.chance(85) ? 1u + rng.below(10) : 11u + rng.below(30);
        mem<int32_t>(d + 0x1b00u) = static_cast<int32_t>(n);
        for (uint32_t i = 0; i < n + 2u && i < 99u; ++i)
        {
            for (uint32_t w = 0; w < 0x20u; w += 4u)
                mem<uint32_t>(d + 0x240u + i * 0x40u + w) = rng.next();
            entryName(d + 0x240u + i * 0x40u + 0x20u, rng.next());
        }
    }

    void cards()
    {
        directory(0);
        directory(1);
        mem<int32_t>(kCardVars + 0x5880u) = rng.chance(85) ? static_cast<int32_t>(rng.below(6)) : rng.pick({-1, 0x20}); // descriptor
        mem<uint32_t>(kCardVars + 0x5884u) = rng.pick({0u, 0x2000u, 0x3c4u, 0xfcf6u, rng.next()}); // bytesWritten
        G(typeG) = rng.pick({0, 1, 2, 2, 3});
        G(freeG) = rng.pick({0, 0x4b, 0xce, 0x2000});
        G(formattedG) = rng.below(2);
    }

    void loadSave()
    {
        const uint32_t l = kLoadSave;
        static const int32_t kStatus[] = {0, 0, 0, 0, 1, 2, 3, 4, 5};
        static const int32_t kCards[] = {0, 0, 0, 1, 1, 1, -1, 2};
        mem<int32_t>(l + 4u) = rng.pick(kCards);
        for (const uint32_t offset : {0x8u, 0xcu, 0x10u, 0x14u, 0x18u, 0x1cu})
            mem<int32_t>(l + offset) = rng.pick(kStatus);
        for (const uint32_t offset : {0x20u, 0x24u, 0x28u})
            mem<int32_t>(l + offset) = rng.chance(90) ? static_cast<int32_t>(rng.below(2)) : 2;
        mem<int32_t>(l + 0x34u) = rng.chance(95) ? static_cast<int32_t>(rng.below(5)) : static_cast<int32_t>(rng.next());
        mem<int32_t>(l + 0x38u) = rng.chance(90) ? static_cast<int32_t>(rng.below(2)) : 2;
        mem<int32_t>(l + 0x3cu) = rng.chance(90) ? static_cast<int32_t>(rng.below(2)) : 2;
        mem<uint32_t>(l + 0x40u) = rng.chance(85) ? rng.below(5) : rng.next();
        for (uint32_t i = 0; i < 45u; ++i)
            mem<uint32_t>(l + 0x44u + 4u * i) = s + kMsgs + i * 0x80u;
        mem<uint32_t>(l + 0xf8u) = s + kSprites;
        mem<uint32_t>(l + 0xfcu) = s + kSprites + 0x40u;
        for (uint32_t i = 0; i < 6u; ++i)
            mem<uint32_t>(l + 0x100u + 4u * i) = string(static_cast<int>(rng.below(16)));
        // The save buffer and game data: blocks of the arena, or none.
        mem<uint32_t>(l) = rng.chance(60) ? alloc(0x2000u) : 0u;
        mem<uint32_t>(l + 0x118u) = rng.chance(60) ? alloc(0x130u) : 0u;
        trackId(sym::revolt2TrackSaveIdToDelete, 24);
        trackId(sym::revolt2TrackSaveId, 24);
        std::snprintf(cstr(sym::revolt2TrackSaveName), 32, "%s", rng.chance(50) ? cstr(sym::revolt2TrackNameGeneric) : "x");
    }

    uint32_t alloc(uint32_t size)
    {
        const uint32_t at = env(eArena);
        setEnv(eArena, at + ((size + 15u) & ~15u));
        for (uint32_t i = 0; i < size; i += 4u)
            mem<uint32_t>(at + i) = rng.next();
        return at;
    }

    void menuSystem()
    {
        const uint32_t m = s + kMenuSys;
        for (uint32_t w = 0; w < 0x100u; w += 4u)
            mem<uint32_t>(m + w) = rng.next();
        static const uint32_t kCodes[] = {0, kBack, kYes, kNo, 0x10000075u, 0x1000007bu, 0x10u, 1u, 2u, 3u, 15u, 16u, 0x7fff0002u, 0x10000074u};
        mem<uint32_t>(m + 0x68u) = rng.chance(90) ? rng.pick(kCodes) : rng.next();
        // The typed name: letters, spaces, blank, or a name already on the card.
        const uint32_t name = m + 0xc4u;
        const uint32_t kind = rng.below(5);
        if (kind == 0)
        {
            const uint32_t n = rng.below(6);
            std::memset(guest_host(name), ' ', n);
            mem<char>(name + n) = 0;
        }
        else if (kind == 1)
            std::snprintf(cstr(name), 40, "Track%u%s", 1u + rng.below(16), rng.chance(50) ? "  " : "");
        else
        {
            randomName(name, 20);
            if (rng.chance(40))
                std::strcat(cstr(name), "   ");
        }
        G(pMenuSystemG) = ptr32<sMenuSystem>(m);
    }

    void frontEnd()
    {
        const uint32_t f = sym::frontEndVarG;
        mem<int8_t>(f + 0x2d0u) = rng.chance(75) ? 0 : static_cast<int8_t>(rng.between(1, 3)); // flagPTT
        for (uint32_t w = 0x138u; w < 0x138u + 0x130u; w += 4u)
            if (rng.chance(30))
                mem<uint32_t>(f + w) = rng.next();
    }

    // Arguments.
    int32_t card() { return rng.chance(92) ? static_cast<int32_t>(rng.below(2)) : rng.pick({-1, 2}); }
};

} // namespace

void cardStatusNow(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    World w(ctx, rng, scratch);
    setArg(ctx, 0, static_cast<uint32_t>(w.card()));
}

void cardStatusASync(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    World w(ctx, rng, scratch);
    setArg(ctx, 0, static_cast<uint32_t>(w.card()));
    setArg(ctx, 1, rng.chance(60) ? 0u : rng.chance(80) ? 1u : rng.next());
    setArg(ctx, 2, scratch + kResults);
}

void cardIconHeader(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    World w(ctx, rng, scratch);
    for (uint32_t i = 0; i < 0xc0u; i += 4u)
        mem<uint32_t>(scratch + kVectors + i) = rng.next();
    for (uint32_t i = 0; i < 3u; ++i)
        mem<uint32_t>(scratch + kIconNames + 4u * i) = w.string(static_cast<int>(rng.below(16)));
    w.randomName(scratch + kTitle, 0x3f);
    setArg(ctx, 0, rng.chance(70) ? scratch + kIconOut : sym::cardVarG + 0x10u);
    setArg(ctx, 1, scratch + kVectors + rng.below(4));
    setArg(ctx, 2, scratch + kVectors + 0x40u);
    setArg(ctx, 3, scratch + kVectors + 0x70u);
    setArg(ctx, 4, scratch + kVectors + 0xa0u + rng.below(4));
    setArg(ctx, 5, rng.chance(80) ? scratch + kTitle : sym::revolt2GameSaveName);
    setArg(ctx, 6, scratch + kIconNames);
    setArg(ctx, 7, rng.next());
    mem<uint32_t>(getRegU32(ctx, 29)) = rng.chance(50) ? 0x1eu : rng.next(); // lineBreakPos, the ninth
}

void cardFile(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    World w(ctx, rng, scratch);
    setArg(ctx, 0, static_cast<uint32_t>(w.card()));
    setArg(ctx, 1, w.string(static_cast<int>(rng.below(16))));
    setArg(ctx, 2, rng.pick({0x2000u, 0x3c4u, 0x10u, 0u, 0x203u, 1u, 2u, 0x100u}));
    setArg(ctx, 3, scratch + kArena + 0x100u * rng.below(8));
}

void cardPlain(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    World w(ctx, rng, scratch);
    setArg(ctx, 0, static_cast<uint32_t>(w.card()));
}

void cardWait(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    World w(ctx, rng, scratch);
    mem<uint32_t>(scratch + kResults) = rng.next();
    setArg(ctx, 0, scratch + kResults);
}

void cardRename(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    World w(ctx, rng, scratch);
    setArg(ctx, 0, static_cast<uint32_t>(w.card()));
    setArg(ctx, 1, w.string(static_cast<int>(rng.below(16))));
    setArg(ctx, 2, rng.chance(30) ? 0u : w.string(static_cast<int>(rng.below(16))));
}

void cardGetDir(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    World w(ctx, rng, scratch);
    const int32_t card = w.card();
    setArg(ctx, 0, static_cast<uint32_t>(card));
    setArg(ctx, 1, rng.chance(50) ? sym::revolt2ProductId : w.string(0));
    setArg(ctx, 2, rng.pick({99u, 1u, 5u, 12u, 0u}));
    setArg(ctx, 3, rng.chance(60) ? World::dir(card) + 0x240u : scratch + kArena);
}

void lsCreateDir(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    World w(ctx, rng, scratch);
    setArg(ctx, 0, static_cast<uint32_t>(w.card()));
    setArg(ctx, 1, rng.chance(50) ? sym::revolt2GameId : rng.chance(50) ? sym::revolt2TrackSaveId : w.string(1));
}

void lsTrackNames(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    World w(ctx, rng, scratch);
    setArg(ctx, 0, rng.below(2));
}

void lsDefaultName(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    World w(ctx, rng, scratch);
    const int32_t card = static_cast<int32_t>(rng.below(2));
    if (rng.chance(40)) // a card with many "Track<n>" saves, up to all fifteen
    {
        const uint32_t d = World::dir(card), n = 10u + rng.below(8);
        mem<int32_t>(d + 0x1b00u) = static_cast<int32_t>(n);
        for (uint32_t i = 0; i < n; ++i)
            std::snprintf(cstr(d + 0x260u + i * 0x40u), 32, "%sTrack%u", cstr(sym::revolt2TrackId), rng.chance(90) ? i + 1u : 1u + rng.below(16));
    }
    setArg(ctx, 0, static_cast<uint32_t>(card));
    setArg(ctx, 1, w.string(14)); // the save name's buffer
    setArg(ctx, 2, rng.chance(90) ? rng.below(2) : 2u);
}

void lsUniqueName(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    World w(ctx, rng, scratch);
    setArg(ctx, 0, rng.below(2));
    setArg(ctx, 1, scratch + kMenuSys + 0xc4u);
    setArg(ctx, 2, rng.chance(90) ? rng.below(2) : 2u);
    if (rng.chance(40)) // the name of a track on the card
    {
        const int32_t card = static_cast<int32_t>(arg(ctx, 0)), n = mem<int32_t>(World::dir(card) + 0x1b00u);
        if (n > 0)
        {
            const uint32_t entry = World::dir(card) + 0x260u + rng.below(static_cast<uint32_t>(n)) * 0x40u;
            const size_t idLength = std::strlen(cstr(sym::revolt2TrackId));
            if (std::strncmp(cstr(entry), cstr(sym::revolt2TrackId), idLength) == 0)
                std::snprintf(cstr(scratch + kMenuSys + 0xc4u), 40, "%s%s", cstr(entry) + idLength, rng.chance(50) ? " " : "");
            if (rng.chance(50))
                std::snprintf(cstr(sym::revolt2TrackSaveIdToDelete), 24, "%s", cstr(entry));
        }
    }
}

void lsGameData(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    World w(ctx, rng, scratch);
    mem<uint32_t>(kLoadSave) = w.alloc(0x2000u);
    mem<uint32_t>(kLoadSave + 0x118u) = w.alloc(0x130u);
}

void lsMerge(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    World w(ctx, rng, scratch);
    const uint32_t data = w.alloc(0x130u);
    mem<uint32_t>(kLoadSave + 0x118u) = data;
    // Times and cups either side of the game's; screen offsets of both signs.
    for (uint32_t i = 0; i < 0x30u; ++i)
    {
        const uint32_t game = sym::frontEndVarG + 0x138u + 0x64u + 4u * i;
        mem<int32_t>(data + 0x64u + 4u * i) = rng.chance(70) ? mem<int32_t>(game) + rng.between(-3, 3) : static_cast<int32_t>(rng.next());
    }
    mem<int32_t>(data + 0x124u) = mem<int32_t>(sym::frontEndVarG + 0x25cu) + rng.between(-2, 2);
    mem<int16_t>(data + 0x12cu) = static_cast<int16_t>(rng.between(-40, 40));
    mem<int16_t>(data + 0x12eu) = static_cast<int16_t>(rng.chance(80) ? rng.between(-40, 40) : static_cast<int32_t>(rng.next()));
}

void lsInit(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    World w(ctx, rng, scratch);
    setArg(ctx, 0, rng.chance(90) ? rng.below(5) : rng.next());
}

void lsAutoLoad(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    World w(ctx, rng, scratch);
    setEnv(eValidSave, rng.pick({50u, 90u}));
}

void lsFreeSpace(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    World w(ctx, rng, scratch);
    setArg(ctx, 0, rng.chance(50) ? 25u * rng.below(1000) : rng.next());
}

namespace {

// The step functions: a step their own most of the time, a result word, the caller's leftover register.
void step(R5900Context *ctx, Rng &rng, World &w, uint32_t scratch, uint32_t low, uint32_t high)
{
    mem<uint32_t>(kLoadSave + 0x30u) = rng.chance(85) ? static_cast<uint32_t>(rng.between(static_cast<int32_t>(low), static_cast<int32_t>(high)))
                                                      : rng.chance(80) ? rng.below(14) : rng.next();
    mem<uint32_t>(scratch + kResults) = rng.next();
    mem<uint32_t>(scratch + kResults + 4u) = rng.next();
    setArg(ctx, 0, static_cast<uint32_t>(w.card()));
    setArg(ctx, 1, rng.chance(50) ? sym::levelLoadG + 0x2e0u : w.string(static_cast<int>(rng.below(16))));
}

} // namespace

void lsWrite(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    World w(ctx, rng, scratch);
    step(ctx, rng, w, scratch, 0, 5);
    setArg(ctx, 2, rng.pick({0x2000u, 0x3c4u, 0xfcf6u, 0u, mem<uint32_t>(kCardVars + 0x5884u)}));
    setArg(ctx, 3, scratch + kArena);
    setArg(ctx, 4, scratch + kResults);
    setArg(ctx, 5, rng.next()); // $t1
}

void lsExists(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    World w(ctx, rng, scratch);
    step(ctx, rng, w, scratch, 4, 7);
    setArg(ctx, 2, scratch + kResults);
    setArg(ctx, 3, scratch + kResults + 4u);
    setArg(ctx, 4, rng.next()); // $t0
}

void lsErase(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    World w(ctx, rng, scratch);
    step(ctx, rng, w, scratch, 8, 9);
    setArg(ctx, 2, scratch + kResults);
    setArg(ctx, 3, rng.next()); // $a3
}

void lsRename(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    World w(ctx, rng, scratch);
    step(ctx, rng, w, scratch, 10, 11);
    setArg(ctx, 2, w.string(static_cast<int>(rng.below(16))));
    setArg(ctx, 3, scratch + kResults);
    setArg(ctx, 4, rng.next()); // $t0
}

void lsSetupClass(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    World w(ctx, rng, scratch);
    for (uint32_t i = 0; i < 0xd4u; i += 4u)
        mem<uint32_t>(scratch + kLsData + i) = rng.next();
    setArg(ctx, 0, scratch + kLsData);
}

void lsProcess(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    World w(ctx, rng, scratch);
    // The states with the most paths more often.
    static const uint32_t kHot[] = {2, 4, 4, 4, 9, 19, 21, 23, 35, 36, 37, 38, 40, 41, 44, 45, 45, 58, 60, 62, 64, 66, 66, 71};
    const uint32_t status = rng.chance(40) ? rng.pick(kHot) : rng.chance(92) ? rng.below(72) : rng.chance(50) ? 72u + rng.below(8) : rng.next();
    mem<uint32_t>(kLoadSave + 0x2cu) = status;
    // Each file step's subState as the state machine sets it, now and then another.
    uint32_t low = 0, high = 13;
    switch (status)
    {
    case 13: case 17: case 18: case 48: case 50: case 54: case 55: case 56: case 57: low = 0, high = 5; break; // write
    case 19: case 21: case 23: case 58: case 60: case 62: case 64: case 66: low = 4, high = 7; break;          // exists
    case 20: case 22: case 24: case 29: case 49: case 59: case 61: case 63: case 65: case 67: low = 8, high = 9; break; // erase
    case 26: low = 10, high = 11; break;                                                                        // rename
    default: break;
    }
    mem<uint32_t>(kLoadSave + 0x30u) = rng.chance(88) ? static_cast<uint32_t>(rng.between(static_cast<int32_t>(low), static_cast<int32_t>(high)))
                                                      : rng.chance(80) ? rng.below(14) : rng.next();
    if (rng.chance(70)) // the counted states mostly near their end
        mem<uint32_t>(kLoadSave + 0x40u) = rng.chance(60) ? 1u : rng.below(4);
    if (rng.chance(70)) // the answers the pages wait for
    {
        static const uint32_t kCodes[] = {kYes, kYes, kNo, kBack, 0x10000075u, 0x1000007bu, 1u, 2u, 3u, 15u, 16u, 0x10u};
        mem<uint32_t>(scratch + kMenuSys + 0x68u) = rng.pick(kCodes);
    }
    if (rng.chance(50)) // the selected card ready
        mem<int32_t>(kLoadSave + 0x10u) = 0;
    setArg(ctx, 0, rng.chance(50) ? 25u * rng.below(1000) : rng.next());
    setReg(ctx, 8, rng.next()); // $t0, $t1: the leftovers the file steps may return
    setReg(ctx, 9, rng.next());
}

void lsRenderCards(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    World w(ctx, rng, scratch);
    mem<uint32_t>(kLoadSave + 0x2cu) = rng.chance(80) ? rng.below(8) : rng.next();
}

void lsModeAvailable(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    World w(ctx, rng, scratch);
    mem<int32_t>(kLoadSave + 4u) = rng.chance(95) ? static_cast<int32_t>(rng.below(2)) : 2;
}

} // namespace rc::fuzz
