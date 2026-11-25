#pragma once

#include <windows.h>

#include <cstdint>
#include <memory>
#include <unordered_map>

namespace universal_ble {

// Lets a new connection to an address wait until every close of an earlier
// connection to that address has finished. Old GATT objects can deny the new
// connection's lookups until they are closed.
//
// A close holds a Reservation for as long as it runs. The address is closing
// while any reservation for it is alive; reservations are released from any
// thread. Reserve, IsClosing and Released require the caller's lock, so that
// taking a reservation is atomic with removing the connection it belongs to.
class AddressCleanupGate {
public:
  using Reservation = std::shared_ptr<void>;

  // A manual-reset event, set once every reservation of one closing period
  // has been released.
  class Signal {
  public:
    Signal() : handle_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}
    ~Signal() {
      if (handle_ != nullptr) {
        CloseHandle(handle_);
      }
    }
    Signal(const Signal &) = delete;
    Signal &operator=(const Signal &) = delete;

    HANDLE handle() const { return handle_; }
    void Set() const {
      if (handle_ != nullptr) {
        SetEvent(handle_);
      }
    }

  private:
    HANDLE handle_;
  };

  Reservation Reserve(uint64_t address) {
    // Forget addresses whose closes have all finished, so addresses that are
    // never connected again do not accumulate.
    for (auto it = closing_.begin(); it != closing_.end();) {
      if (it->first != address && it->second.reservation.expired()) {
        it = closing_.erase(it);
      } else {
        ++it;
      }
    }
    auto &slot = closing_[address];
    if (auto existing = slot.reservation.lock()) {
      return existing;
    }
    auto released = std::make_shared<Signal>();
    Reservation reservation(new char, [released](void *token) {
      delete static_cast<char *>(token);
      released->Set();
    });
    slot = {reservation, released};
    return reservation;
  }

  bool IsClosing(uint64_t address) { return Released(address) != nullptr; }

  // The signal of the address's closing period, or nullptr when it is not
  // closing. Holding the result keeps the event valid.
  std::shared_ptr<const Signal> Released(uint64_t address) {
    const auto it = closing_.find(address);
    if (it == closing_.end()) {
      return nullptr;
    }
    if (it->second.reservation.expired()) {
      closing_.erase(it);
      return nullptr;
    }
    return it->second.released;
  }

  size_t TrackedAddresses() const { return closing_.size(); }

private:
  struct Slot {
    std::weak_ptr<void> reservation;
    std::shared_ptr<const Signal> released;
  };
  std::unordered_map<uint64_t, Slot> closing_;
};

}  // namespace universal_ble
