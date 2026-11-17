// Builds the random-test inputs of SFX.c and REVSFX.c. See sfx_fuzz.h.

#include "sfx_fuzz.h"

#include <algorithm>
#include <cstdlib>

#include "res_fuzz.h"

// The runtime's hook on stores to hardware registers (ps2_runtime.cpp), for the oracle only.
extern bool (*g_ps2HardwareStoreHook)(uint32_t address, uint32_t value);

namespace rc::fuzz {

namespace {

constexpr uint32_t kSfx = sym::sfxVarG, kRev = sym::revSfxVarG, kMain = sym::mainVarG, kCamera = sym::cameraVarG, kSbuff = sym::sbuff;
constexpr uint32_t kSifCallRpc = sym::sceSifCallRpc, kSdTransToIOP = sym::sceSdTransToIOP, kSdRemoteInit = sym::sceSdRemoteInit, kSifAllocIopHeap = sym::sceSifAllocIopHeap,
                   kSifFreeIopHeap = sym::sceSifFreeIopHeap, kSifInitIopHeap = sym::sceSifInitIopHeap, kFlushCache = sym::FlushCache, kPrintf = sym::printf;
constexpr uint32_t kHardwareStore = 1u;

// Control words of the fakes (in scratch, so each run starts from the same values and they are compared).
constexpr uint32_t kEnv = kFuzzScratch + 0x7400u;
enum : uint32_t
{
    eRpcCalls = 0x0,
    eSeed = 0x4,         // the reply script's seed
    eFailPercent = 0x8,  // how often 0x8000 and 0x80d0 (and the unchecked commands) answer -1
    eIopHeap = 0xc,      // sceSifAllocIopHeap's answer
    eTransAnswer = 0x10, // sceSdTransToIOP's
    eRemoteInit = 0x14,  // sceSdRemoteInit's
    eInitHeap = 0x18,    // sceSifInitIopHeap's
    eHash = 0x1c,        // a hash of every logged call, so calls past the log's end are compared too
    eAddressBase = 0x20, // when set, every voice starts at this address and 0x8060 answers near it plus 0x36
};
constexpr uint32_t kLog = kFuzzScratch + 0x7440u, kLogMax = 40u; // count word, then 5 words an entry

template <typename T>
T &mem(uint32_t address) { return *static_cast<T *>(guest_host(address)); }

uint32_t rnd(uint64_t &state, uint32_t n) { return n == 0 ? 0u : nextFuzz(state) % n; }
bool chance(uint64_t &state, uint32_t percent) { return rnd(state, 100) < percent; }
int32_t between(uint64_t &state, int32_t low, int32_t high) { return low + static_cast<int32_t>(rnd(state, static_cast<uint32_t>(high - low + 1))); }
void setArg(R5900Context *ctx, int index, uint32_t value) { SET_GPR_S32(ctx, 4 + index, static_cast<int32_t>(value)); }
uint32_t env(uint32_t offset) { return mem<uint32_t>(kEnv + offset); }
void setEnv(uint32_t offset, uint32_t value) { mem<uint32_t>(kEnv + offset) = value; }
uint32_t arg(R5900Context *ctx, int i) { return getRegU32(ctx, 4 + i); }

void returnWith(R5900Context *ctx, uint32_t value)
{
    SET_GPR_S32(ctx, 2, static_cast<int32_t>(value));
    ctx->pc = getRegU32(ctx, 31);
}

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
        hash = (hash ^ mem<uint8_t>(at + i)) * 16777619u;
    return hash;
}

void logEntry(uint32_t what, uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
    static const bool trace = std::getenv("RC_SFXFUZZ_TRACE") != nullptr; // each logged call on stderr
    if (trace)
        std::fprintf(stderr, "[sfx_fuzz] %x %x %x %x %x\n", what, a, b, c, d);
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
void loggedCall(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    const auto a = [ctx](int i) { return i < Args ? arg(ctx, i) : 0u; };
    logEntry(Address, a(0), a(1), a(2), a(3));
    returnWith(ctx, 0);
}

template <uint32_t Address, uint32_t Answer>
void answeredCall(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    logEntry(Address, arg(ctx, 0), 0, 0, 0);
    returnWith(ctx, env(Answer));
}

// The argument words (after sceSdRemote's header word) each sdrdrv command reads.
uint32_t usedWords(uint32_t command)
{
    switch (command)
    {
    case 0x8000: case 0x8020: case 0x8040: case 0x8060: case 0x8080: return 1;
    case 0x8010: case 0x8030: case 0x8050: case 0x8070: case 0x80f0: return 2;
    case 0x80b0: case 0x8150: return 3;
    case 0x80d0: return 5;
    default: return 6;
    }
}

// The IOP's answer to call k of this input.
uint32_t reply(uint32_t command)
{
    const uint32_t k = env(eRpcCalls);
    setEnv(eRpcCalls, k + 1u);
    const uint32_t r = mix(env(eSeed) + k * 0x9e3779b9u), r2 = mix(r ^ command);
    if (command == 0x8040u) // the voices whose sample ended
    {
        switch (r & 3u)
        {
        case 0: return 0;
        case 1: return 0xffffffu;
        case 2: return mix(r2) & mix(r2 + 1u) & 0xffffffu;
        default: return r2 & 0xffffffu;
        }
    }
    if (command == 0x8060u) // a voice's current address: anywhere, or at the boundaries SetDolbyVolume tests
    {
        if (env(eAddressBase) != 0u && (r & 1u) != 0u)
            return env(eAddressBase) + 0x35u + (r2 % 3u);
        return (r & 6u) == 0 ? r2 : (r2 % 0x1f0000u) & ~0xfu;
    }
    return r % 100u < env(eFailPercent) ? 0xffffffffu : 0u;
}

// sceSifCallRpc as sceSdRemote calls it: a1 the command, a2 the mode, a3 the buffer sent, t1 the reply's buffer.
void fakeCallRpc(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    const uint32_t command = arg(ctx, 1), mode = arg(ctx, 2), send = arg(ctx, 3), recv = arg(ctx, 5);
    if ((command & ~0xfu) == 0x8130u || (command & ~0xfu) == 0x8140u)
    {
        // The effect attributes go (or come) directly; sbuff keeps the core and the pointer, the rest is stale.
        logEntry(kSifCallRpc, command, mode, send, hashBytes(send, 0x14u));
        for (uint32_t i = 3; i <= 6; ++i)
            mem<uint32_t>(kSbuff + i * 4u) = 0;
        returnWith(ctx, 0);
        return;
    }
    const uint32_t used = usedWords(command);
    uint32_t hash = 2166136261u;
    for (uint32_t i = 1; i <= used; ++i)
        hash = (hash ^ mem<uint32_t>(kSbuff + i * 4u)) * 16777619u;
    const uint32_t answer = reply(command);
    logEntry(kSifCallRpc, command, mode, hash, answer);
    for (uint32_t i = used + 1u; i <= 6u; ++i)
        mem<uint32_t>(kSbuff + i * 4u) = 0;
    if (recv != 0u)
    {
        mem<uint32_t>(recv) = answer;
        for (uint32_t i = 1; i < 4u; ++i)
            mem<uint32_t>(recv + i * 4u) = 0;
    }
    returnWith(ctx, 0);
}

void fakeTransToIop(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    const uint32_t from = arg(ctx, 0), size = arg(ctx, 2);
    logEntry(kSdTransToIOP, from, arg(ctx, 1), size, hashBytes(from, std::min(size, 0x10000u)) ^ arg(ctx, 3));
    returnWith(ctx, env(eTransAnswer));
}

void fakeInitHeap(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    logEntry(kSifInitIopHeap, 0, 0, 0, 0);
    returnWith(ctx, env(eInitHeap));
}

void fakeRemoteInit(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    logEntry(kSdRemoteInit, 0, 0, 0, 0);
    returnWith(ctx, env(eRemoteInit));
}

bool loggedStore(uint32_t address, uint32_t value)
{
    logEntry(kHardwareStore, address, value, 0, 0);
    return true; // dropped
}

// Offline: installs the fakes (once) and resets their control words: replies seeded at random, a few failures, the
// IOP heap answering, timer 0 stopped.
void quietSound(uint64_t &state)
{
    if (!g_offline)
        return;
    static bool installed = false;
    if (!installed)
    {
        installed = true;
        PS2Runtime &rt = *g_runtime;
        rt.replaceFunction(kSifCallRpc, &fakeCallRpc);
        rt.replaceFunction(kSdTransToIOP, &fakeTransToIop);
        rt.replaceFunction(kSifInitIopHeap, &fakeInitHeap);
        rt.replaceFunction(kSdRemoteInit, &fakeRemoteInit);
        rt.replaceFunction(kSifAllocIopHeap, &answeredCall<kSifAllocIopHeap, eIopHeap>);
        rt.replaceFunction(kSifFreeIopHeap, &loggedCall<kSifFreeIopHeap, 1>);
        rt.replaceFunction(kFlushCache, &loggedCall<kFlushCache, 1>);
        rt.replaceFunction(kPrintf, &loggedCall<kPrintf, 2>);
        g_ps2HardwareStoreHook = &loggedStore;
    }
    for (uint32_t offset = 0; offset < 0x40u; offset += 4u)
        setEnv(offset, 0);
    mem<uint32_t>(kLog) = 0;
    setEnv(eSeed, nextFuzz(state));
    setEnv(eFailPercent, chance(state, 70) ? 0u : rnd(state, 60));
    setEnv(eIopHeap, chance(state, 85) ? 0xa0000u + 0x40u * rnd(state, 0x800) : 0u);
    setEnv(eTransAnswer, chance(state, 85) ? 0u : chance(state, 50) ? 0xffffffffu : nextFuzz(state));
    setEnv(eRemoteInit, chance(state, 80) ? 0u : chance(state, 50) ? 0xffffffffu : nextFuzz(state));
    setEnv(eInitHeap, chance(state, 80) ? 0u : chance(state, 50) ? 0xffffffffu : nextFuzz(state));
    g_runtime->memory().write32(0x10000010u, 0);
    g_runtime->memory().write32(0x10000000u, rnd(state, 0x10000));
}

// --- The sound state. ---

sfxClass &sfx() { return G(sfxVarG); }
VoiceStruct &voice(int32_t v) { return mem<VoiceStruct>(kSfx + 0x1268u + static_cast<uint32_t>(v) * 0x18u); }
PlayStruct &play(int32_t p) { return mem<PlayStruct>(kSfx + 0x16e8u + static_cast<uint32_t>(p) * 0x14u); }
EffectStruct &effect(int32_t e) { return mem<EffectStruct>(kSfx + 0xe8u + static_cast<uint32_t>(e) * 0x20u); }

int8 voiceIndex(uint64_t &state)
{
    if (chance(state, 25))
        return -1;
    if (chance(state, 5))
        return static_cast<int8>(nextFuzz(state));
    return static_cast<int8>(rnd(state, 0x30));
}

int16 playIndex(uint64_t &state) { return static_cast<int16>(chance(state, 15) ? -1 : between(state, -2, 0x31)); }

int16 volumeWord(uint64_t &state)
{
    if (chance(state, 15))
        return 0;
    if (chance(state, 15))
        return static_cast<int16>(nextFuzz(state));
    return static_cast<int16>(rnd(state, 0x4000));
}

// The dump's voices and plays, some of them varied: flags, delays, links (consistent or not), volumes, pitches.
void varyVoices(uint64_t &state)
{
    sfxClass &s = sfx();
    const uint32_t percent = chance(state, 30) ? 0u : chance(state, 50) ? 20u : 70u;
    for (int32_t v = 0; v < 0x30; ++v)
    {
        if (!chance(state, percent))
            continue;
        VoiceStruct &vc = voice(v);
        static const int16 kFlags[] = {0, 1, 3, 5, 7, 2, 4, 1};
        vc.wFlags = kFlags[rnd(state, 8)];
        static const int16 kDelays[] = {0, 0, 0, 1, 1, 2, 3, -1};
        vc.wDelay = chance(state, 95) ? kDelays[rnd(state, 8)] : static_cast<int16>(nextFuzz(state));
        vc.wPlayId = playIndex(state);
        vc.noClickCount = static_cast<uint8>(chance(state, 60) ? 0 : chance(state, 90) ? rnd(state, 6) : nextFuzz(state));
        vc.bVolChanged = static_cast<uint8>(chance(state, 50) ? 0 : chance(state, 90) ? 1 : nextFuzz(state));
        vc.bPitchChanged = static_cast<uint8>(chance(state, 50) ? 0 : 1);
        vc.bStopNow = static_cast<uint8>(chance(state, 70) ? 0 : 1);
        vc.wLeftVol = volumeWord(state);
        vc.wRightVol = chance(state, 20) ? vc.wLeftVol : volumeWord(state);
        vc.wPitch = static_cast<int16>(chance(state, 50) ? between(state, 0x80, 0x200) : static_cast<int32_t>(nextFuzz(state)));
        static const int32_t kAddresses[] = {0xc2310, 0xc37d0, 0x5000, 0x10bb50};
        vc.dwStoreAddress = chance(state, 15) ? kAddresses[rnd(state, 4)] : static_cast<int32_t>(rnd(state, 0x200000));
        vc.dwStoreLoopAddress = chance(state, 30) ? 0 : static_cast<int32_t>(rnd(state, 0x200000));
    }
    for (int32_t p = 0; p < 0x30; ++p)
    {
        if (!chance(state, percent))
            continue;
        PlayStruct &pl = play(p);
        static const int16 kControl[] = {0, 0, 1, 2, 3, 3, 5, 7, 4, 6};
        pl.wControlFlags = kControl[rnd(state, 10)];
        pl.wEffectFlags = static_cast<int16>(chance(state, 50) ? 0x44 : chance(state, 50) ? 0 : static_cast<int32_t>(nextFuzz(state)));
        pl.bVoice1 = voiceIndex(state);
        pl.bVoice2 = chance(state, 50) ? -1 : voiceIndex(state);
        pl.bVol = static_cast<uint8>(nextFuzz(state));
        pl.bLRPan = static_cast<uint8>(chance(state, 85) ? rnd(state, 0x1f) : nextFuzz(state));
        pl.bFBPan = static_cast<uint8>(chance(state, 85) ? rnd(state, 0x1f) : nextFuzz(state));
        pl.wPitch = static_cast<int16>(chance(state, 50) ? 0x100 : static_cast<int32_t>(nextFuzz(state)));
        pl.wChainId = static_cast<int16>(between(state, -1, 0x2f));
        pl.dwMulti = chance(state, 60) ? 0x800 : chance(state, 50) ? 0x400 : static_cast<int32_t>(nextFuzz(state) >> rnd(state, 20));
        // Mostly linked both ways, as the game keeps them.
        if (chance(state, 70) && pl.bVoice1 >= 0 && pl.bVoice1 < 0x30)
            voice(pl.bVoice1).wPlayId = static_cast<int16>(p);
        if (chance(state, 70) && pl.bVoice2 >= 0 && pl.bVoice2 < 0x30)
            voice(pl.bVoice2).wPlayId = static_cast<int16>(p);
    }
    if (chance(state, 25)) // every voice starting at one address (the fake's 0x8060 answers near it)
    {
        const uint32_t base = 0x5000u + 0x10u * rnd(state, 0x10000);
        setEnv(eAddressBase, base);
        for (int32_t v = 0; v < 0x30; ++v)
            voice(v).dwStoreAddress = static_cast<int32_t>(base);
    }
    if (chance(state, 15)) // the first free voice or play anywhere, the last ones included
    {
        const int32_t first = between(state, 0x10, 0x2f);
        for (int32_t v = 0; v < 0x30; ++v)
        {
            voice(v).wFlags = static_cast<int16>(v < first ? voice(v).wFlags | 1 : voice(v).wFlags & ~1);
            play(v).wControlFlags = static_cast<int16>(v < first ? play(v).wControlFlags | 1 : play(v).wControlFlags & ~1);
        }
    }
    else if (chance(state, 15)) // every voice or play busy
    {
        for (int32_t v = 0; v < 0x30; ++v)
            voice(v).wFlags = static_cast<int16>(voice(v).wFlags | 1);
        if (chance(state, 50))
            for (int32_t p = 0; p < 0x30; ++p)
                play(p).wControlFlags = static_cast<int16>(play(p).wControlFlags | 1);
    }
    if (chance(state, 15)) // the pause store equal to the voices (RevSfxPauseFade's skip test)
        for (int32_t v = 0; v < 0x30; ++v)
        {
            s.pwVolumeStore[2 * v] = voice(v).wLeftVol;
            s.pwVolumeStore[2 * v + 1] = chance(state, 50) ? static_cast<int16>(voice(v).wLeftVol + 1) : voice(v).wRightVol;
        }
    else if (chance(state, 50))
        for (int32_t i = 0; i < 0x60; ++i)
            s.pwVolumeStore[i] = volumeWord(state);
}

// Effects: the dump's (a level's VAGs), with some loops, rates and sizes varied.
void varyEffects(uint64_t &state)
{
    if (chance(state, 50))
        return;
    for (int32_t e = 0; e < 0x8c; ++e)
    {
        if (!chance(state, 10))
            continue;
        EffectStruct &ef = effect(e);
        ef.dwLoop = chance(state, 50) ? 0 : 1;
        ef.dwMulti = chance(state, 60) ? 0x800 : chance(state, 50) ? 0x400 : static_cast<int32_t>(nextFuzz(state) >> rnd(state, 20));
        ef.dwSize = chance(state, 20) ? 0 : static_cast<int32_t>(rnd(state, 0x20000));
        ef.dwLoopAddr = chance(state, 30) ? 0 : static_cast<int32_t>(rnd(state, 0x200000));
    }
}

void varySound(uint64_t &state)
{
    sfxClass &s = sfx();
    s.bInitOk = static_cast<uint8>(chance(state, 92) ? 1 : 0);
    s.bSfxOn = static_cast<uint8>(chance(state, 92) ? 1 : 0);
    s.mode = chance(state, 95) ? static_cast<int32_t>(rnd(state, 3)) : static_cast<int32_t>(nextFuzz(state));
    s.bMasterVol = static_cast<uint8>(chance(state, 40) ? 0xc4 : chance(state, 30) ? 0xff : nextFuzz(state));
    s.masterVolChanged = static_cast<int16>(chance(state, 75) ? 0 : chance(state, 80) ? 1 : static_cast<int32_t>(nextFuzz(state)));
    s.dontStopSfxVoiceId = static_cast<int16>(chance(state, 70) ? -1 : chance(state, 80) ? between(state, 0, 0x2f) : between(state, -40, 80));
    varyVoices(state);
    varyEffects(state);
}

// A VAG in scratch: big-endian size and rate, data blocks with now and then a loop-start flag, and after the data's
// end (at 0x21 past the size) a non-looping end of either type, nearly one, or anything.
uint32_t buildVag(uint64_t &state, uint32_t at, uint32_t size)
{
    const auto big = [](uint32_t v) { return (v >> 24) | (v << 24) | ((v & 0xff00u) << 8) | ((v >> 8) & 0xff00u); };
    for (uint32_t i = 0; i < 0x30u + size + 0x40u; i += 4)
        mem<uint32_t>(at + i) = nextFuzz(state);
    mem<uint32_t>(at + 0xcu) = big(size);
    static const uint32_t kRates[] = {22050u, 44100u, 11025u, 32000u};
    mem<uint32_t>(at + 0x10u) = big(chance(state, 90) ? kRates[rnd(state, 4)] : nextFuzz(state));
    const uint32_t blocks = size / 16u;
    const uint32_t loopBlock = chance(state, 40) ? 0xffffffffu : rnd(state, blocks + 2u);
    for (uint32_t b = 0; b < blocks + 4u; ++b)
    {
        uint8_t &flag = mem<uint8_t>(at + 0x30u + b * 16u + 1u);
        flag = static_cast<uint8_t>(b == loopBlock ? flag | 4u : flag & ~4u);
    }
    const uint32_t tail = at + size + 0x21u;
    const uint32_t kind = rnd(state, 4);
    for (uint32_t i = 0; i < 15u; ++i)
    {
        if (kind == 0)
            mem<uint8_t>(tail + i) = static_cast<uint8_t>(i == 0 ? 7 : 0);
        else if (kind == 1)
            mem<uint8_t>(tail + i) = static_cast<uint8_t>(i == 0 ? 0 : 0x77);
    }
    if (kind < 2 && chance(state, 25))
        mem<uint8_t>(tail + rnd(state, 15)) ^= static_cast<uint8_t>(1u << rnd(state, 8));
    return at;
}

// --- The ambient sounds. ---

uint32_t ambientAt(int32_t i) { return kRev + static_cast<uint32_t>(i) * 0x24u; }
sAmbientSound &ambient(int32_t i) { return mem<sAmbientSound>(ambientAt(i)); }
revsfxClass &rev() { return G(revSfxVarG); }
int32_t now() { return mem<int32_t>(kMain + 0x1cu); }

// The game neither paused nor over, mostly.
void runningGame(uint64_t &state)
{
    mem<uint16_t>(kMain + 0x366u) = static_cast<uint16_t>(chance(state, 90) ? 0 : chance(state, 50) ? 1 : nextFuzz(state));
    mem<int32_t>(kMain) = chance(state, 90) ? mem<int32_t>(kMain) == 4 ? 1 : mem<int32_t>(kMain) : chance(state, 50) ? 4 : static_cast<int32_t>(rnd(state, 8));
    mem<uint32_t>(kMain + 0x174u) = chance(state, 90) ? rnd(state, 2) : chance(state, 50) ? 2u : nextFuzz(state);
    mem<uint8_t>(kMain + 0x368u) = static_cast<uint8_t>(chance(state, 70) ? 0 : 1); // split screen
}

// A place near the camera (within earshot half the time), or anywhere.
void placeNear(uint64_t &state, uint32_t at, uint32_t camera, int32_t range)
{
    for (uint32_t axis = 0; axis < 3; ++axis)
    {
        const int32_t centre = mem<int32_t>(camera + 0xcu + axis * 4u);
        const int32_t spread = chance(state, 50) ? std::max(1, std::abs(range)) * 256 : 0x1000000;
        mem<int32_t>(at + axis * 4u) =
            chance(state, 5) ? static_cast<int32_t>(nextFuzz(state)) : centre + between(state, -spread, spread);
    }
    if (chance(state, 5)) // at the camera: no direction
        for (uint32_t axis = 0; axis < 3; ++axis)
            mem<int32_t>(at + axis * 4u) = mem<int32_t>(camera + 0xcu + axis * 4u) + between(state, -255, 255);
}

int16 ambientRange(uint64_t &state)
{
    return static_cast<int16>(chance(state, 80) ? between(state, 1, 0x3000) : chance(state, 70) ? between(state, -0x3000, -1)
                                                                                                 : static_cast<int32_t>(nextFuzz(state) | 1u));
}

// Varies the ambient list: types (every handled one), timers due or not, intervals, pitch variances, handles, Dolby.
// Intervals and variances are never ones the original divides by zero with.
void varyAmbients(uint64_t &state)
{
    revsfxClass &r = rev();
    if (chance(state, 30) || r.ambientActualCt <= 0 || r.ambientActualCt > 100)
        r.ambientActualCt = static_cast<int32_t>(1u + rnd(state, chance(state, 50) ? 8u : 100u));
    const int32_t time = now();
    const uint32_t percent = chance(state, 30) ? 30u : 80u;
    for (int32_t i = 0; i < r.ambientActualCt; ++i)
    {
        if (!chance(state, percent))
            continue;
        sAmbientSound &a = ambient(i);
        static const int32_t kTypes[] = {0, 0, 1, 1, 2, 3, 5, 6, 7, 8, 9, 10, 4, 11, 0x96, -1};
        a.type = chance(state, 95) ? kTypes[rnd(state, 16)] : static_cast<int32_t>(nextFuzz(state));
        a.timer = chance(state, 20) ? 0 : chance(state, 50) ? time - between(state, 1, 5000) : time + between(state, 0, 5000);
        a.randInterval = static_cast<int16>(chance(state, 20) ? 0 : between(state, 1, 0x800));
        if (a.type == 1 || a.type == 7 || a.type == 8)
            a.randInterval = static_cast<int16>(between(state, 1, 0x800));
        static const int16 kVariances[] = {0, 0x100, 0x101, 0x40, 0x80, 0x81, 0x200, -0x40, 2, 3};
        a.pitchVariance = chance(state, 70) ? kVariances[rnd(state, 10)] : static_cast<int16>(nextFuzz(state));
        if (a.type == 0 && a.randInterval != 0 && (a.pitchVariance >> 1) == 0)
            a.pitchVariance = 0x100;
        a.handle[0] = static_cast<int16>(chance(state, 50) ? -1 : between(state, 0, 0x2f));
        a.handle[1] = static_cast<int16>(chance(state, 50) ? -1 : between(state, 0, 0x2f));
        a.sample = static_cast<int16>(chance(state, 95) ? between(state, 0, 0x8b) : static_cast<int32_t>(nextFuzz(state) & 0x7fu));
        a.range = ambientRange(state);
        a.dolby = chance(state, 60) ? 0 : chance(state, 90) ? 0x40 : static_cast<int32_t>(nextFuzz(state));
        if (chance(state, 60))
            placeNear(state, ambientAt(i), kCamera, a.range);
    }
}

} // namespace

// --- SFX ---

void sfxInit(R5900Context *ctx, uint64_t &state, uint32_t)
{
    quietSound(state);
    varySound(state);
    setArg(ctx, 0, chance(state, 90) ? rnd(state, 3) : nextFuzz(state));
}

void sfxIop(R5900Context *, uint64_t &state, uint32_t)
{
    quietSound(state);
    varySound(state);
}

void sfxReverb(R5900Context *, uint64_t &state, uint32_t)
{
    quietSound(state);
    varySound(state);
    sfx().bReverbInit = static_cast<uint8>(chance(state, 70) ? 0 : 1);
    sfx().bReverbDepth = static_cast<uint8>(nextFuzz(state));
}

void sfxReverbDepth(R5900Context *ctx, uint64_t &state, uint32_t)
{
    quietSound(state);
    varySound(state);
    sfx().bReverbInit = static_cast<uint8>(chance(state, 85) ? 1 : 0);
    sfx().bReverbOn = static_cast<uint8>(chance(state, 85) ? 1 : 0);
    static const uint32_t kDepths[] = {0, 0x28, 0x46, 0x50, 0x7f, 0x80, 0xff};
    setArg(ctx, 0, chance(state, 60) ? kDepths[rnd(state, 7)] : nextFuzz(state));
}

void sfxTransEffect(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    quietSound(state);
    varySound(state);
    // Free effect slots: the dump's, the first one, a gap, or none (-3).
    const uint32_t kind = rnd(state, 5);
    if (kind == 1)
        effect(0).dwSize = 0;
    else if (kind == 2)
        effect(static_cast<int32_t>(rnd(state, 0x8c))).dwSize = 0;
    else if (kind == 3)
        for (int32_t e = 0; e < 0x8c; ++e)
            effect(e).dwSize = static_cast<int32_t>(1u + rnd(state, 0x8000));
    sfx().SPUCurrentAddr = chance(state, 50) ? sfx().SPUCurrentAddr : static_cast<int32_t>(rnd(state, 0x200000));
    const uint32_t size = chance(state, 80) ? 16u * (1u + rnd(state, 0x200)) : 1u + rnd(state, 0x2000);
    setArg(ctx, 0, buildVag(state, scratch + 0x100u, size));
    setArg(ctx, 1, chance(state, 80) ? 0u : nextFuzz(state));
    setArg(ctx, 2, nextFuzz(state));
}

void sfxVoices(R5900Context *, uint64_t &state, uint32_t)
{
    quietSound(state);
    varySound(state);
}

void sfxPlay(R5900Context *ctx, uint64_t &state, uint32_t)
{
    quietSound(state);
    varySound(state);
    setArg(ctx, 0, static_cast<uint32_t>(chance(state, 90) ? between(state, 0, 0x8b) : static_cast<int32_t>(static_cast<int16_t>(nextFuzz(state) & 0x3ff))));
    setArg(ctx, 1, nextFuzz(state) & 0xffu);
    setArg(ctx, 2, chance(state, 85) ? rnd(state, 0x1f) : nextFuzz(state) & 0xffu);
    setArg(ctx, 3, chance(state, 85) ? rnd(state, 0x1f) : nextFuzz(state) & 0xffu);
    setArg(ctx, 4, static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(chance(state, 50) ? 0x100 : nextFuzz(state)))));
    setArg(ctx, 5, static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(chance(state, 50) ? 0x44 : chance(state, 50) ? 4 : nextFuzz(state)))));
    setArg(ctx, 6, static_cast<uint32_t>(chance(state, 60) ? 0 : between(state, -2, 0x31)));
}

