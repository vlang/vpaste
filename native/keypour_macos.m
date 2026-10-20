/* Keypour macOS adapter. Original implementation, MIT licensed.
 * Build as Objective-C; link AppKit, ApplicationServices and Carbon.
 * The policy/state machine lives in V. This file only calls native APIs. */
#import <AppKit/AppKit.h>
#import <ApplicationServices/ApplicationServices.h>
#import <Carbon/Carbon.h>
#include "keypour.h"
#include <errno.h>
#include <mach/mach_time.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define KP_EVENT_MARKER INT64_C(0x4b4559504f5552)
#define KP_MAP_CAPACITY (128 * 4)

struct kp_mapping {
    uint32_t scalar;
    CGKeyCode key;
    CGEventFlags modifiers;
};

static CFMachPortRef kp_tap;
static CFRunLoopSourceRef kp_run_source;
static CGEventSourceRef kp_event_source;
static AXUIElementRef kp_target_window;
static AXUIElementRef kp_clicked_window;
static CFStringRef kp_target_input_id;
static int kp_target_caps;
static pid_t kp_target_pid;
static pid_t kp_clicked_pid;
static CGPoint kp_click_position;
static CFDataRef kp_cached_layout;
static struct kp_mapping kp_map[KP_MAP_CAPACITY];
static size_t kp_map_count;
static int kp_cached_caps = -1;
static CGKeyCode kp_hotkey_code;
static int kp_hotkey_modifiers;
static int kp_mode;
static int kp_active;
static int kp_pending;
static int kp_hotkey_down;
static int kp_escape_down;
static int kp_left_down;
static int kp_click_needs_capture;
static int kp_initialized;
static volatile sig_atomic_t kp_quit_requested;
static struct sigaction kp_old_int;
static struct sigaction kp_old_term;
static int kp_signals_installed;
static char kp_diagnostic[384];
static mach_timebase_info_data_t kp_timebase;

static void kp_fail(const char *message) {
    snprintf(kp_diagnostic, sizeof(kp_diagnostic), "%s", message);
}

const char *kp_error(void) { return kp_diagnostic; }

void kp_wipe(void *data, size_t len) {
    volatile unsigned char *p = (volatile unsigned char *)data;
    while (len--) *p++ = 0;
}

void kp_free_text(char *text, int len) {
    if (text) {
        if (len > 0) kp_wipe(text, (size_t)len);
        free(text);
    }
}

static void kp_signal_handler(int signal_number) {
    (void)signal_number;
    kp_quit_requested = 1;
}

static int kp_normalize_flags(CGEventFlags flags) {
    return ((flags & kCGEventFlagMaskControl) ? KP_MOD_CTRL : 0)
        | ((flags & kCGEventFlagMaskAlternate) ? KP_MOD_ALT : 0)
        | ((flags & kCGEventFlagMaskShift) ? KP_MOD_SHIFT : 0)
        | ((flags & kCGEventFlagMaskCommand) ? KP_MOD_SUPER : 0);
}

static CGEventFlags kp_hardware_flags(void) {
    return CGEventSourceFlagsState(kCGEventSourceStateHIDSystemState)
        & (kCGEventFlagMaskShift | kCGEventFlagMaskControl
           | kCGEventFlagMaskAlternate | kCGEventFlagMaskCommand
           | kCGEventFlagMaskAlphaShift | kCGEventFlagMaskSecondaryFn);
}

