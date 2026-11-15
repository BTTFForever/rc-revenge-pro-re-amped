// Connects decompiled functions to the running game.
//
// RC_DECOMP_FN(name, 0xaddress) puts rc::name in place of the recompiled function at that guest
// address. With RC_SHADOW=1 in the environment, each sampled call runs the recompiled original and
// the decompiled function on the same state and compares their results and every byte of guest RAM.
//
// A function that can block uses RC_DECOMP_FN_BLOCKING instead, and calls guest code with guest_call.
// tools/decomp_order.py lists the functions that cannot block, which are the ones that can be compared.
#pragma once

#include <atomic>
#include <cmath>
#include <cstddef>
#include <iterator>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "ps2_runtime.h"
#include "ps2_recompiled_functions.h"
#include "runtime/ee_scheduler.h"
#include "rc_guest.h"
#include "rc_globals.h"
#include "rc_types.h"

namespace rc {

using RecompFn = void (*)(uint8_t *, R5900Context *, PS2Runtime *);

enum class ReturnKind : uint8_t { None, Gpr, Float };

struct Entry
{
    const char *name;
    uint32_t address;
    RecompFn native;
    RecompFn original;
    ReturnKind returns;
    void (*randomiseArgs)(R5900Context *, uint64_t &, uint32_t); // Null if random inputs would be meaningless.
    bool fuzzWholeRam = false; // The random test compares all of guest RAM, for a function that uses the game's heap.
    bool offlineOnly = false;  // The random test runs only in the offline oracle, whose setup replaces the waits.
    std::atomic<uint64_t> calls{0};
    std::atomic<uint64_t> compared{0};
    std::atomic<uint64_t> mismatches{0};
};

void registerEntry(Entry &entry, RecompFn shadow);
void shadowCall(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, Entry &entry);

// Called once when the game is loaded.
void installDecomp(PS2Runtime &runtime);

// The random test's memory: the scratch area its inputs are built in, and the game's globals (the ELF's data and bss).
constexpr uint32_t kFuzzScratch = 0x01F00000u, kFuzzScratchSize = 0x8000u;
constexpr uint32_t kFuzzDataStart = 0x001E9400u, kFuzzDataEnd = 0x004C7190u;
constexpr int kFuzzWholeRamInputs = 40; // At most, for a function registered with RC_DECOMP_FN_SETUP_HEAP.

struct FuzzCaseResult
{
    bool cutShort = false; // The original did not return (a scheduling point or a wait); nothing was compared.
    bool same = true;
    bool returnedNonZero = false;
    bool wroteMemory = false;
};

// One random input of a function's random test, original against decompiled (rc_decomp.cpp). Used by the live test and
// by the offline oracle (decomp/oracle).
FuzzCaseResult fuzzCase(Entry &entry, uint8_t *rdram, const R5900Context &live, PS2Runtime *runtime, uint64_t &state,
                        const uint8_t *ramBackup, int iteration, uint64_t mismatchesSoFar);

// Every registered function, in registration order.
std::vector<Entry *> registeredEntries();

// For the offline oracle: installs every function as under RC_SHADOW=1 (nested calls inside a comparison reach the
// originals), without the log thread, and allocates the comparison's buffers.
void installForOracle(PS2Runtime &runtime);
extern bool g_offline; // Set by installForOracle: running in the offline oracle, not the game.

// The decompiled version of the function at an address, or fallback if it has none or decompilation is off.
RecompFn decompOr(uint32_t address, RecompFn fallback);

extern PS2Runtime *g_runtime;

// Advances the EE's clock without yielding, as a call or a loop iteration does in recompiled code. A native loop that
// waits on the clock or on hardware must call this, or time would stand still. Recompiled code acts on any interrupt
// that falls due at its next call or backward branch.
inline void charge_ee_cycles(uint32_t cycles) { g_runtime->eeScheduler().accountCycles(cycles); }

// RC_DECOMP_CHARGE=<cycles>: an extra flat charge per native call, for testing timing effects.
extern uint32_t g_nativeExtraCycles;

// Reads a hardware register, such as a timer, as the recompiled code does.
uint32_t hw_read32(uint32_t address);

// Writes a hardware register (DMA channel, VIF), as the recompiled code does.
void hw_write32(uint32_t address, uint32_t value);

// Calls guest code that has not been decompiled yet. It may block: this frame survives on the thread's fiber.
template <typename R = void, typename... A>
R guest_call(uint32_t address, A... args);

template <typename T> struct is_ptr32 : std::false_type {};
template <typename T> struct is_ptr32<ptr32<T>> : std::true_type {};

// A simple generator, so a failing input can be reproduced from its seed.
inline uint32_t nextFuzz(uint64_t &state)
{
    state = state * 6364136223846793005ull + 1442695040888963407ull;
    return static_cast<uint32_t>(state >> 32);
}

// Integers favour the values that break arithmetic; pointers land inside a scratch area of random bytes.
template <typename T>
void randomiseArg(R5900Context *ctx, uint64_t &state, uint32_t scratch, int &gpr, int &fpr)
{
    if constexpr (std::is_same_v<T, ee_float> || std::is_same_v<T, float>)
    {
        const uint32_t kind = nextFuzz(state) % 4u;
        float value = static_cast<float>(static_cast<int32_t>(nextFuzz(state))) / 65536.0f;
        if (kind == 1u)
            value /= 1000.0f;
        else if (kind == 2u)
            value *= 1000.0f;
        else if (kind == 3u)
            value = 0.0f;
        ctx->f[12 + fpr++] = value;
    }
    else
    {
        const int reg = 4 + gpr++;
        if constexpr (is_ptr32<T>::value)
        {
            SET_GPR_S32(ctx, reg, static_cast<int32_t>(scratch + (nextFuzz(state) % 0x400u) * 16u));
        }
        else
        {
            static constexpr uint32_t kEdges[] = {0u, 1u, 2u, 0xFFFFu, 0x7FFFu, 0x8000u, 0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFFu};
            const uint32_t pick = nextFuzz(state);
            const uint32_t raw = (pick & 3u) == 0u ? kEdges[(pick >> 8) % std::size(kEdges)] : nextFuzz(state) >> (pick >> 27);
            // Passed the way callers pass it: extended from the declared type.
            SET_GPR_S32(ctx, reg, static_cast<int32_t>(static_cast<T>(raw)));
        }
    }
}

struct ArgCursor
{
    R5900Context *ctx;
    int gpr = 0;
    int fpr = 0;
};

// The EE passes integers in $a0-$a3 then $t0-$t3, and floats in $f12-$f19, each counted separately.
template <typename T>
T takeArg(ArgCursor &cursor)
{
    if constexpr (std::is_same_v<T, ee_float> || std::is_same_v<T, float>)
        return T(cursor.ctx->f[12 + cursor.fpr++]);
    else
    {
        const int reg = 4 + cursor.gpr++;
        if constexpr (is_ptr32<T>::value)
            return T(getRegU32(cursor.ctx, reg));
        else if constexpr (sizeof(T) == 8)
            return static_cast<T>(GPR_U64(cursor.ctx, reg));
        else
            return static_cast<T>(getRegU32(cursor.ctx, reg));
    }
}

template <typename R>
void storeReturn(R5900Context *ctx, R value)
{
    if constexpr (std::is_same_v<R, ee_float> || std::is_same_v<R, float>)
        ctx->f[0] = value;
    else if constexpr (is_ptr32<R>::value)
    {
        SET_GPR_S32(ctx, 2, static_cast<int32_t>(value.address));
    }
    else if constexpr (sizeof(R) == 8)
    {
        SET_GPR_U64(ctx, 2, static_cast<uint64_t>(value));
    }
    else
    {
        // Word-sized results are sign-extended in the EE's 64-bit registers. Narrower unsigned types are positive.
        SET_GPR_S32(ctx, 2, static_cast<int32_t>(value));
    }
}

template <auto Fn>
struct Thunk;

template <typename R, typename... A, R (*Fn)(A...)>
struct Thunk<Fn>
{
    static constexpr ReturnKind returns = std::is_void_v<R> ? ReturnKind::None
                                          : (std::is_same_v<R, ee_float> || std::is_same_v<R, float>) ? ReturnKind::Float
                                                                                                      : ReturnKind::Gpr;

