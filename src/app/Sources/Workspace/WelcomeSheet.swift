// First-launch overview (Phase 0 onboarding).
import SwiftUI

struct WelcomeSheet: View {
    @EnvironmentObject private var model: AppModel

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            Text("Welcome to Bifrost Studio")
                .font(.title2.bold())
            Text("Flash Samsung firmware in Download Mode — with offline review before any data is sent.")
                .foregroundStyle(.secondary)

            VStack(alignment: .leading, spacing: 8) {
                Label("Flash — add firmware, review the plan, flash", systemImage: "bolt.fill")
                Label("Inspect — verify packages and PIT without the phone", systemImage: "shippingbox")
                Label("History — logs for every flash, probe and PIT read", systemImage: "clock")
                Label("Device — probe and PIT backup (uses one Download Mode session)", systemImage: "iphone.gen3")
                Label("Android — adb and TWRP prep (phone booted to Android)", systemImage: "cable.connector")
            }
            .font(.callout)

            Text("Register the background agent and USB helper in **Settings › Services**, then approve them in **Login Items**. Flashes keep running if you close this window.")
                .font(.callout).foregroundStyle(.secondary)

            HStack {
                Button("Open Settings") {
                    model.dismissWelcome()
                    NSApp.sendAction(Selector(("showSettingsWindow:")), to: nil, from: nil)
                }
                Spacer()
                Button("Get Started") { model.dismissWelcome() }
                    .keyboardShortcut(.defaultAction)
            }
        }
        .padding(24)
        .frame(width: 440)
    }
}