// Brings the main window forward and handles Dock re-open (ad-hoc dev builds often
// restore a hidden or zero-size frame from a previous session).
import AppKit

final class AppDelegate: NSObject, NSApplicationDelegate {
    func applicationWillFinishLaunching(_ notification: Notification) {
        UserDefaults.standard.set(false, forKey: "NSQuitAlwaysKeepsWindows")
    }

    func applicationDidFinishLaunching(_ notification: Notification) {
        NSApp.setActivationPolicy(.regular)
        // SwiftUI creates the window shortly after this; retry so we always order it front.
        for step in 0..<12 {
            let delay = 0.05 * Double(step)
            DispatchQueue.main.asyncAfter(deadline: .now() + delay) { self.presentMainWindow() }
        }
    }

    func applicationShouldHandleReopen(_ sender: NSApplication, hasVisibleWindows flag: Bool) -> Bool {
        if !flag { presentMainWindow() }
        NSApp.activate(ignoringOtherApps: true)
        return true
    }

    private func presentMainWindow() {
        NSApp.activate(ignoringOtherApps: true)
        let candidates = NSApp.windows.filter { w in
            w.canBecomeMain && !w.className.contains("Settings")
        }
        guard !candidates.isEmpty else { return }
        for window in candidates {
            if window.frame.width < 200 || window.frame.height < 200
                || window.frame.origin.x < -5000 || window.frame.origin.y < -5000 {
                window.setFrame(NSRect(x: 0, y: 0, width: 1100, height: 760), display: false)
                window.center()
            }
            window.makeKeyAndOrderFront(nil)
            window.orderFrontRegardless()
            window.title = "Bifrost Studio"
        }
    }
}