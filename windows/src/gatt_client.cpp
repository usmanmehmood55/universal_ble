// GATT client: operations that look their characteristic up when they
// run, service discovery, and subscriptions.
#include "universal_ble_plugin.h"

#include "plugin_helpers.h"

namespace universal_ble {

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

void UniversalBlePlugin::ReadValue(
    const std::string &device_id, const std::string &service,
    const std::string &characteristic,
    std::function<void(ErrorOr<std::vector<uint8_t>> reply)> result) {
  ReadValueAsync(device_id, service, characteristic, std::move(result));
}

void UniversalBlePlugin::WriteValue(
    const std::string &device_id, const std::string &service,
    const std::string &characteristic, const std::vector<uint8_t> &value,
    const BleOutputProperty &ble_output_property,
    std::function<void(std::optional<FlutterError> reply)> result) {
  WriteValueAsync(device_id, service, characteristic, value,
                  ble_output_property, std::move(result));
}

void UniversalBlePlugin::ReadDescriptorValue(
    const std::string &device_id, const std::string &service,
    const std::string &characteristic, const std::string &descriptor,
    std::function<void(ErrorOr<std::vector<uint8_t>> reply)> result) {
  ReadDescriptorValueAsync(device_id, service, characteristic, descriptor,
                           std::move(result));
}

void UniversalBlePlugin::WriteDescriptorValue(
    const std::string &device_id, const std::string &service,
    const std::string &characteristic, const std::string &descriptor,
    const std::vector<uint8_t> &value,
    std::function<void(std::optional<FlutterError> reply)> result) {
  WriteDescriptorValueAsync(device_id, service, characteristic, descriptor,
                            value, std::move(result));
}

// GATT operations run on the thread pool, like the rest of the connection's
// WinRT work (see ConnectAsync), and hold the plugin lease only while
// running. A characteristic is looked up for each operation.
fire_and_forget UniversalBlePlugin::ReadValueAsync(
    std::string device_id, std::string service, std::string characteristic,
    std::function<void(ErrorOr<std::vector<uint8_t>> reply)> result) {
  ScopedLease plugin(callback_operations_);
  if (!plugin.Acquire())
    co_return;
  const auto target = device_id + " " + service + " " + characteristic;
  UniversalBleLogger::LogDebugWithTimestamp("READ -> " + target);
  std::shared_ptr<Connection> connection;
  try {
    co_await Unleased(plugin, winrt::resume_background());
    const auto live = RequireLive(device_id);
    connection = live.connection;
    const auto lookup = std::make_shared<CharacteristicLookup>();
    co_await Unleased(plugin,
                      LookupCharacteristicAsync(live.device, service,
                                                characteristic, lookup));
    const auto gatt_characteristic = lookup->Take();
    if ((gatt_characteristic.CharacteristicProperties() &
         GattCharacteristicProperties::Read) ==
        GattCharacteristicProperties::None) {
      throw create_flutter_error(
          UniversalBleErrorCode::kCharacteristicDoesNotSupportRead,
          "Characteristic does not support read");
    }
    const auto read = co_await Unleased(
        plugin,
        gatt_characteristic.ReadValueAsync(BluetoothCacheMode::Uncached));
    if (connection->closed.load())
      throw ConnectionClosed{};
    ThrowIfFailed("ReadValue", target, read);
    safe_reply("ReadValue", result, to_bytevc(read.Value()));
  } catch (const PluginGone &) {
  } catch (...) {
    safe_reply("ReadValue", result, GattFailure(connection, "read"));
  }
}

fire_and_forget UniversalBlePlugin::WriteValueAsync(
    std::string device_id, std::string service, std::string characteristic,
    std::vector<uint8_t> value, BleOutputProperty ble_output_property,
    std::function<void(std::optional<FlutterError> reply)> result) {
  ScopedLease plugin(callback_operations_);
  if (!plugin.Acquire())
    co_return;
  const auto target = device_id + " " + service + " " + characteristic;
  UniversalBleLogger::LogDebugWithTimestamp(
      "WRITE -> " + target + " len=" + std::to_string(value.size()) +
      " property=" + std::to_string(static_cast<int>(ble_output_property)));
  std::shared_ptr<Connection> connection;
  try {
    co_await Unleased(plugin, winrt::resume_background());
    const auto live = RequireLive(device_id);
    connection = live.connection;
    const auto lookup = std::make_shared<CharacteristicLookup>();
    co_await Unleased(plugin,
                      LookupCharacteristicAsync(live.device, service,
                                                characteristic, lookup));
    const auto gatt_characteristic = lookup->Take();
    const auto properties = gatt_characteristic.CharacteristicProperties();
    const bool without_response =
        ble_output_property == BleOutputProperty::kWithoutResponse;
    if (without_response &&
        (properties & GattCharacteristicProperties::WriteWithoutResponse) ==
            GattCharacteristicProperties::None) {
      throw create_flutter_error(
          UniversalBleErrorCode::
              kCharacteristicDoesNotSupportWriteWithoutResponse,
          "Characteristic does not support WriteWithoutResponse");
    }
    if (!without_response && (properties & GattCharacteristicProperties::Write) ==
                                 GattCharacteristicProperties::None) {
      throw create_flutter_error(
          UniversalBleErrorCode::kCharacteristicDoesNotSupportWrite,
          "Characteristic does not support Write");
    }
    const auto write = co_await Unleased(
        plugin, gatt_characteristic.WriteValueWithResultAsync(
                    from_bytevc(value),
                    without_response ? GattWriteOption::WriteWithoutResponse
                                     : GattWriteOption::WriteWithResponse));
    if (connection->closed.load())
      throw ConnectionClosed{};
    ThrowIfFailed("WriteValue", target, write);
    safe_reply("WriteValue", result, std::nullopt);
  } catch (const PluginGone &) {
  } catch (...) {
    safe_reply("WriteValue", result, GattFailure(connection, "write"));
  }
}

fire_and_forget UniversalBlePlugin::ReadDescriptorValueAsync(
    std::string device_id, std::string service, std::string characteristic,
    std::string descriptor,
    std::function<void(ErrorOr<std::vector<uint8_t>> reply)> result) {
  ScopedLease plugin(callback_operations_);
  if (!plugin.Acquire())
    co_return;
  const auto target =
      device_id + " " + service + " " + characteristic + " " + descriptor;
  UniversalBleLogger::LogDebugWithTimestamp("READ_DESCRIPTOR -> " + target);
  std::shared_ptr<Connection> connection;
  try {
    co_await Unleased(plugin, winrt::resume_background());
    const auto live = RequireLive(device_id);
    connection = live.connection;
    const auto lookup = std::make_shared<CharacteristicLookup>();
    co_await Unleased(plugin,
                      LookupCharacteristicAsync(live.device, service,
                                                characteristic, lookup));
    const auto found = co_await Unleased(
        plugin, lookup->Take().GetDescriptorsForUuidAsync(
                    uuid_to_guid(descriptor), BluetoothCacheMode::Uncached));
    ThrowIfFailed("ReadDescriptorValue", target, found);
    const auto read = co_await Unleased(
        plugin, FirstDescriptorOrThrow(found, descriptor)
                    .ReadValueAsync(BluetoothCacheMode::Uncached));
    if (connection->closed.load())
      throw ConnectionClosed{};
    ThrowIfFailed("ReadDescriptorValue", target, read);
    safe_reply("ReadDescriptorValue", result, to_bytevc(read.Value()));
  } catch (const PluginGone &) {
  } catch (...) {
    safe_reply("ReadDescriptorValue", result,
               GattFailure(connection, "descriptor read"));
  }
}

fire_and_forget UniversalBlePlugin::WriteDescriptorValueAsync(
    std::string device_id, std::string service, std::string characteristic,
    std::string descriptor, std::vector<uint8_t> value,
    std::function<void(std::optional<FlutterError> reply)> result) {
  ScopedLease plugin(callback_operations_);
  if (!plugin.Acquire())
    co_return;
  const auto target =
      device_id + " " + service + " " + characteristic + " " + descriptor;
  UniversalBleLogger::LogDebugWithTimestamp(
      "WRITE_DESCRIPTOR -> " + target + " len=" + std::to_string(value.size()));
  std::shared_ptr<Connection> connection;
  try {
    co_await Unleased(plugin, winrt::resume_background());
    const auto live = RequireLive(device_id);
    connection = live.connection;
    const auto lookup = std::make_shared<CharacteristicLookup>();
    co_await Unleased(plugin,
                      LookupCharacteristicAsync(live.device, service,
                                                characteristic, lookup));
    const auto found = co_await Unleased(
        plugin, lookup->Take().GetDescriptorsForUuidAsync(
                    uuid_to_guid(descriptor), BluetoothCacheMode::Uncached));
    ThrowIfFailed("WriteDescriptorValue", target, found);
    const auto write = co_await Unleased(
        plugin, FirstDescriptorOrThrow(found, descriptor)
                    .WriteValueWithResultAsync(from_bytevc(value)));
    if (connection->closed.load())
      throw ConnectionClosed{};
    ThrowIfFailed("WriteDescriptorValue", target, write);
    safe_reply("WriteDescriptorValue", result, std::nullopt);
  } catch (const PluginGone &) {
  } catch (...) {
    safe_reply("WriteDescriptorValue", result,
               GattFailure(connection, "descriptor write"));
  }
}

void UniversalBlePlugin::RequestMtu(
    const std::string &device_id, int64_t expected_mtu,
    std::function<void(ErrorOr<int64_t> reply)> result) {
  UniversalBleLogger::LogDebugWithTimestamp(
      "REQUEST_MTU -> " + device_id +
      " expected=" + std::to_string(expected_mtu));
  try {
    // Read under one lock with the connected check: a link loss in between
    // would leave the record's objects empty.
    GattSession session{nullptr};
    {
      std::lock_guard<std::mutex> lock(connections_mutex_);
      const auto it = connections_.find(str_to_mac_address(device_id));
      if (it != connections_.end() && it->second->connected) {
        session = it->second->objects.session;
      }
    }
    if (!session) {
      throw create_flutter_error(UniversalBleErrorCode::kDeviceNotFound,
                                 "Unknown deviceId: " + device_id);
    }
    // Windows negotiates the MTU itself; this reports the current value.
    result(static_cast<int64_t>(session.MaxPduSize()));
  } catch (...) {
    result(CurrentExceptionAsFlutterError());
  }
}

// After a service change, each subscription's characteristic is looked up
// again and its handler moved to the new object; Microsoft specifies Cached
// here, as Windows refreshes its cache while processing the change. A
// subscription whose characteristic is gone is dropped. A subscription with
// a CCCD write in flight is skipped; the write refreshes again when it is
// done (see SetNotifiableAsync).
fire_and_forget UniversalBlePlugin::RefreshSubscriptionsAsync(
    std::weak_ptr<Connection> weak_connection) {
  ScopedLease plugin(callback_operations_);
  if (!plugin.Acquire())
    co_return;
  try {
    co_await Unleased(plugin, winrt::resume_background());
    const auto connection = weak_connection.lock();
    if (!connection)
      co_return;
    std::vector<SubscriptionKey> keys;
    BluetoothLEDevice device{nullptr};
    {
      std::lock_guard<std::mutex> lock(connections_mutex_);
      if (connection->closed.load())
        co_return;
      device = connection->objects.device;
      for (const auto &[key, subscription] :
           connection->objects.subscriptions) {
        if (subscription.token.has_value())
          keys.push_back(key);
      }
    }
    for (const auto &key : keys) {
      const auto lookup = std::make_shared<CharacteristicLookup>();
      co_await Unleased(plugin, LookupCharacteristicAsync(device, key.first,
                                                          key.second, lookup));
      std::optional<event_token> added;
      if (!lookup->error.has_value()) {
        added = RegisterValueChangedHandler(connection, key.first, key.second,
                                            lookup->characteristic);
      }
      // The handler and object that are no longer needed.
      GattCharacteristic stale{nullptr};
      std::optional<event_token> stale_token;
      {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        auto &subscriptions = connection->objects.subscriptions;
        const auto it = subscriptions.find(key);
        const bool live = !connection->closed.load() &&
                          it != subscriptions.end() &&
                          it->second.operation == 0 &&
                          it->second.token.has_value();
        if (!live) {
          stale = lookup->characteristic;
          stale_token = added;
        } else if (!added.has_value()) {
          UniversalBleLogger::LogWarning(
              "Dropping the subscription to " + key.second +
              " after a service change: " + lookup->error->message());
          stale = it->second.characteristic;
          stale_token = it->second.token;
          subscriptions.erase(it);
        } else {
          stale = std::exchange(it->second.characteristic,
                                lookup->characteristic);
          stale_token = std::exchange(it->second.token, added);
        }
      }
      if (stale && stale_token.has_value()) {
        Attempt("RefreshSubscriptions remove handler",
                [&] { stale.ValueChanged(stale_token.value()); });
      }
    }
  } catch (const PluginGone &) {
  } catch (...) {
    LogCurrentException("RefreshSubscriptionsAsync");
  }
}

// Discovery is on demand: services and their characteristics are read from
// the device, their descriptors, when asked, from Windows' cache, and all of
// them are released once the reply is built.
//
// While Windows processes a Service Changed indication, a service object
// from before the change fails: AccessDenied, or ERROR_FILE_NOT_FOUND or
// RO_E_CLOSED once its handles are gone. Discovery then starts over from the
// service query, a bounded number of times, before the error fails it.
fire_and_forget UniversalBlePlugin::DiscoverServicesAsync(
    std::string device_id, bool with_descriptors,
    std::function<void(ErrorOr<flutter::EncodableList> reply)> result) {
  constexpr int kStaleRetries = 2;
  ScopedLease plugin(callback_operations_);
  if (!plugin.Acquire())
    co_return;
  std::shared_ptr<Connection> connection;
  try {
    co_await Unleased(plugin, winrt::resume_background());
    const auto connected = RequireConnected(device_id);
    connection = connected.connection;
    for (int pass = 0;; ++pass) {
      const auto discovery = std::make_shared<ServiceDiscovery>();
      co_await Unleased(plugin, DiscoverServicesOnceAsync(
                                    connected.device, with_descriptors,
                                    discovery));
      if (connection->closed.load())
        throw ConnectionClosed{};
      if (!discovery->error.has_value()) {
        safe_reply("DiscoverServices", result, std::move(discovery->services));
        co_return;
      }
      if (!discovery->stale || pass >= kStaleRetries)
        throw discovery->error.value();
      UniversalBleLogger::LogInfo(
          "DiscoverServices: a service failed during a service change; "
          "discovering again");
      co_await Unleased(plugin,
                        winrt::resume_after(
                            std::chrono::milliseconds(250 * (pass + 1))));
    }
  } catch (const PluginGone &) {
  } catch (...) {
    safe_reply("DiscoverServices", result,
               GattFailure(connection, "discovery"));
  }
}

// One discovery pass. Touches nothing of the plugin, so it needs no lease.
//
// If Windows denies access to HID over GATT, which it reserves for itself,
// the service is listed with no characteristics.
IAsyncAction UniversalBlePlugin::DiscoverServicesOnceAsync(
    BluetoothLEDevice device, bool with_descriptors,
    std::shared_ptr<ServiceDiscovery> out) {
  try {
    // Discovery reads the device, as discovering services means, so it
    // also brings Windows' cache up to date for the lookups that follow.
    const auto services =
        co_await device.GetGattServicesAsync(BluetoothCacheMode::Uncached);
    ThrowIfFailed("DiscoverServices", "services", services);
    for (const auto &service : services.Services()) {
      const auto service_uuid = guid_to_uuid(service.Uuid());
      const bool system_owned =
          service.Uuid() == GattServiceUuids::HumanInterfaceDevice();
      GattCharacteristicsResult characteristics{nullptr};
      try {
        characteristics = co_await service.GetCharacteristicsAsync(
            BluetoothCacheMode::Uncached);
      } catch (const hresult_error &err) {
        if (err.code() != E_ACCESSDENIED || !system_owned)
          throw;
      }
      flutter::EncodableList universal_characteristics;
      if (!characteristics ||
          (system_owned &&
           characteristics.Status() == GattCommunicationStatus::AccessDenied)) {
        UniversalBleLogger::LogInfo("Service " + service_uuid +
                                    " is reserved by Windows");
      } else {
        if (characteristics.Status() == GattCommunicationStatus::AccessDenied)
          throw hresult_access_denied();
        ThrowIfFailed("DiscoverServices", service_uuid, characteristics);
        for (const auto &characteristic : characteristics.Characteristics()) {
          auto descriptors = flutter::EncodableList();
          if (with_descriptors) {
            const auto found = co_await characteristic.GetDescriptorsAsync(
                BluetoothCacheMode::Cached);
            if (found.Status() == GattCommunicationStatus::Success) {
              for (const auto &descriptor : found.Descriptors()) {
                descriptors.push_back(flutter::CustomEncodableValue(
                    UniversalBleDescriptor(guid_to_uuid(descriptor.Uuid()))));
              }
            } else {
              UniversalBleLogger::LogError(
                  "DiscoverServices: descriptor query for " +
                  guid_to_uuid(characteristic.Uuid()) + " failed with status " +
                  std::to_string(static_cast<int>(found.Status())));
            }
          }
          universal_characteristics.push_back(
              flutter::CustomEncodableValue(UniversalBleCharacteristic(
                  guid_to_uuid(characteristic.Uuid()),
                  properties_to_flutter_encodable(
                      characteristic.CharacteristicProperties()),
                  descriptors)));
        }
      }
      auto universal_service = UniversalBleService(service_uuid);
      universal_service.set_characteristics(universal_characteristics);
      out->services.push_back(flutter::CustomEncodableValue(universal_service));
    }
  } catch (const hresult_error &err) {
    const auto code = err.code();
    out->stale = code == E_ACCESSDENIED ||
                 code == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) ||
                 code == RO_E_CLOSED;
    out->error = CurrentExceptionAsFlutterError();
  } catch (...) {
    // An exception must not leave a WinRT coroutine.
    out->error = CurrentExceptionAsFlutterError();
  }
}

