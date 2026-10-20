import 'dart:convert';

import 'package:flutter/foundation.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:integration_test/integration_test.dart';
import 'package:universal_ble/universal_ble.dart';
import 'package:universal_ble_hil/src/hil_peripheral.dart';

void main() {
  IntegrationTestWidgetsFlutterBinding.ensureInitialized();

  group('Apple duplicate UUID HIL', () {
    HilPeripheral? fixture;

    setUp(() async {
      await UniversalBle.requestPermissions();
      fixture = await HilPeripheral.open(expectedContractRevision: 3);
      await fixture!.setDuplicateUuidService(enabled: false);
      await _waitForProfile(fixture!, enabled: false);
    });

    tearDown(() async {
      final peripheral = fixture;
      fixture = null;
      if (peripheral == null) return;
      try {
        await peripheral.setDuplicateUuidService(enabled: false);
        await _waitForProfile(peripheral, enabled: false);
      } finally {
        await peripheral.close();
      }
    });

    for (final withDescriptors in [false, true]) {
      testWidgets(
        'discovers distinct duplicate UUID instances withDescriptors=$withDescriptors',
        (_) async {
          final peripheral = fixture!;
          await peripheral.setDuplicateUuidService(enabled: true);
          // Wait without descriptors first, so the first descriptor discovery
          // below exercises fresh descriptor callbacks rather than cached data.
          await _waitForProfile(peripheral, enabled: true);
          _expectDuplicateProfile(
            await peripheral.discover(withDescriptors: withDescriptors),
            withDescriptors: withDescriptors,
          );
          // Repeat to exercise the cached CoreBluetooth object path too.
          _expectDuplicateProfile(
            await peripheral.discover(withDescriptors: withDescriptors),
            withDescriptors: withDescriptors,
          );
        },
      );
    }

    testWidgets(
      '[FIT-DISC-015] preserves duplicate UUID instances across profile changes',
      (_) async {
        final peripheral = fixture!;
        for (var cycle = 0; cycle < 3; cycle++) {
          await peripheral.setDuplicateUuidService(enabled: true);
          await _waitForProfile(peripheral, enabled: true);
          _expectDuplicateProfile(
            await peripheral.discover(),
            withDescriptors: true,
          );

          await peripheral.setDuplicateUuidService(enabled: false);
          await _waitForProfile(peripheral, enabled: false);
          final restored = _services(await peripheral.discover());
          expect(restored, hasLength(1));
          expect(restored.single.characteristics, hasLength(10));
          expect(
            utf8.decode(await peripheral.read(HilUuid.read)),
            'HIL-READ-V1',
          );
        }
      },
      timeout: const Timeout(Duration(minutes: 2)),
    );
  },
      skip: kIsWeb ||
          (defaultTargetPlatform != TargetPlatform.iOS &&
              defaultTargetPlatform != TargetPlatform.macOS));
}

List<BleService> _services(List<BleService> services) => services
    .where((service) =>
        BleUuidParser.compareStrings(service.uuid, HilUuid.service))
    .toList();

void _expectDuplicateProfile(
  List<BleService> services, {
  required bool withDescriptors,
}) {
  final duplicates = _services(services);
  expect(duplicates, hasLength(2));
  expect(
    duplicates.map((service) => service.characteristics.length),
    unorderedEquals([10, 2]),
  );
  final added = duplicates.singleWhere(
    (service) => service.characteristics.length == 2,
  );
  for (final characteristic in added.characteristics) {
    expect(BleUuidParser.compareStrings(characteristic.uuid, HilUuid.read),
        isTrue);
    expect(characteristic.properties,
        unorderedEquals([CharacteristicProperty.read]));
    if (withDescriptors) {
      expect(
        characteristic.descriptors.map((descriptor) => descriptor.uuid),
        unorderedEquals([BleUuidParser.string('2901')]),
      );
    }
  }
  if (!withDescriptors) {
    expect(
      duplicates
          .expand((service) => service.characteristics)
          .expand((characteristic) => characteristic.descriptors),
      isEmpty,
    );
  }
}

Future<void> _waitForProfile(
  HilPeripheral peripheral, {
  required bool enabled,
}) async {
  final deadline = DateTime.now().add(HilPeripheral.operationTimeout);
  do {
    // A failed discovery is deliberately not swallowed: timeout is one of the
    // regressions these tests must expose. Only Service Changed delivery polls.
    final services =
        _services(await peripheral.discover(withDescriptors: false));
    if (services.length == (enabled ? 2 : 1)) return;
    await Future<void>.delayed(const Duration(milliseconds: 100));
  } while (DateTime.now().isBefore(deadline));
  fail('Duplicate UUID profile did not become enabled=$enabled');
}
