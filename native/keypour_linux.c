#define _POSIX_C_SOURCE 200809L
#include "keypour.h"
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/XKBlib.h>
#include <X11/keysym.h>
#include <X11/extensions/XTest.h>
#include <X11/extensions/XInput2.h>
#include <xkbcommon/xkbcommon.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Only OS access lives here; session, configuration, text policy and scheduling
   live in V. No XSendEvent or clipboard-paste shortcuts are used. */
struct keyboard_source {
    int id, physical;
    unsigned int held_modifiers; /* only Shift/Level3; no typed text is retained */
};
static struct {
    Display *display;
    Window root, clicked, target;
    int xi_opcode, active, mode, pending, x_error, hot_down, left_down;
    struct keyboard_source *sources;
    int source_count;
    int signals_installed;
    unsigned int hot_mods, num_mask, level3_mask;
    KeyCode hot_key, escape_key, shift_key, level3_key;
    int min_key, max_key, layout_group;
    unsigned int layout_locked;
    unsigned long map_generation, target_map_generation;
    Atom clipboard, utf8, transfer, incr;
    int (*old_error)(Display *, XErrorEvent *);
    void (*old_int)(int);
    void (*old_term)(int);
    char error[256];
} kp;
static volatile sig_atomic_t interrupted;

void kp_wipe(void *data, size_t len) {
    volatile unsigned char *p = (volatile unsigned char *)data;
    while (len--) *p++ = 0;
}
void kp_free_text(char *text, int len) {
    if (text) { if (len > 0) kp_wipe(text, (size_t)len); free(text); }
}
const char *kp_error(void) { return kp.error[0] ? kp.error : "X11 operation failed"; }
static int fail(const char *message) {
    snprintf(kp.error, sizeof(kp.error), "%s", message);
    return 0;
}
static int x_error(Display *display, XErrorEvent *event) {
    (void)display; kp.x_error = event->error_code; return 0;
}
static void stop_signal(int number) { (void)number; interrupted = 1; }
static int64_t monotonic_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
static void pause_ms(int ms) {
    struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L};
    while (nanosleep(&ts, &ts) && errno == EINTR && !interrupted) {}
}
static Window top_window(Window window) {
    if (!window || window == PointerRoot || window == kp.root) return None;
    for (int depth = 0; depth < 64; ++depth) {
        Window root, parent, *children = NULL;
        unsigned int count;
        if (!XQueryTree(kp.display, window, &root, &parent, &children, &count)) return None;
        if (children) XFree(children);
        if (parent == kp.root) return window;
        if (!parent || parent == window) return None;
        window = parent;
    }
    return None;
}
static Window focused_window(void) {
    Window focus; int revert;
    XGetInputFocus(kp.display, &focus, &revert);
    return top_window(focus);
}
static Window pointed_window(void) {
    Window root, child; int rx, ry, wx, wy; unsigned int state;
    if (!XQueryPointer(kp.display, kp.root, &root, &child, &rx, &ry, &wx, &wy, &state)) return None;
    return child;
}
static unsigned int mask_for_key(KeyCode key, XModifierKeymap *map) {
    if (!key) return 0;
    for (int m = 0; m < 8; ++m)
        for (int j = 0; j < map->max_keypermod; ++j)
            if (map->modifiermap[m * map->max_keypermod + j] == key) return 1u << m;
    return 0;
}
static int refresh_sources(void) {
    int count = 0;
    XIDeviceInfo *devices = XIQueryDevice(kp.display, XIAllDevices, &count);
    if (!devices) return fail("Cannot enumerate XInput keyboard devices");
    struct keyboard_source *sources = (struct keyboard_source *)calloc((size_t)(count ? count : 1), sizeof(*sources));
    if (!sources) { XIFreeDeviceInfo(devices); return fail("Cannot allocate keyboard device state"); }
    for (int i = 0; i < count; ++i) {
        sources[i].id = devices[i].deviceid;
        int keyboard = 0;
        for (int c = 0; c < devices[i].num_classes; ++c)
            if (devices[i].classes[c]->type == XIKeyClass) keyboard = 1;
        /* Xorg exposes synthetic input through its XTEST slave device. Raw
           events name the originating slave in sourceid. Unknown sources are
           handled conservatively below until enumeration catches up. */
        sources[i].physical = keyboard && devices[i].use != XIMasterKeyboard
            && !(devices[i].name && strstr(devices[i].name, "XTEST"));
        for (int old = 0; old < kp.source_count; ++old)
            if (kp.sources[old].id == sources[i].id && sources[i].physical)
                sources[i].held_modifiers = kp.sources[old].held_modifiers;
    }
    XIFreeDeviceInfo(devices);
    free(kp.sources);
    kp.sources = sources;
    kp.source_count = count;
    return 1;
}
static void physical_transition(XIRawEvent *raw, int pressed) {
    unsigned int bit = (raw->detail == kp.shift_key ? 1u : 0)
        | (raw->detail == kp.level3_key ? 2u : 0);
    if (!bit) return;
    for (int attempt = 0; attempt < 2; ++attempt) {
        for (int i = 0; i < kp.source_count; ++i) {
            if (kp.sources[i].id != raw->sourceid) continue;
            if (kp.sources[i].physical) {
                if (pressed) kp.sources[i].held_modifiers |= bit;
                else kp.sources[i].held_modifiers &= ~bit;
            }
            return;
        }
        if (!refresh_sources()) { kp.pending |= KP_EVENT_ERROR; return; }
    }
    fail("An input device changed while typing; restart Keypour");
    kp.pending |= KP_EVENT_ERROR;
}
static int physical_modifier_down(KeyCode key) {
    unsigned int bit = (key == kp.shift_key ? 1u : 0) | (key == kp.level3_key ? 2u : 0);
    for (int i = 0; i < kp.source_count; ++i)
        if (kp.sources[i].physical && (kp.sources[i].held_modifiers & bit)) return 1;
    return 0;
}
static void handle_event(XEvent *event) {
    if (event->type == KeyPress && event->xkey.keycode == kp.hot_key) {
        if (!kp.hot_down) kp.pending |= KP_EVENT_HOTKEY;
        kp.hot_down = 1;
    } else if (event->type == KeyRelease && event->xkey.keycode == kp.hot_key) {
        kp.hot_down = 0;
    } else if (event->type == KeyPress && event->xkey.keycode == kp.escape_key && kp.active) {
        kp.pending |= KP_EVENT_CANCEL;
    } else if (event->type == MappingNotify) {
        XRefreshKeyboardMapping(&event->xmapping);
        ++kp.map_generation;
    } else if (event->type == GenericEvent && event->xcookie.extension == kp.xi_opcode &&
               XGetEventData(kp.display, &event->xcookie)) {
        if (event->xcookie.evtype == XI_RawKeyPress || event->xcookie.evtype == XI_RawKeyRelease) {
            physical_transition((XIRawEvent *)event->xcookie.data, event->xcookie.evtype == XI_RawKeyPress);
        } else if (event->xcookie.evtype == XI_HierarchyChanged) {
            if (!refresh_sources()) kp.pending |= KP_EVENT_ERROR;
        } else if (kp.active && event->xcookie.evtype == XI_RawButtonPress) {
            XIRawEvent *raw = (XIRawEvent *)event->xcookie.data;
            if (raw->detail == 1) kp.left_down = 1;
        } else if (kp.active && event->xcookie.evtype == XI_RawButtonRelease) {
            XIRawEvent *raw = (XIRawEvent *)event->xcookie.data;
            if (raw->detail == 1 && kp.left_down) {
                kp.left_down = 0;
                kp.clicked = pointed_window();
                kp.pending |= KP_EVENT_CLICK;
            }
        }
        XFreeEventData(kp.display, &event->xcookie);
    }
}
static void drain_events(void) {
    while (XPending(kp.display)) { XEvent event; XNextEvent(kp.display, &event); handle_event(&event); }
}
int kp_poll(void) {
    if (!kp.display) return KP_EVENT_ERROR;
    drain_events();
    if (interrupted) kp.pending |= KP_EVENT_QUIT;
    int events = kp.pending; kp.pending = 0;
    return events;
}
void kp_set_active(int active) {
    kp.active = active != 0;
    kp.pending &= ~(KP_EVENT_CLICK | KP_EVENT_CANCEL);
    kp.target = None; kp.clicked = None; kp.left_down = 0;
    if (!kp.display) return;
    kp.x_error = 0;
    /* Only reserve plain Escape (including lock variants). AnyModifier also
       claims desktop shortcuts such as Cinnamon's Ctrl+Alt+Escape; one such
       conflict makes the entire AnyModifier grab fail with BadAccess. */
    unsigned int locks[] = {0, LockMask, kp.num_mask, LockMask | kp.num_mask};
    if (kp.active) {
        for (int i = 0; i < 4; ++i)
            XGrabKey(kp.display, kp.escape_key, locks[i], kp.root, False, GrabModeAsync, GrabModeAsync);
        XSync(kp.display, False);
        if (kp.x_error) { fail("Escape is already reserved by another application"); kp.pending |= KP_EVENT_ERROR; }
    } else {
        for (int i = 0; i < 4; ++i)
            XUngrabKey(kp.display, kp.escape_key, locks[i], kp.root);
        XFlush(kp.display);
    }
}
int kp_init(int modifiers, const char *key, int mode) {
    if (kp.display) return fail("Keypour is already initialized");
    memset(&kp, 0, sizeof(kp)); interrupted = 0;
    if (!key || !*key || modifiers <= 0 || (modifiers & ~15) || (mode != 0 && mode != 1))
        return fail("Invalid hotkey or input mode");
    const char *session = getenv("XDG_SESSION_TYPE");
    const char *wayland = getenv("WAYLAND_DISPLAY");
    if ((session && strcmp(session, "wayland") == 0) || (wayland && *wayland))
        return fail("Native Wayland is unsupported. Log into an X11/Xorg desktop session.");
    kp.mode = mode;
    kp.display = XOpenDisplay(NULL);
    if (!kp.display) return fail("Cannot open DISPLAY. Start Keypour inside an X11 desktop session.");
    kp.old_error = XSetErrorHandler(x_error);
    kp.root = DefaultRootWindow(kp.display);
    int event_base, error_base, major, minor;
    if (!XTestQueryExtension(kp.display, &event_base, &error_base, &major, &minor)) {
        fail("The X server does not support the XTEST extension"); goto error;
    }
    if (!XQueryExtension(kp.display, "XInputExtension", &kp.xi_opcode, &event_base, &error_base)) {
        fail("The X server does not support XInput2"); goto error;
    }
    /* XI 2.1 delivers raw releases even during the target's implicit mouse grab. */
    major = 2; minor = 1;
    if ((XIQueryVersion(kp.display, &major, &minor) != Success || major < 2 || (major == 2 && minor < 1))) {
        fail("XInput version 2.1 or later is required"); goto error;
    }
    Bool detectable;
    if (!XkbSetDetectableAutoRepeat(kp.display, True, &detectable) || !detectable) {
        fail("The X server cannot suppress repeating hotkeys"); goto error;
    }
    kp.hot_key = XKeysymToKeycode(kp.display, XStringToKeysym(key));
    kp.escape_key = XKeysymToKeycode(kp.display, XK_Escape);
    kp.shift_key = XKeysymToKeycode(kp.display, XK_Shift_L);
    kp.level3_key = XKeysymToKeycode(kp.display, XK_ISO_Level3_Shift);
    if (!kp.level3_key) kp.level3_key = XKeysymToKeycode(kp.display, XK_Mode_switch);
    if (!kp.hot_key || !kp.escape_key || !kp.shift_key) {
        fail("Hotkey or required control keys are absent from the X11 keymap"); goto error;
    }
    XModifierKeymap *map = XGetModifierMapping(kp.display);
    if (!map) { fail("Cannot read the X11 modifier map"); goto error; }
    kp.num_mask = mask_for_key(XKeysymToKeycode(kp.display, XK_Num_Lock), map);
    kp.level3_mask = mask_for_key(kp.level3_key, map);
    unsigned int alt_mask = mask_for_key(XKeysymToKeycode(kp.display, XK_Alt_L), map);
    unsigned int super_mask = mask_for_key(XKeysymToKeycode(kp.display, XK_Super_L), map);
    if (!alt_mask) alt_mask = mask_for_key(XKeysymToKeycode(kp.display, XK_Alt_R), map);
    if (!super_mask) super_mask = mask_for_key(XKeysymToKeycode(kp.display, XK_Super_R), map);
    unsigned int reserved_masks = ControlMask | ShiftMask | LockMask | kp.num_mask;
    if (((modifiers & KP_MOD_ALT) && (!alt_mask || (alt_mask & reserved_masks))) ||
        ((modifiers & KP_MOD_SUPER) && (!super_mask || (super_mask & reserved_masks))) ||
        ((modifiers & KP_MOD_ALT) && (modifiers & KP_MOD_SUPER) && (alt_mask & super_mask))) {
        XFreeModifiermap(map);
        fail("The requested Alt/Super keys are not available as distinct X11 modifiers"); goto error;
    }
    if (modifiers & KP_MOD_CTRL) kp.hot_mods |= ControlMask;
    if (modifiers & KP_MOD_SHIFT) kp.hot_mods |= ShiftMask;
    if (modifiers & KP_MOD_ALT) kp.hot_mods |= alt_mask;
    if (modifiers & KP_MOD_SUPER) kp.hot_mods |= super_mask;
    XFreeModifiermap(map);
    unsigned int locks[] = {0, LockMask, kp.num_mask, LockMask | kp.num_mask};
    kp.x_error = 0;
    for (int i = 0; i < 4; ++i)
        XGrabKey(kp.display, kp.hot_key, kp.hot_mods | locks[i], kp.root, False, GrabModeAsync, GrabModeAsync);
    XSync(kp.display, False);
    if (kp.x_error) { fail("The hotkey is already registered; choose a different --hotkey"); goto error; }
    unsigned char mask[XIMaskLen(XI_RawButtonRelease)] = {0};
    XISetMask(mask, XI_RawButtonPress); XISetMask(mask, XI_RawButtonRelease);
    XISetMask(mask, XI_RawKeyPress); XISetMask(mask, XI_RawKeyRelease);
    unsigned char hierarchy[XIMaskLen(XI_HierarchyChanged)] = {0};
    XISetMask(hierarchy, XI_HierarchyChanged);
    XIEventMask events[] = {{XIAllMasterDevices, sizeof(mask), mask},
                            {XIAllDevices, sizeof(hierarchy), hierarchy}};
    XISelectEvents(kp.display, kp.root, events, 2);
    if (!refresh_sources()) goto error;
    kp.clipboard = XInternAtom(kp.display, "CLIPBOARD", False);
    kp.utf8 = XInternAtom(kp.display, "UTF8_STRING", False);
    kp.transfer = XInternAtom(kp.display, "KEYPOUR_SELECTION", False);
    kp.incr = XInternAtom(kp.display, "INCR", False);
    XDisplayKeycodes(kp.display, &kp.min_key, &kp.max_key);
    kp.old_int = signal(SIGINT, stop_signal);
    if (kp.old_int == SIG_ERR) { fail("Cannot install the interrupt handler"); goto error; }
    kp.signals_installed |= 1;
    kp.old_term = signal(SIGTERM, stop_signal);
    if (kp.old_term == SIG_ERR) { fail("Cannot install the termination handler"); goto error; }
    kp.signals_installed |= 2;
    XSync(kp.display, False);
    if (kp.x_error) { fail("X11 rejected input event registration"); goto error; }
    return 1;
error:
    kp_shutdown(); return 0;
}
void kp_shutdown(void) {
    if (kp.display) {
        XUngrabKey(kp.display, AnyKey, AnyModifier, kp.root);
        XCloseDisplay(kp.display); kp.display = NULL;
        XSetErrorHandler(kp.old_error);
    }
    if (kp.signals_installed & 1) signal(SIGINT, kp.old_int);
    if (kp.signals_installed & 2) signal(SIGTERM, kp.old_term);
    kp.signals_installed = 0;
    free(kp.sources); kp.sources = NULL; kp.source_count = 0;
}

