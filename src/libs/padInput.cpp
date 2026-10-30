#include "libs/padInput.h"

#include <algorithm>
#include <cstdlib>

namespace Libs::Controller {

namespace {

constexpr uint32_t TRIGGER_BUTTONS = PAD_BUTTON_L2 | PAD_BUTTON_R2;

int Tilt(int value) {
	return std::abs(value - 128);
}

int StickTilt(const std::array<int, PAD_AXES>& axes, Axis x) {
	const auto index = static_cast<int>(x);
	return std::max(Tilt(axes[index]), Tilt(axes[index + 1]));
}

bool SameInput(const PadState& a, const PadState& b) {
	const auto same_touch = [](const PadState::Touch& first, const PadState::Touch& second) {
		return first.id == second.id && first.down == second.down && first.x == second.x &&
		       first.y == second.y;
	};
	return a.buttons == b.buttons && a.axes == b.axes && same_touch(a.touch[0], b.touch[0]) &&
	       same_touch(a.touch[1], b.touch[1]) && a.motion == b.motion;
}

} // namespace

PadInput::Source* PadInput::Find(int id) {
	const auto it = std::find_if(m_sources.begin(), m_sources.end(),
	                             [id](const Source& source) { return source.id == id; });
	return it != m_sources.end() ? &*it : nullptr;
}

std::vector<int> PadInput::SourceIds() const {
	std::vector<int> ids;
	ids.reserve(m_sources.size());
	for (const auto& source: m_sources) {
		ids.push_back(source.id);
	}
	return ids;
}

bool PadInput::Connect(int id) {
	if (Find(id) != nullptr) {
		return false;
	}
	if (m_sources.empty()) {
		m_connected_count++;
	}
	m_sources.push_back({.id = id});
	ChooseActive();
	Update(m_latest.time);
	return true;
}

void PadInput::Disconnect(int id) {
	const auto it = std::find_if(m_sources.begin(), m_sources.end(),
	                             [id](const Source& source) { return source.id == id; });
	// An emergency shutdown calls DisconnectAll (controller.cpp), and the window thread can still
	// deliver a gamepad's removal after it: the id is then already gone.
	if (it == m_sources.end()) {
		return;
	}
	m_sources.erase(it);
	ChooseActive();
	Update(m_latest.time);
}

void PadInput::DisconnectAll() {
	m_sources.clear();
	ChooseActive();
	Update(m_latest.time);
}

void PadInput::Claim(Source& source) {
	if (source.id != HOST_INPUT_CONTROLLER_ID) {
		source.claimed = ++m_claims;
		ChooseActive();
	}
}

void PadInput::ChooseActive() {
	const Source* active = nullptr;
	for (const auto& source: m_sources) {
		if (source.id == HOST_INPUT_CONTROLLER_ID) {
			continue;
		}
		// The first connected gamepad until one claims; then the last that claimed.
		if (active == nullptr || source.claimed > active->claimed) {
			active = &source;
		}
	}
	int id = active != nullptr ? active->id : -1;
	if (id == -1 && !m_sources.empty()) {
		id = HOST_INPUT_CONTROLLER_ID;
	}
	if (id != m_active_id) {
		// The sensors of the new gamepad start from rest.
		m_active_id = id;
		m_motion    = {};
	}
}

void PadInput::SetButton(int id, uint32_t button, bool down, uint64_t time) {
	auto* source = Find(id);
	button &= ~TRIGGER_BUTTONS;
	if (source == nullptr || button == 0) {
		return;
	}
	source->buttons = down ? source->buttons | button : source->buttons & ~button;
	if (down) {
		Claim(*source);
	}
	Update(time);
}

void PadInput::SetAxis(int id, Axis axis, int value, uint64_t time) {
	auto*      source = Find(id);
	const auto index  = static_cast<int>(axis);
	if (source == nullptr || index < 0 || index >= PAD_AXES) {
		return;
	}
	source->axes[index] = value;
	const bool trigger  = axis == Axis::TriggerLeft || axis == Axis::TriggerRight;
	if (trigger ? value >= TRIGGER_CLAIM : Tilt(value) >= STICK_CLAIM) {
		Claim(*source);
	}
	Update(time);
}

void PadInput::SetRightStick(int id, int x, int y, uint64_t time) {
	auto* source = Find(id);
	if (source == nullptr) {
		return;
	}
	source->axes[static_cast<int>(Axis::RightX)] = x;
	source->axes[static_cast<int>(Axis::RightY)] = y;
	if (std::max(Tilt(x), Tilt(y)) >= STICK_CLAIM) {
		Claim(*source);
	}
	Update(time);
}

void PadInput::SetTouch(int id, int finger, bool down, uint16_t x, uint16_t y, uint64_t time) {
	auto* source = Find(id);
	if (source == nullptr || finger < 0 || finger >= 2) {
		return;
	}
	auto& touch = source->touch[finger];
	if (down && !touch.down) {
		touch.id        = m_next_touch_id;
		m_next_touch_id = m_next_touch_id == 127 ? 1 : m_next_touch_id + 1;
		Claim(*source);
	}
	touch.down = down;
	touch.x    = x;
	touch.y    = y;
	if (id == HOST_INPUT_CONTROLLER_ID) {
		source->buttons = down ? source->buttons | PAD_BUTTON_TOUCH_PAD
		                       : source->buttons & ~PAD_BUTTON_TOUCH_PAD;
	}
	Update(time);
}

void PadInput::SetMotion(const PadMotion& motion, uint64_t time) {
	m_motion = motion;
	Update(time);
}

void PadInput::Reset(uint64_t time) {
	for (auto& source: m_sources) {
		source.buttons = 0;
		source.axes    = PAD_AXES_AT_REST;
		source.touch   = {};
	}
	m_next_touch_id = 1;
	Update(time);
}

void PadInput::Update(uint64_t time) {
	PadState state;
	state.time   = time;
	state.motion = m_motion;

	const Source* stick_left  = nullptr;
	const Source* stick_right = nullptr;
	for (const auto& source: m_sources) {
		if (source.id != HOST_INPUT_CONTROLLER_ID && source.id != m_active_id) {
			continue;
		}
		state.buttons |= source.buttons;
		// The gamepad comes after the keyboard in a tie.
		if (stick_left == nullptr || StickTilt(source.axes, Axis::LeftX) >=
		                                 StickTilt(stick_left->axes, Axis::LeftX)) {
			stick_left = &source;
		}
		if (stick_right == nullptr || StickTilt(source.axes, Axis::RightX) >=
		                                  StickTilt(stick_right->axes, Axis::RightX)) {
			stick_right = &source;
		}
		for (const auto trigger: {Axis::TriggerLeft, Axis::TriggerRight}) {
			const auto index    = static_cast<int>(trigger);
			state.axes[index] = std::max(state.axes[index], source.axes[index]);
		}
		for (int finger = 0; finger < 2; finger++) {
			if (source.touch[finger].down && !state.touch[finger].down) {
				state.touch[finger] = source.touch[finger];
			}
		}
	}
	if (stick_left != nullptr) {
		for (const auto axis: {Axis::LeftX, Axis::LeftY}) {
			state.axes[static_cast<int>(axis)] = stick_left->axes[static_cast<int>(axis)];
		}
	}
	if (stick_right != nullptr) {
		for (const auto axis: {Axis::RightX, Axis::RightY}) {
			state.axes[static_cast<int>(axis)] = stick_right->axes[static_cast<int>(axis)];
		}
	}
	if (state.axes[static_cast<int>(Axis::TriggerLeft)] >= TRIGGER_PRESSED) {
		state.buttons |= PAD_BUTTON_L2;
	}
	if (state.axes[static_cast<int>(Axis::TriggerRight)] >= TRIGGER_PRESSED) {
		state.buttons |= PAD_BUTTON_R2;
	}

	const bool changed = m_written == 0 || !SameInput(state, m_latest);
	m_latest           = state;
	if (!changed) {
		return;
	}
	// Input that changes no button or touch contact updates the newest unread state, so a burst
	// of stick or motion events between two reads cannot push unread button or touch changes out.
	if (m_written > m_read) {
		auto& newest = m_history[(m_written - 1) % HISTORY];
		if (newest.buttons == state.buttons && newest.touch[0].down == state.touch[0].down &&
		    newest.touch[1].down == state.touch[1].down) {
			newest = state;
			return;
		}
	}
	m_history[m_written % HISTORY] = state;
	m_written++;
}

int PadInput::Read(std::span<PadState> out) {
	const uint64_t oldest = std::max(m_read, m_written > HISTORY ? m_written - HISTORY : 0);
	const uint64_t count  = std::min<uint64_t>(m_written - oldest, out.size());
	const uint64_t first  = m_written - count;
	for (uint64_t i = 0; i < count; i++) {
		out[i] = m_history[(first + i) % HISTORY];
	}
	m_read = m_written;
	return static_cast<int>(count);
}

} // namespace Libs::Controller
