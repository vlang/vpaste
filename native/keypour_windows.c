/* Keypour: native Win32 input adapter. Original implementation, MIT licensed.
 * Requires Windows 10 1607+ for non-mutating ToUnicodeEx translation.
 * Native APIs only: User32 and the standard C runtime.
 */
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include "keypour.h"

#define KP_HOTKEY_ID 0x4B50
#define KP_INPUT_TAG ((ULONG_PTR)0x4B505231)

static HHOOK kp_keyboard_hook;
static HHOOK kp_mouse_hook;
static HWND kp_target;
static HWND kp_clicked;
static DWORD kp_target_pid;
static DWORD kp_target_thread;
static HKL kp_target_layout;
static unsigned int kp_target_locks;
static int kp_active;
static int kp_pending;
static int kp_mode;
static int kp_registered;
static UINT kp_registered_vk;
static int kp_registered_modifiers;
static int kp_hotkey_latched;
static int kp_ctrl_handler_installed;
static int kp_escape_consumed;
static unsigned char kp_physical[256];
static volatile LONG kp_quit_requested;
static volatile LONG kp_clipboard_busy;
static char kp_message[256];

static void kp_fail(const char *message) {
    snprintf(kp_message, sizeof(kp_message), "%s", message);
}

static void kp_fail_win32(const char *message, DWORD code) {
    snprintf(kp_message, sizeof(kp_message), "%s (Windows error %lu).",
             message, (unsigned long)code);
}

const char *kp_error(void) {
    return kp_message[0] ? kp_message : "Windows input operation failed.";
}

void kp_wipe(void *data, size_t len) {
    if (data && len) SecureZeroMemory(data, len);
}

void kp_free_text(char *text, int len) {
    if (!text) return;
    if (len >= 0) kp_wipe(text, (size_t)len + 1);
    free(text);
}

static int kp_is_modifier(UINT vk) {
    return vk == VK_LSHIFT || vk == VK_RSHIFT ||
           vk == VK_LCONTROL || vk == VK_RCONTROL ||
           vk == VK_LMENU || vk == VK_RMENU ||
           vk == VK_LWIN || vk == VK_RWIN;
}

static UINT kp_specific_vk(const KBDLLHOOKSTRUCT *key) {
    UINT vk = key->vkCode;
    if (vk == VK_SHIFT)
        return key->scanCode == 0x36 ? VK_RSHIFT : VK_LSHIFT;
    if (vk == VK_CONTROL)
        return (key->flags & LLKHF_EXTENDED) ? VK_RCONTROL : VK_LCONTROL;
    if (vk == VK_MENU)
        return (key->flags & LLKHF_EXTENDED) ? VK_RMENU : VK_LMENU;
    return vk;
}

static LRESULT CALLBACK kp_keyboard_proc(int code, WPARAM message, LPARAM data) {
    if (code == HC_ACTION) {
        const KBDLLHOOKSTRUCT *key = (const KBDLLHOOKSTRUCT *)data;
        /* Also debounce at the hook boundary. This covers implementations that
         * still deliver repeated WM_HOTKEY for repeated injected key-downs. */
        if (key->vkCode == kp_registered_vk &&
            (message == WM_KEYUP || message == WM_SYSKEYUP))
            kp_hotkey_latched = 0;
        if (!(key->flags & LLKHF_INJECTED)) {
            UINT vk = kp_specific_vk(key);
            int down = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
            int up = message == WM_KEYUP || message == WM_SYSKEYUP;
            if (vk < 256 && (down || up)) kp_physical[vk] = (unsigned char)down;
            if (vk == VK_ESCAPE && (down || up)) {
                if (kp_active && down) {
                    kp_pending |= KP_EVENT_CANCEL;
                    kp_escape_consumed = 1;
                    return 1;
                }
                /* Match a consumed key-down even if V has already disarmed. */
                if (kp_escape_consumed) {
                    if (up) kp_escape_consumed = 0;
                    return 1;
                }
            }
        }
    }
    return CallNextHookEx(kp_keyboard_hook, code, message, data);
}

