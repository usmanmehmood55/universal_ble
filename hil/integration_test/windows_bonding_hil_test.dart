import 'dart:convert';
import 'dart:io';

import 'package:flutter/foundation.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:integration_test/integration_test.dart';
import 'package:universal_ble/universal_ble.dart';
import 'package:universal_ble_hil/src/hil_peripheral.dart';

/// Fixture command (contract revision 2, see `docs/protocol.md` in the
/// firmware repository): accept Just Works pairing, bond kept in RAM.
const _setBonding = 0x19;

/// Answers the Windows "Pair device?" prompt that pair() raises on Windows 11,
/// so the tests run unattended. Clicks Allow (automation id `ButtonOk`, not
/// language dependent) only when the prompt names [deviceName].
const _acceptPairingPrompt = r"""
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
$A = [System.Windows.Automation.AutomationElement]
$scope = [System.Windows.Automation.TreeScope]
$title = New-Object System.Windows.Automation.PropertyCondition($A::NameProperty, 'Pair Device')
$allow = New-Object System.Windows.Automation.PropertyCondition($A::AutomationIdProperty, 'ButtonOk')
$text = New-Object System.Windows.Automation.PropertyCondition($A::AutomationIdProperty, 'TextInstruction')
$doneName = New-Object System.Windows.Automation.PropertyCondition($A::AutomationIdProperty, 'TextPairingCompleteDeviceName')
$close = New-Object System.Windows.Automation.PropertyCondition($A::AutomationIdProperty, 'ButtonClose')
function Invoke($element) {
  $element.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()
}
while ($true) {
  try {
    foreach ($dialog in $A::RootElement.FindAll($scope::Children, $title)) {
      # The consent prompt: allow it.
      $instruction = $dialog.FindFirst($scope::Descendants, $text)
      $button = $dialog.FindFirst($scope::Descendants, $allow)
      if ($instruction -and $button -and $instruction.Current.Name -like "*$DeviceName*") {
        Invoke $button
        [Console]::Out.WriteLine('ACCEPTED')
      }
      # The result screen Windows leaves open afterwards: close it, or they
      # pile up and later pairing attempts fail.
      $name = $dialog.FindFirst($scope::Descendants, $doneName)
      $button = $dialog.FindFirst($scope::Descendants, $close)
      if ($name -and $button -and $name.Current.Name -eq $DeviceName) {
        Invoke $button
        [Console]::Out.WriteLine('CLOSED')
      }
      [Console]::Out.Flush()
    }
  } catch {}
  Start-Sleep -Milliseconds 200
}
""";

Future<Process> _startPromptWatcher() {
  final script =
      "\$DeviceName = '${HilPeripheral.deviceName}'\n$_acceptPairingPrompt";
  return Process.start('powershell.exe', [
    '-NoProfile',
    '-NonInteractive',
    '-EncodedCommand',
    base64Encode(_utf16le(script)),
  ]);
}

List<int> _utf16le(String text) => [
      for (final unit in text.codeUnits) ...[unit & 0xff, unit >> 8],
    ];

