#!/bin/bash
set -euo pipefail
[ "${REQUIRE_RELEASE_SIGNATURES:-false}" = true ] || exit 0
: "${MACOS_SIGNING_P12:?missing Developer ID certificate}"
: "${MACOS_SIGNING_PASSWORD:?missing certificate password}"
: "${MACOS_SIGNING_IDENTITY:?missing Developer ID identity}"
: "${APPLE_ID:?missing notarization Apple ID}"
: "${APPLE_APP_PASSWORD:?missing notarization password}"
: "${APPLE_TEAM_ID:?missing Apple team ID}"
signing_dir=$(mktemp -d "$RUNNER_TEMP/ngpost-signing.XXXXXX")
keychain="$signing_dir/signing.keychain-db"
cleanup() { security delete-keychain "$keychain" 2>/dev/null || true; rm -rf -- "$signing_dir"; }
trap cleanup EXIT
printf '%s' "$MACOS_SIGNING_P12" | base64 --decode > "$signing_dir/cert.p12"
keychain_password=$(uuidgen)
security create-keychain -p "$keychain_password" "$keychain"
security set-keychain-settings -lut 21600 "$keychain"
security unlock-keychain -p "$keychain_password" "$keychain"
security import "$signing_dir/cert.p12" -k "$keychain" -P "$MACOS_SIGNING_PASSWORD" -T /usr/bin/codesign
security set-key-partition-list -S apple-tool:,apple:,codesign: -s -k "$keychain_password" "$keychain"
security list-keychains -d user -s "$keychain" login.keychain-db
while IFS= read -r -d '' component; do
    codesign --force --options runtime --timestamp --keychain "$keychain" --sign "$MACOS_SIGNING_IDENTITY" "$component"
done < <(find src/ngPost.app -depth \( -name '*.dylib' -o -name '*.framework' -o -name '*.app' \) -print0)
codesign --verify --deep --strict src/ngPost.app
ditto -c -k --keepParent src/ngPost.app "$signing_dir/notarize.zip"
xcrun notarytool submit "$signing_dir/notarize.zip" --apple-id "$APPLE_ID" --password "$APPLE_APP_PASSWORD" --team-id "$APPLE_TEAM_ID" --wait --output-format json > "$signing_dir/result.json"
python3 -c 'import json,sys; sys.exit(json.load(open(sys.argv[1]))["status"] != "Accepted")' "$signing_dir/result.json"
xcrun stapler staple src/ngPost.app
xcrun stapler validate src/ngPost.app
