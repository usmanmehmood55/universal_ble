import 'dart:async';
import 'dart:convert';

import 'package:flutter/foundation.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:integration_test/integration_test.dart';
import 'package:universal_ble/universal_ble.dart';
import 'package:universal_ble_hil/src/hil_peripheral.dart';

/// Fixture commands added for these tests (contract revision 2, see
/// `docs/protocol.md` in the firmware repository).
abstract final class _Command {
  static const setHidService = 0x17;
  static const pauseAdvertising = 0x18;
}

const _hidService = '00001812-0000-1000-8000-00805f9b34fb';

/// Windows connection lifecycle: restricted services, autoConnect and
/// disconnecting with live subscriptions.
void main() {
  IntegrationTestWidgetsFlutterBinding.ensureInitialized();

  group('[windows-lifecycle]', () {
    late HilPeripheral peripheral;
    var setupSucceeded = false;

    setUp(() async {
      setupSucceeded = false;
      await UniversalBle.requestPermissions();
      peripheral = await HilPeripheral.open();
      setupSucceeded = true;
    });

    tearDown(() async {
      if (!setupSucceeded) return;
      setupSucceeded = false;
      // Fixture commands go over the connection. The reset ends an
      // advertising pause and removes the HID service.
      if (await UniversalBle.getConnectionState(peripheral.deviceId) !=
          BleConnectionState.connected) {
        await peripheral.reconnect();
      }
      await peripheral.reset();
      await peripheral.close();
    });

    testWidgets(
      '[WIN-LIFE-001] connects to a device exposing a service Windows reserves',
      (_) async {
        await peripheral.command(_Command.setHidService, [1]);
        // The service is added while connected; reconnect so the host sees it
        // during the initial uncached discovery.
        await UniversalBle.disconnect(peripheral.deviceId);
        await _waitDisconnected(peripheral);
        await peripheral.reconnect();

        final services = await peripheral.discover(withDescriptors: false);
        final hid = services.where(
          (service) => BleUuidParser.compareStrings(service.uuid, _hidService),
        );
        expect(hid, hasLength(1), reason: 'HID service listed');
        expect(hid.single.characteristics, isEmpty,
            reason: 'Windows keeps HID characteristics from apps');
        expect(utf8.decode(await peripheral.read(HilUuid.read)),
            'HIL-READ-V1');
      },
      skip: defaultTargetPlatform != TargetPlatform.windows,
    );

    testWidgets(
      '[WIN-LIFE-002] autoConnect waits for a device that is out of range',
      (_) async {
        // A host disconnect leaves Windows holding the link for a while, so
        // the fixture drops it: advertising is paused first and stays off
        // after the disconnect.
        const pause = Duration(seconds: 6);
        final paused = Stopwatch()..start();
        await peripheral.command(
            _Command.pauseAdvertising, _uint16(pause.inMilliseconds));
        await peripheral.requestDisconnect(_dropDelay);
        await _waitDisconnected(peripheral);

        final events = <bool>[];
        final subscription = peripheral.connections.listen(events.add);
        addTearDown(subscription.cancel);
        await UniversalBle.connect(
          peripheral.deviceId,
          autoConnect: true,
          timeout: const Duration(seconds: 30),
        );

        expect(paused.elapsed, greaterThanOrEqualTo(pause),
            reason: 'connected only after advertising resumed');
        expect(events, isNot(contains(false)),
            reason: 'no failure while the device was away');
        expect(utf8.decode(await peripheral.read(HilUuid.read)),
            'HIL-READ-V1');
      },
      timeout: const Timeout(Duration(minutes: 1)),
      skip: defaultTargetPlatform != TargetPlatform.windows,
    );

    testWidgets(
      '[WIN-LIFE-003] autoConnect re-establishes a lost link',
      (_) async {
        await UniversalBle.disconnect(peripheral.deviceId);
        await _waitDisconnected(peripheral);
        await UniversalBle.connect(
          peripheral.deviceId,
          autoConnect: true,
          timeout: const Duration(seconds: 30),
        );

        final events = <bool>[];
        final subscription = peripheral.connections.listen(events.add);
        addTearDown(subscription.cancel);
        await peripheral.requestDisconnect(_dropDelay);
        await _waitUntil(() async => events.length >= 2,
            timeout: const Duration(seconds: 30));

        expect(events.take(2), [false, true],
            reason: 'disconnected, then connected without a new connect()');
        await peripheral.discover(withDescriptors: false);
        expect(utf8.decode(await peripheral.read(HilUuid.read)),
            'HIL-READ-V1');
      },
      timeout: const Timeout(Duration(minutes: 1)),
      skip: defaultTargetPlatform != TargetPlatform.windows,
    );

    testWidgets(
      '[WIN-LIFE-004] disconnect() stops autoConnect',
      (_) async {
        await UniversalBle.disconnect(peripheral.deviceId);
        await _waitDisconnected(peripheral);
        await UniversalBle.connect(
          peripheral.deviceId,
          autoConnect: true,
          timeout: const Duration(seconds: 30),
        );
        await UniversalBle.disconnect(peripheral.deviceId);
        await _waitDisconnected(peripheral);

        final events = <bool>[];
        final subscription = peripheral.connections.listen(events.add);
        addTearDown(subscription.cancel);
        await Future<void>.delayed(const Duration(seconds: 8));

        expect(events, isNot(contains(true)),
            reason: 'no reconnection after disconnect()');
        expect(await UniversalBle.getConnectionState(peripheral.deviceId),
            BleConnectionState.disconnected);
      },
      timeout: const Timeout(Duration(minutes: 1)),
      skip: defaultTargetPlatform != TargetPlatform.windows,
    );

    testWidgets(
      '[WIN-LIFE-005] disconnects promptly with live subscriptions',
      (_) async {
        await peripheral.subscribe(HilUuid.notify);
        await peripheral.subscribe(HilUuid.indicate, indicate: true);

        final started = Stopwatch()..start();
        await UniversalBle.disconnect(peripheral.deviceId);
        await _waitDisconnected(peripheral);
        expect(started.elapsed, lessThan(const Duration(seconds: 3)));

        // The next connection must not be held up by the previous one's
        // unsubscribe and close.
        final reconnect = Stopwatch()..start();
        await peripheral.reconnect();
        expect(reconnect.elapsed, lessThan(const Duration(seconds: 15)));

        final values = <List<int>>[];
        final subscription =
            peripheral.values(HilUuid.notify).listen(values.add);
        addTearDown(subscription.cancel);
        await peripheral.subscribe(HilUuid.notify);
        await peripheral.requestNotification([7, 8, 9]);
        await _waitUntil(() async => values.isNotEmpty);
        expect(values.single, [7, 8, 9]);
      },
      timeout: const Timeout(Duration(minutes: 1)),
      skip: defaultTargetPlatform != TargetPlatform.windows,
    );

    testWidgets(
      '[WIN-LIFE-006] reconnects promptly after the peripheral drops a '
      'subscribed link',
      (_) async {
        await peripheral.subscribe(HilUuid.notify);
        await peripheral.requestDisconnect(_dropDelay);
        await _waitDisconnected(peripheral);

        // No CCC write is attempted on a link that is already gone, so the
        // reconnect is not delayed by a GATT timeout.
        final reconnect = Stopwatch()..start();
        await peripheral.reconnect();
        expect(reconnect.elapsed, lessThan(const Duration(seconds: 15)));
        expect(utf8.decode(await peripheral.read(HilUuid.read)),
            'HIL-READ-V1');
      },
      timeout: const Timeout(Duration(minutes: 1)),
      skip: defaultTargetPlatform != TargetPlatform.windows,
    );

    testWidgets(
      '[WIN-LIFE-007] disconnect() after a link loss stops auto-connect',
      (_) async {
        await UniversalBle.disconnect(peripheral.deviceId);
        await _waitDisconnected(peripheral);
        await UniversalBle.connect(peripheral.deviceId,
            autoConnect: true, timeout: const Duration(seconds: 30));

        // The link drops while the device stays away for a while, so the
        // plugin's reconnect attempt is pending when disconnect() comes.
        await peripheral.command(
            _Command.pauseAdvertising, _uint16(_away.inMilliseconds));
        await peripheral.requestDisconnect(_dropDelay);
        await _waitDisconnected(peripheral);
        await UniversalBle.disconnect(peripheral.deviceId);

        expect(await _connectsWithin(peripheral, _away + _grace), isFalse,
            reason: 'no reconnection after disconnect()');
      },
      timeout: const Timeout(Duration(minutes: 1)),
      skip: defaultTargetPlatform != TargetPlatform.windows,
    );

    testWidgets(
      '[WIN-LIFE-008] a plain connect() replaces a timed-out auto-connect',
      (_) async {
        await peripheral.command(
            _Command.pauseAdvertising, _uint16(_away.inMilliseconds));
        await peripheral.requestDisconnect(_dropDelay);
        await _waitDisconnected(peripheral);

        // Dart gives up; Windows keeps the auto-connect attempt pending.
        await expectLater(
            UniversalBle.connect(peripheral.deviceId,
                autoConnect: true, timeout: const Duration(seconds: 2)),
            throwsA(anything));
        await UniversalBle.connect(peripheral.deviceId,
            timeout: const Duration(seconds: 30));

        // The connection is a plain one: a link loss is not repaired.
        await peripheral.requestDisconnect(_dropDelay);
        await _waitDisconnected(peripheral);
        expect(await _connectsWithin(peripheral, _grace), isFalse,
            reason: 'plain connection does not auto-reconnect');
      },
      timeout: const Timeout(Duration(minutes: 1)),
      skip: defaultTargetPlatform != TargetPlatform.windows,
    );

    testWidgets(
      '[WIN-LIFE-009] disconnect() cancels a pending auto-connect',
      (_) async {
        await peripheral.command(
            _Command.pauseAdvertising, _uint16(_away.inMilliseconds));
        await peripheral.requestDisconnect(_dropDelay);
        await _waitDisconnected(peripheral);

        await expectLater(
            UniversalBle.connect(peripheral.deviceId,
                autoConnect: true, timeout: const Duration(seconds: 2)),
            throwsA(anything));
        await UniversalBle.disconnect(peripheral.deviceId);

        expect(await _connectsWithin(peripheral, _away + _grace), isFalse,
            reason: 'the cancelled attempt does not connect later');
      },
      timeout: const Timeout(Duration(minutes: 1)),
      skip: defaultTargetPlatform != TargetPlatform.windows,
    );
  });
}

