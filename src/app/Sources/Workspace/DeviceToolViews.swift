// Download Mode tool forms (Probe, PIT backup).
import AppKit
import SwiftUI
import UniformTypeIdentifiers

struct PhoneRow: View {
    @EnvironmentObject private var model: AppModel

    var body: some View {
        LabeledContent("Phone (Download Mode)") {
            if let id = model.selectedDevice {
                Label(id.label, systemImage: "iphone")
            } else {
                Text("None in Download Mode").foregroundStyle(.secondary)
            }
        }
    }
}

struct ToolResult: View {
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

struct ProbeToolView: View {
    @EnvironmentObject private var model: AppModel
    @State private var copied = false

    var body: some View {
        Form {
            Section {
                PhoneRow()
                Button("Run Probe") { model.probe() }
                    .disabled(!model.canStartJob)
                ToolResult(backup: false)
                if let job = model.job, case .probe = job.purpose, let result = job.result, !job.running {
                    Button("Copy Session Info") {
                        NSPasteboard.general.clearContents()
                        NSPasteboard.general.setString(AppModel.sessionReport(result), forType: .string)
                        copied = true
                        DispatchQueue.main.asyncAfter(deadline: .now() + 2) { copied = false }
                    }
                    if copied { Text("Copied").font(.caption).foregroundStyle(.secondary) }
                }
            } header: {
                Text("Probe")
            } footer: {
                Text("Opens and closes a session to check that the phone answers. Sends no partition table and no firmware. It uses up this Download Mode session: restart the phone into Download Mode before flashing.")
            }
        }
        .formStyle(.grouped)
    }
}

struct PitBackupToolView: View {
    @EnvironmentObject private var model: AppModel
    @State private var pitCompareError: String?

    var body: some View {
        Form {
            Section {
                PhoneRow()
                Button("Back Up PIT…") { backup() }
                    .disabled(!model.canStartJob)
                if model.firmwarePackagesForTools != nil {
                    Button("Back Up and Compare with Firmware") { backup(andCompare: true) }
                        .disabled(!model.canStartJob)
                }
                ToolResult(backup: true)
                if let url = model.lastBackup {
                    LabeledContent {
                        Button("Show in Finder") { NSWorkspace.shared.activateFileViewerSelecting([url]) }
                    } label: {
                        Label("Saved \(url.lastPathComponent)", systemImage: "checkmark.circle.fill")
                            .foregroundStyle(.green)
                    }
                    Button("Open in Inspect") {
                        if let data = try? Data(contentsOf: url) {
                            model.openInspect(with: data, name: url.lastPathComponent)
                        }
                    }
                    if model.firmwarePackagesForTools != nil {
                        Button("Compare file with firmware PIT") {
                            pitCompareError = nil
                            if let error = model.compareToolsPitFile(url) {
                                pitCompareError = error.message
                            }
                        }
                    }
                }
                if let pitCompareError {
                    Text(pitCompareError).font(.callout).foregroundStyle(.red)
                }
            } header: {
                Text("PIT backup")
            } footer: {
                Text("Reads the phone's partition table. After a successful backup, Inspect opens when firmware is loaded in Flash or Inspect.")
            }
            if let diff = model.toolsPitDiff, model.firmwarePackagesForTools != nil {
                Section("PIT diff (device vs firmware)") {
                    PitDiffView(comparison: diff)
                }
            }
            if model.firmwarePackagesForTools == nil {
                Section {
                    Text("Load firmware in Flash or Inspect to compare the device PIT against the CSC embedded table.")
                        .font(.callout).foregroundStyle(.secondary)
                }
            }
        }
        .formStyle(.grouped)
    }

    private func backup(andCompare: Bool = false) {
        let panel = NSSavePanel()
        panel.nameFieldStringValue = "device.pit"
        panel.allowedContentTypes = [UTType(filenameExtension: "pit") ?? .data]
        guard panel.runModal() == .OK, let url = panel.url else { return }
        model.backupPit(to: url)
        _ = andCompare
    }
}