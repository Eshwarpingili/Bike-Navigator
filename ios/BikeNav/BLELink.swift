import CoreBluetooth
import Foundation

/// Finds the BikeNav board, keeps the connection alive (also in the background)
/// and writes packets to it, newest-wins.
final class BLELink: NSObject, ObservableObject {
    enum State: String {
        case off = "Bluetooth is off"
        case unauthorized = "Bluetooth not allowed"
        case searching = "Looking for display"
        case connecting = "Connecting"
        case connected = "Display connected"
    }

    static let serviceUUID = CBUUID(string: "DD3F0AD1-6239-4E1F-81F1-91F6C9F01D86")
    static let indicateUUID = CBUUID(string: "DD3F0AD2-6239-4E1F-81F1-91F6C9F01D86")
    static let writeUUID = CBUUID(string: "DD3F0AD3-6239-4E1F-81F1-91F6C9F01D86")
    private static let savedPeripheralKey = "bikenavPeripheral"

    @Published private(set) var state: State = .searching

    /// Called once the write characteristic is ready, so the owner can resend state.
    var onReady: (() -> Void)?

    private var central: CBCentralManager!
    private var peripheral: CBPeripheral?
    private var writeChar: CBCharacteristic?
    private var pending: Data?
    private var writeInFlight = false
    private var lastSent: Data?
    private var lastSentAt = Date.distantPast

    override init() {
        super.init()
        central = CBCentralManager(delegate: self, queue: nil, options: [
            CBCentralManagerOptionRestoreIdentifierKey: "BikeNavCentral",
            CBCentralManagerOptionShowPowerAlertKey: true,
        ])
    }

    var isReady: Bool { writeChar != nil }

    /// Queue a packet. Identical packets are skipped unless 2 s have passed (keeps the
    /// board's "no data" warning away) or `force` is set.
    func send(_ packet: Data, force: Bool = false) {
        guard force || packet != lastSent || Date().timeIntervalSince(lastSentAt) > 2 else { return }
        pending = packet
        flush()
    }

    /// Forget the paired board and look for any BikeNav display again.
    func forget() {
        UserDefaults.standard.removeObject(forKey: Self.savedPeripheralKey)
        if let p = peripheral { central.cancelPeripheralConnection(p) }
        peripheral = nil
        writeChar = nil
        startScan()
    }

    private func flush() {
        guard !writeInFlight, let p = peripheral, let c = writeChar, let data = pending else { return }
        pending = nil
        writeInFlight = true
        lastSent = data
        lastSentAt = Date()
        p.writeValue(data, for: c, type: .withResponse)
    }

    private func startScan() {
        guard central.state == .poweredOn else { return }
        state = .searching
        central.scanForPeripherals(withServices: [Self.serviceUUID], options: nil)
    }

    private func connect(_ p: CBPeripheral) {
        central.stopScan()
        peripheral = p
        p.delegate = self
        state = .connecting
        central.connect(p, options: nil)
    }

    private func reconnectOrScan() {
        if let s = UserDefaults.standard.string(forKey: Self.savedPeripheralKey),
           let id = UUID(uuidString: s),
           let known = central.retrievePeripherals(withIdentifiers: [id]).first {
            connect(known)
        } else if let already = central.retrieveConnectedPeripherals(withServices: [Self.serviceUUID]).first {
            connect(already)
        } else {
            startScan()
        }
    }

    private func resetLink() {
        writeChar = nil
        writeInFlight = false
        lastSent = nil
    }
}

extension BLELink: CBCentralManagerDelegate {
    func centralManagerDidUpdateState(_ central: CBCentralManager) {
        switch central.state {
        case .poweredOn:
            if let p = peripheral, p.state == .connected {
                p.discoverServices([Self.serviceUUID])
            } else {
                reconnectOrScan()
            }
        case .unauthorized:
            state = .unauthorized
        default:
            resetLink()
            state = .off
        }
    }

    func centralManager(_ central: CBCentralManager, willRestoreState dict: [String: Any]) {
        if let restored = (dict[CBCentralManagerRestoredStatePeripheralsKey] as? [CBPeripheral])?.first {
            peripheral = restored
            restored.delegate = self
        }
    }

    func centralManager(_ central: CBCentralManager, didDiscover peripheral: CBPeripheral,
                        advertisementData: [String: Any], rssi RSSI: NSNumber) {
        connect(peripheral)
    }

    func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        UserDefaults.standard.set(peripheral.identifier.uuidString, forKey: Self.savedPeripheralKey)
        peripheral.discoverServices([Self.serviceUUID])
    }

    func centralManager(_ central: CBCentralManager, didFailToConnect peripheral: CBPeripheral, error: Error?) {
        resetLink()
        startScan()
    }

    func centralManager(_ central: CBCentralManager, didDisconnectPeripheral peripheral: CBPeripheral, error: Error?) {
        resetLink()
        guard self.peripheral == peripheral else { return }
        // Re-arm: iOS completes this whenever the board comes back, even in the background.
        state = .connecting
        central.connect(peripheral, options: nil)
    }
}

extension BLELink: CBPeripheralDelegate {
    func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        guard let service = peripheral.services?.first(where: { $0.uuid == Self.serviceUUID }) else { return }
        peripheral.discoverCharacteristics([Self.writeUUID, Self.indicateUUID], for: service)
    }

    func peripheral(_ peripheral: CBPeripheral, didDiscoverCharacteristicsFor service: CBService, error: Error?) {
        for c in service.characteristics ?? [] {
            if c.uuid == Self.writeUUID { writeChar = c }
            if c.uuid == Self.indicateUUID { peripheral.setNotifyValue(true, for: c) }
        }
        guard writeChar != nil else { return }
        state = .connected
        writeInFlight = false
        onReady?()
        flush()
    }

    func peripheral(_ peripheral: CBPeripheral, didWriteValueFor characteristic: CBCharacteristic, error: Error?) {
        writeInFlight = false
        flush()
    }

    func peripheral(_ peripheral: CBPeripheral, didUpdateValueFor characteristic: CBCharacteristic, error: Error?) {
        // Keep-alive indications from the board; nothing to do.
    }
}
