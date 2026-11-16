# WinRT Bluetooth LE references

Official Microsoft references for the Universal BLE Windows plugin, which uses C++/WinRT from a Flutter desktop application.

## Retrieving sources

- For Microsoft Learn pages, open the link or append `?accept=text/markdown` for the Markdown version. In PowerShell: `(Invoke-WebRequest -Uri "${url}?accept=text/markdown").Content`.
- For Microsoft samples, use the raw file links below to read source without the GitHub UI. These track Microsoft's default branch; if a finding depends on the sample implementation, cite a commit permalink.
- Start with the GATT client guide for the documented behavior, then read the relevant API member and sample. The samples are UWP apps, not Flutter desktop runners. Check threading, packaging, and object ownership separately before applying sample code here. Use HIL evidence for behavior the docs do not guarantee.

## Microsoft Learn guides

- [Bluetooth Low Energy in Windows apps](https://learn.microsoft.com/en-us/windows/apps/develop/devices-sensors/bluetooth-low-energy-overview): GAP, GATT, and BLE roles.
- [Bluetooth LE advertisements](https://learn.microsoft.com/en-us/windows/apps/develop/devices-sensors/ble-beacon): watching, filtering, and publishing advertisements.
- [Bluetooth GATT client](https://learn.microsoft.com/en-us/windows/apps/develop/devices-sensors/gatt-client): discovery, connection behavior, reads, writes, and subscriptions.
- [Bluetooth GATT server](https://learn.microsoft.com/en-us/windows/apps/develop/devices-sensors/gatt-server): local services, read/write requests, and notifications.
- [Bluetooth developer FAQ](https://learn.microsoft.com/en-us/windows/uwp/devices-sensors/bluetooth-dev-faq): Windows Bluetooth behavior, including pairing.

## Microsoft samples and raw C++/WinRT source

- Bluetooth LE sample: [raw README](https://raw.githubusercontent.com/microsoft/Windows-universal-samples/main/Samples/BluetoothLE/README.md), [discovery](https://raw.githubusercontent.com/microsoft/Windows-universal-samples/main/Samples/BluetoothLE/cppwinrt/Scenario1_Discovery.cpp), [GATT client](https://raw.githubusercontent.com/microsoft/Windows-universal-samples/main/Samples/BluetoothLE/cppwinrt/Scenario2_Client.cpp), [foreground GATT server](https://raw.githubusercontent.com/microsoft/Windows-universal-samples/main/Samples/BluetoothLE/cppwinrt/Scenario3_ServerForeground.cpp).
- Bluetooth Advertisement sample: [raw README](https://raw.githubusercontent.com/microsoft/Windows-universal-samples/main/Samples/BluetoothAdvertisement/README.md), [watcher](https://raw.githubusercontent.com/microsoft/Windows-universal-samples/main/Samples/BluetoothAdvertisement/cppwinrt/Scenario1_Watcher.cpp), [publisher](https://raw.githubusercontent.com/microsoft/Windows-universal-samples/main/Samples/BluetoothAdvertisement/cppwinrt/Scenario2_Publisher.cpp).

## WinRT API reference

Namespaces: [Bluetooth](https://learn.microsoft.com/en-us/uwp/api/windows.devices.bluetooth), [Advertisement](https://learn.microsoft.com/en-us/uwp/api/windows.devices.bluetooth.advertisement), [GenericAttributeProfile](https://learn.microsoft.com/en-us/uwp/api/windows.devices.bluetooth.genericattributeprofile).

| Topic | Start here |
| --- | --- |
| Scan and publish | [Advertisement watcher](https://learn.microsoft.com/en-us/uwp/api/windows.devices.bluetooth.advertisement.bluetoothleadvertisementwatcher), [advertisement publisher](https://learn.microsoft.com/en-us/uwp/api/windows.devices.bluetooth.advertisement.bluetoothleadvertisementpublisher) |
| Device acquisition vs. establishing a link | [FromBluetoothAddressAsync](https://learn.microsoft.com/en-us/uwp/api/windows.devices.bluetooth.bluetoothledevice.frombluetoothaddressasync), [BluetoothLEDevice.Close](https://learn.microsoft.com/en-us/uwp/api/windows.devices.bluetooth.bluetoothledevice.close) |
| Service discovery, caching, and invalidation | [GetGattServicesAsync](https://learn.microsoft.com/en-us/uwp/api/windows.devices.bluetooth.bluetoothledevice.getgattservicesasync), [BluetoothCacheMode](https://learn.microsoft.com/en-us/uwp/api/windows.devices.bluetooth.bluetoothcachemode), [GattServicesChanged](https://learn.microsoft.com/en-us/uwp/api/windows.devices.bluetooth.bluetoothledevice.gattserviceschanged) |
| GATT reads, writes, and descriptors | [GattCharacteristic](https://learn.microsoft.com/en-us/uwp/api/windows.devices.bluetooth.genericattributeprofile.gattcharacteristic), [GattDescriptor](https://learn.microsoft.com/en-us/uwp/api/windows.devices.bluetooth.genericattributeprofile.gattdescriptor), [GattCommunicationStatus](https://learn.microsoft.com/en-us/uwp/api/windows.devices.bluetooth.genericattributeprofile.gattcommunicationstatus) |
| Notifications and indications | [CCCD write](https://learn.microsoft.com/en-us/uwp/api/windows.devices.bluetooth.genericattributeprofile.gattcharacteristic.writeclientcharacteristicconfigurationdescriptorasync), [ValueChanged](https://learn.microsoft.com/en-us/uwp/api/windows.devices.bluetooth.genericattributeprofile.gattcharacteristic.valuechanged) |
| Session lifetime and ATT MTU | [GattSession](https://learn.microsoft.com/en-us/uwp/api/windows.devices.bluetooth.genericattributeprofile.gattsession), [MaxPduSize](https://learn.microsoft.com/en-us/uwp/api/windows.devices.bluetooth.genericattributeprofile.gattsession.maxpdusize) |

## Desktop integration and C++/WinRT lifetime

- [Call Windows Runtime APIs in desktop apps](https://learn.microsoft.com/en-us/windows/apps/desktop/modernize/desktop-to-uwp-enhance): check desktop restrictions and package identity requirements alongside the member-specific docs.
- [Get started with C++/WinRT](https://learn.microsoft.com/en-us/windows/apps/develop/cpp-winrt/get-started) and [C++/WinRT documentation](https://learn.microsoft.com/en-us/windows/uwp/cpp-and-winrt-apis/): SDK setup and language projection.
- [Concurrency and asynchronous operations](https://learn.microsoft.com/en-us/windows/uwp/cpp-and-winrt-apis/concurrency-2) and [Safely accessing the this pointer with an event-handling delegate](https://learn.microsoft.com/en-us/windows/uwp/cpp-and-winrt-apis/weak-references): apartment transitions, coroutine lifetime, and callback ownership.

## Debugging and interoperability testing

- Bluetooth LE Explorer: [Microsoft source repository](https://github.com/microsoft/BluetoothLEExplorer), [Microsoft Store](https://apps.microsoft.com/detail/9n0ztkf1qd98). Useful for comparing device behavior independently of the plugin.
- [Bluetooth driver testing resources](https://learn.microsoft.com/en-us/windows-hardware/drivers/bluetooth/testing-bluetooth-main): Windows Bluetooth tracing and test resources.
- [Bluetooth Test Platform package](https://learn.microsoft.com/en-us/windows-hardware/drivers/bluetooth/testing-btp-setup-package): Microsoft interoperability testing with supported hardware. Separate from this repository's HIL fixture.
