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
                    .foregroundStyle(statusColor)
                    .transition(.opacity)
            }
        }
        .background {
            Button {
                if ble.isConnected {
                    ble.sendNextPage()
                    WKInterfaceDevice.current().play(.success)
                } else {
                    ble.sendNextPage { success in
                        WKInterfaceDevice.current().play(success ? .success : .failure)
                    }
                }
            } label: {
                EmptyView()
            }
            .buttonStyle(.plain)
            .frame(width: 0, height: 0)
            .handGestureShortcut(.primaryAction)
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

    private var statusColor: Color {
        switch ble.state {
        case .connected: .green
        case .scanning, .connecting: .yellow
        case .disconnected: .red
        }
    }
}

#if DEBUG
#Preview("Connected") {
    ContentView(ble: .preview(state: .connected))
}
#Preview("Connected + Action") {
    ContentView(ble: .preview(state: .connected, lastAction: "Next →"))
}
#Preview("Scanning") {
    ContentView(ble: .preview(state: .scanning))
}
#Preview("Disconnected") {
    ContentView(ble: .preview(state: .disconnected))
}
#Preview("Connecting…") {
    ContentView(ble: .preview(state: .connecting, lastAction: "Next →"))
}
#Preview("Failed") {
    ContentView(ble: .preview(state: .disconnected, lastAction: "Failed"))
}
#endif
