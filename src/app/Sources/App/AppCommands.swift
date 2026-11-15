// Menu bar commands (Help, section shortcuts). Flash has no shortcut by design.
import AppKit
import SwiftUI

enum HelpDocument {
    static func openUserGuide() {
        let candidates = [
            Bundle.main.url(forResource: "UserGuide", withExtension: "md"),
            Bundle.main.url(forResource: "UserGuide", withExtension: "md", subdirectory: "Resources"),
        ]
        guard let url = candidates.compactMap({ $0 }).first else { return }
        NSWorkspace.shared.open(url)
    }

    static func openDesignGuide() {
        let candidates = [
            Bundle.main.url(forResource: "Design", withExtension: "md"),
            Bundle.main.url(forResource: "Design", withExtension: "md", subdirectory: "Resources"),
        ]
        guard let url = candidates.compactMap({ $0 }).first else { return }
        NSWorkspace.shared.open(url)
    }
}

struct AppCommands: Commands {
    @ObservedObject var model: AppModel

    var body: some Commands {
        SidebarCommands()
        CommandGroup(after: .sidebar) {
            Divider()
            Button(L10n.flash) { model.section = .flash }
                .keyboardShortcut("1", modifiers: .command)
            Button(L10n.inspect) { model.section = .inspect }
                .keyboardShortcut("2", modifiers: .command)
            Button(L10n.history) { model.section = .history }
                .keyboardShortcut("3", modifiers: .command)
            Button(L10n.device) { model.section = .device }
                .keyboardShortcut("4", modifiers: .command)
            Button(L10n.android) { model.section = .android }
                .keyboardShortcut("5", modifiers: .command)
        }
        CommandGroup(replacing: .help) {
            Button(L10n.appHelp) { HelpDocument.openUserGuide() }
                .keyboardShortcut("?", modifiers: .command)
            Button(L10n.designDoc) { HelpDocument.openDesignGuide() }
        }
    }
}