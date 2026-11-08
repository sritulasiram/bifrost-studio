#!/bin/sh
# Builds a compressed DMG containing the app, an Applications link and the
# read-me. Usage: make-dmg.sh "/path/Bifrost Studio.app" OUTPUT.dmg VERSION
set -eu
APP="$1"; OUT="$2"; VERSION="$3"
HERE="$(cd "$(dirname "$0")" && pwd)"
[ -d "$APP" ] || { echo "no app at $APP" >&2; exit 1; }
codesign --verify --strict "$APP"
STAGE="$(mktemp -d "${TMPDIR:-/tmp}/bifrost-dmg.XXXXXX")"
trap 'rm -rf "$STAGE"' EXIT
ditto "$APP" "$STAGE/Bifrost Studio.app"
ln -s /Applications "$STAGE/Applications"
cp "$HERE/Read Me First.txt" "$STAGE/Read Me First.txt"
rm -f "$OUT"
hdiutil create -volname "Bifrost Studio $VERSION" -srcfolder "$STAGE" \
    -fs HFS+ -format UDZO -imagekey zlib-level=9 -ov "$OUT" >/dev/null
hdiutil verify "$OUT" >/dev/null
echo "$OUT"
