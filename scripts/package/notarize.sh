#!/usr/bin/env bash
# Submits a Developer ID-signed package to Apple's notary service and staples the ticket.
#   scripts/package/notarize.sh <Nexus.dmg|Nexus.zip>
# Requires a notarytool keychain profile created once with:
#   xcrun notarytool store-credentials nexus-notary --apple-id <id> --team-id <TEAM> --password <app-specific-password>
# NEXUS_NOTARY_PROFILE selects the profile (default: nexus-notary).
set -euo pipefail
ARTIFACT="${1:?usage: notarize.sh <dmg-or-zip>}"
PROFILE="${NEXUS_NOTARY_PROFILE:-nexus-notary}"
xcrun notarytool submit "$ARTIFACT" --keychain-profile "$PROFILE" --wait
if [[ "$ARTIFACT" == *.dmg ]]; then
  xcrun stapler staple "$ARTIFACT"
  xcrun stapler validate "$ARTIFACT"
fi
spctl --assess --type open --context context:primary-signature --verbose "$ARTIFACT" || true
echo "Notarization finished for $ARTIFACT"
