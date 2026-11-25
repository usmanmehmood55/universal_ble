#pragma once

#include <utility>

#include "async_operation_tracker.h"

namespace universal_ble {

// Thrown by Unleased when the plugin lease cannot be retaken after an await:
// the plugin is shutting down or gone. A coroutine that catches it may touch
// only what its own frame owns.
struct PluginGone {};

namespace detail {
template <typename T> decltype(auto) GetAwaiter(T &&value) {
  if constexpr (requires { std::forward<T>(value).operator co_await(); }) {
    return std::forward<T>(value).operator co_await();
  } else if constexpr (requires {
                         operator co_await(std::forward<T>(value));
                       }) {
    return operator co_await(std::forward<T>(value));
  } else {
    return std::forward<T>(value);
  }
}
}  // namespace detail

// Awaits `awaitable` without holding `lease`, so a suspended coroutine never
// holds up shutdown:
//
//   const auto device =
//       co_await Unleased(plugin, FromBluetoothAddressAsync(address));
//
// The lease is released before the coroutine can suspend and retaken before
// the result is used. When it cannot be retaken, PluginGone is thrown instead
// of returning the result. An error of the awaited operation itself is
// thrown with the lease held.
//
// Use it only directly in a co_await expression: it refers to `awaitable`,
// a temporary that lives until the end of that expression, because
// C++/WinRT's awaiters can be neither copied nor moved.
template <typename Awaitable> class Unleased {
  using Inner = decltype(detail::GetAwaiter(std::declval<Awaitable &>()));

public:
  Unleased(ScopedLease &lease, Awaitable &&awaitable)
      : lease_(lease), awaitable_(std::forward<Awaitable>(awaitable)),
        inner_(detail::GetAwaiter(awaitable_)) {}
  Unleased(const Unleased &) = delete;
  Unleased &operator=(const Unleased &) = delete;

  bool await_ready() {
    lease_.Release();
    return inner_.await_ready();
  }
  template <typename Handle> decltype(auto) await_suspend(Handle handle) {
    return inner_.await_suspend(handle);
  }
  decltype(auto) await_resume() {
    if (!lease_.Acquire()) {
      throw PluginGone{};
    }
    return inner_.await_resume();
  }

private:
  ScopedLease &lease_;
  Awaitable &&awaitable_;
  Inner inner_;
};

template <typename Awaitable>
Unleased(ScopedLease &, Awaitable &&) -> Unleased<Awaitable>;

}  // namespace universal_ble
