#!/usr/bin/env bash
# Archive committed sources, not local tools, outputs, or untracked files.
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$ROOT"
VERSION=${1:?Usage: scripts/package-source.sh VERSION}
[[ "$VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+([-.][A-Za-z0-9.]+)?$ ]] || { echo 'Invalid release version.' >&2; exit 2; }
[[ "$(git rev-parse --show-toplevel)" == "$ROOT" ]] || { echo 'Project must be its own Git repository.' >&2; exit 1; }
[[ -z "$(git status --porcelain --untracked-files=normal)" ]] || { echo 'Commit source changes before making a source release.' >&2; exit 1; }
mkdir -p dist
git archive --format=tar --prefix="keypour-$VERSION/" HEAD | gzip -n > "dist/keypour-$VERSION-source.tar.gz"
printf 'Packaged %s\n' "$ROOT/dist/keypour-$VERSION-source.tar.gz"