static LRESULT CALLBACK kp_mouse_proc(int code, WPARAM message, LPARAM data) {
    if (code == HC_ACTION && kp_active && message == WM_LBUTTONUP) {
        const MSLLHOOKSTRUCT *mouse = (const MSLLHOOKSTRUCT *)data;
        if (!(mouse->flags & LLMHF_INJECTED)) {
            HWND hit = WindowFromPoint(mouse->pt);
            kp_clicked = hit ? GetAncestor(hit, GA_ROOT) : NULL;
            kp_pending |= KP_EVENT_CLICK;
        }
    }
    return CallNextHookEx(kp_mouse_hook, code, message, data);
}

static BOOL WINAPI kp_console_handler(DWORD event) {
    if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT) {
        InterlockedExchange(&kp_quit_requested, 1);
        return TRUE;
    }
    return FALSE;
}

static void kp_pump(void) {
    MSG message;
    unsigned int count = 0;
    while (count++ < 256 && PeekMessageW(&message, NULL, 0, 0, PM_REMOVE)) {
        if (message.message == WM_QUIT) {
            kp_pending |= KP_EVENT_QUIT;
        } else if (message.message == WM_HOTKEY &&
                   message.wParam == (WPARAM)KP_HOTKEY_ID) {
            if (!kp_hotkey_latched) {
                kp_hotkey_latched = 1;
                kp_pending |= KP_EVENT_HOTKEY;
            }
        } else {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    if (InterlockedExchange(&kp_quit_requested, 0)) kp_pending |= KP_EVENT_QUIT;
}

int kp_poll(void) {
    int events;
    kp_pump();
    events = kp_pending;
    kp_pending = 0;
    return events;
}

static UINT kp_hotkey_code(const char *key) {
    size_t len;
    if (!key) return 0;
    len = strlen(key);
    if (len == 1 && ((key[0] >= 'A' && key[0] <= 'Z') ||
                     (key[0] >= '0' && key[0] <= '9')))
        return (UINT)(unsigned char)key[0];
    if (len == 2 && key[0] == 'F' && key[1] >= '1' && key[1] <= '9')
        return VK_F1 + (UINT)(key[1] - '1');
    if (len == 3 && key[0] == 'F' && key[1] == '1' && key[2] >= '0' && key[2] <= '2')
        return VK_F10 + (UINT)(key[2] - '0');
    return 0;
}

static int kp_supported_windows(void) {
    typedef LONG (WINAPI *KpRtlGetVersion)(OSVERSIONINFOW *);
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    KpRtlGetVersion get_version;
    FARPROC address;
    OSVERSIONINFOW version;
    if (!ntdll) return 0;
    /* Avoid GetVersion's compatibility-manifest-dependent version reporting. */
    address = GetProcAddress(ntdll, "RtlGetVersion");
    if (!address || sizeof(address) != sizeof(get_version)) return 0;
    memcpy(&get_version, &address, sizeof(get_version));
    memset(&version, 0, sizeof(version));
    version.dwOSVersionInfoSize = sizeof(version);
    if (get_version(&version) != 0) return 0;
    return version.dwMajorVersion > 10 ||
           (version.dwMajorVersion == 10 && version.dwBuildNumber >= 14393);
}

int kp_init(int modifiers, const char *key, int mode) {
    UINT native_modifiers = MOD_NOREPEAT;
    UINT vk = kp_hotkey_code(key);
    MSG unused;
    unsigned int i;
    kp_shutdown();
    kp_message[0] = 0;
    if (!kp_supported_windows()) {
        kp_fail("Keypour requires Windows 10 version 1607 or newer.");
        return 0;
    }
    if (!vk || !modifiers || (modifiers & ~15) || (mode != 0 && mode != 1)) {
        kp_fail("Invalid Windows hotkey or input mode.");
        return 0;
    }
    if (modifiers & KP_MOD_CTRL) native_modifiers |= MOD_CONTROL;
    if (modifiers & KP_MOD_ALT) native_modifiers |= MOD_ALT;
    if (modifiers & KP_MOD_SHIFT) native_modifiers |= MOD_SHIFT;
    if (modifiers & KP_MOD_SUPER) native_modifiers |= MOD_WIN;
    /* Establish this thread's message queue before registering a thread hotkey. */
    PeekMessageW(&unused, NULL, WM_USER, WM_USER, PM_NOREMOVE);
    if (!RegisterHotKey(NULL, KP_HOTKEY_ID, native_modifiers, vk)) {
        kp_fail_win32("Cannot register hotkey; it may be reserved or already in use", GetLastError());
        return 0;
    }
    kp_registered = 1;
    kp_registered_vk = vk;
    kp_registered_modifiers = modifiers;
    kp_mode = mode;
    for (i = 0; i < 256; ++i)
        kp_physical[i] = (unsigned char)((GetAsyncKeyState((int)i) & 0x8000) != 0);
    kp_keyboard_hook = SetWindowsHookExW(WH_KEYBOARD_LL, kp_keyboard_proc,
                                         GetModuleHandleW(NULL), 0);
    if (!kp_keyboard_hook) {
        kp_fail_win32("Cannot install keyboard cancellation hook", GetLastError());
        kp_shutdown();
        return 0;
    }
    kp_mouse_hook = SetWindowsHookExW(WH_MOUSE_LL, kp_mouse_proc,
                                      GetModuleHandleW(NULL), 0);
    if (!kp_mouse_hook) {
        kp_fail_win32("Cannot install target-selection mouse hook", GetLastError());
        kp_shutdown();
        return 0;
    }
    kp_ctrl_handler_installed = SetConsoleCtrlHandler(kp_console_handler, TRUE) != 0;
    return 1;
}

void kp_shutdown(void) {
    if (kp_keyboard_hook) {
        UnhookWindowsHookEx(kp_keyboard_hook);
        kp_keyboard_hook = NULL;
    }
    if (kp_mouse_hook) {
        UnhookWindowsHookEx(kp_mouse_hook);
        kp_mouse_hook = NULL;
    }
    if (kp_registered) {
        UnregisterHotKey(NULL, KP_HOTKEY_ID);
        kp_registered = 0;
    }
    if (kp_ctrl_handler_installed) {
        SetConsoleCtrlHandler(kp_console_handler, FALSE);
        kp_ctrl_handler_installed = 0;
    }
    kp_active = 0;
    kp_registered_vk = 0;
    kp_registered_modifiers = 0;
    kp_hotkey_latched = 0;
    kp_pending = 0;
    kp_escape_consumed = 0;
    kp_target = kp_clicked = NULL;
    kp_target_pid = kp_target_thread = 0;
    kp_target_layout = NULL;
    kp_target_locks = 0;
    InterlockedExchange(&kp_quit_requested, 0);
    memset(kp_physical, 0, sizeof(kp_physical));
}

void kp_set_active(int active) {
    kp_active = active != 0;
    kp_pending &= ~(KP_EVENT_CLICK | KP_EVENT_CANCEL);
    kp_clicked = kp_target = NULL;
    kp_target_pid = kp_target_thread = 0;
    kp_target_layout = NULL;
    kp_target_locks = 0;
}

/* GetClipboardData may wait for delayed rendering by an unresponsive owner.
 * Keep it off the hook/message thread. A timed-out worker owns its resources
 * until Windows returns, then wipes them; it never enters V or types anything.
 */
typedef struct KpClipboardJob {
    volatile LONG references;
    HANDLE done;
    int max_bytes;
    int length;
    char *text;
    char error[256];
} KpClipboardJob;

static void kp_clipboard_release(KpClipboardJob *job) {
    if (InterlockedDecrement(&job->references) == 0) {
        if (job->text) kp_free_text(job->text, job->length);
        CloseHandle(job->done);
        free(job);
    }
}

static DWORD WINAPI kp_clipboard_worker(void *argument) {
    KpClipboardJob *job = (KpClipboardJob *)argument;
    ULONGLONG deadline = GetTickCount64() + 100;
    HANDLE handle = NULL;
    const WCHAR *wide = NULL;
    SIZE_T capacity = 0, length = 0;
    int bytes = 0, opened = 0;
    const char *failure = NULL;
    while (!OpenClipboard(NULL)) {
        if (GetTickCount64() >= deadline) {
            failure = "Clipboard is busy; retry the hotkey.";
            goto complete;
        }
        Sleep(5);
    }
    opened = 1;
    handle = GetClipboardData(CF_UNICODETEXT);
    if (!handle) {
        failure = "Clipboard does not contain plain text.";
        goto complete;
    }
    capacity = GlobalSize(handle) / sizeof(WCHAR);
    wide = (const WCHAR *)GlobalLock(handle);
    if (!wide || !capacity) {
        failure = "Cannot access clipboard text.";
        goto complete;
    }
    /* UTF-8 cannot need fewer bytes than UTF-16 code units for valid text. */
    for (length = 0; length < capacity && length <= (SIZE_T)job->max_bytes; ++length)
        if (wide[length] == 0) break;
    if (length >= capacity || length > (SIZE_T)job->max_bytes) {
        failure = "Clipboard text is too large or is not terminated correctly.";
        goto complete;
    }
    bytes = length ? WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide,
                                          (int)length, NULL, 0, NULL, NULL) : 0;
    if ((length && bytes <= 0) || bytes > job->max_bytes) {
        failure = "Clipboard text exceeds the limit or has invalid Unicode.";
        goto complete;
    }
    job->text = (char *)malloc((size_t)bytes + 1);
    if (!job->text) {
        failure = "Cannot allocate clipboard snapshot.";
        goto complete;
    }
    job->length = bytes;
    if (bytes && WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide,
                                      (int)length, job->text, bytes, NULL, NULL) != bytes) {
        failure = "Cannot convert clipboard text to UTF-8.";
        goto complete;
    }
    job->text[bytes] = 0;
complete:
    if (wide) GlobalUnlock(handle);
    if (opened) CloseClipboard();
    if (failure) {
        snprintf(job->error, sizeof(job->error), "%s", failure);
        if (job->text) {
            kp_free_text(job->text, job->length);
            job->text = NULL;
            job->length = 0;
        }
    }
    InterlockedExchange(&kp_clipboard_busy, 0);
    SetEvent(job->done);
    kp_clipboard_release(job);
    return 0;
}

