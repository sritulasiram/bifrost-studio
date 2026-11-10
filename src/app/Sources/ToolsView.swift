// Tools: read-only phone checks and the firmware / PIT inspector.
import AppKit
import SwiftUI
import UniformTypeIdentifiers

private enum ToolTab: String, CaseIterable, Identifiable {
    case probe = "Probe", backup = "PIT Backup", inspect = "Inspect", android = "Android", twrp = "TWRP"
    var id: String { rawValue }
}

struct ToolsView: View {
    @EnvironmentObject private var model: AppModel
    @State private var tab: ToolTab = .probe

    var body: some View {
        VStack(spacing: 0) {
            if model.handshakeDone { SessionBanner() }
            switch tab {
            case .probe: ProbeTool()
            case .backup: BackupTool()
            case .inspect: InspectTool()
            case .android: AndroidTool()
            case .twrp: TwrpTool()
            }
        }
        .navigationTitle("Tools")
        .toolbar {
            ToolbarItem(placement: .principal) {
                Picker("Tool", selection: $tab) {
                    ForEach(ToolTab.allCases) { Text($0.rawValue).tag($0) }
                }
                .pickerStyle(.segmented)
                .help("Choose a tool")
            }
        }
    }
}

/// The phone these read-only tools will talk to.
private struct PhoneRow: View {
    @EnvironmentObject private var model: AppModel

    var body: some View {
        LabeledContent("Phone") {
            if let id = model.selectedDevice {
                Label(id.label, systemImage: "iphone")
            } else {
                Text("None in Download Mode").foregroundStyle(.secondary)
            }
        }
    }
}

/// Shown once above every tool, only after a session was used.
private struct SessionBanner: View {
    @EnvironmentObject private var model: AppModel

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            Label("Restart the phone into Download Mode before flashing", systemImage: "exclamationmark.triangle.fill")
                .foregroundStyle(.orange)
            Text("The phone answers one session per Download Mode. A probe or PIT backup has used it.")
                .font(.callout).foregroundStyle(.secondary)
            HStack {
                Button("I Restarted It") { model.phoneWasRestarted() }
                Spacer()
            }
            HandshakeToggle()
        }
        .padding(12)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(.bar)
    }
}

private struct ProbeTool: View {
    @EnvironmentObject private var model: AppModel

    var body: some View {
        Form {
            Section {
                PhoneRow()
                Button("Run Probe") { model.probe() }
                    .disabled(!model.canStartJob)
                ToolResult(backup: false)
            } header: {
                Text("Probe")
            } footer: {
                Text("Opens and closes a session to check that the phone answers. Sends no partition table and no firmware. It uses up this Download Mode session: restart the phone into Download Mode before flashing, and do not run it just before a flash.")
            }
        }
        .formStyle(.grouped)
    }
}

private struct BackupTool: View {
    @EnvironmentObject private var model: AppModel

    var body: some View {
        Form {
            Section {
                PhoneRow()
                Button("Back Up PIT…") { backup() }
                    .disabled(!model.canStartJob)
                ToolResult(backup: true)
                if let url = model.lastBackup {
                    LabeledContent {
                        Button("Show in Finder") { NSWorkspace.shared.activateFileViewerSelecting([url]) }
                    } label: {
                        Label("Saved \(url.lastPathComponent)", systemImage: "checkmark.circle.fill")
                            .foregroundStyle(.green)
                    }
                }
            } header: {
                Text("PIT backup")
            } footer: {
                Text("Reads the phone's partition table into a file. Writes nothing to the phone. It uses up this Download Mode session: restart the phone into Download Mode before flashing.")
            }
        }
        .formStyle(.grouped)
    }

    private func backup() {
        let panel = NSSavePanel()
        panel.nameFieldStringValue = "device.pit"
        panel.allowedContentTypes = [UTType(filenameExtension: "pit") ?? .data]
        if panel.runModal() == .OK, let url = panel.url {
            model.backupPit(to: url)
        }
    }
}

/// The status or result of this tool's job (kept while you switch pages).
private struct ToolResult: View {
    @EnvironmentObject private var model: AppModel
    let backup: Bool

    var body: some View {
        if let job = model.job, matches(job.purpose) {
            JobLine(job: job)
        }
    }