static int kp_hotkey_key(const char *key, CGKeyCode *code) {
    /* Hotkey names are ANSI key positions, so Ctrl+Alt+V keeps working while
       a non-Latin layout is selected. Text mapping below uses the live layout. */
    static const CGKeyCode letters[26] = {
        kVK_ANSI_A, kVK_ANSI_B, kVK_ANSI_C, kVK_ANSI_D, kVK_ANSI_E,
        kVK_ANSI_F, kVK_ANSI_G, kVK_ANSI_H, kVK_ANSI_I, kVK_ANSI_J,
        kVK_ANSI_K, kVK_ANSI_L, kVK_ANSI_M, kVK_ANSI_N, kVK_ANSI_O,
        kVK_ANSI_P, kVK_ANSI_Q, kVK_ANSI_R, kVK_ANSI_S, kVK_ANSI_T,
        kVK_ANSI_U, kVK_ANSI_V, kVK_ANSI_W, kVK_ANSI_X, kVK_ANSI_Y,
        kVK_ANSI_Z
    };
    static const CGKeyCode digits[10] = {
        kVK_ANSI_0, kVK_ANSI_1, kVK_ANSI_2, kVK_ANSI_3, kVK_ANSI_4,
        kVK_ANSI_5, kVK_ANSI_6, kVK_ANSI_7, kVK_ANSI_8, kVK_ANSI_9
    };
    static const CGKeyCode functions[12] = {
        kVK_F1, kVK_F2, kVK_F3, kVK_F4, kVK_F5, kVK_F6,
        kVK_F7, kVK_F8, kVK_F9, kVK_F10, kVK_F11, kVK_F12
    };
    if (!key || !*key) return 0;
    if (!key[1] && key[0] >= 'A' && key[0] <= 'Z') {
        *code = letters[key[0] - 'A'];
        return 1;
    }
    if (!key[1] && key[0] >= '0' && key[0] <= '9') {
        *code = digits[key[0] - '0'];
        return 1;
    }
    if (key[0] == 'F') {
        char *end = NULL;
        long number = strtol(key + 1, &end, 10);
        if (key[1] && end && !*end && number >= 1 && number <= 12) {
            *code = functions[number - 1];
            return 1;
        }
    }
    return 0;
}

static CGEventRef kp_tap_callback(CGEventTapProxy proxy, CGEventType type,
                                   CGEventRef event, void *context) {
    (void)proxy;
    (void)context;
    /* Keep this callback small: no clipboard, AX, logging or allocation. */
    if (type == kCGEventTapDisabledByTimeout) {
        if (kp_tap) CGEventTapEnable(kp_tap, true);
        if (kp_active) kp_pending |= KP_EVENT_CANCEL;
        return event;
    }
    if (type == kCGEventTapDisabledByUserInput) {
        kp_fail("macOS disabled the input event tap; check Accessibility and Input Monitoring, then restart Keypour.");
        kp_pending |= KP_EVENT_ERROR;
        return event;
    }
    if (!event || CGEventGetIntegerValueField(event, kCGEventSourceUserData) == KP_EVENT_MARKER)
        return event;
    if (type == kCGEventKeyDown || type == kCGEventKeyUp) {
        CGKeyCode code = (CGKeyCode)CGEventGetIntegerValueField(event, kCGKeyboardEventKeycode);
        if (code == kp_hotkey_code) {
            if (type == kCGEventKeyUp && kp_hotkey_down) {
                kp_hotkey_down = 0;
                return NULL;
            }
            if (type == kCGEventKeyDown && (kp_hotkey_down
                || kp_normalize_flags(CGEventGetFlags(event)) == kp_hotkey_modifiers)) {
                if (!kp_hotkey_down) kp_pending |= KP_EVENT_HOTKEY;
                kp_hotkey_down = 1;
                return NULL;
            }
        }
        if (code == kVK_Escape) {
            if (type == kCGEventKeyUp && kp_escape_down) {
                kp_escape_down = 0;
                return NULL;
            }
            if (type == kCGEventKeyDown && (kp_active || kp_escape_down)) {
                if (!kp_escape_down) kp_pending |= KP_EVENT_CANCEL;
                kp_escape_down = 1;
                return NULL;
            }
        }
    }
    if (kp_active && type == kCGEventLeftMouseDown) kp_left_down = 1;
    if (kp_active && type == kCGEventLeftMouseUp && kp_left_down) {
        kp_left_down = 0;
        kp_click_position = CGEventGetLocation(event);
        kp_click_needs_capture = 1;
        kp_pending |= KP_EVENT_CLICK;
    }
    return event;
}

