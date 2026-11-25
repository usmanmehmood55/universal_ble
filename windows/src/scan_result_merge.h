#ifndef FLUTTER_PLUGIN_UNIVERSAL_BLE_SCAN_RESULT_MERGE_H_
#define FLUTTER_PLUGIN_UNIVERSAL_BLE_SCAN_RESULT_MERGE_H_

#include <optional>

namespace universal_ble {

// Determines whether the scan result is connectable. On Windows, a scan
// response always reports IsConnectable == false; it inherits connectability
// if the device was already seen advertising connectably.
//
// An advertisement whose connectability could not be read carries no value
// (see BluetoothLeWatcherReceived), and unknown stays unknown: the caller keeps
// the field null instead of inventing a state, so `connectableOnly` does not
// drop devices the host cannot classify. A scan response for a device already
// heard connectable still resolves to connectable, because that answer comes
// from the cache rather than from the packet.
inline std::optional<bool> DeriveConnectableState(
    const std::optional<bool> is_connectable, const bool is_scan_response,
    const bool address_heard_connectable) {
  if (is_connectable.value_or(false) ||
      (is_scan_response && address_heard_connectable)) {
    return true;
  }
  return is_connectable;
}

// Where a device is in waiting for its first scan response during one scan.
enum class ScanResponseWait {
  kNone,  // not seen yet
  kHeld,  // first advertisement held back, waiting for the scan response
  kDone,  // scan response arrived or the wait ran out; report everything
};

struct ScanResponseStep {
  ScanResponseWait state;
  bool report;       // report this packet now
  bool start_timer;  // start the wait for the scan response
};

// Only the first advertisement of a device that accepts scan requests is
// held, and only once per scan: a device whose scan response never comes is
// reported when the wait runs out and not held again. A scan response ends
// the wait and is reported, merged with what was held.
inline ScanResponseStep NextScanResponseWait(ScanResponseWait state,
                                             bool is_scan_response,
                                             bool expects_scan_response) {
  if (is_scan_response) {
    return {ScanResponseWait::kDone, true, false};
  }
  switch (state) {
  case ScanResponseWait::kNone:
    if (expects_scan_response) {
      return {ScanResponseWait::kHeld, false, true};
    }
    return {ScanResponseWait::kDone, true, false};
  case ScanResponseWait::kHeld:
    return {ScanResponseWait::kHeld, false, false};
  case ScanResponseWait::kDone:
  default:
    return {ScanResponseWait::kDone, true, false};
  }
}

}  // namespace universal_ble

#endif  // FLUTTER_PLUGIN_UNIVERSAL_BLE_SCAN_RESULT_MERGE_H_
