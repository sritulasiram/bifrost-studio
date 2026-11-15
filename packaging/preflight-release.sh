#!/bin/sh
# Preflight before release.sh / notarization. Usage: preflight-release.sh BUILD_DIR
set -eu
BUILD="${1:?build directory — e.g. ~/Library/Caches/BifrostBuild}"
APP="$BUILD/Bifrost Studio.app"

fail() { echo "preflight: $*" >&2; exit 1; }

[ -d "$APP" ] || fail "missing $APP — cmake --build $BUILD --target BifrostStudio"

[ -f "$APP/Contents/Resources/Localizable.xcstrings" ] \
    || fail "missing Localizable.xcstrings — rebuild BifrostStudio"

codesign --verify --deep --strict "$APP" 2>/dev/null || fail "codesign verify failed (sign the app first)"

for bin in BifrostStudio bifrost-agent bifrost-cli bifrost-usbd; do
    [ -x "$APP/Contents/MacOS/$bin" ] || fail "missing $bin"
done

USB="$APP/Contents/Frameworks/libusb-1.0.0.dylib"
[ -f "$USB" ] || fail "missing bundled libusb at Contents/Frameworks/libusb-1.0.0.dylib"

if otool -L "$APP/Contents/MacOS/bifrost-agent" | grep -q "$HOME"; then
    fail "bifrost-agent links libusb with a home-directory path — run packaging/fix-libusb-rpaths.sh"
fi

IDENT="$(codesign -dv "$APP" 2>&1 | sed -n 's/.*Identifier=//p' | head -1)"
echo "Bundle ID: $IDENT"

if [ -z "${BIFROST_TEAM_ID:-}" ]; then
    echo "preflight: warn — BIFROST_TEAM_ID unset (peer checks are identifier-only, SEC-4)"
fi

if [ -z "${BIFROST_SIGN_IDENTITY:-}" ] || [ "$BIFROST_SIGN_IDENTITY" = "-" ]; then
    echo "preflight: warn — ad-hoc signing; set BIFROST_SIGN_IDENTITY for a distributable build"
else
    echo "Signing identity: $BIFROST_SIGN_IDENTITY"
fi

echo "preflight: OK"