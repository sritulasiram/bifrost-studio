# Release (P1)

Ship a **notarized** `BifrostStudio-x.y.z.dmg` with **replaceable libusb** in the app bundle.

## Prerequisites

| Item | Purpose |
|------|---------|
| Apple Developer account | Developer ID Application + Developer ID Installer (optional) |
| Team ID | `-DBIFROST_TEAM_ID=XXXXXXXXXX` for XPC peer checks (SEC-5) |
| `libusb-1.0.0.dylib` under `LIBUSB_ROOT` | Universal dylib; see [build-and-install.md](build-and-install.md) |
| Notary credentials | `xcrun notarytool store-credentials BifrostNotary` |

## One-time setup

```sh
# Notarytool keychain profile (app-specific password from appleid.apple.com)
xcrun notarytool store-credentials BifrostNotary \
  --apple-id "you@example.com" \
  --team-id "XXXXXXXXXX" \
  --password "xxxx-xxxx-xxxx-xxxx"
```

Pick identifiers you control:

```sh
cmake -S . -B "$BUILD" \
  -DCMAKE_BUILD_TYPE=Release \
  -DLIBUSB_ROOT="$HOME/.local" \
  -DBIFROST_BUNDLE_ID=studio.bifrost.macos \
  -DBIFROST_TEAM_ID=XXXXXXXXXX \
  -DBIFROST_CLI_IDENTIFIER=studio.bifrost.cli
```

**Trademark (D-6):** complete the [checklist below](#trademark-d-6) before publishing.

## Build and sign

```sh
BUILD=~/Library/Caches/BifrostBuild
cmake --build "$BUILD" --target BifrostStudio
export BIFROST_TEAM_ID=XXXXXXXXXX
export BIFROST_SIGN_IDENTITY="Developer ID Application: Your Name (XXXXXXXXXX)"
chmod +x packaging/preflight-release.sh
./packaging/preflight-release.sh "$BUILD"
./packaging/release.sh "$BUILD" "$(grep -m1 'project(BifrostStudio VERSION' CMakeLists.txt | sed 's/.*VERSION \([0-9.]*\).*/\1/')"
```

Ad-hoc (local only, not Gatekeeper-friendly):

```sh
cmake --build "$BUILD" --target BifrostStudio
./packaging/codesign-app.sh "$BUILD/Bifrost Studio.app"
cmake --build "$BUILD" --target dmg
```

## Notarize

After `release.sh` with Developer ID signing:

```sh
export BIFROST_NOTARY_PROFILE=BifrostNotary
export BIFROST_NOTARIZE=1
./packaging/release.sh "$BUILD" "$(grep -m1 'project(BifrostStudio VERSION' CMakeLists.txt | sed 's/.*VERSION \([0-9.]*\).*/\1/')"
```

Or notarize an existing DMG:

```sh
# Replace 1.1.0 with the version passed to release.sh
BIFROST_STAPLE_APP="$BUILD/Bifrost Studio.app" ./packaging/notarize.sh "$BUILD/BifrostStudio-1.1.0.dmg"
```

## Verify before sharing

```sh
codesign --verify --deep --strict "$BUILD/Bifrost Studio.app"
spctl -a -vv -t install "$BUILD/Bifrost Studio.app"
otool -L "$BUILD/Bifrost Studio.app/Contents/MacOS/bifrost-agent" | grep libusb
# Must show @rpath/libusb-1.0.0.dylib, not a home-directory path
test -f "$BUILD/Bifrost Studio.app/Contents/Frameworks/libusb-1.0.0.dylib"
```

## LGPL (libusb)

The bundle ships `Contents/Frameworks/libusb-1.0.0.dylib`. Users may replace it with their own build of the same soname. Source and licence text: [THIRD-PARTY-NOTICES.txt](../THIRD-PARTY-NOTICES.txt) and `Contents/Resources/libusb-COPYING.txt` when present.

## CI

**Default CI** (`.github/workflows/ci.yml`) builds and tests with ad-hoc signing only.

**Release + notarize** (`.github/workflows/release-notarize.yml`) runs on `workflow_dispatch` or `v*` tags. The job is skipped unless repository secrets are configured.

| Secret / variable | Purpose |
|-------------------|---------|
| `BIFROST_CERTIFICATE_P12` | Base64-encoded `.p12` (Developer ID Application) |
| `BIFROST_CERTIFICATE_PASSWORD` | Import password for the `.p12` |
| `BIFROST_SIGN_IDENTITY` | Full codesign identity string |
| `BIFROST_TEAM_ID` | Apple Team ID (`-DBIFROST_TEAM_ID`) |
| `BIFROST_NOTARY_KEY_ID`, `BIFROST_NOTARY_ISSUER_ID`, `BIFROST_NOTARY_KEY_P8` | Optional App Store Connect API key for `notarytool` (when notarizing in CI) |
| Repository variable `BIFROST_BUNDLE_ID` | Optional; defaults to `studio.bifrost.macos` |

Local notarization still uses a keychain profile (`BIFROST_NOTARY_PROFILE`) via `packaging/notarize.sh`. CI uses the API key env vars documented in that script.

## GitHub discovery

README, release notes, and the repo **About** box should share the same keywords (Samsung firmware, Odin alternative, Download Mode, `.tar.md5`). Checklist and `gh` commands: [packaging/github-discovery.md](../packaging/github-discovery.md).

## Trademark (D-6) {#trademark-d-6}

Complete before the first **public** DMG or GitHub release. Record the date and outcome in release notes and [CHANGELOG.md](../CHANGELOG.md).

### Name: “Bifrost Studio” / “Bifrost”

- [ ] [USPTO TESS](https://tmsearch.uspto.gov/) — class 9 (software), class 42 (SaaS) — note conflicting marks
- [ ] [EUIPO eSearch](https://euipo.europa.eu/eSearch/) — same classes if you distribute in the EU
- [ ] GitHub org/repo name `bifrost-studio` — no active confusing project in the same niche
- [ ] Mac App Store search — no identical flash-tool name (we do not ship on the App Store today)
- [ ] Samsung / Odin — marketing says **not affiliated** (README and in-app About)

### If a conflict appears

1. Rename the product (`BIFROST_BUNDLE_ID`, display name, docs).
2. Or narrow positioning (e.g. “Bifrost Studio for Mac” + distinct icon).
3. Do **not** ship until the maintainer records the decision in `CHANGELOG.md`.

This is a process checklist, not legal advice.