#pragma once

#include <cstdint>
#include <exception>
#include <string>

#include "helper/universal_ble_base.h"
#include "generated/universal_ble.g.h"
#include "scan_filter_matcher.h"

namespace universal_ble {
// Translates the pigeon filter into the plain values the matcher works with.
ScanFilterValues ToScanFilter(const UniversalScanFilter &filter);
// Whether `scanResult` passes `filter`; an empty filter passes everything.
bool filterDevice(const ScanFilterValues &filter,
                  const UniversalBleScanResult &scanResult);
}  // namespace universal_ble
