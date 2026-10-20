#!/usr/bin/env python3
"""Run the production discovery state machine with deterministic CoreBluetooth doubles.

Requires macOS and the Xcode command line tools; no Flutter build or BLE hardware.
Only platform imports are removed; the discovery implementation is unchanged.
"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parent
source = (root.parent / 'universal_ble/Sources/universal_ble/UniversalBleAsyncServiceDiscovery.swift').read_text()
source = source.replace('import CoreBluetooth\n', '')
source = source.replace('#if os(iOS)\n    import Flutter\n#elseif os(OSX)\n    import FlutterMacOS\n#endif', '')
with tempfile.TemporaryDirectory(prefix='ble-discovery-tests-') as directory:
    test = Path(directory) / 'main.swift'
    test.write_text((root / 'ServiceDiscoveryDoubles.swift').read_text() + '\n' + source + '\n' + (root / 'ServiceDiscoveryTests.swift').read_text())
    binary = Path(directory) / 'tests'
    subprocess.run(['xcrun', 'swiftc', str(test), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
