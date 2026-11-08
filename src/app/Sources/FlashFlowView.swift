// The flash flow: Setup, Review & Confirm, Flash. Standard grouped forms throughout.
import AppKit
import SwiftUI
import UniformTypeIdentifiers

struct FlashFlowView: View {
    @EnvironmentObject private var model: AppModel

    var body: some View {
        Group {
            switch model.step {
            case .setup: SetupStep()
            case .review: ReviewStep()
            case .flash: FlashProgressStep()
            }
        }
        .navigationTitle("Flash")
        .navigationSubtitle("Step \(model.step.rawValue + 1) of \(FlashStep.allCases.count) · \(model.step.title)")
    }
}

private extension AppModel {
    /// Whether a step's prerequisites are met (earlier steps are complete).
    func reachable(_ step: FlashStep) -> Bool {
        if job?.running ?? false { return step == .flash }
        switch step {
        case .setup: return true
        case .review: return selectedDevice != nil && packages != nil
        case .flash: return job?.kind == .flash
        }
    }
}

/// The bar under a form: a hint on the left, the actions on the trailing edge.
private struct ActionBar<Actions: View>: View {
    let hint: String
    @ViewBuilder let actions: Actions

    var body: some View {
        VStack(spacing: 0) {
            Divider()
            HStack {
                Text(hint).font(.callout).foregroundStyle(.secondary)
                Spacer()
                actions
            }
            .padding(.horizontal, 20).padding(.vertical, 12)
        }
        .background(.bar)
    }
}

// MARK: Setup

private struct SetupStep: View {
    @EnvironmentObject private var model: AppModel
    @AppStorage(Prefs.showGuide) private var showGuide = true

    private var phoneReady: Bool {
        guard let id = model.selectedDevice else { return false }
        return model.devices.contains { $0.id == id && $0.inDownloadMode }
    }
    private var infos: [PackageInfo] { model.packages?.infos ?? [] }
    private var models: Set<String> { Set(infos.map(\.model).filter { !$0.isEmpty }) }

    private var next: String {
        if !phoneReady {
            if model.devices.filter(\.inDownloadMode).count > 1 { return "Choose a phone under Devices in the sidebar." }
            return model.devices.isEmpty ? "Connect a phone in Download Mode." : "Restart the phone into Download Mode."
        }
        if model.orderedPaths.isEmpty { return "Add the firmware files." }
        if model.loadingProgress != nil { return "Waiting for verification to finish." }
        if model.packages == nil { return "Fix the firmware problem shown above." }
        return "Ready to review."
    }

    var body: some View {
        Form {
            Section("Phone") {
                LabeledContent("Status") {
                    if phoneReady {
                        Label("Ready in Download Mode", systemImage: "checkmark.circle.fill").foregroundStyle(.green)
                    } else {
                        Label(model.devices.isEmpty ? "No phone found" : "Not in Download Mode",
                              systemImage: "iphone.slash").foregroundStyle(.secondary)
                    }
                }
                if phoneReady, let id = model.selectedDevice {
                    LabeledContent("Device", value: id.label)
                }
                DisclosureGroup("How do I start the phone in Download Mode?", isExpanded: $showGuide) {
                    DownloadModeGuide().padding(.vertical, 4)
                }
            }

            Section {
                ForEach(Slot.allCases) { slot in SlotRow(slot: slot) }
                if let progress = model.loadingProgress {
                    LabeledContent {
                        Button("Cancel") { model.cancelVerification() }
                    } label: {
                        VStack(alignment: .leading, spacing: 4) {
                            Text(progress.entry.isEmpty ? "Verifying…" : "Verifying \(progress.entry)")
                                .lineLimit(1)
                            ProgressView(value: progress.fileFraction)
                                .accessibilityLabel("Verifying \(progress.entry)")
                        }
                    }
                }
                if let error = model.firmwareError {
                    RecoveryCard(error: error, rebooted: false)
                    Button("Verify Again") { model.verifyFirmware() }
                        .disabled(model.orderedPaths.isEmpty || model.loadingProgress != nil)
                }
            } header: {
                Text("Firmware")
            } footer: {
                Text("Use files from one release for this exact model. Drop a file on a row or choose it. Files are checked automatically; .tar.md5 files against their MD5.")
            }

            if !infos.isEmpty {
                Section("Verified") {
                    ForEach(infos) { info in PackageRow(info: info) }
                    if models.count > 1 {
                        Label("The files are for different models", systemImage: "xmark.octagon.fill")
                            .foregroundStyle(.red)
                    }
                }
            }
        }
        .formStyle(.grouped)
        .safeAreaInset(edge: .bottom, spacing: 0) {
            ActionBar(hint: next) {
                Button("Continue") { model.step = .review }
                    .buttonStyle(.borderedProminent)
                    .keyboardShortcut(.defaultAction)
                    .disabled(!model.reachable(.review))
            }
        }
    }
}

