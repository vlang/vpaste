module core

import strconv

pub enum Command {
	run
	help
	version
	init_config
	print_config
	check_config
}
pub struct Options {
pub mut:
	command     Command
	config_path string
	overrides   map[string]string
}

pub fn parse_args(args []string) !Options {
	mut result := Options{}
	mut index := 0
	mut command_seen := false
	for index < args.len {
		arg := args[index]
		index++
		if arg in ['--help', '-h', '--version', '--init-config', '--print-config', '--check-config'] {
			if command_seen {
				return error('use only one of --help, --version, --init-config, --print-config, --check-config')
			}
			result.command = match arg {
				'--help', '-h' { .help }
				'--version' { .version }
				'--init-config' { .init_config }
				'--print-config' { .print_config }
				else { .check_config }
			}

			command_seen = true
			continue
		}
		if arg == '--trim-trailing-newlines' {
			result.overrides['trim_trailing_newlines'] = 'true'
			continue
		}
		name := if arg.contains('=') { arg.all_before('=') } else { arg }
		if name !in ['--config', '--hotkey', '--interval-ms', '--hold-ms', '--start-delay-ms',
			'--line-delay-ms', '--arm-timeout-ms', '--max-chars', '--mode'] {
			return error('unknown argument: ${name}; use --help')
		}
		value := if arg.contains('=') {
			arg.all_after('=')
		} else {
			if index >= args.len { return error('${name} requires a value') }
			next := args[index]
			index++
			next
		}
		if value == '' { return error('${name} requires a nonempty value') }
		if name == '--config' {
			result.config_path = value
		} else {
			result.overrides[name] = value
		}
	}
	return result
}

pub fn apply_overrides(config Config, options Options) !Config {
	mut c := config
	for key, value in options.overrides {
		match key {
			'--hotkey' {
				c.hotkey = value
			}
			'--mode' {
				c.input_mode = value
			}
			'trim_trailing_newlines' {
				c.trim_trailing_newlines = true
			}
			else {
				n := strconv.atoi(value) or { return error('${key} requires a whole number') }
				match key {
					'--interval-ms' { c.interval_ms = n }
					'--hold-ms' { c.key_hold_ms = n }
					'--start-delay-ms' { c.start_delay_ms = n }
					'--line-delay-ms' { c.line_delay_ms = n }
					'--arm-timeout-ms' { c.arm_timeout_ms = n }
					'--max-chars' { c.max_chars = n }
					else { return error('unsupported option: ${key}') }
				}
			}
		}
	}
	c.validate()!
	return c
}

pub const help_text = 'Keypour — clipboard text as keystrokes for VNC/IPMI consoles

Usage: keypour [options]

Copy text, press Ctrl+Alt+V, release the keys, then click the console input.
Escape or the hotkey again cancels. Ctrl+C in this terminal exits.

  --hotkey Ctrl+Alt+V          Global hotkey (A-Z, 0-9, F1-F12)
  --interval-ms 35             Pause after each character (plus key hold)
  --hold-ms 8                  Duration of each key press, 1..100 ms
  --start-delay-ms 400         Delay after the selecting click
  --line-delay-ms 150          Extra pause after each newline
  --arm-timeout-ms 15000       Time allowed to select a target
  --max-chars 100000           Maximum clipboard character count
  --mode keys|unicode         Physical keys by default; Unicode is opt-in
  --trim-trailing-newlines     Omit final newlines (default: preserve)
  --config PATH               Read a specific JSON configuration
  --init-config               Write defaults/overrides; never overwrite
  --print-config              Print the effective configuration and exit
  --check-config              Validate settings without opening desktop APIs
  --version                   Print version and exit
  --help                      Show this help

Hotkey modifiers: Ctrl, Alt/Option, Shift, Super/Win/Cmd.
At least one of Ctrl, Alt, Super is required. Restart to apply file changes.
Newlines send Enter; tabs send Tab. Braces and other text are literal.
Linux requires X11. macOS requires Accessibility and Input Monitoring.
The remote console and local keyboard layouts should match in keys mode.'
