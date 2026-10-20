module core

fn test_hotkey_parser_and_aliases() {
	assert parse_hotkey('Ctrl+Alt+V')! == Hotkey{
		modifiers: 3
		key:       'V'
	}
	assert parse_hotkey(' command + shift + f8 ')! == Hotkey{
		modifiers: 12
		key:       'F8'
	}
	assert parse_hotkey('Control+Option+9')!.modifiers == 3
	for invalid in ['', 'V', 'Shift+V', 'Ctrl++V', 'Ctrl+Ctrl+V', 'Ctrl+X+V', 'Ctrl+Escape',
		'Ctrl+F13', 'Ctrl'] {
		if _ := parse_hotkey(invalid) { assert false, 'accepted invalid hotkey: ${invalid}'
		 }
	}
}

fn test_configuration_defaults_and_strict_fields() {
	assert decode_config('{}')! == Config{}
	parsed :=
		decode_config('{"hotkey":"Ctrl+Shift+F8","interval_ms":80,"trim_trailing_newlines":true}')!
	assert parsed.interval_ms == 80
	assert parsed.key_hold_ms == 8
	assert parsed.trim_trailing_newlines
	for invalid in ['[]', '{"intervl_ms":1}', '{"interval_ms":-1}', '{"key_hold_ms":0}',
		'{"arm_timeout_ms":999}', '{"input_mode":"paste"}', '{"interval_ms":"35"}',
		'{"interval_ms":1.5}', '{"trim_trailing_newlines":1}', '{"max_chars":0}'] {
		if _ := decode_config(invalid) { assert false, 'accepted invalid configuration: ${invalid}'
		 }
	}
}

fn test_payload_is_literal_normalizes_crlf_and_preserves_unicode() {
	assert prepare_text('a\r\nb\rc\n\t{ENTER}+$', 100, false)! == 'a\nb\nc\n\t{ENTER}+$'.runes()
	assert prepare_text('Привет 🙂', 20, false)! == 'Привет 🙂'.runes()
	assert prepare_text('a\r\n\r\n', 20, true)! == [rune(`a`)]
	assert prepare_text('a\n', 20, false)! == [rune(`a`), `\n`]
	assert prepare_text('éé', 2, false)!.len == 2
}

fn test_payload_rejects_entire_input_on_control_or_invalid_utf8() {
	for bad in ['', 'a\x00b', 'a\x1bb', '\x08', '\x7f', '\xc2\x85', '\xff', '\xc0\x80',
		'\xed\xa0\x80'] {
		if _ := prepare_text(bad, 100, false) { assert false, 'accepted unsupported clipboard bytes'
		 }
	}
	if _ := prepare_text('abc', 2, false) { assert false
	 }
	if _ := prepare_text('\r\n', 10, true) { assert false
	 }
}

fn test_session_click_delay_modifier_release_and_rearm() {
	c := Config{}
	mut s := Session{}
	assert s.step(event_hotkey, 0, c, true, false) == .arm
	assert s.step(0, 10, c, false, false) == .none
	assert s.step(event_click, 100, c, false, false) == .capture
	assert s.step(0, 499, c, false, true) == .none
	assert s.step(0, 500, c, true, true) == .none
	assert s.step(0, 501, c, false, true) == .begin_typing
	assert s.step(0, 520, c, true, true) == .interrupted
	assert s.phase == .idle
	assert s.step(event_hotkey, 600, c, false, false) == .arm
	assert s.step(event_hotkey, 610, c, false, false) == .cancel
}

fn test_session_timeout_and_cancel_win_over_typing() {
	c := Config{}
	mut s := Session{}
	s.step(event_hotkey, 100, c, false, false)
	assert s.step(event_click, 15100, c, false, false) == .timed_out
	s.step(event_hotkey, 20000, c, false, false)
	s.step(event_click, 20001, c, false, false)
	assert s.step(event_cancel, 20401, c, false, true) == .cancel
	s.step(event_hotkey, 30000, c, false, false)
	s.step(event_click, 30001, c, false, false)
	assert s.step(0, 30401, c, false, false) == .focus_lost
	assert s.step(event_hotkey | event_quit, 40000, c, false, true) == .quit
	assert s.phase == .idle
}

fn test_cli_overrides_do_not_change_other_defaults() {
	options := parse_args(['--hotkey=Ctrl+Shift+F8', '--interval-ms', '80',
		'--trim-trailing-newlines', '--config', '/tmp/example.json'])!
	assert options.config_path == '/tmp/example.json'
	c := apply_overrides(Config{}, options)!
	assert c.hotkey == 'Ctrl+Shift+F8'
	assert c.interval_ms == 80
	assert c.key_hold_ms == 8
	assert c.trim_trailing_newlines
	for args in [['--unknown'], ['--interval-ms'], ['--help', '--version']] {
		if _ := parse_args(args) { assert false, 'invalid CLI accepted'
		 }
	}
	bad := parse_args(['--interval-ms', 'fast'])!
	if _ := apply_overrides(Config{}, bad) { assert false
	 }
}

fn test_second_click_and_backend_error_stop_before_next_key() {
	c := Config{}
	mut s := Session{}
	s.step(event_hotkey, 0, c, false, true)
	s.step(event_click, 1, c, false, true)
	assert s.step(event_click, 50, c, false, true) == .cancel
	s.step(event_hotkey, 100, c, false, true)
	s.step(event_click, 101, c, false, true)
	assert s.step(0, 501, c, false, true) == .begin_typing
	assert s.step(event_click, 502, c, false, true) == .cancel
	assert s.step(event_error | event_hotkey, 600, c, false, true) == .backend_error
	assert s.phase == .idle
}
