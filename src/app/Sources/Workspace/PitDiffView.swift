// Shows a firmware vs device (or file) PIT comparison from Review or Tools.
import SwiftUI

struct PitDiffView: View {
    let comparison: PitComparison

    var body: some View {
        if comparison.sameLayout, comparison.sizeDifferences.isEmpty {
            Label("Partition layout matches the firmware PIT", systemImage: "checkmark.circle.fill")
                .foregroundStyle(.green)
        } else {
            if !comparison.sameLayout {
                Label("Layout differs — this firmware may be for another device", systemImage: "xmark.octagon.fill")
                    .foregroundStyle(.red)
                differenceList(comparison.layoutDifferences)
            }
            if !comparison.sizeDifferences.isEmpty {
                Label("Some partition sizes differ (may be a PIT revision)", systemImage: "exclamationmark.triangle.fill")
                    .foregroundStyle(.orange)
                differenceList(comparison.sizeDifferences)
            }
        }
    }

    private func differenceList(_ lines: [String]) -> some View {
        VStack(alignment: .leading, spacing: 4) {
            ForEach(Array(lines.enumerated()), id: \.offset) { _, line in
                Text("· \(line)").font(.callout).foregroundStyle(.secondary)
            }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
    }
}