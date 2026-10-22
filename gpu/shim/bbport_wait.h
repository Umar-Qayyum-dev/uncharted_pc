// bbport: waiting on an atomic that really sleeps. libc++'s std::atomic::wait on Windows (MinGW,
// clang64) polls with yields (__libcpp_thread_poll_with_backoff): the draw recording thread,
// "sleeping" between packets, took ~90 % of a core on a 4-core/8-thread PC whose CPU was full.
// On Windows the wait is WaitOnAddress / WakeByAddressSingle (synchronization library);
// BB_TOGGLE_FILE bit 62 (SleepingWaits) goes back to std::atomic::wait while running.
#pragma once

#include <atomic>
#include <cstddef>
#include "bbport_toggles.h"

#ifdef _WIN32
extern "C" __declspec(dllimport) int __stdcall WaitOnAddress(volatile void* address, void* compare,
                                                             std::size_t size, unsigned long ms);
extern "C" __declspec(dllimport) void __stdcall WakeByAddressSingle(void* address);
#endif

namespace BbWait {

/// Returns when `value` may differ from `old` (spurious returns: the caller checks again).
template <typename T>
inline void Wait(std::atomic<T>& value, T old) {
#ifdef _WIN32
    static_assert(sizeof(std::atomic<T>) == sizeof(T));
    if (!BbToggle::Disabled(BbToggle::SleepingWaits)) {
        WaitOnAddress(&value, &old, sizeof(T), 0xFFFFFFFFul);
        return;
    }
#endif
    value.wait(old, std::memory_order_seq_cst);
}

/// Wakes a thread in Wait() on `value` (either kind).
template <typename T>
inline void WakeOne(std::atomic<T>& value) {
#ifdef _WIN32
    WakeByAddressSingle(&value);
#endif
    value.notify_one();
}

} // namespace BbWait
