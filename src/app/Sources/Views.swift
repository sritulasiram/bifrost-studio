// Window layout, status pill, service setup and shared components.
import AppKit
import ServiceManagement
import SwiftUI

enum SidebarItem: String, CaseIterable, Identifiable {
    case flash = "Flash", history = "History", tools = "Tools"
    var id: String { rawValue }
    var symbol: String {
        switch self {
        case .flash: return "bolt.fill"
        case .history: return "clock"
        case .tools: return "wrench.and.screwdriver"
        }
    }
}

struct RootView: View {
    @EnvironmentObject private var model: AppModel

    var body: some View {
        NavigationSplitView {
            // Plain buttons that set the section directly. List(selection:) did
            // not register clicks on macOS 26 (checked with a real click).
            List {
                Section("Workspace") {
                    ForEach(SidebarItem.allCases) { item in
                        SidebarRow(item: item)
                            .listRowBackground(
                                RoundedRectangle(cornerRadius: 6)
                                    .fill(model.section == item ? Color.accentColor.opacity(0.25) : Color.clear)
                                    .padding(.horizontal, 6))
                    }
                }
                Section("Devices") {
                    if model.devices.isEmpty {
                        Text("No phone").foregroundStyle(.secondary)
                    }
                    ForEach(model.devices) { device in
                        DeviceRow(device: device)
                    }
                }
            }
            .listStyle(.sidebar)
            .navigationSplitViewColumnWidth(min: 170, ideal: 190)
        } detail: {
            VStack(spacing: 0) {
                AgentBanner()
                switch model.section {
                case .flash: FlashFlowView()
                case .history: HistoryView()
                case .tools: ToolsView()
                }
            }
        }
        .navigationTitle(model.section.rawValue)
        .sheet(isPresented: $model.showServiceSetup) { ServiceSetupSheet() }
        .alert("Something went wrong", isPresented: Binding(
            get: { model.actionError != nil }, set: { if !$0 { model.actionError = nil } })) {
            Button("OK", role: .cancel) {}
        } message: {
            Text(model.actionError?.message ?? "")
        }
    }
}

private struct SidebarRow: View {
    @EnvironmentObject private var model: AppModel
    let item: SidebarItem

    var body: some View {
        Button {
            model.section = item
        } label: {
            HStack {
                Label(item.rawValue, systemImage: item.symbol)
                Spacer()
                if item == .flash, model.job?.running ?? false {
                    Text("Running").font(.caption)
                        .padding(.horizontal, 6).padding(.vertical, 1)
                        .background(Capsule().fill(Color.accentColor.opacity(0.25)))
                }
            }
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .accessibilityAddTraits(model.section == item ? .isSelected : [])
    }
}

/// A phone in the sidebar. Choosing one selects it for flashing, as in Finder's device list.
private struct DeviceRow: View {
    @EnvironmentObject private var model: AppModel
    let device: Device

    private var selected: Bool { model.selectedDevice == device.id }

    var body: some View {
        Button {
            if device.inDownloadMode { model.selectedDevice = device.id }
        } label: {
            HStack(spacing: 8) {
                Image(systemName: "iphone")
                    .foregroundStyle(device.inDownloadMode ? Color.green : Color.secondary)
                    .accessibilityHidden(true)
                VStack(alignment: .leading, spacing: 1) {
                    Text("Samsung phone").lineLimit(1)
                        .foregroundStyle(device.inDownloadMode ? .primary : .secondary)
                    Text(device.inDownloadMode ? "Download Mode" : "Not in Download Mode")
                        .font(.caption).foregroundStyle(.secondary)
                }
                Spacer()
                if selected {
                    Image(systemName: "checkmark").foregroundStyle(.secondary).accessibilityHidden(true)
                }
            }
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .accessibilityElement(children: .combine)
        .accessibilityAddTraits(selected ? .isSelected : [])
        .help(device.inDownloadMode ? "Use this phone for flashing (\(device.id.label))"
              : "Restart this phone into Download Mode to use it (\(device.id.label))")
    }
}

// MARK: - Service setup

/// One line, shown only when the background agent can't be reached.
struct AgentBanner: View {
    @EnvironmentObject private var model: AppModel

