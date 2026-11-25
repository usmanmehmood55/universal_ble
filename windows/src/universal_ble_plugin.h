#ifndef FLUTTER_PLUGIN_UNIVERSAL_BLE_PLUGIN_H_
#define FLUTTER_PLUGIN_UNIVERSAL_BLE_PLUGIN_H_

#include <flutter/method_channel.h>
#include <flutter/plugin_registrar_windows.h>

#include <windows.h>
#include <winrt/Windows.Devices.Bluetooth.Advertisement.h>
#include <winrt/Windows.Devices.Bluetooth.GenericAttributeProfile.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Devices.Radios.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/base.h>

#include "generated/universal_ble.g.h"
#include "address_cleanup_gate.h"
#include "async_operation_tracker.h"
#include "helper/universal_ble_base.h"
#include "helper/universal_enum.h"
#include "helper/utils.h"
#include "ui_thread_handler.hpp"
#include "scan_filter_matcher.h"
#include "scan_result_merge.h"
#include "unleased.h"
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <unordered_map>
#include <vector>

namespace universal_ble {
// What one scan knows about one device.
struct ScanRecord {
  // Every packet merged so far, as reported.
  UniversalBleScanResult result;
  // Heard in a connectable packet; a scan response does not say.
  bool heard_connectable = false;
  ScanResponseWait wait = ScanResponseWait::kNone;
};

// A device Windows knows, from the lookup made when a scan starts.
struct KnownDevice {
  bool is_paired = false;
  std::string name;
};

// A notification or indication subscription. Only subscribed characteristics
// are kept: every other GATT operation looks its characteristic up when it
// runs.
struct Subscription {
  GattCharacteristic characteristic{nullptr};
  // The ValueChanged handler, once installed.
  std::optional<event_token> token;
  // Nonzero while a CCCD write for this characteristic runs; names it.
  uint64_t operation = 0;
};

// Service and characteristic UUID, as the caller passed them.
using SubscriptionKey = std::pair<std::string, std::string>;

// The WinRT objects of one connection, closed together off the UI thread.
struct ConnectionObjects {
  BluetoothLEDevice device{nullptr};
  GattSession session{nullptr};
  std::optional<event_token> status_token;
  std::optional<event_token> services_token;
  std::map<SubscriptionKey, Subscription> subscriptions;
};

// One connect() for an address, from the attempt until it is closed. The
// fields after the constants are guarded by the plugin's connections_mutex_.
struct Connection {
  Connection(uint64_t address, uint64_t generation, bool auto_connect)
      : address(address), generation(generation), auto_connect(auto_connect) {}

