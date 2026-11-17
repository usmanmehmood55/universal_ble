// ReSharper disable CppTooWideScopeInitStatement
// ReSharper disable CppTooWideScope
#include "universal_ble_plugin.h"
#include <windows.h>

#include <flutter/plugin_registrar_windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>

#include "advertisement_uuid.h"
#include "enum_parser.h"
#include "callback_drain.h"
#include "generated/universal_ble.g.h"
#include "helper/universal_ble_logger.h"
#include "helper/universal_enum.h"
#include "helper/utils.h"
#include "notification_failure_log.h"
#include "pin_entry.h"
#include "scan_result_merge.h"
#include "universal_ble_filter_util.h"

namespace universal_ble {
using universal_ble::ErrorOr;
using universal_ble::UniversalBleCallbackChannel;
using universal_ble::UniversalBlePlatformChannel;
using universal_ble::UniversalBleScanResult;

const auto device_address_key = L"System.Devices.Aep.DeviceAddress";

namespace {
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
FlutterError CurrentExceptionAsFlutterError() {
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
void LogCurrentException(const char *what) noexcept {
  Attempt(what, [] { throw; });
}

// Names a GATT operation in replies and logs.
struct GattOperationName {
  const char *context;  // Log context, e.g. "ReadValue".
  const char *activity; // Lower-case noun for messages, e.g. "read".
  std::string target;   // Device, service and characteristic, for logs.
};

// Waits for `operation`, then passes its result to `on_success`. Replies an
// error instead when the device was disconnected meanwhile, the operation was
// cancelled or failed, a GATT result reports a status other than Success, or
// anything throws, including `on_success` itself before it replies.
template <typename Result, typename Reply, typename OnSuccess>
void OnGattCompleted(const IAsyncOperation<Result> &operation,
                     std::shared_ptr<BluetoothDeviceAgent> agent,
                     GattOperationName name, Reply result,
                     OnSuccess on_success) {
  operation.Completed([agent = std::move(agent), name = std::move(name),
                       result = std::move(result),
                       on_success = std::move(on_success)](
                          const IAsyncOperation<Result> &sender,
                          const AsyncStatus status) {
    try {
      if (!agent->IsActive()) {
        safe_reply(name.context, result,
                   create_flutter_error(
                       UniversalBleErrorCode::kDeviceDisconnected,
                       std::string("Device disconnected during ") +
                           name.activity));
        return;
      }
      if (status != AsyncStatus::Completed) {
        safe_reply(name.context, result,
                   create_flutter_error(UniversalBleErrorCode::kFailed,
                                        std::string(name.activity) +
                                            (status == AsyncStatus::Canceled
                                                 ? " was cancelled"
                                                 : " failed")));
        return;
      }
      const Result value = sender.GetResults();
      if constexpr (requires(const Result &r) {
                      r.Status();
                      r.ProtocolError();
                    }) {
        if (value.Status() != GattCommunicationStatus::Success) {
          UniversalBleLogger::LogError(
              std::string(name.context) + " failed <- " + name.target +
              " status=" + std::to_string(static_cast<int>(value.Status())));
          safe_reply(name.context, result,
                     create_flutter_error_from_gatt_result(
                         value.Status(), value.ProtocolError()));
          return;
        }
      }
      on_success(value);
    } catch (...) {
      safe_reply(name.context, result, CurrentExceptionAsFlutterError());
    }
  });
}

GattDescriptor FirstDescriptorOrThrow(const GattDescriptorsResult &found,
                                      const std::string &descriptor) {
  if (found.Descriptors().Size() == 0) {
    throw create_flutter_error(UniversalBleErrorCode::kFailed,
                               "Descriptor not found: " + descriptor);
  }
  return found.Descriptors().GetAt(0);
}

// Pins this DLL for the life of the process. A worker that outlives the plugin
// keeps running plugin code, which a host that unloads the DLL would unmap.
bool KeepModuleLoaded() noexcept {
  static const wchar_t module_anchor = L'\0';
  HMODULE module = nullptr;
  // Avoid stream output here: a blocked log sink could stall shutdown again.
  return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_PIN,
                            &module_anchor, &module) != FALSE;
}

std::string to_lower_case(std::string value) {
  std::transform(
      value.begin(), value.end(), value.begin(),
      [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

IPropertyValue lookup_i_property_value(
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
NotificationFailureLog &notification_failure_log() {
  static auto *log = new NotificationFailureLog();
  return *log;
}

// Whether the missing-interface warning below still has to be logged. The
// advertisement callback runs once per received packet and a scan can deliver
// thousands of them, so the fallback would otherwise either spam the log or
// stay silent. It is logged once per process: #373 only reproduces on affected
// hosts, so a silent fallback leaves nothing to confirm it with in the field.
bool should_log_missing_connectable_interface() {
  static std::atomic_bool logged{false};
  return !logged.exchange(true);
}

const char *async_status_to_string(const AsyncStatus status) {
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
}  // namespace

void UniversalBlePlugin::RegisterWithRegistrar(
    flutter::PluginRegistrarWindows *registrar) {
  auto plugin = std::make_unique<UniversalBlePlugin>(registrar);
  UniversalBlePlatformChannel::SetUp(registrar->messenger(), plugin.get());
  UniversalBlePeripheralChannel::SetUp(registrar->messenger(), plugin.get());
  registrar->AddPlugin(std::move(plugin));
}

UniversalBlePlugin::UniversalBlePlugin(
    flutter::PluginRegistrarWindows *registrar)
    : registrar_(registrar),
      callback_channel_(std::make_unique<UniversalBleCallbackChannel>(
          registrar->messenger())),
      peripheral_callback_channel_(
          std::make_unique<UniversalBlePeripheralCallback>(
              registrar->messenger())),
      ui_thread_handler_(registrar) {
  InitializeAsync();
}

void UniversalBlePlugin::PostCentralReport(
    std::function<void(UniversalBleCallbackChannel *)> &&report) {
  auto *channel = callback_channel_.get();
  ui_thread_handler_.Post(
      [channel, report = std::move(report)] { report(channel); });
}

void UniversalBlePlugin::PostPeripheralReport(
    std::function<void(UniversalBlePeripheralCallback *)> &&report) {
  auto *channel = peripheral_callback_channel_.get();
  ui_thread_handler_.Post(
      [channel, report = std::move(report)] { report(channel); });
}

UniversalBlePlugin::~UniversalBlePlugin() {
  // Coroutines release the plugin lease while suspended, so one can resume
  // after this object is gone. It then touches only what its own frame owns,
  // but it still runs code from this DLL, so the DLL must stay loaded. Flutter
  // runners link the plugin at build time and never unload it; this covers
  // hosts that load it dynamically.
  KeepModuleLoaded();
  // Stop new WinRT callbacks from entering, let callbacks which already hold a
  // lease finish while every member is still alive, then release owned state.
  callback_operations_.Close();
  initialization_operations_.Close();
  // Invariant: the raw system-operation Completed handler finishes
  // independently of this UI apartment and only queues publication. Do not
  // replace it with an apartment-affine continuation: this drain deliberately
  // does not pump. Teardown must wait for every acquired lease to avoid
  // destroying live state.
  while (!initialization_operations_.WaitUntilIdleFor(
      std::chrono::milliseconds(10))) {
  }
  WaitForCallbacksWithMessagePump(callback_operations_);
  {
    std::lock_guard<std::mutex> lock(peripheral_mutex_);
    DisposeAdvertisementPublisher();
  }
  ResetState();
  ClearServices();
  cleanup_operations_.Close();
  // Disposal owns its WinRT objects and leases, and never accesses the plugin
  // instance. A stalled WinRT call can outlive the engine if the DLL
  // stays loaded.
  // Keep disposal failure logs to diagnose handler removal and Close()
  // failures. If the DLL is pinned at the deadline, those logs may reach
  // stdout after engine teardown.
  DrainConnectionCleanup(cleanup_operations_, std::chrono::seconds(3),
                         KeepModuleLoaded);
  ui_thread_handler_.Shutdown();
  callback_channel_.reset();
  peripheral_callback_channel_.reset();
}

// UniversalBlePlatformChannel implementation.
void UniversalBlePlugin::GetBluetoothAvailabilityState(
    std::function<void(ErrorOr<AvailabilityState> reply)> result) {
  if (!bluetooth_radio_) {
    if (!initialized_) {
      result(AvailabilityState::kUnknown);
    } else {
      result(AvailabilityState::kUnsupported);
    }
  } else {
    result(get_availability_state_from_radio(bluetooth_radio_.State()));
  }
};

void UniversalBlePlugin::EnableBluetooth(
    std::function<void(ErrorOr<bool> reply)> result) {
  if (!bluetooth_radio_) {
    result(create_flutter_error(UniversalBleErrorCode::kBluetoothNotAvailable,
                                "Bluetooth is not available"));
    return;
  }

  if (bluetooth_radio_.State() == RadioState::On) {
    result(true);
    return;
  }

  bluetooth_radio_.SetStateAsync(RadioState::On)
      .Completed([result](const IAsyncOperation<RadioAccessStatus> &sender,
                          const AsyncStatus args) {
        try {
          if (args != AsyncStatus::Completed) {
            safe_reply(
                "EnableBluetooth", result,
                create_flutter_error(UniversalBleErrorCode::kFailed,
                                     args == AsyncStatus::Canceled
                                         ? "Enable bluetooth was cancelled"
                                         : "Failed to enable bluetooth"));
            return;
          }
          if (const auto radio_access_status = sender.GetResults();
              radio_access_status == RadioAccessStatus::Allowed) {
            safe_reply("EnableBluetooth", result, true);
          } else {
            safe_reply("EnableBluetooth", result,
                       create_flutter_error(UniversalBleErrorCode::kFailed,
                                            "Failed to enable bluetooth"));
          }
        } catch (const hresult_error &err) {
          safe_reply("EnableBluetooth", result,
                     create_flutter_error(UniversalBleErrorCode::kFailed,
                                          to_string(err.message()),
                                          std::to_string(err.code())));
        } catch (...) {
          safe_reply("EnableBluetooth", result, create_flutter_unknown_error());
        }
      });
}

void UniversalBlePlugin::RequestDevice(
    const AppleAccessorySetupOptions &,
    std::function<void(ErrorOr<UniversalDevicePickerResult> reply)> result) {
  result(create_flutter_error(UniversalBleErrorCode::kNotSupported,
                              "AccessorySetupKit is only supported on iOS 18+"));
}

void UniversalBlePlugin::DisableBluetooth(
    std::function<void(ErrorOr<bool> reply)> result) {
  if (!bluetooth_radio_) {
    result(create_flutter_error(UniversalBleErrorCode::kBluetoothNotAvailable,
                                "Bluetooth is not available"));
    return;
  }

  if (bluetooth_radio_.State() == RadioState::Off) {
    result(true);
    return;
  }

  bluetooth_radio_.SetStateAsync(RadioState::Off)
      .Completed([result](IAsyncOperation<RadioAccessStatus> const &sender,
                          AsyncStatus const args) {
        try {
          if (args != AsyncStatus::Completed) {
            safe_reply(
                "DisableBluetooth", result,
                create_flutter_error(UniversalBleErrorCode::kFailed,
                                     args == AsyncStatus::Canceled
                                         ? "Disable bluetooth was cancelled"
                                         : "Failed to disable bluetooth"));
            return;
          }
          if (const auto radio_access_status = sender.GetResults();
              radio_access_status == RadioAccessStatus::Allowed) {
            safe_reply("DisableBluetooth", result, true);
          } else {
            safe_reply("DisableBluetooth", result,
                       create_flutter_error(UniversalBleErrorCode::kFailed,
                                            "Failed to disable bluetooth"));
          }
        } catch (const hresult_error &err) {
          safe_reply("DisableBluetooth", result,
                     create_flutter_error(UniversalBleErrorCode::kFailed,
                                          to_string(err.message()),
                                          std::to_string(err.code())));
        } catch (...) {
          safe_reply("DisableBluetooth", result,
                     create_flutter_unknown_error());
        }
      });
}

ErrorOr<bool> UniversalBlePlugin::HasPermissions(
    bool with_android_fine_location) {
  // Windows does not require runtime permissions for Bluetooth
  return true;
}

void UniversalBlePlugin::RequestPermissions(
    bool with_android_fine_location,
    std::function<void(std::optional<FlutterError> reply)> result) {
  // Windows does not require runtime permissions for Bluetooth
  result(std::nullopt);
  return;
}

std::optional<FlutterError> UniversalBlePlugin::StartScan(
    const UniversalScanFilter *filter, const UniversalScanConfig *config) {
  if (!bluetooth_radio_ || bluetooth_radio_.State() != RadioState::On) {
    return create_flutter_error(UniversalBleErrorCode::kBluetoothNotAvailable,
                                "Bluetooth is not available");
  }

  try {
    scan_results_.clear();
    connectable_heard_.clear();
    LoadPairedDevicesAsync();

    // Setup LeWatcher and apply filters
    if (!bluetooth_le_watcher_) {
      bluetooth_le_watcher_ = BluetoothLEAdvertisementWatcher();
      bluetooth_le_watcher_.ScanningMode(BluetoothLEScanningMode::Active);
      // Guarded defensively: AllowExtendedAdvertisements requires
      // UniversalApiContract v10 (Windows 10 2004+), which not every supported
      // host provides. The #373 field dump showed construction and StartScan
      // completing on Windows 10 1909 before the advertisement callback ran, so
      // this call is not what faults on the affected host, and the guard stays
      // for hosts where the property is genuinely unavailable. Scans continue
      // with legacy advertisements only.
      try {
        bluetooth_le_watcher_.AllowExtendedAdvertisements(true);
      } catch (const hresult_error &err) {
        // Expected on Windows builds before 10.0.19041, or on adapters without
        // Bluetooth 5 support.
        UniversalBleLogger::LogWarning(
            "Extended advertisements unavailable hr=" +
            std::to_string(err.code()) + " msg=" + to_string(err.message()));
      } catch (...) {
        log_and_swallow_unknown("StartScan AllowExtendedAdvertisements");
      }
      const auto callback_operations = callback_operations_;
      bluetooth_le_watcher_received_token_ = bluetooth_le_watcher_.Received(
          [this, callback_operations](const auto &sender, const auto &args) {
            const auto callback = callback_operations.TryAcquire();
            if (!callback.has_value())
              return;
            BluetoothLeWatcherReceived(sender, args);
          });
      // Windows stops the watcher on its own when the radio goes off or the
      // adapter fails. Drop it then, so the next StartScan builds a new one.
      bluetooth_le_watcher_stopped_token_ = bluetooth_le_watcher_.Stopped(
          [this, callback_operations](
              const BluetoothLEAdvertisementWatcher &sender,
              const BluetoothLEAdvertisementWatcherStoppedEventArgs &args) {
            const auto callback = callback_operations.TryAcquire();
            if (!callback.has_value() ||
                args.Error() == BluetoothError::Success)
              return;
            UniversalBleLogger::LogWarning(
                "Scan stopped by Windows: " +
                ParsePeripheralBluetoothError(args.Error()));
            ui_thread_handler_.Post([this, sender] {
              if (bluetooth_le_watcher_ == sender)
                DisposeAdvertisementWatcher();
            });
          });
    }
    // Applied on every start, so a scan restarted without stopScan() does not
    // keep the previous filter.
    resetScanFilter();
    if (filter != nullptr) {
      UniversalBleLogger::LogInfo("Using Custom Scan Filter");
      setScanFilter(*filter);
    }
    bluetooth_le_watcher_.Start();
    return std::nullopt;
  } catch (const hresult_error &err) {
    StopScan();
    UniversalBleLogger::LogError(
        "StartScan hresult_error hr=" + std::to_string(err.code()) +
        " msg=" + to_string(err.message()));
    return create_flutter_error(UniversalBleErrorCode::kFailed,
                                to_string(err.message()),
                                std::to_string(err.code()));
  } catch (...) {
    StopScan();
    UniversalBleLogger::LogError("Unknown error StartScan");
    return create_flutter_error(UniversalBleErrorCode::kUnknownError,
                                "Unknown error");
  }
};

std::optional<FlutterError> UniversalBlePlugin::StopScan() {
  DisposeAdvertisementWatcher();
  scan_results_.clear();
  connectable_heard_.clear();
  return std::nullopt;
}

void UniversalBlePlugin::DisposeAdvertisementWatcher() noexcept {
  if (!bluetooth_le_watcher_)
    return;
  Attempt("Unregister advertisement handler", [&] {
    bluetooth_le_watcher_.Received(bluetooth_le_watcher_received_token_);
  });
  Attempt("Unregister advertisement stop handler", [&] {
    bluetooth_le_watcher_.Stopped(bluetooth_le_watcher_stopped_token_);
  });
  Attempt("Stop advertisement watcher", [&] { bluetooth_le_watcher_.Stop(); });
  bluetooth_le_watcher_ = nullptr;
}

ErrorOr<bool> UniversalBlePlugin::IsScanning() {
  if (bluetooth_le_watcher_ != nullptr) {
    try {
      return bluetooth_le_watcher_.Status() ==
             BluetoothLEAdvertisementWatcherStatus::Started;
    } catch (...) {
    }
  }
  return false;
}

ErrorOr<BleConnectionState> UniversalBlePlugin::GetConnectionState(
    const std::string &device_id) {
  try {
    const auto device_agent = GetConnectedDevice(str_to_mac_address(device_id));
    if (!device_agent || !device_agent->IsActive()) {
      return BleConnectionState::kDisconnected;
    }

    if (device_agent->device.ConnectionStatus() ==
        BluetoothConnectionStatus::Connected) {
      return BleConnectionState::kConnected;
    }
    return BleConnectionState::kDisconnected;
  } catch (...) {
    return CurrentExceptionAsFlutterError();
  }
}

std::optional<FlutterError> UniversalBlePlugin::SetLogLevel(
    const BleLogLevel &log_level) {
  UniversalBleLogger::SetLogLevel(log_level);
  return std::nullopt;
}

std::optional<FlutterError> UniversalBlePlugin::Connect(
    const std::string &device_id, const bool *auto_connect,
    const ConnectionPlatformConfig *platform_config) {
  // Note: platformConfig carries platform-specific options
  StartConnect(str_to_mac_address(device_id),
               auto_connect != nullptr && *auto_connect);
  return std::nullopt;
}

void UniversalBlePlugin::StartConnect(const uint64_t bluetooth_address,
                                      const bool auto_connect) {
  std::shared_ptr<BluetoothDeviceAgent> stale_device;
  std::optional<uint64_t> connect_generation;
  {
    std::lock_guard<std::mutex> lock(connected_devices_mutex_);
    const auto existing = connected_devices_.find(bluetooth_address);
    if (existing != connected_devices_.end()) {
      bool is_connected = false;
      try {
        is_connected = existing->second->IsActive() &&
                       existing->second->device.ConnectionStatus() ==
                           BluetoothConnectionStatus::Connected;
      } catch (const hresult_error &err) {
        UniversalBleLogger::LogWarning(
            "Connect: failed to inspect existing connection hr=" +
            std::to_string(err.code()));
      } catch (...) {
        UniversalBleLogger::LogWarning(
            "Connect: failed to inspect existing connection");
      }
      if (is_connected) {
        NotifyConnectionChanged(bluetooth_address, true, std::nullopt,
                                ConnectGenerationLocked(bluetooth_address));
        return;
      }

      stale_device = existing->second;
      ReserveConnectionCleanupLocked(stale_device);
      connected_devices_.erase(existing);
    }

    if (pending_connects_.find(bluetooth_address) == pending_connects_.end()) {
      connect_generation = ++next_connect_generation_;
      connect_generations_.insert_or_assign(bluetooth_address,
                                            connect_generation.value());
      pending_connects_.insert(bluetooth_address);
    }
  }

  DisposeConnection(stale_device);
  if (connect_generation.has_value()) {
    ConnectAsync(bluetooth_address, connect_generation.value(), auto_connect);
  }
}

std::optional<FlutterError> UniversalBlePlugin::Disconnect(
    const std::string &device_id) {
  const auto device_address = str_to_mac_address(device_id);
  InvalidateConnectAttempt(device_address);
  uint64_t disconnect_generation = 0;
  DisposeConnection(
      RemoveConnectedDevice(device_address, nullptr, &disconnect_generation));
  NotifyConnectionChanged(device_address, false, std::nullopt,
                          disconnect_generation);
  return std::nullopt;
}

void UniversalBlePlugin::DiscoverServices(
    const std::string &device_id, bool with_descriptors,
    const DiscoverServicesPlatformConfig * /*platform_config*/,
    std::function<void(ErrorOr<flutter::EncodableList> reply)> result) {
  // No GATT cache control on WinRT; platform_config is ignored.
  DiscoverServicesAsync(device_id, with_descriptors, result);
}

void UniversalBlePlugin::SetNotifiable(
    const std::string &device_id, const std::string &service,
    const std::string &characteristic,
    const BleInputProperty &ble_input_property,
    std::function<void(std::optional<FlutterError> reply)> result) {
  SetNotifiableAsync(device_id, service, characteristic, ble_input_property,
                     result);
};

std::shared_ptr<BluetoothDeviceAgent>
UniversalBlePlugin::ConnectedDeviceOrThrow(const std::string &device_id) {
  auto device_agent = GetConnectedDevice(str_to_mac_address(device_id));
  if (!device_agent) {
    throw create_flutter_error(UniversalBleErrorCode::kDeviceNotFound,
                               "Unknown deviceId: " + device_id);
  }
  return device_agent;
}

void UniversalBlePlugin::ReadValue(
    const std::string &device_id, const std::string &service,
    const std::string &characteristic,
    std::function<void(ErrorOr<std::vector<uint8_t>> reply)> result) {
  const auto target = device_id + " " + service + " " + characteristic;
  UniversalBleLogger::LogDebugWithTimestamp("READ -> " + target);
  try {
    const auto agent = ConnectedDeviceOrThrow(device_id);
    const auto lease =
        agent->FetchCharacteristicForOperation(service, characteristic);
    const auto &gatt_characteristic = lease.characteristic.obj;
    if ((gatt_characteristic.CharacteristicProperties() &
         GattCharacteristicProperties::Read) ==
        GattCharacteristicProperties::None) {
      result(create_flutter_error(
          UniversalBleErrorCode::kCharacteristicDoesNotSupportRead,
          "Characteristic does not support read"));
      return;
    }
    OnGattCompleted(
        gatt_characteristic.ReadValueAsync(BluetoothCacheMode::Uncached),
        agent, {"ReadValue", "read", target}, result,
        [lease, result](const GattReadResult &read) {
          safe_reply("ReadValue", result, to_bytevc(read.Value()));
        });
  } catch (...) {
    result(CurrentExceptionAsFlutterError());
  }
}

void UniversalBlePlugin::WriteValue(
    const std::string &device_id, const std::string &service,
    const std::string &characteristic, const std::vector<uint8_t> &value,
    const BleOutputProperty &ble_output_property,
    std::function<void(std::optional<FlutterError> reply)> result) {
  const auto target = device_id + " " + service + " " + characteristic;
  UniversalBleLogger::LogDebugWithTimestamp(
      "WRITE -> " + target + " len=" + std::to_string(value.size()) +
      " property=" + std::to_string(static_cast<int>(ble_output_property)));
  try {
    const auto agent = ConnectedDeviceOrThrow(device_id);
    const auto lease =
        agent->FetchCharacteristicForOperation(service, characteristic);
    const auto &gatt_characteristic = lease.characteristic.obj;
    const auto properties = gatt_characteristic.CharacteristicProperties();
    const bool without_response =
        ble_output_property == BleOutputProperty::kWithoutResponse;
    if (without_response &&
        (properties & GattCharacteristicProperties::WriteWithoutResponse) ==
            GattCharacteristicProperties::None) {
      result(create_flutter_error(
          UniversalBleErrorCode::
              kCharacteristicDoesNotSupportWriteWithoutResponse,
          "Characteristic does not support WriteWithoutResponse"));
      return;
    }
    if (!without_response && (properties & GattCharacteristicProperties::Write) ==
                                 GattCharacteristicProperties::None) {
      result(create_flutter_error(
          UniversalBleErrorCode::kCharacteristicDoesNotSupportWrite,
          "Characteristic does not support Write"));
      return;
    }
    OnGattCompleted(
        gatt_characteristic.WriteValueWithResultAsync(
            from_bytevc(value), without_response
                                    ? GattWriteOption::WriteWithoutResponse
                                    : GattWriteOption::WriteWithResponse),
        agent, {"WriteValue", "write", target}, result,
        [lease, result](const GattWriteResult &) {
          safe_reply("WriteValue", result, std::nullopt);
        });
  } catch (...) {
    result(CurrentExceptionAsFlutterError());
  }
}

void UniversalBlePlugin::ReadDescriptorValue(
    const std::string &device_id, const std::string &service,
    const std::string &characteristic, const std::string &descriptor,
    std::function<void(ErrorOr<std::vector<uint8_t>> reply)> result) {
  const auto target =
      device_id + " " + service + " " + characteristic + " " + descriptor;
  UniversalBleLogger::LogDebugWithTimestamp("READ_DESCRIPTOR -> " + target);
  try {
    const auto agent = ConnectedDeviceOrThrow(device_id);
    const auto lease =
        agent->FetchCharacteristicForOperation(service, characteristic);
    const GattOperationName name{"ReadDescriptorValue", "descriptor read",
                                 target};
    OnGattCompleted(
        lease.characteristic.obj.GetDescriptorsForUuidAsync(
            uuid_to_guid(descriptor), BluetoothCacheMode::Uncached),
        agent, name, result,
        [agent, lease, name, result,
         descriptor](const GattDescriptorsResult &found) {
          OnGattCompleted(FirstDescriptorOrThrow(found, descriptor)
                              .ReadValueAsync(BluetoothCacheMode::Uncached),
                          agent, name, result,
                          [lease, result](const GattReadResult &read) {
                            safe_reply("ReadDescriptorValue", result,
                                       to_bytevc(read.Value()));
                          });
        });
  } catch (...) {
    result(CurrentExceptionAsFlutterError());
  }
}

void UniversalBlePlugin::WriteDescriptorValue(
    const std::string &device_id, const std::string &service,
    const std::string &characteristic, const std::string &descriptor,
    const std::vector<uint8_t> &value,
    std::function<void(std::optional<FlutterError> reply)> result) {
  const auto target =
      device_id + " " + service + " " + characteristic + " " + descriptor;
  UniversalBleLogger::LogDebugWithTimestamp(
      "WRITE_DESCRIPTOR -> " + target + " len=" + std::to_string(value.size()));
  try {
    const auto agent = ConnectedDeviceOrThrow(device_id);
    const auto lease =
        agent->FetchCharacteristicForOperation(service, characteristic);
    const GattOperationName name{"WriteDescriptorValue", "descriptor write",
                                 target};
    OnGattCompleted(
        lease.characteristic.obj.GetDescriptorsForUuidAsync(
            uuid_to_guid(descriptor), BluetoothCacheMode::Uncached),
        agent, name, result,
        [agent, lease, name, result, descriptor,
         value](const GattDescriptorsResult &found) {
          OnGattCompleted(FirstDescriptorOrThrow(found, descriptor)
                              .WriteValueWithResultAsync(from_bytevc(value)),
                          agent, name, result,
                          [lease, result](const GattWriteResult &) {
                            safe_reply("WriteDescriptorValue", result,
                                       std::nullopt);
                          });
        });
  } catch (...) {
    result(CurrentExceptionAsFlutterError());
  }
}

void UniversalBlePlugin::RequestMtu(
    const std::string &device_id, int64_t expected_mtu,
    std::function<void(ErrorOr<int64_t> reply)> result) {
  UniversalBleLogger::LogDebugWithTimestamp(
      "REQUEST_MTU -> " + device_id +
      " expected=" + std::to_string(expected_mtu));
  try {
    const auto agent = ConnectedDeviceOrThrow(device_id);
    // Windows negotiates the MTU itself; this reports the current value.
    OnGattCompleted(
        GattSession::FromDeviceIdAsync(agent->device.BluetoothDeviceId()),
        agent, {"RequestMtu", "MTU query", device_id}, result,
        [result](const GattSession &session) {
          safe_reply("RequestMtu", result,
                     static_cast<int64_t>(session.MaxPduSize()));
        });
  } catch (...) {
    result(CurrentExceptionAsFlutterError());
  }
}

void UniversalBlePlugin::RequestConnectionPriority(
    const std::string &device_id, const BleConnectionPriority &priority,
    std::function<void(std::optional<FlutterError> reply)> result) {
  result(create_flutter_error(
      UniversalBleErrorCode::kNotSupported,
      "requestConnectionPriority is not supported on Windows platform"));
}

void UniversalBlePlugin::ReadRssi(
    const std::string &device_id,
    std::function<void(ErrorOr<int64_t> reply)> result) {
  result(
      create_flutter_error(UniversalBleErrorCode::kNotImplemented,
                           "readRssi is not implemented on Windows platform"));
}

void UniversalBlePlugin::IsPaired(
    const std::string &device_id,
    std::function<void(ErrorOr<bool> reply)> result) {
  IsPairedAsync(device_id, result);
}

void UniversalBlePlugin::Pair(const std::string &device_id,
                              std::function<void(ErrorOr<bool> reply)> result) {
  // Windows 11 shows its own pairing dialog. Earlier releases need the
  // ceremony handled here.
  PairAsync(device_id, !is_windows11_or_greater(), result);
}

void UniversalBlePlugin::UnPair(
    const std::string &device_id,
    std::function<void(std::optional<FlutterError> reply)> result) {
  UnPairAsync(device_id, result);
}

// Awaited rather than waited on: a blocking wait here would freeze Flutter's
// platform thread for as long as Windows takes to unpair.
fire_and_forget UniversalBlePlugin::UnPairAsync(
    std::string device_id,
    std::function<void(std::optional<FlutterError> reply)> result) {
  // Touches no plugin state; the lease is taken only to reply, since a
  // reply after shutdown would go through a channel that is gone.
  ScopedLease plugin(callback_operations_);
  const auto reply = [&](std::optional<FlutterError> value) {
    if (plugin.Acquire())
      safe_reply("UnPair", result, std::move(value));
  };
  try {
    const auto device = co_await BluetoothLEDevice::FromBluetoothAddressAsync(
        str_to_mac_address(device_id));
    if (device == nullptr) {
      reply(create_flutter_error(UniversalBleErrorCode::kDeviceNotFound,
                                  "Unknown deviceId: " + device_id));
      co_return;
    }
    const auto pairing = device.DeviceInformation().Pairing();
    if (!pairing.IsPaired()) {
      reply(create_flutter_error(UniversalBleErrorCode::kNotPaired,
                                  "Device is not paired"));
      co_return;
    }
    const auto status = (co_await pairing.UnpairAsync()).Status();
    if (status != DeviceUnpairingResultStatus::Unpaired) {
      reply(create_flutter_error_from_unpairing_status(status));
      co_return;
    }
    reply(std::nullopt);
  } catch (...) {
    reply(CurrentExceptionAsFlutterError());
  }
}

void UniversalBlePlugin::GetSystemDevices(
    const flutter::EncodableList &with_services,
    std::function<void(ErrorOr<flutter::EncodableList> reply)> result) {
  auto with_services_str = std::vector<std::string>();
  for (const auto &item : with_services) {
    auto service_id = std::get<std::string>(item);
    with_services_str.push_back(service_id);
  }
  GetSystemDevicesAsync(with_services_str, result);
}

/// Helper Methods

void UniversalBlePlugin::InitializeAsync() {
  const auto initialization_operations = initialization_operations_;
  const auto publish_result = [this, initialization_operations](
                                  const Radio &radio) noexcept {
    try {
      ui_thread_handler_.Post([this, initialization_operations, radio] {
        const auto callback = initialization_operations.TryAcquire();
        if (!callback.has_value())
          return;
        try {
          if (radio) {
            bluetooth_radio_ = radio;
            const auto callback_operations = callback_operations_;
            radio_state_changed_revoker_ = bluetooth_radio_.StateChanged(
                auto_revoke, [this, callback_operations](const auto &sender,
                                                         const auto &args) {
                  const auto callback = callback_operations.TryAcquire();
                  if (!callback.has_value())
                    return;
                  RadioStateChanged(sender, args);
                });
            RadioStateChanged(bluetooth_radio_, nullptr);
            LoadPeripheralSupportAsync();
          } else {
            UniversalBleLogger::LogError("Bluetooth is not available");
            callback_channel_->OnAvailabilityChanged(
                AvailabilityState::kUnsupported, SuccessCallback,
                ErrorCallback);
          }
        } catch (...) {
          log_and_swallow_unknown("Bluetooth initialization publication");
        }
        initialized_ = true;
      });
    } catch (...) {
      // Queue construction can fail too. Never let an exception escape the
      // WinRT completion delegate, including when diagnostic logging fails.
      try {
        log_and_swallow_unknown("Bluetooth initialization queue publication");
      } catch (...) {
      }
    }
  };

  try {
    // Do not keep a lease or capture the UI apartment while enumeration is
    // pending. An early close must not pump engine messages to finish startup.
    Radio::GetRadiosAsync().Completed([initialization_operations,
                                       publish_result](const auto &operation,
                                                       const auto &) {
      // The radio queries are synchronous WinRT calls that can stall, so they
      // run without the lease: they touch nothing the plugin owns. The lease
      // is taken only to publish.
      Radio selected_radio{nullptr};
      try {
        for (auto &&radio : operation.GetResults()) {
          if (radio.Kind() == RadioKind::Bluetooth) {
            selected_radio = radio;
            break;
          }
        }
      } catch (...) {
        LogCurrentException("Bluetooth initialization");
      }
      const auto callback = initialization_operations.TryAcquire();
      if (!callback.has_value())
        return;
      publish_result(selected_radio);
    });
  } catch (...) {
    LogCurrentException("Bluetooth initialization");
    publish_result(nullptr);
  }
}

// The plugin lease is released while Windows pairs: that can wait on the user
// for as long as the dialog is open and must not hold up shutdown.
fire_and_forget UniversalBlePlugin::PairAsync(
    std::string device_id, bool custom_ceremony,
    std::function<void(ErrorOr<bool> reply)> result) {
  ScopedLease plugin(callback_operations_);
  if (!plugin.Acquire())
    co_return;
  bool replied = false;
  try {
    const auto bluetooth_address = str_to_mac_address(device_id);
    plugin.Release();
    const auto device =
        co_await BluetoothLEDevice::FromBluetoothAddressAsync(bluetooth_address);
    if (!plugin.Acquire())
      co_return;
    replied = true;
    if (device == nullptr) {
      result(create_flutter_error(UniversalBleErrorCode::kDeviceNotFound,
                                  "Unknown deviceId: " + device_id));
      co_return;
    }
    const auto pairing = device.DeviceInformation().Pairing();
    if (pairing.IsPaired()) {
      result(true);
      co_return;
    }
    if (!pairing.CanPair()) {
      result(create_flutter_error(UniversalBleErrorCode::kNotPairable,
                                  "Device is not pairable"));
      co_return;
    }
    replied = false;

    UniversalBleLogger::LogInfo("PairLog: Trying to pair");
    DevicePairingResult pair_result{nullptr};
    plugin.Release();
    if (custom_ceremony) {
      const auto custom_pairing = pairing.Custom();
      const auto pairing_requested_revoker = custom_pairing.PairingRequested(
          auto_revoke, [](const DeviceInformationCustomPairing &,
                          const DevicePairingRequestedEventArgs &args) {
            PairingRequestedHandler(args);
          });
      // DevicePairingKinds => None, ConfirmOnly, DisplayPin, ProvidePin,
      // ConfirmPinMatch, ProvidePasswordCredential
      pair_result = co_await custom_pairing.PairAsync(
          DevicePairingKinds::ConfirmOnly | DevicePairingKinds::ProvidePin,
          pairing.ProtectionLevel());
    } else {
      pair_result = co_await pairing.PairAsync();
    }
    if (!plugin.Acquire())
      co_return;

    const auto status = pair_result.Status();
    const bool is_paired = status == DevicePairingResultStatus::Paired;
    replied = true;
    result(is_paired);
    PostCentralReport(
        [device_id, is_paired, error = device_pairing_result_to_string(status)](
            UniversalBleCallbackChannel *channel) {
          channel->OnPairStateChange(device_id, is_paired,
                                     error.has_value() ? &error.value()
                                                       : nullptr,
                                     SuccessCallback, ErrorCallback);
        });
  } catch (...) {
    if (!plugin.Held() && !plugin.Acquire())
      co_return;
    UniversalBleLogger::LogError("PairLog: pairing failed");
    if (!replied)
      result(false);
  }
}

void UniversalBlePlugin::PairingRequestedHandler(
    const DevicePairingRequestedEventArgs &event_args) {
  UniversalBleLogger::LogInfo("PairLog: Got PairingRequest");
  const DevicePairingKinds kind = event_args.PairingKind();
  if (kind != DevicePairingKinds::ProvidePin) {
    event_args.Accept();
    return;
  }

  UniversalBleLogger::LogInfo("PairLog: Trying to get pin from user");
  const hstring pin = askForPairingPin();
  UniversalBleLogger::LogInfo("PairLog: Got PIN from user");
  event_args.Accept(pin);
}

// Send device to callback channel
// if device is already discovered in deviceWatcher then merge the scan result
//
// A scan response is not a stray broadcast: it is the answer to the scan
// request the host itself sent, and it belongs to a device that has just
// advertised. A peripheral may publish part of its data there - a 128-bit
// service UUID that no longer fits in the advertisement next to its name, for
// instance - and Windows reports that packet with `IsConnectable == false`, so
// this gate used to drop it. The caller then never saw the service, while
// Android, BlueZ, CoreBluetooth and bleak all merge the two packets of a
// device into one result.
void UniversalBlePlugin::PushUniversalScanResult(
    UniversalBleScanResult scan_result,
    const std::optional<bool> is_connectable, const bool is_scan_response) {
  if (is_connectable.value_or(false)) {
    connectable_heard_.insert_or_assign(scan_result.device_id(), true);
  }

  // Presence is derived from emptiness, not nullness: the advertisement
  // watcher sets `services` unconditionally, so an empty non-null list means
  // "carries nothing" just like a null one.
  auto has_items = [](const auto *value) {
    return value != nullptr && !value->empty();
  };
  ScanFieldPresence incoming;
  incoming.name = scan_result.name() != nullptr && !scan_result.name()->empty();
  incoming.is_paired = scan_result.is_paired() != nullptr;
  incoming.manufacturer_data = has_items(scan_result.manufacturer_data_list());
  incoming.services = has_items(scan_result.services());
  incoming.service_data = has_items(scan_result.service_data());

  const std::optional<UniversalBleScanResult> it =
      scan_results_.get(scan_result.device_id());
  const bool address_heard_connectable =
      connectable_heard_.get(scan_result.device_id()).value_or(false) ||
      (it.has_value() && it.value().connectable() != nullptr &&
       *it.value().connectable());
  const std::optional<bool> device_is_connectable = DeriveConnectableState(
      is_connectable, is_scan_response, address_heard_connectable);
  if (device_is_connectable.has_value()) {
    scan_result.set_connectable(*device_is_connectable);
  } else {
    // Unknown stays unknown: the packet did not answer the question, and a
    // concrete `false` would make `connectableOnly` drop every device on a host
    // where the query fails (#373). The cast selects the Pigeon pointer setter,
    // whose null value means "no value".
    scan_result.set_connectable(static_cast<const bool *>(nullptr));
  }

  if (it.has_value()) {
    const UniversalBleScanResult &current_scan_result = it.value();

    // Check if current scanResult name is longer than the received scanResult
    // name
    if (incoming.name && current_scan_result.name() != nullptr &&
        !current_scan_result.name()->empty() &&
        current_scan_result.name()->size() > scan_result.name()->size()) {
      scan_result.set_name(*current_scan_result.name());
    }

    ScanFieldPresence cached;
    cached.name = current_scan_result.name() != nullptr &&
                  !current_scan_result.name()->empty();
    cached.is_paired = current_scan_result.is_paired() != nullptr;
    // A cached list that is null *or* empty counts as absent, the same rule the
    // incoming packet uses above: copying an empty list over a packet that
    // carries nothing is not a reason to deliver.
    cached.manufacturer_data =
        has_items(current_scan_result.manufacturer_data_list());
    cached.services = has_items(current_scan_result.services());
    cached.service_data = has_items(current_scan_result.service_data());

    // Backfill cached fields the incoming packet does not carry, so a sparse
    // scan response cannot evict data the advertisement already supplied.
    const ScanBackfillPlan plan = PlanScanBackfill(incoming, cached);
    if (plan.name) {
      scan_result.set_name(*current_scan_result.name());
    }
    if (plan.is_paired) {
      scan_result.set_is_paired(current_scan_result.is_paired());
    }
    if (plan.manufacturer_data) {
      scan_result.set_manufacturer_data_list(
          current_scan_result.manufacturer_data_list());
    }
    if (plan.services) {
      scan_result.set_services(current_scan_result.services());
    }
    if (plan.service_data) {
      scan_result.set_service_data(current_scan_result.service_data());
    }

    // A changed connectable state is an update in itself: a device whose
    // connectability state changes must reach the caller, even when the packet
    // carries nothing else new. Comparing the optional values makes "unknown"
    // a state of its own, so a host that stops answering is visible instead of
    // being read as a change to `false`.
    const std::optional<bool> current_connectable =
        current_scan_result.connectable() == nullptr
            ? std::nullopt
            : std::optional<bool>(*current_scan_result.connectable());
    const bool connectable_changed =
        current_connectable != device_is_connectable;

    // if nothing to update then return
    if (!ShouldUpdateScanResult(plan, is_scan_response, connectable_changed)) {
      return;
    }
  }

  // Update cache
  scan_results_.insert_or_assign(scan_result.device_id(), scan_result);

  // Filter final result before sending to Flutter
  if (filterDevice(scan_result)) {
    const auto timestamp_microseconds = GetCurrentTimestampMicros();
    scan_result.set_timestamp(timestamp_microseconds / 1000);
    scan_result.set_timestamp_microseconds(timestamp_microseconds);
    PostCentralReport([scan_result](UniversalBleCallbackChannel *channel) {
      channel->OnScanResult(scan_result, SuccessCallback, ErrorCallback);
    });
  }
}

// Scan results report whether a device is paired. Windows has no pairing
// state in an advertisement, so the paired LE devices are listed once per scan
// and matched by address. This replaces a DeviceWatcher that ran alongside the
// advertisement watcher and was torn down from its own callback thread.
fire_and_forget UniversalBlePlugin::LoadPairedDevicesAsync() {
  ScopedLease plugin(callback_operations_);
  if (!plugin.Acquire())
    co_return;
  try {
    paired_devices_loaded_ = false;
    plugin.Release();
    const auto devices = co_await DeviceInformation::FindAllAsync(
        BluetoothLEDevice::GetDeviceSelectorFromPairingState(true),
        {device_address_key});
    if (!plugin.Acquire())
      co_return;
    paired_devices_.clear();
    for (auto &&device_info : devices) {
      const auto properties = device_info.Properties();
      if (!properties.HasKey(device_address_key))
        continue;
      const auto address = lookup_i_property_value(
          properties, device_address_key, "DeviceAddress", "LoadPairedDevices");
      if (address) {
        paired_devices_.insert_or_assign(
            to_lower_case(to_string(address.GetString())),
            to_string(device_info.Name()));
      }
    }
    paired_devices_loaded_ = true;
  } catch (...) {
    // Best effort: without the list, scan results leave pairing state unset.
    LogCurrentException("LoadPairedDevices");
  }
}

/// Advertisement received from advertisementWatcher
void UniversalBlePlugin::BluetoothLeWatcherReceived(
    const BluetoothLEAdvertisementWatcher &,
    const BluetoothLEAdvertisementReceivedEventArgs &args) {
  try {
    auto device_id = mac_address_to_str(args.BluetoothAddress());
    auto universal_scan_result = UniversalBleScanResult(device_id);
    std::string name = to_string(args.Advertisement().LocalName());

    auto manufacturer_data_encodable_list = flutter::EncodableList();
    if (args.Advertisement() != nullptr) {
      for (BluetoothLEManufacturerData msd :
           args.Advertisement().ManufacturerData()) {
        auto universal_manufacturer_data = UniversalManufacturerData(
            static_cast<int64_t>(msd.CompanyId()), to_bytevc(msd.Data()));
        manufacturer_data_encodable_list.push_back(
            flutter::CustomEncodableValue(universal_manufacturer_data));
      }
    }

    auto service_data_map = flutter::EncodableMap();
    auto data_section = args.Advertisement().DataSections();
    for (auto &&data : data_section) {
      auto data_bytes = to_bytevc(data.Data());
      auto data_type = data.DataType();

      // Use CompleteName from dataType if localName is empty
      if (name.empty() &&
          data_type == static_cast<uint8_t>(
                           AdvertisementSectionType::CompleteLocalName)) {
        name = std::string(data_bytes.begin(), data_bytes.end());
      }
      // Use ShortenedLocalName from dataType if localName is empty
      else if (name.empty() &&
               data_type == static_cast<uint8_t>(
                                AdvertisementSectionType::ShortenedLocalName)) {
        name = std::string(data_bytes.begin(), data_bytes.end());
      }
      // Extract service data
      else if (const auto service_data =
                   ParseServiceData(data_bytes, data_type)) {
        service_data_map[service_data->first] =
            flutter::EncodableValue(service_data->second);
      }
    }

    if (!name.empty()) {
      universal_scan_result.set_name(name);
    }

    if (!manufacturer_data_encodable_list.empty()) {
      universal_scan_result.set_manufacturer_data_list(
          manufacturer_data_encodable_list);
    }

    universal_scan_result.set_rssi(args.RawSignalStrengthInDBm());

    // Add services
    auto services = flutter::EncodableList();
    for (auto &&uuid : args.Advertisement().ServiceUuids())
      services.push_back(guid_to_uuid(uuid));
    universal_scan_result.set_services(services);

    // Add service data
    if (!service_data_map.empty()) {
      universal_scan_result.set_service_data(&service_data_map);
    }

    // Pairing state and a fallback name come from the paired-device list
    // loaded when the scan started. Until it has loaded, the state is unknown
    // and is left unset.
    const auto paired_name = paired_devices_.get(device_id);
    if (paired_name.has_value()) {
      universal_scan_result.set_is_paired(true);
      if (name.empty() && !paired_name.value().empty())
        universal_scan_result.set_name(paired_name.value());
    } else if (paired_devices_loaded_.load()) {
      universal_scan_result.set_is_paired(false);
    }

    // The scan-response flag comes from the advertisement type and not from
    // BluetoothLEAdvertisementReceivedEventArgs::IsScanResponse: the property
    // exists only since Windows 10 2004 (UniversalApiContract v10), so on
    // older releases - including supported LTSC builds - reading it inside
    // this callback fails instead of returning a value, and the catch around
    // this block would then drop every advertisement.
    // BluetoothLEAdvertisementType::ScanResponse has shipped with every
    // Windows 10.
    const bool is_scan_response =
        args.AdvertisementType() == BluetoothLEAdvertisementType::ScanResponse;
    // IsConnectable lives on the secondary interface
    // IBluetoothLEAdvertisementReceivedEventArgs2, which C++/WinRT reaches
    // through `require`: its conversion is noexcept, so a failed
    // QueryInterface leaves a null object instead of throwing, and calling the
    // property then faults on a null vtable. The access violation bypasses the
    // catch below and kills the process (#373). Query the interface explicitly
    // and keep the state unknown (null) when the query fails: the answer is
    // unavailable, so the caller is told nothing rather than "not connectable".
    std::optional<bool> connectable;
    const auto connectable_args =
        args.try_as<IBluetoothLEAdvertisementReceivedEventArgs2>();
    if (connectable_args) {
      connectable = connectable_args.IsConnectable();
    } else if (should_log_missing_connectable_interface()) {
      UniversalBleLogger::LogWarning(
          "IBluetoothLEAdvertisementReceivedEventArgs2 unavailable; reporting "
          "connectability as unknown (#373)");
    }
    // A scan response reports IsConnectable == false by definition, so trust
    // that flag only for regular advertisements; PushUniversalScanResult
    // derives the connectable state of a scan response from the cache.
    if (!is_scan_response && connectable.has_value()) {
      universal_scan_result.set_connectable(*connectable);
    }
    // Filter Device
    PushUniversalScanResult(universal_scan_result, connectable,
                            is_scan_response);
  } catch (...) {
    UniversalBleLogger::LogError("ScanResultErrorInParsing");
  }
}

void UniversalBlePlugin::RadioStateChanged(const Radio &sender,
                                           const IInspectable &) {
  try {
    const auto radio_state = !sender ? RadioState::Disabled : sender.State();
    if (old_radio_state_ == radio_state) {
      return;
    }
    old_radio_state_ = radio_state;
    auto state = get_availability_state_from_radio(radio_state);

    PostCentralReport([state](UniversalBleCallbackChannel *channel) {
      channel->OnAvailabilityChanged(state, SuccessCallback, ErrorCallback);
    });
  } catch (...) {
    LogCurrentException("RadioStateChanged");
  }
}

void UniversalBlePlugin::NotifyConnectionChanged(
    const uint64_t bluetooth_address, const bool connected,
    std::optional<std::string> error,
    std::optional<uint64_t> expected_generation) {
  uint64_t event_generation = 0;
  if (expected_generation.has_value()) {
    event_generation = expected_generation.value();
  } else {
    std::lock_guard<std::mutex> lock(connected_devices_mutex_);
    event_generation = ConnectGenerationLocked(bluetooth_address);
  }

  ui_thread_handler_.Post([this, bluetooth_address, connected,
                           error = std::move(error), event_generation] {
    {
      std::lock_guard<std::mutex> lock(connected_devices_mutex_);
      if (ConnectGenerationLocked(bluetooth_address) != event_generation) {
        UniversalBleLogger::LogDebug(
            "Ignoring a queued connection event from an older generation");
        return;
      }
      if (!connected && pending_connects_.find(bluetooth_address) !=
                            pending_connects_.end()) {
        UniversalBleLogger::LogDebug(
            "Ignoring a queued disconnect event during a newer connection "
            "attempt");
        return;
      }
    }
    const std::string *error_ptr = error.has_value() ? &error.value() : nullptr;
    // WinRT reports no numeric code for connection failures.
    callback_channel_->OnConnectionChanged(
        mac_address_to_str(bluetooth_address), connected, error_ptr, nullptr,
        nullptr, SuccessCallback, ErrorCallback);
  });
}

void UniversalBlePlugin::NotifyConnectionException(
    const uint64_t bluetooth_address, const std::string &error_message,
    const BluetoothLEDevice *expected_device) {
  UniversalBleLogger::LogError(error_message);
  if (bluetooth_address != 0) {
    uint64_t disconnect_generation = 0;
    if (expected_device == nullptr) {
      CleanConnection(bluetooth_address, nullptr, &disconnect_generation);
      NotifyConnectionChanged(bluetooth_address, false, error_message,
                              disconnect_generation);
    } else if (CleanConnection(bluetooth_address, expected_device,
                               &disconnect_generation)) {
      NotifyConnectionChanged(bluetooth_address, false, error_message,
                              disconnect_generation);
    } else {
      UniversalBleLogger::LogDebug(
          "Ignoring exception from stale Bluetooth device instance");
    }
  }
}

// Generations start at 1, so 0 stands for "no attempt yet" and never matches
// a real attempt.
uint64_t UniversalBlePlugin::ConnectGenerationLocked(
    const uint64_t bluetooth_address) const {
  const auto generation = connect_generations_.find(bluetooth_address);
  return generation == connect_generations_.end() ? 0 : generation->second;
}

bool UniversalBlePlugin::IsCurrentConnectAttempt(
    const uint64_t bluetooth_address, const uint64_t connect_generation) {
  std::lock_guard<std::mutex> lock(connected_devices_mutex_);
  return ConnectGenerationLocked(bluetooth_address) == connect_generation;
}

// The plugin lease is held only while this coroutine runs, never while it is
// suspended. Windows can take seconds to give up on an unreachable device and
// a connection attempt cannot be cancelled, so a lease held across those
// awaits would stall app shutdown for as long. After every await the lease is
// retaken; when that fails the plugin is shutting down or gone, and the
// attempt is abandoned without touching `this`.
fire_and_forget UniversalBlePlugin::ConnectAsync(uint64_t bluetooth_address,
                                                 uint64_t connect_generation,
                                                 bool auto_connect) {
  ScopedLease plugin(callback_operations_);
  if (!plugin.Acquire())
    co_return;
  ClosingConnection partial;
  auto &device = partial.device;
  std::shared_ptr<BluetoothDeviceAgent> device_agent;
  const auto abandon = [&]() noexcept {
    CloseConnectionAsync(std::move(partial), nullptr, nullptr);
  };
  std::optional<std::string> failure;
  try {
    // Old services can deny replacement discovery until Close finishes. Yield
    // rather than block Flutter's UI thread, and abandon superseded attempts.
    // A Close that never returns must not hold the attempt until the Dart-side
    // timeout, so give up after a bounded wait and report the failure.
    constexpr auto kConnectionCleanupWait = std::chrono::seconds(10);
    const auto cleanup_deadline =
        std::chrono::steady_clock::now() + kConnectionCleanupWait;
    for (;;) {
      {
        std::lock_guard<std::mutex> lock(connected_devices_mutex_);
        if (ConnectGenerationLocked(bluetooth_address) != connect_generation) {
          co_return;
        }
        const auto cleanup = connection_cleanup_.find(bluetooth_address);
        if (cleanup == connection_cleanup_.end())
          break;
        if (cleanup->second.IsIdle()) {
          connection_cleanup_.erase(cleanup);
          break;
        }
      }
      if (std::chrono::steady_clock::now() >= cleanup_deadline) {
        NotifyConnectFailureIfCurrent(
            bluetooth_address, connect_generation,
            "Previous connection to the device is still closing");
        co_return;
      }
      plugin.Release();
      co_await winrt::resume_after(std::chrono::milliseconds(10));
      if (!plugin.Acquire())
        co_return;
    }
    plugin.Release();
    device = co_await BluetoothLEDevice::FromBluetoothAddressAsync(
        bluetooth_address);
    if (!plugin.Acquire()) {
      abandon();
      co_return;
    }
    if (!device) {
      NotifyConnectFailureIfCurrent(
          bluetooth_address, connect_generation,
          "Failed to get device. Windows only knows a device that is paired "
          "or was seen in a recent scan");
      co_return;
    }
    UniversalBleLogger::LogInfo("ConnectionLog: Device found");

    if (auto_connect) {
      // MaintainConnection asks Windows to connect whenever the device is in
      // range and to keep waiting until it is. There is no completion to
      // await, so poll the link state until the device shows up or the
      // attempt is superseded by disconnect() or a newer connect().
      plugin.Release();
      partial.session =
          co_await GattSession::FromDeviceIdAsync(device.BluetoothDeviceId());
      if (!plugin.Acquire()) {
        abandon();
        co_return;
      }
      partial.session.MaintainConnection(true);
      while (device.ConnectionStatus() !=
             BluetoothConnectionStatus::Connected) {
        if (!IsCurrentConnectAttempt(bluetooth_address, connect_generation)) {
          DisposePartialConnection(bluetooth_address, std::move(partial));
          co_return;
        }
        plugin.Release();
        co_await winrt::resume_after(std::chrono::milliseconds(250));
        if (!plugin.Acquire()) {
          abandon();
          co_return;
        }
      }
    }

    plugin.Release();
    auto services_result =
        co_await device.GetGattServicesAsync(BluetoothCacheMode::Uncached);
    if (!plugin.Acquire()) {
      abandon();
      co_return;
    }
    // Creating a BluetoothLEDevice does not itself establish a connection.
    // The first uncached service query initiates it, but an immediate
    // post-scan query can transiently return Unreachable. Retry only that
    // status before reporting the connection attempt as failed.
    constexpr int kGattReadinessRetries = 2;
    for (int retry = 0;
         services_result.Status() == GattCommunicationStatus::Unreachable &&
         retry < kGattReadinessRetries;
         ++retry) {
      UniversalBleLogger::LogInfo(
          "ConnectionLog: GATT services temporarily unreachable; retrying " +
          std::to_string(retry + 1) + "/" +
          std::to_string(kGattReadinessRetries));
      plugin.Release();
      co_await winrt::resume_after(
          std::chrono::milliseconds(250 * (retry + 1)));
      services_result =
          co_await device.GetGattServicesAsync(BluetoothCacheMode::Uncached);
      if (!plugin.Acquire()) {
        abandon();
        co_return;
      }
    }
    // Discovery failures report the generic status message rather than the ATT
    // code in `services_result.ProtocolError()` (unlike the read/write paths,
    // which use create_flutter_error_from_gatt_result): this runs during connect
    // and is surfaced as a connection failure. The same applies to the
    // characteristics query below.
    auto services_result_error =
        gatt_communication_status_to_error(services_result.Status());
    if (services_result_error.has_value()) {
      DisposePartialConnection(bluetooth_address, std::move(partial));
      NotifyConnectFailureIfCurrent(
          bluetooth_address, connect_generation,
          "Failed to get services: " + services_result_error.value());
      co_return;
    }

    UniversalBleLogger::LogInfo("ConnectionLog: Services discovered");
    const auto discovery = std::make_shared<GattDiscovery>();
    plugin.Release();
    co_await DiscoverCharacteristicsAsync(services_result.Services(),
                                          BluetoothCacheMode::Uncached,
                                          discovery);
    partial.gatt_map = std::move(discovery->map);
    if (!plugin.Acquire()) {
      abandon();
      co_return;
    }
    if (discovery->error.has_value()) {
      DisposePartialConnection(bluetooth_address, std::move(partial));
      NotifyConnectFailureIfCurrent(bluetooth_address, connect_generation,
                                    discovery->error.value());
      co_return;
    }

    const auto connection_callback_operations = callback_operations_;
    partial.connection_status_changed_token = device.ConnectionStatusChanged(
        [this, connection_callback_operations](const BluetoothLEDevice &sender,
                                               const IInspectable &args) {
          const auto callback = connection_callback_operations.TryAcquire();
          if (!callback.has_value())
            return;
          BluetoothLeDeviceConnectionStatusChanged(sender, args);
        });
    const auto services_callback_operations = callback_operations_;
    partial.gatt_services_changed_token = device.GattServicesChanged(
        [this, services_callback_operations](const BluetoothLEDevice &sender,
                                             const IInspectable &args) {
          const auto callback = services_callback_operations.TryAcquire();
          if (!callback.has_value()) {
            return;
          }
          BluetoothLeDeviceGattServicesChanged(sender, args);
        });
    device_agent = std::make_shared<BluetoothDeviceAgent>(
        bluetooth_address, device,
        partial.connection_status_changed_token.value(),
        partial.gatt_services_changed_token.value(),
        std::move(partial.gatt_map));
    device_agent->session = partial.session;
    device_agent->auto_connect = auto_connect;
    std::shared_ptr<BluetoothDeviceAgent> previous_device_agent;
    if (!InstallConnectedDevice(bluetooth_address, connect_generation,
                                device_agent, previous_device_agent)) {
      DisposeConnection(device_agent);
      co_return;
    }
    DisposeConnection(previous_device_agent);
    if (NotifyConnectedIfCurrent(bluetooth_address, connect_generation,
                                 device)) {
      UniversalBleLogger::LogInfo("ConnectionLog: Connected");
    } else if (CleanConnection(bluetooth_address, &device)) {
      NotifyConnectionChanged(
          bluetooth_address, false,
          std::string("Device disconnected while connecting"),
          connect_generation);
    }
  } catch (const hresult_error &err) {
    failure = "ConnectAsync hresult_error hr=" + std::to_string(err.code()) +
              " msg=" + to_string(err.message());
  } catch (const std::exception &ex) {
    failure = std::string("ConnectAsync std::exception: ") + ex.what();
  } catch (...) {
    failure = "ConnectAsync unknown exception";
  }
  if (!failure.has_value())
    co_return;
  // An await can throw while the lease is released.
  if (!plugin.Held() && !plugin.Acquire()) {
    abandon();
    co_return;
  }
  Attempt("ConnectAsync partial cleanup", [&] {
    if (device_agent) {
      CleanConnection(bluetooth_address, &device);
      DisposeConnection(device_agent);
    } else {
      DisposePartialConnection(bluetooth_address, std::move(partial));
    }
  });
  NotifyConnectFailureIfCurrent(bluetooth_address, connect_generation,
                                failure.value());
}

// Builds the characteristic map for `services`. Any failure ends discovery and
// is reported in `out->error`; services queried so far stay in `out->map` so
// the caller can close them.
//
// AccessDenied has two causes. Windows owns HID over GATT for its own HID
// driver and always denies apps that service; it is kept with no
// characteristics, as Microsoft's GATT client sample does for a restricted
// service, so such a device can still connect. Any other AccessDenied is
// transient: handles from a previous connection to the device are still
// closing. That fails discovery, and the connect, so the caller can retry.
IAsyncAction UniversalBlePlugin::DiscoverCharacteristicsAsync(
    IVectorView<GattDeviceService> services, BluetoothCacheMode cache_mode,
    std::shared_ptr<GattDiscovery> out) {
  for (GattDeviceService &&service : services) {
    GattServiceObject gatt_service;
    gatt_service.obj = service;
    const auto service_uuid = guid_to_uuid(service.Uuid());
    const bool system_owned =
        service.Uuid() == GattServiceUuids::HumanInterfaceDevice();
    try {
      const auto result = co_await service.GetCharacteristicsAsync(cache_mode);
      const auto status = result.Status();
      if (status == GattCommunicationStatus::AccessDenied && system_owned) {
        UniversalBleLogger::LogInfo("Service " + service_uuid +
                                    " is reserved by Windows");
      } else if (const auto error = gatt_communication_status_to_error(status);
                 error.has_value()) {
        out->error = "Failed to get characteristics for service " +
                     service_uuid + ": " + error.value();
      } else {
        for (GattCharacteristic &&characteristic : result.Characteristics()) {
          GattCharacteristicObject gatt_characteristic;
          gatt_characteristic.obj = characteristic;
          gatt_service.characteristics.insert_or_assign(
              guid_to_uuid(characteristic.Uuid()),
              std::move(gatt_characteristic));
        }
      }
    } catch (const hresult_error &err) {
      if (err.code() == E_ACCESSDENIED && system_owned) {
        UniversalBleLogger::LogInfo("Service " + service_uuid +
                                    " is reserved by Windows");
      } else {
        out->error = "Service discovery hresult_error hr=" +
                     std::to_string(err.code()) +
                     " msg=" + to_string(err.message());
      }
    } catch (const std::exception &ex) {
      out->error = std::string("Service discovery exception: ") + ex.what();
    } catch (...) {
      out->error = "Service discovery unknown error";
    }
    out->map.insert_or_assign(service_uuid, std::move(gatt_service));
    if (out->error.has_value()) {
      co_return;
    }
  }
}

fire_and_forget UniversalBlePlugin::RefreshGattServicesAsync(
    const uint64_t bluetooth_address,
    std::shared_ptr<BluetoothDeviceAgent> device_agent) {
  // The plugin lease is released across every await. When it cannot be
  // retaken the plugin is shutting down: only the agent, which this frame
  // owns, may still be touched.
  ScopedLease plugin(callback_operations_);
  if (!plugin.Acquire()) {
    device_agent->FinishGattRefresh();
    co_return;
  }
  while (true) {
    bool refreshed = false;
    try {
      const auto current = GetConnectedDevice(bluetooth_address);
      if (!current || current != device_agent) {
        device_agent->FinishGattRefresh();
        co_return;
      }

      auto subscriptions = device_agent->SnapshotSubscriptions();
      while (!subscriptions.has_value()) {
        if (!device_agent->IsActive()) {
          device_agent->FinishGattRefresh();
          co_return;
        }
        // A CCC operation is changing the subscription map. Yield instead of
        // blocking its WinRT continuation, then take one coherent snapshot.
        plugin.Release();
        co_await winrt::resume_after(std::chrono::milliseconds(10));
        if (!plugin.Acquire()) {
          device_agent->FinishGattRefresh();
          co_return;
        }
        subscriptions = device_agent->SnapshotSubscriptions();
      }

      // Characteristics returned to an in-flight read, write, or discovery
      // belong to the current service map. Do not Close that map underneath
      // the WinRT operation; refreshes are rare, so waiting here preserves
      // ordinary-operation concurrency without globally serializing GATT.
      while (!device_agent->gatt_operations.IsIdle()) {
        if (!device_agent->IsActive()) {
          device_agent->FinishGattRefresh();
          co_return;
        }
        plugin.Release();
        co_await winrt::resume_after(std::chrono::milliseconds(10));
        if (!plugin.Acquire()) {
          device_agent->FinishGattRefresh();
          co_return;
        }
      }

      // Microsoft specifies Cached here after GattServicesChanged. Windows
      // refreshes its cache as part of processing the Service Changed
      // indication; using Uncached can race that processing and return a
      // transient or incomplete database.
      plugin.Release();
      const auto services_result =
          co_await device_agent->device.GetGattServicesAsync(
              BluetoothCacheMode::Cached);
      if (!plugin.Acquire()) {
        device_agent->FinishGattRefresh();
        co_return;
      }
      const auto services_error =
          gatt_communication_status_to_error(services_result.Status());
      if (services_error.has_value()) {
        UniversalBleLogger::LogError("GATT service refresh failed: " +
                                     services_error.value());
      } else {
        const auto discovery = std::make_shared<GattDiscovery>();
        plugin.Release();
        co_await DiscoverCharacteristicsAsync(
            services_result.Services(), BluetoothCacheMode::Cached, discovery);
        auto &replacement = discovery->map;
        if (!plugin.Acquire()) {
          const auto retained_services = device_agent->SnapshotGattServices();
          DisposeGattMap(std::move(replacement), &retained_services);
          device_agent->FinishGattRefresh();
          co_return;
        }
        const auto &discovery_error = discovery->error;

        if (discovery_error.has_value()) {
          UniversalBleLogger::LogError(discovery_error.value());
          const auto retained_services = device_agent->SnapshotGattServices();
          DisposeGattMap(std::move(replacement), &retained_services);
        } else {
          try {
            const auto device_id = mac_address_to_str(bluetooth_address);
            for (const auto &[service_id, characteristic_id] :
                 subscriptions->characteristics) {
              const auto service = replacement.find(service_id);
              if (service == replacement.end()) {
                continue;
              }
              const auto characteristic =
                  service->second.characteristics.find(characteristic_id);
              if (characteristic == service->second.characteristics.end()) {
                continue;
              }
              characteristic->second.subscription_token =
                  RegisterGattValueChangedHandler(device_agent, device_id,
                                                  characteristic_id,
                                                  characteristic->second.obj);
            }
          } catch (...) {
            const auto retained_services = device_agent->SnapshotGattServices();
            DisposeGattMap(std::move(replacement), &retained_services);
            throw;
          }

          const auto still_current = GetConnectedDevice(bluetooth_address);
          if (!still_current || still_current != device_agent) {
            const auto retained_services = device_agent->SnapshotGattServices();
            DisposeGattMap(std::move(replacement), &retained_services);
            device_agent->FinishGattRefresh();
            co_return;
          }
          std::vector<GattDeviceService> retained_services;
          retained_services.reserve(replacement.size());
          for (const auto &[service_id, service] : replacement) {
            (void)service_id;
            retained_services.push_back(service.obj);
          }
          std::unordered_map<std::string, GattServiceObject> previous;
          if (device_agent->ReplaceGattMapIfUnchanged(
                  replacement, subscriptions->revision, previous)) {
            DisposeGattMap(std::move(previous), &retained_services);
            refreshed = true;
          } else {
            // A subscribe/unsubscribe or disconnect overlapped discovery.
            // Never install a map built from stale subscription state.
            const auto current_services = device_agent->SnapshotGattServices();
            DisposeGattMap(std::move(replacement), &current_services);
            device_agent->RequestGattRefresh();
          }
        }
      }
    } catch (...) {
      LogCurrentException("RefreshGattServicesAsync");
    }

    const bool repeat = device_agent->FinishGattRefresh();
    // An await can throw while the lease is released.
    if (repeat && !plugin.Held() && !plugin.Acquire()) {
      device_agent->FinishGattRefresh();
      co_return;
    }
    if (!repeat) {
      if (refreshed) {
        UniversalBleLogger::LogInfo("GATT services refreshed after change");
      }
      co_return;
    }
  }
}

void UniversalBlePlugin::BluetoothLeDeviceConnectionStatusChanged(
    const BluetoothLEDevice &sender, const IInspectable &) {
  try {
    const auto bluetooth_address = sender.BluetoothAddress();
    if (sender.ConnectionStatus() == BluetoothConnectionStatus::Disconnected) {
      const auto device_agent = GetConnectedDevice(bluetooth_address);
      const bool reconnect = device_agent && device_agent->device == sender &&
                             device_agent->auto_connect;
      uint64_t disconnect_generation = 0;
      if (CleanConnection(bluetooth_address, &sender, &disconnect_generation)) {
        NotifyConnectionChanged(bluetooth_address, false, std::nullopt,
                                disconnect_generation);
        if (reconnect) {
          // Queued behind the disconnect event above, which a newer connect
          // generation would otherwise suppress.
          ui_thread_handler_.Post([this, bluetooth_address] {
            StartConnect(bluetooth_address, true);
          });
        }
      } else {
        UniversalBleLogger::LogDebug(
            "Ignoring stale connection status callback");
      }
    }
  } catch (...) {
    LogCurrentException("ConnectionStatusChanged");
  }
}

void UniversalBlePlugin::BluetoothLeDeviceGattServicesChanged(
    const BluetoothLEDevice &sender, const IInspectable &) {
  try {
    const auto bluetooth_address = sender.BluetoothAddress();
    const auto device_agent = GetConnectedDevice(bluetooth_address);
    if (!device_agent || device_agent->device != sender) {
      UniversalBleLogger::LogDebug(
          "Ignoring GATT services change from stale Bluetooth device instance");
      return;
    }
    if (device_agent->BeginGattRefresh()) {
      RefreshGattServicesAsync(bluetooth_address, device_agent);
    }
  } catch (...) {
    LogCurrentException("GattServicesChanged");
  }
}

std::shared_ptr<BluetoothDeviceAgent> UniversalBlePlugin::GetConnectedDevice(
    const uint64_t bluetooth_address) {
  std::lock_guard<std::mutex> lock(connected_devices_mutex_);
  const auto it = connected_devices_.find(bluetooth_address);
  return it == connected_devices_.end() ? nullptr : it->second;
}

void UniversalBlePlugin::InvalidateConnectAttempt(
    const uint64_t bluetooth_address) {
  std::lock_guard<std::mutex> lock(connected_devices_mutex_);
  pending_connects_.erase(bluetooth_address);
  connect_generations_.insert_or_assign(bluetooth_address,
                                        ++next_connect_generation_);
}

std::shared_ptr<BluetoothDeviceAgent> UniversalBlePlugin::RemoveConnectedDevice(
    const uint64_t bluetooth_address, const BluetoothLEDevice *expected_device,
    uint64_t *removed_generation) {
  std::lock_guard<std::mutex> lock(connected_devices_mutex_);
  const auto it = connected_devices_.find(bluetooth_address);
  if (it == connected_devices_.end() ||
      (expected_device != nullptr && it->second->device != *expected_device)) {
    return nullptr;
  }
  const auto device_agent = it->second;
  ReserveConnectionCleanupLocked(device_agent);
  if (removed_generation != nullptr) {
    *removed_generation = ConnectGenerationLocked(bluetooth_address);
  }
  connected_devices_.erase(it);
  return device_agent;
}

bool UniversalBlePlugin::InstallConnectedDevice(
    const uint64_t bluetooth_address, const uint64_t connect_generation,
    std::shared_ptr<BluetoothDeviceAgent> device_agent,
    std::shared_ptr<BluetoothDeviceAgent> &previous_device_agent) {
  std::lock_guard<std::mutex> lock(connected_devices_mutex_);
  if (ConnectGenerationLocked(bluetooth_address) != connect_generation) {
    return false;
  }
  pending_connects_.erase(bluetooth_address);
  const auto it = connected_devices_.find(bluetooth_address);
  if (it != connected_devices_.end()) {
    previous_device_agent = it->second;
    ReserveConnectionCleanupLocked(previous_device_agent);
  }
  connected_devices_.insert_or_assign(bluetooth_address,
                                      std::move(device_agent));
  return true;
}

bool UniversalBlePlugin::NotifyConnectedIfCurrent(
    const uint64_t bluetooth_address, const uint64_t connect_generation,
    const BluetoothLEDevice &expected_device) {
  std::lock_guard<std::mutex> lock(connected_devices_mutex_);
  const auto device_agent = connected_devices_.find(bluetooth_address);
  if (ConnectGenerationLocked(bluetooth_address) != connect_generation ||
      device_agent == connected_devices_.end() ||
      device_agent->second->device != expected_device ||
      expected_device.ConnectionStatus() !=
          BluetoothConnectionStatus::Connected) {
    return false;
  }
  NotifyConnectionChanged(bluetooth_address, true, std::nullopt,
                          connect_generation);
  return true;
}

void UniversalBlePlugin::NotifyConnectFailureIfCurrent(
    const uint64_t bluetooth_address, const uint64_t connect_generation,
    const std::string &error_message) noexcept {
  try {
    UniversalBleLogger::LogError(error_message);
    std::lock_guard<std::mutex> lock(connected_devices_mutex_);
    if (ConnectGenerationLocked(bluetooth_address) == connect_generation) {
      pending_connects_.erase(bluetooth_address);
      NotifyConnectionChanged(bluetooth_address, false, error_message,
                              connect_generation);
    } else {
      UniversalBleLogger::LogDebug(
          "Ignoring failure from stale connection attempt");
    }
  } catch (...) {
    try {
      UniversalBleLogger::LogError(
          "Failed to publish connection-attempt failure");
    } catch (...) {
    }
  }
}

bool UniversalBlePlugin::CleanConnection(
    const uint64_t bluetooth_address, const BluetoothLEDevice *expected_device,
    uint64_t *removed_generation) {
  try {
    const auto device_agent = RemoveConnectedDevice(
        bluetooth_address, expected_device, removed_generation);
    if (!device_agent) {
      return false;
    }
    DisposeConnection(device_agent);
    return true;
  } catch (...) {
    LogCurrentException("CleanConnection");
  }
  return false;
}

void UniversalBlePlugin::ReserveConnectionCleanupLocked(
    const std::shared_ptr<BluetoothDeviceAgent> &device_agent) {
  if (device_agent->IsActive() && !device_agent->cleanup_reservation) {
    device_agent->cleanup_reservation =
        ReserveAddressCleanupLocked(device_agent->bluetooth_address);
  }
}

AsyncOperationTracker::Lease UniversalBlePlugin::ReserveAddressCleanupLocked(
    const uint64_t bluetooth_address) {
  // Entries are otherwise only erased when the same address is connected
  // again. Drop finished ones here so addresses that are never revisited do
  // not accumulate. Idle entries gate nothing, and every acquisition happens
  // under connected_devices_mutex_, so none can become busy while erased.
  for (auto it = connection_cleanup_.begin();
       it != connection_cleanup_.end();) {
    if (it->first != bluetooth_address && it->second.IsIdle()) {
      it = connection_cleanup_.erase(it);
    } else {
      ++it;
    }
  }
  return connection_cleanup_[bluetooth_address].TryAcquire().value();
}

void UniversalBlePlugin::DisposeConnection(
    const std::shared_ptr<BluetoothDeviceAgent> &device_agent) {
  if (!device_agent || !device_agent->Deactivate()) {
    return;
  }
  AsyncOperationTracker::Lease reservation;
  {
    std::lock_guard<std::mutex> lock(connected_devices_mutex_);
    reservation = std::move(device_agent->cleanup_reservation);
  }
  CloseConnection(device_agent->bluetooth_address,
                  {device_agent->device, device_agent->session,
                   device_agent->TakeGattMap(),
                   device_agent->connection_status_changed_token,
                   device_agent->gatt_services_changed_token},
                  std::move(reservation));
}

void UniversalBlePlugin::DisposePartialConnection(
    const uint64_t bluetooth_address, ClosingConnection closing) {
  if (!closing.device && closing.gatt_map.empty()) {
    return;
  }
  CloseConnection(bluetooth_address, std::move(closing), nullptr);
}

// WinRT GATT Close can synchronously wait for COM teardown. Calling it on
// Flutter's STA can deadlock when a peripheral disconnect arrives during that
// wait, e.g. when a peripheral that is rebooting drops the link mid-discovery.
// The objects are closed on the thread pool instead, and the address's cleanup
// reservation is held until that has finished so a reconnect waits for it.
void UniversalBlePlugin::CloseConnection(
    const uint64_t bluetooth_address, ClosingConnection closing,
    AsyncOperationTracker::Lease reservation) {
  const auto cleanup = cleanup_operations_.TryAcquire();
  if (!cleanup.has_value()) {
    // Shutdown schedules every disposal before closing this tracker. If that
    // ordering is ever violated, dropping the references is all that is left.
    Attempt("CloseConnection", [] {
      UniversalBleLogger::LogError(
          "CloseConnection: cleanup tracker closed before disposal was "
          "scheduled");
    });
    return;
  }
  if (!reservation) {
    std::lock_guard<std::mutex> lock(connected_devices_mutex_);
    reservation = ReserveAddressCleanupLocked(bluetooth_address);
  }
  CloseConnectionAsync(std::move(closing), cleanup.value(),
                       std::move(reservation));
}

// The coroutine frame owns the WinRT objects and leases. No plugin pointer is
// captured: shutdown may leave this worker running with the DLL pinned.
fire_and_forget UniversalBlePlugin::CloseConnectionAsync(
    ClosingConnection closing, AsyncOperationTracker::Lease cleanup_lease,
    AsyncOperationTracker::Lease connection_cleanup_lease) {
  try {
    co_await winrt::resume_background();
    auto &device = closing.device;
    if (device && closing.connection_status_changed_token.has_value()) {
      Attempt("CloseConnection remove connection status handler", [&] {
        device.ConnectionStatusChanged(
            closing.connection_status_changed_token.value());
      });
    }
    if (device && closing.gatt_services_changed_token.has_value()) {
      Attempt("CloseConnection remove GATT services handler", [&] {
        device.GattServicesChanged(
            closing.gatt_services_changed_token.value());
      });
    }
    // Tell the peripheral to stop notifying, as Microsoft's GATT client sample
    // does before closing. Only while the link is up: a write to a device that
    // is already gone would wait out the Windows GATT timeout for nothing.
    bool connected = false;
    Attempt("CloseConnection read connection status", [&] {
      connected = device && device.ConnectionStatus() ==
                                BluetoothConnectionStatus::Connected;
    });
    if (connected) {
      for (auto &[service_id, service] : closing.gatt_map) {
        for (auto &[char_id, characteristic] : service.characteristics) {
          if (!characteristic.subscription_token.has_value())
            continue;
          try {
            co_await characteristic.obj
                .WriteClientCharacteristicConfigurationDescriptorAsync(
                    GattClientCharacteristicConfigurationDescriptorValue::
                        None);
          } catch (...) {
            Attempt("CloseConnection", [&] {
              UniversalBleLogger::LogWarning(
                  "CloseConnection: could not unsubscribe " + char_id);
            });
          }
        }
      }
    }
    DisposeGattMap(std::move(closing.gatt_map));
    if (closing.session) {
      Attempt("CloseConnection close GATT session", [&] {
        closing.session.MaintainConnection(false);
        closing.session.Close();
      });
    }
    if (device) {
      Attempt("CloseConnection close Bluetooth device",
              [&] { device.Close(); });
    }
  } catch (...) {
    // Thread-pool submission failed. Do not close WinRT objects on the UI
    // apartment instead.
    Attempt("CloseConnectionAsync",
            [] { log_and_swallow_unknown("CloseConnectionAsync"); });
  }
  // Release every old object before allowing another discovery for the address.
  closing = {};
  connection_cleanup_lease.reset();
}

void UniversalBlePlugin::DisposeGattMap(
    std::unordered_map<std::string, GattServiceObject> gatt_map,
    const std::vector<GattDeviceService> *retained_services) {
  for (auto &[service_id, service] : gatt_map) {
    for (auto &[char_id, characteristic] : service.characteristics) {
      if (characteristic.subscription_token.has_value() &&
          !characteristic.notification_operation_in_progress) {
        Attempt("DisposeGattMap unsubscribe", [&] {
          characteristic.obj.ValueChanged(
              characteristic.subscription_token.value());
        });
        characteristic.subscription_token = std::nullopt;
      }
    }
    const bool is_retained =
        retained_services != nullptr &&
        std::find(retained_services->begin(), retained_services->end(),
                  service.obj) != retained_services->end();
    if (!is_retained) {
      Attempt("DisposeGattMap close service", [&] { service.obj.Close(); });
    }
  }
}

/**
 * @brief In some cases, it helps to reset the whole Bluetooth state to get
 * rid of any dangling connections, before scanning or connecting.
 */
void UniversalBlePlugin::ResetState() {
  try {
    DisposeAdvertisementWatcher();
    scan_results_.clear();
    connectable_heard_.clear();
    paired_devices_.clear();

    // Close all connected devices and clear map
    std::vector<std::shared_ptr<BluetoothDeviceAgent>> device_agents;
    {
      std::lock_guard<std::mutex> lock(connected_devices_mutex_);
      device_agents.reserve(connected_devices_.size());
      for (const auto &entry : connected_devices_) {
        ReserveConnectionCleanupLocked(entry.second);
        device_agents.push_back(entry.second);
      }
      connected_devices_.clear();
      connect_generations_.clear();
      pending_connects_.clear();
    }
    for (const auto &device_agent : device_agents) {
      DisposeConnection(device_agent);
    }

    UniversalBleLogger::LogInfo("ResetState: completed clean slate");
  } catch (...) {
    LogCurrentException("ResetState");
  }
}

fire_and_forget UniversalBlePlugin::GetSystemDevicesAsync(
    std::vector<std::string> with_services,
    std::function<void(ErrorOr<flutter::EncodableList> reply)> result) {
  // Touches no plugin state; the lease is taken only to reply, since a
  // reply after shutdown would go through a channel that is gone.
  ScopedLease plugin(callback_operations_);
  const auto reply = [&](ErrorOr<flutter::EncodableList> value) {
    if (plugin.Acquire())
      safe_reply("GetSystemDevices", result, std::move(value));
  };
  try {
    auto selector = BluetoothLEDevice::GetDeviceSelectorFromConnectionStatus(
        BluetoothConnectionStatus::Connected);
    DeviceInformationCollection devices =
        co_await DeviceInformation::FindAllAsync(selector);
    auto results = flutter::EncodableList();
    for (auto &&device_info : devices) {
      try {
        BluetoothLEDevice device =
            co_await BluetoothLEDevice::FromIdAsync(device_info.Id());
        auto device_id = mac_address_to_str(device.BluetoothAddress());
        // Filter by services
        bool include_device = true;
        if (!with_services.empty()) {
          auto service_result =
              co_await device.GetGattServicesAsync(BluetoothCacheMode::Cached);
          if (service_result.Status() == GattCommunicationStatus::Success) {
            bool has_service = false;
            for (auto service : service_result.Services()) {
              std::string service_uuid = to_uuidstr(service.Uuid());
              if (std::find(with_services.begin(), with_services.end(),
                            service_uuid) != with_services.end()) {
                has_service = true;
              }
            }
            include_device = has_service;
          } else {
            include_device = false;
          }
        }
        if (include_device) {
          // Add to results, if pass all filters
          auto universal_scan_result = UniversalBleScanResult(device_id);
          universal_scan_result.set_name(to_string(device_info.Name()));
          universal_scan_result.set_is_paired(device_info.Pairing().IsPaired());
          universal_scan_result.set_connectable(true);
          results.push_back(
              flutter::CustomEncodableValue(universal_scan_result));
        }
      } catch (...) {
      }
    }
    reply(std::move(results));
  } catch (...) {
    reply(CurrentExceptionAsFlutterError());
  }
}

fire_and_forget UniversalBlePlugin::DiscoverServicesAsync(
    std::string device_id, bool with_descriptors,
    std::function<void(ErrorOr<flutter::EncodableList> reply)> result) {
  // The plugin lease is released around each descriptor query, which can take
  // the full GATT timeout. A reply needs the lease: after shutdown the channel
  // it would go through is gone.
  ScopedLease plugin(callback_operations_);
  if (!plugin.Acquire())
    co_return;
  try {
    const auto device_agent = GetConnectedDevice(str_to_mac_address(device_id));
    if (!device_agent) {
      result(create_flutter_error(UniversalBleErrorCode::kDeviceNotFound,
                                  "Unknown deviceId: " + device_id));
      co_return;
    }

    const auto gatt_map_lease = device_agent->SnapshotGattMapForOperation();
    const auto &gatt_map = gatt_map_lease.map;
    auto universal_services = flutter::EncodableList();
    for (const auto &[service_id, service] : gatt_map) {
      flutter::EncodableList universal_characteristics;
      for (const auto &[char_id, characteristic] : service.characteristics) {
        const auto c = characteristic.obj;
        const auto properties_value = c.CharacteristicProperties();
        auto properties = properties_to_flutter_encodable(properties_value);
        auto descriptors = flutter::EncodableList();
        if (with_descriptors) {
          try {
            // Awaited on the platform thread, like connect, so the reply below
            // is sent from the thread Flutter delivered the call on.
            plugin.Release();
            auto descriptor_result =
                co_await c.GetDescriptorsAsync(BluetoothCacheMode::Cached);
            if (!plugin.Acquire())
              co_return;
            if (descriptor_result.Status() ==
                GattCommunicationStatus::Success) {
              auto descriptors_list = descriptor_result.Descriptors();
              for (auto &&descriptor : descriptors_list) {
                descriptors.push_back(flutter::CustomEncodableValue(
                    UniversalBleDescriptor(to_uuidstr(descriptor.Uuid()))));
              }
            } else {
              UniversalBleLogger::LogError(
                  "DiscoverServicesAsync: descriptor query for " +
                  to_uuidstr(c.Uuid()) + " failed with status " +
                  std::to_string(static_cast<int>(descriptor_result.Status())));
            }
          } catch (...) {
            if (!plugin.Held() && !plugin.Acquire())
              co_return;
            LogCurrentException("DiscoverServicesAsync descriptors");
          }
        }
        universal_characteristics.push_back(
            flutter::CustomEncodableValue(UniversalBleCharacteristic(
                to_uuidstr(c.Uuid()), properties, descriptors)));
      }

      auto universal_ble_service =
          UniversalBleService(to_uuidstr(service.obj.Uuid()));
      universal_ble_service.set_characteristics(universal_characteristics);
      universal_services.push_back(
          flutter::CustomEncodableValue(universal_ble_service));
    }
    if (!device_agent->IsActive()) {
      result(create_flutter_error(UniversalBleErrorCode::kDeviceDisconnected,
                                  "Device disconnected during discovery"));
      co_return;
    }
    result(universal_services);
  } catch (...) {
    // Every await above catches its own exceptions, so the lease is held.
    result(CurrentExceptionAsFlutterError());
  }
}

fire_and_forget UniversalBlePlugin::IsPairedAsync(
    std::string device_id,
    const std::function<void(ErrorOr<bool> reply)> result) {
  // Touches no plugin state; the lease is taken only to reply, since a
  // reply after shutdown would go through a channel that is gone.
  ScopedLease plugin(callback_operations_);
  const auto reply = [&](ErrorOr<bool> value) {
    if (plugin.Acquire())
      safe_reply("IsPaired", result, std::move(value));
  };
  try {
    const auto device = co_await BluetoothLEDevice::FromBluetoothAddressAsync(
        str_to_mac_address(device_id));
    if (device == nullptr) {
      reply(create_flutter_error(UniversalBleErrorCode::kDeviceNotFound,
                                  "Unknown deviceId: " + device_id));
      co_return;
    }
    const bool is_paired = device.DeviceInformation().Pairing().IsPaired();
    reply(is_paired);
  } catch (...) {
    reply(CurrentExceptionAsFlutterError());
  }
}

// Writes the CCC descriptor and keeps the ValueChanged handler in step with
// it. The handler is installed before an enabling write, because a peripheral
// may notify as soon as it has processed the write, before the WinRT operation
// completes. The plugin lease is released across the write; if it cannot be
// retaken, the operation is rolled back using only objects this frame owns and
// nothing is replied, since the channel is gone.
fire_and_forget UniversalBlePlugin::SetNotifiableAsync(
    std::string device_id, std::string service, std::string characteristic,
    BleInputProperty ble_input_property,
    const std::function<void(std::optional<FlutterError> reply)> result) {
  ScopedLease plugin(callback_operations_);
  if (!plugin.Acquire())
    co_return;
  const char *stage = "entry";
  std::shared_ptr<BluetoothDeviceAgent> device_agent;
  GattCharacteristic gatt_characteristic{nullptr};
  std::optional<event_token> provisional_token;
  std::optional<event_token> previous_token;
  bool operation_started = false;
  // Undoes a started operation: removes a handler installed for it and puts
  // the previous subscription token back.
  const auto roll_back = [&]() noexcept {
    if (!operation_started)
      return;
    operation_started = false;
    if (provisional_token.has_value()) {
      Attempt("SET_NOTIFY remove provisional handler",
              [&] { gatt_characteristic.ValueChanged(*provisional_token); });
      provisional_token.reset();
    }
    Attempt("SET_NOTIFY restore subscription", [&] {
      device_agent->FinishNotificationOperation(service, characteristic,
                                                previous_token);
    });
  };
  const auto fail = [&](FlutterError error) {
    roll_back();
    safe_reply("SetNotifiable", result, std::move(error));
  };
  const auto disconnected = [](const char *message) {
    return create_flutter_error(UniversalBleErrorCode::kDeviceDisconnected,
                                message);
  };

  try {
    UniversalBleLogger::LogDebugWithTimestamp(
        "SET_NOTIFY -> " + device_id + " " + service + " " + characteristic +
        " input=" + std::to_string(static_cast<int>(ble_input_property)));
    stage = "lookup";
    device_agent = GetConnectedDevice(str_to_mac_address(device_id));
    if (!device_agent) {
      fail(create_flutter_error(UniversalBleErrorCode::kDeviceNotFound,
                                "Unknown deviceId: " + device_id));
      co_return;
    }
    auto gatt_char = device_agent->FetchCharacteristic(service, characteristic);

    const auto properties = gatt_char.obj.CharacteristicProperties();
    auto descriptor_value =
        GattClientCharacteristicConfigurationDescriptorValue::None;
    if (ble_input_property == BleInputProperty::kNotification) {
      descriptor_value =
          GattClientCharacteristicConfigurationDescriptorValue::Notify;
      if ((properties & GattCharacteristicProperties::Notify) ==
          GattCharacteristicProperties::None) {
        fail(create_flutter_error(
            UniversalBleErrorCode::kCharacteristicDoesNotSupportNotify,
            "Characteristic does not support notify"));
        co_return;
      }
    } else if (ble_input_property == BleInputProperty::kIndication) {
      descriptor_value =
          GattClientCharacteristicConfigurationDescriptorValue::Indicate;
      if ((properties & GattCharacteristicProperties::Indicate) ==
          GattCharacteristicProperties::None) {
        fail(create_flutter_error(
            UniversalBleErrorCode::kCharacteristicDoesNotSupportIndicate,
            "Characteristic does not support indicate"));
        co_return;
      }
    }
    const bool enabling =
        descriptor_value !=
        GattClientCharacteristicConfigurationDescriptorValue::None;

    if (!device_agent->BeginNotificationOperation(service, characteristic,
                                                  gatt_char)) {
      fail(create_flutter_error(
          UniversalBleErrorCode::kFailed,
          "Another notification operation is already running or the device "
          "was disconnected"));
      co_return;
    }
    operation_started = true;
    gatt_characteristic = gatt_char.obj;
    previous_token = gatt_char.subscription_token;

    if (enabling && !previous_token.has_value()) {
      stage = "register-handler";
      provisional_token = RegisterGattValueChangedHandler(
          device_agent, device_id, characteristic, gatt_characteristic);
      if (!device_agent->UpdateNotificationOperationToken(
              service, characteristic, provisional_token)) {
        fail(disconnected("Device disconnected while subscribing"));
        co_return;
      }
    }

    stage = "write-descriptor";
    GattWriteResult write_result{nullptr};
    std::optional<FlutterError> write_error;
    plugin.Release();
    try {
      write_result =
          co_await gatt_characteristic
              .WriteClientCharacteristicConfigurationDescriptorWithResultAsync(
                  descriptor_value);
    } catch (...) {
      write_error = CurrentExceptionAsFlutterError();
    }
    if (!plugin.Acquire()) {
      roll_back();
      co_return;
    }
    if (write_error.has_value()) {
      UniversalBleLogger::LogError("SET_NOTIFY write failed: " +
                                   write_error->message() + " device=" +
                                   device_id + " char=" + characteristic);
      fail(std::move(write_error.value()));
      co_return;
    }
    if (write_result.Status() != GattCommunicationStatus::Success) {
      UniversalBleLogger::LogError(
          "SET_NOTIFY_FAILED <- " + device_id + " " + service + " " +
          characteristic + " status=" +
          std::to_string(static_cast<int>(write_result.Status())));
      fail(create_flutter_error_from_gatt_result(
          write_result.Status(), write_result.ProtocolError()));
      co_return;
    }

    stage = "finish";
    if (!enabling && previous_token.has_value()) {
      gatt_characteristic.ValueChanged(previous_token.value());
      UniversalBleLogger::LogInfo("Unsubscribed " + characteristic);
    }
    const auto active_token =
        enabling ? (provisional_token.has_value() ? provisional_token
                                                  : previous_token)
                 : std::nullopt;
    if (!device_agent->FinishNotificationOperation(service, characteristic,
                                                   active_token)) {
      // The device went away meanwhile: nothing to restore.
      operation_started = false;
      if (provisional_token.has_value()) {
        Attempt("SET_NOTIFY remove provisional handler",
                [&] { gatt_characteristic.ValueChanged(*provisional_token); });
      }
      safe_reply("SetNotifiable", result,
                 disconnected(enabling ? "Device disconnected while subscribing"
                                       : "Device disconnected while "
                                         "unsubscribing"));
      co_return;
    }
    operation_started = false;
    safe_reply("SetNotifiable", result, std::nullopt);
  } catch (...) {
    // Every await above catches its own exceptions, so the lease is held.
    auto error = CurrentExceptionAsFlutterError();
    UniversalBleLogger::LogError("SET_NOTIFY failed at stage=" +
                                 std::string(stage) + ": " + error.message() +
                                 " device=" + device_id +
                                 " char=" + characteristic);
    fail(std::move(error));
  }
}

event_token UniversalBlePlugin::RegisterGattValueChangedHandler(
    const std::shared_ptr<BluetoothDeviceAgent> &device_agent,
    const std::string &device_id, const std::string &characteristic_id,
    const GattCharacteristic &characteristic) {
  const auto notification_callback_operations = callback_operations_;
  return characteristic.ValueChanged(
      [this, notification_callback_operations,
       weak_device_agent = std::weak_ptr(device_agent), device_id,
       characteristic_id](const GattCharacteristic &,
                          const GattValueChangedEventArgs &args) {
        const auto callback = notification_callback_operations.TryAcquire();
        if (!callback.has_value()) {
          return;
        }
        GattCharacteristicValueChanged(weak_device_agent, device_id,
                                       characteristic_id, args);
      });
}

void UniversalBlePlugin::GattCharacteristicValueChanged(
    const std::weak_ptr<BluetoothDeviceAgent> &weak_device_agent,
    const std::string &device_id, const std::string &characteristic_id,
    const GattValueChangedEventArgs &args) {
  try {
    const auto device_agent = weak_device_agent.lock();
    if (!device_agent || !device_agent->IsActive()) {
      UniversalBleLogger::LogDebug(
          "Ignoring notification from stale Bluetooth device instance");
      return;
    }
    const auto bytes = to_bytevc(args.CharacteristicValue());

    UniversalBleLogger::LogVerboseWithTimestamp(
        "NOTIFY <- " + device_id + " " + characteristic_id +
        " len=" + std::to_string(bytes.size()));

    const auto timestamp = GetCurrentTimestampMillis();
    PostCentralReport([weak_device_agent, device_id, characteristic_id, bytes,
                       timestamp](UniversalBleCallbackChannel *channel) {
      const auto current_device_agent = weak_device_agent.lock();
      if (!current_device_agent || !current_device_agent->IsActive()) {
        return;
      }
      channel->OnValueChanged(device_id, characteristic_id, bytes, &timestamp,
                              SuccessCallback, ErrorCallback);
    });
  } catch (...) {
    LogCurrentException("GattCharacteristicValueChanged");
  }
}

ErrorOr<PeripheralAdvertisingState> UniversalBlePlugin::GetAdvertisingState() {
  std::lock_guard<std::mutex> lock(peripheral_mutex_);
  if (advertisement_publisher_) {
    switch (advertisement_publisher_.Status()) {
      case BluetoothLEAdvertisementPublisherStatus::Started:
        return PeripheralAdvertisingState::kAdvertising;
      case BluetoothLEAdvertisementPublisherStatus::Aborted:
        return PeripheralAdvertisingState::kError;
      default:
        return PeripheralAdvertisingState::kIdle;
    }
  }
  if (peripheral_service_provider_map_.empty()) {
    return PeripheralAdvertisingState::kIdle;
  }
  return ArePeripheralAdvertisingTargetsStarted()
             ? PeripheralAdvertisingState::kAdvertising
             : PeripheralAdvertisingState::kIdle;
}

ErrorOr<PeripheralReadinessState> UniversalBlePlugin::GetReadinessState() {
  if (!bluetooth_radio_) {
    return initialized_ ? PeripheralReadinessState::kUnsupported
                        : PeripheralReadinessState::kUnknown;
  }
  // Peripheral role depends on the adapter. Until the adapter has answered,
  // assume it is supported, as before.
  if (!peripheral_role_supported_.value_or(true)) {
    return PeripheralReadinessState::kUnsupported;
  }
  if (bluetooth_radio_.State() != RadioState::On) {
    return PeripheralReadinessState::kBluetoothOff;
  }
  return PeripheralReadinessState::kReady;
}

fire_and_forget UniversalBlePlugin::LoadPeripheralSupportAsync() {
  ScopedLease plugin(callback_operations_);
  if (!plugin.Acquire())
    co_return;
  try {
    plugin.Release();
    const auto adapter = co_await BluetoothAdapter::GetDefaultAsync();
    if (!plugin.Acquire())
      co_return;
    if (adapter) {
      peripheral_role_supported_ = adapter.IsPeripheralRoleSupported();
    }
  } catch (...) {
    // The state stays unknown and is treated as supported.
  }
}

std::optional<FlutterError> UniversalBlePlugin::StopAdvertising() {
  std::lock_guard<std::mutex> lock(peripheral_mutex_);
  DisposeAdvertisementPublisher();
  peripheral_advertising_targets_lc_.clear();
  for (auto const &[key, provider] : peripheral_service_provider_map_) {
    try {
      provider->obj.StopAdvertising();
    } catch (...) {
    }
  }
  PostPeripheralReport([](UniversalBlePeripheralCallback *channel) {
    channel->OnAdvertisingStateChange(PeripheralAdvertisingState::kIdle,
                                      nullptr, SuccessCallback, ErrorCallback);
  });
  return std::nullopt;
}

std::optional<FlutterError> UniversalBlePlugin::AddService(
    const PeripheralService &service) {
  for (const auto &encoded : service.characteristics()) {
    const auto characteristic = std::any_cast<PeripheralCharacteristic>(
        std::get<flutter::CustomEncodableValue>(encoded));
    if (characteristic.instance_id() != nullptr) {
      return FlutterError("not-supported",
                          "Windows does not support characteristic instance IDs",
                          nullptr);
    }
  }
  PeripheralAddServiceAsync(service);
  return std::nullopt;
}

std::optional<FlutterError> UniversalBlePlugin::RemoveService(
    const std::string &service_id) {
  std::lock_guard<std::mutex> lock(peripheral_mutex_);
  const std::string service_id_lc = to_lower_case(service_id);
  peripheral_advertising_targets_lc_.erase(
      std::remove(peripheral_advertising_targets_lc_.begin(),
                  peripheral_advertising_targets_lc_.end(), service_id_lc),
      peripheral_advertising_targets_lc_.end());

  const auto it = peripheral_service_provider_map_.find(service_id_lc);
  if (it == peripheral_service_provider_map_.end()) {
    return FlutterError("not-found", "Service not found", nullptr);
  }

  DisposePeripheralServiceProvider(it->second.get());
  peripheral_service_provider_map_.erase(it);
  return std::nullopt;
}

std::optional<FlutterError> UniversalBlePlugin::ClearServices() {
  std::lock_guard<std::mutex> lock(peripheral_mutex_);
  for (auto const &[_, gatt_service_object] :
       peripheral_service_provider_map_) {
    DisposePeripheralServiceProvider(gatt_service_object.get());
  }
  peripheral_service_provider_map_.clear();
  peripheral_advertising_targets_lc_.clear();
  return std::nullopt;
}

ErrorOr<flutter::EncodableList> UniversalBlePlugin::GetServices() {
  std::lock_guard<std::mutex> lock(peripheral_mutex_);
  flutter::EncodableList services;
  for (auto const &[key, _] : peripheral_service_provider_map_) {
    services.emplace_back(key);
  }
  return services;
}

ErrorOr<flutter::EncodableList> UniversalBlePlugin::GetSubscribedClients(
    const std::string &characteristic_id, const std::string *characteristic_instance_id) {
  if (characteristic_instance_id != nullptr) {
    return FlutterError("not-supported",
                        "Windows does not support characteristic instance IDs",
                        nullptr);
  }
  std::lock_guard<std::mutex> lock(peripheral_mutex_);
  flutter::EncodableList out;
  auto *char_obj = FindPeripheralGattCharacteristicObject(characteristic_id);
  if (char_obj == nullptr || char_obj->obj == nullptr) {
    return out;
  }
  try {
    auto clients = char_obj->obj.SubscribedClients();
    for (uint32_t i = 0; i < clients.Size(); ++i) {
      auto client = clients.GetAt(i);
      out.push_back(flutter::EncodableValue(
          ParsePeripheralBluetoothClientId(client.Session().DeviceId().Id())));
    }
  } catch (...) {
  }
  return out;
}

std::optional<FlutterError> UniversalBlePlugin::ClearPendingNotifications(const std::string *device_id) {
  // Windows submits synchronously; there is no plugin-owned notification queue.
  (void)device_id;
  return std::nullopt;
}

ErrorOr<std::optional<int64_t>> UniversalBlePlugin::GetMaximumNotifyLength(
    const std::string &device_id) {
  std::lock_guard<std::mutex> lock(peripheral_mutex_);
  for (auto const &[service_id, service_provider] :
       peripheral_service_provider_map_) {
    (void)service_id;
    for (auto const &[characteristic_id, characteristic_object] :
         service_provider->characteristics) {
      (void)characteristic_id;
      try {
        auto clients = characteristic_object->obj.SubscribedClients();
        for (uint32_t i = 0; i < clients.Size(); ++i) {
          auto client = clients.GetAt(i);
          auto id = ParsePeripheralBluetoothClientId(
              client.Session().DeviceId().Id());
          if (to_lower_case(id) != to_lower_case(device_id)) {
            continue;
          }
          const int64_t pdu_size =
              static_cast<int64_t>(client.Session().MaxPduSize());
          return std::optional<int64_t>(std::max<int64_t>(0, pdu_size - 3));
        }
      } catch (...) {
      }
    }
  }
  return std::optional<int64_t>{};
}

std::optional<FlutterError> UniversalBlePlugin::SetAutoConnect(bool auto_connect) {
  // Android-only option; no-op on Windows.
  (void)auto_connect;
  return std::nullopt;
}

std::optional<FlutterError> UniversalBlePlugin::StartAdvertising(
    const flutter::EncodableList &services, const std::string *local_name,
    const int64_t *timeout, const UniversalManufacturerData *manufacturer_data,
    const PeripheralPlatformConfig *platform_config) {
  std::lock_guard<std::mutex> lock(peripheral_mutex_);
  if (local_name != nullptr) {
    return FlutterError("not-supported",
                        "Windows cannot advertise a local name");
  }
  if (manufacturer_data != nullptr && !services.empty()) {
    return FlutterError(
        "not-supported",
        "Windows manufacturer advertising requires services: []");
  }
  if (manufacturer_data != nullptr &&
      (manufacturer_data->company_identifier() < 0 ||
       manufacturer_data->company_identifier() > 0xffff ||
       manufacturer_data->data().size() > 27)) {
    return FlutterError("invalid-arguments",
                        "Manufacturer advertising requires a 16-bit company ID "
                        "and at most 27 payload bytes");
  }
  if (timeout != nullptr && *timeout > 0) {
    UniversalBleLogger::LogDebug(
        "Windows GattServiceProvider advertising timeout is not supported");
  }
  try {
    if (manufacturer_data != nullptr) {
      // Connectionless advertising does not need a registered GATT service.
      // Windows reserves local names and service UUID AD types for the system.
      DisposeAdvertisementPublisher();
      for (const auto &[_, provider] : peripheral_service_provider_map_) {
        try {
          if (provider != nullptr)
            provider->obj.StopAdvertising();
        } catch (...) {
        }
      }
      peripheral_advertising_targets_lc_.clear();
      BluetoothLEAdvertisement advertisement;
      advertisement.ManufacturerData().Append(BluetoothLEManufacturerData(
          static_cast<uint16_t>(manufacturer_data->company_identifier()),
          from_bytevc(manufacturer_data->data())));
      advertisement_publisher_ =
          BluetoothLEAdvertisementPublisher(advertisement);
      const auto callback_operations = callback_operations_;
      advertisement_publisher_status_token_ =
          advertisement_publisher_.StatusChanged([this, callback_operations](
                                                     const auto &publisher,
                                                     const auto &args) {
            const auto callback = callback_operations.TryAcquire();
            if (!callback.has_value())
              return;
            const auto status = args.Status();
            const auto error = args.Error();
            ui_thread_handler_.Post([this, publisher, status, error] {
              std::lock_guard<std::mutex> lock(peripheral_mutex_);
              // A queued callback from a replaced/stopped publisher is stale.
              if (publisher != advertisement_publisher_)
                return;
              const auto state =
                  status == BluetoothLEAdvertisementPublisherStatus::Started
                      ? PeripheralAdvertisingState::kAdvertising
                  : status == BluetoothLEAdvertisementPublisherStatus::Aborted
                      ? PeripheralAdvertisingState::kError
                      : PeripheralAdvertisingState::kIdle;
              const auto message = ParsePeripheralBluetoothError(error);
              peripheral_callback_channel_->OnAdvertisingStateChange(
                  state,
                  state == PeripheralAdvertisingState::kError ? &message
                                                              : nullptr,
                  SuccessCallback, ErrorCallback);
            });
          });
      advertisement_publisher_.Start();
      return std::nullopt;
    }
    if (peripheral_service_provider_map_.empty()) {
      return FlutterError("failed", "No services added to advertise");
    }
    std::vector<std::string> selected_services_lc;
    selected_services_lc.reserve(services.size());
    for (const auto &service_encoded : services) {
      const auto &service_id = std::get<std::string>(service_encoded);
      const auto service_id_lc = to_lower_case(service_id);
      selected_services_lc.push_back(service_id_lc);
      if (peripheral_service_provider_map_.count(service_id_lc) == 0) {
        return FlutterError("not-found",
                            "Service not found for advertising: " + service_id);
      }
      if (peripheral_service_provider_map_[service_id_lc] == nullptr) {
        return FlutterError("failed",
                            "Service provider is null: " + service_id);
      }
    }

    auto params = GattServiceProviderAdvertisingParameters();
    params.IsDiscoverable(true);
    params.IsConnectable(true);
    DisposeAdvertisementPublisher();
    for (auto const &[key, provider] : peripheral_service_provider_map_) {
      const bool should_start =
          selected_services_lc.empty() ||
          std::find(selected_services_lc.begin(), selected_services_lc.end(),
                    key) != selected_services_lc.end();
      if (!should_start) {
        continue;
      }
      if (provider == nullptr) {
        return FlutterError("failed", "Service provider is null: " + key);
      }
      if (provider->obj.AdvertisementStatus() !=
          GattServiceProviderAdvertisementStatus::Started) {
        provider->obj.StartAdvertising(params);
      }
    }
    peripheral_advertising_targets_lc_ = std::move(selected_services_lc);
    return std::nullopt;
  } catch (const hresult_error &err) {
    DisposeAdvertisementPublisher();
    return FlutterError("failed", "Failed to start advertising (hr=" +
                                      std::to_string(err.code()) +
                                      "): " + to_string(err.message()));
  } catch (...) {
    DisposeAdvertisementPublisher();
    return FlutterError("failed", "Failed to start advertising");
  }
}

void UniversalBlePlugin::DisposeAdvertisementPublisher() {
  if (!advertisement_publisher_)
    return;
  auto publisher = advertisement_publisher_;
  advertisement_publisher_ = nullptr;
  try {
    publisher.StatusChanged(advertisement_publisher_status_token_);
  } catch (...) {
  }
  try {
    publisher.Stop();
  } catch (...) {
  }
}

std::optional<FlutterError> UniversalBlePlugin::UpdateCharacteristic(
    const std::string &characteristic_id, const std::string *characteristic_instance_id,
    const std::vector<uint8_t> &value, const std::string *device_id) {
  if (characteristic_instance_id != nullptr) {
    return FlutterError("not-supported",
                        "Windows does not support characteristic instance IDs",
                        nullptr);
  }
  GattLocalCharacteristic local_char = nullptr;
  IBuffer buffer = nullptr;
  {
    std::lock_guard<std::mutex> lock(peripheral_mutex_);
    if (device_id != nullptr) {
      return FlutterError("not-supported",
                          "Windows does not support targeting a specific "
                          "device for notifications",
                          nullptr);
    }
    bool ambiguous_match = false;
    auto *characteristic_object = FindPeripheralGattCharacteristicObject(
        characteristic_id, &ambiguous_match);
    if (ambiguous_match) {
      return FlutterError("ambiguous-characteristic",
                          "Characteristic UUID exists in multiple services; "
                          "cannot update uniquely",
                          nullptr);
    }
    if (characteristic_object == nullptr) {
      return FlutterError("not-found", "Characteristic not found", nullptr);
    }
    IBuffer bytes = from_bytevc(value);
    DataWriter writer;
    writer.ByteOrder(ByteOrder::LittleEndian);
    writer.WriteBuffer(bytes);
    local_char = characteristic_object->obj;
    buffer = writer.DetachBuffer();
  }

  try {
    // NotifyValueAsync completes only once the value has been handed to every
    // subscribed client, so waiting for it costs the platform thread a full
    // GATT round trip (and blocks it for good when a client link is wedged).
    // Apple and Android queue the update and return, so submit it and report
    // the per-client outcome from the completion handler instead. Consecutive
    // calls are still submitted from the platform thread in call order, but
    // Windows does not promise that overlapping operations are serialized or
    // delivered in that order.
    const auto value_size = buffer.Length();
    local_char.NotifyValueAsync(buffer).Completed(
        [characteristic_id, value_size](
            const IAsyncOperation<IVectorView<GattClientNotificationResult>>
                &sender,
            const AsyncStatus status) {
          LogPeripheralNotificationResult(characteristic_id, value_size, sender,
                                          status);
        });
  } catch (...) {
    // Starting the operation failed (for example the characteristic object is
    // no longer valid); that is still reported synchronously.
    return FlutterError(
        "failed",
        "Failed to notify subscribed clients for characteristic update",
        nullptr);
  }
  return std::nullopt;
}

// Runs on a WinRT thread pool thread, possibly after this plugin is gone, which
// is why it is static and only touches the failure log and the logger.
void UniversalBlePlugin::LogPeripheralNotificationResult(
    const std::string &characteristic_id, const uint32_t value_size,
    const IAsyncOperation<IVectorView<GattClientNotificationResult>> &sender,
    const AsyncStatus status) noexcept {
  try {
    const std::string context = "PERIPHERAL_NOTIFY " + characteristic_id;
    bool failing = false;
    std::string details;
    if (status != AsyncStatus::Completed) {
      failing = true;
      details = std::string(" did not complete, status=") +
                async_status_to_string(status);
      if (status == AsyncStatus::Error) {
        details += " hr=" + std::to_string(sender.ErrorCode());
        try {
          // GetResults rethrows the operation's exception, whose message is
          // more useful than the bare HRESULT on ErrorCode().
          sender.GetResults();
        } catch (const hresult_error &err) {
          details += " msg=" + to_string(err.message());
        } catch (...) {
        }
      }
    } else {
      // Only requested when something failed: this runs for every update of a
      // characteristic that may be notifying at stream rate.
      const auto client_id = [](const GattClientNotificationResult &result) {
        return ParsePeripheralBluetoothClientId(
            result.SubscribedClient().Session().DeviceId().Id());
      };
      for (const auto &client_result : sender.GetResults()) {
        if (client_result.Status() != GattCommunicationStatus::Success) {
          failing = true;
          const auto status_name =
              gatt_communication_status_to_error(client_result.Status());
          details +=
              " client=" + client_id(client_result) + " status=" +
              (status_name.has_value()
                   ? status_name.value()
                   : std::to_string(static_cast<int>(client_result.Status())));
          if (const auto protocol_error = client_result.ProtocolError()) {
            details += " att_error=" + std::to_string(protocol_error.Value());
          }
          continue;
        }
        if (static_cast<uint32_t>(client_result.BytesSent()) < value_size) {
          // The client's MTU is smaller than the value, so it was truncated on
          // the way out. Nothing else reports this.
          failing = true;
          details += " client=" + client_id(client_result) +
                     " truncated=" + std::to_string(client_result.BytesSent()) +
                     "/" + std::to_string(value_size);
        }
      }
    }

    // A client that stops reading fails every update, so logs are emitted on
    // the transition into and out of failure instead of per notification.
    if (!notification_failure_log().Update(
            characteristic_id, failing,
            UniversalBleLogger::current_log_level() != BleLogLevel::kNone)) {
      return;
    }
    if (failing) {
      UniversalBleLogger::LogError(context + " failed:" + details);
    } else {
      UniversalBleLogger::LogInfo(context + " recovered");
    }
  } catch (const hresult_error &err) {
    try {
      UniversalBleLogger::LogError(
          "PERIPHERAL_NOTIFY hresult_error hr=" + std::to_string(err.code()) +
          " msg=" + to_string(err.message()));
    } catch (...) {
    }
  } catch (...) {
    try {
      UniversalBleLogger::LogError(
          "PERIPHERAL_NOTIFY failed with an unknown exception");
    } catch (...) {
    }
  }
}

// The plugin lease is released across each WinRT creation call. If it cannot
// be retaken the plugin is gone, and the half-built provider is dropped
// without touching plugin state.
fire_and_forget UniversalBlePlugin::PeripheralAddServiceAsync(
    PeripheralService service) {
  ScopedLease plugin(callback_operations_);
  if (!plugin.Acquire())
    co_return;
  const auto service_uuid = service.uuid();
  std::optional<std::string> error;
  try {
    plugin.Release();
    const auto provider_result =
        co_await GattServiceProvider::CreateAsync(uuid_to_guid(service_uuid));
    if (!plugin.Acquire())
      co_return;
    if (provider_result.Error() != BluetoothError::Success) {
      throw std::runtime_error(
          "Failed to create service provider, errorCode: " +
          ParsePeripheralBluetoothError(provider_result.Error()));
    }
    auto provider = std::make_unique<PeripheralGattServiceProviderObject>();
    provider->obj = provider_result.ServiceProvider();

    for (const auto &characteristic_encoded : service.characteristics()) {
      const auto characteristic = std::any_cast<PeripheralCharacteristic>(
          std::get<flutter::CustomEncodableValue>(characteristic_encoded));
      GattLocalCharacteristicParameters parameters;
      for (const auto &property_encoded : characteristic.properties()) {
        parameters.CharacteristicProperties(
            parameters.CharacteristicProperties() |
            ToPeripheralGattCharacteristicProperties(
                std::any_cast<CharacteristicProperty>(
                    std::get<flutter::CustomEncodableValue>(
                        property_encoded))));
      }
      ApplyPeripheralPermissions(characteristic.permissions(), parameters);
      if (characteristic.value() != nullptr) {
        parameters.StaticValue(from_bytevc(*characteristic.value()));
      }

      plugin.Release();
      const auto characteristic_result =
          co_await provider->obj.Service().CreateCharacteristicAsync(
              uuid_to_guid(characteristic.uuid()), parameters);
      if (!plugin.Acquire())
        co_return;
      if (characteristic_result.Error() != BluetoothError::Success) {
        throw std::runtime_error(
            "Failed to create characteristic " + characteristic.uuid() +
            ", errorCode: " +
            ParsePeripheralBluetoothError(characteristic_result.Error()));
      }
      const auto local_characteristic = characteristic_result.Characteristic();

      if (characteristic.descriptors() != nullptr) {
        for (const auto &descriptor_encoded : *characteristic.descriptors()) {
          const auto descriptor = std::any_cast<PeripheralDescriptor>(
              std::get<flutter::CustomEncodableValue>(descriptor_encoded));
          GattLocalDescriptorParameters descriptor_parameters;
          if (descriptor.permissions() != nullptr) {
            ApplyPeripheralPermissions(*descriptor.permissions(),
                                       descriptor_parameters);
          }
          if (descriptor.value() != nullptr) {
            descriptor_parameters.StaticValue(
                from_bytevc(*descriptor.value()));
          }
          plugin.Release();
          const auto descriptor_result =
              co_await local_characteristic.CreateDescriptorAsync(
                  uuid_to_guid(descriptor.uuid()), descriptor_parameters);
          if (!plugin.Acquire())
            co_return;
          if (descriptor_result.Error() != BluetoothError::Success) {
            throw std::runtime_error(
                "Failed to create descriptor " + descriptor.uuid() +
                ", errorCode: " +
                ParsePeripheralBluetoothError(descriptor_result.Error()));
          }
        }
      }

      // Handlers are registered only once the whole service has been built,
      // so a failed build leaves nothing that can call back into the plugin.
      auto characteristic_object =
          std::make_unique<PeripheralGattCharacteristicObject>();
      characteristic_object->obj = local_characteristic;
      provider->characteristics.insert_or_assign(
          guid_to_uuid(local_characteristic.Uuid()),
          std::move(characteristic_object));
    }

    const auto callback_operations = callback_operations_;
    for (auto &[_, characteristic_object] : provider->characteristics) {
      auto &local_characteristic = characteristic_object->obj;
      characteristic_object->stored_clients =
          local_characteristic.SubscribedClients();
      characteristic_object->read_requested_token =
          local_characteristic.ReadRequested(
              [this, callback_operations](const auto &sender,
                                          const auto &args) {
                const auto callback = callback_operations.TryAcquire();
                if (callback.has_value())
                  PeripheralReadRequestedAsync(sender, args);
              });
      characteristic_object->write_requested_token =
          local_characteristic.WriteRequested(
              [this, callback_operations](const auto &sender,
                                          const auto &args) {
                const auto callback = callback_operations.TryAcquire();
                if (callback.has_value())
                  PeripheralWriteRequestedAsync(sender, args);
              });
      characteristic_object->value_changed_token =
          local_characteristic.SubscribedClientsChanged(
              [this, callback_operations](const auto &sender,
                                          const auto &args) {
                const auto callback = callback_operations.TryAcquire();
                if (callback.has_value())
                  PeripheralSubscribedClientsChanged(sender, args);
              });
    }
    provider->advertisement_status_changed_token =
        provider->obj.AdvertisementStatusChanged(
            [this, callback_operations](const auto &sender, const auto &args) {
              const auto callback = callback_operations.TryAcquire();
              if (callback.has_value())
                PeripheralAdvertisementStatusChanged(sender, args);
            });
    {
      std::lock_guard<std::mutex> lock(peripheral_mutex_);
      const auto key = guid_to_uuid(provider->obj.Service().Uuid());
      const auto existing = peripheral_service_provider_map_.find(key);
      if (existing != peripheral_service_provider_map_.end()) {
        DisposePeripheralServiceProvider(existing->second.get());
      }
      peripheral_service_provider_map_.insert_or_assign(key,
                                                        std::move(provider));
    }
  } catch (const hresult_error &err) {
    error = to_string(err.message());
  } catch (const std::exception &ex) {
    error = ex.what();
  } catch (...) {
    error = "Unknown error";
  }
  // An await can throw while the lease is released.
  if (!plugin.Held() && !plugin.Acquire())
    co_return;
  if (error.has_value()) {
    UniversalBleLogger::LogError("PeripheralAddService " + service_uuid +
                                 ": " + error.value());
  }
  PostPeripheralReport([service_uuid, error](
                           UniversalBlePeripheralCallback *peripheral_channel) {
    peripheral_channel->OnServiceAdded(
        service_uuid, error.has_value() ? &error.value() : nullptr,
        SuccessCallback, ErrorCallback);
  });
}

fire_and_forget UniversalBlePlugin::PeripheralSubscribedClientsChanged(
    GattLocalCharacteristic local_char, IInspectable) {
  ScopedLease plugin(callback_operations_);
  if (!plugin.Acquire())
    co_return;
  const auto characteristic_id = guid_to_uuid(local_char.Uuid());
  IVectorView<GattSubscribedClient> current_clients = nullptr;
  IVectorView<GattSubscribedClient> old_clients = nullptr;
  {
    std::lock_guard<std::mutex> lock(peripheral_mutex_);
    auto *characteristic_object =
        FindPeripheralGattCharacteristicObject(characteristic_id);
    if (characteristic_object == nullptr) {
      co_return;
    }
    current_clients = local_char.SubscribedClients();
    old_clients = characteristic_object->stored_clients;
    characteristic_object->stored_clients = current_clients;
  }

  for (uint32_t i = 0; i < current_clients.Size(); ++i) {
    auto client = current_clients.GetAt(i);
    bool found = false;
    for (uint32_t j = 0; j < old_clients.Size(); ++j) {
      if (old_clients.GetAt(j) == client) {
        found = true;
        break;
      }
    }
    if (!found) {
      const auto device_id =
          ParsePeripheralBluetoothClientId(client.Session().DeviceId().Id());
      std::string device_name;
      plugin.Release();
      try {
        auto device_info = co_await DeviceInformation::CreateFromIdAsync(
            client.Session().DeviceId().Id());
        device_name = winrt::to_string(device_info.Name());
      } catch (...) {
        // The name is optional.
      }
      if (!plugin.Acquire())
        co_return;
      PostPeripheralReport(
          [device_id, characteristic_id,
           device_name](UniversalBlePeripheralCallback *peripheral_channel) {
            const std::string *name_ptr =
                device_name.empty() ? nullptr : &device_name;
            peripheral_channel->OnCharacteristicSubscriptionChange(
                device_id, characteristic_id, nullptr, true, name_ptr, SuccessCallback,
                ErrorCallback);
          });
      const int64_t mtu = client.Session().MaxPduSize();
      PostPeripheralReport(
          [device_id, mtu](UniversalBlePeripheralCallback *peripheral_channel) {
            peripheral_channel->OnMtuChange(device_id, mtu, SuccessCallback,
                                            ErrorCallback);
          });
    }
  }

  for (uint32_t i = 0; i < old_clients.Size(); ++i) {
    auto client = old_clients.GetAt(i);
    bool found = false;
    for (uint32_t j = 0; j < current_clients.Size(); ++j) {
      if (current_clients.GetAt(j) == client) {
        found = true;
        break;
      }
    }
    if (!found) {
      const auto device_id =
          ParsePeripheralBluetoothClientId(client.Session().DeviceId().Id());
      PostPeripheralReport(
          [device_id, characteristic_id](
              UniversalBlePeripheralCallback *peripheral_channel) {
            peripheral_channel->OnCharacteristicSubscriptionChange(
                device_id, characteristic_id, nullptr, false, nullptr, SuccessCallback,
                ErrorCallback);
          });
    }
  }
}

fire_and_forget UniversalBlePlugin::PeripheralReadRequestedAsync(
    GattLocalCharacteristic local_char,
    GattReadRequestedEventArgs args) {
  ScopedLease plugin(callback_operations_);
  if (!plugin.Acquire())
    co_return;
  auto deferral = args.GetDeferral();
  try {
    std::string characteristicId = to_uuidstr(local_char.Uuid());
    auto value_holder = std::make_shared<std::vector<uint8_t>>();
    std::vector<uint8_t> *value_arg = nullptr;
    IBuffer charValue = local_char.StaticValue();
    if (charValue != nullptr) {
      *value_holder = to_bytevc(charValue);
      value_arg = value_holder.get();
    }
    plugin.Release();
    auto request = co_await args.GetRequestAsync();
    if (!plugin.Acquire()) {
      deferral.Complete();
      co_return;
    }
    if (request == nullptr) {
      UniversalBleLogger::LogError(
          "PERIPHERAL_READ_REQ: no access to the requesting device");
      deferral.Complete();
      co_return;
    }
    const auto device_id =
        ParsePeripheralBluetoothClientId(args.Session().DeviceId().Id());
    const int64_t offset = request.Offset();
    PostPeripheralReport(
        [device_id, characteristicId, offset, value_arg, value_holder, deferral,
         request](UniversalBlePeripheralCallback *peripheral_channel) {
          peripheral_channel->OnReadRequest(
              device_id, characteristicId, nullptr, offset, value_arg,
              // SuccessCallback
              // Responding throws when the request was cancelled or timed
              // out meanwhile. The deferral is completed either way.
              [request, deferral](const PeripheralReadRequestResult *result) {
                Attempt("PERIPHERAL_READ_REQ response", [&] {
                  if (result == nullptr) {
                    request.RespondWithProtocolError(0x01);
                  } else if (result->status() != nullptr) {
                    request.RespondWithProtocolError(
                        ToGattProtocolError(*result->status()));
                  } else {
                    request.RespondWithValue(from_bytevc(result->value()));
                  }
                });
                deferral.Complete();
              },
              // ErrorCallback
              [request, deferral](const FlutterError &error) {
                Attempt("PERIPHERAL_READ_REQ error response",
                        [&] { request.RespondWithProtocolError(0x0E); });
                deferral.Complete();
              });
        });
  } catch (...) {
    deferral.Complete();
  }
}

fire_and_forget UniversalBlePlugin::PeripheralWriteRequestedAsync(
    GattLocalCharacteristic localChar,
    GattWriteRequestedEventArgs args) {
  ScopedLease plugin(callback_operations_);
  if (!plugin.Acquire())
    co_return;
  auto deferral = args.GetDeferral();
  try {
    std::string characteristicId = to_uuidstr(localChar.Uuid());

    plugin.Release();
    GattWriteRequest request = co_await args.GetRequestAsync();
    if (!plugin.Acquire()) {
      deferral.Complete();
      co_return;
    }
    if (request == nullptr) {
      deferral.Complete();
      co_return;
    }

    std::string deviceId =
        ParsePeripheralBluetoothClientId(args.Session().DeviceId().Id());

    // Each property is read on its own, so one failing read still lets the
    // request reach Dart with the others.
    int64_t offset = 0;
    Attempt("PERIPHERAL_WRITE_REQ offset", [&] { offset = request.Offset(); });
    bool with_response = false;
    Attempt("PERIPHERAL_WRITE_REQ option", [&] {
      with_response = request.Option() == GattWriteOption::WriteWithResponse;
    });
    auto value_holder = std::make_shared<std::vector<uint8_t>>();
    Attempt("PERIPHERAL_WRITE_REQ value", [&] {
      if (request.Value() != nullptr) {
        *value_holder = to_bytevc(request.Value());
      }
    });

    PostPeripheralReport(
        [characteristicId, offset, value_holder, request, deferral, deviceId,
         with_response](UniversalBlePeripheralCallback *peripheral_channel) {
          peripheral_channel->OnWriteRequest(
              deviceId, characteristicId, nullptr, offset, value_holder.get(),
              // A write without response takes no answer. Responding throws
              // when the request was cancelled meanwhile; the deferral is
              // completed either way.
              [deferral, request,
               with_response](const PeripheralWriteRequestResult *writeResult) {
                if (with_response) {
                  Attempt("PERIPHERAL_WRITE_REQ response", [&] {
                    if (writeResult != nullptr &&
                        writeResult->status() != nullptr) {
                      request.RespondWithProtocolError(
                          ToGattProtocolError(*writeResult->status()));
                    } else {
                      request.Respond();
                    }
                  });
                }
                deferral.Complete();
              },
              [deferral, request, with_response](const FlutterError &) {
                if (with_response) {
                  Attempt("PERIPHERAL_WRITE_REQ error response",
                          [&] { request.RespondWithProtocolError(0x0E); });
                }
                deferral.Complete();
              });
        });
  } catch (...) {
    LogCurrentException("PERIPHERAL_WRITE_REQ");
    deferral.Complete();
  }
}

void UniversalBlePlugin::PeripheralAdvertisementStatusChanged(
    GattServiceProvider const &sender,
    GattServiceProviderAdvertisementStatusChangedEventArgs const &args) {
  if (args.Error() != BluetoothError::Success) {
    auto error_str = ParsePeripheralBluetoothError(args.Error());
    PostPeripheralReport(
        [error_str](UniversalBlePeripheralCallback *peripheral_channel) {
          peripheral_channel->OnAdvertisingStateChange(
              PeripheralAdvertisingState::kError, &error_str, SuccessCallback,
              ErrorCallback);
        });
    return;
  }
  std::lock_guard<std::mutex> lock(peripheral_mutex_);
  if (ArePeripheralAdvertisingTargetsStarted()) {
    PostPeripheralReport(
        [](UniversalBlePeripheralCallback *peripheral_channel) {
          peripheral_channel->OnAdvertisingStateChange(
              PeripheralAdvertisingState::kAdvertising, nullptr,
              SuccessCallback, ErrorCallback);
        });
  }
}

void UniversalBlePlugin::DisposePeripheralServiceProvider(
    PeripheralGattServiceProviderObject *service_provider_object) {
  if (service_provider_object == nullptr) {
    return;
  }
  try {
    if (service_provider_object->obj.AdvertisementStatus() ==
        GattServiceProviderAdvertisementStatus::Started) {
      service_provider_object->obj.StopAdvertising();
    }
  } catch (...) {
  }
  try {
    service_provider_object->obj.AdvertisementStatusChanged(
        service_provider_object->advertisement_status_changed_token);
  } catch (...) {
  }
  for (auto const &[_, characteristic_object] :
       service_provider_object->characteristics) {
    try {
      characteristic_object->obj.ReadRequested(
          characteristic_object->read_requested_token);
      characteristic_object->obj.WriteRequested(
          characteristic_object->write_requested_token);
      characteristic_object->obj.SubscribedClientsChanged(
          characteristic_object->value_changed_token);
    } catch (...) {
    }
  }
}

// Requires peripheral_mutex_. A characteristic UUID can occur in more than
// one service; the first match is returned and `ambiguous_match` is set.
PeripheralGattCharacteristicObject *
UniversalBlePlugin::FindPeripheralGattCharacteristicObject(
    const std::string &characteristic_id, bool *ambiguous_match) {
  const std::string characteristic_id_lc = to_lower_case(characteristic_id);
  PeripheralGattCharacteristicObject *first_match = nullptr;
  for (auto const &[_, service_provider] : peripheral_service_provider_map_) {
    const auto it = service_provider->characteristics.find(characteristic_id_lc);
    if (it == service_provider->characteristics.end())
      continue;
    if (first_match == nullptr) {
      first_match = it->second.get();
    } else if (ambiguous_match != nullptr) {
      *ambiguous_match = true;
    }
  }
  return first_match;
}

bool UniversalBlePlugin::ArePeripheralAdvertisingTargetsStarted() const {
  if (peripheral_service_provider_map_.empty()) {
    return false;
  }
  if (peripheral_advertising_targets_lc_.empty()) {
    for (auto const &[_, service_provider] : peripheral_service_provider_map_) {
      if (service_provider->obj.AdvertisementStatus() !=
          GattServiceProviderAdvertisementStatus::Started) {
        return false;
      }
    }
    return true;
  }
  for (const auto &target_id : peripheral_advertising_targets_lc_) {
    const auto it = peripheral_service_provider_map_.find(target_id);
    if (it == peripheral_service_provider_map_.end()) {
      return false;
    }
    if (it->second->obj.AdvertisementStatus() !=
        GattServiceProviderAdvertisementStatus::Started) {
      return false;
    }
  }
  return true;
}

GattCharacteristicProperties
UniversalBlePlugin::ToPeripheralGattCharacteristicProperties(
    CharacteristicProperty property) {
  switch (property) {
    case CharacteristicProperty::kBroadcast:
      return GattCharacteristicProperties::Broadcast;
    case CharacteristicProperty::kRead:
      return GattCharacteristicProperties::Read;
    case CharacteristicProperty::kWriteWithoutResponse:
      return GattCharacteristicProperties::WriteWithoutResponse;
    case CharacteristicProperty::kWrite:
      return GattCharacteristicProperties::Write;
    case CharacteristicProperty::kNotify:
      return GattCharacteristicProperties::Notify;
    case CharacteristicProperty::kIndicate:
      return GattCharacteristicProperties::Indicate;
    case CharacteristicProperty::kAuthenticatedSignedWrites:
      return GattCharacteristicProperties::AuthenticatedSignedWrites;
    case CharacteristicProperty::kExtendedProperties:
      return GattCharacteristicProperties::ExtendedProperties;
    default:
      return GattCharacteristicProperties::None;
  }
}

std::string UniversalBlePlugin::PeripheralAdvertisementStatusToString(
    GattServiceProviderAdvertisementStatus status) {
  switch (status) {
    case GattServiceProviderAdvertisementStatus::Created:
      return "Created";
    case GattServiceProviderAdvertisementStatus::Started:
      return "Started";
    case GattServiceProviderAdvertisementStatus::Stopped:
      return "Stopped";
    case GattServiceProviderAdvertisementStatus::Aborted:
      return "Aborted";
    case GattServiceProviderAdvertisementStatus::
        StartedWithoutAllAdvertisementData:
      return "StartedWithoutAllAdvertisementData";
    default:
      return "Unknown";
  }
}

std::string UniversalBlePlugin::ParsePeripheralBluetoothClientId(
    hstring client_id) {
  auto id = winrt::to_string(client_id);
  const auto pos = id.find_last_of('-');
  if (pos != std::string::npos) {
    return id.substr(pos + 1);
  }
  return id;
}

std::string UniversalBlePlugin::ParsePeripheralBluetoothError(
    BluetoothError error) {
  switch (error) {
    case BluetoothError::Success:
      return "Success";
    case BluetoothError::RadioNotAvailable:
      return "RadioNotAvailable";
    case BluetoothError::ResourceInUse:
      return "ResourceInUse";
    case BluetoothError::DeviceNotConnected:
      return "DeviceNotConnected";
    case BluetoothError::OtherError:
      return "OtherError";
    case BluetoothError::DisabledByPolicy:
      return "DisabledByPolicy";
    case BluetoothError::NotSupported:
      return "NotSupported";
    case BluetoothError::DisabledByUser:
      return "DisabledByUser";
    case BluetoothError::ConsentRequired:
      return "ConsentRequired";
    case BluetoothError::TransportNotSupported:
      return "TransportNotSupported";
    default:
      return "Unknown";
  }
}

uint8_t UniversalBlePlugin::ToGattProtocolError(int64_t status_code) {
  if (status_code < 0) {
    return 0x01;
  }
  if (status_code > 0xFF) {
    return 0xFF;
  }
  return static_cast<uint8_t>(status_code);
}

}  // namespace universal_ble
