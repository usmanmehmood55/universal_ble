# Review of `windows-audit-simplify` (a937b74)

An independent review of the squashed commit a937b74 ("Simplify the Windows BLE implementation") against its base, upstream 32fce88. It asks two questions:

1. Does it simplify the existing architecture or reduce complexity without losing functionality?
2. Does it follow the WinRT Bluetooth LE guidelines?

Line references are to `windows/src/universal_ble_plugin.cpp` at a937b74 unless another file is named.

## Verification

- `cmake -S windows/test` and `ctest`: 12/12 native tests pass. These tests do not compile the plugin itself.
- `flutter build windows --debug` in `example/` compiles the plugin cleanly.
- No HIL run was made for this review.

## Summary

The branch adds three features: `autoConnect`, scan responses merged into the first result, and per-scan pairing state. It still ends up smaller and clearer in the scan and reply code.

The connection lifecycle is not simpler. It has more locks and polling loops than before.

The code mostly follows Microsoft's WinRT guidance. There is one deviation, in `autoConnect`, and one new problem, in the scan name lookup.

## 1. Simplification

### Simpler

- **DeviceWatcher removed.** The `DeviceWatcher` that ran beside the advertisement watcher is gone. So are its 4 `ThreadSafeMap`s, its 6 event tokens and `universal_ble_thread_safe.h`. A paired-device lookup made once per scan replaces it.
- **Scan state.** All scan state lives under one mutex (`scan_mutex_`) with a generation counter and a check of which watcher sent each packet. Nothing from an earlier scan or a stopped watcher reaches the current one. The scan filter is per scan, where it used to be global state.
- **GATT replies.** One helper, `OnGattCompleted` (line 145), handles the replies for read, write and both descriptor operations. Each used to have about 7 hand-written `safe_reply` branches.
- **Pairing.** `CustomPairAsync` is merged into `PairAsync`, and pairing always replies exactly once.
- **Cleanup tracking.** A map of per-address `AsyncOperationTracker`s became `AddressCleanupGate` (`address_cleanup_gate.h`, 58 lines). The separate initialization tracker is gone.
- **Size.** `windows/src` went from 14,540 to 14,328 lines, about 210 fewer. `universal_ble_plugin.cpp` lost 298 lines, and the header gained 46. The PR draft's "about 330 lines shorter" is out of date.

### Not simpler

The connection lifecycle is coordinated by about nine separate mechanisms:

- connect generations (`connect_generations_`, `next_connect_generation_`);
- the pending-connects map, which now stores the auto-connect setting;
- `AddressCleanupGate` reservations, held by attempts, closes and refreshes;
- the `BluetoothDeviceAgent::active` flag;
- the `gatt_operations` tracker;
- `gatt_revision`;
- `notification_operation_in_progress` on each characteristic;
- the GATT refresh requested and in-progress flags;
- the superseded-services list and up to 3 discovery passes during connect.

| Construct in the plugin | Base | Branch |
| --- | --- | --- |
| `lock_guard` sites | 26 | 42 |
| `co_await` | 30 | 41 |
| `resume_after` (polling or delays) | 4 | 7 |

Almost all of this exists to protect the eager GATT map, the map of every service and characteristic built at connect time. A design that looks up services and characteristics per operation would remove most of it: `gatt_revision`, `SnapshotSubscriptions`, `ReplaceGattMapIfUnchanged`, the refresh loop, superseded services and the discovery passes.

`universal_ble_plugin.cpp` is still about 4,300 lines, holding central, scanning and peripheral code in one class. The header holds about 260 lines of locking logic inline in `BluetoothDeviceAgent`.

### Functionality lost

- **Pairing changes during a scan.** Pairing state is read once per scan. If a device is paired or unpaired from outside the app during a scan, the results don't show it; the old `DeviceWatcher` did. Pairing done through the plugin still sends its own event.
- Nothing else was found. The remembered name for an unnamed device is kept through a per-address lookup; its cost is under 2.1 below.

