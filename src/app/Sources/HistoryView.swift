// History: every device job, with filters, a summary, export and retry.
import AppKit
import SwiftUI
import UniformTypeIdentifiers

private enum HistoryFilter: String, CaseIterable, Identifiable {
    case all = "All", flashes = "Flashes", reads = "Reads", failed = "Failed"
    var id: String { rawValue }

    func includes(_ entry: HistoryEntry) -> Bool {
        switch self {
        case .all: return true
        case .flashes: return entry.kind == .flash
        case .reads: return entry.kind != .flash
        case .failed: return !entry.succeeded
        }
    }
}

private extension JobKind {
    var symbol: String {
        switch self {
        case .flash: return "bolt.fill"
        case .probe: return "waveform.path.ecg"
        case .readPit: return "tablecells"
        }
    }
}

private func formatDuration(_ seconds: Double) -> String {
    let formatter = DateComponentsFormatter()
    formatter.allowedUnits = seconds >= 3600 ? [.hour, .minute, .second] : [.minute, .second]
    formatter.unitsStyle = .abbreviated
    return formatter.string(from: max(1, seconds.rounded())) ?? "\(Int(seconds)) s"
}

struct HistoryView: View {
    @EnvironmentObject private var model: AppModel
    @State private var filter: HistoryFilter = .all
    @State private var search = ""
    @State private var confirmDelete = false
    @State private var copied = false

    private var selected: HistoryEntry? { model.history.first { $0.id == model.selectedHistoryID } }

    private var days: [(day: Date, entries: [HistoryEntry])] {
        let groups = Dictionary(grouping: shown) { Calendar.current.startOfDay(for: $0.date) }
        return groups.keys.sorted(by: >).map { ($0, groups[$0]!.sorted { $0.date > $1.date }) }
    }

    private func dayTitle(_ day: Date) -> String {
        if Calendar.current.isDateInToday(day) { return "Today" }
        if Calendar.current.isDateInYesterday(day) { return "Yesterday" }
        return day.formatted(date: .complete, time: .omitted)
    }

    private var shown: [HistoryEntry] {
        model.history.filter { entry in
            guard filter.includes(entry) else { return false }
            let query = search.trimmingCharacters(in: .whitespaces).lowercased()
            if query.isEmpty { return true }
            let haystack = ([entry.kind.title, entry.device, entry.model, entry.error?.message ?? "",
                             entry.error?.codeName ?? ""] + entry.files).joined(separator: " ").lowercased()
            return haystack.contains(query)
        }
    }

    var body: some View {
        if model.history.isEmpty {
            VStack(spacing: 8) {
                Image(systemName: "clock.arrow.circlepath").font(.largeTitle).foregroundStyle(.secondary)
                    .accessibilityHidden(true)
                Text("No history yet").font(.headline)
                Text("Flashes, probes and partition-table reads appear here with their logs.")
                    .foregroundStyle(.secondary)
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity)
            .navigationTitle("History")
        } else {
            HSplitView {
                list.frame(minWidth: 240, idealWidth: 270, maxWidth: 340)
                detail.frame(minWidth: 380)
            }
            .navigationTitle("History")
            .onAppear {
                if model.selectedHistoryID == nil || selected == nil {
                    model.selectedHistoryID = shown.first?.id
                }
            }
            .searchable(text: $search, placement: .toolbar, prompt: "Search history")
            .toolbar { toolbarItems }
        }
    }

    @ToolbarContentBuilder
    private var toolbarItems: some ToolbarContent {
        ToolbarItemGroup {
            Menu {
                Picker("Show", selection: $filter) {
                    ForEach(HistoryFilter.allCases) { Text($0.rawValue).tag($0) }
                }
                .pickerStyle(.inline)
            } label: {
                Label("Filter: \(filter.rawValue)", systemImage: "line.3.horizontal.decrease.circle")
            }
            .help("Filter the list")

            if let entry = selected {
                if entry.kind == .flash {
                    Button { model.retry(entry) } label: {
                        Label("Retry", systemImage: "arrow.clockwise")
                    }
                    .disabled(!model.canRetry(entry) || (model.job?.running ?? false))
                    .help(model.canRetry(entry) ? "Opens Flash with these files. You still review and confirm."
                          : "The firmware files are no longer where they were.")
                }
                Menu {
                    Button("Log as Text…") { exportText(entry) }
                    Button("Entry as JSON…") { exportJSON(entry) }
                    Button("Copy for Bug Report") { copyReport(entry) }
                    Divider()
                    Button("Show in Finder") {
                        NSWorkspace.shared.activateFileViewerSelecting([model.historyURL(entry)])
                    }
                } label: {
                    Label(copied ? "Copied" : "Export", systemImage: "square.and.arrow.up")
                }
                .help("Export or copy this entry")
                Button(role: .destructive) { confirmDelete = true } label: {
                    Label("Delete", systemImage: "trash")
                }
                .help("Delete this entry")
            }
        }
    }

    private func copyReport(_ entry: HistoryEntry) {
        let text = Diagnostics.text(model) + "\n\n" + model.exportText(entry)
        NSPasteboard.general.clearContents()
        NSPasteboard.general.setString(text, forType: .string)
        copied = true
        DispatchQueue.main.asyncAfter(deadline: .now() + 2) { copied = false }
    }

