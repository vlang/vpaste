module core

import json
import os
import x.json2

pub const version = '0.1.0'

pub struct Config {
pub mut:
	hotkey                 string = 'Ctrl+Alt+V'
	interval_ms            int    = 35
	key_hold_ms            int    = 8
	start_delay_ms         int    = 400
	line_delay_ms          int    = 150
	arm_timeout_ms         int    = 15000
	max_chars              int    = 100000
	input_mode             string = 'keys'
	trim_trailing_newlines bool
}

pub fn (c Config) validate() ! {
	parse_hotkey(c.hotkey)!
	if c.interval_ms < 0 || c.interval_ms > 10000 { return error('interval_ms must be 0..10000') }
	if c.key_hold_ms < 1 || c.key_hold_ms > 100 { return error('key_hold_ms must be 1..100') }
	if c.start_delay_ms < 100 || c.start_delay_ms > 10000 {
		return error('start_delay_ms must be 100..10000')
	}
	if c.line_delay_ms < 0 || c.line_delay_ms > 30000 {
		return error('line_delay_ms must be 0..30000')
	}
	if c.arm_timeout_ms < 1000 || c.arm_timeout_ms > 300000 {
		return error('arm_timeout_ms must be 1000..300000')
	}
	if c.max_chars < 1 || c.max_chars > 1000000 { return error('max_chars must be 1..1000000') }
	if c.input_mode !in ['keys', 'unicode'] { return error('input_mode must be keys or unicode') }
}

pub fn decode_config(text string) !Config {
	// Catch misspelled settings instead of silently using a different speed/hotkey.
	parsed := json2.decode[json2.Any](text, strict: true)!
	if parsed !is map[string]json2.Any { return error('configuration must be a JSON object') }
	known := ['hotkey', 'interval_ms', 'key_hold_ms', 'start_delay_ms', 'line_delay_ms',
		'arm_timeout_ms', 'max_chars', 'input_mode', 'trim_trailing_newlines']
	fields := parsed as map[string]json2.Any
	for key, value in fields {
		if key !in known { return error('unknown configuration field: ${key}') }
		if key in ['hotkey', 'input_mode'] {
			if value !is string { return error('${key} must be a string') }
		} else if key == 'trim_trailing_newlines' {
			if value !is bool { return error('${key} must be a boolean') }
		} else {
			number := match value {
				int { f64(value) }
				i64 { f64(value) }
				u64 { f64(value) }
				f64 { value }
				else { return error('${key} must be an integer') }
			}

			if number < 0 || number > 1000000 || number != f64(int(number)) {
				return error('${key} must be an integer in 0..1000000')
			}
		}
	}
	config := json.decode(Config, text)!
	config.validate()!
	return config
}

pub fn default_config_path() !string {
	return os.join_path(os.config_dir()!, 'keypour', 'config.json')
}

pub fn load_config(path string, explicit bool) !Config {
	if !os.exists(path) {
		if explicit { return error('config file does not exist: ${path}') }
		return Config{}
	}
	if os.file_size(path) > 65536 { return error('config file exceeds 64 KiB') }
	return decode_config(os.read_file(path)!)
}

pub fn config_json(c Config) string {
	return json.encode_pretty(c)
}
