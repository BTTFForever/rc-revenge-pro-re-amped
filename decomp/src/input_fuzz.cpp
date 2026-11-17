// Builds the random-test inputs of INPUT.c. See input_fuzz.h.
//
// The pads: offline, the setups replace the runtime's libpad stubs with a scripted pad. Its answers (a pad's state, the
// bytes a read gives, the mode it reports, whether a command is taken) come from the input's configuration and do not
// change as it is called, so the original and the decompiled run get the same answers; the real stub keeps state
// (a queued state that the next scePadGetState consumes, the main mode a command sets), which would differ between the
// two runs. Every call is logged into scratch (function, then a0-a3), so the calls, their order and their arguments (the
// registers each function reads) are compared too. Live (RC_SHADOW_FUZZ) the real stub stays; these setups are meant for the oracle.

#include "input_fuzz.h"

#include <cstring>

namespace rc::fuzz {

namespace {

constexpr uint32_t kInput = sym::inputVarG;
constexpr uint32_t kLog = kFuzzScratch + 0x7000u, kLogMax = 100u; // count word, then 5 words a call

template <typename T>
T &mem(uint32_t address) { return *static_cast<T *>(guest_host(address)); }

uint32_t rnd(uint64_t &state, uint32_t n) { return n == 0 ? 0u : nextFuzz(state) % n; }
bool chance(uint64_t &state, uint32_t percent) { return rnd(state, 100) < percent; }
void setArg(R5900Context *ctx, int index, uint32_t value) { SET_GPR_S32(ctx, 4 + index, static_cast<int32_t>(value)); }

enum PadFn : uint32_t
{
    kInit, kEnd, kOpen, kClose, kRead, kGetState, kInfoMode, kSetMainMode, kActDirect, kActAlign, kInfoPress, kEnterPress,
    kExitPress, kFnCount
};
constexpr uint32_t kPadFnAddress[kFnCount] = {sym::scePadInit, sym::scePadEnd, sym::scePadPortOpen, sym::scePadPortClose, sym::scePadRead, sym::scePadGetState, sym::scePadInfoMode,
                                              sym::scePadSetMainMode, sym::scePadSetActDirect, sym::scePadSetActAlign, sym::scePadInfoPressMode, sym::scePadEnterPressMode, sym::scePadExitPressMode};
constexpr uint32_t kPadFnArgs[kFnCount] = {1, 0, 3, 2, 3, 2, 4, 4, 3, 3, 2, 2, 2}; // the registers each one reads

struct ScriptedPad
{
    int32_t answer[kFnCount][2]; // by function and port (port & 1)
    uint8_t data[2][32];         // what a read gives
};
ScriptedPad g_pad;

template <PadFn Fn>
void scriptedCall(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    const uint32_t port = getRegU32(ctx, 4);
    const uint32_t count = mem<uint32_t>(kLog);
    if (count < kLogMax)
    {
        const uint32_t at = kLog + 4u + count * 20u;
        mem<uint32_t>(at) = Fn;
        for (uint32_t i = 0; i < 4; ++i)
            mem<uint32_t>(at + 4u + i * 4u) = i < kPadFnArgs[Fn] ? getRegU32(ctx, 4 + static_cast<int>(i)) : 0u;
        mem<uint32_t>(kLog) = count + 1u;
    }
    const int32_t answer = g_pad.answer[Fn][port & 1u];
    if constexpr (Fn == kRead)
        if (answer != 0)
            std::memcpy(guest_host(getRegU32(ctx, 6)), g_pad.data[port & 1u], 32);
    SET_GPR_S32(ctx, 2, answer);
    ctx->pc = getRegU32(ctx, 31);
}

template <std::size_t... I>
void installScripted(std::index_sequence<I...>)
{
    (g_runtime->replaceFunction(kPadFnAddress[I], &scriptedCall<static_cast<PadFn>(I)>), ...);
}

int32_t padState(uint64_t &state)
{
    static const int32_t kStates[] = {0, 1, 2, 5, 6, 6, 6, 7};
    return chance(state, 5) ? static_cast<int32_t>(nextFuzz(state)) : kStates[rnd(state, 8)];
}

// A configuration for the scripted pad (offline only) and a log emptied for this input.
void scriptPad(uint64_t &state)
{
    if (!g_offline)
        return;
    static bool installed = false;
    if (!installed)
    {
        installed = true;
        installScripted(std::make_index_sequence<kFnCount>{});
    }
    for (uint32_t fn = 0; fn < kFnCount; ++fn)
        for (int32_t &a : g_pad.answer[fn])
            a = chance(state, 25) ? 0 : chance(state, 80) ? 1 : static_cast<int32_t>(nextFuzz(state));
    for (int port = 0; port < 2; ++port)
    {
        g_pad.answer[kGetState][port] = padState(state);
        static const int32_t kModes[] = {0, 4, 7, 7, 4, 1};
        g_pad.answer[kInfoMode][port] = chance(state, 10) ? static_cast<int32_t>(nextFuzz(state)) : kModes[rnd(state, 6)];
        uint8_t *data = g_pad.data[port];
        for (int i = 0; i < 32; ++i)
            data[i] = static_cast<uint8_t>(nextFuzz(state));
        data[0] = chance(state, 85) ? 0 : data[0];
        static const uint8_t kIds[] = {0x41, 0x73, 0x79, 0x53, 0x41, 0x73, 0x79};
        data[1] = chance(state, 15) ? data[1] : kIds[rnd(state, 7)];
        if (chance(state, 30))
            data[2] = data[3] = 0xff; // nothing pressed
    }
    mem<uint32_t>(kLog) = 0;
}

// inputVarG at random, with its states, counters and flags mostly in the ranges the game gives them.
void randomInput(uint64_t &state)
{
    inputClass &v = mem<inputClass>(kInput);
    uint8_t *bytes = reinterpret_cast<uint8_t *>(&v);
    for (uint32_t i = 0; i < sizeof(inputClass); ++i)
        bytes[i] = static_cast<uint8_t>(nextFuzz(state));
    for (int pad = 0; pad < 2; ++pad)
    {
        v.padState[pad] = padState(state);
        v.send[pad] = static_cast<uint8>(chance(state, 10) ? nextFuzz(state) : rnd(state, 4));
        v.vibrationFlags[pad] = static_cast<uint8>(chance(state, 10) ? nextFuzz(state) : rnd(state, 2));
        if (chance(state, 30))
            v.userShockVals[pad].motor0 = 0;
        else if (chance(state, 70))
            v.userShockVals[pad].motor0 = static_cast<uint8>(rnd(state, 15));
        if (chance(state, 30))
            v.userShockVals[pad].motor1 = 0;
        v.signalSent[pad] = static_cast<bool_>(rnd(state, 2));
        v.pressureModeSet[pad] = static_cast<bool_>(rnd(state, 2));
    }
    const uint32_t held = rnd(state, 4);
    for (uint8 &key : v.inactiveBuffer)
        key = static_cast<uint8>((rnd(state, 4) < held ? 1u : 0u) | (nextFuzz(state) & 0xfeu));
    if (chance(state, 20))
        std::memset(v.inactiveBuffer, 0, sizeof(v.inactiveBuffer));
    for (uint8 *buffer : {v.padBuffer1, v.padBuffer2})
    {
        buffer[0] = static_cast<uint8>(chance(state, 70) ? 0 : nextFuzz(state));
        static const uint8_t kIds[] = {0x41, 0x73, 0x79, 0x53};
        if (chance(state, 80))
            buffer[1] = kIds[rnd(state, 4)];
    }
}

uint32_t key(uint64_t &state)
{
    // Junk above the byte now and then: the original masks it off.
    const uint32_t k = chance(state, 90) ? rnd(state, 32) : rnd(state, 256);
    return chance(state, 10) ? k | (nextFuzz(state) & 0xffffff00u) : k;
}

} // namespace

void inputKeys(R5900Context *ctx, uint64_t &state, uint32_t)
{
    randomInput(state);
    const uint32_t k = key(state);
    const uint32_t interest = nextFuzz(state) & 0xffu;
    const uint8_t bits = mem<inputClass>(kInput).inactiveBuffer[k & 0xff];
    setArg(ctx, 0, k);
    setArg(ctx, 1, interest);
    setArg(ctx, 2, chance(state, 60) ? (bits & interest) : nextFuzz(state) & 0xffu);
}

void inputPad(R5900Context *ctx, uint64_t &state, uint32_t)
{
    randomInput(state);
    scriptPad(state);
    // An eInputPad (the port table has 8 entries), or a controller number for the static functions.
    setArg(ctx, 0, rnd(state, 8));
    setArg(ctx, 1, nextFuzz(state) & (chance(state, 80) ? 0xffu : 0xffffffffu));
}

void inputController(R5900Context *ctx, uint64_t &state, uint32_t)
{
    randomInput(state);
    scriptPad(state);
    setArg(ctx, 0, rnd(state, 2) | (chance(state, 10) ? nextFuzz(state) & 0xffffff00u : 0u));
}

} // namespace rc::fuzz
