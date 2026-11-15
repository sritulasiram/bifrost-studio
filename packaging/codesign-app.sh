#!/bin/sh
# Signs Bifrost Studio.app for local (ad hoc) or release (Developer ID + hardened runtime).
# Usage: codesign-app.sh "/path/Bifrost Studio.app" [SIGNING_IDENTITY]
set -eu
APP="$1"
IDENTITY="${2:-${BIFROST_SIGN_IDENTITY:--}}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
"${SCRIPT_DIR}/fix-libusb-rpaths.sh" "$APP"

if [ "$IDENTITY" = "-" ]; then
    RUNTIME=""
    echo "Signing ad hoc (not notarizable)"
else
    RUNTIME="--options runtime"
    echo "Signing for notarization: $IDENTITY"
fi

CLI_ID="${BIFROST_CLI_IDENTIFIER:-studio.bifrost.cli}"
AGENT_ID="${BIFROST_AGENT_LABEL:-local.bifrost.agent}"
USBD_ID="${BIFROST_USBD_LABEL:-local.bifrost.usbd}"

sign() {
    # shellcheck disable=SC2068
    codesign --force --sign "$IDENTITY" --timestamp $RUNTIME "$@"
}

if [ -f "$APP/Contents/Frameworks/libusb-1.0.0.dylib" ]; then
    sign "$APP/Contents/Frameworks/libusb-1.0.0.dylib"
fi
if [ -f "$APP/Contents/Frameworks/libbifrost.1.dylib" ]; then
    sign "$APP/Contents/Frameworks/libbifrost.1.dylib"
fi

sign -i "$CLI_ID" "$APP/Contents/MacOS/bifrost-cli"
sign -i "$AGENT_ID" "$APP/Contents/MacOS/bifrost-agent"
sign -i "$USBD_ID" "$APP/Contents/MacOS/bifrost-usbd"

if [ -d "$APP/Contents/Resources/platform-tools" ]; then
    find "$APP/Contents/Resources/platform-tools" -type f -perm +111 2>/dev/null | while read -r tool; do
        case "$(file -b "$tool")" in *Mach-O*) sign "$tool" ;; esac
    done
fi

sign "$APP/Contents/MacOS/BifrostStudio"
sign "$APP"

codesign --verify --deep --strict --verbose=2 "$APP"
echo "codesign: OK"