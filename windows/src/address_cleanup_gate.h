#pragma once

#include <cstdint>
#include <memory>
#include <unordered_map>

namespace universal_ble {

// Lets a new connection to an address wait until every close of an earlier
// connection to that address has finished. Old GATT services can deny the new
// connection's discovery until they are closed.
//
// A close holds a Reservation for as long as it runs. The address is closing
// while any reservation for it is alive; reservations are released from any
// thread. Reserve and IsClosing require the caller's lock, so that taking a
// reservation is atomic with removing the connection it belongs to.
class AddressCleanupGate {
public:
  using Reservation = std::shared_ptr<void>;

  Reservation Reserve(uint64_t address) {
    // Forget addresses whose closes have all finished, so addresses that are
    // never connected again do not accumulate.
    for (auto it = closing_.begin(); it != closing_.end();) {
      if (it->first != address && it->second.expired()) {
        it = closing_.erase(it);
      } else {
        ++it;
      }
    }
    auto &slot = closing_[address];
    if (auto existing = slot.lock()) {
      return existing;
    }
    auto reservation = std::make_shared<char>();
    slot = reservation;
    return reservation;
  }

  bool IsClosing(uint64_t address) {
    const auto it = closing_.find(address);
    if (it == closing_.end()) {
      return false;
    }
    if (it->second.expired()) {
      closing_.erase(it);
      return false;
    }
    return true;
  }

  size_t TrackedAddresses() const { return closing_.size(); }

private:
  std::unordered_map<uint64_t, std::weak_ptr<void>> closing_;
};

}  // namespace universal_ble