static void kp_release_target(void) {
    if (kp_target_window) CFRelease(kp_target_window);
    if (kp_target_input_id) CFRelease(kp_target_input_id);
    kp_target_window = NULL;
    kp_target_input_id = NULL;
    kp_target_pid = 0;
}

static CFStringRef kp_input_id(void) {
    TISInputSourceRef source = TISCopyCurrentKeyboardInputSource();
    if (!source) return NULL;
    CFTypeRef identifier = TISGetInputSourceProperty(source, kTISPropertyInputSourceID);
    CFStringRef result = (identifier && CFGetTypeID(identifier) == CFStringGetTypeID())
        ? (CFStringRef)CFRetain(identifier) : NULL;
    CFRelease(source);
    return result;
}

static void kp_release_clicked(void) {
    if (kp_clicked_window) CFRelease(kp_clicked_window);
    kp_clicked_window = NULL;
    kp_clicked_pid = 0;
}

/* The returned AX reference is owned by the caller. */
static AXUIElementRef kp_focused_window(pid_t *pid) {
    @autoreleasepool {
        NSRunningApplication *app = [[NSWorkspace sharedWorkspace] frontmostApplication];
        if (!app || [app processIdentifier] <= 0) return NULL;
        *pid = [app processIdentifier];
        AXUIElementRef application = AXUIElementCreateApplication(*pid);
        if (!application) return NULL;
        AXUIElementSetMessagingTimeout(application, 0.15f);
        CFTypeRef window = NULL;
        AXError result = AXUIElementCopyAttributeValue(application, kAXFocusedWindowAttribute, &window);
        CFRelease(application);
        if (result != kAXErrorSuccess || !window) {
            if (window) CFRelease(window);
            return NULL;
        }
        if (CFGetTypeID(window) != AXUIElementGetTypeID()) {
            CFRelease(window);
            return NULL;
        }
        AXUIElementSetMessagingTimeout((AXUIElementRef)window, 0.15f);
        return (AXUIElementRef)window;
    }
}

static AXUIElementRef kp_containing_window(AXUIElementRef element) {
    CFTypeRef window = NULL;
    if (AXUIElementCopyAttributeValue(element, kAXWindowAttribute, &window) == kAXErrorSuccess
        && window && CFGetTypeID(window) == AXUIElementGetTypeID())
        return (AXUIElementRef)window;
    if (window) CFRelease(window);
    /* A point in a title bar can return the window itself. */
    CFTypeRef role = NULL;
    if (AXUIElementCopyAttributeValue(element, kAXRoleAttribute, &role) == kAXErrorSuccess
        && role) {
        int is_window = CFEqual(role, kAXWindowRole);
        CFRelease(role);
        if (is_window) return (AXUIElementRef)CFRetain(element);
    }
    return NULL;
}

static void kp_snapshot_clicked_window(void) {
    kp_release_clicked();
    pid_t front_pid = 0;
    AXUIElementRef focused = kp_focused_window(&front_pid);
    if (!focused) return;
    AXUIElementRef system = AXUIElementCreateSystemWide();
    AXUIElementRef element = NULL;
    if (!system) { CFRelease(focused); return; }
    AXUIElementSetMessagingTimeout(system, 0.15f);
    AXError result = AXUIElementCopyElementAtPosition(system,
        (float)kp_click_position.x, (float)kp_click_position.y, &element);
    CFRelease(system);
    if (result != kAXErrorSuccess || !element) { CFRelease(focused); return; }
    AXUIElementSetMessagingTimeout(element, 0.15f);
    pid_t hit_pid = 0;
    AXUIElementGetPid(element, &hit_pid);
    AXUIElementRef hit_window = kp_containing_window(element);
    CFRelease(element);
    if (hit_window && hit_pid == front_pid && CFEqual(hit_window, focused)) {
        kp_clicked_window = focused;
        kp_clicked_pid = front_pid;
    } else {
        CFRelease(focused);
    }
    if (hit_window) CFRelease(hit_window);
}

