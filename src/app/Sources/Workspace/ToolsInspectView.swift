// Tools › Firmware: package + PIT inspector (with or without Flash).
import AppKit
import SwiftUI
import UniformTypeIdentifiers

struct InspectToolView: View {
    @EnvironmentObject private var model: AppModel
    @State private var offlinePlan: Plan?
    @State private var pitTable: PitTable?
    @State private var pitError: String?
    @State private var targeted = false
    @State private var copiedPlan = false

    private var packages: LoadedPackages? { model.packages ?? model.toolsInspectPackages }

    var body: some View {
        Form {
            loadSection
            firmwareSection
            partitionsSection
            if let diff = model.toolsPitDiff, packages != nil {
                Section("PIT diff (loaded vs firmware)") {
                    PitDiffView(comparison: diff)
                }
            }
        }
        .formStyle(.grouped)
        .navigationTitle(L10n.inspect)
        .onChange(of: model.packages?.infos.count) { _ in reloadPitIfNeeded() }
        .onChange(of: model.toolsInspectPackages?.infos.count) { _ in reloadPitIfNeeded() }
        .onChange(of: model.inspectPitData?.count) { _ in reloadPitFromModel() }
    }

    private var loadSection: some View {
        Section {
            if model.packages != nil {
                Text("Using firmware verified in Flash. You can also load extra packages here without changing Flash.")
                    .font(.callout).foregroundStyle(.secondary)
            }
            HStack {
                Button("Open Package…") { openPackages() }
                    .disabled(model.toolsInspectLoading)
                if model.toolsInspectPackages != nil {
                    Button("Clear Inspect Packages") { model.clearInspectPackages() }
                }
            }
            if model.toolsInspectLoading { ProgressView("Verifying package…") }
            if let error = model.toolsInspectError {
                Text(error).foregroundStyle(.red).font(.callout)
            }
        } header: {
            Text("Packages")
        } footer: {
            Text("Inspect verifies MD5 and lists tar entries. Does not talk to the phone.")
        }
    }

    private var firmwareSection: some View {
        Section {
            if packages == nil {
                Text("No firmware loaded. Open a package or add files in Flash › Setup.")
                    .foregroundStyle(.secondary)
            }
            if let bl = packages?.infos.first(where: { $0.slot == .bl }), bl.bootloaderRevision >= 0 {
                LabeledContent("Bootloader revision (BL)", value: String(bl.bootloaderRevision))
            }
            ForEach(packages?.infos ?? []) { info in
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
            Text("Firmware contents")
        }
    }

    private var partitionsSection: some View {
        Section {
            LabeledContent(pitTable == nil ? "No PIT loaded" : model.inspectPitName) {
                HStack {
                    if packages != nil { Button("Firmware PIT") { loadEmbeddedPit() } }
                    if let data = model.devicePit?.bytes {
                        Button("PIT from Review") { apply(data, name: model.pitSource) }
                    }
                    Button("Open PIT File…") { openPit() }
                }
            }
            if let pitError { Text(pitError).foregroundStyle(.red).font(.callout) }
            if let table = pitTable {
                Text(AppModel.pitSummary(table)).font(.callout).foregroundStyle(.secondary)
                Table(table.entries) {
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
                .frame(minHeight: 220)
                .accessibilityLabel("Partition table")
            }
            if let plan = offlinePlan {
                DisclosureGroup("Offline plan (no phone access)") { PlanView(plan: plan) }
                HStack {
                    Button("Copy Plan Text") { copyPlan(plan) }
                    Button("Export Plan JSON…") { exportPlanJSON(plan) }
                    if copiedPlan { Text("Copied").font(.caption).foregroundStyle(.secondary) }
                }
            } else if pitTable != nil, packages == nil {
                Text("Load firmware to see how images map to this table.")
                    .foregroundStyle(.secondary)
            }
        } header: {
            Text("Partition table")
        } footer: {
            Text("Drop a .pit file here. After a PIT backup, Inspect opens automatically with the device table.")
        }
        .dropDestination(for: URL.self) { urls, _ in
            guard let url = urls.first else { return false }
            load(url)
            return true
        } isTargeted: { targeted = $0 }
    }

    private func openPackages() {
        let panel = NSOpenPanel()
        panel.canChooseDirectories = false
        panel.allowsMultipleSelection = true
        panel.message = "Choose one or more Samsung firmware archives (.tar.md5 or .tar)"
        guard panel.runModal() == .OK else { return }
        model.loadInspectPackages(paths: panel.urls.map(\.path))
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

    private func loadEmbeddedPit() {
        guard let packages else { return }
        switch PitTable.embedded(in: packages) {
        case let .success(found):
            apply(found.bytes, name: "Firmware: \(found.name)")
        case let .failure(error):
            pitError = error.message
        }
    }

    private func apply(_ data: Data, name: String) {
        model.inspectPitData = data
        model.inspectPitName = name
        switch PitTable.parse(data, packages: packages) {
        case let .success((table, plan)):
            pitTable = table
            offlinePlan = plan
            pitError = nil
            model.refreshToolsPitDiff(devicePit: data)
        case let .failure(error):
            pitTable = nil
            offlinePlan = nil
            pitError = error.message
            model.toolsPitDiff = nil
        }
    }

    private func reloadPitIfNeeded() {
        guard let data = model.inspectPitData, !data.isEmpty else { return }
        apply(data, name: model.inspectPitName)
    }

    private func reloadPitFromModel() {
        guard let data = model.inspectPitData else {
            pitTable = nil
            offlinePlan = nil
            return
        }
        apply(data, name: model.inspectPitName.isEmpty ? "PIT" : model.inspectPitName)
    }

    private func copyPlan(_ plan: Plan) {
        NSPasteboard.general.clearContents()
        NSPasteboard.general.setString(AppModel.planExportText(plan, pitName: model.inspectPitName),
                                       forType: .string)
        copiedPlan = true
        DispatchQueue.main.asyncAfter(deadline: .now() + 2) { copiedPlan = false }
    }

    private func exportPlanJSON(_ plan: Plan) {
        model.savePlanJSON(plan, pitName: model.inspectPitName, defaultFileName: "offline-plan.json")
    }
}