/// Windows pairing on a bondable fixture: pair, pairing state in scan results,
/// unpair, and the CCC clear on disconnect, which is only observable for a
/// bonded peer (the fixture keeps a bonded peer's CCC state across links).
///
/// The fixture is paired once for the whole file and unpaired by the last
/// test. Windows fails a second pairing of the same device after an unpair
/// in the same session (`DevicePairingResultStatus.Failed`, before any prompt
/// or contact with the device; the plugin before this branch behaves the
/// same), so the tests share one bond and run in order.
void main() {
  IntegrationTestWidgetsFlutterBinding.ensureInitialized();

  group('[windows-bonding]', () {
    late HilPeripheral peripheral;
    Process? promptWatcher;
    var paired = false;

    setUpAll(() async {
      promptWatcher = await _startPromptWatcher();
      promptWatcher!.stdout
          .transform(const SystemEncoding().decoder)
          .listen((line) => debugPrint('pairing prompt: ${line.trim()}'));
      await UniversalBle.requestPermissions();
      peripheral = await HilPeripheral.open();
      await peripheral.command(_setBonding, [1]);
      // Windows refuses to pair a device again for a while after it was
      // unpaired, also by an earlier run. The refusal comes at once, without
      // a prompt, so retrying is safe.
      for (var attempt = 1;; attempt++) {
        try {
          await UniversalBle.pair(peripheral.deviceId,
              timeout: const Duration(seconds: 30));
          break;
        } on PairingException catch (error) {
          if (attempt == 7) rethrow;
          debugPrint('pairing attempt $attempt refused: $error; retrying');
          await Future<void>.delayed(const Duration(seconds: 10));
        }
      }
      paired = true;
    });

    tearDownAll(() async {
      UniversalBle.queueType = QueueType.global;
      try {
        if (await UniversalBle.isPaired(peripheral.deviceId) == true) {
          await UniversalBle.unpair(peripheral.deviceId);
        }
      } catch (error) {
        debugPrint('bonding teardown unpair failed: $error');
      }
      // Deletes the fixture's bond and refuses pairing again. Fixture
      // commands go over the connection, and unpairing drops it a moment
      // later, so reconnect after the drop and retry if it comes late.
      for (var attempt = 1;; attempt++) {
        await Future<void>.delayed(const Duration(seconds: 2));
        try {
          if (await UniversalBle.getConnectionState(peripheral.deviceId) !=
              BleConnectionState.connected) {
            await peripheral.reconnect();
          }
          await peripheral.reset();
          break;
        } on UniversalBleException {
          if (attempt == 3) rethrow;
        }
      }
      await peripheral.close();
      promptWatcher?.kill();
    });

    // Each test starts connected, with the fixture's notify subscription off.
    setUp(() async {
      expect(paired, isTrue, reason: 'pairing in setUpAll failed');
      if (await UniversalBle.getConnectionState(peripheral.deviceId) !=
          BleConnectionState.connected) {
        await peripheral.reconnect();
      }
      await _waitUntil(() async => (await _securityLevel(peripheral)) >= 2);
      if ((await peripheral.readState()).notificationsEnabled) {
        await peripheral.unsubscribe(HilUuid.notify);
      }
    });

    testWidgets(
      '[WIN-BOND-001] pairing is reported and the link is encrypted',
      (_) async {
        expect(await UniversalBle.isPaired(peripheral.deviceId), isTrue);
        expect(await _fixtureBonds(peripheral), 1);
      },
      skip: defaultTargetPlatform != TargetPlatform.windows,
    );

    testWidgets(
      '[WIN-BOND-002] disconnect clears a bonded peer subscription',
      (_) async {
        await peripheral.subscribe(HilUuid.notify);
        expect((await peripheral.readState()).notificationsEnabled, isTrue);

        // The plugin writes the CCC back to "none" before closing. Without
        // that, the fixture restores the bonded peer's subscription when the
        // link comes back (WIN-BOND-003).
        await UniversalBle.disconnect(peripheral.deviceId);
        await _waitDisconnected(peripheral);
        await peripheral.reconnect();
        await _waitUntil(() async => (await _securityLevel(peripheral)) >= 2);
        expect((await peripheral.readState()).notificationsEnabled, isFalse,
            reason: 'subscription cleared on the peripheral');
      },
      timeout: const Timeout(Duration(minutes: 2)),
      skip: defaultTargetPlatform != TargetPlatform.windows,
    );

    testWidgets(
      '[WIN-BOND-003] a bonded subscription survives a link the host could '
      'not close (control for WIN-BOND-002)',
      (_) async {
        await peripheral.subscribe(HilUuid.notify);

        // The fixture drops the link, so the host gets no chance to write
        // the CCC first, and the fixture restores the subscription.
        await peripheral.requestDisconnect(_dropDelay);
        await _waitDisconnected(peripheral);
        await peripheral.reconnect();
        await _waitUntil(() async => (await _securityLevel(peripheral)) >= 2);
        expect((await peripheral.readState()).notificationsEnabled, isTrue,
            reason: 'fixture keeps a bonded peer subscription');
      },
      timeout: const Timeout(Duration(minutes: 2)),
      skip: defaultTargetPlatform != TargetPlatform.windows,
    );

    testWidgets(
      '[WIN-BOND-004] a running scan reports the pairing state and its '
      'change, and unpair does not block the platform thread',
      (_) async {
        // The fixture only advertises while unconnected.
        await peripheral.requestDisconnect(_dropDelay);
        await _waitDisconnected(peripheral);

        final id = UniversalBle.canonicalDeviceId(peripheral.deviceId);
        final states = <bool?>[];
        final subscription = UniversalBle.scanStream.listen((device) {
          if (device.deviceId == id) states.add(device.paired);
        });
        addTearDown(subscription.cancel);
        await UniversalBle.startScan(
          scanFilter: ScanFilter(withServices: const [HilUuid.service]),
        );
        try {
          await _waitUntil(() async => states.contains(true),
              timeout: HilPeripheral.scanTimeout);
          // Results reported before Windows listed the paired devices carry
          // no pairing state, and are reported again once it is known.
          debugPrint('pairing states before the first paired result: '
              '${states.takeWhile((paired) => paired != true).toList()}');

          // Another platform call made while unpairing answers promptly.
          UniversalBle.queueType = QueueType.none;
          final unpairing = UniversalBle.unpair(peripheral.deviceId);
          final probe = Stopwatch()..start();
          await UniversalBle.getBluetoothAvailabilityState();
          expect(probe.elapsed, lessThan(const Duration(milliseconds: 500)),
              reason: 'platform thread free while unpairing');
          await unpairing;
          UniversalBle.queueType = QueueType.global;

          // The scan, still running, reports the device unpaired.
          await _waitUntil(() async => states.isNotEmpty && states.last == false,
              timeout: HilPeripheral.scanTimeout);
        } finally {
          await UniversalBle.stopScan();
        }
        expect(await UniversalBle.isPaired(peripheral.deviceId), isFalse);
      },
      timeout: const Timeout(Duration(minutes: 2)),
      skip: defaultTargetPlatform != TargetPlatform.windows,
    );
  });
}

Future<Uint8List> _rawState(HilPeripheral peripheral) =>
    peripheral.read(HilUuid.state);

Future<int> _securityLevel(HilPeripheral peripheral) async =>
    (await _rawState(peripheral))[14];

Future<int> _fixtureBonds(HilPeripheral peripheral) async =>
    (await _rawState(peripheral))[15];

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
    await Future<void>.delayed(const Duration(milliseconds: 100));
  }
  fail('Condition was not satisfied within $timeout');
}

/// Fixture commands go over the link, so a requested disconnect leaves time
/// for the command's write response.
const _dropDelay = Duration(milliseconds: 100);
