#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace universal_ble {

/// Expands the UUID of an advertising service-data section (types `0x16`,
/// `0x20`, `0x21`) to the full lower-case 128-bit form used by Universal BLE.
///
/// Advertising data carries every multi-byte value little-endian (Bluetooth
/// Core Specification Supplement, Part A, section 1; Service Data is defined in
/// section 1.11), so a 128-bit UUID arrives least significant octet first and
/// all 16 bytes are reversed to get the canonical string. The 16- and 32-bit
/// branches read their bytes in that same order. `uuid_bytes` holds the UUID bytes only, without
/// the service-data payload. Returns an empty string when the section is too
/// short or `uuid_type` is not a service-data type.
///
/// Deliberately free of flutter/winrt types: the rules live here so they can be
/// unit tested without a Flutter engine (see
/// windows/test/advertisement_uuid_test.cpp).
std::string ExpandServiceUuid(const std::vector<uint8_t> &uuid_bytes,
                              uint8_t uuid_type);

/// Splits a whole service-data section (types `0x16`, `0x20`, `0x21`) into
/// its expanded UUID and the payload bytes after it. Returns nothing when the
/// section is shorter than its UUID or `section_type` is not service data.
std::optional<std::pair<std::string, std::vector<uint8_t>>>
ParseServiceData(const std::vector<uint8_t> &section, uint8_t section_type);

}  // namespace universal_ble