## 2. WinRT guideline compliance

### Follows the guidance

- Connection WinRT objects are created and closed on the thread pool, not on Flutter's UI thread, which is a single-threaded COM apartment (STA).
- The advertisement watcher:
  - uses `Active` scanning;
  - handles `Stopped` when it carries an error;
  - drops packets from a stopped watcher, as Microsoft's discovery sample does.
- `IsConnectable` is read through `try_as<IBluetoothLEAdvertisementReceivedEventArgs2>`. A missing interface reports connectability as unknown instead of crashing (#373).
- `disconnect()` turns notifications off before closing (CCCD write `None`), as the GATT client sample does.
- After `GattServicesChanged`, services are queried with `BluetoothCacheMode::Cached`.
- Reads use `Uncached`, and writes use `WriteValueWithResultAsync`, which reports the protocol error.
- The `ValueChanged` handler is registered before the CCCD write that enables notifications, so no early notification is lost.
- Closing order is services, then `GattSession` (`MaintainConnection(false)`), then `BluetoothLEDevice`.
- HID over GATT, which Windows reserves for itself, gets `AccessDenied` and is listed without characteristics instead of failing the connect.

### Issues

#### 2.1 The scan name lookup starts a 30-second inquiry per unnamed device

`LoadScanNameAsync` (line 1448) runs `DeviceInformation::FindAllAsync` with the selector from `BluetoothLEDevice::GetDeviceSelectorFromBluetoothAddress`, with `AssociationEndpoint` as the kind. That selector includes `System.Devices.Aep.Bluetooth.IssueInquiry:=True`:

```
System.Devices.DevObjectType:=5 AND System.Devices.Aep.ProtocolId:="{BB7BB05E-...}" AND
(System.DeviceInterface.Bluetooth.DeviceAddress:="112233445566" OR
 System.Devices.Aep.Bluetooth.IssueInquiry:=System.StructuredQueryType.Boolean#True)
```

- The same lookup run for an address that isn't present took 30.1 s and returned 0 results. The address filter does work; it returned no wrong device.
- Even when the device is found, the name probably arrives about 30 s later, because the lookup completes only when the inquiry ends.
- In a busy room, every unnamed advertiser starts its own inquiry, possibly hundreds at once. `scan_name_lookups_` removes duplicates only per address and per scan. All of them use the same radio the scan uses.
- The CHANGELOG says "without enumerating all unpaired devices". That is true, but it hides this cost. The PR draft says unnamed unpaired devices lose the remembered name, which no longer matches the code.

**Fix:** drop the lookup, or build the selector with `IssueInquiry:=False` so it reads only what Windows has already cached.

#### 2.2 autoConnect tears down and rebuilds the connection after a link loss

On `Disconnected`, `BluetoothLeDeviceConnectionStatusChanged` (line 2404) disposes the agent. That turns `MaintainConnection` off and closes the session and the device. A new attempt then starts through `StartConnect`, `RetryAutoConnectAsync` and the `only_if_generation` check.

With `GattSession.MaintainConnection`, Windows reconnects on its own as long as the session is kept. Rebuilding instead:

- runs a full `Uncached` discovery on every reconnect;
- leaves gaps where no session asks Windows to keep the link;
- needs extra machinery: `RetryAutoConnectAsync`, `only_if_generation` and the auto-connect value in `pending_connects_`.

The wait for the link polls `ConnectionStatus()` every 250 ms (line 1864) instead of waiting for `ConnectionStatusChanged`. That handler is registered only later, at line 2000.

#### 2.3 Blocking waits on thread-pool threads

- Closing superseded services during connect uses `close.wait_for(kConnectionCleanupWait)`, which blocks for up to 10 s (line 2026). It could be an await with a timeout.
- The unsubscribe step in `CloseConnectionAsync` uses `wait_for`, sharing a 2 s budget across all characteristics (line 2705).

Neither wait runs on the UI thread, so the impact is limited.

#### 2.4 Polling instead of signalling

| Where | Interval |
| --- | --- |
| Cleanup gate wait in `ConnectAsync` (line 1793) | 10 ms |
| Subscription snapshot in `RefreshGattServicesAsync` (line 2248) | 10 ms |
| GATT operations idle wait in `RefreshGattServicesAsync` (line 2266) | 10 ms |
| autoConnect link wait (line 1871) | 250 ms |

A steady stream of reads or writes keeps `gatt_operations` busy. The refresh then waits indefinitely at line 2260, and the device keeps the old GATT map after a service change.

## 3. Smaller findings

- **Two retirement paths.** Retired GATT maps are closed in two ways. `RetireGattMap` hands them to the cleanup worker through `CloseConnection`, while the refresh loop calls `DisposeGattMap` itself after `resume_background` (line 2373). The comment at line 2216 describes only the first.
- **Dead branch in `StartScan`.** `if (!bluetooth_le_watcher_)` (line 494) is always true, because `DisposeAdvertisementWatcher()` has just cleared the watcher.
- **Unguarded replies in `DiscoverServicesAsync`.** It replies with `result(...)` directly (lines 2908, 2969, 2973 and 2976) instead of `safe_reply`, unlike every other path. If the reply throws inside the `try` and again in the `catch`, the exception escapes the `fire_and_forget` coroutine and terminates the app.
- **GATT replies without the plugin lease.** Replies from `OnGattCompleted` and `RequestMtu` come from WinRT completion threads without a `callback_operations_` lease. Comments elsewhere say a reply after shutdown needs the lease, because the channel is gone. The base commit did the same, so this is not a regression.
- **Duplicate structs.** `ScanFieldPresence` and `ScanBackfillPlan` in `scan_result_merge.h` have identical fields.
- **Session reuse in `RequestMtu`.** It creates a new `GattSession` on every call, even when the agent already holds one (autoConnect).
- **Stale PR draft.** It misstates the line savings (about 330 against about 210 measured). It also describes the unnamed-device name as removed, which no longer matches the code.

## TODO

Status after the simplification commits (fb1fdfb..56f1d20); see `WINDOWS_SIMPLIFICATION_PLAN.md`.


- [x] Remove the scan name lookup, or use a selector with `IssueInquiry:=False` (2.1).
- [x] Update the CHANGELOG and the PR draft to match the name behaviour and the real line count (2.1, section 3).
- [x] autoConnect: keep the `GattSession` with `MaintainConnection` across a link loss instead of rebuilding the connection (2.2).
- [x] autoConnect: wait for the link on `ConnectionStatusChanged` instead of polling every 250 ms (2.2).
- [x] Replace the 10 s `wait_for` on superseded services with an await that has a timeout (2.3).
- [x] Replace the 10 ms polling loops with signals (2.4). The cleanup gate now sets an event when an address's last close finishes, and `ConnectAsync` awaits it.
- [x] Stop a steady stream of reads or writes from holding off a GATT refresh indefinitely (2.4).
- [x] Close retired GATT maps through one path, and fix the comment at line 2216 (section 3).
- [x] Remove the always-true watcher check in `StartScan` (section 3).
- [x] Use `safe_reply` in `DiscoverServicesAsync` (section 3).
- [x] Take the plugin lease before replying in `OnGattCompleted` and `RequestMtu`, or document why it isn't needed (section 3).
- [x] Merge `ScanFieldPresence` and `ScanBackfillPlan` (section 3).
- [x] Reuse the agent's `GattSession` in `RequestMtu` when one exists (section 3).
- [x] Decide whether pairing changes made during a scan must be reported, and restore that if so (section 1). A paired-device watcher runs for each scan.
- [x] Reduce the connection lifecycle state with per-operation GATT lookups, and split `universal_ble_plugin.cpp` (section 1).