// Writes the CCC descriptor and keeps the subscription in step with it. The
// handler is installed before an enabling write, because a peripheral may
// notify as soon as it has processed the write, before the WinRT operation
// completes. One CCCD write runs per characteristic at a time.
fire_and_forget UniversalBlePlugin::SetNotifiableAsync(
    std::string device_id, std::string service, std::string characteristic,
    BleInputProperty ble_input_property,
    const std::function<void(std::optional<FlutterError> reply)> result) {
  ScopedLease plugin(callback_operations_);
  if (!plugin.Acquire())
    co_return;
  const auto target = device_id + " " + service + " " + characteristic;
  const SubscriptionKey key{service, characteristic};
  std::shared_ptr<Connection> connection;
  GattCharacteristic gatt_characteristic{nullptr};
  std::optional<event_token> provisional_token;
  uint64_t operation = 0;
  // Undoes a started operation: removes a handler installed for it, and
  // forgets a subscription that has no handler.
  const auto roll_back = [&]() noexcept {
    if (provisional_token.has_value()) {
      Attempt("SET_NOTIFY remove provisional handler",
              [&] { gatt_characteristic.ValueChanged(*provisional_token); });
      provisional_token.reset();
    }
    if (operation == 0)
      return;
    Attempt("SET_NOTIFY roll back", [&] {
      std::lock_guard<std::mutex> lock(connections_mutex_);
      auto &subscriptions = connection->objects.subscriptions;
      const auto it = subscriptions.find(key);
      if (it != subscriptions.end() && it->second.operation == operation) {
        it->second.operation = 0;
        if (!it->second.token.has_value())
          subscriptions.erase(it);
      }
    });
    operation = 0;
  };

  try {
    co_await Unleased(plugin, winrt::resume_background());
    UniversalBleLogger::LogDebugWithTimestamp(
        "SET_NOTIFY -> " + target +
        " input=" + std::to_string(static_cast<int>(ble_input_property)));
    const auto live = RequireLive(device_id);
    connection = live.connection;
    const auto lookup = std::make_shared<CharacteristicLookup>();
    co_await Unleased(plugin,
                      LookupCharacteristicAsync(live.device, service,
                                                characteristic, lookup));
    gatt_characteristic = lookup->Take();

    const auto properties = gatt_characteristic.CharacteristicProperties();
    auto descriptor_value =
        GattClientCharacteristicConfigurationDescriptorValue::None;
    if (ble_input_property == BleInputProperty::kNotification) {
      descriptor_value =
          GattClientCharacteristicConfigurationDescriptorValue::Notify;
      if ((properties & GattCharacteristicProperties::Notify) ==
          GattCharacteristicProperties::None) {
        throw create_flutter_error(
            UniversalBleErrorCode::kCharacteristicDoesNotSupportNotify,
            "Characteristic does not support notify");
      }
    } else if (ble_input_property == BleInputProperty::kIndication) {
      descriptor_value =
          GattClientCharacteristicConfigurationDescriptorValue::Indicate;
      if ((properties & GattCharacteristicProperties::Indicate) ==
          GattCharacteristicProperties::None) {
        throw create_flutter_error(
            UniversalBleErrorCode::kCharacteristicDoesNotSupportIndicate,
            "Characteristic does not support indicate");
      }
    }
    const bool enabling =
        descriptor_value !=
        GattClientCharacteristicConfigurationDescriptorValue::None;

    bool subscribed = false;
    uint64_t services_changes = 0;
    {
      std::lock_guard<std::mutex> lock(connections_mutex_);
      if (connection->closed.load())
        throw ConnectionClosed{};
      auto &subscription = connection->objects.subscriptions[key];
      if (subscription.operation != 0) {
        throw create_flutter_error(
            UniversalBleErrorCode::kFailed,
            "Another notification operation is already running for this "
            "characteristic");
      }
      operation = ++connection->next_operation;
      subscription.operation = operation;
      subscribed = subscription.token.has_value();
      services_changes = connection->services_changes;
    }
    if (enabling && !subscribed) {
      provisional_token = RegisterValueChangedHandler(
          connection, service, characteristic, gatt_characteristic);
    }

    const auto write = co_await Unleased(
        plugin,
        gatt_characteristic
            .WriteClientCharacteristicConfigurationDescriptorWithResultAsync(
                descriptor_value));
    if (connection->closed.load())
      throw ConnectionClosed{};
    ThrowIfFailed("SetNotifiable", target, write);

    GattCharacteristic unsubscribed{nullptr};
    std::optional<event_token> unsubscribed_token;
    bool services_changed = false;
    {
      std::lock_guard<std::mutex> lock(connections_mutex_);
      auto &subscriptions = connection->objects.subscriptions;
      const auto it = subscriptions.find(key);
      if (connection->closed.load() || it == subscriptions.end() ||
          it->second.operation != operation) {
        // Dropped with the link meanwhile.
        throw create_flutter_error(
            UniversalBleErrorCode::kDeviceDisconnected,
            enabling ? "Device disconnected while subscribing"
                     : "Device disconnected while unsubscribing");
      }
      if (!enabling) {
        unsubscribed = it->second.characteristic;
        unsubscribed_token = it->second.token;
        subscriptions.erase(it);
      } else {
        if (provisional_token.has_value()) {
          it->second.characteristic = gatt_characteristic;
          it->second.token = std::exchange(provisional_token, std::nullopt);
        }
        it->second.operation = 0;
        services_changed = connection->services_changes != services_changes;
      }
      operation = 0;
    }
    if (unsubscribed && unsubscribed_token.has_value()) {
      unsubscribed.ValueChanged(unsubscribed_token.value());
      UniversalBleLogger::LogInfo("Unsubscribed " + characteristic);
    }
    if (services_changed) {
      // The subscription may hold an object from before the change.
      RefreshSubscriptionsAsync(connection);
    }
    safe_reply("SetNotifiable", result, std::nullopt);
  } catch (const PluginGone &) {
    // Shutdown closes every connection; only this frame's handler is left.
    operation = 0;
    roll_back();
  } catch (...) {
    auto error = GattFailure(connection, "subscription");
    UniversalBleLogger::LogError("SET_NOTIFY failed: " + error.message() +
                                 " <- " + target);
    roll_back();
    safe_reply("SetNotifiable", result, std::move(error));
  }
}