    private var list: some View {
        List(selection: $model.selectedHistoryID) {
            ForEach(days, id: \.day) { group in
                Section(dayTitle(group.day)) {
                    ForEach(group.entries) { entry in
                        HistoryRow(entry: entry)
                            .contextMenu {
                                Button("Export Log…") { exportText(entry) }
                                Button("Delete", role: .destructive) { model.deleteHistory([entry.id]) }
                            }
                    }
                }
            }
        }
        .overlay {
            if shown.isEmpty { Text("Nothing matches").foregroundStyle(.secondary) }
        }
    }

    @ViewBuilder
    private var detail: some View {
        if let entry = model.history.first(where: { $0.id == model.selectedHistoryID }) {
            HistoryDetail(entry: entry)
                .confirmationDialog("Delete this entry?", isPresented: $confirmDelete) {
                    Button("Delete", role: .destructive) { model.deleteHistory([entry.id]) }
                } message: {
                    Text("The log is removed from this Mac. This cannot be undone.")
                }
        } else {
            Text("Select an entry").foregroundStyle(.secondary)
                .frame(maxWidth: .infinity, maxHeight: .infinity)
        }
    }

    private func exportText(_ entry: HistoryEntry) {
        save(name: "bifrost-\(entry.kind.title.lowercased())-log.txt", type: .plainText,
             data: Data(model.exportText(entry).utf8))
    }

    private func exportJSON(_ entry: HistoryEntry) {
        guard let data = model.exportJSON(entry) else { return }
        save(name: "bifrost-\(entry.kind.title.lowercased())-log.json", type: .json, data: data)
    }

    private func save(name: String, type: UTType, data: Data) {
        let panel = NSSavePanel()
        panel.nameFieldStringValue = name
        panel.allowedContentTypes = [type]
        guard panel.runModal() == .OK, let url = panel.url else { return }
        do { try data.write(to: url, options: .atomic) } catch {
            model.actionError = BifrostError(code: BifrostError.io,
                                             message: "Couldn't save the file: \(error.localizedDescription)")
        }
    }
}

private struct HistoryRow: View {
    let entry: HistoryEntry

    var body: some View {
        HStack {
            Image(systemName: entry.succeeded ? "checkmark.circle.fill" : "xmark.circle.fill")
                .foregroundStyle(entry.succeeded ? .green : .red)
                .accessibilityLabel(entry.succeeded ? "Succeeded" : "Failed")
            VStack(alignment: .leading, spacing: 2) {
                Text(entry.model.isEmpty ? entry.kind.title : "\(entry.kind.title) · \(entry.model)").bold()
                Text(entry.date.formatted(date: .omitted, time: .shortened))
                    .font(.caption).foregroundStyle(.secondary)
            }
            Spacer()
            if let duration = entry.duration {
                Text(formatDuration(duration)).font(.caption).foregroundStyle(.secondary).monospacedDigit()
            }
        }
        .tag(entry.id)
    }
}

private struct HistoryDetail: View {
    @EnvironmentObject private var model: AppModel
    let entry: HistoryEntry

    var body: some View {
        Form {
            Section { header }
            if let error = entry.error {
                Section { RecoveryCard(error: error, rebooted: false) }
            }
            Section("Summary") { summary }
            if !entry.files.isEmpty {
                Section("Written") {
                    ForEach(entry.files, id: \.self) { Text($0).font(.callout.monospaced()) }
                }
            }
            Section { LogDisclosure(entry: entry) }
        }
        .formStyle(.grouped)
        .textSelection(.enabled)
        .id(entry.id)
    }

    private var header: some View {
        HStack {
            Image(systemName: entry.kind.symbol).font(.title2).accessibilityHidden(true)
            VStack(alignment: .leading) {
                Text(entry.model.isEmpty ? entry.kind.title : "\(entry.kind.title) · \(entry.model)")
                    .font(.title3.bold())
                Text(entry.date.formatted(date: .complete, time: .standard))
                    .font(.caption).foregroundStyle(.secondary)
            }
            Spacer()
            Label(entry.succeeded ? "Succeeded" : "Failed",
                  systemImage: entry.succeeded ? "checkmark.circle.fill" : "xmark.circle.fill")
                .foregroundStyle(entry.succeeded ? .green : .red)
                .font(.headline)
        }
    }

    @ViewBuilder
    private var summary: some View {
        LabeledContent("Phone", value: entry.device.isEmpty ? "Unknown" : entry.device)
        if let duration = entry.duration {
            LabeledContent("Duration", value: formatDuration(duration))
        }
        if entry.bytesTransferred > 0 {
            LabeledContent("Transferred", value: formatBytes(entry.bytesTransferred))
        }
        if entry.session.packetSize > 0 {
            LabeledContent("Session", value: String(format: "Protocol %u · ACK 0x%x · %@ parts",
                                                    entry.session.protocolVersion, entry.session.ack,
                                                    formatBytes(UInt64(entry.session.packetSize))))
        }
    }
}

/// The log, collapsed unless the job failed.
private struct LogDisclosure: View {
    let entry: HistoryEntry
    @State private var open: Bool

    init(entry: HistoryEntry) {
        self.entry = entry
        _open = State(initialValue: !entry.succeeded)
    }

    var body: some View {
        DisclosureGroup("Log (\(entry.log.count) lines)", isExpanded: $open) {
            VStack(alignment: .leading, spacing: 2) {
                ForEach(entry.log) { line in
                    Text(line.message).font(.caption.monospaced())
                        .foregroundStyle(line.isError ? .red : line.isWarning ? .orange : .primary)
                        .frame(maxWidth: .infinity, alignment: .leading)
                }
            }
            .padding(.top, 4)
        }
    }
}
