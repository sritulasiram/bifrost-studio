# Changelog

All notable changes to Bifrost Studio. The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/). Items marked **UNVERIFIED** are implemented but have not been shown to work on a real phone (see [docs/hardware.md](docs/hardware.md)).

## [Unreleased]

## [1.1.0] - 2026-11-15

### Fixed
- App launch: `AppDelegate` raises the main window; agent XPC runs off the main thread after UI bootstrap (fixes missing window on open).
- Fuzz CI: skip building `libbifrost.dylib` under ASan with lld (`invalid r_symbolnum`).

### Changed
- App Swift sources grouped under `Sources/{App,Model,Bridge,Workspace}/`; removed monolithic `ToolsView.swift` in favour of `DeviceToolViews` and `AndroidToolViews`; [docs/README.md](docs/README.md) repo layout and maintainer doc version examples.
- **Phase 3 reach:** `Localizable.xcstrings` + `scripts/check-l10n.sh`; optional `release-notarize` GitHub workflow; CLI workflow recipes in [docs/cli.md](docs/cli.md).
- `packaging/install-local.sh` for dev installs to `~/Applications`.
- **Phase 2 UX:** flash presets (typical release, BL+AP+CP, recovery); Review plan JSON export; sidebar firmware drop → Setup; history failure clusters; pre-data session close on PIT/announce errors.
- **Phase 1 compatibility:** unlisted Download Mode USB product IDs; bootloader downgrade acknowledgement; SUPER/A-B plan warnings; Review plan export; keep-data preset.
- Documentation: merged coverage into [status.md](docs/status.md), reliability into [testing.md](docs/testing.md), trademark checklist into [release.md](docs/release.md), HIG notes into [app.md](docs/app.md); added [docs/README.md](docs/README.md) and [docs/roadmap.md](docs/roadmap.md).
- **Phase 0 UI:** Replaced Tools with sidebar **Inspect**, **Device**, **Android**; welcome sheet; menu **⌘1–5**; `packaging/preflight-release.sh`.

### Added
- Agent job journal (`agent-journal.json`) for crash recovery (R-4).
- `test_journal` suite.
- **P3 UX:** `bf_pit_compare` C API; Review & Tools PIT diff UI; Settings diagnostics zip export; Setup **AP only** preset; history logic in `AppModel+History.swift`.
- **P4 coverage:** F-3 agent hot-plug test; SUPER 512 rounding sim test; bootloader downgrade planner warning; partial LOKE handshake reads; Tools inspector polish (see [docs/status.md](docs/status.md#coverage-code-vs-hardware)).
- **macOS UX/HIG:** native sidebar `List(selection:)`; Help menu (⌘?) + bundled guides; `L10n` + `Localizable.strings`; accessibility on progress bars; [docs/app.md](docs/app.md#macos-design-hig).
- **Tools overhaul:** Device / Firmware / Android navigation; standalone Inspect; `toolsPitDiff` separate from Review; PIT backup → Inspect ingest; probe session copy; adb device picker and fastboot; TWRP tar validation. UNVERIFIED.

### Changed
- **P2 reliability:** journal v2 (progress, device, last log); app reattach on agent crash; `scripts/ci-local.sh`; CI installs CMake; [docs/testing.md#reliability](docs/testing.md#reliability).
- **P1 release:** `packaging/codesign-app.sh`, `fix-libusb-rpaths.sh`, `notarize.sh`, `release.sh`; CMake `release` target; [docs/release.md](docs/release.md).
- `normalizeModel` strips repeated `SM` prefixes (fixes `SMSM-…` typing).
- Odin session drain waits for a quiet IN window before control reads (late ZLP).
- Default bundle ID `studio.bifrost.macos`; optional bundled dynamic libusb when a `.dylib` is in `LIBUSB_ROOT`.

### Added
- **Android tools** (Tools › Android): `adb` from Android platform-tools 37.0.1, bundled in the app, to list phones, reboot to Download Mode or Recovery, and restart the adb server. UNVERIFIED.
- **TWRP tool** (Tools › TWRP): loads a TWRP `.img.tar` as the only AP file and turns off the automatic restart. It never unlocks a bootloader. UNVERIFIED.
- Hardware results: full flashes on the SM-N950F (Galaxy Note 8, 2026-11-09); the SM-N915G has been tested (details not yet recorded).
- [Using Bifrost Studio](docs/using.md): flashing, entering Download Mode and recovering from a failed flash.

### Changed
- The handshake is retried in place, up to 4 attempts 300 ms apart, when it times out. UNVERIFIED; added for the SM-N986U1.
- Before the first handshake the USB transport clears halt on both endpoints, sets the line coding, raises DTR and RTS and waits 400 ms, as Odin on Windows does. USB errors now include the result of this setup. UNVERIFIED.
- A PIT may repeat partition names (Snapdragon layouts list XBL once per logical unit). Comparison pairs repeated names by order, and the planner still refuses any image that matches more than one partition.
- After a timeout with no data sent, the app keeps Flash disabled until the phone is restarted into Download Mode, because each attempt resets its USB.
- Documentation reorganised into one flat `docs/` folder, with the README as the only index. Build instructions no longer assume a particular machine.

### Known issues
- SM-N986U1 (Galaxy Note 20 Ultra 5G): the handshake gets no `LOKE` reply, so probe, PIT read and flash do not work yet.
- Release DMG is ad-hoc signed unless you notarize with Developer ID; see [docs/release.md](docs/release.md).

## [1.0.0] - 2026-11-08

### Added
- First release: a native macOS app, `bifrost-cli`, a C API (`libbifrost`), a background agent and a root USB helper for flashing Samsung firmware in Download Mode.
- Offline plan review, MD5 and LZ4 verification, a refusal-first planner, explicit confirmation of data erase, background flashing and honest failure reporting.
- First full hardware flash on the SM-N920I (Galaxy Note 5): BL + AP + CP + full CSC, about 4.9 GB in about 2.5 minutes.
