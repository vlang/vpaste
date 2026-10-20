#define _POSIX_C_SOURCE 200809L
/* Deterministic X11 selection protocol integration test.
 * Compile with the real native adapter and run inside an isolated Xvfb server.
 * The child is a custom selection owner: even small payloads use ICCCM INCR,
 * so coverage does not depend on xclip's transfer-size heuristics. */
#include "keypour.h"
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define PAYLOAD_SIZE (128 * 1024 + 197)

enum test_case {
    CASE_STOP,
    CASE_UTF8,
    CASE_HEADER_TOO_LARGE,
    CASE_STREAM_TOO_LARGE,
    CASE_DELAY_NOTIFY,
    CASE_DELAY_CHUNK,
    CASE_RECOVER,
    CASE_WRONG_TYPE,
    CASE_LATIN1,
    CASE_ESCAPE_CONFLICT
};

static const unsigned char pattern[] = "Keypour\t\"{}\" \xd0\x9f\xd1\x80\xd0\xb8\xd0\xb2\xd0\xb5\xd1\x82 \xc3\xa9 \xf0\x9f\x99\x82\r\n";
static const unsigned char fresh_text[] = "fresh-after-timeout: \xd0\x94\xd0\xb0 \xf0\x9f\x99\x82\n";
static const unsigned char stale_text[] = "STALE-REPLY-MUST-NOT-ESCAPE";
static const unsigned char latin1_text[] = {'c', 'a', 'f', 0xe9, ' ', 0xa3, '\n'};
static const unsigned char latin1_utf8[] = "caf\xc3\xa9 \xc2\xa3\n";
static int unexpected_x_errors;
static int vanished_requestors;

struct owner {
    Display *display;
    Window window;
    Atom clipboard, utf8, incr;
    int command;
    const unsigned char *large;
    const unsigned char *data;
    size_t length, offset, chunk_index;
    Atom data_type;
    XSelectionRequestEvent request;
    XSelectionRequestEvent delayed;
    int active, has_delayed, stalled;
    int complete_transfers, late_attempts;
};

static int write_all(int fd, const void *bytes, size_t count) {
    const unsigned char *at = (const unsigned char *)bytes;
    while (count) {
        ssize_t sent = write(fd, at, count);
        if (sent < 0 && errno == EINTR) continue;
        if (sent <= 0) return 0;
        at += sent; count -= (size_t)sent;
    }
    return 1;
}

static int read_all(int fd, void *bytes, size_t count) {
    unsigned char *at = (unsigned char *)bytes;
    while (count) {
        ssize_t received = read(fd, at, count);
        if (received < 0 && errno == EINTR) continue;
        if (received <= 0) return 0;
        at += received; count -= (size_t)received;
    }
    return 1;
}

static int owner_error(Display *display, XErrorEvent *event) {
    (void)display;
    /* A requestor may disappear while an INCR chunk is in flight. This is
       expected for rejected/expired transfers and intentional stale replies. */
    if (event->error_code == BadWindow) ++vanished_requestors;
    else {
        ++unexpected_x_errors;
        fprintf(stderr, "selection owner: unexpected X error %u\n", event->error_code);
    }
    return 0;
}

static void notify_request(struct owner *o, const XSelectionRequestEvent *request, Atom property) {
    XEvent event;
    memset(&event, 0, sizeof(event));
    event.xselection.type = SelectionNotify;
    event.xselection.display = o->display;
    event.xselection.requestor = request->requestor;
    event.xselection.selection = request->selection;
    event.xselection.target = request->target;
    event.xselection.property = property;
    event.xselection.time = request->time;
    XSendEvent(o->display, request->requestor, False, NoEventMask, &event);
    XFlush(o->display);
}

static void start_incr(struct owner *o, const XSelectionRequestEvent *request,
                       const unsigned char *data, size_t length, Atom type,
                       unsigned long advertised) {
    o->request = *request;
    o->data = data; o->length = length; o->offset = 0; o->chunk_index = 0;
    o->data_type = type; o->active = 1; o->stalled = 0;
    XSelectInput(o->display, request->requestor, PropertyChangeMask | StructureNotifyMask);
    XChangeProperty(o->display, request->requestor, request->property, o->incr,
        32, PropModeReplace, (const unsigned char *)&advertised, 1);
    notify_request(o, request, request->property);
}