    private func matches(_ purpose: JobState.Purpose) -> Bool {
        switch purpose {
        case .backupPit: return backup
        case .probe: return !backup
        case .flash: return false
        }
    }
}

// MARK: - Inspect

private struct InspectTool: View {
    @EnvironmentObject private var model: AppModel
    @State private var pit: PitTable?
    @State private var pitName = ""
    @State private var offlinePlan: Plan?
    @State private var pitError: String?
    @State private var targeted = false

    var body: some View {
        Form {
            firmware
            partitions
        }
        .formStyle(.grouped)
        .onChange(of: model.packages?.infos.count) { _ in
            // Keep the offline plan in step with the verified firmware.
            if let pit { apply(pit.bytes, name: pitName) }
        }
    }

    private var firmware: some View {
        Section {
            if model.packages == nil {
                Text("No firmware verified yet. Add files in Flash; they appear here.")
                    .foregroundStyle(.secondary)
            }
            ForEach(model.packages?.infos ?? []) { info in
                DisclosureGroup("\(info.slot.title): \(info.fileName) (\(info.entries.count) entries)") {
                    ForEach(info.entries) { entry in
                        LabeledContent {
                            Text((entry.lz4 ? "LZ4 · " : "") + formatBytes(entry.size)).foregroundStyle(.secondary)
                        } label: {
                            Text(entry.name).font(.body.monospaced())
                        }
                    }
                }
            }
        } header: {
            Text("Firmware")
        }
    }

    private var partitions: some View {
        Section {
            LabeledContent(pit == nil ? "No PIT loaded" : pitName) {
                HStack {
                    if let data = model.devicePit?.bytes {
                        Button("Use the PIT from Review") { apply(data, name: model.pitSource) }
                    }
                    Button("Open PIT File…") { openPit() }
                }
            }
            if let pitError { Text(pitError).foregroundStyle(.red) }
            if let pit {
                Table(pit.entries) {
                    TableColumn("ID") { Text("\($0.identifier)").monospacedDigit() }.width(40)
                    TableColumn("Partition") { Text($0.partition).bold() }
                    TableColumn("File") { Text($0.flashFilename).foregroundStyle(.secondary) }
                    TableColumn("Firmware image") { entry in
                        if let file = offlinePlan?.files.first(where: { $0.partitionID == entry.identifier }) {
                            Text(file.entry).font(.callout.monospaced())
                        } else {
                            Text("—").foregroundStyle(.tertiary)
                        }
                    }
                    TableColumn("Blocks") { Text("\($0.blockCount)").monospacedDigit() }.width(80)
                    TableColumn("Note") {
                        if $0.binaryType == 1 { Text("modem type").foregroundStyle(.orange) }
                    }
                    .width(80)
                }
                .frame(minHeight: 280)
            }
            if let offlinePlan {
                DisclosureGroup("Offline plan (no phone access)") { PlanView(plan: offlinePlan) }
            } else if pit != nil, model.packages == nil {
                Text("Verify firmware in Flash to see how it would map to this table.")
                    .foregroundStyle(.secondary)
            }
        } header: {
            Text("Partition table")
        } footer: {
            Text("You can also drop a .pit file onto this section.")
        }
        .dropDestination(for: URL.self) { urls, _ in
            guard let url = urls.first else { return false }
            load(url)
            return true
        } isTargeted: { targeted = $0 }
    }

    private func openPit() {
        let panel = NSOpenPanel()
        panel.canChooseDirectories = false
        panel.message = "Choose a partition table (.pit) file"
        guard panel.runModal() == .OK, let url = panel.url else { return }
        load(url)
    }

    private func load(_ url: URL) {
        do {
            apply(try Data(contentsOf: url), name: url.lastPathComponent)
        } catch {
            pitError = error.localizedDescription
        }
    }

    private func apply(_ data: Data, name: String) {
        switch PitTable.parse(data, packages: model.packages) {
        case let .success((table, plan)):
            pit = table
            pitName = name
            offlinePlan = plan
            pitError = nil
        case let .failure(error):
            pit = nil
            offlinePlan = nil
            pitError = error.message
        }
    }
}

// MARK: - Android (bundled platform-tools)