void sfxPlayId(R5900Context *ctx, uint64_t &state, uint32_t)
{
    quietSound(state);
    varySound(state);
    setArg(ctx, 0, static_cast<uint32_t>(static_cast<int32_t>(playIndex(state))));
}

void sfxPlayIdByte(R5900Context *ctx, uint64_t &state, uint32_t)
{
    quietSound(state);
    varySound(state);
    const int16 id = playIndex(state);
    setArg(ctx, 0, static_cast<uint32_t>(static_cast<int32_t>(id)));
    if (id >= 0 && id < 0x30 && chance(state, 25))
    {
        // The value the play has already (no change): each setter compares with its own field.
        const PlayStruct &p = play(id);
        static const uint32_t kFields[] = {0, 1, 2, 3};
        switch (kFields[rnd(state, 4)])
        {
        case 0: setArg(ctx, 1, p.bLRPan); break;
        case 1: setArg(ctx, 1, p.bFBPan); break;
        case 2: setArg(ctx, 1, p.bVol); break;
        default: setArg(ctx, 1, static_cast<uint32_t>(static_cast<int32_t>(p.wPitch))); break;
        }
        return;
    }
    setArg(ctx, 1, chance(state, 50) ? rnd(state, 0x1f) : static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(nextFuzz(state)))));
}