static void send_next_chunk(struct owner *o) {
    if (!o->active || o->stalled) return;
    if (o->command == CASE_DELAY_CHUNK && o->offset) {
        o->stalled = 1;
        return;
    }
    /* Odd chunk sizes deliberately split UTF-8 code points across properties. */
    static const size_t chunk_sizes[] = {7, 1021, 4093, 16001};
    size_t count = chunk_sizes[o->chunk_index++ % 4];
    if (count > o->length - o->offset) count = o->length - o->offset;
    XChangeProperty(o->display, o->request.requestor, o->request.property,
        o->data_type, 8, PropModeReplace, o->data + o->offset, (int)count);
    XFlush(o->display);
    o->offset += count;
    if (!count) {
        o->active = 0;
        ++o->complete_transfers;
    }
}

static int send_delayed_reply(struct owner *o, const XSelectionRequestEvent *current) {
    if (!o->has_delayed) { fprintf(stderr, "recovery case has no delayed transfer\n"); return 0; }
    if (o->delayed.requestor == current->requestor) {
        fprintf(stderr, "clipboard conversion reused a timed-out request window\n");
        return 0;
    }
    int before = vanished_requestors;
    XChangeProperty(o->display, o->delayed.requestor, o->delayed.property,
        o->utf8, 8, PropModeReplace, stale_text, (int)sizeof(stale_text) - 1);
    notify_request(o, &o->delayed, o->delayed.property);
    XSync(o->display, False);
    if (vanished_requestors <= before) {
        fprintf(stderr, "the timed-out request window was not destroyed\n");
        return 0;
    }
    ++o->late_attempts;
    o->has_delayed = 0;
    return 1;
}

static int serve_request(struct owner *o, const XSelectionRequestEvent *request) {
    if (request->selection != o->clipboard || request->property == None) {
        notify_request(o, request, None);
        return 1;
    }
    if (o->command == CASE_LATIN1 && request->target == o->utf8) {
        notify_request(o, request, None); /* Exercise the documented STRING fallback. */
        return 1;
    }
    Atom supported = o->command == CASE_LATIN1 ? XA_STRING : o->utf8;
    if (request->target != supported) { notify_request(o, request, None); return 1; }
    switch (o->command) {
        case CASE_UTF8:
        case CASE_ESCAPE_CONFLICT:
            start_incr(o, request, o->large, PAYLOAD_SIZE, o->utf8, PAYLOAD_SIZE);
            break;
        case CASE_HEADER_TOO_LARGE:
            start_incr(o, request, o->large, PAYLOAD_SIZE, o->utf8, PAYLOAD_SIZE);
            break;
        case CASE_STREAM_TOO_LARGE:
            /* Zero is a valid lower bound; the actual stream still needs a cap. */
            start_incr(o, request, o->large, PAYLOAD_SIZE, o->utf8, 0);
            break;
        case CASE_DELAY_NOTIFY:
            o->delayed = *request; o->has_delayed = 1;
            XSelectInput(o->display, request->requestor, StructureNotifyMask);
            XFlush(o->display);
            break;
        case CASE_DELAY_CHUNK:
            o->delayed = *request; o->has_delayed = 1;
            start_incr(o, request, o->large, PAYLOAD_SIZE, o->utf8, 0);
            break;
        case CASE_RECOVER:
            if (!send_delayed_reply(o, request)) return 0;
            start_incr(o, request, fresh_text, sizeof(fresh_text) - 1, o->utf8, sizeof(fresh_text) - 1);
            break;
        case CASE_WRONG_TYPE:
            XChangeProperty(o->display, request->requestor, request->property,
                XA_STRING, 8, PropModeReplace, latin1_text, sizeof(latin1_text));
            notify_request(o, request, request->property);
            break;
        case CASE_LATIN1:
            start_incr(o, request, latin1_text, sizeof(latin1_text), XA_STRING, sizeof(latin1_text));
            break;
        default:
            fprintf(stderr, "selection request arrived without an active test case\n");
            return 0;
    }
    return 1;
}