/// Runs the adb that ships inside the app (Contents/Resources/platform-tools).
private enum Adb {
    static var url: URL? {
        Bundle.main.url(forResource: "adb", withExtension: nil, subdirectory: "platform-tools")
    }

    static func run(_ args: [String]) async -> String {
        guard let exe = url else { return "adb is not bundled in this build." }
        return await Task.detached {
            let process = Process()
            process.executableURL = exe
            process.arguments = args
            let pipe = Pipe()
            process.standardOutput = pipe
            process.standardError = pipe
            do { try process.run() } catch { return "Could not run adb: \(error.localizedDescription)" }
            let data = pipe.fileHandleForReading.readDataToEndOfFile()
            process.waitUntilExit()
            let text = String(decoding: data, as: UTF8.self).trimmingCharacters(in: .whitespacesAndNewlines)
            return text.isEmpty ? "(no output)" : text
        }.value
    }
}

private struct AndroidTool: View {
    @State private var output = ""
    @State private var busy = false

    var body: some View {
        Form {
            Section {
                LabeledContent("adb") {
                    Text(Adb.url == nil ? "Not bundled" : "Bundled platform-tools")
                        .foregroundStyle(.secondary)
                }
                Button("List Phones") { run(["devices", "-l"]) }
                Button("Reboot to Download Mode") { run(["reboot", "download"]) }
                Button("Reboot to Recovery") { run(["reboot", "recovery"]) }
                Button("Restart adb Server") { run(["kill-server"], then: ["start-server"]) }
                if !output.isEmpty {
                    Text(output).font(.callout.monospaced()).textSelection(.enabled)
                }
            } header: {
                Text("Android platform-tools")
            } footer: {
                Text("Reboot to Download Mode needs USB debugging on and this Mac authorised on the phone. Without it, use the key combination instead.")
            }
        }
        .formStyle(.grouped)
        .disabled(busy)
    }

    private func run(_ args: [String], then next: [String]? = nil) {
        busy = true
        Task {
            var text = await Adb.run(args)
            if let next { text += "\n" + (await Adb.run(next)) }
            output = text
            busy = false
        }
    }
}

// MARK: - TWRP

/// Prepares a TWRP recovery for the normal Flash flow: it becomes the only
/// file in the AP slot and the phone is not restarted automatically, so you
/// can boot straight into recovery.
private struct TwrpTool: View {
    @EnvironmentObject private var model: AppModel
    @State private var chosen: URL?

    var body: some View {
        Form {
            Section {
                LabeledContent("TWRP file") {
                    Text(chosen?.lastPathComponent ?? "None chosen").foregroundStyle(.secondary)
                }
                Button("Choose TWRP File…") { choose() }
                Button("Prepare Flash") { prepare() }
                    .disabled(chosen == nil)
                Link("Find TWRP for your model (twrp.me)", destination: URL(string: "https://twrp.me/Devices/Samsung/")!)
            } header: {
                Text("Install TWRP")
            } footer: {
                Text("Use the build made for your exact model: the .img.tar for Odin, not the .img. Prepare Flash loads it into the AP slot, turns off the automatic restart and opens Flash.")
            }
            Section {
                Label("Turn on OEM unlocking in Developer options first. If the phone has no such option or it is greyed out, the bootloader cannot be unlocked and TWRP will not install.", systemImage: "lock.open")
                Label("Unlocking the bootloader erases all data and trips Knox permanently.", systemImage: "exclamationmark.triangle.fill")
                    .foregroundStyle(.orange)
                Label("After the flash, hold Vol Down + Power to leave Download Mode, then immediately hold Vol Up + Home + Power (or the keys for your model) to boot into TWRP. A normal start can restore the stock recovery.", systemImage: "arrow.triangle.2.circlepath")
            } header: {
                Text("Before and after")
            }
        }
        .formStyle(.grouped)
    }

    private func choose() {
        let panel = NSOpenPanel()
        panel.message = "Choose a TWRP .img.tar (or .tar) for this phone"
        panel.allowsMultipleSelection = false
        if panel.runModal() == .OK { chosen = panel.url }
    }

    private func prepare() {
        guard let chosen else { return }
        for slot in Slot.allCases { model.paths[slot] = nil }
        model.setPath(chosen.path, for: .ap)
        model.rebootWhenDone = false
        model.section = .flash
    }
}
