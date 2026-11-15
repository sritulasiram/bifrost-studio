#!/bin/sh
# Submit a DMG or zip to Apple notarization and staple the ticket to the app inside.
# Requires: Xcode CLT, Apple ID app-specific password, Developer ID signing already done.
#
# Environment:
#   BIFROST_NOTARY_PROFILE  — keychain profile from: xcrun notarytool store-credentials
#   BIFROST_SIGN_IDENTITY   — Developer ID Application identity (for verify before submit)
#   BIFROST_STAPLE_APP      — optional .app path to staple after approval (else DMG only)
#
# Usage:
#   notarize.sh BifrostStudio-1.0.0.dmg
#   notarize.sh "Bifrost Studio.app"
set -eu
ARTIFACT="$1"
PROFILE="${BIFROST_NOTARY_PROFILE:-BifrostNotary}"
STAPLE_APP="${BIFROST_STAPLE_APP:-}"

submit_path="$ARTIFACT"
cleanup_zip=""
if [ -d "$ARTIFACT" ]; then
    cleanup_zip="$(mktemp "${TMPDIR:-/tmp}/bifrost-notarize.XXXXXX.zip")"
    ditto -c -k --keepParent "$ARTIFACT" "$cleanup_zip"
    submit_path="$cleanup_zip"
fi

if [ -n "${BIFROST_NOTARY_KEY_ID:-}" ] && [ -n "${BIFROST_NOTARY_ISSUER_ID:-}" ] \
    && [ -n "${BIFROST_NOTARY_KEY_PATH:-}" ]; then
    echo "Submitting $submit_path for notarization (API key)..."
    xcrun notarytool submit "$submit_path" --wait \
        --key "$BIFROST_NOTARY_KEY_PATH" \
        --key-id "$BIFROST_NOTARY_KEY_ID" \
        --issuer "$BIFROST_NOTARY_ISSUER_ID"
else
    echo "Submitting $submit_path for notarization (profile $PROFILE)..."
    xcrun notarytool submit "$submit_path" --keychain-profile "$PROFILE" --wait
fi

if [ -n "$cleanup_zip" ]; then
    rm -f "$cleanup_zip"
fi

if [ -n "$STAPLE_APP" ] && [ -d "$STAPLE_APP" ]; then
    echo "Stapling ticket to $STAPLE_APP"
    xcrun stapler staple "$STAPLE_APP"
    xcrun stapler validate "$STAPLE_APP"
elif [ -f "$ARTIFACT" ] && [ "${ARTIFACT##*.}" = "dmg" ]; then
    echo "Stapling ticket to $ARTIFACT"
    xcrun stapler staple "$ARTIFACT"
    xcrun stapler validate "$ARTIFACT"
fi

echo "Notarization complete."