void sfxVagDaemon(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    sfxTransEffect(ctx, state, scratch);
    mem<uint32_t>(scratch) = arg(ctx, 0);
    G(resVarG).binaryHeader.id = nextFuzz(state);
    setArg(ctx, 0, scratch);
}

void sfxVagTail(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    const uint32_t size = chance(state, 70) ? 16u * rnd(state, 0x100) : rnd(state, 0x1000);
    setArg(ctx, 0, buildVag(state, scratch + 0x100u, size));
    setArg(ctx, 1, size);
}

void sfxByte(R5900Context *ctx, uint64_t &state, uint32_t)
{
    quietSound(state);
    varySound(state);
    setArg(ctx, 0, chance(state, 60) ? rnd(state, 3) : nextFuzz(state));
}

void sfxPause(R5900Context *ctx, uint64_t &state, uint32_t)
{
    quietSound(state);
    varySound(state);
    setArg(ctx, 0, chance(state, 80) ? rnd(state, 3) : nextFuzz(state));
}

void sfxStoreAddress(R5900Context *ctx, uint64_t &state, uint32_t)
{
    varySound(state);
    setArg(ctx, 0, static_cast<uint32_t>(between(state, -2, 0x8d)));
    setArg(ctx, 1, chance(state, 50) ? 0u : chance(state, 60) ? 1u : nextFuzz(state));
}