static int run_owner(int commands, int replies, const unsigned char *large) {
    alarm(25);
    struct owner o;
    memset(&o, 0, sizeof(o));
    o.large = large;
    o.display = XOpenDisplay(NULL);
    if (!o.display) { fprintf(stderr, "selection owner cannot open DISPLAY\n"); return 1; }
    XSetErrorHandler(owner_error);
    o.window = XCreateSimpleWindow(o.display, DefaultRootWindow(o.display), 0, 0, 1, 1, 0, 0, 0);
    o.clipboard = XInternAtom(o.display, "CLIPBOARD", False);
    o.utf8 = XInternAtom(o.display, "UTF8_STRING", False);
    o.incr = XInternAtom(o.display, "INCR", False);
    /* Cinnamon reserves Ctrl+Alt+Escape to restart the desktop. Keypour must
       still be able to arm and consume plain Escape on the same X server. */
    KeyCode escape = XKeysymToKeycode(o.display, XK_Escape);
    XGrabKey(o.display, escape, ControlMask | Mod1Mask, DefaultRootWindow(o.display),
        False, GrabModeAsync, GrabModeAsync);
    XSetSelectionOwner(o.display, o.clipboard, o.window, CurrentTime);
    XSync(o.display, False);
    if (XGetSelectionOwner(o.display, o.clipboard) != o.window || !write_all(replies, "R", 1)) {
        XCloseDisplay(o.display); return 1;
    }
    int result = 0;
    for (;;) {
        struct pollfd inputs[2] = {{commands, POLLIN, 0}, {ConnectionNumber(o.display), POLLIN, 0}};
        int ready = poll(inputs, 2, XPending(o.display) ? 0 : 100);
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0) { result = 1; break; }
        if (inputs[0].revents & (POLLIN | POLLHUP)) {
            int command;
            if (!read_all(commands, &command, sizeof(command))) { result = 1; break; }
            if (command == CASE_STOP) {
                if (o.complete_transfers < 5 || o.late_attempts != 2 || unexpected_x_errors) {
                    fprintf(stderr, "owner coverage failed: complete=%d late=%d errors=%d\n",
                        o.complete_transfers, o.late_attempts, unexpected_x_errors);
                    result = 1;
                }
                break;
            }
            o.command = command; o.active = 0; o.stalled = 0;
            if (command == CASE_ESCAPE_CONFLICT) {
                XGrabKey(o.display, escape, 0, DefaultRootWindow(o.display),
                    False, GrabModeAsync, GrabModeAsync);
                XSync(o.display, False);
            }
            if (!write_all(replies, "A", 1)) { result = 1; break; }
        }
        while (XPending(o.display)) {
            XEvent event;
            XNextEvent(o.display, &event);
            if (event.type == SelectionRequest) {
                if (!serve_request(&o, &event.xselectionrequest)) { result = 1; goto done; }
            } else if (event.type == PropertyNotify && o.active
                       && event.xproperty.window == o.request.requestor
                       && event.xproperty.atom == o.request.property
                       && event.xproperty.state == PropertyDelete) {
                send_next_chunk(&o);
            } else if (event.type == DestroyNotify && event.xdestroywindow.window == o.request.requestor) {
                o.active = 0;
            }
        }
        if (unexpected_x_errors) { result = 1; break; }
    }
done:
    XDestroyWindow(o.display, o.window);
    XCloseDisplay(o.display);
    return result;
}

static int select_case(int commands, int replies, int which) {
    char ack = 0;
    return write_all(commands, &which, sizeof(which)) && read_all(replies, &ack, 1) && ack == 'A';
}

static int expect_text(const unsigned char *expected, size_t length, int max_bytes, const char *name) {
    int count = -1;
    char *text = kp_clipboard(max_bytes, &count);
    if (!text) { fprintf(stderr, "FAIL %s: %s\n", name, kp_error()); return 0; }
    int same = count == (int)length && !memcmp(text, expected, length) && text[length] == '\0';
    kp_free_text(text, count);
    if (!same) { fprintf(stderr, "FAIL %s: byte-for-byte payload mismatch\n", name); return 0; }
    printf("PASS %s\n", name);
    return 1;
}

static int expect_rejection(int max_bytes, const char *reason, const char *name) {
    int count = -1;
    char *text = kp_clipboard(max_bytes, &count);
    if (text) {
        kp_free_text(text, count);
        fprintf(stderr, "FAIL %s: transfer unexpectedly succeeded\n", name);
        return 0;
    }
    if (count != 0 || !strstr(kp_error(), reason)) {
        fprintf(stderr, "FAIL %s: unexpected rejection: %s\n", name, kp_error());
        return 0;
    }
    printf("PASS %s\n", name);
    return 1;
}