char *kp_clipboard(int max_bytes, int *out_len) {
    KpClipboardJob *job;
    HANDLE worker;
    ULONGLONG deadline;
    char *text = NULL;
    DWORD result;
    if (out_len) *out_len = 0;
    if (!out_len || max_bytes <= 0 || max_bytes == INT_MAX) {
        kp_fail("Invalid clipboard size limit.");
        return NULL;
    }
    if (InterlockedCompareExchange(&kp_clipboard_busy, 1, 0)) {
        kp_fail("A previous clipboard owner is still not responding; retry later.");
        return NULL;
    }
    job = (KpClipboardJob *)calloc(1, sizeof(*job));
    if (!job) {
        InterlockedExchange(&kp_clipboard_busy, 0);
        kp_fail("Cannot allocate clipboard snapshot state.");
        return NULL;
    }
    job->references = 2; /* One reference for this caller and one for the worker. */
    job->max_bytes = max_bytes;
    job->done = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!job->done) {
        free(job);
        InterlockedExchange(&kp_clipboard_busy, 0);
        kp_fail("Cannot create clipboard completion event.");
        return NULL;
    }
    worker = CreateThread(NULL, 0, kp_clipboard_worker, job, 0, NULL);
    if (!worker) {
        kp_clipboard_release(job);
        kp_clipboard_release(job);
        InterlockedExchange(&kp_clipboard_busy, 0);
        kp_fail("Cannot start clipboard reader.");
        return NULL;
    }
    CloseHandle(worker);
    deadline = GetTickCount64() + 200;
    for (;;) {
        result = WaitForSingleObject(job->done, 5);
        kp_pump();
        if (result == WAIT_OBJECT_0) {
            if (job->text) {
                text = job->text;
                job->text = NULL;
                *out_len = job->length;
            } else {
                kp_fail(job->error[0] ? job->error : "Cannot read clipboard text.");
            }
            break;
        }
        if (result == WAIT_FAILED || GetTickCount64() >= deadline) {
            kp_fail("Clipboard owner did not respond within 200 ms; retry the hotkey.");
            break;
        }
        if (kp_pending & (KP_EVENT_CANCEL | KP_EVENT_QUIT | KP_EVENT_HOTKEY)) {
            kp_pending &= ~(KP_EVENT_CANCEL | KP_EVENT_HOTKEY);
            kp_fail("Clipboard snapshot cancelled.");
            break;
        }
    }
    kp_clipboard_release(job);
    return text;
}

