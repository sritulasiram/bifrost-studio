#!/bin/sh
# Install a local dev build to ~/Applications and launch it.
# Usage: install-local.sh [BUILD_DIR]
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${1:-$HOME/Library/Caches/BifrostBuild-ci}"
SOURCE="$BUILD/Bifrost Studio.app"
DEST="$HOME/Applications/Bifrost Studio.app"

[ -d "$SOURCE" ] || {
    echo "No app at $SOURCE — run: cmake --build $BUILD --target BifrostStudio" >&2
    exit 1
}

osascript -e 'tell application "Bifrost Studio" to quit' 2>/dev/null || true
pkill -x BifrostStudio 2>/dev/null || true
sleep 1

rm -rf "$DEST"
ditto "$SOURCE" "$DEST"
chmod +x "$ROOT/packaging/"*.sh 2>/dev/null || true
"$ROOT/packaging/codesign-app.sh" "$DEST" 2>/dev/null || "$ROOT/packaging/codesign-app.sh" "$DEST" -
xattr -cr "$DEST"

LSREG="/System/Library/Frameworks/CoreServices.framework/Versions/A/Frameworks/LaunchServices.framework/Versions/A/Support/lsregister"
if [ -x "$LSREG" ]; then
    "$LSREG" -f -R -trusted "$DEST" >/dev/null 2>&1 || "$LSREG" -f "$DEST" >/dev/null 2>&1 || true
fi

echo "Installed: $DEST"
open -a "$DEST"