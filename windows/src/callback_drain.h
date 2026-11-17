#pragma once

#include <chrono>

#include "async_operation_tracker.h"

namespace universal_ble {

// Waits until no lease is held. Does not dispatch COM calls or window
// messages, so it is only for leases whose holders never wait for the calling
// thread.
inline void WaitForCallbacksWithoutMessagePump(
    const AsyncOperationTracker &operations) noexcept {
  while (!operations.WaitUntilIdleFor(std::chrono::milliseconds(10))) {
  }
}

// Close admission before waiting. Workers that outlive the drain must own
// their state and avoid caller-owned resources. If pinning fails, keep
// waiting because a host could otherwise unload the running worker code.
template <typename KeepModuleLoaded>
void DrainConnectionCleanup(
    const AsyncOperationTracker &operations,
    std::chrono::steady_clock::duration timeout,
    KeepModuleLoaded keep_module_loaded) noexcept {
  if (operations.WaitUntilIdleFor(timeout)) {
    return;
  }
  if (!keep_module_loaded()) {
    WaitForCallbacksWithoutMessagePump(operations);
  }
}

}  // namespace universal_ble
