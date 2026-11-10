# Changelog

All notable changes to Bifrost Studio. The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/). Items marked **UNVERIFIED** are implemented but have not been shown to work on a real phone (see [docs/hardware.md](docs/hardware.md)).

## [Unreleased]

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
- Not notarized; libusb is linked statically. See [docs/status.md](docs/status.md).

## [1.0.0] - 2026-11-08

### Added
- First release: a native macOS app, `bifrost-cli`, a C API (`libbifrost`), a background agent and a root USB helper for flashing Samsung firmware in Download Mode.
- Offline plan review, MD5 and LZ4 verification, a refusal-first planner, explicit confirmation of data erase, background flashing and honest failure reporting.
- First full hardware flash on the SM-N920I (Galaxy Note 5): BL + AP + CP + full CSC, about 4.9 GB in about 2.5 minutes.