    static void randomise(R5900Context *ctx, uint64_t &state, uint32_t scratch)
    {
        int gpr = 0, fpr = 0;
        (randomiseArg<A>(ctx, state, scratch, gpr, fpr), ...);
    }

    static void run(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
    {
        g_rdram = rdram;
        ArgCursor cursor{ctx};
        std::tuple<A...> args{takeArg<A>(cursor)...};
        const uint32_t returnAddress = getRegU32(ctx, 31); // Calls to guest code replace $ra.
        if constexpr (std::is_void_v<R>)
            std::apply(Fn, args);
        else
            storeReturn<R>(ctx, std::apply(Fn, args));
        ctx->pc = returnAddress;
        charge_ee_cycles(EeScheduler::kGuestDispatchCycles + g_nativeExtraCycles); // What a call into recompiled code charges.
    }
};

struct Registrar
{
    Registrar(Entry &entry, RecompFn shadow) { registerEntry(entry, shadow); }
};

template <typename R, typename... A>
R guest_call(uint32_t address, A... args)
{
    EeScheduler &scheduler = g_runtime->eeScheduler();
    R5900Context &ctx = scheduler.runningContext();
    int gpr = 4, fpr = 12;
    const auto put = [&](auto value)
    {
        using T = decltype(value);
        if constexpr (std::is_same_v<T, ee_float> || std::is_same_v<T, float>)
            ctx.f[fpr++] = value;
        else if constexpr (is_ptr32<T>::value)
        {
            SET_GPR_S32((&ctx), gpr, static_cast<int32_t>(value.address));
            ++gpr;
        }
        else
        {
            SET_GPR_S32((&ctx), gpr, static_cast<int32_t>(value));
            ++gpr;
        }
    };
    (put(args), ...);
    scheduler.callGuestFromNative(ctx, address);
    if constexpr (std::is_void_v<R>)
        return;
    else if constexpr (std::is_same_v<R, ee_float> || std::is_same_v<R, float>)
        return R(ctx.f[0]);
    else if constexpr (is_ptr32<R>::value)
        return R(getRegU32(&ctx, 2));
    else
        return static_cast<R>(getRegU32(&ctx, 2));
}

// A word the original reads from its own stack frame without writing it first, so it holds whatever an earlier call
// left there. frameSize is the original's frame size and offset the word's place in that frame. A native function
// does not move $sp, so the original's frame starts frameSize below the caller's $sp.
template <typename T>
T stale_stack(uint32_t frameSize, uint32_t offset)
{
    const uint32_t sp = getRegU32(&g_runtime->eeScheduler().runningContext(), 29);
    T value;
    std::memcpy(&value, guest_host(sp - frameSize + offset), sizeof(T));
    return value;
}

// An argument past the eighth: the caller stores it at $sp + 8 * index (index 0 for the ninth). Read from the running
// context's $sp, which a native caller sets up with GuestFrame and stores, as the original does.
template <typename T>
T stack_arg(uint32_t index)
{
    const uint32_t sp = getRegU32(&g_runtime->eeScheduler().runningContext(), 29);
    T value;
    std::memcpy(&value, guest_host(sp + 8u * index), sizeof(T));
    return value;
}

// A local variable the original keeps in its stack frame and passes to another function by address. Placing it at
// the same guest address leaves guest RAM as the original would. Only for native code that does not call guest code
// while the variable is live, since a guest callee's frame would overlap it.
template <typename T>
ptr32<T> frame_local(uint32_t frameSize, uint32_t offset)
{
    const uint32_t sp = getRegU32(&g_runtime->eeScheduler().runningContext(), 29);
    return ptr32<T>(sp - frameSize + offset);
}

// The original's stack frame, for native code that passes the address of a local to guest code (or to a native
// function taking ptr32). Lowers the running context's $sp by the original's frame size while it lives, so the
// locals sit where the original kept them and guest callees build their frames below them, as in the original.
class GuestFrame
{
public:
    explicit GuestFrame(uint32_t size)
        : m_ctx(g_runtime->eeScheduler().runningContext()), m_sp(getRegU32(&m_ctx, 29) - size), m_size(size)
    {
        SET_GPR_S32((&m_ctx), 29, static_cast<int32_t>(m_sp));
    }
    ~GuestFrame() { SET_GPR_S32((&m_ctx), 29, static_cast<int32_t>(m_sp + m_size)); }
    GuestFrame(const GuestFrame &) = delete;
    GuestFrame &operator=(const GuestFrame &) = delete;