static HWND kp_foreground_root(void) {
    HWND foreground = GetForegroundWindow();
    return foreground ? GetAncestor(foreground, GA_ROOT) : NULL;
}

static unsigned int kp_lock_state(void) {
    return ((GetKeyState(VK_CAPITAL) & 1) ? 1U : 0U) |
           ((GetKeyState(VK_NUMLOCK) & 1) ? 2U : 0U) |
           ((GetKeyState(VK_SCROLL) & 1) ? 4U : 0U);
}

int kp_capture_target(void) {
    HWND foreground = kp_foreground_root();
    DWORD pid = 0;
    DWORD thread;
    if (!kp_active || !foreground || !kp_clicked || foreground != kp_clicked ||
        foreground == GetDesktopWindow() || foreground == GetShellWindow()) {
        kp_fail("Click inside the target window; it must become the foreground window.");
        return 0;
    }
    thread = GetWindowThreadProcessId(foreground, &pid);
    if (!thread || !pid || pid == GetCurrentProcessId()) {
        kp_fail("Cannot select this window as the typing target.");
        return 0;
    }
    kp_target = foreground;
    kp_target_pid = pid;
    kp_target_thread = thread;
    kp_target_layout = GetKeyboardLayout(thread);
    kp_target_locks = kp_lock_state();
    if (!kp_target_layout) {
        kp_fail("Cannot determine the target keyboard layout.");
        kp_target = NULL;
        return 0;
    }
    return 1;
}

