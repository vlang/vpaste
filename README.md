# Keypour

**Copy text → press a hotkey → click a VNC/IPMI console → type.**

A small resident command-line application written in **V**, with native input
adapters for Windows, Linux/X11, and macOS. It emits keyboard events instead of
asking the destination to paste the clipboard. No server, browser extension,
Qt, Electron, or runtime V installation is needed by release users.

This is an original implementation inspired by the auto-type approach in
[KeePassXC](https://keepassxc.org/docs/KeePassXC_UserGuide#_auto_type). No
KeePassXC source is copied, linked, or distributed. Keypour uses the MIT license;
third-party runtime/library licenses are included in release packages.

## Use

Start the program and keep its terminal open:

```sh
# Linux
chmod +x keypour-0.1.0-linux-amd64.AppImage
./keypour-0.1.0-linux-amd64.AppImage

# macOS: run the extracted binary, or double-click Keypour.command
./keypour
```

On Windows, open `keypour.exe` from the extracted ZIP, or run it in PowerShell.

1. Copy plain text in any application.
2. Press **Ctrl+Alt+V** and release the keys. On macOS, Alt means Option.
3. Click **inside the input area** of the remote console. The click passes through.
4. After a short delay, Keypour types the clipboard snapshot into that window.

**Escape** or the hotkey again cancels. Press **Ctrl+C in Keypour's terminal**
to exit. An additional click, a held modifier, or a change of the focused
window stops further typing. Text is captured at hotkey time; changing the
clipboard after arming does not change the pending text.

Newlines send **Enter**, including a trailing newline. Tabs send **Tab**. For
shell commands these have their normal effects. To omit final line breaks,
use `--trim-trailing-newlines`. CRLF and lone CR are normalized to one Enter.
Everything else is literal: `{ENTER}`, `%`, `$`, `+`, and braces are ordinary
characters, with no KeePass macro expansion or shell interpolation.

## Settings

For a slower console and a different shortcut:

```sh
./keypour --hotkey Ctrl+Shift+F8 --interval-ms 80 --hold-ms 12
```

The normal pause between characters is `key_hold_ms + interval_ms`, plus
scheduling/native API overhead. Defaults are 8 ms hold and 35 ms pause, about
23 characters/second before overhead. An additional 150 ms follows each
newline. Reduce the interval for faster typing; raise it for consoles that
drop or reorder keys.

Create a configuration file without overwriting an existing one:

```sh
./keypour --init-config
./keypour --check-config
./keypour --print-config
./keypour --config ./my-config.json
```

Default locations:

| OS | Path |
| --- | --- |
| Windows | `%APPDATA%\keypour\config.json` |
| Linux | `$XDG_CONFIG_HOME/keypour/config.json`, otherwise `~/.config/keypour/config.json` |
| macOS | `~/Library/Application Support/keypour/config.json` |

An absent default file uses built-in defaults. An explicitly named missing
file is an error. Command-line values override the file. Restart Keypour to
apply configuration-file changes. Unknown fields and out-of-range values
are rejected. See [`config.example.json`](config.example.json).

```json
{
  "hotkey": "Ctrl+Alt+V",
  "interval_ms": 35,
  "key_hold_ms": 8,
  "start_delay_ms": 400,
  "line_delay_ms": 150,
  "arm_timeout_ms": 15000,
  "max_chars": 100000,
  "input_mode": "keys",
  "trim_trailing_newlines": false
}
```

| Setting | CLI option | Accepted range |
| --- | --- | --- |
| `hotkey` | `--hotkey` | Ctrl/Alt/Shift/Super plus A–Z, 0–9, or F1–F12 |
| `interval_ms` | `--interval-ms` | 0–10,000 ms after a character |
| `key_hold_ms` | `--hold-ms` | 1–100 ms key down |
| `start_delay_ms` | `--start-delay-ms` | 100–10,000 ms after click |
| `line_delay_ms` | `--line-delay-ms` | 0–30,000 ms extra after Enter |
| `arm_timeout_ms` | `--arm-timeout-ms` | 1,000–300,000 ms to select a target |
| `max_chars` | `--max-chars` | 1–1,000,000 Unicode scalars |
| `input_mode` | `--mode` | `keys` or `unicode` |
| `trim_trailing_newlines` | `--trim-trailing-newlines` | Boolean; false by default |

Hotkeys need at least one of Ctrl, Alt, or Super. Aliases include Control,
Option, Win, Cmd, and Command. A hotkey reserved by the OS/another application
may be unavailable; choose another. On macOS, hotkey letter/digit names use
ANSI physical key positions, so they stay usable with a non-Latin text layout.

## Compatibility

| Target | Package | Requirements |
| --- | --- | --- |
| Windows 10 amd64 | ZIP containing `keypour.exe` | Windows 10 version 1607+; also intended for Windows 11 |
| Linux amd64 | x86_64 AppImage | X11/Xorg, XTEST, XInput 2.1+; official CI builds on Ubuntu 22.04, glibc 2.35+ baseline |
| macOS arm64 | tar.gz with `keypour` and `Keypour.command` | Apple Silicon, macOS 12+, Accessibility and Input Monitoring |
| Source | tar.gz | V 0.5.2, a C compiler, native development headers |

`amd64` and `x86_64` name the same CPU architecture. Linux has no common OS
version called “10”; this project's compatibility is expressed through
architecture, display server, and glibc baseline. It does not promise support
for every Linux distribution version. Locally built packages inherit the
builder's libc baseline; use the GitHub workflow for the stated release target.

### Physical keys (default)

`keys` mode finds real keys and their required modifiers in the active local
keyboard layout. It uses Windows scan codes, macOS virtual keycodes, or X11
XTEST keycodes. This is the preferred mode for VNC, IPMI KVM, browser-hosted
consoles, and VM viewers that forward key-down/key-up events.

**Local and remote layouts must agree.** Test punctuation such as `@`, `:`,
`/`, `\\`, quotes, and braces before a long paste. A viewer/firmware may apply
its own keyboard mapping or reserve shortcuts. No host-only tool can infer or
control every remote keyboard layout or guarantee every firmware accepts
synthetic events. Windows also limits input injection into higher-integrity
applications; match the viewer's elevation when necessary. Secure desktops
and OS login screens are outside this tool's scope.

Before emitting the first character, Keypour verifies that the complete text
can be represented. Characters requiring an unsupported dead-key composition,
an IME, or an unavailable local key are rejected rather than silently dropped.
The focus and layout are checked again during typing. Caps Lock is preserved;
changing the layout or lock state during typing stops the operation.

### Unicode mode (opt-in)

```sh
./keypour --mode unicode
```

Windows sends UTF-16 Unicode input events and macOS attaches Unicode text to
keyboard events; Enter and Tab remain physical keys. Many ordinary text fields
accept these events, but remote-console viewers may ignore them. On X11,
layout-mapped characters still use physical keys; other scalars temporarily
use an unused keycode with a Unicode keysym. **That X11 fallback is experimental:**
slow recipients may miss a temporary mapping, and many remote consoles cannot
forward it. Default `keys` mode does not remap the system keymap.

### Linux display server

This version requires an **X11/Xorg session** and rejects native Wayland
sessions, including an XWayland `DISPLAY` inside one. That avoids pretending a
global shortcut/focus guard covers native Wayland windows. A future Wayland
backend would use portal-approved remote-desktop/input access; it is not
implemented here. Current KeePassXC development is separate and is not a
compatibility promise for Keypour.

If the AppImage cannot mount because FUSE is unavailable:

```sh
./keypour-0.1.0-linux-amd64.AppImage --appimage-extract
./squashfs-root/AppRun
```

### macOS permissions

Keep the executable at a stable path. Grant it (or the responsible terminal
application shown by macOS) access under **System Settings → Privacy & Security
→ Accessibility** and **Input Monitoring**, then relaunch. The command-line
launcher keeps a terminal available for status and cancellation. Secure Keyboard
Entry can prevent global event observation. The downloadable build is ad-hoc
signed, not Apple Developer ID signed or notarized; use your organization's
normal policy for locally built/unnotarized software. An ad-hoc signature does
not grant Accessibility permission.

## Build and release

See [`BUILDING.md`](BUILDING.md) for dependency installation, native builds,
packages, and CI. Typical build commands:

```sh
./scripts/bootstrap-v.sh
./scripts/build.sh
./scripts/build.sh --package
```

```powershell
.\scripts\bootstrap-v.ps1
.\scripts\build.ps1
.\scripts\build.ps1 -Package
```

The GitHub Actions workflow builds all three native targets on their own
runners, runs the portable tests, and uploads packages. A pushed version tag
such as `v0.1.0` additionally creates a GitHub Release with the three packages,
a source archive, and `SHA256SUMS`. A manual workflow run builds artifacts
without publishing a release. The workflow does not require a personal access
token. No repository or release is created merely by unpacking this project.

## Code and tests

- `core/`: V configuration, hotkey parser, UTF-8/literal-text validation, and
  deterministic session transitions.
- `src/`: V executable, event loop, preflight, pacing, and C ABI declarations.
- `native/`: OS-only clipboard, global input, target-window, and injection APIs.
- `scripts/`, `packaging/`, `.github/workflows/`: pinned toolchain, packaging,
  platform build jobs, and release uploads.
- `tests/`: automated X11/Win32 receivers and documented macOS acceptance steps.

```sh
v -cc gcc test core
./scripts/integration-linux.sh
```

The integration test uses a private Xvfb server and synthetic test text, not
your desktop or clipboard. It tests events received by a separate application.
See [`VALIDATION.md`](VALIDATION.md) for the checks actually completed on this
project and the remaining native-platform/remote-console checks.

The app never logs clipboard contents or writes them to configuration/disk.
Clipboard snapshots are bounded and transient; main payload/native buffers
are wiped when released. This is best-effort memory hygiene, not a guarantee
that the OS, runtime, clipboard owner, or destination holds no copies.

## API references and provenance

The adapters were written for this project using documented OS APIs:

- [KeePassXC auto-type user guide](https://keepassxc.org/docs/KeePassXC_UserGuide#_auto_type)
  — design inspiration; no reused implementation.
- [Windows SendInput](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-sendinput)
  and [RegisterHotKey](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-registerhotkey).
- [Windows ToUnicodeEx](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-tounicodeex)
  — non-mutating translation requires Windows 10 1607+.
- [XTEST specification](https://www.x.org/releases/X11R7.5/doc/man/man3/XTestFakeKeyEvent.3.html)
  and [XKB library](https://www.x.org/releases/current/doc/libX11/XKB/xkblib.html).
- [Apple CGEvent taps](https://developer.apple.com/documentation/coregraphics/cgevent/tapcreate(tap:place:options:eventsofinterest:callback:userinfo:))
  and [UCKeyTranslate](https://developer.apple.com/documentation/coreservices/1390584-uckeytranslate).
- [V C interoperability](https://docs.vlang.io/v-and-c.html).

License: [MIT](LICENSE). Release packages also carry `licenses/` (inside the AppImage under `usr/share/doc/keypour/licenses/`).