/// How long the fixture stays away, and how long to watch after that.
const _away = Duration(seconds: 6);
const _grace = Duration(seconds: 6);

/// Whether a connected event arrives within [window].
Future<bool> _connectsWithin(HilPeripheral peripheral, Duration window) async {
  final connected = Completer<bool>();
  final subscription = peripheral.connections.listen((isConnected) {
    if (isConnected && !connected.isCompleted) connected.complete(true);
  });
  try {
    return await connected.future
        .timeout(window, onTimeout: () => false);
  } finally {
    await subscription.cancel();
  }
}

Future<void> _waitDisconnected(HilPeripheral peripheral) => _waitUntil(
      () async =>
          await UniversalBle.getConnectionState(peripheral.deviceId) ==
          BleConnectionState.disconnected,
    );

Future<void> _waitUntil(
  Future<bool> Function() condition, {
  Duration timeout = HilPeripheral.operationTimeout,
}) async {
  final deadline = DateTime.now().add(timeout);
  while (DateTime.now().isBefore(deadline)) {
    if (await condition()) return;
    await Future<void>.delayed(const Duration(milliseconds: 50));
  }
  fail('Condition was not satisfied within $timeout');
}

List<int> _uint16(int value) => [value & 0xff, value >> 8 & 0xff];

/// Fixture commands go over the link, so a requested disconnect leaves time
/// for the command's write response.
const _dropDelay = Duration(milliseconds: 100);
