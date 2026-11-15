// History persistence and export (keeps AppModel.swift smaller).
import Foundation

struct FailureCluster: Identifiable, Hashable {
    let id: String
    let codeName: String
    let model: String
    let count: Int
    let entryIDs: [UUID]
}

extension AppModel {
    /// Failures sharing the same error code and model (two or more entries).
    var failureClusters: [FailureCluster] {
        let failed = history.filter { !$0.succeeded }
        let grouped = Dictionary(grouping: failed) { entry in
            "\(entry.error?.codeName ?? "unknown")|\(entry.model.isEmpty ? "?" : entry.model)"
        }
        return grouped.compactMap { key, entries in
            guard entries.count >= 2 else { return nil }
            let sample = entries[0]
            return FailureCluster(
                id: key,
                codeName: sample.error?.codeName ?? "unknown",
                model: sample.model.isEmpty ? "Unknown model" : sample.model,
                count: entries.count,
                entryIDs: entries.map(\.id))
        }
        .sorted { $0.count > $1.count }
    }

    func similarFailures(to entry: HistoryEntry) -> [HistoryEntry] {
        guard let code = entry.error?.codeName, !entry.succeeded else { return [] }
        return history.filter {
            $0.id != entry.id && !$0.succeeded && $0.error?.codeName == code &&
                ($0.model == entry.model || entry.model.isEmpty || $0.model.isEmpty)
        }
    }

    var historyDirectory: URL? {
        FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask).first?
            .appendingPathComponent("Bifrost Studio/History", isDirectory: true)
    }

    var historyFolder: URL? { historyDirectory }

    func historyURL(_ entry: HistoryEntry) -> URL {
        (historyDirectory ?? FileManager.default.temporaryDirectory)
            .appendingPathComponent("\(entry.id.uuidString).json")
    }

    func applyRetention() {
        let limit = Prefs.historyKeep
        guard limit > 0, history.count > limit else { return }
        for entry in history[limit...] { try? FileManager.default.removeItem(at: historyURL(entry)) }
        history = Array(history.prefix(limit))
    }

    func deleteHistory(_ ids: Set<UUID>) {
        for entry in history where ids.contains(entry.id) {
            try? FileManager.default.removeItem(at: historyURL(entry))
        }
        history.removeAll { ids.contains($0.id) }
        if let selected = selectedHistoryID, ids.contains(selected) { selectedHistoryID = nil }
    }

    func clearHistory() { deleteHistory(Set(history.map(\.id))) }

    func exportJSON(_ entry: HistoryEntry) -> Data? {
        let encoder = JSONEncoder()
        encoder.dateEncodingStrategy = .iso8601
        encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
        return try? encoder.encode(entry)
    }

    func exportText(_ entry: HistoryEntry) -> String {
        var lines = ["Bifrost Studio \(entry.kind.title) — \(entry.date)",
                     "Device: \(entry.device)", "Model: \(entry.model)",
                     "Result: \(entry.error.map { "FAILED (\($0.codeName), \($0.dataState)): \($0.message)" } ?? "OK")",
                     String(format: "Session ACK: 0x%x, protocol %u", entry.session.ack, entry.session.protocolVersion),
                     "Bytes transferred: \(entry.bytesTransferred)",
                     "Duration: \(entry.duration.map { String(format: "%.0f s", $0) } ?? "unknown")", "", "Files:"]
        lines += entry.files.map { "  \($0)" }
        lines += ["", "Log:"]
        lines += entry.log.map { "  \($0.message)" }
        return lines.joined(separator: "\n")
    }

    func loadHistory() {
        guard let directory = historyDirectory,
              let files = try? FileManager.default.contentsOfDirectory(at: directory,
                                                                       includingPropertiesForKeys: nil)
        else { return }
        let decoder = JSONDecoder()
        decoder.dateDecodingStrategy = .iso8601
        history = files.filter { $0.pathExtension == "json" }
            .compactMap { try? decoder.decode(HistoryEntry.self, from: Data(contentsOf: $0)) }
            .sorted { $0.date > $1.date }
        applyRetention()
    }

    func record(_ state: JobState) {
        guard let result = state.result else { return }
        let entry = HistoryEntry(
            date: state.startedAt, kind: state.kind,
            device: state.device?.label ?? "", model: state.plan?.detectedModel ?? "",
            files: state.plan?.files.map { "\($0.entry) → \($0.partition)" } ?? [],
            error: result.error, bytesTransferred: result.bytesTransferred,
            session: result.session, log: Array(state.log.suffix(2000)),
            duration: Date().timeIntervalSince(state.startedAt),
            packagePaths: state.packagePaths.isEmpty ? nil : state.packagePaths)
        history.insert(entry, at: 0)
        guard let directory = historyDirectory else { return }
        try? FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        let encoder = JSONEncoder()
        encoder.dateEncodingStrategy = .iso8601
        encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
        if let data = try? encoder.encode(entry) {
            try? data.write(to: historyURL(entry))
        }
        applyRetention()
    }
}