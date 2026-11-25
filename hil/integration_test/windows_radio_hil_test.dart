import 'package:flutter/foundation.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:integration_test/integration_test.dart';
import 'package:universal_ble/universal_ble.dart';

/// Turning the radio off disconnects every Bluetooth device of the host,
/// keyboards and headsets included, so these tests run only on request:
///
///     flutter test integration_test/windows_radio_hil_test.dart -d windows `
///       --dart-define=HIL_ALLOW_RADIO_OFF=true
const _allowRadioOff = bool.fromEnvironment('HIL_ALLOW_RADIO_OFF');

/// Windows radio state: what central availability and peripheral readiness
/// report while the radio is off. Needs no fixture.
void main() {
  IntegrationTestWidgetsFlutterBinding.ensureInitialized();

  group('[windows-radio]', () {
    tearDown(() async {
      // Leave the radio on whatever happened.
      await UniversalBle.enableBluetooth();
      await _waitUntil(() async =>
          await UniversalBle.getBluetoothAvailabilityState() ==
          AvailabilityState.poweredOn);
    });

    testWidgets(
      '[WIN-RADIO-001] reports the radio off and back on, as a central and '
      'as a peripheral',
      (_) async {
        expect(await UniversalBlePeripheral.getAvailabilityState(),
            PeripheralReadinessState.ready);

        expect(await UniversalBle.disableBluetooth(), isTrue);
        await _waitUntil(() async =>
            await UniversalBle.getBluetoothAvailabilityState() ==
            AvailabilityState.poweredOff);
        expect(await UniversalBlePeripheral.getAvailabilityState(),
            PeripheralReadinessState.bluetoothOff);

        expect(await UniversalBle.enableBluetooth(), isTrue);
        await _waitUntil(() async =>
            await UniversalBle.getBluetoothAvailabilityState() ==
            AvailabilityState.poweredOn);
        expect(await UniversalBlePeripheral.getAvailabilityState(),
            PeripheralReadinessState.ready);
      },
      skip: defaultTargetPlatform != TargetPlatform.windows || !_allowRadioOff,
    );
  });
}

Future<void> _waitUntil(
  Future<bool> Function() condition, {
  Duration timeout = const Duration(seconds: 15),
}) async {
  final deadline = DateTime.now().add(timeout);
  while (DateTime.now().isBefore(deadline)) {
    if (await condition()) return;
    await Future<void>.delayed(const Duration(milliseconds: 100));
  }
  fail('Condition was not satisfied within $timeout');
}