void sfxClearSram(R5900Context *ctx, uint64_t &state, uint32_t)
{
    varySound(state);
    sfx().lastAmbientIndex = static_cast<int16>(between(state, -3, 0x8b));
    sfx().endOfGenericAddr = static_cast<int32_t>(nextFuzz(state) >> rnd(state, 12));
    const int32_t no = chance(state, 70) ? between(state, std::max(-5, sfx().lastAmbientIndex - 40), sfx().lastAmbientIndex + 3)
                                         : between(state, -5, 0x8c);
    setArg(ctx, 0, static_cast<uint32_t>(chance(state, 10) ? no + 0x10000 : no)); // the high half is ignored
}

void sfxVoiceId(R5900Context *ctx, uint64_t &state, uint32_t)
{
    quietSound(state);
    varySound(state);
    setArg(ctx, 0, static_cast<uint32_t>(chance(state, 90) ? between(state, -3, 0x32) : static_cast<int32_t>(nextFuzz(state))));
}

void sfxLoopAddress(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    varySound(state);
    const int32_t id = chance(state, 95) ? between(state, 0, 0x8b) : between(state, -2, 0x8d);
    EffectStruct &ef = effect(id);
    const uint32_t size = chance(state, 80) ? 16u * rnd(state, 0x200) : rnd(state, 0x2000);
    ef.dwSize = chance(state, 10) ? -static_cast<int32_t>(rnd(state, 100)) : static_cast<int32_t>(size);
    ef.dwLoop = chance(state, 70) ? 1 : 0;
    ef.dwAddr = static_cast<int32_t>(rnd(state, 0x200000));
    setArg(ctx, 0, static_cast<uint32_t>(id));
    setArg(ctx, 1, buildVag(state, scratch + 0x100u, size));
}