/* Selection transfers are bounded, including ICCCM INCR used by large copies.
   The hotkey/cancel message pump continues during the clipboard exchange. */
static int selection_event(int wanted, Window requestor, Atom requested,
                           XEvent *found, int64_t deadline) {
    while (monotonic_ms() < deadline && !interrupted) {
        /* Preserve a backend error's diagnostic and stop before accepting a
           ready selection reply when cancellation/error is already pending. */
        if (kp.pending & KP_EVENT_ERROR) return 0;
        if (kp.pending & (KP_EVENT_CANCEL | KP_EVENT_HOTKEY | KP_EVENT_QUIT))
            return fail("Clipboard capture cancelled");
        if (kp.x_error) return fail("The X server rejected the clipboard transfer");
        if (XPending(kp.display)) {
            XEvent event; XNextEvent(kp.display, &event);
            if ((wanted == SelectionNotify && event.type == SelectionNotify &&
                 event.xselection.requestor == requestor && event.xselection.selection == kp.clipboard &&
                 event.xselection.target == requested &&
                 (event.xselection.property == kp.transfer || event.xselection.property == None)) ||
                (wanted == PropertyNotify && event.type == PropertyNotify && event.xproperty.window == requestor &&
                 event.xproperty.atom == kp.transfer && event.xproperty.state == PropertyNewValue)) {
                *found = event; return 1;
            }
            handle_event(&event);
        } else pause_ms(2);
    }
    return fail(interrupted ? "Clipboard capture interrupted" : "Clipboard owner did not respond within 1.5 seconds");
}
static char *read_selection(Atom requested, int max_bytes, int *out_len) {
    /* A fresh requestor isolates replies from a timed-out/cancelled INCR.
       Destroying it also ends the owner's outstanding per-window transfer. */
    kp.x_error = 0;
    Window requestor = XCreateSimpleWindow(kp.display, kp.root, -1, -1, 1, 1, 0, 0, 0);
    if (!requestor) { fail("Cannot create a clipboard request window"); return NULL; }
    XSelectInput(kp.display, requestor, PropertyChangeMask);
    XConvertSelection(kp.display, kp.clipboard, requested, kp.transfer, requestor, CurrentTime);
    XFlush(kp.display);
    int64_t deadline = monotonic_ms() + 1500;
    XEvent event;
    char *result = NULL;
    size_t total = 0;
    int incremental = 0;
    if (!selection_event(SelectionNotify, requestor, requested, &event, deadline) || event.xselection.property == None)
        goto failed;
    result = (char *)malloc((size_t)max_bytes + 1);
    if (!result) { fail("Not enough memory for clipboard text"); goto failed; }
    for (;;) {
        Atom type; int format; unsigned long count, after; unsigned char *data = NULL;
        if (XGetWindowProperty(kp.display, requestor, kp.transfer, 0, (max_bytes - (long)total + 3) / 4,
                               True, AnyPropertyType, &type, &format, &count, &after, &data) != Success) {
            fail("Cannot read clipboard selection"); goto failed;
        }
        if (type == kp.incr && !incremental) {
            if (format != 32 || count != 1 || after || !data) {
                if (data) XFree(data);
                fail("Clipboard owner returned a malformed INCR header"); goto failed;
            }
            unsigned long advertised = ((unsigned long *)data)[0];
            XFree(data);
            if (advertised > (unsigned long)max_bytes) {
                fail("Clipboard exceeds the configured size limit"); goto failed;
            }
            incremental = 1;
            /* GetProperty(delete=True) acknowledged the INCR header. */
            XFlush(kp.display);
        } else {
            /* UTF8_STRING and STRING have different encodings. Never accept
               one while claiming the other's encoding to the caller. */
            if (format != 8 || type != requested) {
                if (data) XFree(data);
                fail("Clipboard owner returned an unsupported text format"); goto failed;
            }
            if (after || count > (unsigned long)max_bytes - total) {
                if (data) { kp_wipe(data, count); XFree(data); }
                fail("Clipboard exceeds the configured size limit"); goto failed;
            }
            if (count && data) memcpy(result + total, data, count);
            total += count;
            if (data) { kp_wipe(data, count); XFree(data); }
            if (!incremental || count == 0) {
                result[total] = 0; *out_len = (int)total;
                XDestroyWindow(kp.display, requestor); XFlush(kp.display);
                return result;
            }
            XFlush(kp.display);
        }
        if (!selection_event(PropertyNotify, requestor, requested, &event, deadline)) goto failed;
    }
failed:
    kp_free_text(result, (int)total);
    XDestroyWindow(kp.display, requestor); XFlush(kp.display);
    return NULL;
}
char *kp_clipboard(int max_bytes, int *out_len) {
    if (out_len) *out_len = 0;
    if (!out_len || !kp.display || max_bytes <= 0) { fail("Invalid clipboard request"); return NULL; }
    /* kp_set_active may have failed before this call; do not erase why. */
    if (kp.pending & KP_EVENT_ERROR) return NULL;
    if (!XGetSelectionOwner(kp.display, kp.clipboard)) { fail("Clipboard contains no text owner"); return NULL; }
    kp.error[0] = 0;
    char *text = read_selection(kp.utf8, max_bytes, out_len);
    if (text || kp.error[0]) return text;
    text = read_selection(XA_STRING, max_bytes, out_len);
    if (!text) { if (!kp.error[0]) fail("Clipboard does not contain plain text"); return NULL; }
    // XA_STRING is ISO-8859-1, never assume UTF-8 or replace non-ASCII bytes.
    size_t expanded = (size_t)*out_len;
    for (int i = 0; i < *out_len; ++i) if ((unsigned char)text[i] >= 128) ++expanded;
    if (expanded > (size_t)max_bytes) { kp_free_text(text, *out_len); fail("Clipboard exceeds size limit"); return NULL; }
    char *utf8 = (char *)malloc(expanded + 1);
    if (!utf8) { kp_free_text(text, *out_len); fail("Not enough memory for clipboard"); return NULL; }
    size_t at = 0;
    for (int i = 0; i < *out_len; ++i) {
        unsigned char ch = (unsigned char)text[i];
        if (ch < 128) utf8[at++] = (char)ch;
        else { utf8[at++] = (char)(0xc0 | (ch >> 6)); utf8[at++] = (char)(0x80 | (ch & 63)); }
    }
    kp_free_text(text, *out_len); utf8[at] = 0; *out_len = (int)at; return utf8;
}
int kp_capture_target(void) {
    kp.x_error = 0;
    Window focus = focused_window();
    if (!focus || !kp.clicked || focus != kp.clicked)
        return fail("Click a focused input window; the selected window did not receive focus");
    XkbStateRec state;
    if (XkbGetState(kp.display, XkbUseCoreKbd, &state) != Success) return fail("Cannot read active keyboard layout");
    kp.target = focus; kp.layout_group = state.group; kp.layout_locked = state.locked_mods;
    kp.target_map_generation = kp.map_generation;
    return !kp.x_error;
}
int kp_target_matches(void) {
    if (!kp.target || focused_window() != kp.target) return 0;
    XkbStateRec state;
    if (XkbGetState(kp.display, XkbUseCoreKbd, &state) != Success) return 0;
    return state.group == kp.layout_group && state.locked_mods == kp.layout_locked &&
           kp.map_generation == kp.target_map_generation;
}
int kp_modifiers_down(void) {
    if (!kp.display) return 1;
    XkbStateRec state;
    if (XkbGetState(kp.display, XkbUseCoreKbd, &state) != Success) return 1;
    Window root, child; int rx, ry, wx, wy; unsigned int buttons;
    if (!XQueryPointer(kp.display, kp.root, &root, &child, &rx, &ry, &wx, &wy, &buttons)) return 1;
    return kp.hot_down || state.base_mods != 0 || state.latched_mods != 0 ||
           (state.locked_mods & ~(LockMask | kp.num_mask)) != 0 ||
           (buttons & (Button1Mask | Button2Mask | Button3Mask)) != 0;
}
static int lookup_key(uint32_t scalar, KeyCode *code, unsigned int *modifiers) {
    if (scalar == '\n' || scalar == '\t') {
        *code = XKeysymToKeycode(kp.display, scalar == '\n' ? XK_Return : XK_Tab);
        *modifiers = 0; return *code != 0;
    }
    unsigned int choices[] = {0, ShiftMask, kp.level3_mask, kp.level3_mask | ShiftMask};
    for (int m = 0; m < 4; ++m) {
        if (m >= 2 && (!kp.level3_key || !kp.level3_mask ||
            (kp.level3_mask & (ControlMask | Mod1Mask | Mod4Mask)))) continue;
        for (int k = kp.min_key; k <= kp.max_key; ++k) {
            KeySym symbol; unsigned int consumed;
            unsigned int state = XkbBuildCoreState(choices[m] | kp.layout_locked, kp.layout_group);
            if (XkbLookupKeySym(kp.display, (KeyCode)k, state, &consumed, &symbol) &&
                xkb_keysym_to_utf32((xkb_keysym_t)symbol) == scalar) {
                *code = (KeyCode)k; *modifiers = choices[m]; return 1;
            }
        }
    }
    return 0;
}
static KeyCode spare_key(void) {
    char down[32]; XQueryKeymap(kp.display, down);
    for (int key = kp.max_key; key >= kp.min_key; --key) {
        if ((down[key / 8] & (1 << (key % 8))) || key == kp.hot_key || key == kp.escape_key) continue;
        int count; KeySym *symbols = XGetKeyboardMapping(kp.display, (KeyCode)key, 1, &count);
        int empty = symbols != NULL;
        for (int n = 0; symbols && n < count; ++n) if (symbols[n] != NoSymbol) empty = 0;
        if (symbols) XFree(symbols);
        if (empty) return (KeyCode)key;
    }
    return 0;
}
int kp_can_type(uint32_t scalar) {
    if (scalar > 0x10ffff || (scalar >= 0xd800 && scalar <= 0xdfff) ||
        (scalar < 32 && scalar != '\n' && scalar != '\t') || (scalar >= 0x7f && scalar <= 0x9f))
        return fail("Unsupported character in clipboard");
    KeyCode code; unsigned int modifiers;
    if (lookup_key(scalar, &code, &modifiers)) return 1;
    if (kp.mode == 1 && xkb_utf32_to_keysym(scalar) && spare_key()) return 1;
    return fail("A character cannot be typed with the current keyboard layout. Change layout or try --mode unicode.");
}
static int key_event(KeyCode code, int down) {
    return XTestFakeKeyEvent(kp.display, code, down, CurrentTime) != 0;
}
int kp_type(uint32_t scalar, int hold_ms) {
    if (hold_ms < 1 || hold_ms > 100 || !kp_target_matches() || kp_modifiers_down())
        return fail("Focus/layout changed or physical modifiers are held");
    KeyCode code; unsigned int modifiers;
    KeySym *original = NULL; int original_count = 0;
    kp.x_error = 0;
    if (!lookup_key(scalar, &code, &modifiers)) {
        if (kp.mode != 1 || !(code = spare_key())) return fail("Character is absent from the active keyboard layout");
        original = XGetKeyboardMapping(kp.display, code, 1, &original_count);
        if (!original || original_count <= 0) { if (original) XFree(original); return fail("Cannot preserve temporary keymap"); }
        KeySym *replacement = (KeySym *)calloc((size_t)original_count, sizeof(KeySym));
        if (!replacement) { XFree(original); return fail("Not enough memory for temporary keymap"); }
        // Populate every level so Caps/Num Lock cannot change this symbol.
        for (int i = 0; i < original_count; ++i) replacement[i] = (KeySym)xkb_utf32_to_keysym(scalar);
        XChangeKeyboardMapping(kp.display, code, original_count, replacement, 1);
        free(replacement); XSync(kp.display, False); modifiers = 0;
        if (kp.x_error) {
            XChangeKeyboardMapping(kp.display, code, original_count, original, 1);
            XFree(original); XSync(kp.display, False);
            return fail("X11 rejected a temporary Unicode keymap; no keys were emitted");
        }
    }
    int ok = 1;
    if (modifiers & ShiftMask) ok &= key_event(kp.shift_key, True);
    if (kp.level3_mask && (modifiers & kp.level3_mask)) ok &= key_event(kp.level3_key, True);
    ok &= key_event(code, True); XFlush(kp.display);
    pause_ms(hold_ms);
    /* Raw physical modifier transitions arrived while the key was held.
       Preserve pending cancel/click/quit flags for the V state machine. */
    drain_events();
    ok &= key_event(code, False);
    if (kp.level3_mask && (modifiers & kp.level3_mask) && !physical_modifier_down(kp.level3_key))
        ok &= key_event(kp.level3_key, False);
    if ((modifiers & ShiftMask) && !physical_modifier_down(kp.shift_key))
        ok &= key_event(kp.shift_key, False);
    XSync(kp.display, False);
    if (kp.x_error) ok = 0;
    if (original) {
        // Let clients process the mapped key before restoring its old mapping.
        pause_ms(10);
        XChangeKeyboardMapping(kp.display, code, original_count, original, 1);
        XFree(original); XSync(kp.display, False);
        drain_events();
        kp.target_map_generation = kp.map_generation;
    }
    if (kp.x_error) ok = 0;
    return ok ? 1 : fail("XTEST rejected a key event");
}