    var body: some View {
        if case let .unavailable(message) = model.agentState {
            HStack(spacing: 10) {
                Image(systemName: "exclamationmark.triangle.fill").foregroundStyle(.orange)
                    .accessibilityHidden(true)
                Text("Background service not connected").font(.callout.bold())
                Text(model.serviceMessage ?? message).font(.callout).foregroundStyle(.secondary)
                    .lineLimit(1).truncationMode(.tail)
                Spacer()
                Button("Set Up…") { model.showServiceSetup = true }
            }
            .padding(.horizontal, 12).padding(.vertical, 8)
            .background(Color.orange.opacity(0.12))
            .accessibilityElement(children: .combine)
        }
    }
}

struct ServiceSetupSheet: View {
    @EnvironmentObject private var model: AppModel
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            Text("Set up the background service").font(.title2.bold())
            Text("Flashes run in a background service so they keep going if you close this window. A small helper handles USB access and needs your approval once.")
                .foregroundStyle(.secondary)
            VStack(alignment: .leading, spacing: 6) {
                Label("Click Register.", systemImage: "1.circle")
                Label("Turn on “Bifrost Studio” in System Settings › General › Login Items.", systemImage: "2.circle")
                Label("This window closes by itself once the service connects.", systemImage: "3.circle")
            }
            ServiceControlsView()
            HStack {
                Spacer()
                Button("Close") { dismiss() }.keyboardShortcut(.cancelAction)
            }
        }
        .padding(20)
        .frame(width: 520)
        .onChange(of: model.agentState) { if $0 == .connected { dismiss() } }
    }
}

// MARK: - Shared controls

/// The resume decision. Shown only after a start timeout, or when Settings asks for it.
struct HandshakeToggle: View {
    @EnvironmentObject private var model: AppModel
    @AppStorage(Prefs.showHandshake) private var always = false

    var body: some View {
        if always || model.handshakeSuspect {
            VStack(alignment: .leading, spacing: 6) {
                if model.handshakeSuspect {
                    Label("The last attempt timed out before any data was sent. Restart the phone into Download Mode, or change this setting.",
                          systemImage: "exclamationmark.triangle")
                        .font(.callout).foregroundStyle(.orange)
                }
                Toggle(isOn: $model.handshakeDone) {
                    VStack(alignment: .leading, spacing: 2) {
                        Text("Phone already used in this Download Mode session")
                        Text("The phone answers the connection handshake once each time it enters Download Mode. Bifrost Studio sets this automatically.")
                            .font(.caption).foregroundStyle(.secondary)
                    }
                }
            }
        }
    }
}

struct JobLine: View {
    let job: JobState

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            if job.running {
                HStack {
                    ProgressView().controlSize(.small)
                    Text("\(job.kind.title): \(job.progress.phase.title)")
                }
            } else if let result = job.result {
                if let error = result.error {
                    RecoveryCard(error: error, rebooted: result.rebooted)
                } else {
                    Label("\(job.kind.title) succeeded", systemImage: "checkmark.circle.fill")
                        .foregroundStyle(.green)
                    if result.session.packetSize > 0 {
                        Text(String(format: "Protocol %u · session ACK 0x%x · %@ parts",
                                    result.session.protocolVersion, result.session.ack,
                                    formatBytes(UInt64(result.session.packetSize))))
                            .font(.caption).foregroundStyle(.secondary)
                    }
                }
            }
        }
    }
}

// MARK: - Recovery guidance

