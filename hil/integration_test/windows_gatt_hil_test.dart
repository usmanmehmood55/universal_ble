import 'dart:convert';
import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:integration_test/integration_test.dart';
import 'package:universal_ble/universal_ble.dart';
import 'package:universal_ble_hil/src/hil_peripheral.dart';

/// Windows GATT client: every operation looks its characteristic up when it
/// runs, and only subscriptions are kept between operations.
void main() {
  IntegrationTestWidgetsFlutterBinding.ensureInitialized();

  group('[windows-gatt]', () {
    late HilPeripheral peripheral;
    var setupSucceeded = false;

    setUp(() async {
      setupSucceeded = false;
      await UniversalBle.requestPermissions();
      peripheral = await HilPeripheral.open();
      setupSucceeded = true;
    });

    tearDown(() async {
      UniversalBle.queueType = QueueType.global;
      if (!setupSucceeded) return;
      setupSucceeded = false;
      if (await UniversalBle.getConnectionState(peripheral.deviceId) !=
          BleConnectionState.connected) {
        await peripheral.reconnect();
      }
      await peripheral.reset();
      await peripheral.close();
    });

    testWidgets(
      '[WIN-GATT-001] reads, writes, subscribes and uses descriptors without '
      'discovering services',
      (_) async {
        await _reconnectWithoutDiscovery(peripheral);
        await _expectOperationsWork(peripheral);
      },
    );

    testWidgets(
      '[WIN-GATT-002] keeps a subscription notifying across service changes',
      (_) async {
        await peripheral.subscribe(HilUuid.notify);
        await _expectNotification(peripheral, [0x02, 0x01]);

        for (final enabled in [true, false]) {
          await peripheral.setAuxiliaryService(enabled: enabled);
          await _eventually(
            () async =>
                (await peripheral.discover()).any(
                  (service) => BleUuidParser.compareStrings(
                    service.uuid,
                    HilUuid.auxiliaryService,
                  ),
                ) ==
                enabled,
          );
          await _expectNotification(peripheral, [0x02, enabled ? 0x02 : 0x03]);
        }
        await peripheral.unsubscribe(HilUuid.notify);
      },
    );

    testWidgets(
      '[WIN-GATT-003] works after a reconnect without discovering again',
      (_) async {
        for (var cycle = 0; cycle < 3; cycle++) {
          await _reconnectWithoutDiscovery(peripheral);
          expect(
            utf8.decode(await peripheral.read(HilUuid.read)),
            'HIL-READ-V1',
          );
        }
        await _expectOperationsWork(peripheral);
      },
    );

    testWidgets(
      '[WIN-GATT-004] runs concurrent operations on one service while a '
      'subscription is active',
      (_) async {
        // Each operation looks up its own objects for the same service; none
        // may close the service the others, or the subscription, are using.
        UniversalBle.queueType = QueueType.none;
        await peripheral.subscribe(HilUuid.notify);
        for (var round = 0; round < 5; round++) {
          final value = [0x40 + round];
          await Future.wait([
            for (var i = 0; i < 4; i++)
              peripheral
                  .read(HilUuid.read)
                  .then((v) => expect(utf8.decode(v), 'HIL-READ-V1')),
            peripheral.write(HilUuid.write, value),
            peripheral.readState(),
            peripheral.readDescriptor().then(
                  (v) => expect(utf8.decode(v), 'HIL-DESCRIPTOR-V1'),
                ),
          ]);
          expect(
            await peripheral.read(HilUuid.writeMirror),
            orderedEquals(value),
          );
        }
        await _expectNotification(peripheral, [0x04, 0x01]);
        await peripheral.unsubscribe(HilUuid.notify);
      },
    );
  });
}

/// Disconnects and connects again through the plain API, which, unlike
/// [HilPeripheral.reconnect], does not discover services.
Future<void> _reconnectWithoutDiscovery(HilPeripheral peripheral) async {
  await UniversalBle.disconnect(peripheral.deviceId);
  await _eventually(
    () async =>
        await UniversalBle.getConnectionState(peripheral.deviceId) ==
        BleConnectionState.disconnected,
  );
  await UniversalBle.connect(
    peripheral.deviceId,
    timeout: HilPeripheral.operationTimeout,
  );
}

Future<void> _expectOperationsWork(HilPeripheral peripheral) async {
  expect(utf8.decode(await peripheral.read(HilUuid.read)), 'HIL-READ-V1');

  await peripheral.write(HilUuid.write, [0x5a, 0xa5]);
  expect(
    await peripheral.read(HilUuid.writeMirror),
    orderedEquals([0x5a, 0xa5]),
  );

  expect(utf8.decode(await peripheral.readDescriptor()), 'HIL-DESCRIPTOR-V1');
  final descriptor = Uint8List.fromList([0x01, 0x80]);
  await peripheral.writeDescriptor(descriptor);
  expect(await peripheral.readDescriptor(), orderedEquals(descriptor));

  await peripheral.subscribe(HilUuid.notify);
  await _expectNotification(peripheral, [0x01, 0x01]);
  await peripheral.unsubscribe(HilUuid.notify);
}

Future<void> _expectNotification(
  HilPeripheral peripheral,
  List<int> expected,
) async {
  final received = peripheral.values(HilUuid.notify).first;
  await peripheral.requestNotification(expected);
  expect(
    await received.timeout(HilPeripheral.operationTimeout),
    orderedEquals(expected),
  );
}

Future<void> _eventually(
  Future<bool> Function() predicate, {
  Duration timeout = HilPeripheral.operationTimeout,
}) async {
  final deadline = DateTime.now().add(timeout);
  Object? lastError;
  while (DateTime.now().isBefore(deadline)) {
    try {
      if (await predicate()) return;
    } catch (error) {
      lastError = error;
    }
    await Future<void>.delayed(const Duration(milliseconds: 100));
  }
  fail('Condition was not satisfied within $timeout'
      '${lastError == null ? '' : ': $lastError'}');
}
