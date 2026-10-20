module main

#flag -I @VMODROOT/native
#flag @VMODROOT/build/native.o
#include "keypour.h"

$if linux {
	#flag -lX11
	#flag -lXtst
	#flag -lXi
	#flag -lxkbcommon
} $else $if windows {
	#flag -luser32
} $else $if macos {
	#flag -framework AppKit
	#flag -framework ApplicationServices
	#flag -framework Carbon
} $else {
	$compile_error('Keypour supports Windows, Linux X11, and macOS only')
}

fn C.kp_init(modifiers int, const_key &char, mode int) int
fn C.kp_shutdown()
fn C.kp_poll() int
fn C.kp_set_active(active int)
fn C.kp_clipboard(max_bytes int, out_len &i32) &char
fn C.kp_free_text(text &char, len int)
fn C.kp_capture_target() int
fn C.kp_target_matches() int
fn C.kp_modifiers_down() int
fn C.kp_can_type(scalar u32) int
fn C.kp_type(scalar u32, hold_ms int) int
fn C.kp_error() &char
fn C.kp_wipe(data voidptr, len usize)

fn native_error() string {
	return unsafe { cstring_to_vstring(C.kp_error()) }
}
