#!/bin/sh
# Ensures L10n.swift keys exist in Localizable.xcstrings and en.lproj/Localizable.strings (UX-6).
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SWIFT="$ROOT/src/app/Sources/App/L10n.swift"
XCSTRINGS="$ROOT/src/app/Resources/Localizable.xcstrings"
STRINGS="$ROOT/src/app/Resources/en.lproj/Localizable.strings"

[ -f "$SWIFT" ] || { echo "missing $SWIFT" >&2; exit 1; }
[ -f "$XCSTRINGS" ] || { echo "missing $XCSTRINGS" >&2; exit 1; }
[ -f "$STRINGS" ] || { echo "missing $STRINGS" >&2; exit 1; }

missing=0
while IFS= read -r key; do
    [ -n "$key" ] || continue
    if ! grep -Fq "\"$key\"" "$XCSTRINGS"; then
        echo "Localizable.xcstrings: missing key \"$key\"" >&2
        missing=1
    fi
    if ! grep -Fq "\"$key\"" "$STRINGS"; then
        echo "Localizable.strings: missing key \"$key\"" >&2
        missing=1
    fi
done <<EOF
$(sed -n 's/.*String(localized: "\([^"]*\)").*/\1/p' "$SWIFT")
EOF

if [ "$missing" -ne 0 ]; then
    exit 1
fi
echo "check-l10n: OK"