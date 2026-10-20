#!/usr/bin/env bash
# Called by build.sh --package; requires Debian/Ubuntu and an already-built binary.
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$ROOT"
[[ "$(uname -s)/$(uname -m)" == Linux/x86_64 ]] || { echo 'Linux x86_64 is required.' >&2; exit 1; }
VERSION=${KEYPOUR_VERSION:?Build with scripts/build.sh --package}
[[ "$VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+([-.][A-Za-z0-9.]+)?$ ]] || exit 2
mkdir -p .tools dist
DEPLOY_VERSION=1-alpha-20251107-1
DEPLOY_SHA=c20cd71e3a4e3b80c3483cef793cda3f4e990aca14014d23c544ca3ce1270b4d
DEPLOY_TOOL="$ROOT/.tools/linuxdeploy-x86_64.AppImage"
if [[ ! -f "$DEPLOY_TOOL" ]]; then
  curl --fail --location --retry 3 --proto '=https' --tlsv1.2 \
    "https://github.com/linuxdeploy/linuxdeploy/releases/download/$DEPLOY_VERSION/linuxdeploy-x86_64.AppImage" -o "$DEPLOY_TOOL.part"
  mv "$DEPLOY_TOOL.part" "$DEPLOY_TOOL"
fi
ACTUAL=$(sha256sum "$DEPLOY_TOOL" | cut -d ' ' -f1)
[[ "$ACTUAL" == "$DEPLOY_SHA" ]] || { echo 'linuxdeploy SHA-256 mismatch.' >&2; exit 1; }
chmod +x "$DEPLOY_TOOL"
APPDIR="$ROOT/build/Keypour.AppDir"
rm -rf "$APPDIR"
mkdir -p "$APPDIR/usr/share/doc/keypour/licenses"
cp README.md LICENSE config.example.json BUILDING.md VALIDATION.md "$APPDIR/usr/share/doc/keypour/"
cp -R build/runtime-notices/. "$APPDIR/usr/share/doc/keypour/licenses/"
cp packaging/APPIMAGE-RUNTIME-LICENSE.txt "$APPDIR/usr/share/doc/keypour/licenses/"
cat > "$APPDIR/usr/share/doc/keypour/licenses/APPIMAGE-RUNTIME-SOURCE.txt" <<'NOTICE'
The AppImage runtime was extracted from the checksum-pinned linuxdeploy
1-alpha-20251107-1 AppImage. Its runtime version identifies this exact source:
https://github.com/AppImage/type2-runtime/commit/61e6688
Source and build instructions (including static runtime dependencies):
https://github.com/AppImage/type2-runtime/tree/61e6688
NOTICE
python3 scripts/linux-dependencies.py dist/keypour "$APPDIR/usr/share/doc/keypour/licenses" > build/linux-libraries.list
LIB_ARGS=()
while IFS= read -r lib; do LIB_ARGS+=(--library "$lib"); done < build/linux-libraries.list
# Reuse the verified tool's embedded runtime. Do not let appimagetool download
# its default mutable 'continuous' runtime during packaging. Clear the original
# tool's update URL/signature/digest so Keypour cannot inherit its update target.
OFFSET=$("$DEPLOY_TOOL" --appimage-offset)
[[ "$OFFSET" =~ ^[0-9]+$ ]] && [[ "$OFFSET" -gt 1024 ]] || { echo 'Invalid AppImage runtime offset.' >&2; exit 1; }
python3 scripts/extract-runtime.py "$DEPLOY_TOOL" "$OFFSET" build/appimage-runtime-x86_64
chmod +x build/appimage-runtime-x86_64
export APPIMAGE_EXTRACT_AND_RUN=1 ARCH=x86_64
export LDAI_RUNTIME_FILE="$ROOT/build/appimage-runtime-x86_64"
export LDAI_OUTPUT="$ROOT/dist/keypour-$VERSION-linux-amd64.AppImage"
export LINUXDEPLOY_OUTPUT_VERSION="$VERSION" LDAI_NO_APPSTREAM=1
"$DEPLOY_TOOL" --appdir "$APPDIR" --executable dist/keypour \
  "${LIB_ARGS[@]}" --desktop-file packaging/keypour.desktop \
  --icon-file packaging/keypour.svg --custom-apprun packaging/AppRun --output appimage
# Every non-glibc native dependency must actually have been bundled.
while IFS= read -r lib; do
  [[ -e "$APPDIR/usr/lib/$(basename "$lib")" ]] || { echo "Missing bundled library: $lib" >&2; exit 1; }
done < build/linux-libraries.list
"$LDAI_OUTPUT" --appimage-extract-and-run --version
printf 'Packaged %s\n' "$LDAI_OUTPUT"
