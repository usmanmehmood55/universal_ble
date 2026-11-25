#include "async_operation_tracker.h"

#include <chrono>
#include <iostream>

using universal_ble::AsyncOperationTracker;

namespace {
bool Check(bool condition, const char *expression, int line) {
  if (condition) {
    return true;
  }
  std::cerr << "CHECK failed at line " << line << ": " << expression
            << std::endl;
  return false;
}
}  // namespace

#define CHECK(expression)                              \
  do {                                                 \
    if (!Check((expression), #expression, __LINE__)) { \
      return 1;                                        \
    }                                                  \
  } while (false)

int main() {
  {
    AsyncOperationTracker tracker;
    auto first = tracker.TryAcquire();
    CHECK(first.has_value());
    auto retained_copy = first.value();
    first.reset();
    CHECK(!tracker.IsIdle());
    retained_copy.reset();
    CHECK(tracker.IsIdle());
  }

  {
    AsyncOperationTracker tracker;
    auto operation = tracker.TryAcquire();
    CHECK(operation.has_value());
    tracker.Close();
    CHECK(!tracker.TryAcquire().has_value());
    CHECK(!tracker.WaitUntilIdleFor(std::chrono::milliseconds(1)));

    operation.reset();
    CHECK(tracker.WaitUntilIdleFor(std::chrono::milliseconds(1)));
  }

  // ScopedLease keeps the owner busy only while held, and refuses to
  // re-acquire once the owner has closed the tracker, even after the owner
  // itself is gone.
  {
    std::optional<AsyncOperationTracker> owner(std::in_place);
    universal_ble::ScopedLease lease(*owner);
    CHECK(!lease.Held());
    CHECK(lease.Acquire());
    CHECK(lease.Held());
    CHECK(!owner->IsIdle());
    lease.Release();
    CHECK(!lease.Held());
    CHECK(owner->IsIdle());
    CHECK(lease.Acquire());
    owner->Close();
    lease.Release();
    CHECK(owner->WaitUntilIdleFor(std::chrono::milliseconds(1)));
    owner.reset();
    CHECK(!lease.Acquire());
    CHECK(!lease.Held());
  }

  return 0;
}
