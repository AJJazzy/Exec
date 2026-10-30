#!/usr/bin/env bash
# Signs and packages Nexus.app into a .zip and a .dmg.
#   scripts/package/package-macos.sh <path/to/Nexus.app> [output-dir]
#
# Signing identity: NEXUS_CODESIGN_IDENTITY (default "-" = ad hoc signature).
#   Ad hoc signing lets the app run on the machine that built it; Gatekeeper will block it on other
#   Macs unless it is signed with a "Developer ID Application" certificate and notarized
#   (scripts/package/notarize.sh). This script never claims notarization.
set -euo pipefail
APP="${1:?usage: package-macos.sh <Nexus.app> [output-dir]}"
OUT="${2:-dist}"
IDENTITY="${NEXUS_CODESIGN_IDENTITY:--}"
VERSION="$(cat "$(dirname "$0")/../../VERSION")"
ENTITLEMENTS="$(dirname "$0")/Nexus.entitlements"

mkdir -p "$OUT"
STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
ditto "$APP" "$STAGE/Nexus.app"
APP_STAGED="$STAGE/Nexus.app"

TIMESTAMP=()
if [[ "$IDENTITY" != "-" ]]; then TIMESTAMP=(--timestamp); fi

# Sign inside-out: the helper first, then the bundle. Both use the Hardened Runtime.
codesign --force --options runtime ${TIMESTAMP[@]+"${TIMESTAMP[@]}"} --entitlements "$ENTITLEMENTS" --sign "$IDENTITY" "$APP_STAGED/Contents/MacOS/nexus-runner"
codesign --force --options runtime ${TIMESTAMP[@]+"${TIMESTAMP[@]}"} --entitlements "$ENTITLEMENTS" --sign "$IDENTITY" "$APP_STAGED"
codesign --verify --strict --deep --verbose=2 "$APP_STAGED"
codesign --display --verbose=2 "$APP_STAGED" 2>&1 | grep -E "Identifier|Format|flags|TeamIdentifier|Signature" || true

ZIP="$OUT/Nexus-$VERSION-macOS.zip"
DMG="$OUT/Nexus-$VERSION-macOS.dmg"
ditto -c -k --keepParent "$APP_STAGED" "$ZIP"
ln -s /Applications "$STAGE/Applications"
hdiutil create -volname "Nexus $VERSION" -srcfolder "$STAGE" -ov -format UDZO "$DMG" >/dev/null
( cd "$OUT" && shasum -a 256 "$(basename "$ZIP")" "$(basename "$DMG")" > SHA256SUMS )
if [[ "$IDENTITY" == "-" ]]; then
  echo "Packaged with an AD HOC signature (not Developer ID, not notarized): $ZIP, $DMG"
else
  echo "Packaged and signed with '$IDENTITY' (not yet notarized): $ZIP, $DMG"
fi
