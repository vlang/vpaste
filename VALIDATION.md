# Validation record

Project: Keypour 0.1.0. Toolchain: V 0.5.2 (release commit 7647ce1).
Validation performed on 2026-10-20 in a Linux x86_64 workspace.

## Results established here

| Component | Verification | Result |
| --- | --- | --- |
| V application | Linux GCC production compilation, CLI help/version/config checks | Passed |
| V core policy | Hotkey parsing, config defaults/types/ranges/unknown fields, CLI overrides, literal Unicode text, CRLF/CR handling, controls/invalid UTF-8 rejection, size limits, deadlines/cancellation/focus transitions | Passed |
| Linux adapter | GCC C11/gnu11 with `-Wall -Wextra -Werror -pedantic` | Passed |
| Linux application | Ten scenarios against a separate X11 receiver on a private Xvfb server | Passed |
| X11 clipboard protocol | Forced INCR chunks, limits, stale-reply isolation, exact encodings; ten deterministic checks | Passed |
| Windows adapter | MinGW-w64 x86_64 C11 with `-Wall -Wextra -Werror -pedantic` | Passed |
| Windows native behavior | Separate Win32 EDIT receiver under Wine 9 + private Xvfb | Passed, zero test failures |
| Complete Windows application | V cross-compilation with default garbage collector; PE32+ architecture and system-only DLL imports | Passed |
| Windows CLI | `--version`, `--help`, `--check-config` under Wine | Passed |
| Linux AppImage | Native dependency bundling and extract-and-run CLI smoke test | Passed |
| GitHub workflow | YAML parsing and actionlint 1.7.12 | Passed |
| Shell/Python helpers | Shell syntax checks and Python compilation | Passed |
| macOS adapter | Implementation/API review and arm64 build job supplied | Native build/runtime not verified here |

The ten X11 application scenarios exercise:

1. Exact punctuation, upper/lower case, Tab, CRLF and CR normalization, and a
   trailing Enter; no typing before the selection click.
2. A custom hotkey, immutable snapshot after the clipboard changes, and optional
   removal of trailing line breaks.
3. Consumed Escape cancellation.
4. Cancellation by pressing the hotkey a second time.
5. Rejection of unsupported controls before typing any prefix.
6. Whole-text preflight when a later character is unavailable in the layout.
7. Caps Lock handling without toggling the user's lock state.
8. Focus-change cancellation partway through a long paste, with no text
   delivered to the second test window in that test.
9. Armed selection timeout.
10. Large clipboard capture followed by cancellation.

A separate X11 protocol fixture forces INCR even for short text and checks split UTF-8, exact byte limits, oversized headers, understated streaming overflow, late SelectionNotify/partial-INCR timeouts and next-request isolation, wrong encodings, and Latin-1 fallback conversion.

The Windows native suite checks UTF-16/UTF-8 clipboard preservation, Cyrillic,
emoji, size bounds, malformed UTF-16, delayed clipboard-owner timeout/recovery,
hotkey delivery/repeats, physical versus injected click/Escape handling, exact
physical and Unicode text in a separate process, focus checks, and released
modifiers. This is **Wine integration testing**, not proof of behavior on a
physical Windows 10 machine.

## What still requires the target environment

- Run the GitHub workflow in the destination repository to establish that the
  native Windows PowerShell build, macOS arm64 build, and Ubuntu 22.04 package
  jobs pass on those hosted runners. No GitHub repository/release was published
  from this workspace.
- Run macOS permission and event acceptance checks in
  `tests/MANUAL_MACOS.md`. This workspace has no Mac SDK or desktop and did not
  produce a macOS executable.
- Confirm the keyboard layout and a short punctuation sample in the actual
  VNC/IPMI viewer/firmware. Keypour cannot acknowledge delivery inside a remote
  machine. Some viewers reserve hotkeys or reject synthetic/Unicode events.
- Focus checking is performed before each character; checking focus and
  injecting the next event are separate OS operations. It does not guarantee
  atomic delivery across a simultaneous focus switch. It also cannot detect a
  field change inside the same top-level window unless accompanied by a click.
- Linux arbitrary Unicode keymap remapping remains experimental. The default
  physical-key path does not change the system keymap.

## Package baseline and signatures

The locally exercised AppImage is built on Ubuntu 24.04 and imports GLIBC_2.38
symbols. It therefore has a **glibc 2.38+** baseline. The workflow intentionally
builds official Linux release packages on **Ubuntu 22.04**, whose glibc baseline
is 2.35; those CI artifacts have not been produced in this workspace.

Windows packages are unsigned. The macOS packaging script applies an ad-hoc
signature, with no Developer ID signature or notarization. SHA256SUMS verifies
file bytes and does not establish a publisher identity.

## Linux Mint shortcut regression (2026-10-20)

A follow-up run on Ubuntu 24.04 arm64 with a private Xvfb server reproduced
`Clipboard capture cancelled` by reserving Ctrl+Alt+Escape in another X11
client, as Cinnamon does. That arming check passes with the fix. A conflict
on plain Escape now retains its actual diagnostic through clipboard capture.

All 12 clipboard/arming checks and 13 end-to-end X11 scenarios passed,
including consumed Escape with Caps Lock, Num Lock, and both enabled. The
application's C code was generated with pinned V 0.5.2 and compiled with GCC
inside the VM. The V core tests also passed with that compiler on macOS arm64.
This follow-up did not exercise a physical Linux Mint desktop or rebuild the
x86_64 AppImage.

## Re-run

```sh
./scripts/bootstrap-v.sh
./scripts/build.sh
./scripts/integration-linux.sh
./scripts/build.sh --package
```

See `BUILDING.md` for native development/test dependencies and Windows/macOS
commands. All desktop integration tests belong on an isolated test desktop;
they generate synthetic keystrokes and replace that desktop's test clipboard.
