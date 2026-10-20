module core

import encoding.utf8

// Literal clipboard text: no KeePass placeholders or command-language expansion.
// Validate before allocating a payload, so rejected text never leaves a partial
// rune buffer behind. The caller owns and wipes the successful result.
pub fn prepare_text(text string, max_chars int, trim_trailing_newlines bool) ![]rune {
	if text.len == 0 { return error('clipboard contains no text') }
	if max_chars < 1 { return error('max_chars must be positive') }
	if !utf8.validate_str(text) { return error('clipboard is not valid UTF-8') }
	mut count := 0
	mut trailing_newlines := 0
	mut after_cr := false
	mut offset := 0
	for offset < text.len {
		ch := utf8.get_rune(text, offset)
		offset += utf8_char_len(ch)
		if ch == `\n` && after_cr {
			after_cr = false
			continue
		}
		after_cr = ch == `\r`
		normalized := if after_cr { rune(`\n`) } else { ch }
		if (normalized < 32 && normalized !in [`\n`, `\t`])
			|| (normalized >= 0x7f && normalized <= 0x9f) {
			return error('clipboard contains an unsupported control character')
		}
		count++
		if count > max_chars { return error('clipboard exceeds max_chars') }
		trailing_newlines = if normalized == `\n` { trailing_newlines + 1 } else { 0 }
	}
	if trim_trailing_newlines { count -= trailing_newlines }
	if count == 0 { return error('clipboard contains no text after normalization') }
	mut chars := []rune{len: count}
	after_cr = false
	offset = 0
	mut position := 0
	for position < count {
		ch := utf8.get_rune(text, offset)
		offset += utf8_char_len(ch)
		if ch == `\n` && after_cr {
			after_cr = false
			continue
		}
		after_cr = ch == `\r`
		chars[position] = if after_cr { rune(`\n`) } else { ch }
		position++
	}
	return chars
}

fn utf8_char_len(ch rune) int {
	if ch < 0x80 { return 1 }
	if ch < 0x800 { return 2 }
	if ch < 0x10000 { return 3 }
	return 4
}