int kp_init(int modifiers, const char *key, int mode) {
    if (kp_initialized) { kp_fail("Keypour is already initialized."); return 0; }
    kp_diagnostic[0] = 0;
    if ((mode != 0 && mode != 1) || modifiers <= 0 || (modifiers & ~15)
        || !kp_hotkey_key(key, &kp_hotkey_code)) {
        kp_fail("Invalid hotkey or input mode.");
        return 0;
    }
    @autoreleasepool {
        const void *keys[] = {kAXTrustedCheckOptionPrompt};
        const void *values[] = {kCFBooleanTrue};
        CFDictionaryRef options = CFDictionaryCreate(NULL, keys, values, 1,
            &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        Boolean trusted = AXIsProcessTrustedWithOptions(options);
        if (options) CFRelease(options);
        if (!trusted) {
            kp_fail("Enable Accessibility for Keypour (or its launching Terminal) in macOS privacy settings, then restart Keypour.");
            return 0;
        }
        if (!CGPreflightListenEventAccess()) {
            CGRequestListenEventAccess();
            kp_fail("Enable Input Monitoring for Keypour (or its launching Terminal) in macOS privacy settings, then restart Keypour.");
            return 0;
        }
        if (!CGPreflightPostEventAccess()) {
            CGRequestPostEventAccess();
            kp_fail("macOS has not allowed event posting. Grant Accessibility permission and restart Keypour.");
            return 0;
        }
        kp_hotkey_modifiers = modifiers;
        kp_mode = mode;
        mach_timebase_info(&kp_timebase);
        kp_quit_requested = 0;
        kp_pending = kp_active = kp_hotkey_down = kp_escape_down = kp_left_down = 0;
        kp_click_needs_capture = 0;
        kp_event_source = CGEventSourceCreate(kCGEventSourceStatePrivate);
        if (!kp_event_source) { kp_fail("Could not create a Quartz event source."); return 0; }
        CGEventSourceSetUserData(kp_event_source, KP_EVENT_MARKER);
        /* Physical Escape/clicks must remain available during a long paste. */
        CGEventSourceSetLocalEventsSuppressionInterval(kp_event_source, 0.0);
        CGEventSourceSetLocalEventsFilterDuringSuppressionState(kp_event_source,
            kCGEventFilterMaskPermitAllEvents, kCGEventSuppressionStateSuppressionInterval);
        CGEventMask mask = CGEventMaskBit(kCGEventKeyDown) | CGEventMaskBit(kCGEventKeyUp)
            | CGEventMaskBit(kCGEventLeftMouseDown) | CGEventMaskBit(kCGEventLeftMouseUp);
        kp_tap = CGEventTapCreate(kCGSessionEventTap, kCGHeadInsertEventTap,
            kCGEventTapOptionDefault, mask, kp_tap_callback, NULL);
        if (!kp_tap) {
            kp_fail("Could not install the global hotkey event tap. Check Accessibility/Input Monitoring; Secure Keyboard Entry can also block input monitoring.");
            kp_shutdown();
            return 0;
        }
        kp_run_source = CFMachPortCreateRunLoopSource(NULL, kp_tap, 0);
        if (!kp_run_source) {
            kp_fail("Could not create the input run-loop source.");
            kp_shutdown();
            return 0;
        }
        CFRunLoopAddSource(CFRunLoopGetMain(), kp_run_source, kCFRunLoopCommonModes);
        CGEventTapEnable(kp_tap, true);
        struct sigaction action;
        memset(&action, 0, sizeof(action));
        action.sa_handler = kp_signal_handler;
        sigemptyset(&action.sa_mask);
        if (sigaction(SIGINT, &action, &kp_old_int) == 0) {
            if (sigaction(SIGTERM, &action, &kp_old_term) == 0) kp_signals_installed = 1;
            else sigaction(SIGINT, &kp_old_int, NULL);
        }
        kp_initialized = 1;
        return 1;
    }
}

void kp_shutdown(void) {
    if (kp_signals_installed) {
        sigaction(SIGINT, &kp_old_int, NULL);
        sigaction(SIGTERM, &kp_old_term, NULL);
        kp_signals_installed = 0;
    }
    if (kp_run_source) {
        CFRunLoopRemoveSource(CFRunLoopGetMain(), kp_run_source, kCFRunLoopCommonModes);
        CFRelease(kp_run_source);
        kp_run_source = NULL;
    }
    if (kp_tap) {
        CGEventTapEnable(kp_tap, false);
        CFMachPortInvalidate(kp_tap);
        CFRelease(kp_tap);
        kp_tap = NULL;
    }
    if (kp_event_source) { CFRelease(kp_event_source); kp_event_source = NULL; }
    if (kp_cached_layout) { CFRelease(kp_cached_layout); kp_cached_layout = NULL; }
    kp_release_target();
    kp_release_clicked();
    kp_map_count = 0;
    kp_cached_caps = -1;
    kp_initialized = kp_active = kp_pending = kp_click_needs_capture = 0;
}

int kp_poll(void) {
    if (kp_quit_requested) return KP_EVENT_QUIT;
    @autoreleasepool {
        /* Bound the drain so a stream of events cannot starve cancellation. */
        for (int i = 0; i < 64; ++i) {
            if (CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0, true) != kCFRunLoopRunHandledSource)
                break;
        }
        if (kp_click_needs_capture) {
            kp_click_needs_capture = 0;
            kp_snapshot_clicked_window();
        }
    }
    int events = kp_pending;
    kp_pending = 0;
    if (kp_quit_requested) events |= KP_EVENT_QUIT;
    return events;
}