    template <typename T>
    ptr32<T> local(uint32_t offset) const { return ptr32<T>(m_sp + offset); }

private:
    R5900Context &m_ctx;
    uint32_t m_sp;
    uint32_t m_size;
};

// Runs the recompiled original of a registered float(float) function, for paths the native version does not cover.
ee_float callOriginalFloat(uint32_t address, ee_float argument);

// A function that only writes known memory can declare it, for a much cheaper live comparison (rc_decomp.cpp).
using RegionProvider = void (*)(std::vector<std::pair<uint32_t, uint32_t>> &ranges);
void registerRegions(const char *name, RegionProvider provider);

// MIPS division: truncates, and does not trap on the cases x86 does.
inline int32_t ee_div(int32_t a, int32_t b)
{
    if (b == 0)
        return a < 0 ? 1 : -1;
    if (a == INT32_MIN && b == -1)
        return INT32_MIN;
    return a / b;
}

inline uint32_t ee_divu(uint32_t a, uint32_t b)
{
    return b == 0 ? 0xFFFFFFFFu : a / b;
}

} // namespace rc

// For a function that can block: it runs natively and cannot be compared call by call, because the
// comparison needs to run it twice from one state.
#define RC_DECOMP_FN_BLOCKING(name, address)                                                         \
    static ::rc::Entry rc_entry_##name{#name, address, &::rc::Thunk<&::rc::name>::run,               \
                                       &::name##_##address, ::rc::Thunk<&::rc::name>::returns,       \
                                       nullptr};                                                     \
    static ::rc::Registrar rc_registrar_##name(rc_entry_##name, &::rc::Thunk<&::rc::name>::run);

