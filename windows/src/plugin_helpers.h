#pragma once

// Helpers shared by the plugin's source files. Internal to the plugin.

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <memory>
#include <optional>
#include <string>

#include "enum_parser.h"
#include "generated/universal_ble.g.h"
#include "helper/universal_ble_logger.h"
#include "helper/utils.h"
#include "notification_failure_log.h"
#include "universal_ble_plugin.h"

namespace universal_ble {
namespace helpers {
template <typename Reply, typename Value>
void safe_reply(const char *context, const Reply &reply,
                Value &&value) noexcept {
  try {
    reply(std::forward<Value>(value));
  } catch (const hresult_error &err) {
    try {
      UniversalBleLogger::LogError(
          std::string(context) + " reply hresult_error hr=" +
          std::to_string(err.code()) + " msg=" + to_string(err.message()));
    } catch (...) {
    }
  } catch (const std::exception &err) {
    try {
      UniversalBleLogger::LogError(std::string(context) +
                                   " reply std::exception: " + err.what());
    } catch (...) {
    }
  } catch (...) {
    try {
      UniversalBleLogger::LogError(std::string(context) +
                                   " reply unknown exception");
    } catch (...) {
    }
  }
}

// Runs one cleanup step and logs its failure, so the steps after it still run.
template <typename Step>
void Attempt(const char *what, Step &&step) noexcept {
  try {
    try {
      step();
    } catch (const hresult_error &err) {
      UniversalBleLogger::LogError(
          std::string(what) + " hresult_error hr=" +
          std::to_string(err.code()) + " msg=" + to_string(err.message()));
    } catch (const std::exception &ex) {
      log_and_swallow(what, ex);
    } catch (...) {
      log_and_swallow_unknown(what);
    }
  } catch (...) {
    // Logging itself failed; there is nothing left to report with.
  }
}

// Applies Dart attribute permissions to WinRT characteristic or descriptor
// parameters, which share the same protection-level setters.
template <typename Parameters>
void ApplyPeripheralPermissions(const flutter::EncodableList &permissions,
                                Parameters &parameters) {
  for (const auto &encoded : permissions) {
    switch (std::any_cast<PeripheralAttributePermission>(
        std::get<flutter::CustomEncodableValue>(encoded))) {
      case PeripheralAttributePermission::kReadable:
        parameters.ReadProtectionLevel(GattProtectionLevel::Plain);
        break;
      case PeripheralAttributePermission::kWriteable:
        parameters.WriteProtectionLevel(GattProtectionLevel::Plain);
        break;
      case PeripheralAttributePermission::kReadEncryptionRequired:
        parameters.ReadProtectionLevel(GattProtectionLevel::EncryptionRequired);
        break;
      case PeripheralAttributePermission::kWriteEncryptionRequired:
        parameters.WriteProtectionLevel(
            GattProtectionLevel::EncryptionRequired);
        break;
    }
  }
}

// Maps the exception being handled to the error a Dart call fails with.
// Call only from inside a catch block.
inline FlutterError CurrentExceptionAsFlutterError() {
  try {
    throw;
  } catch (const FlutterError &err) {
    return err;
  } catch (const hresult_error &err) {
    return create_flutter_error(UniversalBleErrorCode::kFailed,
                                to_string(err.message()),
                                std::to_string(err.code()));
  } catch (const std::exception &ex) {
    return create_flutter_error(UniversalBleErrorCode::kUnknownError,
                                ex.what());
  } catch (...) {
    return create_flutter_unknown_error();
  }
}

// Logs the exception being handled. Call only from inside a catch block.
inline void LogCurrentException(const char *what) noexcept {
  Attempt(what, [] { throw; });
}

// A characteristic looked up from Windows' cache, or why it could not be.
//
// Every lookup returns new objects, but they share their service with every
// other lookup of it: Close() on one closes the service for all of them,
// subscriptions included. So looked-up objects are released, never closed.
// Only a disconnect closes anything, and then everything of the device.
struct CharacteristicLookup {
  GattCharacteristic characteristic{nullptr};
  std::optional<FlutterError> error;