void kp_set_active(int active) {
    kp_active = !!active;
    kp_pending &= ~(KP_EVENT_CLICK | KP_EVENT_CANCEL);
    kp_left_down = kp_click_needs_capture = 0;
    kp_release_clicked();
    kp_release_target();
}

char *kp_clipboard(int max_bytes, int *out_len) {
    if (out_len) *out_len = 0;
    if (!out_len || max_bytes <= 0) { kp_fail("Invalid clipboard size limit."); return NULL; }
    @autoreleasepool {
        NSString *text = [[NSPasteboard generalPasteboard] stringForType:NSPasteboardTypeString];
        if (!text) { kp_fail("The clipboard does not contain plain text."); return NULL; }
        if ([text length] > (NSUInteger)max_bytes) {
            kp_fail("Clipboard text exceeds the configured size limit.");
            return NULL;
        }
        NSUInteger bytes = [text lengthOfBytesUsingEncoding:NSUTF8StringEncoding];
        if (bytes > (NSUInteger)max_bytes) {
            kp_fail("Clipboard text exceeds the configured size limit.");
            return NULL;
        }
        char *result = (char *)malloc(bytes + 1);
        if (!result) { kp_fail("Could not allocate clipboard memory."); return NULL; }
        NSUInteger used = 0;
        if (bytes && ![text getBytes:result maxLength:bytes usedLength:&used
                encoding:NSUTF8StringEncoding options:0
                range:NSMakeRange(0, [text length]) remainingRange:NULL]) {
            kp_free_text(result, (int)bytes);
            kp_fail("Clipboard text could not be encoded as UTF-8.");
            return NULL;
        }
        if (used != bytes) {
            kp_free_text(result, (int)bytes);
            kp_fail("Clipboard encoding returned incomplete text.");
            return NULL;
        }
        result[bytes] = 0;
        *out_len = (int)bytes;
        return result;
    }
}

int kp_capture_target(void) {
    kp_release_target();
    if (!kp_clicked_window || kp_clicked_pid <= 0) {
        kp_fail("The clicked window could not be identified through Accessibility; click inside a normal application window.");
        return 0;
    }
    kp_target_window = (AXUIElementRef)CFRetain(kp_clicked_window);
    kp_target_pid = kp_clicked_pid;
    kp_target_input_id = kp_input_id();
    kp_target_caps = !!(kp_hardware_flags() & kCGEventFlagMaskAlphaShift);
    if (!kp_target_input_id) {
        kp_release_target();
        kp_fail("The active keyboard input source could not be identified.");
        return 0;
    }
    if (!kp_target_matches()) {
        kp_release_target();
        kp_fail("Focus changed after the target click.");
        return 0;
    }
    return 1;
}

