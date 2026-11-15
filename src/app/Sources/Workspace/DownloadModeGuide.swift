// Shared Download Mode key-combo help (Flash setup and elsewhere).
import SwiftUI

struct DownloadModeGuide: View {
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