// --- REVSFX ---

void revPlay3D(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    quietSound(state);
    varySound(state);
    runningGame(state);
    int32_t flags = 0;
    static const int32_t kBits[] = {1, 4, 8, 0x10, 0x40, 0x80, 0x100, 0x200};
    for (const int32_t bit : kBits)
        if (chance(state, bit == 4 || bit == 8 ? 40 : 25))
            flags |= bit;
    if (chance(state, 5))
        flags = static_cast<int16_t>(nextFuzz(state));
    if (chance(state, 40)) // Dolby: the front/back pan is computed
    {
        sfx().mode = 2;
        flags |= 0x40;
        mem<uint8_t>(kMain + 0x368u) = 0;
    }
    if (chance(state, 60)) // a second camera near the first (the dumps' is all zero)
    {
        for (uint32_t i = 0; i < 0x40u; i += 4)
            mem<uint32_t>(kCamera + 0x5b8u + i) = mem<uint32_t>(kCamera + i);
        for (uint32_t axis = 0; axis < 3; ++axis)
            mem<int32_t>(kCamera + 0x5b8u + 0xcu + axis * 4u) += between(state, -0x200000, 0x200000);
        for (uint32_t k = 0; k < 9; ++k)
            if (chance(state, 30))
                mem<int16_t>(kCamera + 0x5b8u + 0x24u + k * 2u) = static_cast<int16_t>(between(state, -4096, 4096));
    }
    int16 range = ambientRange(state);
    if (range == 0 || (range < 0 && chance(state, 50)))
        range = 0x1194;
    const uint32_t pos = scratch + 0x40u;
    placeNear(state, pos, (flags & 1) != 0 ? (sym::cameraVarG + 0x5b8u) : kCamera, range);
    setArg(ctx, 0, static_cast<uint32_t>((flags & 4) != 0 ? between(state, 0, 0x8b) : static_cast<int32_t>(playIndex(state))));
    setArg(ctx, 1, static_cast<uint32_t>(static_cast<int32_t>(chance(state, 80) ? static_cast<int16>(between(state, 0, 0x100))
                                                                                 : static_cast<int16>(nextFuzz(state)))));
    setArg(ctx, 2, static_cast<uint32_t>(static_cast<int32_t>(chance(state, 50) ? 0x100 : static_cast<int16>(nextFuzz(state)))));
    setArg(ctx, 3, pos);
    setArg(ctx, 4, static_cast<uint32_t>(flags));
    setArg(ctx, 5, static_cast<uint32_t>(static_cast<int32_t>(range)));
}

