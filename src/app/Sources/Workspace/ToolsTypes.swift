// Sub-navigation within Device and Android workspace pages.
import Foundation

enum ToolsDevicePage: String, CaseIterable, Identifiable {
    case probe = "Probe"
    case pit = "PIT Backup"
    var id: String { rawValue }
}

enum ToolsAndroidPage: String, CaseIterable, Identifiable {
    case adb = "adb"
    case twrp = "TWRP"
    var id: String { rawValue }
}