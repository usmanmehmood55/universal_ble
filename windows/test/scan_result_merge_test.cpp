// Unit tests for the pure scan decisions used by
// UniversalBlePlugin::PushScanResultLocked: connectability of scan responses
// and the wait for a device's first scan response.

#include "scan_result_merge.h"

#include <iostream>

using universal_ble::DeriveConnectableState;
using universal_ble::NextScanResponseWait;
using universal_ble::ScanResponseWait;

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
  // Connectability derivation:
  // Connectable advertisements report connectable.
  CHECK(DeriveConnectableState(/*is_connectable=*/true,
                               /*is_scan_response=*/false,
                               /*address_heard_connectable=*/false) == true);

  // Non-connectable advertisements (e.g. beacons) report non-connectable.
  CHECK(DeriveConnectableState(/*is_connectable=*/false,
                               /*is_scan_response=*/false,
                               /*address_heard_connectable=*/false) == false);

  // An answered scan request inherits connectable if heard connectable.
  CHECK(DeriveConnectableState(/*is_connectable=*/false,
                               /*is_scan_response=*/true,
                               /*address_heard_connectable=*/true) == true);

  // A scan response for an address never heard connectable reports
  // non-connectable.
  CHECK(DeriveConnectableState(/*is_connectable=*/false,
                               /*is_scan_response=*/true,
                               /*address_heard_connectable=*/false) == false);

  // An advertisement whose connectability could not be read stays unknown
  // instead of being reported as "not connectable": a concrete `false` would
  // make connectableOnly drop every device on an affected host (#373).
  CHECK(DeriveConnectableState(/*is_connectable=*/std::nullopt,
                               /*is_scan_response=*/false,
                               /*address_heard_connectable=*/false) ==
        std::nullopt);

  // Unknown also stays unknown when nothing in the cache can answer for it.
  CHECK(DeriveConnectableState(/*is_connectable=*/std::nullopt,
                               /*is_scan_response=*/true,
                               /*address_heard_connectable=*/false) ==
        std::nullopt);

  // A scan response with an unanswered flag still inherits connectability from
  // the cache: that answer comes from the cache, not from the packet.
  CHECK(DeriveConnectableState(/*is_connectable=*/std::nullopt,
                               /*is_scan_response=*/true,
                               /*address_heard_connectable=*/true) == true);

  // An unanswered scan response must not promote a device already known to be
  // non-connectable either.
  CHECK(DeriveConnectableState(/*is_connectable=*/std::nullopt,
                               /*is_scan_response=*/false,
                               /*address_heard_connectable=*/true) ==
        std::nullopt);

  // Waiting for the first scan response:
  // The first scannable advertisement is held and starts the wait.
  {
    const auto step = NextScanResponseWait(ScanResponseWait::kNone,
                                           /*is_scan_response=*/false,
                                           /*expects_scan_response=*/true);
    CHECK(step.state == ScanResponseWait::kHeld);
    CHECK(!step.report);
    CHECK(step.start_timer);
  }
  // Repeats while held stay held, without another wait.
  {
    const auto step = NextScanResponseWait(ScanResponseWait::kHeld, false, true);
    CHECK(step.state == ScanResponseWait::kHeld);
    CHECK(!step.report);
    CHECK(!step.start_timer);
  }
  // The scan response ends the wait and is reported.
  {
    const auto step = NextScanResponseWait(ScanResponseWait::kHeld, true, false);
    CHECK(step.state == ScanResponseWait::kDone);
    CHECK(step.report);
    CHECK(!step.start_timer);
  }
  // A scan response with nothing held is reported too.
  CHECK(NextScanResponseWait(ScanResponseWait::kNone, true, false).report);
  // An advertisement that gets no scan response is reported at once.
  {
    const auto step = NextScanResponseWait(ScanResponseWait::kNone, false, false);
    CHECK(step.state == ScanResponseWait::kDone);
    CHECK(step.report);
  }
  // Once the wait is over, a device is never held again in that scan.
  {
    const auto step = NextScanResponseWait(ScanResponseWait::kDone, false, true);
    CHECK(step.report);
    CHECK(!step.start_timer);
  }

  return 0;
}
