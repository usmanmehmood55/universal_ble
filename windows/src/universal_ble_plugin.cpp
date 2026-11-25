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

#include "enum_parser.h"
#include "callback_drain.h"
#include "generated/universal_ble.g.h"
#include "helper/universal_ble_logger.h"
#include "helper/universal_enum.h"
#include "helper/utils.h"
#include "pin_entry.h"
#include "plugin_helpers.h"

namespace universal_ble {
using universal_ble::ErrorOr;
using universal_ble::UniversalBleCallbackChannel;
using universal_ble::UniversalBlePlatformChannel;
using universal_ble::UniversalBleScanResult;

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
  // Stop new callbacks from entering, let those which already hold a lease
  // finish while every member is still alive, then release owned state.
  //
  // The drain does not pump messages. Pumping during engine teardown runs
  // application code with the engine half gone (#319), and nothing needs it:
  // no coroutine holds the lease while suspended (ScopedLease), so every
  // lease still held belongs to code running on another thread that never
  // waits for this one. Keep it that way: do not hold the lease across an
  // await or block on this thread while holding it.
  callback_operations_.Close();
  WaitForCallbacksWithoutMessagePump(callback_operations_);
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
  SetRadioStateAsync(RadioState::On, std::move(result));
}

void UniversalBlePlugin::DisableBluetooth(
    std::function<void(ErrorOr<bool> reply)> result) {
  SetRadioStateAsync(RadioState::Off, std::move(result));
}

// Replies true once the radio is in `state`. Windows can refuse the change,
// for example when Bluetooth is turned off in Settings.
fire_and_forget UniversalBlePlugin::SetRadioStateAsync(
    RadioState state, std::function<void(ErrorOr<bool> reply)> result) {
  const char *context =
      state == RadioState::On ? "EnableBluetooth" : "DisableBluetooth";
  ScopedLease plugin(callback_operations_);
  if (!plugin.Acquire())
    co_return;
  try {
    if (!bluetooth_radio_) {
      throw create_flutter_error(UniversalBleErrorCode::kBluetoothNotAvailable,
                                 "Bluetooth is not available");
    }
    const auto radio = bluetooth_radio_;
    if (radio.State() != state &&
        co_await Unleased(plugin, radio.SetStateAsync(state)) !=
            RadioAccessStatus::Allowed) {
      throw create_flutter_error(UniversalBleErrorCode::kFailed,
                                 state == RadioState::On
                                     ? "Failed to enable bluetooth"
                                     : "Failed to disable bluetooth");
    }
    safe_reply(context, result, true);
  } catch (const PluginGone &) {
  } catch (...) {
    safe_reply(context, result, CurrentExceptionAsFlutterError());
  }
}

void UniversalBlePlugin::RequestDevice(
    const AppleAccessorySetupOptions &,
    std::function<void(ErrorOr<UniversalDevicePickerResult> reply)> result) {
  result(create_flutter_error(UniversalBleErrorCode::kNotSupported,
                              "AccessorySetupKit is only supported on iOS 18+"));
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

std::optional<FlutterError> UniversalBlePlugin::SetLogLevel(
    const BleLogLevel &log_level) {
  UniversalBleLogger::SetLogLevel(log_level);
  return std::nullopt;
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
  const auto callback_operations = callback_operations_;
  const auto publish_result = [this, callback_operations](
                                  const Radio &radio) noexcept {
    try {
      ui_thread_handler_.Post([this, callback_operations, radio] {
        const auto callback = callback_operations.TryAcquire();
        if (!callback.has_value())
          return;
        try {
          if (radio) {
            bluetooth_radio_ = radio;
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
    Radio::GetRadiosAsync().Completed([callback_operations,
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
      const auto callback = callback_operations.TryAcquire();
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
  // Marked just before a reply goes out, after its value is built, so an
  // exception anywhere earlier still gets the fallback reply below.
  bool replied = false;
  const auto reply = [&](ErrorOr<bool> value) {
    replied = true;
    safe_reply("Pair", result, std::move(value));
  };
  try {
    const auto bluetooth_address = str_to_mac_address(device_id);
    plugin.Release();
    const auto device =
        co_await BluetoothLEDevice::FromBluetoothAddressAsync(bluetooth_address);
    if (!plugin.Acquire())
      co_return;
    if (device == nullptr) {
      reply(create_flutter_error(UniversalBleErrorCode::kDeviceNotFound,
                                  "Unknown deviceId: " + device_id));
      co_return;
    }
    const auto pairing = device.DeviceInformation().Pairing();
    if (pairing.IsPaired()) {
      reply(true);
      co_return;
    }
    if (!pairing.CanPair()) {
      reply(create_flutter_error(UniversalBleErrorCode::kNotPairable,
                                  "Device is not pairable"));
      co_return;
    }

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
    reply(is_paired);
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
      reply(false);
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

/**
 * @brief In some cases, it helps to reset the whole Bluetooth state to get
 * rid of any dangling connections, before scanning or connecting.
 */
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
              std::string service_uuid = guid_to_uuid(service.Uuid());
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

std::optional<FlutterError> UniversalBlePlugin::SetAutoConnect(bool auto_connect) {
  // Android-only option; no-op on Windows.
  (void)auto_connect;
  return std::nullopt;
}

}  // namespace universal_ble
