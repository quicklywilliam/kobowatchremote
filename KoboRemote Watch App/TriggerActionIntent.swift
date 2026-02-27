import AppIntents

struct TriggerActionIntent: AppIntent {
    static var title: LocalizedStringResource = "Trigger KoboRemote"
    static var description: IntentDescription = "Confirms the Action Button was pressed"
    static var openAppWhenRun: Bool = true

    @MainActor
    func perform() async throws -> some IntentResult {
        ActionState.shared.trigger()
        return .result()
    }
}

struct KoboRemoteShortcuts: AppShortcutsProvider {
    static var appShortcuts: [AppShortcut] {
        AppShortcut(
            intent: TriggerActionIntent(),
            phrases: [
                "Trigger \(.applicationName)",
            ],
            shortTitle: "Trigger",
            systemImageName: "bolt.fill"
        )
    }
}
