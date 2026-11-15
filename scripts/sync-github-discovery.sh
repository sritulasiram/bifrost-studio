#!/bin/sh
# Apply GitHub About, topics, and v1.1.0 release notes (requires: gh auth login).
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
REPO='sritulasiram/bifrost-studio'

command -v gh >/dev/null 2>&1 || {
    echo 'Install GitHub CLI (brew install gh) and run: gh auth login' >&2
    exit 1
}

gh repo edit "$REPO" \
    --homepage 'https://github.com/sritulasiram/bifrost-studio/releases/latest' \
    --description 'Samsung firmware flasher for Mac: native Odin alternative for macOS. Flash Galaxy .tar.md5 (BL, AP, CP, CSC) in Download Mode. Beta.' \
    --enable-wiki=false

gh api -X PUT "repos/${REPO}/topics" \
    --input - <<'JSON'
{"names":["samsung","samsung-firmware","galaxy","odin","odin-alternative","heimdall","download-mode","firmware-flasher","flash-firmware","macos","mac","swift","swiftui","android","usb","libusb","pit","bifrost"]}
JSON

if gh release view v1.1.0 >/dev/null 2>&1; then
    gh release edit v1.1.0 --repo "$REPO" --notes-file "$ROOT/packaging/release-notes/v1.1.0.md"
fi

echo "sync-github-discovery: OK"