private struct SlotRow: View {
    @EnvironmentObject private var model: AppModel
    let slot: Slot
    @State private var targeted = false

    var body: some View {
        LabeledContent {
            HStack {
                Text(model.paths[slot].map { ($0 as NSString).lastPathComponent } ?? "None")
                    .foregroundStyle(model.paths[slot] == nil ? .secondary : .primary)
                    .lineLimit(1).truncationMode(.middle)
                Button("Choose…") { choose() }
                if model.paths[slot] != nil {
                    Button("Clear") { model.setPath(nil, for: slot) }
                }
            }
        } label: {
            VStack(alignment: .leading, spacing: 2) {
                Text(slot.title)
                Text(slot.detail).font(.caption).foregroundStyle(.secondary)
            }
        }
        .listRowBackground(targeted ? Color.accentColor.opacity(0.15) : nil)
        .dropDestination(for: URL.self) { urls, _ in
            guard let url = urls.first else { return false }
            model.setPath(url.path, for: slot)
            return true
        } isTargeted: { targeted = $0 }
    }

    private func choose() {
        let panel = NSOpenPanel()
        panel.canChooseDirectories = false
        panel.allowsMultipleSelection = false
        panel.message = "Choose the \(slot.title) file (.tar.md5 or .tar)"
        if panel.runModal() == .OK, let url = panel.url {
            model.setPath(url.path, for: slot)
        }
    }
}

private struct PackageRow: View {
    let info: PackageInfo

    var body: some View {
        LabeledContent {
            if let badge = info.kind.badge {
                Text(badge).foregroundStyle(info.kind == .homeCsc ? .green : .orange)
            }
        } label: {
            HStack(alignment: .firstTextBaseline) {
                Image(systemName: info.checksumValid ? "checkmark.seal.fill" : "seal")
                    .foregroundStyle(info.checksumValid ? .green : .orange)
                    .accessibilityLabel(info.checksumValid ? "MD5 verified" : "No MD5 checksum")
                VStack(alignment: .leading, spacing: 2) {
                    Text("\(info.slot.title): \(info.fileName)").lineLimit(1).truncationMode(.middle)
                    Text((info.checksumValid ? "MD5 verified" : "Plain .tar: no checksum")
                         + (info.model.isEmpty && info.build.isEmpty ? ""
                            : " · \(info.model.isEmpty ? "?" : info.model) · \(info.build)"))
                        .font(.caption).foregroundStyle(.secondary)
                }
            }
        }
        .accessibilityElement(children: .combine)
    }
}

private struct DownloadModeGuide: View {
    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            Text("Power the phone off, then hold these keys while connecting it, and press Volume Up at the warning screen:")
            Label("Home button: Volume Down + Home + Power", systemImage: "circle.fill")
            Label("Bixby: Volume Down + Bixby + Power", systemImage: "circle.fill")
            Label("No Home or Bixby button: Volume Up + Volume Down while plugging in the cable", systemImage: "circle.fill")
            Text("Use a data cable plugged directly into this Mac, and quit other USB tools.")
                .foregroundStyle(.secondary)
        }
        .font(.callout)
        .labelStyle(BulletLabelStyle())
    }
}

private struct BulletLabelStyle: LabelStyle {
    func makeBody(configuration: Configuration) -> some View {
        HStack(alignment: .firstTextBaseline, spacing: 6) {
            Text("•")
            configuration.title
        }
    }
}

// MARK: Review & Confirm

/// What must be true before the Flash button is enabled.
@MainActor private struct ConfirmState {
    let model: AppModel
    let plan: Plan

    var modelKnown: Bool { !plan.detectedModel.isEmpty }
    var erases: Bool { plan.requiresAcknowledgement || plan.erasesUserData }
    var ready: Bool {
        (modelKnown || model.acknowledgeUnknownModel)
            && (!plan.requiresAcknowledgement || model.acceptDataErase)
            && model.canStartJob && !model.handshakeDone
    }
    var storedBytes: UInt64 { plan.files.reduce(0) { $0 + $1.size } }
}

