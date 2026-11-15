# Roadmap

Phased delivery. Live status: [status.md](status.md). Odin parity notes are in the **Phase 1** section below.

## Phase 0 — Ship & structure ✅ (in progress)

| Item | Status |
|------|--------|
| Sidebar: Flash · Inspect · History · Device · Android | Done in app |
| First-run welcome sheet + Settings services copy (F-80/F-82) | Done |
| `packaging/preflight-release.sh` before DMG | Done |
| Developer ID + notarization (D-2, SEC-5) | Maintainer Apple ID required |
| Trademark checklist | [release.md#trademark-d-6](release.md#trademark-d-6) |
| F-80/F-82 under launchd | Manual checklist: [testing.md#launchd-flash-resilience](testing.md#launchd-flash-resilience) |
| F-83/F-84 on hardware | UNVERIFIED until tested |

## Phase 1 — Compatibility (Odin-relevant)

**In code (2026-11-15):** unlisted Download Mode PID discovery; bootloader downgrade acknowledgement; SUPER/A-B plan warnings; Review plan copy; HOME_CSC preset hint; diagnostics note unlisted PIDs.

- SM-N986U1 handshake on clean Download Mode
- SM-N915G write-up in [hardware.md](hardware.md)
- Unknown Download Mode PID reporting
- A/B, dynamic partitions / `super` planner + hardware proof
- Bootloader rollback policy
- More PIT fixtures

**Odin gaps addressed here:** newer Qualcomm, super partition, broader USB PIDs — not repartition, NAND erase, modem type 1, or firmware download.

## Phase 2 — UX without S-3 violations ✅ (in code 2026-11-15)

| Item | Status |
|------|--------|
| Flash presets (HOME_CSC / BL+AP+CP / recovery) | Done |
| Plan export on Review (JSON) | Done |
| Drag firmware onto sidebar → Setup | Done |
| History failure clustering + compare links | Done |
| Pre-data session close on PIT/read/announce failures | Done |

## Phase 3 — Reach ✅ (in code 2026-11-15)

| Item | Status |
|------|--------|
| String Catalog `Localizable.xcstrings` + `check-l10n.sh` (UX-6) | Done |
| Notarization CI (`.github/workflows/release-notarize.yml`, secrets) | Done |
| CLI workflow docs ([cli.md](cli.md#workflows)) | Done |

## Out of scope (policy)

Re-partition, PIT flash to device, NAND erase all, EFS clear, modem type 1, firmware downloaders, `0x64/1`, force overrides.