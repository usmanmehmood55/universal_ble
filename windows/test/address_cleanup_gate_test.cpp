#include "address_cleanup_gate.h"

#include <iostream>
#include <thread>
#include <vector>

using universal_ble::AddressCleanupGate;

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
  constexpr uint64_t kA = 0xAABBCCDDEEFF;
  constexpr uint64_t kB = 0x112233445566;

  // An address nobody closed is not closing.
  {
    AddressCleanupGate gate;
    CHECK(!gate.IsClosing(kA));
    CHECK(gate.TrackedAddresses() == 0);
  }

  // Closing lasts as long as any reservation for the address is alive.
  {
    AddressCleanupGate gate;
    auto first = gate.Reserve(kA);
    CHECK(gate.IsClosing(kA));
    auto second = gate.Reserve(kA);
    CHECK(gate.IsClosing(kA));
    first.reset();
    CHECK(gate.IsClosing(kA));
    second.reset();
    CHECK(!gate.IsClosing(kA));
    CHECK(gate.TrackedAddresses() == 0);
  }

  // Addresses are independent.
  {
    AddressCleanupGate gate;
    auto a = gate.Reserve(kA);
    CHECK(gate.IsClosing(kA));
    CHECK(!gate.IsClosing(kB));
    auto b = gate.Reserve(kB);
    a.reset();
    CHECK(!gate.IsClosing(kA));
    CHECK(gate.IsClosing(kB));
  }

  // A new reservation after every close finished starts a new closing period.
  {
    AddressCleanupGate gate;
    gate.Reserve(kA).reset();
    CHECK(!gate.IsClosing(kA));
    auto again = gate.Reserve(kA);
    CHECK(gate.IsClosing(kA));
  }

  // Finished addresses are forgotten when another address is reserved, so the
  // gate does not grow with every address ever connected.
  {
    AddressCleanupGate gate;
    gate.Reserve(kA).reset();
    CHECK(gate.TrackedAddresses() == 1);
    auto b = gate.Reserve(kB);
    CHECK(gate.TrackedAddresses() == 1);
    CHECK(gate.IsClosing(kB));
  }

  // Closes finish on other threads (the WinRT thread pool).
  {
    AddressCleanupGate gate;
    std::vector<std::thread> closers;
    for (int i = 0; i < 8; ++i) {
      closers.emplace_back([reservation = gate.Reserve(kA)]() mutable {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        reservation.reset();
      });
    }
    CHECK(gate.IsClosing(kA));
    for (auto &closer : closers) {
      closer.join();
    }
    CHECK(!gate.IsClosing(kA));
  }

  std::cout << "All address cleanup gate cases passed" << std::endl;
  return 0;
}
