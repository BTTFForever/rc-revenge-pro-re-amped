#include "runtime/static_mutex.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

static_assert(sizeof(SRWLOCK) == sizeof(void *), "StaticMutex holds an SRWLOCK in a pointer");

namespace ps2x {
void StaticMutex::lock() noexcept { AcquireSRWLockExclusive(reinterpret_cast<PSRWLOCK>(&m_lock)); }
bool StaticMutex::try_lock() noexcept { return TryAcquireSRWLockExclusive(reinterpret_cast<PSRWLOCK>(&m_lock)) != 0; }
void StaticMutex::unlock() noexcept { ReleaseSRWLockExclusive(reinterpret_cast<PSRWLOCK>(&m_lock)); }
} // namespace ps2x
#endif
