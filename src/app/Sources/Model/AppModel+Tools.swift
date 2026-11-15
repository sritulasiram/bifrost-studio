// Tools: offline inspect packages, PIT diff, navigation after device reads.
import AppKit
import Foundation
import UniformTypeIdentifiers

extension AppModel {
    /// Firmware available for Tools compare (Flash verification or Inspect-only load).
    var firmwarePackagesForTools: LoadedPackages? { packages ?? toolsInspectPackages }

    func slotGuess(for path: String) -> Slot {
        let name = (path as NSString).lastPathComponent.uppercased()
        if name.hasPrefix("BL_") { return .bl }
        if name.hasPrefix("AP_") || name.hasPrefix("PDA_") { return .ap }
        if name.hasPrefix("CP_") { return .cp }
        if name.hasPrefix("HOME_CSC_") { return .csc }
        if name.hasPrefix("CSC_") { return .csc }
        if name.hasPrefix("USERDATA_") { return .userdata }
        return .ap
    }

    func openInspect(with pit: Data, name: String) {
        inspectPitData = pit
        inspectPitName = name
        section = .inspect
        toolsPitDiff = nil
        refreshToolsPitDiff(devicePit: pit)
    }

    func ingestBackedUpPit(_ pit: Data, label: String) {
        openInspect(with: pit, name: "Device: \(label)")
    }

    func refreshToolsPitDiff(devicePit: Data) {
        toolsPitDiff = nil
        guard let packages = firmwarePackagesForTools else { return }
        switch PitTable.embedded(in: packages) {
        case let .success(found):
            switch PitComparison.compare(device: devicePit, firmware: found.bytes) {
            case let .success(diff): toolsPitDiff = diff
            case .failure: break
            }
        case .failure:
            break
        }
    }

    func compareToolsPitFile(_ url: URL) -> BifrostError? {
        guard firmwarePackagesForTools != nil else {
            return BifrostError(code: BifrostError.invalidArgument,
                                message: "Load firmware in Flash or Inspect first")
        }
        do {
            let data = try Data(contentsOf: url)
            guard data.count <= 1 << 20 else {
                return BifrostError(code: BifrostError.pitInvalid, message: "PIT file is larger than 1 MiB")
            }
            refreshToolsPitDiff(devicePit: data)
            inspectPitData = data
            inspectPitName = url.lastPathComponent
            return nil
        } catch {
            return BifrostError(code: BifrostError.io, message: error.localizedDescription)
        }
    }

    func loadInspectPackages(paths: [String]) {
        guard let local, !paths.isEmpty else { return }
        toolsInspectLoading = true
        toolsInspectError = nil
        let mapped = paths.map { (slotGuess(for: $0), $0) }
        work.async { [weak self] in
            let result = local.loadPackages(mapped) { _ in }
            DispatchQueue.main.async {
                guard let self else { return }
                self.toolsInspectLoading = false
                switch result {
                case let .success(loaded):
                    self.toolsInspectPackages = loaded
                    if let data = self.inspectPitData {
                        self.refreshToolsPitDiff(devicePit: data)
                    }
                case let .failure(error):
                    self.toolsInspectPackages = nil
                    self.toolsInspectError = error.message
                }
            }
        }
    }

    func clearInspectPackages() {
        toolsInspectPackages = nil
        toolsInspectError = nil
        toolsPitDiff = nil
    }

    /// Printable probe / read-pit session line for support paste.
    static func sessionReport(_ result: JobResult) -> String {
        let s = result.session
        var lines = [
            "Session ACK: 0x\(String(format: "%x", s.ack))",
            "Protocol: \(s.protocolVersion)",
            "Packet size: \(s.packetSize)",
            "Parts per sequence: \(s.partsPerSequence)",
            "Compressed transfer: \(s.compressed ? "yes" : "no")",
            "Bytes transferred: \(result.bytesTransferred)",
        ]
        if let err = result.error {
            lines.append("Error: \(err.codeName) — \(err.message)")
        }
        return lines.joined(separator: "\n")
    }

    func savePlanJSON(_ plan: Plan, pitName: String, defaultFileName: String) {
        let panel = NSSavePanel()
        panel.allowedContentTypes = [.json]
        panel.nameFieldStringValue = defaultFileName
        guard panel.runModal() == .OK, let url = panel.url else { return }
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
        guard let data = try? encoder.encode(plan) else { return }
        do {
            try data.write(to: url, options: .atomic)
        } catch {
            actionError = BifrostError(code: BifrostError.io,
                                       message: "Couldn't save the plan: \(error.localizedDescription)")
        }
    }

    static func planExportText(_ plan: Plan, pitName: String) -> String {
        var lines = ["Bifrost Studio offline plan", "PIT: \(pitName)", "Model: \(plan.detectedModel)",
                     "Flashable: \(plan.flashable)", ""]
        lines += plan.files.map { "  \($0.slot.title) \($0.entry) → \($0.partition) (\($0.partitionID))" }
        if !plan.refusals.isEmpty {
            lines += ["", "Refusals:"] + plan.refusals.map { "  \($0)" }
        }
        if !plan.warnings.isEmpty {
            lines += ["", "Warnings:"] + plan.warnings.map { "  \($0.message)" }
        }
        return lines.joined(separator: "\n")
    }

    static func pitSummary(_ table: PitTable) -> String {
        "PIT: \(table.entries.count) entries, \(formatBytes(UInt64(table.bytes.count))) on disk"
    }
}