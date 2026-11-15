// Device workspace: Probe and PIT backup (Download Mode).
import SwiftUI

struct DeviceWorkspaceView: View {
    @EnvironmentObject private var model: AppModel

    var body: some View {
        VStack(spacing: 0) {
            if model.handshakeDone { DeviceSessionBanner() }
            Picker("Device tool", selection: $model.toolsDevicePage) {
                ForEach(ToolsDevicePage.allCases) { Text($0.rawValue).tag($0) }
            }
            .pickerStyle(.segmented)
            .padding(.horizontal, 16)
            .padding(.vertical, 10)
            .frame(maxWidth: 280, alignment: .leading)
            .frame(maxWidth: .infinity, alignment: .leading)
            .background(.bar)

            Group {
                switch model.toolsDevicePage {
                case .probe: ProbeToolView()
                case .pit: PitBackupToolView()
                }
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .top)
        }
        .navigationTitle(L10n.device)
    }
}

private struct DeviceSessionBanner: View {
    @EnvironmentObject private var model: AppModel

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            Label("Restart the phone into Download Mode before flashing", systemImage: "exclamationmark.triangle.fill")
                .foregroundStyle(.orange)
            Text("The phone answers one session per Download Mode. A probe or PIT backup has used it.")
                .font(.callout).foregroundStyle(.secondary)
            HStack {
                Button("I Restarted It") { model.phoneWasRestarted() }
                Spacer()
            }
            HandshakeToggle()
        }
        .padding(12)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(.bar)
    }
}