// Settings (⌘,): General, Services, Flashing, Data, About.
import AppKit
import SwiftUI

struct SettingsView: View {
    var body: some View {
        TabView {
            GeneralSettings()
                .tabItem { Label("General", systemImage: "gearshape") }
            ServicesSettings()
                .tabItem { Label("Services", systemImage: "server.rack") }
            FlashingSettings()
                .tabItem { Label("Flashing", systemImage: "bolt") }
            DataSettings()
                .tabItem { Label("Data", systemImage: "internaldrive") }
            AboutSettings()
                .tabItem { Label("About", systemImage: "info.circle") }
        }
        .frame(width: 560, height: 440)
    }
}

private struct GeneralSettings: View {
    @AppStorage(Prefs.reboot) private var reboot = true
    @AppStorage(Prefs.showGuide) private var showGuide = true

    var body: some View {
        Form {
            Section("New flashes") {
                Toggle("Restart the phone when finished", isOn: $reboot)
                Text("The starting choice on the Confirm panel. Applies to the next flash you set up; you can still change it each time.")
                    .font(.caption).foregroundStyle(.secondary)
            }
            Section("Setup") {
                Toggle("Show the Download Mode instructions by default", isOn: $showGuide)
            }
        }
        .formStyle(.grouped)
    }
}

private struct ServicesSettings: View {
    var body: some View {
        Form {
            Section("Background services") {
                Text("The flash agent runs flashes so they continue if the window closes or the app quits. On logout, launchd allows up to 300 seconds for an in-progress flash to stop safely at the next sequence boundary.")
                    .font(.callout).foregroundStyle(.secondary)
                Text("The USB helper runs as root and only claims the Samsung USB interface and moves data for the agent; it never opens your files.")
                    .font(.callout).foregroundStyle(.secondary)
                ServiceControlsView()
            }
        }
        .formStyle(.grouped)
    }
}

private struct FlashingSettings: View {
    @AppStorage(Prefs.showHandshake) private var showHandshake = false

    var body: some View {
        Form {
            Section("Safety rules (always on)") {
                ForEach(["Anything ambiguous is refused before data is sent.",
                         "The model you type must match the firmware.",
                         "Full CSC or USERDATA needs an explicit data-erase acknowledgement.",
                         "Partition re-layout, NAND erase and userdata-only erase are not offered.",
                         "Modem-type partitions (PIT binary type 1) are refused."], id: \.self) { rule in
                    Label(rule, systemImage: "lock.fill").font(.callout)
                }
            }
            Section("Advanced") {
                Toggle("Always show the Download Mode session override", isOn: $showHandshake)
                Text("The phone answers the connection handshake once per Download Mode entry, and Bifrost Studio tracks this automatically. The override normally appears only after a timeout.")
                    .font(.caption).foregroundStyle(.secondary)
            }
        }
        .formStyle(.grouped)
    }
}

private struct DataSettings: View {
    @EnvironmentObject private var model: AppModel
    @AppStorage(Prefs.historyLimit) private var limit = 200
    @State private var confirmClear = false

    var body: some View {
        Form {
            Section("History") {
                Picker("Keep", selection: $limit) {
                    Text("Last 50 entries").tag(50)
                    Text("Last 200 entries").tag(200)
                    Text("Last 1000 entries").tag(1000)
                    Text("Everything").tag(0)
                }
                .onChange(of: limit) { _ in model.applyRetention() }
                LabeledContent("Stored entries", value: "\(model.history.count)")
                if let folder = model.historyFolder {
                    LabeledContent("Location") {
                        Text(folder.path).font(.caption).foregroundStyle(.secondary)
                            .lineLimit(2).truncationMode(.middle).textSelection(.enabled)
                    }
                }
                HStack {
                    Button("Show in Finder") {
                        if let folder = model.historyFolder {
                            try? FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
                            NSWorkspace.shared.open(folder)
                        }
                    }
                    Spacer()
                    Button("Clear History…", role: .destructive) { confirmClear = true }
                        .disabled(model.history.isEmpty)
                }
            }
        }
        .formStyle(.grouped)
        .confirmationDialog("Clear all history?", isPresented: $confirmClear) {
            Button("Clear History", role: .destructive) { model.clearHistory() }
        } message: {
            Text("All saved logs are removed from this Mac. This cannot be undone.")
        }
    }
}

private struct AboutSettings: View {
    @EnvironmentObject private var model: AppModel
    @State private var copied = false

    private var version: String {
        let info = Bundle.main.infoDictionary
        let short = info?["CFBundleShortVersionString"] as? String ?? "?"
        let build = info?["CFBundleVersion"] as? String ?? "?"
        return "\(short) (\(build))"
    }

