// Builds the random-test inputs of RCMUSIC.c, MUSIC.c, MPEG.c, MPDISPLAY.c, PCFILE.c and FM.c. See stream_fuzz.h.

#include "stream_fuzz.h"

#include <algorithm>
#include <cstdlib>
#include <string>

// The runtime's hook on stores to hardware registers (ps2_runtime.cpp), for the oracle only.
extern bool (*g_ps2HardwareStoreHook)(uint32_t address, uint32_t value);

namespace rc {

bool_ MemBlockAllocLo(ptr32<memHandle> pHandle, uint32 size); // mem.cpp
void MemBlockFree(ptr32<memHandle> pHandle);
uint32 FMFileRead(ptr32<void> pData, size_t size, anFMFile fd); // fm.cpp

namespace fuzz {

namespace {

constexpr uint32_t kFm = 0x29d8c8u, kRc = 0x1ee8c0u, kCd = 0x4bf5c0u;
constexpr uint32_t kMpegG = 0x1fa978u, kSplashG = 0x1fa9e8u, kDisp = 0x1fa9c0u;
constexpr uint32_t kDrawid = 0x328290u, kVblankHandle = 0x328294u, kMpegSwitch = 0x328298u, kMpegLooping = 0x32829cu,
                   kMpegBuffer = 0x3282a0u, kMpegSize = 0x3282a8u, kSplashBuffer = 0x3282b0u, kSplashSize = 0x3282b4u,
                   kRet = 0x3282b8u, kIsCountVblank = 0x3282bcu, kVblankCount = 0x3282c0u, kGetit = 0x3282c4u;
constexpr uint32_t kFrontFade = 0x327f68u;

// The functions replaced offline.
constexpr uint32_t kSifCallRpc = 0x1c0968u, kSifBindRpc = 0x1c07b8u, kSifInitRpc = 0x1c0180u, kPrintf = 0x12e9e8u,
                   kFree = 0x1b7280u;
constexpr uint32_t kCdSearchFile = 0x1cc110u, kCdDiskReady = 0x1cd708u, kCdSeek = 0x1ccea8u, kCdRead = 0x1cc920u,
                   kCdSync = 0x1cd1d8u;
constexpr uint32_t kSceOpen = 0x1c1170u, kSceClose = 0x1c12c8u, kSceLseek = 0x1c1380u, kSceRead = 0x1c14f0u,
                   kSceWrite = 0x1c1620u;
constexpr uint32_t kMpegInit = 0x1ce708u, kMpegCreate = 0x1ce758u, kMpegDelete = 0x1ce998u, kMpegAddBs = 0x1ce9a0u,
                   kMpegGetPicture = 0x1ce9d8u, kMpegIsEnd = 0x1ceaf0u, kMpegReset = 0x1ceb10u;
constexpr uint32_t kGsSyncV = 0x1c3d90u, kGsSyncPath = 0x1c3e28u, kGsPutDispEnv = 0x1c38b8u, kAddIntcHandler = 0x1be400u,
                   kRemoveIntcHandler = 0x1be420u;
constexpr uint32_t kFMFileRead = 0x1a0838u;
constexpr uint32_t kHardwareStore = 1u;

// Control words of the fakes (in scratch, so each run starts from the same values and they are compared).
constexpr uint32_t kEnv = kFuzzScratch + 0x7600u;
enum : uint32_t
{
    eSeed = 0x0,          // the answers' seed
    eHash = 0x4,          // a hash of every logged call, so calls past the log's end are compared too
    eCalls = 0x8,         // answers drawn so far
    eSearchFail = 0xc,    // sceCdSearchFile fails this many times before finding (1000 or more: never)
    eSearchCalls = 0x10,
    eReadRefuse = 0x14,   // percent of sceCdRead calls refused
    eSyncBusy = 0x18,     // percent of sceCdSync answers that are busy (positive)
    eSyncEnd = 0x1c,      // sceCdSync's answer when not busy (0 or negative)
    eFileSize = 0x20,     // the size of a file found (0: from its name)
    eIsEndAfter = 0x24,   // sceMpegIsEnd answers 1 from this call on
    eIsEndCalls = 0x28,
    eIsEndPercent = 0x2c, // and before that, 1 this often
    ePicFail = 0x30,      // percent of sceMpegGetPicture answers that are negative
    eSyncPathBusy = 0x34, // percent of sceGsSyncPath answers that are not 0
    eIntcId = 0x38,       // AddIntcHandler's answer
    eBindFail = 0x3c,     // percent of sceSifBindRpc answers that are negative
    eBindServeAt = 0x40,  // sceSifBindRpc sets the client's server pointer at this call
    eBindCalls = 0x44,
    eShort = 0x48,        // percent of sceRead/sceWrite answers short or negative
    eFrame = 0x4c,        // the first frame number sceMpegGetPicture sets
    eFrameStep = 0x50,    // subtracted at each call (playMpeg's frame wait hangs on a later frame)
    eRpcFail = 0x54,      // percent of sceSifCallRpc replies of -1
    eReadShort = 0x58,    // percent of FMFileRead calls (from readFile) answering one byte short
};
constexpr uint32_t kLog = kFuzzScratch + 0x7700u, kLogMax = 45u; // count word, then 5 words an entry

template <typename T>
T load(uint32_t address)
{
    T value;
    std::memcpy(&value, guest_host(address), sizeof(T));
    return value;
}

template <typename T>
void store(uint32_t address, T value)
{
    std::memcpy(guest_host(address), &value, sizeof(T));
}

uint32_t rnd(uint64_t &state, uint32_t n) { return n == 0 ? 0u : nextFuzz(state) % n; }
bool chance(uint64_t &state, uint32_t percent) { return rnd(state, 100) < percent; }
int32_t between(uint64_t &state, int32_t low, int32_t high) { return low + static_cast<int32_t>(rnd(state, static_cast<uint32_t>(high - low + 1))); }
void setArg(R5900Context *ctx, int index, uint32_t value) { SET_GPR_S32(ctx, 4 + index, static_cast<int32_t>(value)); }
uint32_t env(uint32_t offset) { return load<uint32_t>(kEnv + offset); }
void setEnv(uint32_t offset, uint32_t value) { store<uint32_t>(kEnv + offset, value); }
uint32_t arg(R5900Context *ctx, int i) { return getRegU32(ctx, 4 + i); }

void putString(uint32_t at, const std::string &text) { std::memcpy(guest_host(at), text.c_str(), text.size() + 1u); }

uint32_t mix(uint32_t x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

uint32_t hashBytes(uint32_t at, uint32_t size)
{
    uint32_t hash = 2166136261u;
    for (uint32_t i = 0; i < size; ++i)
        hash = (hash ^ load<uint8_t>(at + i)) * 16777619u;
    return hash;
}

uint32_t hashString(uint32_t at)
{
    uint32_t hash = 2166136261u;
    for (uint32_t i = 0; i < 0x200u; ++i)
    {
        const uint8_t c = load<uint8_t>(at + i);
        hash = (hash ^ c) * 16777619u;
        if (c == 0)
            break;
    }
    return hash;
}

// The next scripted number of this input.
uint32_t draw()
{
    const uint32_t k = env(eCalls);
    setEnv(eCalls, k + 1u);
    return mix(env(eSeed) + k * 0x9e3779b9u);
}

bool drawChance(uint32_t percent) { return draw() % 100u < percent; }

void logEntry(uint32_t what, uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
    static const bool trace = std::getenv("RC_STREAMFUZZ_TRACE") != nullptr; // each logged call on stderr
    if (trace)
        std::fprintf(stderr, "[stream_fuzz] %x %x %x %x %x\n", what, a, b, c, d);
    uint32_t hash = env(eHash);
    for (const uint32_t word : {what, a, b, c, d})
        hash = (hash ^ word) * 16777619u;
    setEnv(eHash, hash);
    const uint32_t count = load<uint32_t>(kLog);
    if (count < kLogMax)
    {
        const uint32_t at = kLog + 4u + count * 20u;
        store<uint32_t>(at, what);
        store<uint32_t>(at + 4u, a);
        store<uint32_t>(at + 8u, b);
        store<uint32_t>(at + 12u, c);
        store<uint32_t>(at + 16u, d);
    }
    store<uint32_t>(kLog, count + 1u);
}

void returnWith(R5900Context *ctx, uint32_t value)
{
    SET_GPR_S32(ctx, 2, static_cast<int32_t>(value));
    ctx->pc = getRegU32(ctx, 31);
}

template <uint32_t Address, int Args>
void loggedCall(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    const auto a = [ctx](int i) { return i < Args ? arg(ctx, i) : 0u; };
    logEntry(Address, a(0), a(1), a(2), a(3));
    returnWith(ctx, 0);
}

// --- The IOP. ---

// sceSifCallRpc(client, command, mode, send, send size, receive, receive size, end function; end argument at $sp).
void fakeCallRpc(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    const uint32_t send = arg(ctx, 3), sendSize = arg(ctx, 4), recv = arg(ctx, 5), recvSize = arg(ctx, 6);
    const uint32_t stackArg = load<uint32_t>(getRegU32(ctx, 29));
    logEntry(kSifCallRpc, arg(ctx, 0), arg(ctx, 1), arg(ctx, 2), hashBytes(send, std::min(sendSize, 0x40u)));
    logEntry(kSifCallRpc + 1u, send, sendSize, recv, recvSize ^ (arg(ctx, 7) << 8) ^ (stackArg << 16));
    const uint32_t r = draw();
    const uint32_t answer = r % 100u < env(eRpcFail) ? 0xffffffffu : (r & 0x10000u) != 0u ? 0u : mix(r) % 0x10000u;
    if (recv != 0u)
        for (uint32_t i = 0; i < std::min(recvSize, 0x40u); i += 4u)
            store<uint32_t>(recv + i, i == 0 ? answer : 0u);
    returnWith(ctx, 0);
}

void fakeBindRpc(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    const uint32_t client = arg(ctx, 0);
    const uint32_t call = env(eBindCalls) + 1u;
    setEnv(eBindCalls, call);
    const bool fail = drawChance(env(eBindFail));
    if (!fail && call >= env(eBindServeAt))
        store<uint32_t>(client + 0x24u, 0x8000u + call);
    logEntry(kSifBindRpc, client, arg(ctx, 1), arg(ctx, 2), fail ? 1u : 0u);
    returnWith(ctx, fail ? 0xfffffffeu : 0u);
}

// --- The disc. ---

// sceCdSearchFile(file, name): fails a scripted number of times, then gives a position and size made from the name.
void fakeSearchFile(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    const uint32_t file = arg(ctx, 0), name = arg(ctx, 1);
    const uint32_t call = env(eSearchCalls);
    setEnv(eSearchCalls, call + 1u);
    const uint32_t hash = hashString(name);
    const bool found = call >= env(eSearchFail);
    if (found)
    {
        store<uint32_t>(file, 0x200u + hash % 0x40000u);
        store<uint32_t>(file + 4u, env(eFileSize) != 0u ? env(eFileSize) : 1u + mix(hash) % 0x3000u);
    }
    logEntry(kCdSearchFile, file, hash, found ? 1u : 0u, 0);
    returnWith(ctx, found ? 1u : 0u);
}

// sceCdRead(sector, count, buffer, mode): refused at random; when accepted, the sectors' pattern is in the buffer.
void fakeCdRead(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    const uint32_t sector = arg(ctx, 0), count = arg(ctx, 1), buffer = arg(ctx, 2), mode = arg(ctx, 3);
    const bool refused = drawChance(env(eReadRefuse));
    if (!refused)
        for (uint32_t s = 0; s < std::min(count, 0x80u); ++s)
            for (uint32_t w = 0; w < 0x200u; ++w)
                store<uint32_t>(buffer + s * 0x800u + w * 4u, mix((sector + s) * 0x200u + w + 0x51ed27u));
    logEntry(kCdRead, sector, count, buffer, load<uint32_t>(mode) & 0xffffffu);
    returnWith(ctx, refused ? 0u : 1u);
}

void fakeCdSync(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    const uint32_t answer = drawChance(env(eSyncBusy)) ? 1u + draw() % 2u : env(eSyncEnd);
    logEntry(kCdSync, arg(ctx, 0), answer, 0, 0);
    returnWith(ctx, answer);
}

void fakeDiskReady(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    logEntry(kCdDiskReady, arg(ctx, 0), 0, 0, 0);
    returnWith(ctx, 2u);
}

// --- Host files. ---

uint32_t shortAnswer(uint32_t size)
{
    if (!drawChance(env(eShort)))
        return size;
    const uint32_t r = draw();
    return (r & 3u) == 0u ? 0xffffffffu : size == 0u ? 0u : r % size;
}

void fakeSceOpen(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    const uint32_t answer = drawChance(20) ? 0xfffffffeu : 3u + draw() % 5u;
    logEntry(kSceOpen, hashString(arg(ctx, 0)), arg(ctx, 1), answer, 0);
    returnWith(ctx, answer);
}

void fakeSceRead(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    const uint32_t fd = arg(ctx, 0), buffer = arg(ctx, 1), size = arg(ctx, 2);
    const uint32_t answer = shortAnswer(size);
    const uint32_t filled = static_cast<int32_t>(answer) < 0 ? 0u : std::min(answer, 0x2000u);
    for (uint32_t i = 0; i < filled; ++i)
        store<uint8_t>(buffer + i, static_cast<uint8_t>(mix(i + 0x77u * fd + env(eSeed))));
    logEntry(kSceRead, fd, buffer, size, answer);
    returnWith(ctx, answer);
}

void fakeSceWrite(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    const uint32_t fd = arg(ctx, 0), buffer = arg(ctx, 1), size = arg(ctx, 2);
    const uint32_t answer = shortAnswer(size);
    logEntry(kSceWrite, fd, buffer, size, hashBytes(buffer, std::min(size, 0x2000u)) ^ answer);
    returnWith(ctx, answer);
}

void fakeSceLseek(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    const uint32_t answer = drawChance(20) ? 0xffffffffu : draw() % 0x10000u;
    logEntry(kSceLseek, arg(ctx, 0), arg(ctx, 1), arg(ctx, 2), answer);
    returnWith(ctx, answer);
}

void fakeSceClose(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    const uint32_t answer = drawChance(20) ? 0xffffffffu : 0u;
    logEntry(kSceClose, arg(ctx, 0), answer, 0, 0);
    returnWith(ctx, answer);
}

// --- The movie decoder, the GS and interrupts. ---

void fakeIsEnd(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    const uint32_t call = env(eIsEndCalls) + 1u;
    setEnv(eIsEndCalls, call);
    const bool end = call >= env(eIsEndAfter) || drawChance(env(eIsEndPercent));
    logEntry(kMpegIsEnd, arg(ctx, 0), call, end ? 1u : 0u, 0);
    returnWith(ctx, end ? 1u : 0u);
}

// sceMpegGetPicture(decoder, buffer, size): fails now and then; sets the decoder's width, height and frame number.
void fakeGetPicture(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    const uint32_t mpeg = arg(ctx, 0);
    const uint32_t r = draw();
    const bool fail = r % 100u < env(ePicFail);
    if (!fail)
    {
        static const uint32_t kWidths[] = {640u, 512u, 320u, 16u}, kHeights[] = {448u, 256u, 224u, 16u};
        store<uint32_t>(mpeg, (r & 0x300u) == 0x300u ? mix(r) % 0x400u : kWidths[(r >> 8) & 3u]);
        store<uint32_t>(mpeg + 4u, (r & 0xc00u) == 0xc00u ? mix(r + 1u) % 0x400u : kHeights[(r >> 10) & 3u]);
        store<uint32_t>(mpeg + 8u, env(eFrame));
        setEnv(eFrame, env(eFrame) - env(eFrameStep));
    }
    logEntry(kMpegGetPicture, mpeg, arg(ctx, 1), arg(ctx, 2), fail ? 1u : 0u);
    returnWith(ctx, fail ? 0xffffffffu : 0u);
}

void fakeSyncV(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    const uint32_t answer = draw() & 1u;
    logEntry(kGsSyncV, arg(ctx, 0), answer, 0, 0);
    returnWith(ctx, answer);
}

void fakeSyncPath(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    const uint32_t answer = drawChance(env(eSyncPathBusy)) ? 1u + draw() % 3u : 0u;
    logEntry(kGsSyncPath, arg(ctx, 0), arg(ctx, 1), answer, 0);
    returnWith(ctx, answer);
}

void fakePutDispEnv(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    logEntry(kGsPutDispEnv, arg(ctx, 0), hashBytes(arg(ctx, 0), 0x28u), 0, 0);
    returnWith(ctx, 0);
}

void fakeAddIntc(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    logEntry(kAddIntcHandler, arg(ctx, 0), arg(ctx, 1), arg(ctx, 2), 0);
    returnWith(ctx, env(eIntcId));
}

void fakePrintf(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    logEntry(kPrintf, arg(ctx, 0), hashString(arg(ctx, 0)), arg(ctx, 1), 0);
    returnWith(ctx, 0);
}

// FMFileRead as it is, but now and then answering one byte short (it always answers the size asked for), so
// readFile's failed read is reached.
void shortFileRead(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t size = arg(ctx, 1);
    Thunk<&FMFileRead>::run(rdram, ctx, runtime);
    if (drawChance(env(eReadShort)))
        SET_GPR_S32(ctx, 2, static_cast<int32_t>(size - 1u));
    logEntry(kFMFileRead, size, getRegU32(ctx, 2), 0, 0);
}

bool loggedStore(uint32_t address, uint32_t value)
{
    logEntry(kHardwareStore, address, value, 0, 0);
    return true; // dropped
}

// Offline: installs the fakes (once) and resets their control words to a random script.
void quietStream(uint64_t &state)
{
    if (!g_offline)
        return;
    static bool installed = false;
    if (!installed)
    {
        installed = true;
        PS2Runtime &rt = *g_runtime;
        rt.replaceFunction(kSifCallRpc, &fakeCallRpc);
        rt.replaceFunction(kSifBindRpc, &fakeBindRpc);
        rt.replaceFunction(kSifInitRpc, &loggedCall<kSifInitRpc, 1>);
        rt.replaceFunction(kPrintf, &fakePrintf);
        rt.replaceFunction(kFree, &loggedCall<kFree, 1>);
        rt.replaceFunction(kCdSearchFile, &fakeSearchFile);
        rt.replaceFunction(kCdDiskReady, &fakeDiskReady);
        rt.replaceFunction(kCdSeek, &loggedCall<kCdSeek, 1>);
        rt.replaceFunction(kCdRead, &fakeCdRead);
        rt.replaceFunction(kCdSync, &fakeCdSync);
        rt.replaceFunction(kSceOpen, &fakeSceOpen);
        rt.replaceFunction(kSceRead, &fakeSceRead);
        rt.replaceFunction(kSceWrite, &fakeSceWrite);
        rt.replaceFunction(kSceLseek, &fakeSceLseek);
        rt.replaceFunction(kSceClose, &fakeSceClose);
        rt.replaceFunction(kMpegInit, &loggedCall<kMpegInit, 0>);
        rt.replaceFunction(kMpegCreate, &loggedCall<kMpegCreate, 3>);
        rt.replaceFunction(kMpegDelete, &loggedCall<kMpegDelete, 1>);
        rt.replaceFunction(kMpegAddBs, &loggedCall<kMpegAddBs, 3>);
        rt.replaceFunction(kMpegReset, &loggedCall<kMpegReset, 1>);
        rt.replaceFunction(kMpegIsEnd, &fakeIsEnd);
        rt.replaceFunction(kMpegGetPicture, &fakeGetPicture);
        rt.replaceFunction(kGsSyncV, &fakeSyncV);
        rt.replaceFunction(kGsSyncPath, &fakeSyncPath);
        rt.replaceFunction(kGsPutDispEnv, &fakePutDispEnv);
        rt.replaceFunction(kAddIntcHandler, &fakeAddIntc);
        rt.replaceFunction(kRemoveIntcHandler, &loggedCall<kRemoveIntcHandler, 2>);
        rt.replaceFunction(kFMFileRead, &shortFileRead);
        g_ps2HardwareStoreHook = &loggedStore;
    }
    for (uint32_t offset = 0; offset < 0x100u; offset += 4u)
        setEnv(offset, 0);
    store<uint32_t>(kLog, 0);
    setEnv(eSeed, nextFuzz(state));
    static const uint32_t kFails[] = {0u, 0u, 0u, 1u, 2u, 5u, 1000u};
    setEnv(eSearchFail, kFails[rnd(state, 7)]);
    setEnv(eReadRefuse, chance(state, 50) ? 0u : rnd(state, 70));
    setEnv(eSyncBusy, chance(state, 50) ? 0u : rnd(state, 70));
    setEnv(eSyncEnd, chance(state, 80) ? 0u : static_cast<uint32_t>(-between(state, 1, 3)));
    setEnv(eIsEndAfter, chance(state, 30) ? 1u : 2u + rnd(state, 6));
    setEnv(eIsEndPercent, chance(state, 50) ? 0u : rnd(state, 40));
    setEnv(ePicFail, chance(state, 50) ? 0u : rnd(state, 60));
    setEnv(eSyncPathBusy, chance(state, 40) ? 0u : rnd(state, 80));
    setEnv(eIntcId, chance(state, 70) ? 1u + rnd(state, 8) : chance(state, 50) ? 0u : nextFuzz(state));
    setEnv(eBindFail, chance(state, 70) ? 0u : rnd(state, 30));
    setEnv(eBindServeAt, chance(state, 50) ? 1u : 1u + rnd(state, 8));
    setEnv(eShort, chance(state, 50) ? 0u : rnd(state, 60));
    setEnv(eFrame, chance(state, 50) ? 0u : static_cast<uint32_t>(-between(state, 0, 50)));
    setEnv(eFrameStep, chance(state, 50) ? 0u : rnd(state, 4));
    setEnv(eRpcFail, chance(state, 70) ? 0u : rnd(state, 50));
    setEnv(eReadShort, chance(state, 70) ? 0u : 50u);
}

// --- The heap. ---

uint32_t largestFree()
{
    const uint32_t header = G(memVarG).pHeader.address;
    uint32_t largest = 0;
    for (uint32_t block = load<uint32_t>(header + 0xcu), n = 0; block != header && n < 100000; block = load<uint32_t>(block + 0xcu), ++n)
        if (load<uint32_t>(block) == MEM_BLOCK_TYPE_FREE)
            largest = std::max(largest, load<uint32_t>(block + 4u));
    return largest;
}

// Frees the level's largest blocks until a free block of this size exists.
void makeRoom(uint32_t bytes, uint32_t spare)
{
    for (int tries = 0; tries < 64 && largestFree() < bytes; ++tries)
    {
        const uint32_t header = G(memVarG).pHeader.address;
        uint32_t biggest = 0, biggestSize = 0;
        for (uint32_t block = load<uint32_t>(header + 0xcu), n = 0; block != header && n < 100000; block = load<uint32_t>(block + 0xcu), ++n)
            if (load<uint32_t>(block) == MEM_BLOCK_TYPE_USED && load<uint32_t>(block + 4u) > biggestSize)
            {
                biggest = block;
                biggestSize = load<uint32_t>(block + 4u);
            }
        if (biggest == 0u)
            return;
        store<uint32_t>(spare, biggest + 0x40u);
        MemBlockFree(ptr32<memHandle>(spare));
    }
}

// A heap block in a handle (or nothing).
void heapBlock(uint64_t &state, uint32_t handle, uint32_t spare, bool maybeNone = true)
{
    store<uint32_t>(handle, 0u);
    if (maybeNone && chance(state, 10))
        return;
    makeRoom(0x10000u, spare);
    MemBlockAllocLo(ptr32<memHandle>(handle), chance(state, 50) ? rnd(state, 0x400) : rnd(state, 0x8000));
}

// --- FM's state. ---

uint32_t fileAt(int32_t slot) { return kFm + static_cast<uint32_t>(slot) * 16u; }
uint32_t dirAt(uint32_t index) { return kFm + 0x60u + index * 0x1cu; }
uint32_t cacheAt(uint32_t index) { return kFm + 0x178u + index * 16u; }

void varyFiles(uint64_t &state)
{
    const uint32_t kind = rnd(state, 4);
    for (int32_t i = 0; i < 6; ++i)
    {
        uint32_t flags = load<uint32_t>(fileAt(i));
        if (kind == 0)
            flags = chance(state, 50) ? 0u : 1u;
        else if (kind == 1)
            flags = 1u; // all in use
        else if (kind == 2)
            flags = chance(state, 80) ? 0u : nextFuzz(state);
        store<uint32_t>(fileAt(i), flags);
        if (chance(state, 50))
        {
            store<uint32_t>(fileAt(i) + 4u, chance(state, 50) ? rnd(state, 0x10000) : nextFuzz(state));
            store<uint32_t>(fileAt(i) + 8u, rnd(state, 0x50000));
            store<uint32_t>(fileAt(i) + 12u, chance(state, 50) ? rnd(state, 0x100000) : nextFuzz(state));
        }
    }
}

const char *const kNames[] = {
    "CAT\\RDS.CAT", "rc2.luf", "_rc2.tlf", "\\MUS1\\FRONTEND.XA2", "\\splash\\acclaim.m2v", "splash\\dolby.m2v",
    "scar1.m2v", "_", "", "a", "\\", "bb\\17\\17.bbk", "Mixed_Case-Name.Ext", "zzzzzzzzzzzzzzzzz", "\\long\\path\\name.dat",
};

std::string randomName(uint64_t &state)
{
    if (chance(state, 70))
        return kNames[rnd(state, sizeof(kNames) / sizeof(kNames[0]))];
    std::string text;
    const uint32_t length = rnd(state, 30);
    for (uint32_t i = 0; i < length; ++i)
    {
        static const char kChars[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_\\.;{`~";
        text.push_back(kChars[rnd(state, sizeof(kChars) - 1u)]);
    }
    return text;
}

// The name FMFileOpen looks up: upper-cased, '_' dropped, "\\" put in front unless there, ";1" after.
std::string decorated(std::string name)
{
    if (!name.empty() && name[0] == '_')
        name.erase(0, 1);
    for (char &c : name)
        if (c >= 'a' && c <= 'z')
            c = static_cast<char>(c - 0x20);
    return (!name.empty() && name[0] == '\\' ? "" : "\\") + name + ";1";
}

void varyDirCache(uint64_t &state, const std::string &name)
{
    sFMClass &f = G(fmVarG);
    for (uint32_t i = 0; i < 10u; ++i)
    {
        if (chance(state, 40))
            continue;
        std::string text = chance(state, 30) ? decorated(name) : chance(state, 50) ? randomName(state) : decorated(randomName(state));
        if (text.size() > 19u)
            text.resize(19u);
        putString(dirAt(i), text);
        store<int32_t>(dirAt(i) + 0x14u, chance(state, 50) ? -1 : static_cast<int32_t>(rnd(state, 0x40000)));
        store<uint32_t>(dirAt(i) + 0x18u, rnd(state, 0x100000));
    }
    f.dirCacheIdx = chance(state, 90) ? rnd(state, 10) : rnd(state, 13);
}

// A cache over a buffer in scratch: one chunk of six (mode 0) or six of one (mode 1), of 1 or 2 sectors.
void scratchCache(uint64_t &state, uint32_t scratch)
{
    sFMClass &f = G(fmVarG);
    const uint32_t real = 1u + rnd(state, 2);
    const bool mode0 = chance(state, 40);
    f.realSectorsPerChunk = real;
    f.sectorsPerChunk = mode0 ? real * 6u : real;
    f.usedCacheEntries = mode0 ? (chance(state, 90) ? 1u : 0u) : (chance(state, 80) ? 6u : rnd(state, 7));
    for (uint32_t i = 0; i < 6u; ++i)
        f.cache[i].pData = ptr32<void>(scratch + i * real * 0x800u);
}

} // namespace

// --- FM ---

void fmPrepareEx(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    setArg(ctx, 0, chance(state, 50) ? scratch + rnd(state, 0x400) * 16u : nextFuzz(state));
    static const uint32_t kSizes[] = {0u, 0x2fffu, 0x3000u, 0x28000u, 0x32000u, 0x4b000u};
    setArg(ctx, 1, chance(state, 50) ? kSizes[rnd(state, 6)] : nextFuzz(state) >> rnd(state, 16));
}

void fmCacheMode(R5900Context *ctx, uint64_t &state, uint32_t)
{
    sFMClass &f = G(fmVarG);
    f.realSectorsPerChunk = chance(state, 50) ? rnd(state, 20) : nextFuzz(state);
    for (uint32_t i = 0; i < 6u; ++i)
        f.cache[i].flags = nextFuzz(state);
    setArg(ctx, 0, chance(state, 40) ? 0u : chance(state, 50) ? 1u : nextFuzz(state));
}

void fmUnPrepare(R5900Context *, uint64_t &state, uint32_t scratch)
{
    heapBlock(state, cacheAt(0) + 0xcu, scratch + 0x10u, false); // freeing a null handle writes below address 0
}

void fmFileOpen(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    quietStream(state);
    const std::string name = randomName(state);
    const uint32_t at = scratch + 0x400u + rnd(state, 4);
    putString(at, name);
    varyFiles(state);
    varyDirCache(state, name);
    setArg(ctx, 0, at);
}

void fmFileRead(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    quietStream(state);
    sFMClass &f = G(fmVarG);
    scratchCache(state, scratch);
    const int32_t fd = static_cast<int32_t>(rnd(state, 6));
    const uint32_t start = chance(state, 30) ? f.sectorsPerChunk * rnd(state, 100) : rnd(state, 0x4000);
    const uint32_t offset = chance(state, 30) ? 0x800u * rnd(state, 8) : chance(state, 50) ? rnd(state, 0x6000) : 0u;
    store<uint32_t>(fileAt(fd) + 4u, offset);
    store<uint32_t>(fileAt(fd) + 8u, start);
    const uint32_t size = chance(state, 10) ? 0u : chance(state, 40) ? 1u + rnd(state, 0x40) : rnd(state, 0x15f0);
    // Cache entries: some holding the chunks the read needs.
    const uint32_t first = ((offset >> 11) + start) / f.sectorsPerChunk;
    for (uint32_t i = 0; i < 6u; ++i)
    {
        static const uint32_t kFlags[] = {0u, 1u, 1u, 3u, 2u};
        f.cache[i].flags = kFlags[rnd(state, 5)];
        f.cache[i].startChunk = chance(state, 50) ? first + rnd(state, 4) - 1u : nextFuzz(state);
        f.cache[i].time = chance(state, 30) ? 5u : chance(state, 80) ? rnd(state, 20) : nextFuzz(state);
    }
    f.timeStamp = static_cast<int32_t>(chance(state, 50) ? rnd(state, 30) : nextFuzz(state));
    setArg(ctx, 0, scratch + 0x6000u + rnd(state, 0x10));
    setArg(ctx, 1, size);
    setArg(ctx, 2, static_cast<uint32_t>(fd));
}

void fmState(R5900Context *, uint64_t &state, uint32_t)
{
    sFMClass &f = G(fmVarG);
    varyFiles(state);
    for (uint32_t i = 0; i < 6u; ++i)
        f.cache[i].flags = nextFuzz(state);
    for (uint32_t i = 0; i < 10u; ++i)
        f.dirCache[i].filename[0] = static_cast<char>(nextFuzz(state));
    f.dirCacheIdx = nextFuzz(state);
    f.timeStamp = static_cast<int32_t>(nextFuzz(state));
}

void fmPrepare(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    const uint32_t kind = rnd(state, 4);
    makeRoom(0x50000u, scratch + 0x10u);
    setArg(ctx, 0, kind == 0 ? rnd(state, 0x3000) : kind == 1 ? 0x28000u : kind == 2 ? rnd(state, 0x50000) : nextFuzz(state));
}

void fmFileArgs(R5900Context *ctx, uint64_t &state, uint32_t)
{
    varyFiles(state);
    setArg(ctx, 0, chance(state, 90) ? rnd(state, 6) : static_cast<uint32_t>(between(state, -2, 9)));
    setArg(ctx, 1, chance(state, 50) ? rnd(state, 0x10000) : nextFuzz(state));
    setArg(ctx, 2, static_cast<uint32_t>(between(state, -1, 3)));
}

void fmFetch(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    quietStream(state);
    setArg(ctx, 0, chance(state, 50) ? rnd(state, 0x50000) : nextFuzz(state));
    setArg(ctx, 1, scratch + rnd(state, 0x100) * 16u);
    setArg(ctx, 2, rnd(state, 9));
}

// --- PCFILE ---

void pcCreat(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    quietStream(state);
    std::string name = randomName(state);
    if (chance(state, 10))
        name.assign(rnd(state, 0x100), 'x'); // up to the 260-byte name buffer, not past the frame
    putString(scratch + 0x400u, name);
    setArg(ctx, 0, scratch + 0x400u);
    setArg(ctx, 1, nextFuzz(state));
}

void pcLseek(R5900Context *ctx, uint64_t &state, uint32_t)
{
    quietStream(state);
    setArg(ctx, 0, rnd(state, 8));
    setArg(ctx, 1, nextFuzz(state));
    setArg(ctx, 2, static_cast<uint32_t>(between(state, -1, 4)));
    setArg(ctx, 3, nextFuzz(state));
}

void pcReadWrite(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    quietStream(state);
    setArg(ctx, 0, rnd(state, 8));
    setArg(ctx, 1, scratch + 0x100u + rnd(state, 0x100));
    setArg(ctx, 2, chance(state, 50) ? rnd(state, 0x24) : rnd(state, 0x1000));
}

void pcClose(R5900Context *ctx, uint64_t &state, uint32_t)
{
    quietStream(state);
    setArg(ctx, 0, chance(state, 80) ? rnd(state, 8) : nextFuzz(state));
}

// --- MPEG, MPDISPLAY ---

namespace {

// The player's state: buffers (in scratch or none), counters, the display settings and the decoder's last picture.
void varyPlayer(uint64_t &state, uint32_t scratch)
{
    store<int32_t>(kMpegSwitch, chance(state, 90) ? static_cast<int32_t>(rnd(state, 2)) : between(state, -1, 3));
    for (uint32_t i = 0; i < 2u; ++i)
    {
        store<uint32_t>(kMpegBuffer + i * 4u, chance(state, 25) ? 0u : scratch + 0x200u + rnd(state, 0x100) * 16u);
        store<int32_t>(kMpegSize + i * 4u, static_cast<int32_t>(chance(state, 50) ? rnd(state, 0x8000) : nextFuzz(state)));
    }
    static const int32_t kLooping[] = {0, 1, 1, -1, 2};
    store<int32_t>(kMpegLooping, kLooping[rnd(state, 5)]);
    static const int32_t kHandles[] = {0, -1, 1, 5};
    store<int32_t>(kVblankHandle, chance(state, 80) ? kHandles[rnd(state, 4)] : static_cast<int32_t>(nextFuzz(state)));
    store<int32_t>(kDrawid, chance(state, 90) ? static_cast<int32_t>(rnd(state, 2)) : static_cast<int32_t>(nextFuzz(state)));
    store<int32_t>(kGetit, chance(state, 70) ? static_cast<int32_t>(rnd(state, 2)) - (chance(state, 40) ? 2 : 0) : static_cast<int32_t>(nextFuzz(state)));
    uint32_t count = chance(state, 50) ? rnd(state, 0x200) : nextFuzz(state);
    if (chance(state, 30))
        count = (count & ~0x3fu) | (chance(state, 50) ? 0u : 0xfu);
    store<uint32_t>(kVblankCount, count);
    store<uint32_t>(kIsCountVblank, chance(state, 80) ? rnd(state, 2) : nextFuzz(state));
    store<uint32_t>(kRet, nextFuzz(state));
    store<uint32_t>(kDisp + 0x10u, nextFuzz(state));
    store<uint32_t>(kDisp + 0x14u, nextFuzz(state));
    for (const uint32_t decoder : {kMpegG, kSplashG})
    {
        store<uint32_t>(decoder, rnd(state, 0x300));
        store<uint32_t>(decoder + 4u, rnd(state, 0x300));
        store<int32_t>(decoder + 8u, chance(state, 50) ? 0 : static_cast<int32_t>(nextFuzz(state)));
    }
    store<uint32_t>(kSplashBuffer, chance(state, 30) ? 0u : scratch + 0x200u);
    store<uint32_t>(kSplashSize, rnd(state, 0x8000));
}

// The disc for readFile: a free file slot (or none), the movie's size, the heap with room (or nearly full).
void movieDisc(uint64_t &state, uint32_t scratch)
{
    varyFiles(state);
    if (chance(state, 85))
        store<uint32_t>(fileAt(static_cast<int32_t>(rnd(state, 6))), 0u);
    makeRoom(0x40000u, scratch + 0x10u);
    static const uint32_t kSizes[] = {0x800u, 0x1234u, 0u, 1u, 0x20000u, 0x3fffffffu};
    setEnv(eFileSize, chance(state, 30) ? 0u : kSizes[rnd(state, 6)]);
}

uint32_t movieName(uint64_t &state, uint32_t at)
{
    std::string name = randomName(state);
    if (name.size() > 0x30u)
        name.resize(0x30u); // the 80-byte path buffer holds "\\splash\\" and the name
    putString(at, name);
    return at;
}

} // namespace

void mpegFile(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    quietStream(state);
    varyPlayer(state, scratch);
    store<int32_t>(kMpegSwitch, static_cast<int32_t>(rnd(state, 2)));
    movieDisc(state, scratch);
    setArg(ctx, 0, movieName(state, scratch + 0x400u));
}

void mpegReadFile(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    quietStream(state);
    movieDisc(state, scratch);
    store<uint32_t>(scratch + 0x20u, nextFuzz(state));
    store<uint32_t>(scratch + 0x24u, nextFuzz(state));
    setArg(ctx, 0, movieName(state, scratch + 0x400u));
    setArg(ctx, 1, scratch + 0x20u);
    setArg(ctx, 2, scratch + 0x24u);
    setArg(ctx, 3, chance(state, 50) ? 0u : chance(state, 80) ? 1u : nextFuzz(state));
}

void mpegLoad(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    quietStream(state);
    varyPlayer(state, scratch);
    movieDisc(state, scratch);
    setArg(ctx, 0, movieName(state, scratch + 0x400u));
    setArg(ctx, 1, rnd(state, 2));
    setArg(ctx, 2, chance(state, 50) ? 0u : 1u);
    setArg(ctx, 3, chance(state, 50) ? 0u : chance(state, 80) ? 1u : nextFuzz(state));
}

void mpegPlay(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    quietStream(state);
    varyPlayer(state, scratch);
    setArg(ctx, 0, chance(state, 90) ? rnd(state, 2) : static_cast<uint32_t>(between(state, -1, 3)));
    static const int32_t kLooping[] = {0, 1, 1, -1, 2};
    setArg(ctx, 1, static_cast<uint32_t>(kLooping[rnd(state, 5)]));
}

void mpegTerm(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    quietStream(state);
    varyPlayer(state, scratch);
    const uint32_t slot = rnd(state, 2);
    heapBlock(state, kMpegBuffer + slot * 4u, scratch + 0x10u);
    if (chance(state, 50))
        heapBlock(state, kMpegBuffer + (slot ^ 1u) * 4u, scratch + 0x10u);
    setArg(ctx, 0, slot);
}

void mpegClear(R5900Context *ctx, uint64_t &state, uint32_t)
{
    quietStream(state);
    setArg(ctx, 0, chance(state, 50) ? 0x80000000u : nextFuzz(state));
}

void mpegPlayMpeg(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    quietStream(state);
    varyPlayer(state, scratch);
    // playMpeg waits for each frame on a counter it never reloads: frames are 0 or counting down from 0.
    setEnv(eFrame, chance(state, 50) ? 0u : static_cast<uint32_t>(-between(state, 0, 50)));
    setEnv(eIsEndAfter, 1u + rnd(state, 6));
    setArg(ctx, 0, chance(state, 80) ? kMpegG : kSplashG);
    setArg(ctx, 1, scratch + 0x200u);
    setArg(ctx, 2, rnd(state, 0x8000));
}

void mpegFrame(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    quietStream(state);
    varyPlayer(state, scratch);
    setArg(ctx, 0, chance(state, 80) ? rnd(state, 0x100) : nextFuzz(state));
}

void mpegState(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    quietStream(state);
    varyPlayer(state, scratch);
    setArg(ctx, 0, nextFuzz(state));
}

void mpegSplashTerm(R5900Context *, uint64_t &state, uint32_t scratch)
{
    quietStream(state);
    varyPlayer(state, scratch);
    heapBlock(state, kSplashBuffer, scratch + 0x10u);
}

void mpegDisplay(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    quietStream(state);
    varyPlayer(state, scratch);
    setArg(ctx, 0, chance(state, 85) ? rnd(state, 2) : nextFuzz(state));
}

void mpegSync(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    quietStream(state);
    varyPlayer(state, scratch);
    // syncDisplay never returns unless the frame is at most half the counter (or that is false only by its overflow).
    const int32_t half = load<int32_t>(kVblankCount) >> 1;
    int32_t frame = half - static_cast<int32_t>(rnd(state, 40));
    if (chance(state, 20))
        frame = half;
    setArg(ctx, 0, static_cast<uint32_t>(frame));
}

// --- RCMUSIC, MUSIC ---

namespace {

rcmusicClass &music() { return G(rcmusicVarG); }

void varyMusic(uint64_t &state)
{
    rcmusicClass &r = music();
    static const int32_t kStates[] = {0, 1, 2, 2, 1, 3};
    r.cdState = chance(state, 90) ? kStates[rnd(state, 6)] : static_cast<int32_t>(nextFuzz(state));
    r.file = chance(state, 85) ? static_cast<int32_t>(rnd(state, 36)) : between(state, -2, 37);
    r.channel = chance(state, 80) ? between(state, -1, 9) : static_cast<int32_t>(nextFuzz(state));
    static const int32_t kFades[] = {0, 0, 1, 2, 15, -1, -2, -15, 0x7f};
    r.fade = chance(state, 90) ? kFades[rnd(state, 9)] : static_cast<int32_t>(nextFuzz(state));
    r.vol = chance(state, 80) ? static_cast<int32_t>(rnd(state, 0x4000)) - (chance(state, 10) ? 0x800 : 0) : static_cast<int32_t>(nextFuzz(state));
    r.volstore = chance(state, 80) ? rnd(state, 0x800) : nextFuzz(state);
    r.userVol = chance(state, 80) ? static_cast<int32_t>(rnd(state, 0x4000)) - (chance(state, 10) ? 0x4000 : 0) : static_cast<int32_t>(nextFuzz(state));
    r.oldUserVol = static_cast<int32_t>(nextFuzz(state));
    static const int8 kModes[] = {0, 0, 1, 2, 3};
    r.chainMode = chance(state, 90) ? kModes[rnd(state, 5)] : static_cast<int8>(nextFuzz(state));
    static const int8 kTargets[] = {100, 101, 102, 103, 104, 0x69, 1, 17, 0, 50};
    r.chainTo = chance(state, 80) ? kTargets[rnd(state, 10)] : static_cast<int8>(nextFuzz(state));
    do
        r.chainRate = static_cast<int8>(chance(state, 80) ? between(state, 1, 30) : static_cast<int32_t>(nextFuzz(state)));
    while (r.chainRate == 0); // the chain divides by it
    r.spawnDuringStart = static_cast<int8>(chance(state, 70) ? 0 : chance(state, 80) ? 1 : nextFuzz(state));
    r.preStart = chance(state, 80) ? 0 : chance(state, 50) ? 1 : static_cast<int32_t>(nextFuzz(state));
    r.alternate = chance(state, 80) ? static_cast<int32_t>(rnd(state, 2)) : static_cast<int32_t>(nextFuzz(state));
    for (int i = 0; i < 36; ++i)
    {
        if (chance(state, 30))
        {
            r.specialSizeRatio[i] = static_cast<int32_t>(nextFuzz(state) >> rnd(state, 24));
            r.fileSizes[i] = static_cast<int32_t>(chance(state, 80) ? rnd(state, 0x2000000) : nextFuzz(state));
            r.fileListCD[i] = static_cast<int32_t>(rnd(state, 0x50000));
        }
    }
    for (int i = 0; i < 100; ++i)
        if (chance(state, 10))
            r.levels[i] = static_cast<uint8>(chance(state, 30) ? 0 : chance(state, 80) ? rnd(state, 36) : nextFuzz(state));
    store<int32_t>(kFrontFade, chance(state, 80) ? static_cast<int32_t>(rnd(state, 30)) : static_cast<int32_t>(nextFuzz(state)));
    if (chance(state, 20)) // the front-end track fading in from silence, near the end of its 26-frame wait
    {
        r.file = 0;
        r.vol = 0;
        r.fade = between(state, -3, -1);
        store<int32_t>(kFrontFade, between(state, 23, 26));
    }
}

} // namespace

void rcLists(R5900Context *, uint64_t &state, uint32_t)
{
    varyMusic(state);
}

void rcLevel(R5900Context *ctx, uint64_t &state, uint32_t)
{
    varyMusic(state);
    setArg(ctx, 0, static_cast<uint32_t>(chance(state, 90) ? between(state, -12, 110) : between(state, -0x260, 0x60)));
}

void rcMusic(R5900Context *ctx, uint64_t &state, uint32_t)
{
    quietStream(state);
    varyMusic(state);
    static const int32_t kArgs[] = {0, 1, 2, 0xa, 0xf, 0x19, 0x7f, -1, 5, 6, 7, 0x60, 0x61, 0x62, 0x64, 0x69, 0x6a};
    setArg(ctx, 0, static_cast<uint32_t>(chance(state, 80) ? kArgs[rnd(state, 17)] : between(state, -5, 120)));
    // Caller registers some functions pass on (RCMusicPlayTrackEdit and RCMusicPlayLevel out of their tables).
    setArg(ctx, 1, static_cast<uint32_t>(chance(state, 85) ? between(state, 0, 35) : between(state, -2, 40)));
    setArg(ctx, 2, chance(state, 80) ? rnd(state, 3) : nextFuzz(state));
}

void rcChain(R5900Context *ctx, uint64_t &state, uint32_t)
{
    quietStream(state);
    varyMusic(state);
    static const int32_t kTargets[] = {100, 101, 102, 103, 104, 0x69, 1, 17, 0, 50, 99, 105};
    setArg(ctx, 0, static_cast<uint32_t>(chance(state, 85) ? kTargets[rnd(state, 12)] : between(state, 0, 120)));
    int32_t rate;
    do
        rate = chance(state, 80) ? between(state, 1, 30) : static_cast<int8_t>(nextFuzz(state));
    while (static_cast<int8_t>(rate) == 0);
    setArg(ctx, 1, static_cast<uint32_t>(rate));
}

void rcVolume(R5900Context *ctx, uint64_t &state, uint32_t)
{
    quietStream(state);
    varyMusic(state);
    int32_t fade;
    do
        fade = chance(state, 80) ? between(state, -20, 20) : static_cast<int32_t>(nextFuzz(state));
    while (fade == 0);
    setArg(ctx, 0, static_cast<uint32_t>(fade));
    setArg(ctx, 1, chance(state, 85) ? rnd(state, 3) : nextFuzz(state));
}

void rcInit(R5900Context *ctx, uint64_t &state, uint32_t)
{
    quietStream(state);
    varyMusic(state);
    setEnv(eSearchFail, rnd(state, 3)); // a failure or two, then found (one search per file)
    store<uint32_t>(kCd + 0x24u, 0u);
    setArg(ctx, 0, static_cast<uint32_t>(chance(state, 80) ? 36 : between(state, -1, 36)));
}

void musicArgs(R5900Context *ctx, uint64_t &state, uint32_t)
{
    quietStream(state);
    store<uint32_t>(kCd + 0x24u, chance(state, 50) ? 0u : nextFuzz(state));
    for (int i = 0; i < 3; ++i)
        setArg(ctx, i, chance(state, 30) ? rnd(state, 4) : chance(state, 50) ? nextFuzz(state) >> rnd(state, 24) : nextFuzz(state));
    if (chance(state, 30))
        setArg(ctx, 2, static_cast<uint32_t>(-between(state, 0, 0x1000))); // a negative size (MusicSwitch rounds it)
}

void musicRpc(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    quietStream(state);
    static const uint32_t kCommands[] = {0x50u, 0x90u, 0xa0u, 0xb0u, 0xc0u, 0xd0u, 0x7ff0u, 0x8000u, 0x8020u, 0x8025u,
                                         0x802fu, 0x8040u, 0x8060u, 0x8070u, 0x80e0u, 0x8100u, 0x18020u};
    const uint32_t command = chance(state, 85) ? kCommands[rnd(state, 17)] : nextFuzz(state);
    setArg(ctx, 0, command);
    setArg(ctx, 1, (command & 0xfff0u) == 0x8020u ? scratch + rnd(state, 0x100) * 16u : nextFuzz(state));
}

} // namespace fuzz
} // namespace rc