/// What happened, whether data was written, and what to do next, keyed on
/// the error code and data state.
struct RecoveryCard: View {
    let error: BifrostError
    let rebooted: Bool

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Label(title, systemImage: symbol).font(.headline).foregroundStyle(color)
            Text(dataStatement).bold()
            Text(error.message).font(.callout).textSelection(.enabled)
            ForEach(nextSteps, id: \.self) { step in
                Label(step, systemImage: "arrow.right.circle").font(.callout)
            }
            Text("Error: \(error.codeName)").font(.caption).foregroundStyle(.secondary)
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .accessibilityElement(children: .combine)
    }

    private var color: Color {
        switch error.dataState {
        case .none: return error.code == BifrostError.cancelled ? .secondary : .orange
        case .partial: return .red
        case .complete: return .yellow
        }
    }

    private var symbol: String {
        error.dataState == .partial ? "exclamationmark.octagon.fill" : "exclamationmark.triangle.fill"
    }

    private var title: String {
        switch error.dataState {
        case .none: return error.code == BifrostError.cancelled ? "Cancelled" : "Stopped before writing"
        case .partial: return "The phone may be partially written"
        case .complete: return "Data written; closing failed"
        }
    }

    private var dataStatement: String {
        switch error.dataState {
        case .none: return "No partition data was sent. The phone's software is unchanged."
        case .partial: return "Some partition data was sent. The phone may not start normally."
        case .complete: return "All data was acknowledged by the phone; only closing the session or the reboot request failed."
        }
    }

    private var nextSteps: [String] {
        switch error.dataState {
        case .partial:
            return ["Do not unplug or restart the phone into Android yet.",
                    "Keep it in Download Mode (re-enter it if it left) and flash the complete firmware again.",
                    "If Bifrost Studio can't reconnect, Odin (Windows) or Heimdall can also recover a phone in Download Mode."]
        case .complete:
            return ["Restart the phone manually (hold Power and Volume Down until it restarts)."]
        case .none:
            switch error.code {
            case BifrostError.usbTimeout:
                return ["Restart the phone into Download Mode and try again. If it times out again, clear “Phone already used in this Download Mode session”, which appears after a timeout."]
            case BifrostError.modelMismatch:
                return ["Check the model on the Download Mode screen and the firmware's model."]
            case BifrostError.planRefused:
                return ["Review the refusals: this firmware does not match the phone's partition table."]
            case BifrostError.serviceUnavailable:
                return ["Set up the background service (Settings › Services, ⌘,) and approve it in Login Items."]
            case BifrostError.usbAccess:
                return ["Quit other USB tools (Smart Switch, browsers using WebUSB, Odin VMs), then try again."]
            case BifrostError.checksumMismatch, BifrostError.fileChanged:
                return ["Download the firmware again; the file is damaged or changed."]
            case BifrostError.cancelled:
                return []
            default:
                return ["Restart the phone into Download Mode and try again."]
            }
        }
    }
}

/// Plan details shared by Review and the Inspector.
struct PlanView: View {
    let plan: Plan

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            if !plan.refusals.isEmpty {
                VStack(alignment: .leading, spacing: 4) {
                    Label("Refused. Nothing will be flashed:", systemImage: "xmark.octagon.fill")
                        .foregroundStyle(.red).font(.headline)
                    ForEach(plan.refusals, id: \.self) { Text("• \($0)") }
                }
            }
            Text(plan.detectedModel.isEmpty ? "Model: not detected from file names"
                                            : "Model from firmware: \(plan.detectedModel)")
                .foregroundStyle(.secondary)
            ForEach(plan.files) { file in
                HStack {
                    Text(file.slot.title).font(.caption.bold()).frame(width: 64, alignment: .leading)
                    Text(file.entry).font(.body.monospaced())
                    Image(systemName: "arrow.right").foregroundStyle(.secondary).accessibilityLabel("to")
                    Text("\(file.partition) (\(file.partitionID))")
                    Spacer()
                    Text(formatBytes(file.size) + (file.lz4 ? " LZ4" : "")).foregroundStyle(.secondary)
                }
                .accessibilityElement(children: .combine)
            }
            if !plan.skipped.isEmpty {
                DisclosureGroup("Skipped (\(plan.skipped.count))") {
                    ForEach(plan.skipped) { s in
                        Text("\(s.slot.title): \(s.entry) — \(s.reason)").font(.callout)
                    }
                }
            }
            ForEach(plan.warnings) { w in
                Label(w.message, systemImage: "exclamationmark.triangle").foregroundStyle(.orange)
            }
        }
    }
}

