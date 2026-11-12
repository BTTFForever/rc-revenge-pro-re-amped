// The rules decompiled code runs under while it shares a process with the recompiled game.
//
// Decompiled code keeps the PS2's data layout: pointers inside structs are 32-bit guest addresses
// (ptr32) and every struct lives in guest RAM, so the game's data files and its recompiled functions
// see exactly what they expect. Floats are ee_float, which does each operation the way the recompiled
// code does, so a decompiled function gives bit-identical results.
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

#include "ps2_runtime_macros.h"
#include "runtime/ps2_memory.h"

namespace rc {

extern uint8_t *g_rdram; // Guest RAM, set once at start.

// The 16 KB scratchpad (0x70000000) is separate memory; everything else is main RAM, mirrors included.
inline void *guest_host(uint32_t address)
{
    if (ps2IsScratchpadAddress(address)) [[unlikely]]
        return ps2GetScratchpadHostPtr() + ps2ScratchpadOffset(address);
    return g_rdram + (address & PS2_RAM_MASK);
}
inline uint32_t guest_addr(const void *host)
{
    const uint8_t *p = static_cast<const uint8_t *>(host);
    const uint8_t *scratch = ps2GetScratchpadHostPtr();
    if (scratch && p >= scratch && p < scratch + PS2_SCRATCHPAD_SIZE) [[unlikely]]
        return PS2_SCRATCHPAD_BASE + static_cast<uint32_t>(p - scratch);
    return static_cast<uint32_t>(p - g_rdram);
}

template <typename T>
struct ptr32
{
    uint32_t address;

    ptr32() = default;
    constexpr ptr32(std::nullptr_t) : address(0u) {}
    constexpr explicit ptr32(uint32_t guest) : address(guest) {}

    T *get() const { return static_cast<T *>(guest_host(address)); }
    T *operator->() const { return get(); }
    template <typename U = T, typename = std::enable_if_t<!std::is_void_v<U>>>
    U &operator*() const { return *static_cast<U *>(get()); }
    template <typename U = T, typename = std::enable_if_t<!std::is_void_v<U>>>
    U &operator[](ptrdiff_t index) const { return get()[index]; }

    explicit operator bool() const { return address != 0u; }
    bool operator==(ptr32 other) const { return address == other.address; }
    bool operator!=(ptr32 other) const { return address != other.address; }

    template <typename U = T, typename = std::enable_if_t<!std::is_void_v<U>>>
    ptr32 operator+(ptrdiff_t count) const { return ptr32(address + static_cast<uint32_t>(count * sizeof(U))); }
    template <typename U = T, typename = std::enable_if_t<!std::is_void_v<U>>>
    ptr32 operator-(ptrdiff_t count) const { return ptr32(address - static_cast<uint32_t>(count * sizeof(U))); }
    template <typename U>
    ptr32<U> cast() const { return ptr32<U>(address); }
};

// A guest code address. The debug data does not record argument types for function pointers.
struct ee_fnptr
{
    uint32_t address;
};

// The EE's 128-bit integers.
struct alignas(16) ee_int128
{
    uint64_t lo, hi;
};
using ee_uint128 = ee_int128;

// A float that does arithmetic the way the EE does. Same size and representation as float.
struct ee_float
{
    float v;

    ee_float() = default;
    constexpr ee_float(float value) : v(value) {}
    operator float() const { return v; }

    friend ee_float operator+(ee_float a, ee_float b) { return FPU_ADD_S(a.v, b.v); }
    friend ee_float operator-(ee_float a, ee_float b) { return FPU_SUB_S(a.v, b.v); }
    friend ee_float operator*(ee_float a, ee_float b) { return FPU_MUL_S(a.v, b.v); }
    friend ee_float operator/(ee_float a, ee_float b) { return FPU_DIV_S(a.v, b.v); }
    ee_float operator-() const { return FPU_NEG_S(v); }
    ee_float &operator+=(ee_float o) { return *this = *this + o; }
    ee_float &operator-=(ee_float o) { return *this = *this - o; }
    ee_float &operator*=(ee_float o) { return *this = *this * o; }
    ee_float &operator/=(ee_float o) { return *this = *this / o; }

    friend bool operator<(ee_float a, ee_float b) { return FPU_C_LT_S(a.v, b.v); }
    friend bool operator<=(ee_float a, ee_float b) { return FPU_C_LE_S(a.v, b.v); }
    friend bool operator>(ee_float a, ee_float b) { return FPU_C_LT_S(b.v, a.v); }
    friend bool operator>=(ee_float a, ee_float b) { return FPU_C_LE_S(b.v, a.v); }
    friend bool operator==(ee_float a, ee_float b) { return FPU_C_EQ_S(a.v, b.v); }
    friend bool operator!=(ee_float a, ee_float b) { return !FPU_C_EQ_S(a.v, b.v); }
};

inline ee_float ee_sqrt(ee_float x) { return FPU_SQRT_S(x.v); }
// a / sqrt(b) as one instruction (rsqrt.s), which rounds differently from a division and a square root in turn.
inline ee_float ee_rsqrt(ee_float a, ee_float b) { return FPU_RSQRT_S(a.v, b.v); }
inline ee_float ee_abs(ee_float x) { return FPU_ABS_S(x.v); }
inline ee_float ee_itof(int32_t x) { return FPU_CVT_S_W(x); }
inline int32_t ee_ftoi(ee_float x) { return FPU_CVT_W_S(x.v); } // Truncates and saturates, as cvt.w.s does.

} // namespace rc
