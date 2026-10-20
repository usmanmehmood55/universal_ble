import Foundation

// Minimal platform boundary. Requests are recorded; tests deliver delegate callbacks.
struct CBUUID { let uuidString: String }
struct TestProperties { var toCharacteristicProperty: [Int] { [] } }
final class CBDescriptor {
    let uuid = CBUUID(uuidString: "2901")
}
final class CBCharacteristic {
    let uuid: CBUUID
    weak var service: CBService?
    let properties = TestProperties()
    var descriptors: [CBDescriptor]?
    init(_ uuid: String) { self.uuid = CBUUID(uuidString: uuid) }
}
final class CBService {
    let uuid: CBUUID
    var characteristics: [CBCharacteristic]? {
        didSet { characteristics?.forEach { $0.service = self } }
    }
    init(_ uuid: String, _ characteristics: [CBCharacteristic]? = nil) {
        self.uuid = CBUUID(uuidString: uuid)
        self.characteristics = characteristics
        characteristics?.forEach { $0.service = self }
    }
}
final class CBPeripheral {
    var services: [CBService]?
    var characteristicRequests: [CBService] = []
    var descriptorRequests: [CBCharacteristic] = []
    func discoverServices(_: [CBUUID]?) {}
    func discoverCharacteristics(_: [CBUUID]?, for service: CBService) {
        characteristicRequests.append(service)
    }
    func discoverDescriptors(for characteristic: CBCharacteristic) {
        descriptorRequests.append(characteristic)
    }
}
struct UniversalBleDescriptor { let uuid: String }
struct UniversalBleCharacteristic {
    let uuid: String
    let properties: [Int]
    let descriptors: [UniversalBleDescriptor]
}
struct UniversalBleService {
    let uuid: String
    let characteristics: [UniversalBleCharacteristic]?
}
struct AppleOptions { let timeoutMillis: Int64? }
struct DiscoverServicesPlatformConfig { let apple: AppleOptions? }
enum TestError: Error { case failed }
func createFlutterError(code: TestError, message: String) -> Error { code }
final class UniversalBleLogger {
    static let shared = UniversalBleLogger()
    func logWarning(_: String) {}
    func logError(_: String) {}
}
