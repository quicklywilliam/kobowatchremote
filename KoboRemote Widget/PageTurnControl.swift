import AppIntents
import SwiftUI
import WidgetKit

struct PageTurnControl: ControlWidget {
    var body: some ControlWidgetConfiguration {
        StaticControlConfiguration(kind: "com.koboremote.pageturn") {
            ControlWidgetButton(action: TriggerActionIntent()) {
                Label("Next Page", systemImage: "chevron.right")
            }
        }
        .displayName("Kobo Next Page")
        .description("Turn to the next page on your Kobo.")
    }
}

@main
struct KoboRemoteWidgetBundle: WidgetBundle {
    var body: some Widget {
        PageTurnControl()
    }
}
