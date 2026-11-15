// Android-side tool forms (adb, TWRP prepare).
import AppKit
import SwiftUI

struct AndroidToolView: View {
    @State private var output = ""
    @State private var busy = false
    @State private var serial = ""
    @State private var devices: [String] = []

    private var adbPresent: Bool { PlatformTool.executable("adb") != nil }

    var body: some View {
        Form {
            Section {
                Label("Phone must be booted into Android with USB debugging on — not Download Mode.",
                      systemImage: "info.circle")
                    .font(.callout).foregroundStyle(.secondary)
                LabeledContent("adb") {
                    Text(adbPresent ? "Bundled platform-tools" : "Not bundled")
                        .foregroundStyle(.secondary)
                }
                if devices.count > 1 {
                    Picker("Device", selection: $serial) {
                        ForEach(devices, id: \.self) { Text($0).tag($0) }
                    }
                } else if devices.count == 1 {
                    LabeledContent("Device", value: devices[0])
                }
                Button("List Phones") { run(["devices", "-l"], refresh: true) }
                Button("Reboot to Download Mode") { run(["reboot", "download"]) }
                Button("Reboot to Recovery") { run(["reboot", "recovery"]) }
                Button("Reboot to Bootloader (fastboot)") { runFastboot(["reboot-bootloader"]) }
                Button("Restart adb Server") { run(["kill-server"], then: ["start-server"]) }
                Text("UNVERIFIED on hardware — confirm with a test phone and note the result in a bug report.")
                    .font(.caption).foregroundStyle(.secondary)
                if !output.isEmpty {
                    Text(output).font(.callout.monospaced()).textSelection(.enabled)
                }
            } header: {
                Text("Android platform-tools")
            } footer: {
                Text("Authorise this Mac on the phone when prompted. Use key combos if the phone is already in Download Mode.")
            }
        }
        .formStyle(.grouped)
        .disabled(busy)
        .onAppear { if devices.isEmpty { run(["devices", "-l"], refresh: true) } }
    }

    private func adbArgs(_ base: [String]) -> [String] {
        guard !serial.isEmpty else { return base }
        return ["-s", serial] + base
    }

    private func run(_ args: [String], then next: [String]? = nil, refresh: Bool = false) {
        busy = true
        Task {
            var text = await PlatformTool.run("adb", args: adbArgs(args))
            if refresh {
                devices = PlatformTool.parseAdbDevices(text)
                if serial.isEmpty || !devices.contains(serial) { serial = devices.first ?? "" }
            }
            if let next { text += "\n" + (await PlatformTool.run("adb", args: adbArgs(next))) }
            output = text
            busy = false
        }
    }

    private func runFastboot(_ args: [String]) {
        busy = true
        Task {
            var bootArgs = args
            if !serial.isEmpty { bootArgs = ["-s", serial] + bootArgs }
            output = await PlatformTool.run("fastboot", args: bootArgs)
            busy = false
        }
    }
}

struct TwrpToolView: View {
    @EnvironmentObject private var model: AppModel
    @State private var chosen: URL?
    @State private var check: TwrpFileCheck.Result?

    var body: some View {
        Form {
            Section {
                LabeledContent("TWRP file") {
                    Text(chosen?.lastPathComponent ?? "None chosen").foregroundStyle(.secondary)
                }
                if let check {
                    if check.ok {
                        Label(check.message, systemImage: "checkmark.circle.fill").foregroundStyle(.green)
                        if !check.modelHint.isEmpty {
                            Text("Model hint from name: \(check.modelHint)").font(.callout)
                        }
                    } else {
                        Text(check.message).foregroundStyle(.red).font(.callout)
                    }
                }
                Button("Choose TWRP File…") { choose() }
                Button("Prepare Flash") { prepare() }
                    .disabled(chosen == nil || check?.ok != true)
                Text("UNVERIFIED on hardware — confirm with a test phone before relying on Prepare Flash.")
                    .font(.caption).foregroundStyle(.secondary)
                Link("Find TWRP for your model (twrp.me)", destination: URL(string: "https://twrp.me/Devices/Samsung/")!)
            } header: {
                Text("Install TWRP")
            } footer: {
                Text("Use the Odin .img.tar for your exact model. Prepare Flash loads AP only, turns off auto-restart, and opens Flash.")
            }
            Section {
                Label("Turn on OEM unlocking in Developer options first.", systemImage: "lock.open")
                Label("Unlocking the bootloader erases all data and trips Knox permanently.", systemImage: "exclamationmark.triangle.fill")
                    .foregroundStyle(.orange)
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
        guard panel.runModal() == .OK, let url = panel.url else { return }
        chosen = url
        check = TwrpFileCheck.validate(url)
    }

    private func prepare() {
        guard let chosen, check?.ok == true else { return }
        for slot in Slot.allCases { model.paths[slot] = nil }
        model.setPath(chosen.path, for: .ap)
        model.rebootWhenDone = false
        model.section = .flash
    }
}