private struct ReviewStep: View {
    @EnvironmentObject private var model: AppModel
    @State private var confirmFlash = false

    private var state: ConfirmState? { model.plan.map { ConfirmState(model: model, plan: $0) } }
    private var canConfirm: Bool { (model.plan?.flashable ?? false) && model.devicePit != nil }

    var body: some View {
        Form {
            Section {
                LabeledContent("Planned with", value: model.pitSource.isEmpty ? "Not yet" : model.pitSource)
                HStack {
                    Button("Use the Firmware's PIT") { model.reviewFromFirmware() }
                        .disabled(model.packages == nil)
                    Button("Open PIT File…") { openPit() }
                }
            } header: {
                Text("Partition table")
            } footer: {
                Text("Every image is mapped to a partition without talking to the phone. At flash time the phone's own PIT is checked, and nothing is sent if it differs. Anything ambiguous is refused.")
            }

            if let error = model.reviewError {
                Section { RecoveryCard(error: error, rebooted: false) }
            }

            if let plan = model.plan, let state {
                Section("Plan") { PlanView(plan: plan) }
                if canConfirm {
                    Section("You are about to flash") {
                        LabeledContent("Phone", value: model.selectedDevice?.label ?? "?")
                        LabeledContent("Writes", value: "\(plan.files.count) images, \(formatBytes(state.storedBytes))")
                        LabeledContent("Partitions", value: plan.files.map(\.partition).joined(separator: ", "))
                        LabeledContent("User data") {
                            Text(state.erases ? "Erased" : "Kept")
                                .foregroundStyle(state.erases ? .red : .green)
                        }
                    }
                    Section("Confirm") {
                        if !state.modelKnown {
                            Toggle(isOn: $model.acknowledgeUnknownModel) {
                                Text("The model could not be detected from the file names. I have checked this firmware is for this exact phone.")
                            }
                        }
                        if plan.requiresAcknowledgement {
                            Toggle("I understand this erases all data on the phone", isOn: $model.acceptDataErase)
                            ForEach(plan.warnings.filter(\.requiresAcknowledgement)) { w in
                                Text(w.message).font(.callout).foregroundStyle(.orange)
                            }
                        }
                        Toggle("Restart the phone when finished", isOn: $model.rebootWhenDone)
                    }
                    if model.handshakeDone { Section { FreshSessionNotice() } }
                }
            }
        }
        .formStyle(.grouped)
        .onAppear {
            if model.plan == nil, model.reviewError == nil { model.reviewFromFirmware() }
        }
        .safeAreaInset(edge: .bottom, spacing: 0) {
            ActionBar(hint: model.plan?.flashable == false ? "The plan was refused. Nothing will be flashed." : "") {
                Button("Back") { model.step = .setup }
                if canConfirm, let plan = model.plan, let state {
                    // Deliberately no keyboard shortcut.
                    Button(state.erases ? "Flash…" : "Flash") {
                        if state.erases { confirmFlash = true } else { model.startFlash(detectedModel: plan.detectedModel) }
                    }
                    .buttonStyle(.borderedProminent)
                    .disabled(!state.ready)
                }
            }
        }
        .alert("Erase and flash \(model.selectedDevice?.label ?? "this phone")?", isPresented: $confirmFlash) {
            Button("Cancel", role: .cancel) {}
            Button("Erase and Flash", role: .destructive) {
                model.startFlash(detectedModel: model.plan?.detectedModel ?? "")
            }
        } message: {
            Text("All data on the phone will be erased. Don't unplug the phone until the flash finishes.")
        }
    }

    private func openPit() {
        let panel = NSOpenPanel()
        panel.canChooseDirectories = false
        panel.allowsMultipleSelection = false
        panel.message = "Choose a PIT file for this phone (for example a PIT Backup from Tools)"
        if panel.runModal() == .OK, let url = panel.url { model.reviewFromFile(url) }
    }
}

/// A flash must be the first session after the phone enters Download Mode.
private struct FreshSessionNotice: View {
    @EnvironmentObject private var model: AppModel

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            Label("Restart the phone into Download Mode before flashing", systemImage: "arrow.clockwise.circle")
                .bold().foregroundStyle(.orange)
            Text("The phone was already used in this Download Mode session (a probe, PIT backup or earlier attempt). A flash has to be the phone's first session: hold the buttons to restart it, enter Download Mode again, then flash. Bifrost Studio notices the restart by itself.")
                .font(.callout).foregroundStyle(.secondary)
            Button("I Restarted It into Download Mode") { model.phoneWasRestarted() }
        }
    }
}

