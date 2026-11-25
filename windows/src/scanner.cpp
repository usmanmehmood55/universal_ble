// Scanning: the advertisement watcher, merging packets into one result per
// device, and the paired-device lookup.
#include "universal_ble_plugin.h"

#include "plugin_helpers.h"
#include "advertisement_uuid.h"
#include "scan_result_merge.h"
#include "universal_ble_filter_util.h"

namespace universal_ble {

const auto device_address_key = L"System.Devices.Aep.DeviceAddress";

std::optional<FlutterError> UniversalBlePlugin::StartScan(
    const UniversalScanFilter *filter, const UniversalScanConfig *config) {
  if (!bluetooth_radio_ || bluetooth_radio_.State() != RadioState::On) {
    return create_flutter_error(UniversalBleErrorCode::kBluetoothNotAvailable,
                                "Bluetooth is not available");
  }

  try {
    // A new watcher gives every native event a distinct scan identity.
    DisposeAdvertisementWatcher();
    uint64_t scan_generation = 0;
    {
      std::lock_guard<std::mutex> lock(scan_mutex_);
      scan_generation = ResetScanStateLocked();
      scan_filter_ = filter != nullptr ? ToScanFilter(*filter)
                                       : ScanFilterValues();
    }
    StartPairedDeviceWatcher(scan_generation);

    // A new watcher for every scan; DisposeAdvertisementWatcher above
    // released the previous one.
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
    bluetooth_le_watcher_received_token_ = bluetooth_le_watcher_.Received(
        Guarded([this](const BluetoothLEAdvertisementWatcher &sender,
                       const BluetoothLEAdvertisementReceivedEventArgs &args) {
          BluetoothLeWatcherReceived(sender, args);
        }));
    // Windows stops the watcher on its own when the radio goes off or the
    // adapter fails. Drop it then, so the next StartScan builds a new one.
    bluetooth_le_watcher_stopped_token_ = bluetooth_le_watcher_.Stopped(
        Guarded([this](
            const BluetoothLEAdvertisementWatcher &sender,
            const BluetoothLEAdvertisementWatcherStoppedEventArgs &args) {
          if (args.Error() == BluetoothError::Success)
            return;
          UniversalBleLogger::LogWarning(
              "Scan stopped by Windows: " +
              ParsePeripheralBluetoothError(args.Error()));
          uint64_t generation;
          {
            std::lock_guard<std::mutex> lock(scan_mutex_);
            if (scan_watcher_ != sender)
              return;
            generation = scan_generation_;
          }
          ui_thread_handler_.Post([this, sender, generation] {
            {
              std::lock_guard<std::mutex> lock(scan_mutex_);
              if (scan_watcher_ != sender || scan_generation_ != generation)
                return;
            }
            StopScan();
          });
        }));
    {
      std::lock_guard<std::mutex> lock(scan_mutex_);
      scan_watcher_ = bluetooth_le_watcher_;
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
  std::lock_guard<std::mutex> lock(scan_mutex_);
  ResetScanStateLocked();
  return std::nullopt;
}

// Requires scan_mutex_. Starts a new scan generation: results, lookups and
// queued reports of the previous one are dropped from here on.
uint64_t UniversalBlePlugin::ResetScanStateLocked() {
  scan_records_.clear();
  known_devices_.clear();
  paired_ids_.clear();
  pairing_state_known_ = false;
  return ++scan_generation_;
}

void UniversalBlePlugin::DisposeAdvertisementWatcher() noexcept {
  StopPairedDeviceWatcher();
  if (!bluetooth_le_watcher_)
    return;
  // Packets the watcher is still delivering are dropped from here on.
  Attempt("Retire advertisement watcher", [&] {
    std::lock_guard<std::mutex> lock(scan_mutex_);
    scan_watcher_ = nullptr;
  });
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

// Windows reports an advertisement and its scan response as two packets.
// Android and CoreBluetooth report them merged, so the first advertisement
// of a device that accepts scan requests is held until its scan response
// arrives, or kScanResponseWait passes, and every packet is reported merged
// with what came before it in this scan.
//
// Requires scan_mutex_, so packets, the paired-device list and a scan
// restart apply to the current scan only.
void UniversalBlePlugin::PushScanResultLocked(
    UniversalBleScanResult scan_result,
    const std::optional<bool> is_connectable, const bool is_scan_response,
    const bool expects_scan_response) {
  const auto device_id = scan_result.device_id();
  auto [it, inserted] =
      scan_records_.try_emplace(device_id, ScanRecord{scan_result});
  auto &record = it->second;
  if (!inserted) {
    MergeScanPacket(record.result, scan_result, is_scan_response);
  }
  if (is_connectable.value_or(false)) {
    record.heard_connectable = true;
  }
  // Unknown stays unknown: a concrete `false` would make `connectableOnly`
  // drop every device on a host where the query fails (#373).
  const auto connectable = DeriveConnectableState(
      is_connectable, is_scan_response, record.heard_connectable);
  if (connectable.has_value()) {
    record.result.set_connectable(connectable.value());
  } else {
    record.result.set_connectable(static_cast<const bool *>(nullptr));
  }

  const auto step = NextScanResponseWait(record.wait, is_scan_response,
                                         expects_scan_response);
  record.wait = step.state;
  if (step.start_timer) {
    ReportHeldScanResultAsync(device_id, scan_generation_);
  }
  if (step.report) {
    ReportScanResultLocked(record.result);
  }
}

// Merges `packet` into what the scan already knows of its device. An
// advertisement replaces the fields it carries; its scan response adds to
// them, since the two carry different parts of the device's data. Fields a
// packet does not carry are kept.
void UniversalBlePlugin::MergeScanPacket(UniversalBleScanResult &merged,
                                         const UniversalBleScanResult &packet,
                                         const bool is_scan_response) {
  const auto has_items = [](const auto *value) {
    return value != nullptr && !value->empty();
  };
  // A shortened name does not replace the complete one.
  if (has_items(packet.name()) &&
      !(has_items(merged.name()) &&
        merged.name()->size() > packet.name()->size())) {
    merged.set_name(*packet.name());
  }
  if (packet.is_paired() != nullptr) {
    merged.set_is_paired(packet.is_paired());
  }
  if (packet.rssi() != nullptr) {
    merged.set_rssi(packet.rssi());
  }
  if (has_items(packet.services())) {
    auto services = *packet.services();
    if (is_scan_response && has_items(merged.services())) {
      for (const auto &service : *merged.services()) {
        if (std::find(services.begin(), services.end(), service) ==
            services.end()) {
          services.push_back(service);
        }
      }
    }
    merged.set_services(services);
  }
  if (has_items(packet.service_data())) {
    auto data = *packet.service_data();
    if (is_scan_response && has_items(merged.service_data())) {
      data.insert(merged.service_data()->begin(),
                  merged.service_data()->end());
    }
    merged.set_service_data(data);
  }
  if (has_items(packet.manufacturer_data_list())) {
    auto data = *packet.manufacturer_data_list();
    if (is_scan_response && has_items(merged.manufacturer_data_list())) {
      const auto value = [](const flutter::EncodableValue &record) {
        return std::any_cast<const UniversalManufacturerData &>(
            std::get<flutter::CustomEncodableValue>(record));
      };
      for (const auto &record : *merged.manufacturer_data_list()) {
        if (std::none_of(data.begin(), data.end(), [&](const auto &entry) {
              return value(entry) == value(record);
            })) {
          data.push_back(record);
        }
      }
    }
    merged.set_manufacturer_data_list(data);
  }
}

// Requires scan_mutex_. Filters the result and queues it for Flutter.
void UniversalBlePlugin::ReportScanResultLocked(
    UniversalBleScanResult scan_result) {
  if (filterDevice(scan_filter_, scan_result)) {
    const auto timestamp_microseconds = GetCurrentTimestampMicros();
    scan_result.set_timestamp(timestamp_microseconds / 1000);
    scan_result.set_timestamp_microseconds(timestamp_microseconds);
    // Delivered only if no stop or restart came in between.
    ui_thread_handler_.Post([this, scan_result,
                             scan_generation = scan_generation_] {
      {
        std::lock_guard<std::mutex> lock(scan_mutex_);
        if (scan_generation != scan_generation_)
          return;
      }
      callback_channel_->OnScanResult(scan_result, SuccessCallback,
                                      ErrorCallback);
    });
  }
}

// Reports a held advertisement whose scan response did not come in time.
//
// Windows can go seconds without handing the watcher a device's scan
// response, so a result still without a name then takes the one Windows
// keeps for the device from the scan responses it did see. Creating the
// BluetoothLEDevice reads Windows' own record and starts no inquiry, and an
// unknown address answers at once. A scan response that arrives meanwhile
// wins: it is reported, and this report is dropped.
fire_and_forget UniversalBlePlugin::ReportHeldScanResultAsync(
    std::string device_id, const uint64_t scan_generation) {
  constexpr auto kScanResponseWait = std::chrono::milliseconds(500);
  ScopedLease plugin(callback_operations_);
  if (!plugin.Acquire())
    co_return;
  try {
    co_await Unleased(plugin, winrt::resume_after(kScanResponseWait));
    bool needs_name = false;
    {
      std::lock_guard<std::mutex> lock(scan_mutex_);
      if (scan_generation != scan_generation_)
        co_return;
      const auto it = scan_records_.find(device_id);
      if (it == scan_records_.end() ||
          it->second.wait != ScanResponseWait::kHeld)
        co_return;
      const auto *name = it->second.result.name();
      needs_name = name == nullptr || name->empty();
    }
    std::string name;
    if (needs_name) {
      try {
        const auto device = co_await Unleased(
            plugin, BluetoothLEDevice::FromBluetoothAddressAsync(
                        str_to_mac_address(device_id)));
        if (device)
          name = to_string(device.Name());
      } catch (const hresult_error &) {
        LogCurrentException("ReportHeldScanResult name");
      }
    }
    std::lock_guard<std::mutex> lock(scan_mutex_);
    if (scan_generation != scan_generation_)
      co_return;
    const auto it = scan_records_.find(device_id);
    if (it == scan_records_.end() ||
        it->second.wait != ScanResponseWait::kHeld)
      co_return;
    it->second.wait = ScanResponseWait::kDone;
    const auto *current = it->second.result.name();
    if (!name.empty() && (current == nullptr || current->empty()))
      it->second.result.set_name(name);
    ReportScanResultLocked(it->second.result);
  } catch (const PluginGone &) {
  } catch (...) {
    LogCurrentException("ReportHeldScanResult");
  }
}

// Scan results report whether a device is paired, and a paired device that
// does not advertise a name gets the one Windows knows for it. An
// advertisement has neither, so the paired LE devices are watched for the
// length of the scan and matched by address. Devices paired or unpaired
// during the scan, also outside the app, update the results already
// reported.
//
// The watcher is stopped from the scan's own stop, never from one of its
// callbacks, and its events carry the scan generation, so events that come
// after a stop or restart are dropped.
void UniversalBlePlugin::StartPairedDeviceWatcher(
    const uint64_t scan_generation) {
  try {
    auto watcher = DeviceInformation::CreateWatcher(
        BluetoothLEDevice::GetDeviceSelectorFromPairingState(true),
        {device_address_key});
    paired_added_token_ = watcher.Added(Guarded(
        [this, scan_generation](const DeviceWatcher &,
                                const DeviceInformation &device) {
          PairedDeviceAdded(scan_generation, device);
        }));
    // Windows requires an Updated handler. Pairing changes arrive as Added
    // and Removed, as the selector matches paired devices only.
    paired_updated_token_ = watcher.Updated(
        [](const DeviceWatcher &, const DeviceInformationUpdate &) {});
    paired_removed_token_ = watcher.Removed(Guarded(
        [this, scan_generation](const DeviceWatcher &,
                                const DeviceInformationUpdate &update) {
          PairedDeviceRemoved(scan_generation, to_string(update.Id()));
        }));
    paired_completed_token_ = watcher.EnumerationCompleted(Guarded(
        [this, scan_generation](const DeviceWatcher &, const IInspectable &) {
          PairedDevicesEnumerated(scan_generation);
        }));
    watcher.Start();
    paired_watcher_ = watcher;
  } catch (...) {
    // Best effort: without the watcher, scan results leave pairing unset.
    LogCurrentException("StartPairedDeviceWatcher");
  }
}

void UniversalBlePlugin::StopPairedDeviceWatcher() noexcept {
  if (!paired_watcher_)
    return;
  const auto watcher = std::exchange(paired_watcher_, nullptr);
  Attempt("Unregister paired-device handlers", [&] {
    watcher.Added(paired_added_token_);
    watcher.Updated(paired_updated_token_);
    watcher.Removed(paired_removed_token_);
    watcher.EnumerationCompleted(paired_completed_token_);
  });
  Attempt("Stop paired-device watcher", [&] {
    const auto status = watcher.Status();
    if (status == DeviceWatcherStatus::Started ||
        status == DeviceWatcherStatus::EnumerationCompleted) {
      watcher.Stop();
    }
  });
}

void UniversalBlePlugin::PairedDeviceAdded(const uint64_t scan_generation,
                                           const DeviceInformation &device) {
  try {
    const auto properties = device.Properties();
    if (!properties.HasKey(device_address_key))
      return;
    const auto address = lookup_i_property_value(
        properties, device_address_key, "DeviceAddress", "PairedDeviceAdded");
    if (!address)
      return;
    const auto device_id = to_lower_case(to_string(address.GetString()));
    std::lock_guard<std::mutex> lock(scan_mutex_);
    if (scan_generation != scan_generation_)
      return;
    known_devices_.insert_or_assign(
        device_id, KnownDevice{true, to_string(device.Name())});
    paired_ids_.insert_or_assign(to_string(device.Id()), device_id);
    UpdateScanRecordLocked(device_id);
  } catch (...) {
    LogCurrentException("PairedDeviceAdded");
  }
}

void UniversalBlePlugin::PairedDeviceRemoved(const uint64_t scan_generation,
                                             const std::string &id) {
  try {
    std::lock_guard<std::mutex> lock(scan_mutex_);
    if (scan_generation != scan_generation_)
      return;
    const auto it = paired_ids_.find(id);
    if (it == paired_ids_.end())
      return;
    const auto device_id = it->second;
    paired_ids_.erase(it);
    known_devices_.erase(device_id);
    UpdateScanRecordLocked(device_id);
  } catch (...) {
    LogCurrentException("PairedDeviceRemoved");
  }
}

// Once the paired devices are listed, a device missing from them is known
// to be unpaired.
void UniversalBlePlugin::PairedDevicesEnumerated(
    const uint64_t scan_generation) {
  try {
    std::lock_guard<std::mutex> lock(scan_mutex_);
    if (scan_generation != scan_generation_)
      return;
    pairing_state_known_ = true;
    for (const auto &[device_id, record] : scan_records_) {
      UpdateScanRecordLocked(device_id);
    }
  } catch (...) {
    LogCurrentException("PairedDevicesEnumerated");
  }
}

// Requires scan_mutex_. Reports the device's result again when what Windows
// knows about it changed it, unless the result is still held.
void UniversalBlePlugin::UpdateScanRecordLocked(const std::string &device_id) {
  const auto it = scan_records_.find(device_id);
  if (it != scan_records_.end() && ApplyKnownDeviceLocked(it->second.result) &&
      it->second.wait != ScanResponseWait::kHeld) {
    ReportScanResultLocked(it->second.result);
  }
}

// Requires scan_mutex_. Sets the pairing state and, when the result
// has no name, the name Windows knows. Returns whether anything changed.
bool UniversalBlePlugin::ApplyKnownDeviceLocked(
    UniversalBleScanResult &scan_result) const {
  bool changed = false;
  const auto known = known_devices_.find(scan_result.device_id());
  if (known != known_devices_.end() || pairing_state_known_) {
    const bool paired =
        known != known_devices_.end() && known->second.is_paired;
    if (scan_result.is_paired() == nullptr ||
        *scan_result.is_paired() != paired) {
      scan_result.set_is_paired(paired);
      changed = true;
    }
  }
  if (known != known_devices_.end() && !known->second.name.empty() &&
      (scan_result.name() == nullptr || scan_result.name()->empty())) {
    scan_result.set_name(known->second.name);
    changed = true;
  }
  return changed;
}

/// Advertisement received from advertisementWatcher
void UniversalBlePlugin::BluetoothLeWatcherReceived(
    const BluetoothLEAdvertisementWatcher &sender,
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


    // The scan-response flag comes from the advertisement type and not from
    // BluetoothLEAdvertisementReceivedEventArgs::IsScanResponse: the property
    // exists only since Windows 10 2004 (UniversalApiContract v10), so on
    // older releases - including supported LTSC builds - reading it inside
    // this callback fails instead of returning a value, and the catch around
    // this block would then drop every advertisement.
    // BluetoothLEAdvertisementType::ScanResponse has shipped with every
    // Windows 10.
    const auto type = args.AdvertisementType();
    bool is_scan_response = type == BluetoothLEAdvertisementType::ScanResponse;
    bool expects_scan_response =
        type == BluetoothLEAdvertisementType::ConnectableUndirected ||
        type == BluetoothLEAdvertisementType::ScannableUndirected;
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
      if (type == BluetoothLEAdvertisementType::Extended) {
        is_scan_response = connectable_args.IsScanResponse();
        expects_scan_response = connectable_args.IsScannable();
      }
    } else if (should_log_missing_connectable_interface()) {
      UniversalBleLogger::LogWarning(
          "IBluetoothLEAdvertisementReceivedEventArgs2 unavailable; reporting "
          "connectability as unknown (#373)");
    }
    // Use advertisement history to preserve connectability on scan responses,
    // which can carry IsConnectable == false even for a connectable peer.
    if (!is_scan_response && connectable.has_value()) {
      universal_scan_result.set_connectable(*connectable);
    }
    std::lock_guard<std::mutex> lock(scan_mutex_);
    // A packet the watcher delivered as it was being stopped belongs to no
    // scan, as in Microsoft's discovery sample.
    if (sender != scan_watcher_)
      return;
    // Pairing state and a fallback name come from the paired devices, listed
    // when the scan started. Until that list is in, the pairing state is
    // unknown and is left unset.
    ApplyKnownDeviceLocked(universal_scan_result);
    PushScanResultLocked(universal_scan_result, connectable, is_scan_response,
                         expects_scan_response);
  } catch (...) {
    UniversalBleLogger::LogError("ScanResultErrorInParsing");
  }
}

}  // namespace universal_ble
