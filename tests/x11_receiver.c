#define _POSIX_C_SOURCE 200809L
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <xkbcommon/xkbcommon.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* Test recipient: receives actual events from a separate Keypour process.
   Focus follows clicks, so no window manager is required in private Xvfb. */
int main(int argc, char **argv) {
    if (argc != 4) return 2;
    Display *d = XOpenDisplay(NULL); if (!d) return 3;
    FILE *output = fopen(argv[1], "ab"); if (!output) return 4;
    setvbuf(output, NULL, _IONBF, 0);
    int x = atoi(argv[2]);
    Window w = XCreateSimpleWindow(d, DefaultRootWindow(d), x, 100, 280, 200, 0, 0, 0xffffff);
    XStoreName(d, w, argv[3]);
    XSelectInput(d, w, KeyPressMask | KeyReleaseMask | ButtonPressMask | ExposureMask);
    XMapWindow(d, w); XFlush(d);
    printf("%lu\n", w); fflush(stdout);
    for (;;) {
        XEvent e; XNextEvent(d, &e);
        if (e.type == ButtonPress) {
            XSetInputFocus(d, w, RevertToParent, CurrentTime); XFlush(d);
        } else if (e.type == KeyPress) {
            KeySym sym; char ignored[32];
            XLookupString(&e.xkey, ignored, sizeof(ignored), &sym, NULL);
            if (IsModifierKey(sym)) continue;
            if ((e.xkey.state & (ControlMask | Mod1Mask | Mod4Mask)) != 0) {
                fputs("[MOD]", output); continue;
            }
            if (sym == XK_Escape) { fputs("[ESC]", output); continue; }
            uint32_t cp = xkb_keysym_to_utf32((xkb_keysym_t)sym);
            if (sym == XK_Return) cp = '\n';
            if (sym == XK_Tab) cp = '\t';
            if (cp && cp <= 0x10ffff) {
                char utf8[8]; int size = xkb_keysym_to_utf8(xkb_utf32_to_keysym(cp), utf8, sizeof(utf8));
                if (size > 1) fwrite(utf8, 1, (size_t)size - 1, output);
            }
        }
    }
}
