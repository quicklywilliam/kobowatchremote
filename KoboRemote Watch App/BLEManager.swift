import CoreBluetooth
import SwiftUI
import os

private let logger = Logger(subsystem: "com.koboremote.watchapp", category: "BLE")

@Observable
final class BLEManager: NSObject {
    static let shared = BLEManager()

    enum ConnectionState: String {
        case disconnected = "Disconnected"
        case scanning = "Scanning…"
        case connecting = "Connecting…"
        case connected = "Connected"
    }

    private static let serviceUUID = CBUUID(string: "0B278E49-7F56-4788-A1BB-4624E0D64B46")
    private static let characteristicUUID = CBUUID(string: "5257acb0-be4d-4cf1-af8f-cbdb67bf998a")

    private static let nextPage: UInt8 = 0x01
    private static let previousPage: UInt8 = 0x02

    private(set) var state: ConnectionState = .disconnected
    private(set) var lastAction: String?
    private(set) var debugInfo: String = ""
    private(set) var foundDevices: [String] = []

    private var centralManager: CBCentralManager!
    private var peripheral: CBPeripheral?
    private var characteristic: CBCharacteristic?

    private override init() {
        super.init()
        centralManager = CBCentralManager(delegate: self, queue: nil)
    }

    var isConnected: Bool { state == .connected }

    func sendNextPage(completion: ((Bool) -> Void)? = nil) {
        sendCommand(Self.nextPage, label: "Next →", completion: completion)
    }

    func sendPreviousPage(completion: ((Bool) -> Void)? = nil) {
        sendCommand(Self.previousPage, label: "← Prev", completion: completion)
    }

    private func sendCommand(_ command: UInt8, label: String, completion: ((Bool) -> Void)?) {
        if let peripheral, let characteristic {
            peripheral.writeValue(Data([command]), for: characteristic, type: .withResponse)
            lastAction = label
            clearAction()
            completion?(true)
        } else {
            // Queue command and attempt reconnect
            lastAction = "Connecting…"
            pendingCommand = (command, label, completion)
            attemptReconnectWithTimeout()
        }
    }

    private var pendingCommand: (command: UInt8, label: String, completion: ((Bool) -> Void)?)?
    private var reconnectTimeoutTask: Task<Void, Never>?

    private func attemptReconnectWithTimeout() {
        // If already scanning/connecting, just set the timeout
        if state == .disconnected {
            startScanning()
        }
        reconnectTimeoutTask?.cancel()
        reconnectTimeoutTask = Task { @MainActor in
            try? await Task.sleep(for: .seconds(2))
            guard !Task.isCancelled else { return }
            // Timed out — flush pending command as failure
            if let pending = pendingCommand {
                lastAction = "Failed"
                clearAction()
                pending.completion?(false)
                pendingCommand = nil
            }
        }
    }

    fileprivate func flushPendingCommand() {
        guard let pending = pendingCommand, let peripheral, let characteristic else { return }
        reconnectTimeoutTask?.cancel()
        peripheral.writeValue(Data([pending.command]), for: characteristic, type: .withResponse)
        lastAction = pending.label
        clearAction()
        pending.completion?(true)
        pendingCommand = nil
    }

    private func clearAction() {
        Task { @MainActor in
            try? await Task.sleep(for: .seconds(1))
            lastAction = nil
        }
    }

    private func log(_ msg: String) {
        logger.info("\(msg)")
    }

    private func startScanning() {
        guard centralManager.state == .poweredOn else {
            log("BT not ready: \(centralManager.state.rawValue)")
            return
        }
        state = .scanning
        log("Scanning for Kobo service UUID…")
        centralManager.scanForPeripherals(withServices: [Self.serviceUUID], options: [CBCentralManagerScanOptionAllowDuplicatesKey: true])
    }
}

// MARK: - CBCentralManagerDelegate
extension BLEManager: CBCentralManagerDelegate {
    func centralManagerDidUpdateState(_ central: CBCentralManager) {
        log("BT state: \(central.state.rawValue)")
        if central.state == .poweredOn {
            startScanning()
        } else {
            state = .disconnected
        }
    }

    func centralManager(_ central: CBCentralManager, didDiscover peripheral: CBPeripheral,
                        advertisementData: [String: Any], rssi RSSI: NSNumber) {
        let name = peripheral.name ?? advertisementData[CBAdvertisementDataLocalNameKey] as? String ?? "?"
        let uuid = peripheral.identifier.uuidString
        let serviceUUIDs = (advertisementData[CBAdvertisementDataServiceUUIDsKey] as? [CBUUID])?.map(\.uuidString) ?? []
        log("Found: \(name) | \(uuid) | services: \(serviceUUIDs)")

        // Match by name or known peripheral UUID
        let isKobo = name.contains("Kobo") || peripheral.identifier.uuidString == "4ABC2FB0-3D4F-7112-4D11-E7ECDF24F9AC"
        guard isKobo else { return }

        log("Connecting to \(name)…")
        self.peripheral = peripheral
        peripheral.delegate = self
        state = .connecting
        centralManager.stopScan()
        centralManager.connect(peripheral)
    }

    func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        log("Connected, discovering services…")
        peripheral.discoverServices([Self.serviceUUID])
    }

    func centralManager(_ central: CBCentralManager, didFailToConnect peripheral: CBPeripheral, error: Error?) {
        log("Failed to connect: \(error?.localizedDescription ?? "unknown")")
        state = .disconnected
        startScanning()
    }

    func centralManager(_ central: CBCentralManager, didDisconnectPeripheral peripheral: CBPeripheral, error: Error?) {
        log("Disconnected")
        self.characteristic = nil
        state = .disconnected
        startScanning()
    }
}

// MARK: - CBPeripheralDelegate
extension BLEManager: CBPeripheralDelegate {
    func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        guard let service = peripheral.services?.first(where: { $0.uuid == Self.serviceUUID }) else {
            return
        }
        peripheral.discoverCharacteristics([Self.characteristicUUID], for: service)
    }

    func peripheral(_ peripheral: CBPeripheral, didDiscoverCharacteristicsFor service: CBService, error: Error?) {
        guard let char = service.characteristics?.first(where: { $0.uuid == Self.characteristicUUID }) else {
            return
        }
        characteristic = char
        state = .connected
        flushPendingCommand()
    }
}