    private var notices: String {
        guard let url = Bundle.main.url(forResource: "THIRD-PARTY-NOTICES", withExtension: "txt"),
              let text = try? String(contentsOf: url, encoding: .utf8) else {
            return "THIRD-PARTY-NOTICES.txt is not in this build."
        }
        return text
    }

    var body: some View {
        Form {
            Section {
                LabeledContent("Bifrost Studio", value: version)
                Text("Hardware-tested on SM-N920I, SM-N950F and others; see the README for current scope. Android tools remain UNVERIFIED until tested on a phone.")
                    .font(.caption).foregroundStyle(.secondary)
                HStack {
                    Button("Copy Diagnostics") {
                        NSPasteboard.general.clearContents()
                        NSPasteboard.general.setString(Diagnostics.text(model), forType: .string)
                        copied = true
                        DispatchQueue.main.asyncAfter(deadline: .now() + 2) { copied = false }
                    }
                    Button("Export Diagnostics Bundle…") { exportBundle() }
                    if copied { Text("Copied").font(.caption).foregroundStyle(.secondary) }
                }
            }
            Section("Third-party licences") {
                ScrollView {
                    Text(notices).font(.caption.monospaced())
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .textSelection(.enabled)
                }
                .frame(height: 140)
            }
        }
        .formStyle(.grouped)
    }

    private func exportBundle() {
        let panel = NSSavePanel()
        panel.allowedContentTypes = [.zip]
        panel.nameFieldStringValue = "BifrostStudio-diagnostics.zip"
        guard panel.runModal() == .OK, let url = panel.url else { return }
        do {
            try Diagnostics.exportBundle(model, to: url)
        } catch {
            let alert = NSAlert(error: error)
            alert.runModal()
        }
    }
}

/// Plain-text state for bug reports. Contains no firmware paths.
@MainActor
enum Diagnostics {
    static func text(_ model: AppModel) -> String {
        let info = Bundle.main.infoDictionary
        #if arch(arm64)
        let arch = "arm64"
        #else
        let arch = "x86_64"
        #endif
        var lines = [
            "Bifrost Studio \(info?["CFBundleShortVersionString"] as? String ?? "?") (\(info?["CFBundleVersion"] as? String ?? "?"))",
            "macOS \(ProcessInfo.processInfo.operatingSystemVersionString) · \(arch)",
            "Services: \(BackgroundServices.summary())",
            "Agent connection: \(describe(model.agentState))",
            "Phones seen: \(phoneSummary(model.devices))",
            "Assumes handshake done: \(model.handshakeDone)",
        ]
        if let last = model.history.first {
            lines.append("Last job: \(last.kind.title) \(last.succeeded ? "OK" : "FAILED") at \(last.date)")
        }
        return lines.joined(separator: "\n")
    }

    /// Writes diagnostics.txt and recent History JSON into a zip (no firmware paths).
    static func exportBundle(_ model: AppModel, to url: URL) throws {
        let fm = FileManager.default
        let staging = fm.temporaryDirectory.appendingPathComponent(UUID().uuidString, isDirectory: true)
        try fm.createDirectory(at: staging, withIntermediateDirectories: true)
        defer { try? fm.removeItem(at: staging) }
        try text(model).write(to: staging.appendingPathComponent("diagnostics.txt"),
                              atomically: true, encoding: .utf8)
        if let history = model.historyFolder, fm.fileExists(atPath: history.path) {
            try fm.copyItem(at: history, to: staging.appendingPathComponent("history", isDirectory: true))
        }
        if fm.fileExists(atPath: url.path) { try fm.removeItem(at: url) }
        let process = Process()
        process.executableURL = URL(fileURLWithPath: "/usr/bin/ditto")
        process.arguments = ["-c", "-k", "--sequesterRsrc", "--keepParent", staging.path, url.path]
        try process.run()
        process.waitUntilExit()
        guard process.terminationStatus == 0 else {
            throw NSError(domain: "BifrostStudio", code: 1,
                          userInfo: [NSLocalizedDescriptionKey: "Could not create the diagnostics zip"])
        }
    }

    private static func phoneSummary(_ devices: [Device]) -> String {
        devices.map { dev in
            var tag = dev.id.label
            if dev.inDownloadMode {
                tag += dev.unlistedProductID ? " [Download Mode, unlisted PID]" : " [Download Mode]"
            }
            return tag
        }.joined(separator: "; ")
    }

    private static func describe(_ state: AgentState) -> String {
        switch state {
        case .connecting: return "connecting"
        case .connected: return "connected"
        case let .unavailable(message): return "unavailable (\(message))"
        }
    }
}