// MARK: ③ Flash

private struct FlashProgressStep: View {
    @EnvironmentObject private var model: AppModel
    @State private var confirmStop = false

    var body: some View {
        Form {
            if let job = model.job, job.kind == .flash {
              Section(title(job)) {
                if job.running {
                    Label("Don't unplug the phone. The flash continues in the background if you close this window.",
                          systemImage: "exclamationmark.triangle.fill")
                        .foregroundStyle(.orange)
                }
                LabeledContent("Progress", value: "\(Int(job.progress.fraction * 100))% · \(formatBytes(job.progress.totalDone)) of \(formatBytes(job.progress.total))")
                    .monospacedDigit()
                ProgressView(value: job.progress.fraction)
                .accessibilityValue("\(Int(job.progress.fraction * 100)) percent")
                if !job.progress.entry.isEmpty, job.running {
                    ProgressView(value: job.progress.fileFraction) {
                        Text("Writing \(partitionName(for: job.progress.entry)) · \(job.progress.entry)").font(.callout)
                    }
                }
                if job.running, job.bytesPerSecond > 0 {
                    Text("\(formatBytes(UInt64(job.bytesPerSecond)))/s"
                         + (job.secondsRemaining.map { " · about \(Int($0 / 60)) min \(Int($0) % 60) s left" } ?? ""))
                        .font(.callout).foregroundStyle(.secondary).monospacedDigit()
                }
                if job.running {
                    HStack {
                        Button("Cancel") { model.cancel(afterSequence: false) }
                            .help("Honoured only before any data is sent.")
                        Button("Stop After This Sequence…") { confirmStop = true }
                            .help("Stops at the next safe point. The phone is left partially written.")
                    }
                }
                if let result = job.result {
                    if let error = result.error {
                        RecoveryCard(error: error, rebooted: result.rebooted)
                    } else {
                        Label(result.rebooted ? "Flash complete. The phone is restarting."
                                              : "Flash complete. The phone stays in Download Mode.",
                              systemImage: "checkmark.circle.fill")
                            .font(.headline).foregroundStyle(.green)
                    }
                    HStack {
                        Button("Flash Another") { model.resetFlow() }
                            .buttonStyle(.borderedProminent)
                        Button("Open in History") {
                            model.selectedHistoryID = model.history.first?.id
                            model.section = .history
                        }
                    }
                }
              }
              Section {
                DisclosureGroup("Log (\(job.log.count) lines)", isExpanded: logExpanded(job)) {
                    ScrollView {
                        VStack(alignment: .leading, spacing: 2) {
                            ForEach(job.log.suffix(300)) { line in
                                Text(line.message).font(.caption.monospaced())
                                    .foregroundStyle(line.isError ? .red : line.isWarning ? .orange : .primary)
                                    .frame(maxWidth: .infinity, alignment: .leading)
                            }
                        }
                        .textSelection(.enabled)
                    }
                    .frame(minHeight: 160, maxHeight: 260)
                }
              }
            } else {
                Section {
                    Text("No flash is running.").foregroundStyle(.secondary)
                    Button("Back to Setup") { model.step = .setup }
                }
            }
        }
        .formStyle(.grouped)
        .alert("Stop after this sequence?", isPresented: $confirmStop) {
            Button("Keep Flashing", role: .cancel) {}
            Button("Stop", role: .destructive) { model.cancel(afterSequence: true) }
        } message: {
            Text("The phone will be left partially written and may not start until you flash the complete firmware again.")
        }
    }

    @State private var logOpen = false

    /// The log opens by itself when something failed.
    private func logExpanded(_ job: JobState) -> Binding<Bool> {
        Binding(get: { logOpen || job.result?.error != nil }, set: { logOpen = $0 })
    }

    private func partitionName(for entry: String) -> String {
        model.plan?.files.first(where: { $0.entry == entry })?.partition ?? job_slot
    }
    private var job_slot: String { model.job?.progress.slot.uppercased() ?? "" }

    private func title(_ job: JobState) -> String {
        if job.running { return job.progress.phase.title }
        return job.result?.error == nil ? "Finished" : "Flash did not finish"
    }
}
