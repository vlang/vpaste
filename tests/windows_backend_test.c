/* Integration harness for an isolated Windows desktop, or Wine + Xvfb.
 * It overwrites that desktop's clipboard. Never run on a working user desktop.
 * Build (source includes the adapter for its hook-boundary tests):
 *   gcc -std=c11 -O2 -Wall -Wextra -Werror tests/windows_backend_test.c \
 *       -o build/windows_backend_test.exe -luser32
 * It creates a separate process with real EDIT controls, checks actual typed
 * text/keyboard state, and simulates non-injected events at the hook boundary.
 */
#include "../native/keypour_windows.c"
#include <wchar.h>

static int failures;
static const WCHAR *receiver_class = L"KeypourNativeTestReceiver";
static HWND receiver_edit;

static void check(int condition, const char *name) {
    printf("%s %s\n", condition ? "PASS" : "FAIL", name);
    fflush(stdout);
    if (!condition) ++failures;
}

static void pause_pumping(unsigned int milliseconds) {
    ULONGLONG deadline = GetTickCount64() + milliseconds;
    while (GetTickCount64() < deadline) {
        kp_pump();
        Sleep(2);
    }
}

static int set_clipboard_units(const WCHAR *text, SIZE_T units, int terminated) {
    HGLOBAL allocation;
    WCHAR *data;
    SIZE_T size, i;
    ULONGLONG deadline = GetTickCount64() + 1000;
    while (!OpenClipboard(NULL)) {
        if (GetTickCount64() > deadline) return 0;
        Sleep(5);
    }
    if (!EmptyClipboard()) {
        CloseClipboard();
        return 0;
    }
    allocation = GlobalAlloc(GMEM_MOVEABLE, units * sizeof(WCHAR));
    if (!allocation) {
        CloseClipboard();
        return 0;
    }
    size = GlobalSize(allocation) / sizeof(WCHAR);
    data = (WCHAR *)GlobalLock(allocation);
    if (!data) {
        GlobalFree(allocation);
        CloseClipboard();
        return 0;
    }
    for (i = 0; i < size; ++i) data[i] = L'X';
    memcpy(data, text, units * sizeof(WCHAR));
    if (terminated) data[units - 1] = 0;
    GlobalUnlock(allocation);
    if (!SetClipboardData(CF_UNICODETEXT, allocation)) {
        GlobalFree(allocation);
        CloseClipboard();
        return 0;
    }
    CloseClipboard();
    return 1;
}

static LRESULT CALLBACK receiver_proc(HWND window, UINT message, WPARAM w, LPARAM l) {
    if (message == WM_CREATE) {
        receiver_edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN,
            15, 15, 470, 230, window, (HMENU)(UINT_PTR)100, GetModuleHandleW(NULL), NULL);
        SetFocus(receiver_edit);
        return 0;
    }
    if (message == WM_SETFOCUS && receiver_edit) {
        SetFocus(receiver_edit);
        return 0;
    }
    if (message == WM_APP + 1) {
        if (OpenClipboard(window)) {
            EmptyClipboard();
            SetClipboardData(CF_UNICODETEXT, NULL);
            CloseClipboard();
        }
        return 0;
    }
    if (message == WM_RENDERFORMAT && w == CF_UNICODETEXT) {
        const WCHAR delayed[] = L"delayed";
        HGLOBAL allocation;
        WCHAR *data;
        Sleep(450);
        allocation = GlobalAlloc(GMEM_MOVEABLE, sizeof(delayed));
        if (!allocation) return 0;
        data = (WCHAR *)GlobalLock(allocation);
        if (!data) {
            GlobalFree(allocation);
            return 0;
        }
        memcpy(data, delayed, sizeof(delayed));
        GlobalUnlock(allocation);
        if (!SetClipboardData(CF_UNICODETEXT, allocation)) GlobalFree(allocation);
        return 0;
    }
    if (message == WM_CLOSE) {
        DestroyWindow(window);
        return 0;
    }
    if (message == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, w, l);
}

