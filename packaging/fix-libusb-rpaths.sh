#!/bin/sh
# Point every Mach-O in the app bundle at the bundled libusb, not the build machine.
set -eu
APP="$1"
FRAMEWORK="$APP/Contents/Frameworks/libusb-1.0.0.dylib"
[ -f "$FRAMEWORK" ] || {
    echo "fix-libusb-rpaths: no bundled libusb (configure with LIBUSB_ROOT containing libusb-1.0.0.dylib)" >&2
    exit 1
}

fix_one() {
    file="$1"
    [ -f "$file" ] || return 0
    case "$(file -b "$file")" in
        *Mach-O*) ;;
        *) return 0 ;;
    esac
    otool -L "$file" 2>/dev/null | awk '/libusb-1\.0(\.0)?\.dylib/ {print $1}' | while read -r old; do
        [ -z "$old" ] && continue
        [ "$old" = "@rpath/libusb-1.0.0.dylib" ] && continue
        install_name_tool -change "$old" "@rpath/libusb-1.0.0.dylib" "$file" 2>/dev/null || true
    done
    # Idempotent: ignore “duplicate rpath”.
    install_name_tool -add_rpath "@executable_path/../Frameworks" "$file" 2>/dev/null || true
}

fix_one "$APP/Contents/MacOS/BifrostStudio"
fix_one "$APP/Contents/MacOS/bifrost-cli"
fix_one "$APP/Contents/MacOS/bifrost-agent"
fix_one "$APP/Contents/MacOS/bifrost-usbd"
fix_one "$APP/Contents/Frameworks/libbifrost.1.dylib"
fix_one "$APP/Contents/Frameworks/libbifrost.dylib"

if [ -d "$APP/Contents/Resources/platform-tools" ]; then
    find "$APP/Contents/Resources/platform-tools" -type f -perm +111 2>/dev/null | while read -r f; do
        fix_one "$f"
    done
fi