#ifndef KEYPOUR_H
#define KEYPOUR_H
#include <stdint.h>
#include <stddef.h>
/* Single-threaded API. All calls run on the main thread. No callback enters V. */
#define KP_MOD_CTRL 1
#define KP_MOD_ALT 2
#define KP_MOD_SHIFT 4
#define KP_MOD_SUPER 8
#define KP_EVENT_HOTKEY 1
#define KP_EVENT_CLICK 2
#define KP_EVENT_CANCEL 4
#define KP_EVENT_QUIT 8
#define KP_EVENT_ERROR 16
/* mode: 0 = layout-mapped physical keys; 1 = explicit Unicode text injection. */
int kp_init(int modifiers, const char *key, int mode);
void kp_shutdown(void);
/* Pump without blocking; return an OR of events. Click is left-button RELEASE.
   Hotkey is consumed; target click passes through. Ignore self-injected events. */
int kp_poll(void);
/* Enable click/cancel observation while armed/typing. Clear stale click events.
   Hotkey is always observed. Escape is consumed only while active. */
void kp_set_active(int active);
/* malloc-owned, NUL-terminated UTF-8. out_len counts bytes, not the NUL.
   Fail rather than truncate if larger than max_bytes. Never log text. */
char *kp_clipboard(int max_bytes, int *out_len);
void kp_free_text(char *text, int len);
/* Capture the focused top-level window after the selecting click; fail if no
   eligible focus. Check the same target before EVERY emitted character. */
int kp_capture_target(void);
int kp_target_matches(void);
/* True if physical modifiers/buttons are still down. Caps/Num Lock excluded. */
int kp_modifiers_down(void);
/* Preflight current layout; no key emission. LF and Tab are real keys. */
int kp_can_type(uint32_t scalar);
/* Synchronously send one balanced down/up pair and required modifiers.
   Keep a key down for hold_ms (1..100). Fail on unsupported/injection errors.
   Never release modifiers that the user, rather than this app, pressed. */
int kp_type(uint32_t scalar, int hold_ms);
/* Stable diagnostic, no clipboard contents; valid until the next API call. */
const char *kp_error(void);
/* Best-effort wipe for V-owned payload buffers; prevents compiler elision. */
void kp_wipe(void *data, size_t len);
#endif