void revStopAll(R5900Context *ctx, uint64_t &state, uint32_t)
{
    quietSound(state);
    varySound(state);
    varyAmbients(state);
    rev().rvbLevel = static_cast<int32_t>(nextFuzz(state));
    sfx().bReverbInit = static_cast<uint8>(chance(state, 85) ? 1 : 0);
    sfx().bReverbOn = static_cast<uint8>(chance(state, 85) ? 1 : 0);
    setArg(ctx, 0, chance(state, 40) ? 99u : chance(state, 50) ? rnd(state, 3) : nextFuzz(state));
}

void revInit(R5900Context *, uint64_t &state, uint32_t)
{
    quietSound(state);
    varySound(state);
    G(sysTimerVarG).tickTime = nextFuzz(state) >> rnd(state, 12);
}

void revAmbient(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    quietSound(state);
    varySound(state);
    runningGame(state);
    varyAmbients(state);
    revsfxClass &r = rev();
    static const int32_t kFades[] = {0, 0, 0, 0xff, 1, 5, -1, -5};
    r.startFade = chance(state, 95) ? kFades[rnd(state, 8)] : static_cast<int32_t>(nextFuzz(state));
    r.startFadeMisc = chance(state, 50) ? 10000 : static_cast<int32_t>(rnd(state, 20000));
    r.startFadeRate = static_cast<int32_t>(rnd(state, 1000));
    r.rain = chance(state, 60) ? 0 : chance(state, 90) ? between(state, 1, 40) : static_cast<int32_t>(nextFuzz(state));
    G(lowHigh) = chance(state, 50) ? 0 : chance(state, 90) ? 1 : static_cast<int32_t>(nextFuzz(state));
    if (chance(state, 20))
        G(animVarG).pAnimData = nullptr;
    else if (chance(state, 50))
    {
        // Animated objects of its own, tagged as types 7 and 8 look for (or not), each pointing at a place.
        const uint32_t anim = scratch + 0x1000u, count = rnd(state, 8);
        mem<uint16_t>(anim) = static_cast<uint16_t>(count);
        static const uint32_t kTags[] = {0x1000047bu, 0x10000501u, 0x100005d4u, 0x10000500u};
        for (uint32_t k = 0; k < count + 1u; ++k)
        {
            const uint32_t entry = anim + 8u + k * 0x64u, object = scratch + 0x3000u + k * 0x20u;
            mem<uint32_t>(entry) = object;
            mem<uint16_t>(entry + 8u) = static_cast<uint16_t>(chance(state, 70) ? 0x40 : nextFuzz(state) & ~0x40u);
            mem<uint32_t>(object) = kTags[rnd(state, 4)];
            placeNear(state, object + 0x10u, kCamera, 0x1000);
        }
        G(animVarG).pAnimData = ptr32<animFile>(anim);
    }
    // Split: one screen (0), two (1), none (negative: nothing per screen is played), and now and then more (the
    // handle slots past the second are the sample, range and type words).
    static const int32_t kSplits[] = {0, 0, 0, 1, 1, -1, 2, 3};
    setArg(ctx, 0, static_cast<uint32_t>(chance(state, 97) ? kSplits[rnd(state, 8)] : static_cast<int32_t>(nextFuzz(state) | 0x80000000u)));
}

