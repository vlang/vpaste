module core

pub const mod_ctrl = 1
pub const mod_alt = 2
pub const mod_shift = 4
pub const mod_super = 8

pub struct Hotkey {
pub:
	modifiers int
	key       string
}

// Restrict hotkeys to combinations that every backend can register reliably.
pub fn parse_hotkey(value string) !Hotkey {
	mut modifiers := 0
	mut key := ''
	for raw in value.split('+') {
		part := raw.trim_space().to_upper()
		if part == '' { return error('hotkey contains an empty component') }
		bit := match part {
			'CTRL', 'CONTROL' { mod_ctrl }
			'ALT', 'OPTION' { mod_alt }
			'SHIFT' { mod_shift }
			'SUPER', 'WIN', 'CMD', 'COMMAND' { mod_super }
			else { 0 }
		}

		if bit != 0 {
			if modifiers & bit != 0 { return error('hotkey repeats a modifier') }
			modifiers |= bit
			continue
		}
		if key != '' { return error('hotkey must contain exactly one non-modifier key') }
		valid_letter := part.len == 1 && ((part[0] >= `A` && part[0] <= `Z`)
			|| (part[0] >= `0` && part[0] <= `9`))
		valid_function := part in ['F1', 'F2', 'F3', 'F4', 'F5', 'F6', 'F7', 'F8', 'F9', 'F10',
			'F11', 'F12']
		if !valid_letter && !valid_function {
			return error('hotkey key must be A-Z, 0-9, or F1-F12')
		}
		key = part
	}
	if key == '' { return error('hotkey has no key') }
	if modifiers & (mod_ctrl | mod_alt | mod_super) == 0 {
		return error('hotkey needs Ctrl, Alt/Option, or Super/Win/Cmd')
	}
	return Hotkey{
		modifiers: modifiers
		key:       key
	}
}