static int run_receiver(void) {
    WNDCLASSW wc;
    HWND window;
    MSG message;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = receiver_proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = receiver_class;
    wc.hCursor = LoadCursorW(NULL, MAKEINTRESOURCEW(32512));
    if (!RegisterClassW(&wc)) return 2;
    LoadKeyboardLayoutW(L"00000409", KLF_ACTIVATE);
    window = CreateWindowW(receiver_class, L"Keypour Receiver", WS_OVERLAPPEDWINDOW,
                          50, 50, 530, 330, NULL, NULL, wc.hInstance, NULL);
    if (!window) return 3;
    ShowWindow(window, SW_SHOW);
    UpdateWindow(window);
    SetForegroundWindow(window);
    while (GetMessageW(&message, NULL, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return 0;
}

static void inject_vk(WORD vk, int down) {
    INPUT input;
    memset(&input, 0, sizeof(input));
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = vk;
    input.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
    check(SendInput(1, &input, sizeof(input)) == 1, "test keystroke accepted by Windows");
    pause_pumping(30);
}

static int select_receiver(HWND window, HWND edit) {
    RECT rectangle;
    MSLLHOOKSTRUCT mouse;
    INPUT click[2];
    SetForegroundWindow(window);
    SendMessageW(window, WM_SETFOCUS, 0, 0);
    pause_pumping(80);
    kp_set_active(1);
    GetWindowRect(edit, &rectangle);
    memset(&mouse, 0, sizeof(mouse));
    mouse.pt.x = rectangle.left + 10;
    mouse.pt.y = rectangle.top + 10;
    /* Give the actual EDIT control focus, including closing a possible menu
     * activated by the preceding Alt-hotkey test, just as the user's click does. */
    SetCursorPos(mouse.pt.x, mouse.pt.y);
    memset(click, 0, sizeof(click));
    click[0].type = click[1].type = INPUT_MOUSE;
    click[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    click[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
    SendInput(2, click, sizeof(INPUT));
    pause_pumping(80);
    /* Real SendInput events are injected and must not arm a target. Exercise
     * the trusted/non-injected hook boundary explicitly instead. */
    kp_mouse_proc(HC_ACTION, WM_LBUTTONUP, (LPARAM)&mouse);
    if (!(kp_poll() & KP_EVENT_CLICK)) return 0;
    return kp_capture_target();
}

static void test_clipboard(HWND window) {
    const WCHAR text[] = {L'A', 0x0416, 0xD83D, 0xDE00, L'\r', L'\n', 0};
    const WCHAR invalid[] = {0xD800, 0};
    const WCHAR unterminated[] = {L'x', L'x', L'x', L'x'};
    const char expected[] = "A\xD0\x96\xF0\x9F\x98\x80\r\n";
    char *snapshot;
    int length = -1;
    ULONGLONG started, elapsed;
    check(set_clipboard_units(text, sizeof(text) / sizeof(text[0]), 1), "set Unicode clipboard fixture");
    snapshot = kp_clipboard(1024, &length);
    check(snapshot && length == (int)strlen(expected) && !memcmp(snapshot, expected, strlen(expected)),
          "clipboard preserves Cyrillic, surrogate-pair emoji and CRLF as UTF-8");
    kp_free_text(snapshot, length);
    snapshot = kp_clipboard(3, &length);
    check(!snapshot && length == 0, "clipboard size limit rejects whole snapshot without truncation");
    kp_free_text(snapshot, length);
    check(set_clipboard_units(invalid, 2, 1), "set malformed UTF-16 fixture");
    snapshot = kp_clipboard(1024, &length);
    check(!snapshot, "isolated UTF-16 surrogate is rejected");
    kp_free_text(snapshot, length);
    check(set_clipboard_units(unterminated, 4, 0), "set unterminated clipboard fixture");
    snapshot = kp_clipboard(1024, &length);
    /* Some clipboard providers append a terminator during SetClipboardData. */
    check(!snapshot || (length <= 4 && length >= 0),
          "unterminated fixture is rejected or safely normalized by the clipboard provider");
    kp_free_text(snapshot, length);
    check(set_clipboard_units(L"", 1, 1), "set empty clipboard fixture");
    snapshot = kp_clipboard(1024, &length);
    check(snapshot && length == 0 && snapshot[0] == 0, "empty clipboard snapshot is owned and terminated");
    kp_free_text(snapshot, length);
    SendMessageW(window, WM_APP + 1, 0, 0);
    started = GetTickCount64();
    snapshot = kp_clipboard(1024, &length);
    elapsed = GetTickCount64() - started;
    check(!snapshot && elapsed < 350, "delayed clipboard owner times out without blocking main input loop");
    kp_free_text(snapshot, length);
    pause_pumping(650);
    snapshot = kp_clipboard(1024, &length);
    check(snapshot && length == 7 && !memcmp(snapshot, "delayed", 7), "abandoned clipboard worker cleans up and permits retry");
    kp_free_text(snapshot, length);
}

static void test_hook_boundaries(HWND window, HWND edit) {
    KBDLLHOOKSTRUCT key;
    MSLLHOOKSTRUCT mouse;
    RECT rectangle;
    int result;
    kp_set_active(1);
    memset(&key, 0, sizeof(key));
    key.vkCode = VK_ESCAPE;
    key.flags = LLKHF_INJECTED;
    kp_keyboard_proc(HC_ACTION, WM_KEYDOWN, (LPARAM)&key);
    check(!(kp_poll() & KP_EVENT_CANCEL), "injected Escape is ignored by cancellation hook");
    key.flags = 0;
    result = (int)kp_keyboard_proc(HC_ACTION, WM_KEYDOWN, (LPARAM)&key);
    check(result == 1 && (kp_poll() & KP_EVENT_CANCEL), "physical Escape is consumed and cancels active operation");
    kp_set_active(0);
    result = (int)kp_keyboard_proc(HC_ACTION, WM_KEYUP, (LPARAM)&key);
    check(result == 1, "matching Escape release remains consumed after disarming");
    GetWindowRect(edit, &rectangle);
    memset(&mouse, 0, sizeof(mouse));
    mouse.pt.x = rectangle.left + 10;
    mouse.pt.y = rectangle.top + 10;
    mouse.flags = LLMHF_INJECTED;
    kp_set_active(1);
    kp_mouse_proc(HC_ACTION, WM_LBUTTONUP, (LPARAM)&mouse);
    check(!(kp_poll() & KP_EVENT_CLICK), "injected click cannot select a target");
    check(select_receiver(window, edit), "physical click release selects separate foreground process");
}

static void test_hotkey(void) {
    int events;
    kp_set_active(0);
    kp_poll();
    inject_vk(VK_LCONTROL, 1);
    inject_vk(VK_LMENU, 1);
    inject_vk('J', 1);
    events = kp_poll();
    check((events & KP_EVENT_HOTKEY) != 0, "registered global Ctrl+Alt+J hotkey is delivered");
    check(kp_modifiers_down(), "held trigger modifiers are detected");
    inject_vk('J', 1);
    check(!(kp_poll() & KP_EVENT_HOTKEY), "hotkey autorepeat is suppressed");
    inject_vk('J', 0);
    inject_vk(VK_LMENU, 0);
    inject_vk(VK_LCONTROL, 0);
    check(!kp_modifiers_down(), "trigger modifier releases are detected");
}

static void type_scalars(const uint32_t *text, size_t length) {
    size_t i;
    int can_type = 1, success = 1;
    for (i = 0; i < length; ++i)
        if (!kp_can_type(text[i])) can_type = 0;
    check(can_type, "whole payload preflight accepts expected character mappings");
    if (!can_type) {
        printf("Diagnostic: %s\n", kp_error());
        return;
    }
    for (i = 0; i < length; ++i) {
        kp_poll();
        if (!kp_type(text[i], 3)) {
            success = 0;
            printf("Diagnostic at index %u: %s\n", (unsigned int)i, kp_error());
            break;
        }
        pause_pumping(20);
    }
    check(success, "native key emission accepts every character");
}

static void test_typing(HWND window, HWND edit) {
    const uint32_t text[] = {'A', 'a', '1', '2', '!', '@', '\t', 'Z', '\n', 'q'};
    const uint32_t unicode[] = {0xE9, 0x0416, 0x1F600};
    const uint32_t caps_text[] = {'a', 'A'};
    int original_caps;
    const WCHAR expected_unicode[] = {0xE9, 0x0416, 0xD83D, 0xDE00, 0};
    WCHAR actual[256];
    HWND other;
    check(select_receiver(window, edit), "capture initial typing target");
    check(!kp_can_type(0xD800) && !kp_can_type(0x1B) && !kp_can_type(0x7F),
          "surrogate, Escape and Delete control data fail preflight");
    check(!kp_can_type(0x1F600), "physical-key mode rejects unmappable supplementary character");
    SendMessageW(edit, WM_SETTEXT, 0, (LPARAM)L"");
    type_scalars(text, sizeof(text) / sizeof(text[0]));
    pause_pumping(100);
    memset(actual, 0, sizeof(actual));
    SendMessageW(edit, WM_GETTEXT, sizeof(actual) / sizeof(actual[0]), (LPARAM)actual);
    if (wcscmp(actual, L"Aa12!@\tZ\r\nq")) {
        size_t i;
        printf("Physical receiver UTF-16 code units:");
        for (i = 0; actual[i] && i < 255; ++i) printf(" %04x", (unsigned int)actual[i]);
        printf("\n");
    }
    check(!wcscmp(actual, L"Aa12!@\tZ\r\nq"), "real EDIT control receives exact physical-key text including Enter and Tab");
    check(!kp_modifiers_down(), "no Shift/Ctrl/Alt/Win/button remains held after physical typing");
    original_caps = (GetKeyState(VK_CAPITAL) & 1) != 0;
    inject_vk(VK_CAPITAL, 1);
    inject_vk(VK_CAPITAL, 0);
    check(!kp_target_matches(), "changing Caps Lock invalidates captured typing state");
    check(!kp_type('X', 3), "no character is emitted after a keyboard lock changes");
    if (!(GetKeyState(VK_CAPITAL) & 1)) {
        inject_vk(VK_CAPITAL, 1);
        inject_vk(VK_CAPITAL, 0);
    }
    check(select_receiver(window, edit), "capture a target with Caps Lock already enabled");
    SendMessageW(edit, WM_SETTEXT, 0, (LPARAM)L"");
    type_scalars(caps_text, sizeof(caps_text) / sizeof(caps_text[0]));
    pause_pumping(100);
    memset(actual, 0, sizeof(actual));
    SendMessageW(edit, WM_GETTEXT, sizeof(actual) / sizeof(actual[0]), (LPARAM)actual);
    check(!wcscmp(actual, L"aA"), "physical mapping compensates for existing Caps Lock");
    check((GetKeyState(VK_CAPITAL) & 1) != 0, "typing preserves enabled Caps Lock");
    if (!original_caps) {
        inject_vk(VK_CAPITAL, 1);
        inject_vk(VK_CAPITAL, 0);
    }
    check(((GetKeyState(VK_CAPITAL) & 1) != 0) == original_caps,
          "test restores original Caps Lock setting");
    other = CreateWindowW(L"STATIC", L"Keypour Other Window", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                          620, 100, 220, 200, NULL, NULL, GetModuleHandleW(NULL), NULL);
    SetForegroundWindow(other);
    pause_pumping(100);
    check(!kp_target_matches(), "focus change to another top-level window invalidates captured target");
    check(!kp_type('X', 3), "no further character is emitted after focus loss");
    DestroyWindow(other);
    kp_shutdown();
    check(kp_init(KP_MOD_CTRL | KP_MOD_ALT, "J", 1), "initialize explicit Unicode mode");
    check(select_receiver(window, edit), "recapture Unicode typing target");
    SendMessageW(edit, WM_SETTEXT, 0, (LPARAM)L"");
    type_scalars(unicode, sizeof(unicode) / sizeof(unicode[0]));
    pause_pumping(100);
    memset(actual, 0, sizeof(actual));
    SendMessageW(edit, WM_GETTEXT, sizeof(actual) / sizeof(actual[0]), (LPARAM)actual);
    check(!wcscmp(actual, expected_unicode), "Unicode mode emits BMP and supplementary characters exactly");
    check(!kp_modifiers_down(), "Unicode typing leaves keyboard modifiers released");
}

int main(int argc, char **argv) {
    WCHAR executable[MAX_PATH], command[MAX_PATH + 32];
    STARTUPINFOW startup;
    PROCESS_INFORMATION process;
    HWND window = NULL, edit = NULL;
    ULONGLONG deadline;
    if (argc > 1 && !strcmp(argv[1], "--receiver")) return run_receiver();
    GetModuleFileNameW(NULL, executable, MAX_PATH);
    _snwprintf(command, sizeof(command) / sizeof(command[0]), L"\"%ls\" --receiver", executable);
    command[(sizeof(command) / sizeof(command[0])) - 1] = 0;
    memset(&startup, 0, sizeof(startup));
    memset(&process, 0, sizeof(process));
    startup.cb = sizeof(startup);
    if (!CreateProcessW(NULL, command, NULL, NULL, FALSE, 0, NULL, NULL, &startup, &process)) {
        fprintf(stderr, "Cannot start receiver process: %lu\n", (unsigned long)GetLastError());
        return 2;
    }
    CloseHandle(process.hThread);
    AllowSetForegroundWindow(process.dwProcessId);
    deadline = GetTickCount64() + 5000;
    while (!window && GetTickCount64() < deadline) {
        window = FindWindowW(receiver_class, L"Keypour Receiver");
        Sleep(10);
    }
    if (window) edit = FindWindowExW(window, NULL, L"EDIT", NULL);
    check(window && edit, "separate Win32 receiver process has an editable window");
    if (window && edit) {
        check(kp_init(KP_MOD_CTRL | KP_MOD_ALT, "J", 0), "initialize Windows native backend");
        if (!kp_registered) printf("Diagnostic: %s\n", kp_error());
        if (kp_registered) {
            test_clipboard(window);
            test_hook_boundaries(window, edit);
            test_hotkey();
            test_typing(window, edit);
        }
    }
    kp_shutdown();
    if (window) PostMessageW(window, WM_CLOSE, 0, 0);
    if (WaitForSingleObject(process.hProcess, 2000) != WAIT_OBJECT_0)
        TerminateProcess(process.hProcess, 2);
    CloseHandle(process.hProcess);
    printf("Windows native tests: %d failure(s).\n", failures);
    return failures ? 1 : 0;
}
