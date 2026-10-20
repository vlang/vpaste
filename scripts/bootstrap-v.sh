#!/usr/bin/env bash
# Fetch a release-pinned V compiler into the repository's .tools directory.
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
VERSION=0.5.2
case "$(uname -s)/$(uname -m)" in
  Linux/x86_64) ASSET=v_linux.zip; SHA=86caf9e70c3342d48ef19eb4f6c47b709f18c90ae86255520d5c29df6b482e23 ;;
  Darwin/arm64) ASSET=v_macos_arm64.zip; SHA=e539a8dc3aeea47267f3cf00c25c4f0a364d8037fb13f5379d2a574a7abac8ee ;;
  *) echo 'bootstrap-v.sh supports Linux x86_64 and macOS arm64.' >&2; exit 1 ;;
esac
mkdir -p "$ROOT/.tools"
ARCHIVE="$ROOT/.tools/$ASSET"
if [[ ! -f "$ARCHIVE" ]]; then
  curl --fail --location --retry 3 --proto '=https' --tlsv1.2 \
    "https://github.com/vlang/v/releases/download/$VERSION/$ASSET" -o "$ARCHIVE.part"
  mv "$ARCHIVE.part" "$ARCHIVE"
fi
if command -v sha256sum >/dev/null 2>&1; then
  ACTUAL=$(sha256sum "$ARCHIVE" | cut -d ' ' -f1)
else
  ACTUAL=$(shasum -a 256 "$ARCHIVE" | cut -d ' ' -f1)
fi
[[ "$ACTUAL" == "$SHA" ]] || { echo "V archive SHA-256 mismatch: $ARCHIVE" >&2; exit 1; }
if [[ ! -x "$ROOT/.tools/v/v" ]]; then
  unzip -q "$ARCHIVE" -d "$ROOT/.tools"
  chmod +x "$ROOT/.tools/v/v"
fi
ACTUAL_VERSION=$("$ROOT/.tools/v/v" version)
[[ "$ACTUAL_VERSION" == "V $VERSION "* || "$ACTUAL_VERSION" == "V $VERSION" ]] || { echo "Unexpected compiler in .tools/v: $ACTUAL_VERSION" >&2; exit 1; }
printf '%s\n' "$ACTUAL_VERSION"
printf 'Compiler: %s\n' "$ROOT/.tools/v/v"
