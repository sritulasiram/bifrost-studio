// Background-service registration (SMAppService): the Settings window and the
// terminal flags --service-status, --register-services, --unregister-services.
import ServiceManagement
import SwiftUI

// SMAppService handles for the agent and the USB helper; plist names come
// from Info.plist (set by CMake).
enum BackgroundServices {
    static var agent: SMAppService? {
        plistName("BifrostAgentPlist").map { SMAppService.agent(plistName: $0) }
    }
    static var helper: SMAppService? {
        plistName("BifrostHelperPlist").map { SMAppService.daemon(plistName: $0) }
    }

    static func plistName(_ key: String) -> String? {
        Bundle.main.object(forInfoDictionaryKey: key) as? String
    }

    static var bothEnabled: Bool {
        agent?.status == .enabled && helper?.status == .enabled
    }

    static func summary() -> String {
        "Agent: \(describe(agent)) · USB helper: \(describe(helper))"
    }

    /// Registers both services; returns a message for the UI.
    static func register() -> String {
        do {
            try agent?.register()
            try helper?.register()
        } catch {
            return "Registration failed: \(error.localizedDescription)"
        }
        if bothEnabled { return "Registered." }
        return "Registered. Now turn on “Bifrost Studio” in System Settings › General › Login Items; Bifrost Studio connects automatically once it's allowed."
    }

    static func describe(_ service: SMAppService?) -> String {
        guard let service else { return "Not configured in Info.plist" }
        switch service.status {
        case .notRegistered: return "Not registered"
        case .enabled: return "Enabled"
        case .requiresApproval: return "Needs approval in Login Items"
        case .notFound: return "Not found in the app bundle"
        @unknown default: return "Unknown"
        }
    }

    /// Handles --service-status, --register-services and --unregister-services.
    /// Returns an exit code, or nil when no such flag was given.
    static func runCommandLine(_ arguments: [String]) -> Int32? {
        let wanted = ["--service-status", "--register-services", "--unregister-services"]
        guard let flag = arguments.first(where: { wanted.contains($0) }) else { return nil }
        var code: Int32 = 0
        for (name, service) in [("agent", agent), ("usb-helper", helper)] {
            guard let service else {
                print("\(name): not configured")
                code = 1
                continue
            }
            do {
                if flag == "--register-services" { try service.register() }
                if flag == "--unregister-services" { try service.unregister() }
            } catch {
                print("\(name): \(flag) failed: \(error.localizedDescription) [\(error)]")
                code = 1
            }
            print("\(name): \(describe(service))")
        }
        print("bundle: \(Bundle.main.bundlePath)")
        return code
    }
}

/// Status and controls for the two background services. Used in the setup
/// sheet and in Settings › Services. The helper needs the user's approval in
/// System Settings > General > Login Items.
struct ServiceControlsView: View {
    @EnvironmentObject private var model: AppModel
    @State private var agentStatus = "Unknown"
    @State private var helperStatus = "Unknown"
    @State private var message = ""
    @State private var confirmUnregister = false

    private var jobRunning: Bool { model.job?.running ?? false }

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            GroupBox {
                VStack(alignment: .leading, spacing: 6) {
                    statusRow("Flash agent", agentStatus, dot: agentStatus == "Enabled" ? .green
                              : agentStatus.hasPrefix("Needs") ? .orange : .red)
                    statusRow("USB helper (root)", helperStatus, dot: helperStatus == "Enabled" ? .green
                              : helperStatus.hasPrefix("Needs") ? .orange : .red)
                    statusRow("Connection", connectionText, dot: connectionColor)
                }
                .frame(maxWidth: .infinity, alignment: .leading)
            }
            if !message.isEmpty { Text(message).font(.callout) }
            HStack {
                Button("Register") { register() }
                Button("Open Login Items") { SMAppService.openSystemSettingsLoginItems() }
                Button("Reconnect") { model.connect(); refresh() }
                Spacer()
                Button("Unregister…") { confirmUnregister = true }
                    .disabled(jobRunning)
                    .help(jobRunning ? "Not available while a job is running." : "Removes both services.")
            }
        }
        .onAppear(perform: refresh)
        .onChange(of: model.serviceStatus) { _ in refresh() }
        .confirmationDialog("Unregister the background services?", isPresented: $confirmUnregister) {
            Button("Unregister", role: .destructive) { unregister() }
        } message: {
            Text("Flashing and phone detection stop working until you register them again.")
        }
    }

    private func statusRow(_ title: String, _ value: String, dot: Color) -> some View {
        LabeledContent(title) {
            HStack(spacing: 6) {
                Circle().fill(dot).frame(width: 8, height: 8).accessibilityHidden(true)
                Text(value)
            }
        }
    }

    private var connectionColor: Color {
        switch model.agentState {
        case .connected: return .green
        case .connecting: return .secondary
        case .unavailable: return .red
        }
    }

    private var connectionText: String {
        switch model.agentState {
        case .connected: return "Connected"
        case .connecting: return "Connecting…"
        case .unavailable: return "Not connected"
        }
    }

    private func refresh() {
        agentStatus = BackgroundServices.describe(BackgroundServices.agent)
        helperStatus = BackgroundServices.describe(BackgroundServices.helper)
    }

    private func register() {
        model.registerServices()
        message = model.serviceMessage ?? ""
        refresh()
    }

    private func unregister() {
        do {
            try BackgroundServices.agent?.unregister()
            try BackgroundServices.helper?.unregister()
            message = "Unregistered."
        } catch {
            message = "Unregistration failed: \(error.localizedDescription)"
        }
        model.refreshServiceStatus()
        refresh()
    }
}