void revPauseFade(R5900Context *ctx, uint64_t &state, uint32_t)
{
    quietSound(state);
    varySound(state);
    revsfxClass &r = rev();
    r.fadeType = chance(state, 45) ? 1 : chance(state, 80) ? 2 : chance(state, 50) ? 0 : static_cast<int32_t>(nextFuzz(state));
    static const int32_t kCounts[] = {0, 1, 9, 10, 11, 0x7f, 3, -1};
    r.fadeCt = chance(state, 90) ? kCounts[rnd(state, 8)] : static_cast<int32_t>(nextFuzz(state) | 1u);
    setArg(ctx, 0, chance(state, 70) ? 0u : chance(state, 80) ? 1u + rnd(state, 3) : nextFuzz(state));
}

void revReverb(R5900Context *ctx, uint64_t &state, uint32_t)
{
    quietSound(state);
    varySound(state);
    sfx().bReverbInit = static_cast<uint8>(chance(state, 85) ? 1 : 0);
    sfx().bReverbOn = static_cast<uint8>(chance(state, 85) ? 1 : 0);
    rev().rvbLevel = chance(state, 60) ? static_cast<int32_t>(rnd(state, 0x80)) : static_cast<int32_t>(nextFuzz(state));
    mem<uint8_t>(kMain + 0x34au) = static_cast<uint8_t>(chance(state, 80) ? rnd(state, 0x50) : nextFuzz(state)); // level
    setArg(ctx, 0, chance(state, 70) ? 0u : chance(state, 50) ? 1u : nextFuzz(state));
}

// An SFX resource in scratch: a count and a type, then per VAG a place, a range (negative: Dolby), a type, an interval,
// a pitch variance and the VAG's resource id (some ids repeated). Most ids are marked loaded (their handles a sample
// number times 4, as the VAG daemon leaves them); the others load from the disc.
void revSfxDaemon(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    const bool disc = resLoadEnvironment(state, scratch);
    quietSound(state);
    varySound(state);
    revsfxClass &r = rev();
    r.genericSoundCt = chance(state, 40) ? 0 : between(state, 1, 0x60);
    sfx().lastAmbientIndex = static_cast<int16>(between(state, -1, 0x8b));
    const uint32_t header = scratch + 0x100u, count = chance(state, 10) ? 0u : 1u + rnd(state, 20);
    mem<uint32_t>(header) = count;
    mem<uint32_t>(header + 4u) = chance(state, 40) ? 0u : chance(state, 80) ? 1u : nextFuzz(state);
    const uint32_t handles = G(resVarG).handlesHandle.address;
    const uint32_t maxId = G(resVarG).lookupHeader.maxId;
    uint32_t ids[8];
    for (uint32_t &id : ids)
    {
        id = disc ? resPickId(state) : 1u + rnd(state, std::max(2u, maxId) - 1u);
        if (handles != 0u && (!disc || chance(state, 75)))
            mem<uint32_t>(handles + id * 4u) = rnd(state, 0x8c) << 2;
    }
    for (uint32_t i = 0; i < count; ++i)
    {
        const uint32_t vag = header + 8u + i * 0x1cu;
        for (uint32_t k = 0; k < 3; ++k)
            mem<int32_t>(vag + k * 4u) = static_cast<int32_t>(nextFuzz(state)) >> rnd(state, 12);
        mem<int16_t>(vag + 0xcu) = ambientRange(state);
        mem<int32_t>(vag + 0x10u) = chance(state, 90) ? static_cast<int32_t>(rnd(state, 11)) : static_cast<int32_t>(nextFuzz(state));
        mem<int16_t>(vag + 0x14u) = static_cast<int16_t>(nextFuzz(state));
        mem<int16_t>(vag + 0x16u) = static_cast<int16_t>(nextFuzz(state));
        mem<uint32_t>(vag + 0x18u) = ids[rnd(state, chance(state, 50) ? 3u : 8u)];
    }
    mem<uint32_t>(scratch) = header;
    setArg(ctx, 0, scratch);
}

// The level's SFX data in scratch (pLvlG->pSFXData pointed at it), laid out as SFXConstructDaemon reads its resource,
// with the end of the generic VAGs at one of the effects or none.
void revSetupClass(R5900Context *, uint64_t &state, uint32_t scratch)
{
    varySound(state);
    revsfxClass &r = rev();
    sfxClass &s = sfx();
    r.genericSoundCt = static_cast<int32_t>(nextFuzz(state) & 0x7fu);
    const uint32_t kind = rnd(state, 4);
    if (kind == 0)
        s.endOfGenericAddr = s.effectList[0].dwAddr;
    else if (kind == 1)
        s.endOfGenericAddr = s.effectList[rnd(state, 0x8c)].dwAddr;
    else if (kind == 2)
        s.endOfGenericAddr = static_cast<int32_t>(nextFuzz(state));
    const uint32_t data = scratch + 0x100u, count = chance(state, 10) ? 0u : 1u + rnd(state, 30);
    mem<uint32_t>(data) = count;
    mem<uint32_t>(data + 4u) = nextFuzz(state);
    for (uint32_t i = 0; i < count; ++i)
    {
        const uint32_t vag = data + 8u + i * 0x1cu;
        for (uint32_t k = 0; k < 3; ++k)
            mem<int32_t>(vag + k * 4u) = static_cast<int32_t>(nextFuzz(state)) >> rnd(state, 12);
        mem<int16_t>(vag + 0xcu) = ambientRange(state);
        mem<int32_t>(vag + 0x10u) = static_cast<int32_t>(rnd(state, 11));
        mem<int16_t>(vag + 0x14u) = static_cast<int16_t>(nextFuzz(state));
        mem<int16_t>(vag + 0x16u) = static_cast<int16_t>(nextFuzz(state));
        mem<uint32_t>(vag + 0x18u) = chance(state, 50) ? rnd(state, 4) : nextFuzz(state);
    }
    const uint32_t level = G(pLvlG).address;
    if (level != 0u && level < 0x2000000u)
        mem<uint32_t>(level + 0x24u) = data;
}

