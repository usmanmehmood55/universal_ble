// Connections: one record per address, connect and autoConnect,
// disconnect, link loss, and closing WinRT objects off the UI thread.
//
// A record is in connections_ exactly while `closed` is false: TakeLocked is
// the only way out of the map, and it sets the flag. So a coroutine that
// holds a record checks `closed` to know whether it is still current.
#include "universal_ble_plugin.h"

#include "plugin_helpers.h"

namespace universal_ble {

ErrorOr<BleConnectionState> UniversalBlePlugin::GetConnectionState(
    const std::string &device_id) {
  try {
    BluetoothLEDevice device{nullptr};
    {
      std::lock_guard<std::mutex> lock(connections_mutex_);
      const auto it = connections_.find(str_to_mac_address(device_id));
      if (it == connections_.end() || !it->second->connected) {
        return BleConnectionState::kDisconnected;
      }
      device = it->second->objects.device;
    }
    return device.ConnectionStatus() == BluetoothConnectionStatus::Connected
               ? BleConnectionState::kConnected
               : BleConnectionState::kDisconnected;
  } catch (...) {
    return CurrentExceptionAsFlutterError();
  }
}

std::optional<FlutterError> UniversalBlePlugin::Connect(
    const std::string &device_id, const bool *auto_connect,
    const ConnectionPlatformConfig *platform_config) {
  // Note: platformConfig carries platform-specific options
  StartConnect(str_to_mac_address(device_id),
               auto_connect != nullptr && *auto_connect);
  return std::nullopt;
}

// A connected record answers at once. A pending attempt with the same
// auto-connect setting absorbs the request; a different setting replaces it,
// so a timed-out auto-connect cannot impose its policy on a later plain
// connect().
//
// A connected record is confirmed before it is reported: right after the
// peripheral drops the link, Windows can still report the old link as
// connected for a moment (HIL FIT-CONN-009). `stale_connection` names a
// record that failed that confirmation, which is replaced although it still
// looks connected.
void UniversalBlePlugin::StartConnect(
    const uint64_t bluetooth_address, const bool auto_connect,
    const std::shared_ptr<Connection> &stale_connection) {
  std::shared_ptr<Connection> connection;
  ConnectionObjects stale;
  AddressCleanupGate::Reservation stale_reservation;
  {
    std::lock_guard<std::mutex> lock(connections_mutex_);
    const auto it = connections_.find(bluetooth_address);
    if (it != connections_.end()) {
      const auto existing = it->second;
      if (existing->connected) {
        bool linked = false;
        try {
          linked = existing != stale_connection &&
                   existing->objects.device.ConnectionStatus() ==
                       BluetoothConnectionStatus::Connected;
        } catch (...) {
          LogCurrentException("Connect: inspect existing connection");
        }
        if (linked) {
          ConfirmLinkAsync(existing, LinkCheck::kExistingLink);
          return;
        }
      } else if (existing->auto_connect == auto_connect) {
        return;
      }
      stale = TakeLocked(existing, stale_reservation);
    }
    connection = std::make_shared<Connection>(
        bluetooth_address, ++next_connect_generation_, auto_connect);
    connections_.emplace(bluetooth_address, connection);
  }
  CloseConnection(bluetooth_address, std::move(stale),
                  std::move(stale_reservation));
  ConnectAsync(connection);
}

std::optional<FlutterError> UniversalBlePlugin::Disconnect(
    const std::string &device_id) {
  const auto address = str_to_mac_address(device_id);
  ConnectionObjects closing;
  AddressCleanupGate::Reservation reservation;
  {
    std::lock_guard<std::mutex> lock(connections_mutex_);
    const auto it = connections_.find(address);
    if (it != connections_.end()) {
      const auto connection = it->second;
      connection->ended_by_caller.store(true);
      closing = TakeLocked(connection, reservation);
    }
    NotifyConnectionChanged(address, false, std::nullopt, 0);
  }
  CloseConnection(address, std::move(closing), std::move(reservation));
  return std::nullopt;
}

LiveConnection
UniversalBlePlugin::RequireConnected(const std::string &device_id) {
  std::lock_guard<std::mutex> lock(connections_mutex_);
  const auto it = connections_.find(str_to_mac_address(device_id));
  if (it == connections_.end() || !it->second->connected) {
    throw create_flutter_error(UniversalBleErrorCode::kDeviceNotFound,
                               "Unknown deviceId: " + device_id);
  }
  return {it->second, it->second->objects.device};
}

LiveConnection UniversalBlePlugin::RequireLive(const std::string &device_id) {
  auto live = RequireConnected(device_id);
  bool linked = true;
  try {
    linked = live.device.ConnectionStatus() ==
             BluetoothConnectionStatus::Connected;
  } catch (...) {
    LogCurrentException("RequireLive: ConnectionStatus");
  }
  if (!linked) {
    throw create_flutter_error(UniversalBleErrorCode::kDeviceDisconnected,
                               "Device Disconnected");
  }
  return live;
}

void UniversalBlePlugin::NotifyConnectionChanged(
    const uint64_t bluetooth_address, const bool connected,
    std::optional<std::string> error, const uint64_t expected_generation) {
  ui_thread_handler_.Post([this, bluetooth_address, connected,
                           error = std::move(error), expected_generation] {
    {
      std::lock_guard<std::mutex> lock(connections_mutex_);
      if (CurrentGenerationLocked(bluetooth_address) != expected_generation) {
        UniversalBleLogger::LogDebug(
            "Ignoring a queued connection event for an earlier connection");
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

uint64_t UniversalBlePlugin::CurrentGenerationLocked(
    const uint64_t bluetooth_address) const {
  const auto it = connections_.find(bluetooth_address);
  return it == connections_.end() ? 0 : it->second->generation;
}

ConnectionObjects UniversalBlePlugin::TakeLocked(
    const std::shared_ptr<Connection> &connection,
    AddressCleanupGate::Reservation &reservation) {
  if (!connection->closed.exchange(true)) {
    connections_.erase(connection->address);
  }
  reservation = std::move(connection->reservation);
  return std::exchange(connection->objects, {});
}

void UniversalBlePlugin::FailConnection(
    const std::shared_ptr<Connection> &connection,
    const std::string &error) noexcept {
  Attempt("FailConnection", [&] {
    ConnectionObjects closing;
    AddressCleanupGate::Reservation reservation;
    {
      std::lock_guard<std::mutex> lock(connections_mutex_);
      if (connection->closed.load()) {
        UniversalBleLogger::LogDebug("Ignoring failure of an earlier attempt: " +
                                     error);
        return;
      }
      UniversalBleLogger::LogError(error);
      closing = TakeLocked(connection, reservation);
      NotifyConnectionChanged(connection->address, false, error, 0);
    }
    CloseConnection(connection->address, std::move(closing),
                    std::move(reservation));
  });
}

// Creates the connection's WinRT objects and hands them to the record, then
// leaves reporting to ConfirmLinkAsync. A failed autoConnect attempt is
// retried here, before the record holds anything: the device may be out of
// range, or unknown to Windows until it advertises again.
//
// The plugin lease is held only while this coroutine runs, never while it is
// suspended: Windows can take seconds to give up on an unreachable device
// and an attempt cannot be cancelled. A superseded attempt stops at its next
// check; whoever removed the record closes what it holds.
//
// Every WinRT object of the connection is created on the thread pool, not on
// the platform thread. C++/WinRT resumes on the calling thread, so the
// objects would otherwise belong to the platform thread, which the Bluetooth
// service calls back into; a Close() waiting on that service then deadlocks
// the platform thread (HIL FIT-CONN-009).
fire_and_forget UniversalBlePlugin::ConnectAsync(
    std::shared_ptr<Connection> connection) {
  constexpr auto kConnectionCleanupWait = std::chrono::seconds(10);
  constexpr auto kAutoConnectRetryDelay = std::chrono::seconds(2);
  ScopedLease plugin(callback_operations_);
  if (!plugin.Acquire())
    co_return;
  const auto address = connection->address;
  const std::weak_ptr<Connection> weak = connection;
  // Objects not yet handed to the record, and the address while they exist.
  ConnectionObjects pending;
  AddressCleanupGate::Reservation ownership;
  for (int attempt = 0;; ++attempt) {
    std::optional<ConnectFailure> failure;
    try {
      if (attempt == 0) {
        co_await Unleased(plugin, winrt::resume_background());
      } else {
        co_await Unleased(plugin, winrt::resume_after(kAutoConnectRetryDelay));
      }
      // Old objects can deny a new connection's lookups until their Close
      // finishes, so wait until every earlier close of the address is done.
      const auto deadline =
          std::chrono::steady_clock::now() + kConnectionCleanupWait;
      for (;;) {
        std::shared_ptr<const AddressCleanupGate::Signal> released;
        {
          std::lock_guard<std::mutex> lock(connections_mutex_);
          if (connection->closed.load())
            co_return;
          released = cleanup_gate_.Released(address);
          if (!released) {
            ownership = cleanup_gate_.Reserve(address);
            break;
          }
        }
        const auto remaining = std::chrono::duration_cast<TimeSpan>(
            deadline - std::chrono::steady_clock::now());
        if (remaining <= TimeSpan::zero())
          throw ConnectFailure{
              "Previous connection to the device is still closing"};
        co_await Unleased(
            plugin, winrt::resume_on_signal(released->handle(), remaining));
      }

      pending.device = co_await Unleased(
          plugin, BluetoothLEDevice::FromBluetoothAddressAsync(address));
      if (!pending.device)
        throw ConnectFailure{
            "Failed to get device. Windows only knows a device that is "
            "paired or was seen in a recent scan"};
      UniversalBleLogger::LogInfo("ConnectionLog: Device found");
      pending.session = co_await Unleased(
          plugin, GattSession::FromDeviceIdAsync(
                      pending.device.BluetoothDeviceId()));
      // The session keeps the link up while nothing else holds it: GATT
      // objects are looked up per operation and released. For autoConnect
      // it also has Windows re-establish a lost link.
      if (pending.session.CanMaintainConnection()) {
        pending.session.MaintainConnection(true);
      } else if (connection->auto_connect) {
        throw ConnectFailure{"Windows cannot maintain this GATT session", true};
      }
      pending.status_token = pending.device.ConnectionStatusChanged(
          Guarded([this, weak](const BluetoothLEDevice &sender,
                               const IInspectable &) {
            ConnectionStatusChanged(weak, sender);
          }));
      pending.services_token = pending.device.GattServicesChanged(
          Guarded([this, weak](const BluetoothLEDevice &,
                               const IInspectable &) {
            if (const auto changed = weak.lock()) {
              {
                std::lock_guard<std::mutex> lock(connections_mutex_);
                ++changed->services_changes;
              }
              RefreshSubscriptionsAsync(weak);
            }
          }));
      break;
    } catch (const PluginGone &) {
      // Only what this frame owns is left to close.
      CloseConnectionAsync(std::move(pending), nullptr, std::move(ownership));
      co_return;
    } catch (const ConnectFailure &err) {
      failure = err;
    } catch (const hresult_error &err) {
      failure = ConnectFailure{"ConnectAsync hresult_error hr=" +
                               std::to_string(err.code()) +
                               " msg=" + to_string(err.message())};
    } catch (const std::exception &ex) {
      failure = ConnectFailure{std::string("ConnectAsync std::exception: ") +
                               ex.what()};
    } catch (...) {
      failure = ConnectFailure{"ConnectAsync unknown exception"};
    }
    // The lease is held: Unleased retakes it before an error is thrown.
    CloseConnection(address, std::exchange(pending, {}),
                    std::exchange(ownership, nullptr));
    if (!connection->auto_connect || failure->permanent) {
      FailConnection(connection, failure->message);
      co_return;
    }
    UniversalBleLogger::LogWarning(failure->message + "; auto-connect retries");
  }

  // Hand the objects to the record: from here on, whoever removes it closes
  // them.
  const auto device = pending.device;
  {
    std::lock_guard<std::mutex> lock(connections_mutex_);
    if (!connection->closed.load()) {
      connection->objects = std::exchange(pending, {});
      connection->reservation = std::exchange(ownership, nullptr);
    }
  }
  if (pending.device) {
    // Superseded while the objects were created.
    CloseConnection(address, std::exchange(pending, {}),
                    std::exchange(ownership, nullptr));
    co_return;
  }
  // A plain connection's first uncached service query opens the link. An
  // autoConnect link is reported by ConnectionStatusChanged once Windows has
  // it up; it may be up already, before the handler was registered.
  bool report = !connection->auto_connect;
  if (!report) {
    try {
      report =
          device.ConnectionStatus() == BluetoothConnectionStatus::Connected;
    } catch (...) {
      LogCurrentException("ConnectAsync: ConnectionStatus");
    }
  }
  if (report)
    ConfirmLinkAsync(connection, LinkCheck::kNewLink);
}

// Reports `connection` connected once an uncached service query shows its
// link is up. The query opens the link of a plain connect(), fills Windows'
// GATT cache for the lookups that follow, and catches a link Windows still
// reports as connected after the peripheral dropped it (HIL FIT-CONN-009).
// Right after a link comes up it can fail transiently, so it is retried.
//
// When it fails:
// - kNewLink, plain: the connection fails.
// - kNewLink, autoConnect: a link that went down again waits for Windows to
//   bring it back. A link still up is reported anyway, since no further
//   status change would come to try again; lookups fall back to an uncached
//   query.
// - kExistingLink (connect() on a connected record): the record is replaced
//   with a new attempt, unless disconnect() or shutdown ended it.
fire_and_forget UniversalBlePlugin::ConfirmLinkAsync(
    std::shared_ptr<Connection> connection, const LinkCheck check) {
  constexpr int kGattReadinessRetries = 2;
  const bool new_link = check == LinkCheck::kNewLink;
  ScopedLease plugin(callback_operations_);
  if (!plugin.Acquire())
    co_return;
  std::optional<std::string> failure;
  try {
    co_await Unleased(plugin, winrt::resume_background());
    BluetoothLEDevice device{nullptr};
    {
      std::lock_guard<std::mutex> lock(connections_mutex_);
      if (new_link && (connection->closed.load() || connection->connected))
        co_return;
      device = connection->objects.device;
    }
    if (!device) {
      // An autoConnect status change can come before ConnectAsync hands the
      // device to the record, which then reports the link itself.
      if (new_link)
        co_return;
      failure = "The connection was closed";
    }
    for (int retry = 0; !failure.has_value(); ++retry) {
      const auto services = co_await Unleased(
          plugin, device.GetGattServicesAsync(BluetoothCacheMode::Uncached));
      const auto error = gatt_communication_status_to_error(services.Status());
      if (!error.has_value())
        break;
      if (retry >= kGattReadinessRetries) {
        if (new_link && connection->auto_connect) {
          UniversalBleLogger::LogWarning(
              "autoConnect: service query failed: " + error.value() +
              "; reporting the link if it is up");
          break;
        }
        failure = "Failed to get services: " + error.value();
        break;
      }
      UniversalBleLogger::LogInfo(
          "ConnectionLog: GATT services temporarily unavailable (" +
          error.value() + "); retrying " + std::to_string(retry + 1) + "/" +
          std::to_string(kGattReadinessRetries));
      co_await Unleased(plugin,
                        winrt::resume_after(
                            std::chrono::milliseconds(250 * (retry + 1))));
      if (connection->closed.load())
        failure = "The connection was closed";
    }
    if (!failure.has_value()) {
      std::lock_guard<std::mutex> lock(connections_mutex_);
      if (connection->closed.load()) {
        failure = "The connection was closed";
      } else if (device.ConnectionStatus() !=
                 BluetoothConnectionStatus::Connected) {
        failure = "Device disconnected while connecting";
      } else if (new_link && connection->connected) {
        co_return;  // Another report got there first.
      } else {
        connection->connected = true;
        UniversalBleLogger::LogInfo("ConnectionLog: Connected");
        NotifyConnectionChanged(connection->address, true, std::nullopt,
                                connection->generation);
        co_return;
      }
    }
  } catch (const PluginGone &) {
    co_return;
  } catch (...) {
    failure = "ConfirmLinkAsync: " + CurrentExceptionAsFlutterError().message();
  }

  if (!new_link) {
    if (connection->ended_by_caller.load())
      co_return;
    UniversalBleLogger::LogInfo("ConnectionLog: the existing link is gone (" +
                                failure.value() + "); connecting again");
    StartConnect(connection->address, connection->auto_connect, connection);
    co_return;
  }
  if (connection->closed.load())
    co_return;
  if (connection->auto_connect) {
    // Windows keeps trying; the next status change reports the link.
    UniversalBleLogger::LogWarning("autoConnect: " + failure.value() +
                                   "; waiting for Windows to reconnect");
    co_return;
  }
  FailConnection(connection, failure.value());
}

// A plain connection ends with its link. An autoConnect connection keeps its
// session, which has Windows re-establish the link; it is reported
// disconnected meanwhile and loses its subscriptions, as the peripheral does.
void UniversalBlePlugin::ConnectionStatusChanged(
    const std::weak_ptr<Connection> &weak_connection,
    const BluetoothLEDevice &sender) {
  try {
    const auto connection = weak_connection.lock();
    if (!connection)
      return;
    const auto status = sender.ConnectionStatus();
    std::map<SubscriptionKey, Subscription> dropped;
    ConnectionObjects closing;
    AddressCleanupGate::Reservation reservation;
    {
      std::lock_guard<std::mutex> lock(connections_mutex_);
      if (connection->closed.load())
        return;
      if (status == BluetoothConnectionStatus::Connected) {
        // A plain attempt reports its own connection.
        if (!connection->auto_connect || connection->connected)
          return;
      } else if (!connection->connected) {
        // An attempt still running reports its own failure.
        return;
      } else if (connection->auto_connect) {
        connection->connected = false;
        dropped.swap(connection->objects.subscriptions);
        NotifyConnectionChanged(connection->address, false, std::nullopt,
                                connection->generation);
      } else {
        closing = TakeLocked(connection, reservation);
        NotifyConnectionChanged(connection->address, false, std::nullopt, 0);
      }
    }
    if (status == BluetoothConnectionStatus::Connected) {
      ConfirmLinkAsync(connection, LinkCheck::kNewLink);
      return;
    }
    RemoveValueChangedHandlers(dropped);
    CloseConnection(connection->address, std::move(closing),
                    std::move(reservation));
  } catch (...) {
    LogCurrentException("ConnectionStatusChanged");
  }
}

// WinRT Close can synchronously wait for COM teardown. Calling it on
// Flutter's STA can deadlock when a peripheral disconnect arrives during that
// wait, so the objects are closed on the thread pool, and the address stays
// reserved until that has finished so a reconnect waits for it.
void UniversalBlePlugin::CloseConnection(
    const uint64_t bluetooth_address, ConnectionObjects objects,
    AddressCleanupGate::Reservation reservation) {
  if (!objects.device && !objects.session) {
    return;
  }
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
    std::lock_guard<std::mutex> lock(connections_mutex_);
    reservation = cleanup_gate_.Reserve(bluetooth_address);
  }
  CloseConnectionAsync(std::move(objects), cleanup.value(),
                       std::move(reservation));
}

// The coroutine frame owns the WinRT objects and leases. No plugin pointer is
// captured: shutdown may leave this worker running with the DLL pinned.
fire_and_forget UniversalBlePlugin::CloseConnectionAsync(
    ConnectionObjects objects, AsyncOperationTracker::Lease cleanup_lease,
    AddressCleanupGate::Reservation reservation) {
  try {
    co_await winrt::resume_background();
    auto &device = objects.device;
    if (device && objects.status_token.has_value()) {
      Attempt("CloseConnection remove connection status handler", [&] {
        device.ConnectionStatusChanged(objects.status_token.value());
      });
    }
    if (device && objects.services_token.has_value()) {
      Attempt("CloseConnection remove GATT services handler", [&] {
        device.GattServicesChanged(objects.services_token.value());
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
      // The writes share one deadline. The address stays reserved until
      // disposal finishes.
      std::vector<std::pair<std::string,
                            IAsyncOperation<GattCommunicationStatus>>>
          writes;
      for (auto &[key, subscription] : objects.subscriptions) {
        if (!subscription.token.has_value())
          continue;
        Attempt("CloseConnection unsubscribe", [&] {
          writes.emplace_back(
              key.second,
              subscription.characteristic
                  .WriteClientCharacteristicConfigurationDescriptorAsync(
                      GattClientCharacteristicConfigurationDescriptorValue::
                          None));
        });
      }
      constexpr auto kUnsubscribeBudget = std::chrono::seconds(2);
      const auto deadline = std::chrono::steady_clock::now() + kUnsubscribeBudget;
      for (auto &write : writes) {
        Attempt("CloseConnection unsubscribe", [&] {
          const auto remaining = std::chrono::duration_cast<TimeSpan>(
              deadline - std::chrono::steady_clock::now());
          if (remaining <= TimeSpan::zero() ||
              write.second.wait_for(remaining) != AsyncStatus::Completed) {
            write.second.Cancel();
            UniversalBleLogger::LogWarning("CloseConnection: unsubscribe of " +
                                           write.first + " did not complete");
          } else if (const auto status = write.second.GetResults();
                     status != GattCommunicationStatus::Success) {
            UniversalBleLogger::LogWarning(
                "CloseConnection: unsubscribe of " + write.first +
                " failed with status " +
                std::to_string(static_cast<int>(status)));
          }
        });
      }
    }
    RemoveValueChangedHandlers(objects.subscriptions);
    if (objects.session) {
      Attempt("CloseConnection close GATT session", [&] {
        objects.session.MaintainConnection(false);
        objects.session.Close();
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
  // Release every old object before allowing another connection.
  objects = {};
  reservation.reset();
}

void UniversalBlePlugin::RemoveValueChangedHandlers(
    std::map<SubscriptionKey, Subscription> &subscriptions) {
  for (auto &[key, subscription] : subscriptions) {
    if (subscription.token.has_value()) {
      Attempt("Remove ValueChanged handler", [&] {
        subscription.characteristic.ValueChanged(subscription.token.value());
      });
      subscription.token.reset();
    }
  }
  subscriptions.clear();
}

void UniversalBlePlugin::ResetState() {
  try {
    DisposeAdvertisementWatcher();
    {
      std::lock_guard<std::mutex> lock(scan_mutex_);
      ResetScanStateLocked();
    }

    // Close every connection and attempt.
    std::vector<std::tuple<uint64_t, ConnectionObjects,
                           AddressCleanupGate::Reservation>>
        closing;
    {
      std::lock_guard<std::mutex> lock(connections_mutex_);
      std::vector<std::shared_ptr<Connection>> connections;
      for (const auto &entry : connections_) {
        connections.push_back(entry.second);
      }
      for (const auto &connection : connections) {
        AddressCleanupGate::Reservation reservation;
        connection->ended_by_caller.store(true);
        auto objects = TakeLocked(connection, reservation);
        closing.emplace_back(connection->address, std::move(objects),
                             std::move(reservation));
      }
    }
    for (auto &[address, objects, reservation] : closing) {
      CloseConnection(address, std::move(objects), std::move(reservation));
    }

    UniversalBleLogger::LogInfo("ResetState: completed clean slate");
  } catch (...) {
    LogCurrentException("ResetState");
  }
}

}  // namespace universal_ble