int kp_target_matches(void) {
    DWORD pid = 0;
    DWORD thread;
    if (!kp_target || !IsWindow(kp_target) || kp_foreground_root() != kp_target) {
        kp_fail("The focused window changed; typing was cancelled.");
        return 0;
    }
    thread = GetWindowThreadProcessId(kp_target, &pid);
    if (pid != kp_target_pid || thread != kp_target_thread ||
        GetKeyboardLayout(thread) != kp_target_layout ||
        kp_lock_state() != kp_target_locks) {
        kp_fail("The target, keyboard layout or lock state changed; typing was cancelled.");
        return 0;
    }
    return 1;
}

int kp_modifiers_down(void) {
    static const int keys[] = {
        VK_LSHIFT, VK_RSHIFT, VK_LCONTROL, VK_RCONTROL,
        VK_LMENU, VK_RMENU, VK_LWIN, VK_RWIN,
        VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2
    };
    size_t i;
    for (i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i)
        if (GetAsyncKeyState(keys[i]) & 0x8000) return 1;
    return 0;
}

static int kp_valid_scalar(uint32_t scalar) {
    return scalar == '\n' || scalar == '\t' ||
           (scalar >= 0x20 && scalar <= 0x10FFFF &&
            !(scalar >= 0x7F && scalar <= 0x9F) &&
            !(scalar >= 0xD800 && scalar <= 0xDFFF));
}

typedef struct KpMapping {
    UINT vk;
    unsigned int modifiers; /* VkKeyScanEx bits: Shift=1, Ctrl=2, Alt=4. */
} KpMapping;

static int kp_translation_matches(UINT vk, unsigned int modifiers, int caps,
                                  WCHAR desired, HKL layout) {
    BYTE state[256];
    WCHAR text[8];
    UINT scan;
    int count;
    memset(state, 0, sizeof(state));
    memset(text, 0, sizeof(text));
    if (modifiers & 1) state[VK_SHIFT] = state[VK_LSHIFT] = 0x80;
    if (modifiers & 2) state[VK_CONTROL] = state[VK_LCONTROL] = 0x80;
    if (modifiers & 4) {
        state[VK_MENU] = 0x80;
        state[(modifiers & 2) ? VK_RMENU : VK_LMENU] = 0x80;
    }
    if (caps) state[VK_CAPITAL] = 1;
    scan = MapVirtualKeyExW(vk, MAPVK_VK_TO_VSC_EX, layout);
    if (!scan) return 0;
    /* Bit 2 explicitly preserves the kernel dead-key state on Windows 10 1607+. */
    count = ToUnicodeEx(vk, scan, state, text, (int)(sizeof(text) / sizeof(text[0])),
                        4, layout);
    return count == 1 && text[0] == desired;
}

