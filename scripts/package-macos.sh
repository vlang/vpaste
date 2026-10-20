#!/usr/bin/env bash
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$ROOT"
[[ "$(uname -s)/$(uname -m)" == Darwin/arm64 ]] || { echo 'macOS arm64 is required.' >&2; exit 1; }
VERSION=${KEYPOUR_VERSION:?Build with scripts/build.sh --package}
[[ "$VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+([-.][A-Za-z0-9.]+)?$ ]] || exit 2
STAGE="$ROOT/build/keypour-$VERSION-macos-arm64"
rm -rf "$STAGE"
mkdir -p "$STAGE/licenses"
cp dist/keypour README.md LICENSE config.example.json BUILDING.md VALIDATION.md "$STAGE/"
cp packaging/Keypour.command "$STAGE/"
cp -R build/runtime-notices/. "$STAGE/licenses/"
chmod +x "$STAGE/keypour" "$STAGE/Keypour.command"
# This is a local ad-hoc signature, not Developer ID signing/notarization.
codesign --force --sign - --identifier org.keypour.cli "$STAGE/keypour"
codesign --verify --strict --verbose=2 "$STAGE/keypour"
[[ "$(lipo -archs "$STAGE/keypour")" == arm64 ]] || { echo 'Unexpected binary architecture.' >&2; exit 1; }
COPYFILE_DISABLE=1 tar -czf "dist/keypour-$VERSION-macos-arm64.tar.gz" -C build "$(basename "$STAGE")"
printf 'Packaged %s\n' "$ROOT/dist/keypour-$VERSION-macos-arm64.tar.gz"
