// Peripheral role: GATT services, advertising, and requests from
// connected centrals.
#include "universal_ble_plugin.h"

#include "plugin_helpers.h"

namespace universal_ble {

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
      advertisement_publisher_status_token_ =
          advertisement_publisher_.StatusChanged(Guarded([this](
                                                     const auto &publisher,
                                                     const auto &args) {
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
          }));
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
    local_char = characteristic_object->obj;
    buffer = from_bytevc(value);
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
  } catch (...) {
    LogCurrentException("PERIPHERAL_NOTIFY");
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

    for (auto &[_, characteristic_object] : provider->characteristics) {
      auto &local_characteristic = characteristic_object->obj;
      characteristic_object->stored_clients =
          local_characteristic.SubscribedClients();
      characteristic_object->read_requested_token =
          local_characteristic.ReadRequested(
              Guarded([this](const auto &sender, const auto &args) {
                PeripheralReadRequestedAsync(sender, args);
              }));
      characteristic_object->write_requested_token =
          local_characteristic.WriteRequested(
              Guarded([this](const auto &sender, const auto &args) {
                PeripheralWriteRequestedAsync(sender, args);
              }));
      characteristic_object->value_changed_token =
          local_characteristic.SubscribedClientsChanged(
              Guarded([this](const auto &sender, const auto &args) {
                PeripheralSubscribedClientsChanged(sender, args);
              }));
    }
    provider->advertisement_status_changed_token =
        provider->obj.AdvertisementStatusChanged(
            Guarded([this](const auto &sender, const auto &args) {
              PeripheralAdvertisementStatusChanged(sender, args);
            }));
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
    std::string characteristicId = guid_to_uuid(local_char.Uuid());
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
    std::string characteristicId = guid_to_uuid(localChar.Uuid());

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
