// Bifrost Studio: Samsung Download Mode flasher for macOS.
import AppKit
import SwiftUI

@main
struct BifrostStudioApp: App {
    @NSApplicationDelegateAdaptor private var appDelegate: AppDelegate
    @StateObject private var model = AppModel()

    init() {
        // Terminal use: register or inspect the background services without UI.
        if let code = BackgroundServices.runCommandLine(CommandLine.arguments) {
            exit(code)
        }
    }

    var body: some Scene {
        WindowGroup(id: "main") {
            RootView()
                .environmentObject(model)
                .frame(minWidth: 900, minHeight: 640)
        }
        .defaultSize(width: 1100, height: 760)
        .commands { AppCommands(model: model) }

        Settings {
            SettingsView().environmentObject(model)
        }
    }
}