  GattCharacteristic Take() {
    if (error.has_value()) {
      throw error.value();
    }
    return characteristic;
  }
};

// The first match in handle order wins when a UUID occurs more than once.
// Windows answers Cached with an empty result when it has not cached the
// device's database, for example when autoConnect brought the link up, so a
// miss is checked once more against the device.
inline IAsyncAction
LookupCharacteristicAsync(BluetoothLEDevice device, std::string service_uuid,
                          std::string characteristic_uuid,
                          std::shared_ptr<CharacteristicLookup> out) {
  const auto service_guid = uuid_to_guid(service_uuid);
  const auto characteristic_guid = uuid_to_guid(characteristic_uuid);
  for (const auto mode :
       {BluetoothCacheMode::Cached, BluetoothCacheMode::Uncached}) {
    const auto services =
        co_await device.GetGattServicesForUuidAsync(service_guid, mode);
    if (services.Status() != GattCommunicationStatus::Success) {
      out->error = create_flutter_error_from_gatt_result(
          services.Status(), services.ProtocolError());
      co_return;
    }
    if (services.Services().Size() == 0) {
      out->error = create_flutter_error(
          UniversalBleErrorCode::kServiceNotFound, "Service not found");
      continue;
    }
    const auto characteristics =
        co_await services.Services().GetAt(0).GetCharacteristicsForUuidAsync(
            characteristic_guid, mode);
    if (characteristics.Status() != GattCommunicationStatus::Success) {
      out->error = create_flutter_error_from_gatt_result(
          characteristics.Status(), characteristics.ProtocolError());
      co_return;
    }
    if (characteristics.Characteristics().Size() == 0) {
      out->error = create_flutter_error(
          UniversalBleErrorCode::kCharacteristicNotFound,
          "Characteristic not found");
      continue;
    }
    out->error.reset();
    out->characteristic = characteristics.Characteristics().GetAt(0);
    co_return;
  }
}

// Throws the error a GATT result's status maps to, unless it is Success.
template <typename Result>
void ThrowIfFailed(const char *context, const std::string &target,
                   const Result &result) {
  if (result.Status() == GattCommunicationStatus::Success) {
    return;
  }
  UniversalBleLogger::LogError(
      std::string(context) + " failed <- " + target +
      " status=" + std::to_string(static_cast<int>(result.Status())));
  throw create_flutter_error_from_gatt_result(result.Status(),
                                              result.ProtocolError());
}

// The error for the exception being handled by a GATT operation on
// `connection`: a closed connection reports the disconnect, which is what
// made the operation fail. Call only from inside a catch block.
inline FlutterError
GattFailure(const std::shared_ptr<Connection> &connection,
            const char *activity) {
  if (connection && connection->closed.load()) {
    return create_flutter_error(
        UniversalBleErrorCode::kDeviceDisconnected,
        std::string("Device disconnected during ") + activity);
  }
  return CurrentExceptionAsFlutterError();
}

inline GattDescriptor
FirstDescriptorOrThrow(const GattDescriptorsResult &found,
                       const std::string &descriptor) {
  if (found.Descriptors().Size() == 0) {
    throw create_flutter_error(UniversalBleErrorCode::kFailed,
                               "Descriptor not found: " + descriptor);
  }
  return found.Descriptors().GetAt(0);
}

// Thrown when a connection is found closed after an await. GattFailure turns
// it into the disconnect error.
struct ConnectionClosed {};

// A failed connection attempt. A permanent one is not retried by autoConnect.
struct ConnectFailure {
  std::string message;
  bool permanent = false;
};

// Pins this DLL for the life of the process. A worker that outlives the plugin
// keeps running plugin code, which a host that unloads the DLL would unmap.
inline bool KeepModuleLoaded() noexcept {
  static const wchar_t module_anchor = L'\0';
  HMODULE module = nullptr;
  // Avoid stream output here: a blocked log sink could stall shutdown again.
  return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_PIN,
                            &module_anchor, &module) != FALSE;
}

inline std::string to_lower_case(std::string value) {
  std::transform(
      value.begin(), value.end(), value.begin(),
      [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

inline IPropertyValue lookup_i_property_value(
    const IMapView<hstring, IInspectable> &properties, const hstring &key,
    const char *property_name, const char *context) {
  try {
    auto value = properties.Lookup(key).try_as<IPropertyValue>();
    if (!value) {
      UniversalBleLogger::LogError(std::string(context) +
                                   ": unexpected type for " + property_name +
                                   " property");
    }
    return value;
  } catch (const hresult_error &err) {
    UniversalBleLogger::LogError(
        std::string(context) + ": failed to lookup " + property_name +
        " property (hr=" + std::to_string(err.code()) + ")");
    return nullptr;
  }
}

// Throttles the notification failure log without holding plugin state: the
// completion handler runs on the WinRT thread pool and can outlive the plugin.
// Intentionally leaked so a handler that runs during teardown cannot touch a
// destroyed object.
inline NotificationFailureLog &notification_failure_log() {
  static auto *log = new NotificationFailureLog();
  return *log;
}

// Whether the missing-interface warning below still has to be logged. The
// advertisement callback runs once per received packet and a scan can deliver
// thousands of them, so the fallback would otherwise either spam the log or
// stay silent. It is logged once per process: #373 only reproduces on affected
// hosts, so a silent fallback leaves nothing to confirm it with in the field.
inline bool should_log_missing_connectable_interface() {
  static std::atomic_bool logged{false};
  return !logged.exchange(true);
}

inline const char *async_status_to_string(const AsyncStatus status) {
  switch (status) {
    case AsyncStatus::Started:
      return "Started";
    case AsyncStatus::Completed:
      return "Completed";
    case AsyncStatus::Canceled:
      return "Canceled";
    case AsyncStatus::Error:
      return "Error";
    default:
      return "Unknown";
  }
}
}  // namespace helpers
using namespace helpers;
}  // namespace universal_ble