// The triggers near the game timer, held at a known time (paused) or running.
void revTrigger(R5900Context *ctx, uint64_t &state, uint32_t)
{
    quietSound(state);
    varySound(state);
    _sTimer &timer = mem<_sTimer>(kMain + 0x10u);
    if (chance(state, 80))
    {
        timer.flags = 1;
        timer.time = static_cast<int32_t>(rnd(state, 0x100000));
    }
    const int32_t time = timer.time;
    for (uint32_t i = 0; i < 24u; ++i)
    {
        sTriggerSfx &t = rev().revSfxTriggers[i];
        if (!chance(state, 60))
            continue;
        t.vagNo = static_cast<int16>(chance(state, 90) ? rnd(state, 0x2f) : nextFuzz(state) & 0x7f);
        t.repeat = static_cast<int16>(chance(state, 90) ? rnd(state, 3) : nextFuzz(state));
        t.timer = chance(state, 20) ? time - between(state, 0x31, 0x33) // the catch-up boundary
                  : chance(state, 80) ? time + between(state, -2000, 2000) : static_cast<int32_t>(nextFuzz(state));
    }
    setArg(ctx, 0, chance(state, 95) ? rnd(state, 24) : rnd(state, 0x40));
    setArg(ctx, 1, nextFuzz(state));
}

void revSpecialTrigger(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    quietSound(state);
    varySound(state);
    runningGame(state);
    varyAmbients(state);
    const int32_t num = between(state, 0, std::max(0, rev().ambientActualCt - 1));
    const uint32_t pos = scratch + 0x40u;
    placeNear(state, pos, kCamera, ambient(num).range);
    if (ambient(num).range == 0)
        ambient(num).range = 1;
    setArg(ctx, 0, static_cast<uint32_t>(num));
    setArg(ctx, 1, chance(state, 50) ? 0u : pos);
    setArg(ctx, 2, static_cast<uint32_t>(ambient(num).timer + between(state, -1000, 1000)));
}

void revAnim(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    quietSound(state);
    varySound(state);
    runningGame(state);
    const uint32_t head = scratch + 0x100u, tso = scratch + 0x200u;
    for (uint32_t i = 0; i < 0x50u; i += 4)
        mem<uint32_t>(head + i) = nextFuzz(state);
    mem<uint32_t>(head) = tso;
    placeNear(state, tso + 0x10u, kCamera, 0x1194);
    animHead &h = mem<animHead>(head);
    h.lastFrame = static_cast<uint16>(chance(state, 80) ? rnd(state, 200) : nextFuzz(state));
    h.sfxSample = static_cast<int8>(chance(state, 15) ? -1 : chance(state, 90) ? between(state, 0, 0x2f) : static_cast<int32_t>(nextFuzz(state)));
    h.sfxHandle = static_cast<int8>(chance(state, 40) ? -1 : chance(state, 30) ? 0 : static_cast<int32_t>(nextFuzz(state)));
    h.sfxMisc = static_cast<int16>(chance(state, 40) ? 1 : chance(state, 50) ? 0 : static_cast<int32_t>(nextFuzz(state)));
    h.sfxTimer = chance(state, 80) ? static_cast<int32_t>(rnd(state, 200)) : static_cast<int32_t>(nextFuzz(state));
    setArg(ctx, 0, head);
    setArg(ctx, 1, chance(state, 50) ? 2u : rnd(state, 4));
}

void revTso(R5900Context *ctx, uint64_t &state, uint32_t)
{
    quietSound(state);
    varySound(state);
    runningGame(state);
    varyAmbients(state);
    for (int32_t i = 0; i < rev().ambientActualCt; ++i)
        if (chance(state, 20))
            ambient(i).type = 2;
    int32_t id = between(state, 0, std::max(0, rev().ambientActualCt - 1));
    if (chance(state, 35))
        id |= 0x200;
    if (chance(state, 35))
        id |= 0x400;
    setArg(ctx, 0, static_cast<uint32_t>(id));
    setArg(ctx, 1, nextFuzz(state));
}

void revAmbientList(R5900Context *ctx, uint64_t &state, uint32_t)
{
    varySound(state);
    varyAmbients(state);
    revsfxClass &r = rev();
    r.ambientSoundCt = chance(state, 30) ? 0 : static_cast<int32_t>(rnd(state, 30));
    r.genericSoundCt = static_cast<int32_t>(chance(state, 70) ? 0x5d : rnd(state, 0x8c));
    sfx().lastAmbientIndex = static_cast<int16>(between(state, -1, 0x8b));
    r.playPauseSfx = chance(state, 50) ? 0 : static_cast<int32_t>(nextFuzz(state));
    r.timerPauseSfx = static_cast<int32_t>(nextFuzz(state));
    for (uint32_t i = 0; i < 24u; ++i)
        r.revSfxTriggers[i].timer = static_cast<int32_t>(nextFuzz(state));
    setArg(ctx, 0, chance(state, 50) ? 0u : chance(state, 50) ? 1u : nextFuzz(state));
}

void revModifyVolume(R5900Context *ctx, uint64_t &state, uint32_t)
{
    quietSound(state);
    varySound(state);
    revsfxClass &r = rev();
    r.userVol = chance(state, 50) ? 0xc4 : static_cast<int32_t>(nextFuzz(state));
    r.oldUserVol = chance(state, 50) ? r.userVol : static_cast<int32_t>(nextFuzz(state));
    r.playPauseSfx = chance(state, 50) ? 0 : 1;
    _sTimer &timer = r.pauseTimer;
    if (chance(state, 80))
    {
        timer.flags = 1;
        timer.time = static_cast<int32_t>(rnd(state, 0x100000));
    }
    r.timerPauseSfx = chance(state, 70) ? timer.time + between(state, -600, 600) : static_cast<int32_t>(nextFuzz(state));
    setArg(ctx, 0, chance(state, 85) ? rnd(state, 5) : nextFuzz(state));
}

void revReadyGo(R5900Context *ctx, uint64_t &state, uint32_t)
{
    int32_t frames = chance(state, 80) ? between(state, -200, 200) : static_cast<int32_t>(nextFuzz(state));
    if (frames == 0)
        frames = chance(state, 50) ? 1 : -1;
    setArg(ctx, 0, static_cast<uint32_t>(frames));
    setArg(ctx, 1, chance(state, 80) ? rnd(state, 101) : nextFuzz(state));
}

void revDolby(R5900Context *ctx, uint64_t &state, uint32_t)
{
    sfx().mode = chance(state, 90) ? static_cast<int32_t>(rnd(state, 3)) : static_cast<int32_t>(nextFuzz(state));
    rev().oldSfxMode = chance(state, 90) ? static_cast<int32_t>(rnd(state, 3)) : static_cast<int32_t>(nextFuzz(state));
    setArg(ctx, 0, chance(state, 50) ? rnd(state, 2) : nextFuzz(state));
    setArg(ctx, 1, chance(state, 60) ? 0u : chance(state, 50) ? 1u : nextFuzz(state));
}

} // namespace rc::fuzz