  const uint64_t address;
  const uint64_t generation;
  const bool auto_connect;
  // Reported connected. False while the attempt runs, and while the link of
  // an autoConnect connection is down and Windows re-establishes it
  // (GattSession.MaintainConnection).
  bool connected = false;
  // Set when the record leaves the plugin's map, which it is in exactly
  // while this is false. Readable without the lock.
  std::atomic<bool> closed{false};
  // Set when disconnect() or shutdown removed the record.
  std::atomic<bool> ended_by_caller{false};
  ConnectionObjects objects;
  // Holds the address from the attempt's first WinRT call until the objects
  // are closed, so a reconnect waits for the close.
  AddressCleanupGate::Reservation reservation;
  uint64_t next_operation = 0;
  // Counts GattServicesChanged events, so a CCCD write that overlapped one
  // refreshes subscriptions once it is done.
  uint64_t services_changes = 0;
};

// The result of one discovery pass. `stale` marks an error from a service
// object that a Service Changed indication made invalid.
struct ServiceDiscovery {
  flutter::EncodableList services;
  std::optional<FlutterError> error;
  bool stale = false;
};

// A connected record with its device, read under the lock.
struct LiveConnection {
  std::shared_ptr<Connection> connection;
  BluetoothLEDevice device{nullptr};
};

struct PeripheralGattCharacteristicObject {
  GattLocalCharacteristic obj = nullptr;
  IVectorView<GattSubscribedClient> stored_clients;
  winrt::event_token value_changed_token;
  winrt::event_token read_requested_token;
  winrt::event_token write_requested_token;
};

struct PeripheralGattServiceProviderObject {
  GattServiceProvider obj = nullptr;
  winrt::event_token advertisement_status_changed_token;
  std::map<std::string, std::unique_ptr<PeripheralGattCharacteristicObject>>
      characteristics;
};

enum class PeripheralBlePermission {
  none,
  readable,
  writeable,
  readEncryptionRequired,
  writeEncryptionRequired,
};

class UniversalBlePlugin : public flutter::Plugin,
                           public UniversalBlePlatformChannel,
                           public UniversalBlePeripheralChannel {
public:
  static void RegisterWithRegistrar(flutter::PluginRegistrarWindows *registrar);

  UniversalBlePlugin(flutter::PluginRegistrarWindows *registrar);

  ~UniversalBlePlugin();

  // Disallow copy and assign.
  UniversalBlePlugin(const UniversalBlePlugin &) = delete;
  UniversalBlePlugin &operator=(const UniversalBlePlugin &) = delete;

private:
  static void SuccessCallback() {}
  static void ErrorCallback(const FlutterError &error) {
    // Ignore ChannelConnection Error, This might occur because of HotReload
    if (error.code() != "channel-error") {
      UniversalBleLogger::LogError("Callback channel error " + error.code() +
                                   ": " + error.message());
    }
  }
  static int64_t GetCurrentTimestampMillis() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
  }
  static int64_t GetCurrentTimestampMicros() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
  }

  flutter::PluginRegistrarWindows *registrar_;
  bool initialized_ = false;

  // Per plugin instance (one per FlutterEngine): the callback channels. Events
  // must go through this instance's messenger; a process-global channel would
  // be overwritten by the next engine's registration and reset on any engine's
  // teardown, dropping the surviving engine's callbacks.
  std::unique_ptr<UniversalBleCallbackChannel> callback_channel_;
  std::unique_ptr<UniversalBlePeripheralCallback> peripheral_callback_channel_;

  // PostCentralReport/PostPeripheralReport post callbacks that capture this
  // instance's channel; the few posted directly capture `this` for other state.
  // Both are safe because the destructor shuts this handler down before it
  // resets the channels, and Shutdown drops still-queued callbacks instead of
  // running them.
  UniversalBleUiThreadHandler ui_thread_handler_;
  AsyncOperationTracker callback_operations_;
  AsyncOperationTracker cleanup_operations_;
  // Guarded by connections_mutex_.
  AddressCleanupGate cleanup_gate_;
  Radio bluetooth_radio_{nullptr};
  // Unset until the default adapter has reported it.
  std::optional<bool> peripheral_role_supported_;
  RadioState old_radio_state_ = RadioState::Unknown;
  BluetoothLEAdvertisementWatcher bluetooth_le_watcher_{nullptr};

  // One record per address, and the records' fields: connections_mutex_.
  std::unordered_map<uint64_t, std::shared_ptr<Connection>> connections_{};
  uint64_t next_connect_generation_ = 0;
  std::mutex connections_mutex_;
  // State of the current scan, guarded by scan_mutex_. Each start or stop
  // begins a new generation; packets from a retired watcher, lookups made for
  // an earlier generation and reports queued before it are dropped.
  std::mutex scan_mutex_;
  uint64_t scan_generation_ = 0;
  BluetoothLEAdvertisementWatcher scan_watcher_{nullptr};
  ScanFilterValues scan_filter_{};
  // Paired LE devices by lower-case address, from the lookup for this scan.
  std::unordered_map<std::string, KnownDevice> known_devices_{};
  // DeviceInformation id to lower-case address, for Removed, which carries
  // only the id.
  std::unordered_map<std::string, std::string> paired_ids_{};
  bool pairing_state_known_ = false;
  std::unordered_map<std::string, ScanRecord> scan_records_{};

  // Watches the paired devices while a scan runs. Platform thread only.
  DeviceWatcher paired_watcher_{nullptr};
  event_token paired_added_token_;
  event_token paired_updated_token_;
  event_token paired_removed_token_;
  event_token paired_completed_token_;
  event_token bluetooth_le_watcher_received_token_;
  event_token bluetooth_le_watcher_stopped_token_;
  event_revoker<IRadio> radio_state_changed_revoker_;

  void InitializeAsync();
  // Post `report` to the UI thread with this instance's callback channel. Keeps
  // channel access in one place, and the callback never needs to capture
  // `this`. The caller must hold a `callback_operations_` lease: the channel
  // pointer is read here, on the caller's thread, and the destructor only
  // resets the channels once every lease has been released.
  void PostCentralReport(
      std::function<void(UniversalBleCallbackChannel *)> &&report);
  void PostPeripheralReport(
      std::function<void(UniversalBlePeripheralCallback *)> &&report);
  // An event handler that runs `handler` only while the plugin accepts
  // callbacks, holding the lease while it does.
  template <typename Handler> auto Guarded(Handler handler) {
    return [operations = callback_operations_,
            handler = std::move(handler)](auto &&...args) {
      const auto lease = operations.TryAcquire();
      if (lease.has_value()) {
        handler(std::forward<decltype(args)>(args)...);
      }
    };
  }

  void StartConnect(
      uint64_t bluetooth_address, bool auto_connect,
      const std::shared_ptr<Connection> &stale_connection = nullptr);
  fire_and_forget ConnectAsync(std::shared_ptr<Connection> connection);
  // What ConfirmLinkAsync confirms: a link that has just come up, or the
  // link of a connected record that connect() was called for again.
  enum class LinkCheck { kNewLink, kExistingLink };
  fire_and_forget ConfirmLinkAsync(std::shared_ptr<Connection> connection,
                                   LinkCheck check);
  // Requires connections_mutex_. 0 when the address has no record.
  uint64_t CurrentGenerationLocked(uint64_t bluetooth_address) const;
  // Requires connections_mutex_. Removes the record and hands back what has
  // to be closed.
  ConnectionObjects TakeLocked(const std::shared_ptr<Connection> &connection,
                               AddressCleanupGate::Reservation &reservation);
  // Removes `connection`, if it is still current, closes it and reports the
  // failure.
  void FailConnection(const std::shared_ptr<Connection> &connection,
                      const std::string &error) noexcept;
  // Posts a connection event, delivered only if the address is still at
  // `expected_generation` then (0: no record).
  void NotifyConnectionChanged(uint64_t bluetooth_address, bool connected,
                               std::optional<std::string> error,
                               uint64_t expected_generation);
  void ConnectionStatusChanged(const std::weak_ptr<Connection> &connection,
                               const BluetoothLEDevice &sender);
  fire_and_forget RefreshSubscriptionsAsync(
      std::weak_ptr<Connection> connection);
  void CloseConnection(uint64_t bluetooth_address, ConnectionObjects objects,
                       AddressCleanupGate::Reservation reservation);
  static fire_and_forget CloseConnectionAsync(
      ConnectionObjects objects, AsyncOperationTracker::Lease cleanup_lease,
      AddressCleanupGate::Reservation reservation);
  static void RemoveValueChangedHandlers(
      std::map<SubscriptionKey, Subscription> &subscriptions);
  void ResetState();
  // The connected record for `device_id`. Throws kDeviceNotFound when there
  // is none.
  LiveConnection RequireConnected(const std::string &device_id);
  /// RequireConnected, and also throws kDeviceDisconnected when the link is
  /// down. The record stays until WinRT reports the drop, so a request
  /// submitted in between would otherwise be issued and fail with an
  /// unrelated GATT status. A status read that throws counts as connected:
  /// the operation itself reports the real error.
  LiveConnection RequireLive(const std::string &device_id);
  event_token RegisterValueChangedHandler(
      const std::shared_ptr<Connection> &connection,
      const std::string &service_id, const std::string &characteristic_id,
      const GattCharacteristic &characteristic);
  void GattCharacteristicValueChanged(
      const std::weak_ptr<Connection> &connection,
      const std::string &device_id, const std::string &service_id,
      const std::string &characteristic_id,
      const GattValueChangedEventArgs &args);

  fire_and_forget ReadValueAsync(
      std::string device_id, std::string service, std::string characteristic,
      std::function<void(ErrorOr<std::vector<uint8_t>> reply)> result);
  fire_and_forget WriteValueAsync(
      std::string device_id, std::string service, std::string characteristic,
      std::vector<uint8_t> value, BleOutputProperty ble_output_property,
      std::function<void(std::optional<FlutterError> reply)> result);
  fire_and_forget ReadDescriptorValueAsync(
      std::string device_id, std::string service, std::string characteristic,
      std::string descriptor,
      std::function<void(ErrorOr<std::vector<uint8_t>> reply)> result);
  fire_and_forget WriteDescriptorValueAsync(
      std::string device_id, std::string service, std::string characteristic,
      std::string descriptor, std::vector<uint8_t> value,
      std::function<void(std::optional<FlutterError> reply)> result);
  fire_and_forget SetNotifiableAsync(
      std::string device_id, std::string service, std::string characteristic,
      BleInputProperty ble_input_property,
      std::function<void(std::optional<FlutterError> reply)> result);
  fire_and_forget DiscoverServicesAsync(
      std::string device_id, bool with_descriptors,
      std::function<void(ErrorOr<flutter::EncodableList> reply)> result);
  static IAsyncAction DiscoverServicesOnceAsync(
      BluetoothLEDevice device, bool with_descriptors,
      std::shared_ptr<ServiceDiscovery> out);
  fire_and_forget PairAsync(std::string device_id, bool custom_ceremony,
                            std::function<void(ErrorOr<bool> reply)> result);
  fire_and_forget GetSystemDevicesAsync(
      std::vector<std::string> with_services,
      std::function<void(ErrorOr<flutter::EncodableList> reply)> result);
  fire_and_forget UnPairAsync(
      std::string device_id,
      std::function<void(std::optional<FlutterError> reply)> result);
  fire_and_forget IsPairedAsync(
      std::string device_id,
      std::function<void(ErrorOr<bool> reply)> result);

  static void PairingRequestedHandler(
      const DevicePairingRequestedEventArgs &event_args);

  void RadioStateChanged(const Radio &sender, const IInspectable &);
  fire_and_forget SetRadioStateAsync(
      RadioState state, std::function<void(ErrorOr<bool> reply)> result);
  void StartPairedDeviceWatcher(uint64_t scan_generation);
  void StopPairedDeviceWatcher() noexcept;
  void PairedDeviceAdded(uint64_t scan_generation,
                         const DeviceInformation &device);
  void PairedDeviceRemoved(uint64_t scan_generation, const std::string &id);
  void PairedDevicesEnumerated(uint64_t scan_generation);
  void UpdateScanRecordLocked(const std::string &device_id);
  uint64_t ResetScanStateLocked();
  bool ApplyKnownDeviceLocked(UniversalBleScanResult &scan_result) const;
  void DisposeAdvertisementWatcher() noexcept;
  fire_and_forget LoadPeripheralSupportAsync();
  void PushScanResultLocked(UniversalBleScanResult scan_result,
                            std::optional<bool> is_connectable,
                            bool is_scan_response, bool expects_scan_response);
  void ReportScanResultLocked(UniversalBleScanResult scan_result);
  static void MergeScanPacket(UniversalBleScanResult &merged,
                              const UniversalBleScanResult &packet,
                              bool is_scan_response);
  fire_and_forget ReportHeldScanResultAsync(std::string device_id,
                                            uint64_t scan_generation);
  void BluetoothLeWatcherReceived(
      const BluetoothLEAdvertisementWatcher &sender,
      const BluetoothLEAdvertisementReceivedEventArgs &args);
  // Peripheral runtime state
  // Guarded by peripheral_mutex_. Pointers into it must not outlive the lock.
  std::map<std::string, std::unique_ptr<PeripheralGattServiceProviderObject>>
      peripheral_service_provider_map_{};
  /// Lowercased service UUIDs from the last successful `StartAdvertising` call.
  /// Empty means all registered services were selected.
  std::vector<std::string> peripheral_advertising_targets_lc_{};
  BluetoothLEAdvertisementPublisher advertisement_publisher_{nullptr};
  event_token advertisement_publisher_status_token_{};
  event_revoker<IRadio> peripheral_radio_state_changed_revoker_;
  std::mutex peripheral_mutex_;

  // Peripheral helpers
  void DisposeAdvertisementPublisher();  // Requires peripheral_mutex_.
  fire_and_forget PeripheralAddServiceAsync(PeripheralService service);
  fire_and_forget PeripheralReadRequestedAsync(
      GattLocalCharacteristic local_char,
      GattReadRequestedEventArgs args);
  fire_and_forget PeripheralWriteRequestedAsync(
      GattLocalCharacteristic local_char,
      GattWriteRequestedEventArgs args);
  fire_and_forget PeripheralSubscribedClientsChanged(
      GattLocalCharacteristic local_char, IInspectable args);
  void PeripheralAdvertisementStatusChanged(
      GattServiceProvider const &sender,
      GattServiceProviderAdvertisementStatusChangedEventArgs const &args);
  void DisposePeripheralServiceProvider(
      PeripheralGattServiceProviderObject *service_provider_object);
  PeripheralGattCharacteristicObject *FindPeripheralGattCharacteristicObject(
      const std::string &characteristic_id, bool *ambiguous_match = nullptr);
  bool ArePeripheralAdvertisingTargetsStarted() const;
  static uint8_t ToGattProtocolError(int64_t status_code);
  static GattCharacteristicProperties ToPeripheralGattCharacteristicProperties(
      CharacteristicProperty property);
  static std::string PeripheralAdvertisementStatusToString(
      GattServiceProviderAdvertisementStatus status);
  static std::string ParsePeripheralBluetoothClientId(hstring client_id);
  static std::string ParsePeripheralBluetoothError(BluetoothError error);
  static void LogPeripheralNotificationResult(
      const std::string &characteristic_id, uint32_t value_size,
      const IAsyncOperation<IVectorView<GattClientNotificationResult>> &sender,
      AsyncStatus status) noexcept;

  // UniversalBlePlatformChannel implementation.
  void GetBluetoothAvailabilityState(
      std::function<void(ErrorOr<AvailabilityState> reply)> result) override;
  void EnableBluetooth(
      std::function<void(ErrorOr<bool> reply)> result) override;
  void DisableBluetooth(
      std::function<void(ErrorOr<bool> reply)> result) override;
  ErrorOr<BleConnectionState> GetConnectionState(
      const std::string &device_id) override;
  std::optional<FlutterError> SetLogLevel(
      const BleLogLevel &log_level) override;
  std::optional<FlutterError> StartScan(
      const UniversalScanFilter *filter,
      const UniversalScanConfig *config) override;
  std::optional<FlutterError> StopScan() override;
  ErrorOr<bool> IsScanning() override;
  void RequestDevice(
      const AppleAccessorySetupOptions &options,
      std::function<void(ErrorOr<UniversalDevicePickerResult> reply)> result)
      override;
  std::optional<FlutterError> Connect(
      const std::string &device_id, const bool *auto_connect,
      const ConnectionPlatformConfig *platform_config) override;
  std::optional<FlutterError> Disconnect(const std::string &device_id) override;
  ErrorOr<bool> HasPermissions(bool with_android_fine_location) override;
  void RequestPermissions(
      bool with_android_fine_location,
      std::function<void(std::optional<FlutterError> reply)> result) override;
  void DiscoverServices(
      const std::string &device_id, bool with_descriptors,
      const DiscoverServicesPlatformConfig *platform_config,
      std::function<void(ErrorOr<flutter::EncodableList> reply)> result)
      override;
  void SetNotifiable(
      const std::string &device_id, const std::string &service,
      const std::string &characteristic,
      const BleInputProperty &ble_input_property,
      std::function<void(std::optional<FlutterError> reply)> result) override;
  void ReadValue(
      const std::string &device_id, const std::string &service,
      const std::string &characteristic,
      std::function<void(ErrorOr<std::vector<uint8_t>> reply)> result) override;
  void ReadDescriptorValue(
      const std::string &device_id, const std::string &service,
      const std::string &characteristic, const std::string &descriptor,
      std::function<void(ErrorOr<std::vector<uint8_t>> reply)> result) override;
  void WriteValue(
      const std::string &device_id, const std::string &service,
      const std::string &characteristic, const std::vector<uint8_t> &value,
      const BleOutputProperty &ble_output_property,
      std::function<void(std::optional<FlutterError> reply)> result) override;
  void WriteDescriptorValue(
      const std::string &device_id, const std::string &service,
      const std::string &characteristic, const std::string &descriptor,
      const std::vector<uint8_t> &value,
      std::function<void(std::optional<FlutterError> reply)> result) override;
  void RequestMtu(const std::string &device_id, int64_t expected_mtu,
                  std::function<void(ErrorOr<int64_t> reply)> result) override;
  void RequestConnectionPriority(
      const std::string &device_id, const BleConnectionPriority &priority,
      std::function<void(std::optional<FlutterError> reply)> result) override;
  void ReadRssi(const std::string &device_id,
                std::function<void(ErrorOr<int64_t> reply)> result) override;
  void IsPaired(const std::string &device_id,
                std::function<void(ErrorOr<bool> reply)> result) override;
  void Pair(const std::string &device_id,
            std::function<void(ErrorOr<bool> reply)> result) override;
  void UnPair(
      const std::string &device_id,
      std::function<void(std::optional<FlutterError> reply)> result) override;
  void GetSystemDevices(
      const flutter::EncodableList &with_services,
      std::function<void(ErrorOr<flutter::EncodableList> reply)> result)
      override;

  // UniversalBlePeripheralChannel implementation.
  ErrorOr<PeripheralAdvertisingState> GetAdvertisingState() override;
  ErrorOr<PeripheralReadinessState> GetReadinessState() override;
  std::optional<FlutterError> StopAdvertising() override;
  std::optional<FlutterError> AddService(
      const PeripheralService &service) override;
  std::optional<FlutterError> RemoveService(
      const std::string &service_id) override;
  std::optional<FlutterError> ClearServices() override;
  ErrorOr<flutter::EncodableList> GetServices() override;
  std::optional<FlutterError> StartAdvertising(
      const flutter::EncodableList &services, const std::string *local_name,
      const int64_t *timeout,
      const UniversalManufacturerData *manufacturer_data,
      const PeripheralPlatformConfig *platform_config) override;
  std::optional<FlutterError> UpdateCharacteristic(
      const std::string &characteristic_id, const std::string *characteristic_instance_id,
      const std::vector<uint8_t> &value, const std::string *device_id) override;
  ErrorOr<flutter::EncodableList> GetSubscribedClients(
      const std::string &characteristic_id, const std::string *characteristic_instance_id) override;
  std::optional<FlutterError> ClearPendingNotifications(const std::string *device_id) override;
  ErrorOr<std::optional<int64_t>> GetMaximumNotifyLength(
      const std::string &device_id) override;
  std::optional<FlutterError> SetAutoConnect(bool auto_connect) override;
};

}  // namespace universal_ble

#endif  // FLUTTER_PLUGIN_UNIVERSAL_BLE_PLUGIN_H_
