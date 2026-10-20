module core

pub const event_hotkey = 1
pub const event_click = 2
pub const event_cancel = 4
pub const event_quit = 8
pub const event_error = 16

pub enum Phase {
	idle
	armed
	settling
	typing
}
pub enum Action {
	none
	arm
	capture
	begin_typing
	cancel
	timed_out
	focus_lost
	interrupted
	quit
	backend_error
}

pub struct Session {
pub mut:
	phase       Phase
	deadline_ms i64
	ready_at_ms i64
}

pub fn (mut s Session) reset() {
	s.phase = .idle
	s.deadline_ms = 0
	s.ready_at_ms = 0
}

// All timing uses a monotonic clock supplied by the caller. Keeping this policy
// separate from native APIs makes cancellation ordering testable without a GUI.
pub fn (mut s Session) step(events int, now_ms i64, c Config, modifiers_down bool, focus_matches bool) Action {
	if events & event_quit != 0 {
		s.reset()
		return .quit
	}
	if events & event_error != 0 {
		s.reset()
		return .backend_error
	}
	if s.phase != .idle && events & (event_cancel | event_hotkey) != 0 {
		s.reset()
		return .cancel
	}
	if s.phase == .idle {
		if events & event_hotkey != 0 {
			s.phase = .armed
			s.deadline_ms = now_ms + c.arm_timeout_ms
			return .arm
		}
		return .none
	}
	if s.phase in [.armed, .settling] && now_ms >= s.deadline_ms {
		s.reset()
		return .timed_out
	}
	if s.phase == .armed {
		if events & event_click != 0 {
			s.phase = .settling
			s.ready_at_ms = now_ms + c.start_delay_ms
			return .capture
		}
		return .none
	}
	if !focus_matches {
		s.reset()
		return .focus_lost
	}
	if events & event_click != 0 {
		s.reset()
		return .cancel
	}
	if s.phase == .settling {
		if now_ms >= s.ready_at_ms && !modifiers_down {
			s.phase = .typing
			return .begin_typing
		}
	} else if modifiers_down {
		s.reset()
		return .interrupted
	}
	return .none
}
