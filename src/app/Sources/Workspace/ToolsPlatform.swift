// Bundled Android platform-tools (adb, fastboot).
import Foundation

enum PlatformTool {
    static func executable(_ name: String) -> URL? {
        Bundle.main.url(forResource: name, withExtension: nil, subdirectory: "platform-tools")
    }

    static func run(_ name: String, args: [String]) async -> String {
        guard let exe = executable(name) else { return "\(name) is not bundled in this build." }
        return await Task.detached {
            let process = Process()
            process.executableURL = exe
            process.arguments = args
            let pipe = Pipe()
            process.standardOutput = pipe
            process.standardError = pipe
            do { try process.run() } catch { return "Could not run \(name): \(error.localizedDescription)" }
            let data = pipe.fileHandleForReading.readDataToEndOfFile()
            process.waitUntilExit()
            let text = String(decoding: data, as: UTF8.self).trimmingCharacters(in: .whitespacesAndNewlines)
            return text.isEmpty ? "(no output)" : text
        }.value
    }

  /// Parses `adb devices -l` lines: "serial\tdevice …"
    static func parseAdbDevices(_ output: String) -> [String] {
        output.split(separator: "\n").compactMap { line in
            let parts = line.split(whereSeparator: { $0.isWhitespace })
            guard parts.count >= 2, parts[1] == "device" else { return nil }
            return String(parts[0])
        }
    }
}

enum TwrpFileCheck {
    struct Result {
        var ok: Bool
        var message: String
        var modelHint: String
    }

    static func validate(_ url: URL) -> Result {
        let name = url.lastPathComponent
        let lower = name.lowercased()
        guard lower.hasSuffix(".tar") || lower.hasSuffix(".tar.md5") else {
            return Result(ok: false, message: "Choose a .img.tar or .tar.md5 built for Odin (not a loose .img).", modelHint: "")
        }
        if lower.contains(".img.tar") || lower.contains("recovery") || lower.hasPrefix("ap_") {
            let model = modelHint(from: name)
            return Result(ok: true, message: "Looks like a flashable tar.", modelHint: model)
        }
        return Result(ok: false, message: "Expected a TWRP Odin package (often named like twrp-x.x.x-X-*.img.tar).", modelHint: "")
    }

    private static func modelHint(from fileName: String) -> String {
        let upper = fileName.uppercased()
        if let range = upper.range(of: #"(SM|SC|SH)[-]?[A-Z0-9]+"#, options: .regularExpression) {
            return String(upper[range]).replacingOccurrences(of: "-", with: "")
        }
        return ""
    }
}