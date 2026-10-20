#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
for tool in cc Xvfb xvfb-run xauth xdotool xclip python3; do
  command -v "$tool" >/dev/null || { echo "Missing integration-test dependency: $tool" >&2; exit 1; }
done
mkdir -p build
cc -std=c11 -O2 -Wall -Wextra -Werror tests/x11_receiver.c -lX11 -lxkbcommon -o build/x11_receiver
cc -std=gnu11 -O2 -Wall -Wextra -Werror -I native tests/x11_clipboard_test.c native/keypour_linux.c \
  -o build/x11-clipboard-test -lX11 -lXtst -lXi -lxkbcommon
xvfb-run -a env -u WAYLAND_DISPLAY XDG_SESSION_TYPE=x11 ./build/x11-clipboard-test
exec python3 tests/integration_linux.py "${KEYPOUR_BINARY:-$PWD/dist/keypour}"
