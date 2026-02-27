import SwiftUI
import WatchKit
import os

private let logger = Logger(subsystem: "com.koboremote.watchapp", category: "UI")

struct ContentView: View {
    var ble = BLEManager.shared
    var actionState = ActionState.shared
    @State private var crownValue = 0.0
    @State private var lastCrownTick = 0

    var body: some View {
        VStack(spacing: 16) {
            // Connection status
            HStack(spacing: 6) {
                Circle()
                    .fill(statusColor)
                    .frame(width: 8, height: 8)
                Text(ble.state.rawValue)
                    .font(.caption2)
                    .foregroundStyle(.secondary)
            }

            if let action = ble.lastAction {
                Text(action)
                    .font(.title3)
                    .foregroundStyle(actionColor(action))
                    .transition(.opacity)
            }
        }
        .padding()
        .animation(.easeInOut(duration: 0.2), value: ble.lastAction)
        .focusable(true)
        .digitalCrownRotation($crownValue, from: -1000, through: 1000, sensitivity: .low, isContinuous: true, isHapticFeedbackEnabled: false)
        .onChange(of: actionState.triggered) {
            if actionState.triggered {
                if ble.isConnected {
                    ble.sendNextPage()
                    WKInterfaceDevice.current().play(.success)
                } else {
                    WKInterfaceDevice.current().play(.start)
                    ble.sendNextPage() { success in
                        WKInterfaceDevice.current().play(success ? .success : .failure)
                    }
                }
            }
        }
        .onChange(of: crownValue) {
            let tick = Int(crownValue / 5)
            if tick != lastCrownTick {
                let send = tick > lastCrownTick ? ble.sendNextPage : ble.sendPreviousPage
                lastCrownTick = tick
                if ble.isConnected {
                    send { _ in }
                    WKInterfaceDevice.current().play(.success)
                } else {
                    WKInterfaceDevice.current().play(.start)
                    send { success in
                        WKInterfaceDevice.current().play(success ? .success : .failure)
                    }
                }
            }
        }
    }

    private func actionColor(_ action: String) -> Color {
        switch action {
        case "Connecting…": .yellow
        case "Failed": .red
        default: .green
        }
    }

    private var statusColor: Color {
        switch ble.state {
        case .connected: .green
        case .scanning, .connecting: .yellow
        case .disconnected: .red
        }
    }
}

#Preview {
    ContentView()
}