int kp_target_matches(void) {
    if (!kp_target_window || !kp_target_pid || !kp_target_input_id) return 0;
    if (kp_target_caps != !!(kp_hardware_flags() & kCGEventFlagMaskAlphaShift)) return 0;
    CFStringRef input = kp_input_id();
    if (!input) return 0;
    int same_input = CFEqual(input, kp_target_input_id);
    CFRelease(input);
    if (!same_input) return 0;
    pid_t pid = 0;
    AXUIElementRef current = kp_focused_window(&pid);
    if (!current) return 0;
    int matches = pid == kp_target_pid && CFEqual(current, kp_target_window);
    CFRelease(current);
    return matches;
}

int kp_modifiers_down(void) {
    if (kp_hotkey_down || kp_escape_down) return 1;
    CGEventFlags flags = kp_hardware_flags();
    if (kp_normalize_flags(flags) || (flags & kCGEventFlagMaskSecondaryFn)) return 1;
    return CGEventSourceButtonState(kCGEventSourceStateHIDSystemState, kCGMouseButtonLeft)
        || CGEventSourceButtonState(kCGEventSourceStateHIDSystemState, kCGMouseButtonRight)
        || CGEventSourceButtonState(kCGEventSourceStateHIDSystemState, kCGMouseButtonCenter);
}

static int kp_valid_scalar(uint32_t scalar) {
    return scalar <= 0x10ffff && !(scalar >= 0xd800 && scalar <= 0xdfff)
        && (scalar == '\n' || scalar == '\t' || (scalar >= 0x20 && !(scalar >= 0x7f && scalar <= 0x9f)));
}

static uint32_t kp_decode_utf16(const UniChar *chars, UniCharCount count) {
    if (count == 1 && !(chars[0] >= 0xd800 && chars[0] <= 0xdfff)) return chars[0];
    if (count == 2 && chars[0] >= 0xd800 && chars[0] <= 0xdbff
        && chars[1] >= 0xdc00 && chars[1] <= 0xdfff)
        return 0x10000u + (((uint32_t)chars[0] - 0xd800u) << 10) + ((uint32_t)chars[1] - 0xdc00u);
    return 0;
}

static int kp_is_text_key(CGKeyCode key) {
    /* Do not map function/navigation keys to Cocoa's private-use character
       values. Only keys which can physically produce text are candidates. */
    if (key <= kVK_ANSI_Grave && key != kVK_Return && key != kVK_Tab) return 1;
    switch (key) {
        case kVK_ANSI_KeypadDecimal: case kVK_ANSI_KeypadMultiply:
        case kVK_ANSI_KeypadPlus: case kVK_ANSI_KeypadDivide:
        case kVK_ANSI_KeypadMinus: case kVK_ANSI_KeypadEquals:
        case kVK_ANSI_Keypad0: case kVK_ANSI_Keypad1: case kVK_ANSI_Keypad2:
        case kVK_ANSI_Keypad3: case kVK_ANSI_Keypad4: case kVK_ANSI_Keypad5:
        case kVK_ANSI_Keypad6: case kVK_ANSI_Keypad7: case kVK_ANSI_Keypad8:
        case kVK_ANSI_Keypad9: case kVK_JIS_Yen: case kVK_JIS_Underscore:
        case kVK_JIS_KeypadComma:
            return 1;
        default:
            return 0;
    }
}

