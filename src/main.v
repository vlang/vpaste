module main

import core
import os
import time

fn main() {
	run() or {
		eprintln('keypour: ${err}')
		exit(1)
	}
}

fn run() ! {
	options := core.parse_args(os.args[1..])!
	if options.command == .help {
		println(core.help_text)
		return
	}
	if options.command == .version {
		println('Keypour ${core.version}')
		return
	}
	path := if options.config_path != '' { options.config_path } else { core.default_config_path()! }
	config := if options.command == .init_config {
		core.apply_overrides(core.Config{}, options)!
	} else {
		core.apply_overrides(core.load_config(path, options.config_path != '')!, options)!
	}
	match options.command {
		.init_config {
			if os.exists(path) { return error('configuration already exists: ${path}') }
			os.mkdir_all(os.dir(path))!
			mut file := os.open_file(path, 'wx', 0o600)!
			defer { file.close() }
			file.writeln(core.config_json(config))!
			println('Created ${path}')
			return
		}
		.print_config {
			println(core.config_json(config))
			return
		}
		.check_config {
			println('Configuration is valid. Hotkey: ${config.hotkey}; mode: ${config.input_mode}.')
			return
		}
		else {}
	}

	hotkey := core.parse_hotkey(config.hotkey)!
	if C.kp_init(hotkey.modifiers, &char(hotkey.key.str), if config.input_mode == 'unicode' {
		1
	} else {
		0
	}) == 0 {
		return error(native_error())
	}
	defer { C.kp_shutdown() }
	println('Keypour ${core.version} | ${config.hotkey} | ${config.input_mode} | ${config.interval_ms} ms + ${config.key_hold_ms} ms hold')
	println('Copy text, press the hotkey, release the keys, and click the console input.')
	println('Escape or the hotkey again cancels; Ctrl+C here exits. Newlines press Enter.')
	run_loop(config)!
}

fn monotonic_ms() i64 {
	return i64(time.sys_mono_now() / 1000000)
}

fn clipboard_payload(config core.Config) ![]rune {
	mut size := i32(0)
	text := C.kp_clipboard(config.max_chars * 4, &size)
	if isnil(text) { return error(native_error()) }
	defer { C.kp_free_text(text, int(size)) }
	view := unsafe { text.vstring_with_len(int(size)) }
	return core.prepare_text(view, config.max_chars, config.trim_trailing_newlines)
}

fn wipe_payload(mut payload []rune) {
	if payload.cap > 0 {
		C.kp_wipe(payload.data, usize(payload.cap) * sizeof(rune))
	}
	payload.clear()
}

fn run_loop(config core.Config) ! {
	mut session := core.Session{}
	mut payload := []rune{}
	defer { wipe_payload(mut payload) }
	mut position := 0
	mut due_ms := i64(0)
	for {
		events := C.kp_poll()
		now := monotonic_ms()
		watching := session.phase in [.settling, .typing]
		modifiers := if watching { C.kp_modifiers_down() != 0 } else { false }
		matches := if watching { C.kp_target_matches() != 0 } else { true }
		action := session.step(events, now, config, modifiers, matches)
		match action {
			.quit {
				return
			}
			.backend_error {
				return error(native_error())
			}
			.arm {
				wipe_payload(mut payload)
				C.kp_set_active(1)
				payload = clipboard_payload(config) or {
					eprintln('Not armed: ${err}')
					session.reset()
					C.kp_set_active(0)
					continue
				}
				position = 0
				println('Armed: ${payload.len} characters. Click the destination within ${config.arm_timeout_ms / 1000} seconds.')
			}
			.capture {
				if C.kp_capture_target() == 0 {
					eprintln('Cancelled: ${native_error()}')
					session.reset()
					C.kp_set_active(0)
					wipe_payload(mut payload)
					continue
				}
				// Preflight each distinct scalar before sending any key. Service
				// native event loops throughout (required by macOS/Windows hooks).
				mut seen := map[rune]bool{}
				mut failed := false
				for ch in payload {
					if ch in seen { continue
					 }
					seen[ch] = true
					if C.kp_can_type(u32(ch)) == 0 {
						eprintln('Cancelled before typing: ${native_error()}')
						failed = true
						break
					}
					check := C.kp_poll()
					if check & core.event_quit != 0 { return }
					if check & core.event_error != 0 { return error(native_error()) }
					if check & (core.event_cancel | core.event_hotkey | core.event_click) != 0
						|| C.kp_target_matches() == 0 {
						eprintln('Cancelled during character validation.')
						failed = true
						break
					}
				}
				// Discard the preflight lookup set; payload buffers are wiped separately.
				seen.clear()
				if failed {
					session.reset()
					C.kp_set_active(0)
					wipe_payload(mut payload)
				}
			}
			.begin_typing {
				println('Typing ${payload.len} characters…')
				due_ms = now
			}
			.cancel, .timed_out, .focus_lost, .interrupted {
				message := match action {
					.timed_out { 'Selection timed out.' }
					.focus_lost { 'Stopped: focused window or keyboard layout changed.' }
					.interrupted { 'Stopped: a modifier key or mouse button was pressed.' }
					else { 'Cancelled.' }
				}

				println('${message} ${position} of ${payload.len} characters sent.')
				C.kp_set_active(0)
				wipe_payload(mut payload)
			}
			.none {}
		}

		if session.phase == .typing && monotonic_ms() >= due_ms {
			// Native send checks focus again immediately before the key down.
			if C.kp_type(u32(payload[position]), config.key_hold_ms) == 0 {
				eprintln('Stopped after ${position} characters: ${native_error()}')
				session.reset()
				C.kp_set_active(0)
				wipe_payload(mut payload)
				continue
			}
			is_newline := payload[position] == `\n`
			position++
			due_ms = monotonic_ms() + config.interval_ms +
				if is_newline { config.line_delay_ms } else { 0 }
			if position == payload.len {
				println('Finished: ${position} characters sent.')
				session.reset()
				C.kp_set_active(0)
				wipe_payload(mut payload)
			}
		}
		time.sleep(5 * time.millisecond)
	}
}
