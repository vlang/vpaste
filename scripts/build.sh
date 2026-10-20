#!/usr/bin/env bash
# Build locally; --package additionally makes the native release archive/AppImage.
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$ROOT"
PACKAGE=0
case "${1:-}" in
  '') ;;
  --package) PACKAGE=1 ;;
  *) echo 'Usage: scripts/build.sh [--package]' >&2; exit 2 ;;
esac
[[ $# -le 1 ]] || { echo 'Too many arguments.' >&2; exit 2; }
if [[ -n "${KEYPOUR_V:-}" ]]; then
  V=$KEYPOUR_V
elif [[ -x "$ROOT/.tools/v/v" ]]; then
  V="$ROOT/.tools/v/v"
else
  V=$(command -v v) || { echo 'Run scripts/bootstrap-v.sh or set KEYPOUR_V.' >&2; exit 1; }
fi
mkdir -p build dist
case "$(uname -s)/$(uname -m)" in
  Linux/x86_64)
    CC=${CC:-gcc}
    "$CC" -std=gnu11 -O2 -Wall -Wextra -fPIC -I native -c native/keypour_linux.c -o build/native.o
    "$V" -cc "$CC" -prod -o dist/keypour src
    ;;
  Darwin/arm64)
    CC=${CC:-clang}
    export MACOSX_DEPLOYMENT_TARGET=12.0
    "$CC" -std=c11 -O2 -Wall -Wextra -arch arm64 -mmacosx-version-min=12.0 \
      -I native -c native/keypour_macos.m -o build/native.o
    "$V" -cc "$CC" -prod -cflags '-arch arm64 -mmacosx-version-min=12.0' -o dist/keypour src
    ;;
  *) echo 'Use Linux x86_64, macOS arm64, or scripts/build.ps1 on Windows x64.' >&2; exit 1 ;;
esac
"$V" -cc "$CC" test core
./dist/keypour --version
./dist/keypour --help >/dev/null
if [[ "$PACKAGE" == 1 ]]; then
  BINARY_VERSION=$(./dist/keypour --version | awk '{print $NF}')
  SOURCE_VERSION=$(python3 scripts/project-version.py)
  VERSION=${KEYPOUR_VERSION:-$BINARY_VERSION}
  [[ "$VERSION" == "$BINARY_VERSION" && "$VERSION" == "$SOURCE_VERSION" ]] || { echo 'Binary, source, and package versions must match.' >&2; exit 1; }
  [[ "$VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+([-.][A-Za-z0-9.]+)?$ ]] || { echo "Invalid release version: $VERSION" >&2; exit 1; }
  export KEYPOUR_VERSION=$VERSION KEYPOUR_V=$V
  python3 scripts/runtime-notices.py "$V" build/runtime-notices
  if [[ "$(uname -s)" == Linux ]]; then
    scripts/package-linux.sh
  else
    scripts/package-macos.sh
  fi
fi
printf 'Built %s\n' "$ROOT/dist/keypour"