static int kp_refresh_mapping(void) {
    TISInputSourceRef source = TISCopyCurrentKeyboardInputSource();
    if (!source) { kp_fail("Could not read the active keyboard input source."); return 0; }
    CFDataRef data = (CFDataRef)TISGetInputSourceProperty(source, kTISPropertyUnicodeKeyLayoutData);
    if (!data || CFGetTypeID(data) != CFDataGetTypeID()
        || CFDataGetLength(data) < (CFIndex)sizeof(UCKeyboardLayout)) {
        CFRelease(source);
        kp_fail("The active input method has no direct keyboard layout. Select a keyboard layout, or use Unicode mode if the console supports it.");
        return 0;
    }
    int caps = !!(kp_hardware_flags() & kCGEventFlagMaskAlphaShift);
    if (kp_cached_layout && kp_cached_caps == caps && CFEqual(data, kp_cached_layout)) {
        CFRelease(source);
        return 1;
    }
    if (kp_cached_layout) CFRelease(kp_cached_layout);
    kp_cached_layout = (CFDataRef)CFRetain(data);
    kp_cached_caps = caps;
    kp_map_count = 0;
    const UCKeyboardLayout *layout = (const UCKeyboardLayout *)CFDataGetBytePtr(data);
    const UInt32 carbon_modifiers[4] = {0, shiftKey, optionKey, shiftKey | optionKey};
    const CGEventFlags quartz_modifiers[4] = {0, kCGEventFlagMaskShift,
        kCGEventFlagMaskAlternate, kCGEventFlagMaskShift | kCGEventFlagMaskAlternate};
    for (int combination = 0; combination < 4; ++combination) {
        UInt32 mods = carbon_modifiers[combination] | (caps ? alphaLock : 0);
        for (CGKeyCode key = 0; key < 128; ++key) {
            if (!kp_is_text_key(key)) continue;
            UInt32 dead_state = 0;
            UniChar text[4];
            UniCharCount length = 0;
            /* Do not set NoDeadKeys: accepting the displayed accent from a
               dead key would leave a composition pending in the actual app. */
            OSStatus result = UCKeyTranslate(layout, key, kUCKeyActionDown,
                (mods >> 8) & 0xff, LMGetKbdType(), 0, &dead_state,
                4, &length, text);
            uint32_t scalar = (result == noErr && dead_state == 0)
                ? kp_decode_utf16(text, length) : 0;
            if (scalar >= 0x20 && kp_valid_scalar(scalar) && kp_map_count < KP_MAP_CAPACITY) {
                kp_map[kp_map_count++] = (struct kp_mapping){scalar, key, quartz_modifiers[combination]};
            }
        }
    }
    CFRelease(source);
    return 1;
}

static int kp_find_mapping(uint32_t scalar, struct kp_mapping *mapping) {
    if (!kp_valid_scalar(scalar)) { kp_fail("Unsupported control character or invalid Unicode scalar."); return 0; }
    if (scalar == '\n' || scalar == '\t') {
        *mapping = (struct kp_mapping){scalar, scalar == '\n' ? kVK_Return : kVK_Tab, 0};
        return 1;
    }
    if (kp_mode == 1) { *mapping = (struct kp_mapping){scalar, 0, 0}; return 1; }
    if (!kp_refresh_mapping()) return 0;
    for (size_t i = 0; i < kp_map_count; ++i) {
        if (kp_map[i].scalar == scalar) { *mapping = kp_map[i]; return 1; }
    }
    kp_fail("A character is unavailable as a single key in the active layout; Unicode mode may work if the remote console accepts text events.");
    return 0;
}

int kp_can_type(uint32_t scalar) {
    struct kp_mapping mapping;
    return kp_find_mapping(scalar, &mapping);
}

static CGEventRef kp_make_key(CGKeyCode key, int down, int modifier) {
    CGEventRef event = CGEventCreateKeyboardEvent(kp_event_source, key, down);
    if (event) {
        CGEventSetIntegerValueField(event, kCGEventSourceUserData, KP_EVENT_MARKER);
        CGEventSetIntegerValueField(event, kCGKeyboardEventAutorepeat, 0);
        if (modifier) CGEventSetType(event, kCGEventFlagsChanged);
    }
    return event;
}

