// Connects decompiled functions to the running game.
//
// RC_DECOMP_FN(name, 0xaddress) puts rc::name in place of the recompiled function at that guest
// address. With RC_SHADOW=1 in the environment, each sampled call runs the recompiled original and
// the decompiled function on the same state and compares their results and every byte of guest RAM.
//
// A decompiled function must not reach anything that can block or switch guest threads: the runtime
// does that by unwinding the host stack. tools/decomp_order.py lists the functions that are safe.
#pragma once

#include <atomic>
#include <cmath>
#include <cstddef>
#include <iterator>
#include <tuple>
#include <type_traits>

#include "ps2_runtime.h"
#include "ps2_recompiled_functions.h"
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
    std::atomic<uint64_t> calls{0};
    std::atomic<uint64_t> compared{0};
    std::atomic<uint64_t> mismatches{0};
};

void registerEntry(Entry &entry, RecompFn shadow);
void shadowCall(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, Entry &entry);

// Called once when the game is loaded.
void installDecomp(PS2Runtime &runtime);

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
        if constexpr (std::is_void_v<R>)
            std::apply(Fn, args);
        else
            storeReturn<R>(ctx, std::apply(Fn, args));
        ctx->pc = getRegU32(ctx, 31);
    }
};

struct Registrar
{
    Registrar(Entry &entry, RecompFn shadow) { registerEntry(entry, shadow); }
};

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

#define RC_DECOMP_FN(name, address)                                                                  \
    static ::rc::Entry rc_entry_##name{#name, address, &::rc::Thunk<&::rc::name>::run,               \
                                       &::name##_##address, ::rc::Thunk<&::rc::name>::returns,       \
                                       &::rc::Thunk<&::rc::name>::randomise};                        \
    static void rc_shadow_##name(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)             \
    {                                                                                                \
        ::rc::shadowCall(rdram, ctx, runtime, rc_entry_##name);                                      \
    }                                                                                                \
    static ::rc::Registrar rc_registrar_##name(rc_entry_##name, &rc_shadow_##name);
