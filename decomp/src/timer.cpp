// Decompiled from SYSTIMER.c and TIMER.c: the system clock and the game's pausable timers.
//
// SYSTIMER runs EE timer 0 at 576 kHz (bus clock / 256) with its compare at 0x3d86 ticks (about 36.6 Hz). Its
// interrupt handler adds the ticks since the last interrupt to tickTime (carrying into tickTimeCarry every 0x200000),
// calls the callbacks of the timers on its list that fell due and sets the next compare to the nearest due one.
// SysTimerGet is the time in milliseconds-like units: (ticks * 1000) / 0x3d86, from tickTime, the carry and the
// counter's current value. A TIMER (_sTimer) is a time that runs (or, with flag 2, counts down) with the system
// clock while flag 1 (paused) is clear.
//
// Never called by the game: SysTimerAdd, SysTimerRemove, SysTimerGetTicks (so the handler's list is always empty),
// TimerSet and TimerReset. SysTimerHandler is reached only as the timer 0 interrupt handler.

#include "rc_decomp.h"

namespace rc {

namespace {

// EE timer 0's registers.
constexpr uint32_t kT0Count = 0x10000000u, kT0Mode = 0x10000010u, kT0Comp = 0x10000020u;
constexpr uint32_t kAddIntcHandler2 = sym::AddIntcHandler2, kRemoveIntcHandler = sym::RemoveIntcHandler, kDisableIntc = sym::DisableIntc,
                   kEnableIntc = sym::EnableIntc;
constexpr uint32_t kSysTimerHandler = sym::SysTimerHandler, kSysTimer = sym::sysTimerVarG, kProgramCounter = (sym::sysTimerVarG + 0x18u), kListHead = (sym::sysTimerVarG + 0x14u);
constexpr int32_t kIntcTimer0 = 9;
constexpr uint32_t kTicksPerPeriod = 0x3d86u;

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

sysTimerClass &sys() { return G(sysTimerVarG); }

// _sSysTimer: pNext +0, frequency +4 (ticks between calls), dueTick +8 (ticks until due), pFnCallback +0xc.
// Moves every timer on the list on by lastTicks, calls those that fell due (with how far past due, at most 0) and
// returns the nearest due tick, starting from limit.
int32_t runTimers(uint32_t timer, int32_t limit)
{
    while (timer != 0u)
    {
        const int32_t due = load<int32_t>(timer + 8u);
        const int32_t last = sys().lastTicks;
        const uint32_t next = load<uint32_t>(timer);
        const int32_t left = static_cast<int32_t>(static_cast<uint32_t>(due) - static_cast<uint32_t>(last));
        store<int32_t>(timer + 8u, left);
        if (left <= 0)
        {
            store<uint32_t>(timer + 8u, static_cast<uint32_t>(left) + load<uint32_t>(timer + 4u));
            guest_call(load<uint32_t>(timer + 0xcu), left);
        }
        const int32_t now = load<int32_t>(timer + 8u);
        timer = next;
        if (now < limit)
            limit = now;
    }
    return limit;
}

} // namespace

void SysTimerInit()
{
    RC_DIVERT_IF_CHANGED(SysTimerInit);
    {
        GuestFrame frame(0x30);
        sysTimerClass &t = sys();
        t.lastTicks = kTicksPerPeriod;
        t.tickTime = 0;
        t.tickTimeCarry = 0;
        t.pSysTimerList = nullptr;
        t.eventHandle = guest_call<int32_t>(kAddIntcHandler2, kIntcTimer0, kSysTimerHandler, 0, kProgramCounter);
        hw_write32(kT0Comp, kTicksPerPeriod);
        hw_write32(kT0Count, 0);
        hw_write32(kT0Mode, 0x183); // bus clock / 256, compare interrupt, counting, reset at compare
    }
    guest_call(kEnableIntc, kIntcTimer0);
}

void SysTimerTerm()
{
    {
        GuestFrame frame(0x10);
        guest_call(kRemoveIntcHandler, kIntcTimer0, sys().eventHandle);
    }
    guest_call(kDisableIntc, kIntcTimer0);
}

// Adds a timer called every 0x3d86 / frequency ticks, catches the clock up and runs the list as the handler does.
void SysTimerAdd(ptr32<_sSysTimer> pNewTimer, ee_fnptr pFnCallback, uint32 frequency)
{
    RC_DIVERT_IF_CHANGED(SysTimerAdd, pNewTimer, pFnCallback, frequency);
    {
        GuestFrame frame(0x50);
        sysTimerClass &t = sys();
        const uint32_t period = ee_divu(kTicksPerPeriod, frequency);
        const uint32_t timer = pNewTimer.address;
        store<uint32_t>(timer, t.pSysTimerList.address);
        store<uint32_t>(timer + 0xcu, pFnCallback.address);
        store<uint32_t>(timer + 4u, period);
        store<int32_t>(timer + 8u, t.lastTicks);
        guest_call(kDisableIntc, kIntcTimer0);
        const uint32_t tick = t.tickTime;
        const uint32_t count = hw_read32(kT0Count);
        t.pSysTimerList = pNewTimer;
        t.lastTicks = static_cast<int32_t>(count);
        t.tickTime = tick + count;
        const int32_t nearest = runTimers(timer, 0xffff);
        t.lastTicks = nearest;
        hw_write32(kT0Comp, static_cast<uint32_t>(nearest));
        hw_write32(kT0Count, 0);
    }
    guest_call(kEnableIntc, kIntcTimer0);
}

// Unlinks the list's first timer, replacing it with pSysTimer's successor. With an empty list the original's search
// loop runs on the word at address 0: it unlinks at address 0 if that word is non-zero, and never ends if it is zero
// (unless pSysTimer is null, when it unlinks at the list head).
void SysTimerRemove(ptr32<_sSysTimer> pSysTimer)
{
    {
        GuestFrame frame(0x30);
        uint32_t link = kListHead;
        if (sys().pSysTimerList.address == 0u && pSysTimer.address != 0u)
        {
            link = 0;
            while (*static_cast<volatile uint32_t *>(guest_host(0)) == 0u)
                charge_ee_cycles(4);
        }
        guest_call(kDisableIntc, kIntcTimer0);
        store<uint32_t>(link, load<uint32_t>(pSysTimer.address));
    }
    guest_call(kEnableIntc, kIntcTimer0);
}

tTime SysTimerGet()
{
    RC_DIVERT_IF_CHANGED(SysTimerGet);
    const sysTimerClass &t = sys();
    const uint32_t carry = t.tickTimeCarry;
    const int32_t count = static_cast<int32_t>(hw_read32(kT0Count));
    const uint32_t tick = t.tickTime;
    const uint64_t ticks = static_cast<uint64_t>(static_cast<int64_t>(count)) + tick + static_cast<uint32_t>(carry << 21);
    // __divdi3: a signed 64-bit division, truncating.
    const int64_t time = static_cast<int64_t>(ticks * 1000u) / static_cast<int64_t>(kTicksPerPeriod);
    return static_cast<tTime>(static_cast<int32_t>(time));
}

int64 SysTimerGetTicks()
{
    const sysTimerClass &t = sys();
    const int32_t count = static_cast<int32_t>(hw_read32(kT0Count));
    const uint32_t carry = t.tickTimeCarry;
    const uint32_t tick = t.tickTime;
    return static_cast<int64>(static_cast<uint64_t>(static_cast<int64_t>(count)) + tick + static_cast<uint32_t>(carry << 21));
}

// Timer 0's compare interrupt (INTC 9). Saves the EPC where pStore points, as the handler protocol expects.
int32_t SysTimerHandler(int32_t arg0, ptr32<void> pStore, ptr32<void> pEPC)
{
    RC_DIVERT_IF_CHANGED(SysTimerHandler, arg0, pStore, pEPC);
    GuestFrame frame(0x50);
    sysTimerClass &t = sys();
    store<uint32_t>(pStore.address, pEPC.address);
    const uint32_t tick = t.tickTime + static_cast<uint32_t>(t.lastTicks);
    t.tickTime = tick;
    if (tick > 0x200000u)
    {
        t.tickTime = tick - 0x200000u;
        t.tickTimeCarry += 1;
    }
    const int32_t nearest = runTimers(t.pSysTimerList.address, static_cast<int32_t>(kTicksPerPeriod));
    t.lastTicks = nearest;
    hw_write32(kT0Comp, static_cast<uint32_t>(nearest));
    hw_write32(kT0Mode, 0);
    hw_write32(kT0Count, 0);
    hw_write32(kT0Mode, 0x583); // as SysTimerInit's, with the compare flag written to clear it
    g_runtime->eeScheduler().runningContext().cop0_status |= 0x10000u; // ei
    return 0;
}

// TIMER.c. _sTimer: flags +0 (1 paused, 2 counting down), sysTime +4 (the clock when last updated), time +8.

void TimerSetup(ptr32<_sTimer> pTimer, tTime time, uint32 flags)
{
    pTimer->flags = flags;
    pTimer->time = time;
    pTimer->sysTime = SysTimerGet();
}

namespace {

// Brings the time up to the clock now (all in wrapping 32-bit arithmetic, as the original's addu/subu).
void catchUp(_sTimer &timer)
{
    const uint32_t now = static_cast<uint32_t>(SysTimerGet());
    const uint32_t flags = timer.flags;
    const uint32_t elapsed = now - static_cast<uint32_t>(timer.sysTime);
    const uint32_t time = static_cast<uint32_t>(timer.time);
    timer.sysTime = static_cast<tTime>(now);
    timer.time = static_cast<tTime>((flags & 2u) == 0u ? time + elapsed : time - elapsed);
}

} // namespace

void TimerPause(ptr32<_sTimer> pTimer)
{
    _sTimer &timer = *pTimer;
    if ((timer.flags & 1u) != 0u)
        return;
    catchUp(timer);
    timer.flags |= 1u;
}

void TimerResume(ptr32<_sTimer> pTimer)
{
    pTimer->flags &= ~1u;
    pTimer->sysTime = SysTimerGet();
}

void TimerSet(ptr32<_sTimer> pTimer, tTime time)
{
    pTimer->time = time;
    pTimer->sysTime = SysTimerGet();
}

void TimerReset(ptr32<_sTimer> pTimer)
{
    pTimer->time = 0;
    pTimer->sysTime = SysTimerGet();
}

tTime TimerGet(ptr32<_sTimer> pTimer)
{
    _sTimer &timer = *pTimer;
    if ((timer.flags & 1u) == 0u)
        catchUp(timer);
    return timer.time;
}

} // namespace rc