event_token UniversalBlePlugin::RegisterValueChangedHandler(
    const std::shared_ptr<Connection> &connection,
    const std::string &service_id, const std::string &characteristic_id,
    const GattCharacteristic &characteristic) {
  return characteristic.ValueChanged(Guarded(
      [this, weak_connection = std::weak_ptr(connection),
       device_id = mac_address_to_str(connection->address), service_id,
       characteristic_id](const GattCharacteristic &,
                          const GattValueChangedEventArgs &args) {
        GattCharacteristicValueChanged(weak_connection, device_id, service_id,
                                       characteristic_id, args);
      }));
}

void UniversalBlePlugin::GattCharacteristicValueChanged(
    const std::weak_ptr<Connection> &weak_connection,
    const std::string &device_id, const std::string &service_id,
    const std::string &characteristic_id,
    const GattValueChangedEventArgs &args) {
  try {
    const auto connection = weak_connection.lock();
    if (!connection || connection->closed.load()) {
      UniversalBleLogger::LogDebug(
          "Ignoring notification from a closed connection");
      return;
    }
    const auto bytes = to_bytevc(args.CharacteristicValue());

    UniversalBleLogger::LogVerboseWithTimestamp(
        "NOTIFY <- " + device_id + " " + characteristic_id +
        " len=" + std::to_string(bytes.size()));

    const auto timestamp = GetCurrentTimestampMillis();
    PostCentralReport([weak_connection, device_id, service_id,
                       characteristic_id, bytes,
                       timestamp](UniversalBleCallbackChannel *channel) {
      const auto current = weak_connection.lock();
      if (!current || current->closed.load()) {
        return;
      }
      channel->OnValueChanged(device_id, characteristic_id, &service_id, bytes,
                              &timestamp, SuccessCallback, ErrorCallback);
    });
  } catch (...) {
    LogCurrentException("GattCharacteristicValueChanged");
  }
}

}  // namespace universal_ble
