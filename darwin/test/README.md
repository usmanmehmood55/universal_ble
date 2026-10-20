# Apple service discovery regression tests

On macOS with Xcode command line tools installed:

```sh
python3 darwin/test/run_service_discovery_tests.py
```

The runner compiles the production `UniversalBleAsyncServiceDiscovery.swift`
with minimal CoreBluetooth, Pigeon model, and logger doubles. Only platform
imports are removed from the production source. Callbacks are delivered
explicitly, so the tests can assert that discovery stays pending until each
individual service and characteristic completes.

Coverage includes duplicate service UUIDs with descriptors enabled and disabled,
cached and asynchronous discovery, reversed callback order, duplicate
characteristic UUIDs with different descriptor lists, and empty services.
The pull request workflow runs these tests on macOS.

These tests verify discovery bookkeeping. They do not replace a native plugin
build or hardware validation of CoreBluetooth behavior, and do not add public
API support for selecting a particular duplicate UUID instance.