// For a function that can block, or that writes hardware registers (a live comparison would run them twice), but whose
// waits and hardware the setup can replace offline: not compared in the game, as RC_DECOMP_FN_BLOCKING, and given
// random inputs by setup in the offline oracle only. The _HEAP form compares all of RAM.
#define RC_DECOMP_FN_BLOCKING_SETUP_IMPL(name, address, setup, wholeRam)                              \
    static ::rc::Entry rc_entry_##name{#name, address, &::rc::Thunk<&::rc::name>::run,               \
                                       &::name##_##address, ::rc::Thunk<&::rc::name>::returns,       \
                                       &setup, wholeRam, true};                                      \
    static ::rc::Registrar rc_registrar_##name(rc_entry_##name, &::rc::Thunk<&::rc::name>::run);
#define RC_DECOMP_FN_BLOCKING_SETUP(name, address, setup) RC_DECOMP_FN_BLOCKING_SETUP_IMPL(name, address, setup, false)
#define RC_DECOMP_FN_BLOCKING_SETUP_HEAP(name, address, setup) RC_DECOMP_FN_BLOCKING_SETUP_IMPL(name, address, setup, true)

// Compared on the game's own calls but not on random inputs, for functions whose real inputs are limited.
#define RC_DECOMP_FN_NOFUZZ(name, address)                                                           \
    static ::rc::Entry rc_entry_##name{#name, address, &::rc::Thunk<&::rc::name>::run,               \
                                       &::name##_##address, ::rc::Thunk<&::rc::name>::returns,       \
                                       nullptr};                                                     \
    static void rc_shadow_##name(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)             \
    {                                                                                                \
        ::rc::shadowCall(rdram, ctx, runtime, rc_entry_##name);                                      \
    }                                                                                                \
    static ::rc::Registrar rc_registrar_##name(rc_entry_##name, &rc_shadow_##name);

// For a function that follows pointers stored in its data, where random bytes would be meaningless. The random test
// calls setup(ctx, state, scratch) instead of randomising the arguments: it builds valid inputs in the scratch area
// (0x8000 bytes of guest RAM at the address given) and in the game's globals, and sets the argument registers.
#define RC_DECOMP_FN_SETUP(name, address, setup)                                                     \
    static ::rc::Entry rc_entry_##name{#name, address, &::rc::Thunk<&::rc::name>::run,               \
                                       &::name##_##address, ::rc::Thunk<&::rc::name>::returns,       \
                                       &setup};                                                      \
    static void rc_shadow_##name(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)             \
    {                                                                                                \
        ::rc::shadowCall(rdram, ctx, runtime, rc_entry_##name);                                      \
    }                                                                                                \
    static ::rc::Registrar rc_registrar_##name(rc_entry_##name, &rc_shadow_##name);

// As RC_DECOMP_FN_SETUP, for a function that allocates from the game's heap: the random test compares all of guest
// RAM instead of the scratch area and the globals, on fewer inputs because that is slow.
#define RC_DECOMP_FN_SETUP_HEAP(name, address, setup)                                                \
    static ::rc::Entry rc_entry_##name{#name, address, &::rc::Thunk<&::rc::name>::run,               \
                                       &::name##_##address, ::rc::Thunk<&::rc::name>::returns,       \
                                       &setup, true};                                                \
    static void rc_shadow_##name(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)             \
    {                                                                                                \
        ::rc::shadowCall(rdram, ctx, runtime, rc_entry_##name);                                      \
    }                                                                                                \
    static ::rc::Registrar rc_registrar_##name(rc_entry_##name, &rc_shadow_##name);

#define RC_DECOMP_FN(name, address)                                                                  \
    static ::rc::Entry rc_entry_##name{#name, address, &::rc::Thunk<&::rc::name>::run,               \
                                       &::name##_##address, ::rc::Thunk<&::rc::name>::returns,       \
                                       &::rc::Thunk<&::rc::name>::randomise};                        \
    static void rc_shadow_##name(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)             \
    {                                                                                                \
        ::rc::shadowCall(rdram, ctx, runtime, rc_entry_##name);                                      \
    }                                                                                                \
    static ::rc::Registrar rc_registrar_##name(rc_entry_##name, &rc_shadow_##name);
