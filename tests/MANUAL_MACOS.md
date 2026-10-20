# macOS arm64 acceptance checks

Run on an Apple Silicon Mac with macOS 12 or newer. These are interactive
acceptance checks; a successful CI compilation alone does not prove that a
particular VNC/IPMI viewer forwards the generated events correctly.

Build with `./scripts/build.sh`, then launch `./dist/keypour` from Terminal so
status and errors are visible. Stop it with Ctrl+C in that Terminal. Use a plain
text editor for the first checks. Test a remote viewer only after local delivery
works, using a disposable text editor inside the remote system.

## Permissions and target selection

1. Launch without pre-granted permissions. The process must exit with an
   actionable Accessibility or Input Monitoring message. Grant the requested
   permission in macOS privacy settings for Keypour or its launching Terminal,
   then relaunch. On macOS 12 the settings app is named System Preferences.
2. Copy `AbZ 019 !@#$%^&*() []{} +=_-;:'",.<>/?\\|`. Press Ctrl+Option+V, release
   all keys, then click inside the editor. Nothing should be typed before the
   click or before the configured start delay. The target click must still
   position the editor's caret. The copied characters must then appear once,
   with the exact case and punctuation. Verify Caps Lock both on and off before
   arming. Hotkey letter names refer to ANSI key positions; text follows the
   active layout.
3. Repeat with another configured shortcut, for example Ctrl+Shift+F8. Ensure
   that the shortcut key itself does not appear in the editor. On laptops,
   macOS may require Fn for function keys. Keep a hotkey modifier held after
   clicking: typing must wait until it is released.

## Clipboard snapshot, pacing, and cancellation

4. Copy a distinctive text, arm, change the clipboard from another application
   without clicking the intended target yet, then select the target. Delivery
   must use the snapshot from the hotkey, not the later clipboard value. For a
   direct snapshot check without extra clicks, change the clipboard from a
   second terminal process with `pbcopy` after arming.
5. Copy several lines containing a tab and the literal text `{ENTER}`. Newlines
   and tabs must be actual Return/Tab keystrokes, while `{ENTER}` remains seven
   literal characters. Compare two `interval_ms` settings in a configuration
   file; slower settings must visibly increase the interval between characters.
6. Start a long paste at a slow interval. Separately exercise Escape, the
   configured shortcut again, a second click, switching to another application
   with Command+Tab, switching to another window of the same application, and
   changing the input source or Caps Lock. Each must stop further typing. No
   remainder may appear in the other application/window. Because one native
   down/up pair is atomic, cancellation can take up to the current key hold
   plus event-pump delay. Check that a manually held Shift/Option key is still
   held after cancellation and is released only when you physically release it.

## Layout and Unicode behavior

7. With `input_mode` set to `keys`, try a character absent from the active
   layout and a character available only through a dead-key composition. The
   entire payload must fail preflight without typing a prefix. Select an input
   method without direct keyboard layout data and confirm an explanatory
   failure. Then use `input_mode: "unicode"` and try `Привет 🙂` in the local
   editor. It should arrive exactly; repeat in the intended VNC/IPMI viewer and
   record whether that viewer supports Unicode text events. Return/Tab must
   remain physical keys in both modes.

The focused target is an Accessibility window identity plus its process ID,
input source, and Caps Lock state. Applications that do not expose their window
through Accessibility are rejected. A browser tab or field inside the same
native window does not provide a separate native window identity; clicking
again cancels, but application-driven caret changes cannot be detected reliably.
Secure Keyboard Entry can prevent the global event tap from receiving keys.
