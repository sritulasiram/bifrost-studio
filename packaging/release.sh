#!/bin/sh
# Release pipeline: sign app → DMG → optional notarization.
# Usage: release.sh BUILD_DIR [VERSION]
set -eu
BUILD="${1:?build directory}"
VERSION="${2:-1.0.0}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
APP="$BUILD/Bifrost Studio.app"
DMG="$BUILD/BifrostStudio-${VERSION}.dmg"

[ -d "$APP" ] || { echo "No app at $APP — run: cmake --build $BUILD --target BifrostStudio" >&2; exit 1; }

chmod +x "$ROOT/packaging/"*.sh
"$ROOT/packaging/codesign-app.sh" "$APP"
"$ROOT/packaging/make-dmg.sh" "$APP" "$DMG" "$VERSION"

if [ "${BIFROST_NOTARIZE:-}" = "1" ] || [ "${BIFROST_NOTARIZE:-}" = "yes" ]; then
    BIFROST_STAPLE_APP="$APP" "$ROOT/packaging/notarize.sh" "$DMG"
fi

echo "Release artifact: $DMG"