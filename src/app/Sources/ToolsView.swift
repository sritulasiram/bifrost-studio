// Tools: read-only phone checks and the firmware / PIT inspector.
import AppKit
import SwiftUI
import UniformTypeIdentifiers

private enum ToolTab: String, CaseIterable, Identifiable {
    case probe = "Probe", backup = "PIT Backup", inspect = "Inspect"
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
                Text("Opens and closes a session to check that the phone answers. Sends no partition table and no firmware.")
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
                Text("Reads the phone's partition table into a file. Writes nothing to the phone.")
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
