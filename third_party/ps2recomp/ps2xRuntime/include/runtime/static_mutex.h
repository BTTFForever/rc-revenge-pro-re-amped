#pragma once
// A mutex usable before dynamic initialisation: for globals that code running from a priority constructor (the game's
// settings load, decomp/rc_settings.cpp, constructor(101)) can lock. std::mutex is constant-initialised with glibc,
// but not with MinGW-w64's libstdc++, whose constructor runs with the other dynamic initialisers: locking one before
// that throws std::system_error (EINVAL). On Windows this is an SRWLOCK (all zero bits is its initial state; the calls
// are in src/lib/static_mutex.cpp so that this header needs no windows.h).
// Meets the Lockable requirements, so std::lock_guard / std::unique_lock work with it (not std::condition_variable:
// use std::condition_variable_any).

#ifdef _WIN32
namespace ps2x {
class StaticMutex
{
public:
    constexpr StaticMutex() noexcept = default;
    StaticMutex(const StaticMutex &) = delete;
    StaticMutex &operator=(const StaticMutex &) = delete;
    void lock() noexcept;
    bool try_lock() noexcept;
    void unlock() noexcept;

private:
    void *m_lock = nullptr; // an SRWLOCK
};
} // namespace ps2x
#else
#include <mutex>

namespace ps2x {
using StaticMutex = std::mutex;
} // namespace ps2x
#endif
