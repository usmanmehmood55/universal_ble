import 'dart:async';
import 'dart:io';

import 'package:flutter/widgets.dart';
import 'package:universal_ble/universal_ble.dart';
import 'package:universal_ble_hil/src/hil_peripheral.dart';

/// Puts the plugin into one state with a WinRT operation pending, then writes
/// a ready file. `close_hil_test.py` then closes the window and times the
/// process exit, which must not wait for the pending operation.
///
/// Build with `-t lib/close_scenario.dart` and:
/// - `--dart-define=HIL_CLOSE_SCENARIO=connect|connected|subscribe`
/// - `--dart-define=HIL_READY_FILE=<path>`
const _scenario = String.fromEnvironment('HIL_CLOSE_SCENARIO');
const _readyFile = String.fromEnvironment('HIL_READY_FILE');

const _setCccDelay = 0x0d;
const _pauseAdvertising = 0x18;

/// How long the fixture keeps the operation pending: far longer than the
/// process may take to exit.
const _pendingFor = Duration(seconds: 15);

Future<void> main() async {
  WidgetsFlutterBinding.ensureInitialized();
  runApp(const Center(
    child: Text('HIL close scenario: $_scenario',
        textDirection: TextDirection.ltr),
  ));
  try {
    // The plugin finds the radio asynchronously; a scan started before that
    // reports Bluetooth as unavailable.
    final deadline = DateTime.now().add(HilPeripheral.operationTimeout);
    while (await UniversalBle.getBluetoothAvailabilityState() !=
        AvailabilityState.poweredOn) {
      if (DateTime.now().isAfter(deadline)) {
        throw TimeoutException('Bluetooth did not become available');
      }
      await Future<void>.delayed(const Duration(milliseconds: 100));
    }
    final peripheral = await HilPeripheral.open();
    final pending = _uint16(_pendingFor.inMilliseconds);
    switch (_scenario) {
      case 'connect':
        // The device goes out of range, then a connect waits for it.
        await peripheral.command(_pauseAdvertising, pending);
        await peripheral.requestDisconnect(_dropDelay);
        // Polled: the disconnect event can arrive before a listener would.
        final deadline = DateTime.now().add(HilPeripheral.operationTimeout);
        while (await UniversalBle.getConnectionState(peripheral.deviceId) !=
            BleConnectionState.disconnected) {
          if (DateTime.now().isAfter(deadline)) {
            throw TimeoutException('fixture did not disconnect');
          }
          await Future<void>.delayed(const Duration(milliseconds: 100));
        }
        unawaited(UniversalBle.connect(peripheral.deviceId,
                autoConnect: true, timeout: const Duration(minutes: 1))
            .catchError((Object _) {}));
      case 'connected':
        break;
      case 'subscribe':
        // The fixture holds the CCC write, so the subscribe stays pending.
        await peripheral.command(_setCccDelay, pending);
        unawaited(peripheral.subscribe(HilUuid.notify).catchError((Object _) {}));
      default:
        throw ArgumentError.value(_scenario, 'HIL_CLOSE_SCENARIO');
    }
    // Let the operation reach WinRT before signalling.
    await Future<void>.delayed(const Duration(milliseconds: 500));
    File(_readyFile).writeAsStringSync('ready');
  } catch (error) {
    File(_readyFile).writeAsStringSync('error: $error');
  }
}

List<int> _uint16(int value) => [value & 0xff, value >> 8 & 0xff];

/// Fixture commands go over the link, so a requested disconnect leaves time
/// for the command's write response.
const _dropDelay = Duration(milliseconds: 100);
