# App design (macOS HIG)

How Bifrost Studio is meant to feel on the Mac.

## Principles

1. **Safety before speed** — Ambiguous flashes are refused offline. Destructive flashes need explicit toggles and a confirmation alert. The Flash action has **no keyboard shortcut**.
2. **Honest state** — Download Mode vs not, session-used, service disconnected, and partial-write errors are visible in plain language (`RecoveryCard`).
3. **Native shell** — `NavigationSplitView`, grouped settings (⌘,), system symbols, and standard sheets/alerts.
4. **One job at a time** — Flash runs in the background agent; the UI can close without stopping work.

## Layout

| Area | Pattern |
|------|---------|
| Sidebar | Workspace sections (single selection) + read-only device list |
| Flash | Three-step wizard inside the detail column; bottom action bar for the primary action |
| History | `HSplitView`: list + detail |
| Inspect / Device / Android | Separate sidebar items; Device and Android use one segmented sub-picker each |
| Settings | Tabbed preferences scene |

## Accessibility

- Sidebar rows and device rows expose selection state to VoiceOver.
- Progress views include percent accessibility values.
- Recovery cards are combined accessibility elements with structured labels.

## Localization

Navigation and menu strings live in `Resources/Localizable.xcstrings` (UX-6). `en.lproj/Localizable.strings` mirrors them for tooling checks.

**Help → Bifrost Studio Help** opens the user guide (flashing and recovery).