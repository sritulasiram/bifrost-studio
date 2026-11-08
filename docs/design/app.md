# The app

Bifrost Studio.app: macOS 13+, SwiftUI with `ObservableObject`. Device work goes through the agent (`bf_agent_*`, see [../reference/c-api.md](../reference/c-api.md)). Read-only work (package verification, PIT parsing, offline plans) runs in the app process. UX requirements are in [../project/requirements.md](../project/requirements.md#user-experience).

## Flash flow

1. **Setup.** Pick the phone; it is chosen automatically if only one is in Download Mode. Drop the firmware files. They are MD5-verified and parsed in the app process, with no phone access.
2. **Review.** The plan is built offline from the PIT inside the firmware (normally the CSC's, via `bf_packages_embedded_pit`) or from a PIT file the user opens. **No session is opened on the phone** (F-27).
3. **Confirm.** Acknowledge any data erase and choose whether to reboot. The app passes the firmware's detected model to the agent itself; the core still checks it. Erasing flashes ask for a final "Erase and Flash" confirmation.
4. **Flash.** The agent opens the files' descriptors, claims the phone through the root helper and runs `flash::flash` (see [architecture.md](architecture.md#a-flash-step-by-step)).

```mermaid
flowchart LR
    S["1 Setup<br/>phone + firmware<br/>verify MD5, parse"] --> V["2 Review<br/>offline plan from<br/>embedded PIT or PIT file"] --> C["3 Confirm<br/>accept erase,<br/>choose reboot"] --> F["4 Flash<br/>agent + root helper"]
    V -. "no session on the phone (F-27)" .-> V
    F --> H[(History)]
```

The flash must be the **first session since the phone entered Download Mode** (F-40). The app tracks this (`handshakeDone`) and blocks Flash until the phone has been restarted into Download Mode, for example after a probe or PIT backup.

```mermaid
stateDiagram-v2
    [*] --> Fresh: phone enters Download Mode
    Fresh --> Used: a job opens a session without rebooting (probe, PIT backup)
    Used --> Fresh: no Download Mode phone seen for more than 3 s (phone restarted)
    Used --> Fresh: user taps "I Restarted It into Download Mode"
    Fresh --> Flashing: Flash enabled
    Used --> Used: Flash disabled, notice explains how to restart
    Flashing --> [*]
```

## Source files

| File | Contents |
|---|---|
| `BifrostStudioApp.swift` | `@main`, Settings scene, ⌘1–3 shortcuts, terminal flags `--service-status`, `--register-services`, `--unregister-services` |
| `Bridge.swift` | Swift value types copied from the C API, `LocalCore` (in-process), `PitTable` (including the firmware's embedded PIT) |
| `AgentConnection.swift` | `bf_agent_*` wrapper; events are copied in the callback and delivered on the main thread |
| `AppModel.swift` | State, job tracking, review (firmware PIT or file), the `handshakeDone` heuristic, history and retention, `Prefs` |
| `FlashFlowView.swift` | ① Setup ② Review & Confirm ③ Flash, and the "restart into Download Mode" notice |
| `ToolsView.swift` | Probe, PIT Backup, Inspect |
| `HistoryView.swift` | Filter, search, summary, export, retry, delete |
| `SettingsView.swift` | General, Services, Flashing, Data, About, Diagnostics |
| `Views.swift` | Sidebar (Workspace and Devices), service banner, `HandshakeToggle`, `JobLine`, `RecoveryCard`, `PlanView` |
| `BackgroundServices.swift` | `SMAppService` helpers and service controls |

- **`handshakeDone`:** set after any job that opened a session without rebooting the phone. Cleared when no Download Mode phone has been seen for more than 3 s, which means the phone was restarted. The user can override it ("I Restarted It into Download Mode").
- **History:** one JSON file per job in `~/Library/Application Support/Bifrost Studio/History/`.
