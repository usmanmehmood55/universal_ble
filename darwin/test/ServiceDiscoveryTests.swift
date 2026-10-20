func require(_ condition: @autoclosure () -> Bool, _ message: String) {
    guard condition() else { fatalError(message) }
}

// Duplicate service instances must fill distinct result slots, regardless of order.
for withDescriptors in [false, true] {
    for cached in [false, true] {
        let peripheral = CBPeripheral()
        let first = CBService("180D")
        let second = CBService("180D")
        let a = CBCharacteristic("2A37")
        let b = CBCharacteristic("2A38")
        if cached {
            first.characteristics = [a]
            second.characteristics = [b]
            a.descriptors = [CBDescriptor()]
            b.descriptors = [CBDescriptor()]
        }
        peripheral.services = [first, second]
        var result: [UniversalBleService]?
        var completions = 0
        let discovery = UniversalBleAsyncServiceDiscovery(
            peripheral: peripheral, deviceId: "test", withDescriptors: withDescriptors
        ) {
            result = try! $0.get()
            completions += 1
        }
        discovery.startDiscovery()
        if !cached {
            require(peripheral.characteristicRequests.count == 2, "Must request both service instances")
            for (service, characteristic) in [(second, b), (first, a)] {
                service.characteristics = [characteristic]
                discovery.handleDidDiscoverCharacteristicsFor(peripheral, service: service, error: nil)
                if withDescriptors {
                    characteristic.descriptors = [CBDescriptor()]
                    discovery.handleDidDiscoverDescriptorsFor(peripheral, characteristic: characteristic, error: nil)
                }
                if service === second { require(result == nil, "Other service still pending") }
            }
        }
        require(completions == 1, "Duplicate services must complete exactly once")
        require(result?.count == 2, "Must preserve both services")
        require(result?[0].characteristics?.first?.uuid == "2A37", "First service must retain its characteristics")
        require(result?[1].characteristics?.first?.uuid == "2A38", "Second service must retain its characteristics")
        require(result?[0].characteristics?.first?.descriptors.count == (withDescriptors ? 1 : 0), "Descriptor option must be respected")
    }
}

// Equal characteristic UUIDs must not share descriptor completion state.
for reverse in [false, true] {
    let peripheral = CBPeripheral()
    let a = CBCharacteristic("2A37")
    let b = CBCharacteristic("2A37")
    let service = CBService("180D", [a, b])
    peripheral.services = [service]
    var result: [UniversalBleService]?
    let discovery = UniversalBleAsyncServiceDiscovery(
        peripheral: peripheral, deviceId: "test", withDescriptors: true
    ) { result = try! $0.get() }
    discovery.startDiscovery()
    require(peripheral.descriptorRequests.count == 2, "Must request both descriptor lists")
    let order = reverse ? [b, a] : [a, b]
    order[0].descriptors = [CBDescriptor()]
    discovery.handleDidDiscoverDescriptorsFor(peripheral, characteristic: order[0], error: nil)
    require(result == nil, "First callback must not complete another characteristic")
    order[1].descriptors = [CBDescriptor(), CBDescriptor()]
    discovery.handleDidDiscoverDescriptorsFor(peripheral, characteristic: order[1], error: nil)
    require(result?.first?.characteristics?.count == 2, "Must preserve both characteristics")
    let counts = result!.first!.characteristics!.map { $0.descriptors.count }.sorted()
    require(counts == [1, 2], "Each characteristic must retain its own descriptors")
}

// Empty duplicate services must also complete independently.
do {
    let peripheral = CBPeripheral()
    let services = [CBService("180D"), CBService("180D")]
    peripheral.services = services
    var completed = false
    let discovery = UniversalBleAsyncServiceDiscovery(
        peripheral: peripheral, deviceId: "test", withDescriptors: true
    ) {
        require(try! $0.get().count == 2, "Must preserve empty services")
        completed = true
    }
    discovery.startDiscovery()
    for service in services {
        service.characteristics = []
        discovery.handleDidDiscoverCharacteristicsFor(peripheral, service: service, error: nil)
    }
    require(completed, "Empty duplicate services must complete")
}
print("Passed 7 service discovery regression scenarios")