#include "res_fuzz.h"

// All read the free-running counter (T0_COUNT) or write the timer's registers, so a live comparison, which runs the
// original and then the native function, would see two different counts: they are compared offline only, where the
// counter is stopped at a value the setup chooses and the register writes and kernel calls are logged.
RC_DECOMP_FN_BLOCKING_SETUP(SysTimerInit, rc::fuzz::sysTimer)
RC_DECOMP_FN_BLOCKING_SETUP(SysTimerTerm, rc::fuzz::sysTimer)
RC_DECOMP_FN_BLOCKING_SETUP(SysTimerAdd, rc::fuzz::sysTimerAdd)
RC_DECOMP_FN_BLOCKING_SETUP_HEAP(SysTimerRemove, rc::fuzz::sysTimerRemove)
RC_DECOMP_FN_BLOCKING_SETUP(SysTimerGet, rc::fuzz::sysTimer)
RC_DECOMP_FN_BLOCKING_SETUP(SysTimerGetTicks, rc::fuzz::sysTimer)
RC_DECOMP_FN_BLOCKING_SETUP(SysTimerHandler, rc::fuzz::sysTimerHandler)
RC_DECOMP_FN_BLOCKING_SETUP(TimerSetup, rc::fuzz::timerArgs)
RC_DECOMP_FN_BLOCKING_SETUP(TimerPause, rc::fuzz::timerArgs)
RC_DECOMP_FN_BLOCKING_SETUP(TimerResume, rc::fuzz::timerArgs)
RC_DECOMP_FN_BLOCKING_SETUP(TimerSet, rc::fuzz::timerArgs)
RC_DECOMP_FN_BLOCKING_SETUP(TimerReset, rc::fuzz::timerArgs)
RC_DECOMP_FN_BLOCKING_SETUP(TimerGet, rc::fuzz::timerArgs)
