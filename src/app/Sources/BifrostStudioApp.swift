// Bifrost Studio: Samsung Download Mode flasher for macOS.
import AppKit
import SwiftUI

@main
struct BifrostStudioApp: App {
    @StateObject private var model = AppModel()

    init() {
        // Terminal use: register or inspect the background services without UI.
        if let code = BackgroundServices.runCommandLine(CommandLine.arguments) {
            exit(code)
        }
    }

    var body: some Scene {
        WindowGroup {
            RootView()
                .environmentObject(model)
                .frame(minWidth: 900, minHeight: 640)
        }
        .defaultSize(width: 1100, height: 760)
        .commands {
            SidebarCommands()
            CommandGroup(after: .sidebar) {
                Divider()
                Button("Flash") { model.section = .flash }.keyboardShortcut("1", modifiers: .command)
                Button("History") { model.section = .history }.keyboardShortcut("2", modifiers: .command)
                Button("Tools") { model.section = .tools }.keyboardShortcut("3", modifiers: .command)
            }
            // Deliberately no keyboard shortcut that starts a flash.
        }

        Settings {
            SettingsView().environmentObject(model)
        }
    }
}
