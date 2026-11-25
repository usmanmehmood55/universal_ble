# Windows plugin: complexity holes and the simplification plan

This follows `WINDOWS_REVIEW.md`. The review found that the connection lifecycle is the most complex part of the Windows plugin. This document:

1. records what a WinRT probe on the HIL fixture showed about looking up GATT objects per operation;
2. lists every complexity hole found in `windows/src`, ranked by how much each costs;
3. lays out a target design and the order to build it in.

Line references are to `windows/src/universal_ble_plugin.cpp` at a937b74.

## Status (2026-11-26)

Done in fb1fdfb..56f1d20, with D1–D6 as recommended. Hand-written `windows/src` went from 6,817 lines at a937b74 to 5,982 (upstream 32fce88: 7,029).

| Hole | Outcome |
| --- | --- |
| H1, H2 | Done (d70241e). No GATT map; one `Connection` record per address with `TakeLocked` and `CloseConnection`. |
| H3 | Done (d70241e). autoConnect keeps its session; `ReportAutoConnectedAsync` reports the link after an uncached service query. |
| H4 | Done. `Unleased` (`unleased.h`) and `Guarded`. `InitializeAsync` keeps its own guards, see below. |
| H5 | Done (7cfc361). One `ScanRecord`, one merge, every packet reported, 500 ms hold. |
| H6 | Done (fb1fdfb). |
| H7 | Done. The cleanup gate signals an event that `ConnectAsync` awaits. |
| H8 | Radio state and UUID helpers done. `InitializeAsync` left as it is: its nested shape keeps the radio lookup from resuming on the UI thread during shutdown (#319). |
| H9 | Not done: a shared posting helper would save about four lines per call site. |
| H10 | Done (56f1d20): `scanner.cpp`, `connection.cpp`, `gatt_client.cpp`, `peripheral.cpp`, `plugin_helpers.h`. |

What the hardware showed that this plan did not expect:

- **Cached lookups do not read from the device.** When Windows has not cached the database, for example after autoConnect brought the link up, `Cached` answers Success with nothing in it. The probe in section 1 only worked because the cache was already filled. Lookups and discovery retry a miss with `Uncached`, and autoConnect fills the cache before it reports the connection.
- **Service objects go stale during Service Changed.** `GetCharacteristicsAsync` on a service object from before the change fails with AccessDenied, `ERROR_FILE_NOT_FOUND` or `RO_E_CLOSED`. `discoverServices` starts over, up to twice. A CCCD write that overlapped a change refreshes the subscriptions when it is done.
- **The scan-name flake is Windows withholding scan responses.** Packet logs show the watcher delivering a device's advertisements with no scan response for seconds at a time, once for over 30 s; the fixture sends one every time. `BluetoothLEAdvertisementScanParameters::LowLatency()` changed nothing (window 29, interval 189 before and after), and a longer hold did not help. The hold is 500 ms; a result still without a name then takes the name Windows keeps for the device (`FromBluetoothAddressAsync(...).Name()`, 59 ms, no inquiry). Both scan tests then passed 10 of 10.
- **`connect()` reused a stale link (FIT-CONN-009).** It now confirms an apparently connected record with an uncached service query and connects again if that fails.
- **Discovery could return a stale database.** `discoverServices` reads services and characteristics with `Uncached`.

## 1. Probe results (Windows 11, nRF52 HIL fixture, unpaired)

The probe ran WinRT from PowerShell 5.1; the script is not part of the repo. It created a `BluetoothLEDevice` with `FromBluetoothAddressAsync` and ran no discovery first.

| Question | Result |
| --- | --- |
| Does a `Cached` lookup work with no discovery before it? | Yes. `GetGattServicesForUuidAsync(service, Cached)` then `GetCharacteristicsForUuidAsync(characteristic, Cached)` returned the characteristic, and the read succeeded. |
| Does the lookup open the link? | Yes. The status went from `Disconnected` to `Connected`; the first lookup took 1.5–2.0 s. |
| Cost of a lookup once connected | 6–16 ms for service plus characteristic, `Cached`, over 30 lookups. |
| Is each lookup a new object? | Yes. Two lookups of the same service return different objects, even both with `Cached`. |
| Does `Close()` on one object affect the others? | **Yes.** Closing any service object for a UUID closes all of them. Reads through another object's characteristic then fail with `RO_E_CLOSED` (0x80000013). |
| Is releasing an object without `Close()` safe? | Yes. Ten lookups released without `Close` (garbage-collected) left the first object working, and the link stayed up. |

What this means for the design:

- **Operations can look up their own GATT objects.** Nothing has to be discovered at connect, and a lookup costs about 10 ms.
- **No operation may call `Close()` on a service.** Closing one would break every concurrent operation and every subscription on that service. Objects an operation looks up are released, never closed.
- `Close()` belongs only to disconnect, where everything for the device goes at once.
- One step of the current code is not needed. The 2-attempt retry when connect hits `Unreachable` existed because `BluetoothLEDevice` creation does not connect; a `Cached` lookup connects by itself.

## 2. Complexity holes

They are ranked by how much complexity each one costs against how much a fix removes.

### H1. The eager GATT map and everything that guards it (the big one)

At connect, the plugin discovers every service and characteristic and keeps all of them open in `gatt_map`. Every other part of the connection code then has to protect that map:

- `gatt_mutex` and about 15 locked methods on `BluetoothDeviceAgent`, around 260 lines inline in the header;
- the `gatt_operations` tracker, so that a refresh waits until no read or write is using the map;
- `gatt_revision`, `SnapshotSubscriptions` and `ReplaceGattMapIfUnchanged`, so that a refresh does not install a map built from stale subscription state;
- `notification_operation_in_progress` with `Begin`/`Update`/`FinishNotificationOperation`;
- `RefreshGattServicesAsync` (201 lines), with its requested and in-progress flags and two 10 ms polling loops;
- discovery passes during connect (up to 3), the superseded-services list, `CloseSupersededServicesAsync` with a 10 s blocking wait, and `GattDiscovery::proceed`;
- two ways of closing a retired map (`RetireGattMap` and the inline `DisposeGattMap`);
- `ClosingConnection` carrying a whole map.

The probe shows the map is not needed: an operation finds its characteristic in about 10 ms. The only objects the plugin has to keep are the characteristics it is subscribed to, because their `ValueChanged` handlers live on them.

**Simpler:** no map. A connection keeps only its subscriptions.

### H2. A connection is spread over three maps and nine teardown functions

Per address, the state lives in `connected_devices_`, `connect_generations_` and `pending_connects_`, kept consistent under one mutex. Removing a connection goes through up to nine functions:

`EndConnection`, `RemoveConnectedDevice`, `CleanConnection`, `DisposeConnection`, `DisposePartialConnection`, `CloseConnection`, `CloseConnectionAsync`, `ReserveConnectionCleanupLocked` and `ResetState`.

`ConnectAsync` alone is 350 lines, with 9 places where it checks whether it has been superseded.

**Simpler:** one record per address, `{generation, auto_connect, state, device, session, subscriptions}`, with state `connecting`, `connected` or `waiting`. Two operations act on it:

- `Take(address, condition)` removes the record under the lock;
- `Close(record)` closes it on the cleanup worker.

The cleanup gate stays for now, because closing old services can still deny a new connection's lookups. With fewer objects to close, it may turn out to be unnecessary; the HIL suites will show.

### H3. autoConnect rebuilds the connection after every link loss

After a link loss the plugin:

1. closes the `GattSession` (which turns `MaintainConnection` off) and the device;
2. waits for the UI thread;
3. calls `StartConnect` with `only_if_generation`;
4. runs a new `ConnectAsync`, which polls `ConnectionStatus` every 250 ms;
5. on failure, goes through `RetryAutoConnectAsync`, waiting 2 s.

`MaintainConnection` exists precisely so that Windows reconnects by itself while the session is kept.

**Simpler:** an autoConnect record keeps its device and session across a link loss. `ConnectionStatusChanged` reports the disconnect and later the reconnect. Subscriptions are dropped on the disconnect; the README already tells apps to subscribe again. No retry coroutine, no polling and no `only_if_generation`.

The one retry left is for a device Windows does not know yet, where `FromBluetoothAddressAsync` returns null.

### H4. The lease is paired by hand around every await

There are 31 `plugin.Release()` and 54 `plugin.Acquire()` calls in the file. Each await reads:

```cpp
plugin.Release();
x = co_await op;
if (!plugin.Acquire()) { abandon(); co_return; }
```

Each path has to remember its own cleanup (`abandon`, `FinishGattRefresh`, `roll_back`). On top of that, 13 event registrations each copy `callback_operations_` and repeat the same `TryAcquire` guard.

**Simpler:**

- An awaitable wrapper, `x = co_await Unleased(plugin, op)`. It releases, awaits and reacquires. If reacquiring fails, it throws a private `PluginGone` exception that only the outermost `catch` handles.
- Cleanup moves into RAII guards, for example a pending-connection guard whose destructor schedules the close.
- A `Guarded(&UniversalBlePlugin::Method)` helper builds every event handler.

### H5. Scan state is five parallel maps and a 171-line merge

Five maps are keyed by the same address:

- `scan_results_`;
- `connectable_heard_`, which stores only `true`;
- `scan_response_waits_`;
- `scan_name_lookups_`, keyed by `uint64_t` where the others use strings;
- `known_devices_`, plus the separate flag `pairing_state_known_`.

`PushScanResultLocked` combines:

- field backfill from the cache, through `ScanFieldPresence` and `ScanBackfillPlan`, two identical structs;
- a second merge pass that takes the union when a held advertisement meets its scan response;
- a rule that keeps the longer name;
- field-by-field change detection through `ShouldUpdateScanResult` and `DeriveConnectableState`.

The change detection is a Windows-only policy. Android reports every packet (`UniversalBlePlugin.kt:1924`), and Dart deduplicates only when the app asks (`ScanDeduplicator`). Because Windows drops a packet that carries nothing new, RSSI updates for a device that keeps advertising the same data are never reported on Windows.

**Simpler:** one `ScanRecord` per address, `{merged result, heard_connectable, wait state, name lookup started}`, and one merge function: fields the packet carries replace what the record has, and the record fills in the rest. Decision D5 is whether to report every merged packet, as Android does. That would remove the change detection entirely and fix the missing RSSI updates.

### H6. The scan name lookup starts a 30-second inquiry per unnamed device

This is review finding 2.1. It is listed here because the fix is a simplification too: removing `LoadScanNameAsync` also removes `scan_name_lookups_` and one more asynchronous writer into scan state.

### H7. Waits that poll instead of being signalled

There are 7 `resume_after` loops. Four of them disappear with H1 and H3: the subscription snapshot, the idle wait, the 250 ms link poll and the retry delay.

The cleanup-gate wait, 10 ms in `ConnectAsync`, stays unless the gate itself goes (see H2).

### H8. Duplicated plumbing

- **Radio state.** `EnableBluetooth` and `DisableBluetooth` are the same 45 lines apart from `RadioState::On`/`Off` and two strings. They should be one `SetRadioStateAsync(state)` coroutine.
- **Initialization.** `InitializeAsync` (76 lines) nests a `Completed` handler inside a lambda that posts to the UI thread. As a coroutine, like the rest of the file, it would be about half as long.
- **Exception handling.** Six helpers overlap: `safe_reply`, `Attempt`, `LogCurrentException`, `log_and_swallow`, `log_and_swallow_unknown` and `CurrentExceptionAsFlutterError`. Enable/Disable also have their own hand-written catch chains. `Attempt` and `LogCurrentException` already cover the logging, so the two `log_and_swallow` helpers can go.
- **UUID conversion.** `guid_to_uuid` (stringstream) and `to_uuidstr` (`sprintf_s`) in `helper/utils.cpp` produce the same string. `uuid_to_guid` parses by hand, with an unaligned `uint64_t*` cast into `Data4`. One formatter and one parser are enough.
- **Connection lookups.** There are five ways to get a connection: `GetConnectedDevice`, `ConnectedDeviceOrThrow`, `LiveDeviceOrThrow`, `HasLiveLink` and `GetConnectionState`. After H2, two are enough: `Find(address)` and `RequireLive(device_id)`.

### H9. Three ways to post results to the UI thread

1. `PostCentralReport` and `PostPeripheralReport` capture the channel.
2. Scan results post `[this, …]` straight to `ui_thread_handler_` with their own generation check under `scan_mutex_`.
3. `NotifyConnectionChanged` posts `[this, …]` with a check on the connect generation and on pending connects.

**Simpler:** one `PostIfCurrent(check, report)` that runs `check()` on the UI thread before `report(channel)`.

### H10. One 4,300-line file

Central, scanning and peripheral code share one class, and `BluetoothDeviceAgent` lives in the header.

**Simpler:** after H1–H5, split the file into `scanner.cpp`, `connection.cpp` (records, connect, disconnect, autoConnect), `gatt_client.cpp` (operations, subscriptions, discovery) and `peripheral.cpp`. Splitting first would only move the complexity around.

### Not holes, for the record

These are complex because the problem is, and they should stay:

- `ScopedLease` itself, `callback_operations_` and `cleanup_operations_`;
- the non-pumping drain and the DLL pin;
- connect generations, which stay as one field of the per-address record;
- the scan generation and the check of which watcher sent each packet;
- the pairing ceremony split between Windows 10 and 11;
- the peripheral code. Its two advertising modes and the targets list are complex, but it is separate from the client side and has the least HIL coverage, so it stays out of scope here.

## 3. Target design (H1–H3)

**State per address:**

```cpp
struct Subscription {
  GattCharacteristic characteristic;  // keeps its service alive
  event_token token;
  bool busy = false;                  // a CCCD write is in flight
};
struct Connection {
  uint64_t generation;
  bool auto_connect;
  enum class State { kConnecting, kConnected, kWaiting } state;
  BluetoothLEDevice device{nullptr};
  GattSession session{nullptr};
  event_token status_token, services_token;
  std::map<std::pair<std::string, std::string>, Subscription> subscriptions;
};
std::unordered_map<uint64_t, std::shared_ptr<Connection>> connections_;
```

**connect():**

1. Create or replace the record with a new generation.
2. Wait for the cleanup gate.
3. Call `FromBluetoothAddressAsync`, then `GattSession::FromDeviceIdAsync`, and set `MaintainConnection(true)`, for plain connections too.
4. Register `ConnectionStatusChanged` and `GattServicesChanged`.
5. For a plain connect, run one `GetGattServicesAsync(Uncached)`. It opens the link, fills Windows' cache and reports an unreachable device as a connect failure. Its services are released, not closed.
6. Report connected when `ConnectionStatus` is `Connected`.

For autoConnect, wait on `ConnectionStatusChanged` instead of step 5.

**Read, write and descriptor operations:**

1. `RequireLive(device_id)`.
2. `co_await` a `Cached` lookup of the service and then the characteristic.
3. Check the characteristic's properties, run the operation and reply.

There is no lease on the map and no `Close`. `OnGattCompleted` stays as it is.

**setNotifiable:**

1. Look up the characteristic.
2. Under the record lock, set `busy` on its subscription, creating the entry if there is none.
3. Register `ValueChanged` before an enabling write.
4. Write the CCCD.
5. Keep the subscription, or remove it.

This replaces `Begin`/`Update`/`FinishNotificationOperation` and `gatt_revision`.

**GattServicesChanged:** for each subscription, look the characteristic up again with `Cached` and move the handler to the new object. A subscription whose characteristic is gone is dropped and logged. No map is rebuilt, and nothing waits for idle.

**discoverServices:**

1. On demand: `GetGattServicesAsync(Uncached)`, then each service's characteristics, and descriptors when asked.
2. Build the reply and release everything.

The HID `AccessDenied` handling moves here.

**Disconnect and link loss:**

- `disconnect()` takes the record and closes it on the worker: CCCD `None` for live subscriptions within the 2 s budget, then `ValueChanged` removal, `MaintainConnection(false)`, `session.Close()` and `device.Close()`.
- A link loss on a plain connection takes and closes the record the same way.
- A link loss on an autoConnect record keeps it: subscriptions are cleared, the state becomes `kWaiting` and the disconnect is reported. `ConnectionStatusChanged` → `Connected` reports the reconnect.

**Removed:**

- from `BluetoothDeviceAgent`: `gatt_map` and every locked method on it;
- `GattDiscovery`, `GattMapLease` and `GattCharacteristicLease`;
- `DiscoverCharacteristicsAsync`;
- the discovery passes and the 2-attempt retry when connect hits `Unreachable`;
- `CloseSupersededServicesAsync`, `RefreshGattServicesAsync`, `RetireGattMap`, `DisposeGattMap` and `RemoveValueChangedHandlers`;
- `RetryAutoConnectAsync`, `only_if_generation` and `pending_connects_`;
- `EndConnection`, `RemoveConnectedDevice`, `CleanConnection` and `DisposePartialConnection`, folded into `Take` and `Close`.

Rough estimate: 700–900 fewer lines across the `.cpp` and the header. `ConnectAsync` drops to about 120 lines.

## 4. Decisions needed

| # | Decision | Recommendation |
| --- | --- | --- |
| D1 | Do GATT operations work without `discoverServices()` first? Android and Apple require it; Windows works without it today. | **Keep it working.** It costs nothing with per-operation lookups. |
| D2 | Errors in looking up services and characteristics move from `connect()` to the first operation or `discoverServices()`. Connect still fails when the device is unreachable. | **Accept.** This matches Android and Apple, where `connect()` does not discover. |
| D3 | A service UUID that appears more than once: per-operation lookup uses the first match. Today the map keeps the last one, because `insert_or_assign` overwrites. | **First match in handle order.** It is deterministic and documented by WinRT. |
| D4 | Minimum Windows version: the per-UUID lookups need Windows 10 1703. | **1703+.** It already needs `UniversalApiContract` features beyond that for extended advertisements, which are guarded. |
| D5 | Scan reporting: report every merged packet, as Android does, or keep reporting only on change. | **Every packet.** That removes the change detection and gives RSSI updates; Dart deduplicates when the app asks. It sends more traffic to Flutter. |
| D6 | The scan name lookup (H6). | **Remove it.** Unnamed devices then have no name unless paired; restore the breaking-change line in the CHANGELOG. |

## 5. Risks to check on hardware

- **Disconnect actually drops the link.** No service objects are closed any more. The probe kept the link up for at least 10 s after releasing every service while the device object was still held. `MaintainConnection(false)` plus closing the session and device should drop it; WIN-LIFE and close timing will show.
- **The cleanup gate is still needed** if old service objects, released but not closed, can deny a reconnect's lookups. Keep the gate until FIT-CONN-009 and the lifecycle suite pass without it.
- **Subscriptions across a service change.** The firmware's auxiliary service (Service Changed indication) exercises this. A subscription on the main service must keep notifying after the auxiliary service is added or removed.
- **The first operation after connect pays the cold lookup.** That is 1.5–2 s if Windows' cache is empty. Step 5 of connect, the `Uncached` service query, fills the cache, so this should not happen after a plain connect.
- **autoConnect reconnecting with a kept session.** Windows reconnects by itself; the record must report `Connected` from `ConnectionStatusChanged` and not double-report.

## 6. Order of work

After each step: build the example app, run the native tests, then run HIL baseline, fault injection, lifecycle, bonding and close timing. Commit only with green suites, or with flakes already known.

1. **H6:** remove the scan name lookup and fix the CHANGELOG. Small and independent.
2. **H4:** add the `Unleased` and `Guarded` helpers and convert the existing coroutines. This changes no behaviour, and the later steps get shorter.
3. **H1, H2:** introduce the `Connection` record and per-operation lookups, rewrite `setNotifiable` and `discoverServices`, and delete the map machinery. This is the main step. New HIL tests:
   - read, write, subscribe and descriptors without `discoverServices`;
   - a subscription across an auxiliary service change;
   - operations after a reconnect;
   - concurrent reads on two characteristics of the same service, which guards the no-`Close` rule.
4. **H3:** keep the autoConnect session across a link loss, and extend WIN-LIFE for drop and reconnect without a new connect.
5. **H5:** one `ScanRecord` and one merge function, with reporting per D5. Update the scan tests in `windows/test`.
6. **H7–H9:** radio helper, initialization coroutine, exception helpers, UUID helpers, one UI-thread post helper.
7. **H10:** split the file.
8. Update `WINDOWS_REVIEW.md`, the PR draft and the CHANGELOG.