static int kp_map_scalar(uint32_t scalar, KpMapping *mapping) {
    SHORT key;
    unsigned int modifiers;
    int caps;
    if (!kp_valid_scalar(scalar)) {
        kp_fail("Text contains an unsupported control character or invalid Unicode.");
        return 0;
    }
    if (scalar == '\n' || scalar == '\t') {
        mapping->vk = scalar == '\n' ? VK_RETURN : VK_TAB;
        mapping->modifiers = 0;
        return 1;
    }
    if (scalar > 0xFFFF || !kp_target_layout) {
        kp_fail("A character has no physical key mapping in the target layout; use Unicode mode if the target supports it.");
        return 0;
    }
    key = VkKeyScanExW((WCHAR)scalar, kp_target_layout);
    if (key == -1) {
        kp_fail("A character has no physical key mapping in the target layout; use Unicode mode if the target supports it.");
        return 0;
    }
    modifiers = ((unsigned int)(unsigned short)key >> 8) & 0xFF;
    if (modifiers & ~7U) {
        kp_fail("The target layout requires an unsupported keyboard shift state.");
        return 0;
    }
    mapping->vk = (UINT)(key & 0xFF);
    caps = (kp_target_locks & 1U) != 0;
    if (!kp_translation_matches(mapping->vk, modifiers, caps, (WCHAR)scalar,
                                kp_target_layout)) {
        modifiers ^= 1;
        if (!kp_translation_matches(mapping->vk, modifiers, caps, (WCHAR)scalar,
                                    kp_target_layout)) {
            kp_fail("A character needs a dead key, composition, or an unsupported layout mapping.");
            return 0;
        }
    }
    if (mapping->vk == kp_registered_vk &&
        (((modifiers & 1) ? KP_MOD_SHIFT : 0) |
         ((modifiers & 2) ? KP_MOD_CTRL : 0) |
         ((modifiers & 4) ? KP_MOD_ALT : 0)) == kp_registered_modifiers) {
        kp_fail("A character conflicts with the global hotkey; choose a different hotkey or Unicode mode.");
        return 0;
    }
    mapping->modifiers = modifiers;
    return 1;
}

int kp_can_type(uint32_t scalar) {
    KpMapping mapping;
    if (!kp_valid_scalar(scalar)) {
        kp_fail("Text contains an unsupported control character or invalid Unicode.");
        return 0;
    }
    if (kp_mode == 1 && scalar != '\n' && scalar != '\t') return 1;
    return kp_map_scalar(scalar, &mapping);
}

static int kp_make_key(INPUT *input, UINT vk, int down) {
    UINT scan = MapVirtualKeyExW(vk, MAPVK_VK_TO_VSC_EX, kp_target_layout);
    if (!scan || (scan & 0xFF00) == 0xE100) {
        kp_fail("The target key has no supported scan code.");
        return 0;
    }
    memset(input, 0, sizeof(*input));
    input->type = INPUT_KEYBOARD;
    /* Windows ignores wVk with KEYEVENTF_SCANCODE; keeping the matching value
     * also lets Win32 compatibility layers translate the physical event. */
    input->ki.wVk = (WORD)vk;
    input->ki.wScan = (WORD)(scan & 0xFF);
    input->ki.dwFlags = KEYEVENTF_SCANCODE |
                        ((scan & 0xFF00) == 0xE000 ? KEYEVENTF_EXTENDEDKEY : 0) |
                        (down ? 0 : KEYEVENTF_KEYUP);
    input->ki.dwExtraInfo = KP_INPUT_TAG;
    return 1;
}

