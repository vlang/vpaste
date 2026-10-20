#!/usr/bin/env bash
# Opening this file in Finder starts Keypour in Terminal, with visible status.
set -euo pipefail
DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
"$DIR/keypour" "$@"