static void kp_post(CGEventRef event, CGEventFlags owned_flags) {
    uint64_t ticks = mach_absolute_time();
    /* Events are allocated before posting, so refresh their timestamp here;
       the key-up timestamp must include the configured hold interval. */
    uint64_t nanos = (ticks / kp_timebase.denom) * kp_timebase.numer
        + ((ticks % kp_timebase.denom) * kp_timebase.numer) / kp_timebase.denom;
    CGEventSetTimestamp(event, nanos);
    CGEventSetFlags(event, kp_hardware_flags() | owned_flags);
    CGEventPost(kCGHIDEventTap, event);
}

int kp_type(uint32_t scalar, int hold_ms) {
    struct kp_mapping mapping;
    if (!kp_initialized) { kp_fail("Keypour is not initialized."); return 0; }
    if (hold_ms < 1 || hold_ms > 100) { kp_fail("Key hold time must be between 1 and 100 ms."); return 0; }
    if (!kp_find_mapping(scalar, &mapping)) return 0;
    if (!AXIsProcessTrusted() || !CGPreflightPostEventAccess()) {
        kp_fail("Accessibility permission was revoked; no more keys were posted.");
        return 0;
    }
    if (kp_modifiers_down()) { kp_fail("A physical modifier or mouse button is held; typing stopped."); return 0; }
    if (!kp_target_matches()) { kp_fail("The target window lost focus."); return 0; }
    CGKeyCode mod_keys[2] = {kVK_Shift, kVK_Option};
    CGEventFlags mod_flags[2] = {kCGEventFlagMaskShift, kCGEventFlagMaskAlternate};
    CGEventRef mod_down[2] = {NULL, NULL};
    CGEventRef mod_up[2] = {NULL, NULL};
    CGEventRef key_down = kp_make_key(mapping.key, 1, 0);
    CGEventRef key_up = kp_make_key(mapping.key, 0, 0);
    int allocated = key_down && key_up;
    for (int i = 0; i < 2; ++i) {
        if (mapping.modifiers & mod_flags[i]) {
            mod_down[i] = kp_make_key(mod_keys[i], 1, 1);
            mod_up[i] = kp_make_key(mod_keys[i], 0, 1);
            if (!mod_down[i] || !mod_up[i]) allocated = 0;
        }
    }
    if (!allocated) {
        kp_fail("Could not allocate keyboard events; no keys were posted.");
        goto cleanup;
    }
    if (kp_mode == 1 && scalar != '\n' && scalar != '\t') {
        UniChar text[2];
        UniCharCount length = 1;
        if (scalar > 0xffff) {
            uint32_t n = scalar - 0x10000;
            text[0] = (UniChar)(0xd800 + (n >> 10));
            text[1] = (UniChar)(0xdc00 + (n & 0x3ff));
            length = 2;
        } else text[0] = (UniChar)scalar;
        CGEventKeyboardSetUnicodeString(key_down, length, text);
        CGEventKeyboardSetUnicodeString(key_up, length, text);
    }
    CGEventFlags owned = 0;
    for (int i = 0; i < 2; ++i) {
        if (mod_down[i]) {
            owned |= mod_flags[i];
            kp_post(mod_down[i], owned);
        }
    }
    kp_post(key_down, owned);
    struct timespec remaining = {hold_ms / 1000, (long)(hold_ms % 1000) * 1000000L};
    while (nanosleep(&remaining, &remaining) != 0 && errno == EINTR) { }
    kp_post(key_up, owned);
    for (int i = 1; i >= 0; --i) {
        if (mod_up[i]) {
            owned &= ~mod_flags[i];
            /* If the user pressed this physical key during the short hold,
               leave its key-up to that user; never release their modifier. */
            if (!CGEventSourceKeyState(kCGEventSourceStateHIDSystemState, mod_keys[i]))
                kp_post(mod_up[i], owned);
        }
    }
cleanup:
    if (key_down) CFRelease(key_down);
    if (key_up) CFRelease(key_up);
    for (int i = 0; i < 2; ++i) {
        if (mod_down[i]) CFRelease(mod_down[i]);
        if (mod_up[i]) CFRelease(mod_up[i]);
    }
    return allocated;
}