static void kp_hold(int hold_ms) {
    ULONGLONG deadline = GetTickCount64() + (ULONGLONG)hold_ms;
    while (GetTickCount64() < deadline) {
        DWORD remaining = (DWORD)(deadline - GetTickCount64());
        Sleep(remaining > 5 ? 5 : remaining);
        kp_pump();
        if (kp_pending & (KP_EVENT_CANCEL | KP_EVENT_QUIT)) break;
    }
}

static int kp_send_physical(KpMapping mapping, int hold_ms) {
    INPUT down[4];
    INPUT up[4];
    UINT keys[4];
    UINT count = 0, inserted, up_count = 0;
    DWORD failure;
    int i;
    if (mapping.modifiers & 2) keys[count++] = VK_LCONTROL;
    if (mapping.modifiers & 4)
        keys[count++] = (mapping.modifiers & 2) ? VK_RMENU : VK_LMENU;
    if (mapping.modifiers & 1) keys[count++] = VK_LSHIFT;
    keys[count++] = mapping.vk;
    for (i = 0; i < (int)count; ++i)
        if (!kp_make_key(&down[i], keys[i], 1)) return 0;
    SetLastError(0);
    inserted = SendInput(count, down, sizeof(INPUT));
    failure = GetLastError();
    if (inserted == count) kp_hold(hold_ms);
    else kp_pump();
    /* Release only events actually accepted. If the user pressed the same
     * modifier during the hold, its later physical release owns that state. */
    for (i = (int)inserted - 1; i >= 0; --i) {
        if (kp_is_modifier(keys[i]) && kp_physical[keys[i]]) continue;
        up[up_count] = down[i];
        up[up_count].ki.dwFlags |= KEYEVENTF_KEYUP;
        ++up_count;
    }
    if (up_count && SendInput(up_count, up, sizeof(INPUT)) != up_count) {
        kp_fail("Windows rejected key release; check target permissions and release any stuck modifier manually.");
        return 0;
    }
    if (inserted != count) {
        kp_fail_win32("Windows rejected input; the target may require the same elevation level", failure);
        return 0;
    }
    return 1;
}

static int kp_send_unicode_unit(WORD unit, int hold_ms) {
    INPUT input;
    DWORD failure;
    memset(&input, 0, sizeof(input));
    input.type = INPUT_KEYBOARD;
    input.ki.wScan = unit;
    input.ki.dwFlags = KEYEVENTF_UNICODE;
    input.ki.dwExtraInfo = KP_INPUT_TAG;
    SetLastError(0);
    if (SendInput(1, &input, sizeof(INPUT)) != 1) {
        failure = GetLastError();
        kp_fail_win32("Windows rejected Unicode input; check target permissions", failure);
        return 0;
    }
    kp_hold(hold_ms);
    input.ki.dwFlags |= KEYEVENTF_KEYUP;
    if (SendInput(1, &input, sizeof(INPUT)) != 1) {
        kp_fail("Windows rejected the Unicode key-up event.");
        return 0;
    }
    return 1;
}

int kp_type(uint32_t scalar, int hold_ms) {
    KpMapping mapping;
    kp_pump();
    if (kp_pending & (KP_EVENT_CANCEL | KP_EVENT_QUIT | KP_EVENT_HOTKEY | KP_EVENT_CLICK)) {
        kp_fail("Typing was cancelled by a new input event.");
        return 0;
    }
    if (hold_ms < 1 || hold_ms > 100 || !kp_valid_scalar(scalar)) {
        kp_fail("Invalid character or key-hold duration.");
        return 0;
    }
    if (!kp_target_matches()) return 0;
    if (kp_modifiers_down()) {
        kp_fail("A modifier or mouse button was pressed; typing was cancelled.");
        return 0;
    }
    if (kp_mode == 1 && scalar != '\n' && scalar != '\t') {
        if (scalar <= 0xFFFF) return kp_send_unicode_unit((WORD)scalar, hold_ms);
        scalar -= 0x10000;
        if (!kp_send_unicode_unit((WORD)(0xD800 + (scalar >> 10)), hold_ms)) return 0;
        return kp_send_unicode_unit((WORD)(0xDC00 + (scalar & 0x3FF)), hold_ms);
    }
    if (!kp_map_scalar(scalar, &mapping)) return 0;
    return kp_send_physical(mapping, hold_ms);
}
