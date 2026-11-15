// Android workspace: adb and TWRP preparation while the phone runs Android.
import SwiftUI

struct AndroidWorkspaceView: View {
    @EnvironmentObject private var model: AppModel

    var body: some View {
        VStack(spacing: 0) {
            Picker("Android tool", selection: $model.toolsAndroidPage) {
                ForEach(ToolsAndroidPage.allCases) { Text($0.rawValue).tag($0) }
            }
            .pickerStyle(.segmented)
            .padding(.horizontal, 16)
            .padding(.vertical, 10)
            .frame(maxWidth: 200, alignment: .leading)
            .frame(maxWidth: .infinity, alignment: .leading)
            .background(.bar)

            Group {
                switch model.toolsAndroidPage {
                case .adb: AndroidToolView()
                case .twrp: TwrpToolView()
                }
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .top)
        }
        .navigationTitle(L10n.android)
    }
}