int main(void) {
    alarm(30);
    unsigned char *large = (unsigned char *)malloc(PAYLOAD_SIZE);
    if (!large) return 1;
    /* End with complete code points while retaining a non-round byte length. */
    size_t pattern_length = sizeof(pattern) - 1;
    size_t full = PAYLOAD_SIZE / pattern_length;
    for (size_t i = 0; i < full; ++i) memcpy(large + i * pattern_length, pattern, pattern_length);
    memset(large + full * pattern_length, 'x', PAYLOAD_SIZE - full * pattern_length);
    int commands[2], replies[2];
    if (pipe(commands) || pipe(replies)) { perror("pipe"); free(large); return 1; }
    pid_t child = fork();
    if (child < 0) { perror("fork"); free(large); return 1; }
    if (!child) {
        close(commands[1]); close(replies[0]);
        int status = run_owner(commands[0], replies[1], large);
        close(commands[0]); close(replies[1]); free(large);
        _exit(status);
    }
    close(commands[0]); close(replies[1]);
    int initialized = 0, passed = 0;
    char ready = 0;
    if (!read_all(replies[0], &ready, 1) || ready != 'R') goto cleanup;
    if (!kp_init(KP_MOD_CTRL | KP_MOD_ALT, "F12", 0)) {
        fprintf(stderr, "native initialization: %s\n", kp_error()); goto cleanup;
    }
    initialized = 1;
#define SELECT(which) do { if (!select_case(commands[1], replies[0], (which))) goto cleanup; } while (0)
#define EXPECT(check) do { if (!(check)) goto cleanup; } while (0)
    SELECT(CASE_UTF8);
    kp_set_active(1);
    EXPECT(expect_text(large, PAYLOAD_SIZE, PAYLOAD_SIZE,
        "arming tolerates the desktop's Ctrl+Alt+Escape shortcut"));
    EXPECT(!(kp_poll() & KP_EVENT_ERROR));
    kp_set_active(0);
    SELECT(CASE_UTF8);
    EXPECT(expect_text(large, PAYLOAD_SIZE, PAYLOAD_SIZE + 64, "forced INCR reassembles split UTF-8 exactly"));
    SELECT(CASE_UTF8);
    EXPECT(expect_text(large, PAYLOAD_SIZE, PAYLOAD_SIZE, "INCR payload exactly at the byte limit"));
    SELECT(CASE_HEADER_TOO_LARGE);
    EXPECT(expect_rejection(64, "size limit", "oversized INCR lower bound rejected"));
    SELECT(CASE_STREAM_TOO_LARGE);
    EXPECT(expect_rejection(64, "size limit", "underreported INCR bounded during streaming"));
    SELECT(CASE_DELAY_NOTIFY);
    EXPECT(expect_rejection(PAYLOAD_SIZE, "1.5 seconds", "late SelectionNotify expires"));
    SELECT(CASE_RECOVER);
    EXPECT(expect_text(fresh_text, sizeof(fresh_text) - 1, 1024, "late SelectionNotify cannot pollute next conversion"));
    SELECT(CASE_DELAY_CHUNK);
    EXPECT(expect_rejection(PAYLOAD_SIZE, "1.5 seconds", "partially received INCR expires"));
    SELECT(CASE_RECOVER);
    EXPECT(expect_text(fresh_text, sizeof(fresh_text) - 1, 1024, "late INCR chunk cannot pollute next conversion"));
    SELECT(CASE_WRONG_TYPE);
    EXPECT(expect_rejection(1024, "unsupported text format", "mismatched STRING response to UTF8 request rejected"));
    SELECT(CASE_LATIN1);
    EXPECT(expect_text(latin1_utf8, sizeof(latin1_utf8) - 1, 1024, "INCR STRING fallback converts Latin-1 to UTF-8"));
    SELECT(CASE_ESCAPE_CONFLICT);
    kp_set_active(1);
    EXPECT(expect_rejection(PAYLOAD_SIZE, "Escape is already reserved",
        "a plain Escape conflict keeps its diagnostic during clipboard capture"));
    EXPECT(kp_poll() & KP_EVENT_ERROR);
    kp_set_active(0);
    passed = 1;
cleanup:
    if (initialized) kp_shutdown();
    if (passed) {
        int stop = CASE_STOP;
        if (!write_all(commands[1], &stop, sizeof(stop))) passed = 0;
    } else kill(child, SIGTERM);
    close(commands[1]); close(replies[0]);
    int child_status = 0;
    while (waitpid(child, &child_status, 0) < 0) {
        if (errno != EINTR) { passed = 0; break; }
    }
    if (!WIFEXITED(child_status) || WEXITSTATUS(child_status) != 0) passed = 0;
    free(large);
    if (passed) puts("12 deterministic X11 clipboard/arming checks passed.");
    return passed ? 0 : 1;
}
