#include "graphics/presentation/window/hostInput.h"

#include <SDL3/SDL.h>

#include "common/assert.h"
#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "libs/controller.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace Libs::Graphics {

namespace {

struct ControlInfo {
	std::string_view name;
	uint32_t         button   = 0;
	Controller::Axis axis     = Controller::Axis::AxisMax;
	bool             positive = false;
	float            touch_x  = 0.0f;

	Controller::Setting setting {};
	bool                gyro          = false;
	bool                touchpad_mode = false;
};

static constexpr std::array CONTROL_INFO = {
    ControlInfo {"L3", Controller::PAD_BUTTON_L3},
    ControlInfo {"R3", Controller::PAD_BUTTON_R3},
    ControlInfo {"Options", Controller::PAD_BUTTON_OPTIONS},
    ControlInfo {"Up", Controller::PAD_BUTTON_UP},
    ControlInfo {"Right", Controller::PAD_BUTTON_RIGHT},
    ControlInfo {"Down", Controller::PAD_BUTTON_DOWN},
    ControlInfo {"Left", Controller::PAD_BUTTON_LEFT},
    ControlInfo {"L2", Controller::PAD_BUTTON_L2},
    ControlInfo {"R2", Controller::PAD_BUTTON_R2},
    ControlInfo {"L1", Controller::PAD_BUTTON_L1},
    ControlInfo {"R1", Controller::PAD_BUTTON_R1},
    ControlInfo {"Triangle", Controller::PAD_BUTTON_TRIANGLE},
    ControlInfo {"Circle", Controller::PAD_BUTTON_CIRCLE},
    ControlInfo {"Cross", Controller::PAD_BUTTON_CROSS},
    ControlInfo {"Square", Controller::PAD_BUTTON_SQUARE},
    ControlInfo {"TouchPad", Controller::PAD_BUTTON_TOUCH_PAD, Controller::Axis::AxisMax, false,
                 0.25f},
    ControlInfo {"TouchPadRight", Controller::PAD_BUTTON_TOUCH_PAD, Controller::Axis::AxisMax,
                 false, 0.75f},
    ControlInfo {"LeftStickLeft", 0, Controller::Axis::LeftX, false},
    ControlInfo {"LeftStickRight", 0, Controller::Axis::LeftX, true},
    ControlInfo {"LeftStickUp", 0, Controller::Axis::LeftY, false},
    ControlInfo {"LeftStickDown", 0, Controller::Axis::LeftY, true},
    ControlInfo {"RightStickLeft", 0, Controller::Axis::RightX, false},
    ControlInfo {"RightStickRight", 0, Controller::Axis::RightX, true},
    ControlInfo {"RightStickUp", 0, Controller::Axis::RightY, false},
    ControlInfo {"RightStickDown", 0, Controller::Axis::RightY, true},
    ControlInfo {.name = "SpeakerVolume", .setting = Controller::Setting::SpeakerVolume},
    ControlInfo {.name = "VibrationIntensity", .setting = Controller::Setting::VibrationIntensity},
    ControlInfo {.name    = "TriggerEffectIntensity",
                 .setting = Controller::Setting::TriggerEffectIntensity},
    ControlInfo {.name = "GyroToggle", .gyro = true},
    ControlInfo {.name = "TouchPadMode", .touchpad_mode = true},
};

constexpr std::size_t INVALID_CONTROL = CONTROL_INFO.size();

struct PadSource {
	std::string_view name;
	int              button; // SDL_GamepadButton, or -1 for an analog trigger
};

constexpr std::array PAD_SOURCES = {
    PadSource {"South", SDL_GAMEPAD_BUTTON_SOUTH},
    PadSource {"East", SDL_GAMEPAD_BUTTON_EAST},
    PadSource {"West", SDL_GAMEPAD_BUTTON_WEST},
    PadSource {"North", SDL_GAMEPAD_BUTTON_NORTH},
    PadSource {"Back", SDL_GAMEPAD_BUTTON_BACK},
    PadSource {"Guide", SDL_GAMEPAD_BUTTON_GUIDE},
    PadSource {"Start", SDL_GAMEPAD_BUTTON_START},
    PadSource {"LeftStick", SDL_GAMEPAD_BUTTON_LEFT_STICK},
    PadSource {"RightStick", SDL_GAMEPAD_BUTTON_RIGHT_STICK},
    PadSource {"LeftShoulder", SDL_GAMEPAD_BUTTON_LEFT_SHOULDER},
    PadSource {"RightShoulder", SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER},
    PadSource {"DpadUp", SDL_GAMEPAD_BUTTON_DPAD_UP},
    PadSource {"DpadDown", SDL_GAMEPAD_BUTTON_DPAD_DOWN},
    PadSource {"DpadLeft", SDL_GAMEPAD_BUTTON_DPAD_LEFT},
    PadSource {"DpadRight", SDL_GAMEPAD_BUTTON_DPAD_RIGHT},
    PadSource {"Misc1", SDL_GAMEPAD_BUTTON_MISC1},
    PadSource {"RightPaddle1", SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1},
    PadSource {"LeftPaddle1", SDL_GAMEPAD_BUTTON_LEFT_PADDLE1},
    PadSource {"RightPaddle2", SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2},
    PadSource {"LeftPaddle2", SDL_GAMEPAD_BUTTON_LEFT_PADDLE2},
    PadSource {"Touchpad", SDL_GAMEPAD_BUTTON_TOUCHPAD},
    PadSource {"LeftTrigger", -1},
    PadSource {"RightTrigger", -1},
};

constexpr std::size_t LEFT_TRIGGER_SOURCE  = PAD_SOURCES.size() - 2;
constexpr std::size_t RIGHT_TRIGGER_SOURCE = PAD_SOURCES.size() - 1;
constexpr std::size_t INVALID_SOURCE       = PAD_SOURCES.size();

struct PadDefault {
	std::string_view control;
	std::string_view source;
};

constexpr std::array PAD_DEFAULTS = {
    PadDefault {"Cross", "South"},         PadDefault {"Circle", "East"},
    PadDefault {"Square", "West"},         PadDefault {"Triangle", "North"},
    PadDefault {"Options", "Start"},       PadDefault {"L3", "LeftStick"},
    PadDefault {"R3", "RightStick"},       PadDefault {"L1", "LeftShoulder"},
    PadDefault {"R1", "RightShoulder"},    PadDefault {"L2", "LeftTrigger"},
    PadDefault {"R2", "RightTrigger"},     PadDefault {"Up", "DpadUp"},
    PadDefault {"Down", "DpadDown"},       PadDefault {"Left", "DpadLeft"},
    PadDefault {"Right", "DpadRight"},     PadDefault {"TouchPad", "Touchpad"},
    PadDefault {"GyroToggle", "Back"},
};

constexpr std::string_view PAD_PREFIX = "Pad:";

struct Binding {
	SDL_Keycode key          = SDLK_UNKNOWN;
	uint8_t     mouse_button = 0;
	std::size_t control      = INVALID_CONTROL;
};

constexpr int              MOUSE_POLL_INTERVAL_MS = 33;
constexpr uint64_t         CURSOR_IDLE_HIDE_MS    = 2000;
constexpr std::string_view MOUSE_SENSITIVITY      = "MouseSensitivity=";
constexpr std::string_view TOUCH_PARTICLE_SKIN    = "TouchParticleSkin=";

struct MouseJoystickState {
	bool     enabled   = false;
	bool     output    = false;
	uint64_t next_poll = 0;
};

MouseJoystickState g_mouse;
SDL_Window*        g_mouse_window   = nullptr;
uint64_t           g_cursor_hide_at = 0;

struct MouseTouchPadState {
	bool     enabled        = false;
	bool     cursor_visible = true;
	bool     drawing        = false;
	bool     position_valid = false;
	float    x              = 0.5f;
	float    y              = 0.5f;
	uint64_t revision       = 0;
	uint64_t gesture        = 0;
};

struct GamepadTouchPadState {
	bool     enabled   = false;
	bool     drawing   = false;
	int      id        = -1;
	int      x_axis    = 128;
	int      y_axis    = 128;
	float    x         = 0.5f;
	float    y         = 0.5f;
	uint64_t next_poll = 0;
	uint64_t last_poll = 0;
};

constexpr int   GAMEPAD_TOUCHPAD_POLL_MS = 8;
constexpr int   GAMEPAD_TOUCHPAD_DEADZONE = 18;
constexpr float GAMEPAD_TOUCHPAD_SPEED    = 0.8f;

MouseTouchPadState   g_mouse_touchpad;
GamepadTouchPadState g_gamepad_touchpad;
std::mutex           g_touchpad_mutex;
std::unordered_map<int, std::array<bool, 2>> g_pad_trigger_pressed;

std::size_t ControlFromName(std::string_view name) {
	const auto info = std::find_if(CONTROL_INFO.begin(), CONTROL_INFO.end(),
	                               [name](const auto& item) { return item.name == name; });
	return static_cast<std::size_t>(std::distance(CONTROL_INFO.begin(), info));
}

SDL_Keycode NormalizeKey(SDL_Keycode key) {
	switch (key) {
		case SDLK_RSHIFT: return SDLK_LSHIFT;
		case SDLK_RCTRL: return SDLK_LCTRL;
		case SDLK_RALT: return SDLK_LALT;
		case SDLK_RGUI: return SDLK_LGUI;
		default: return key;
	}
}

uint8_t MouseButtonFromName(std::string_view name) {
	static constexpr std::array buttons = {
	    std::pair {std::string_view("Mouse:Left"), uint8_t {SDL_BUTTON_LEFT}},
	    std::pair {std::string_view("Mouse:Middle"), uint8_t {SDL_BUTTON_MIDDLE}},
	    std::pair {std::string_view("Mouse:Right"), uint8_t {SDL_BUTTON_RIGHT}},
	    std::pair {std::string_view("Mouse:X1"), uint8_t {SDL_BUTTON_X1}},
	    std::pair {std::string_view("Mouse:X2"), uint8_t {SDL_BUTTON_X2}},
	};

	const auto button = std::find_if(buttons.begin(), buttons.end(),
	                                 [name](const auto& item) { return item.first == name; });
	return button != buttons.end() ? button->second : 0;
}

bool Conflicts(const Binding& first, const Binding& second) {
	return first.control == second.control ||
	       (first.key != SDLK_UNKNOWN && first.key == second.key) ||
	       (first.mouse_button != 0 && first.mouse_button == second.mouse_button);
}

class InputMap {
public:
	InputMap() {
		for (const auto& value: Config::GetKeymap()) {
			const std::string_view entry = value;
			if (entry == "Keyboard=Disabled") {
				m_custom = true;
				continue;
			}
			if (entry == "Pad=Disabled") {
				m_custom_pad = true;
				continue;
			}
			if (entry.starts_with(MOUSE_SENSITIVITY)) {
				const float sensitivity =
				    std::strtof(value.c_str() + MOUSE_SENSITIVITY.size(), nullptr);
				m_mouse_sensitivity =
				    std::clamp(std::isfinite(sensitivity) ? sensitivity : 1.0f, 0.1f, 5.0f);
				continue;
			}
			if (entry.starts_with(TOUCH_PARTICLE_SKIN) || entry.starts_with("TouchCursorImage=")) {
				continue;
			}
			if (entry.starts_with(PAD_PREFIX)) {
				AddPadEntry(entry.substr(PAD_PREFIX.size()), value);
				continue;
			}
			const auto split = entry.find('=');

			Binding binding;
			if (split != std::string_view::npos) {
				binding.control       = ControlFromName(entry.substr(0, split));
				const auto host_input = entry.substr(split + 1);
				binding.mouse_button  = MouseButtonFromName(host_input);
				if (binding.mouse_button == 0) {
					binding.key = NormalizeKey(SDL_GetKeyFromName(std::string(host_input).c_str()));
				}
			}

			const bool reserved = binding.key == SDLK_ESCAPE || binding.key == SDLK_F1 ||
			                      binding.key == SDLK_F7 || binding.key == SDLK_F8 ||
			                      binding.key == SDLK_F11;
			if (binding.control == INVALID_CONTROL || reserved ||
			    (binding.key == SDLK_UNKNOWN && binding.mouse_button == 0)) {
				EXIT("Invalid input mapping: %s\n", value.c_str());
			}
			Add(binding);
		}
		if (!m_custom_pad) {
			for (const auto& item: PAD_DEFAULTS) {
				AssignPad(ControlFromName(item.control), SourceFromName(item.source));
			}
		}
	}
	[[nodiscard]] bool  Custom() const { return m_custom; }
	[[nodiscard]] float MouseSensitivity() const { return m_mouse_sensitivity; }

	[[nodiscard]] std::size_t PadControl(std::size_t source) const {
		return source < m_pad_controls.size() ? m_pad_controls[source] : INVALID_CONTROL;
	}

	static std::size_t SourceFromButton(int button) {
		const auto item = std::find_if(PAD_SOURCES.begin(), PAD_SOURCES.end(),
		                               [button](const auto& s) { return s.button == button; });
		return static_cast<std::size_t>(std::distance(PAD_SOURCES.begin(), item));
	}

	[[nodiscard]] std::size_t FindKey(int key_code) const {
		key_code = NormalizeKey(static_cast<SDL_Keycode>(key_code));
		const auto binding =
		    std::find_if(m_bindings.begin(), m_bindings.begin() + m_size,
		                 [key_code](const auto& item) { return item.key == key_code; });
		return binding != m_bindings.begin() + m_size ? binding->control : INVALID_CONTROL;
	}

	[[nodiscard]] std::size_t FindMouseButton(uint8_t mouse_button) const {
		const auto binding = std::find_if(
		    m_bindings.begin(), m_bindings.begin() + m_size,
		    [mouse_button](const auto& item) { return item.mouse_button == mouse_button; });
		return binding != m_bindings.begin() + m_size ? binding->control : INVALID_CONTROL;
	}

private:
	static std::size_t SourceFromName(std::string_view name) {
		const auto item = std::find_if(PAD_SOURCES.begin(), PAD_SOURCES.end(),
		                               [name](const auto& s) { return s.name == name; });
		return static_cast<std::size_t>(std::distance(PAD_SOURCES.begin(), item));
	}

	void AssignPad(std::size_t control, std::size_t source) {
		for (auto& existing: m_pad_controls) {
			if (existing == control) {
				existing = INVALID_CONTROL;
			}
		}
		m_pad_controls[source] = control;
	}

	void AddPadEntry(std::string_view entry, const std::string& original) {
		const auto split   = entry.find('=');
		const auto control = split != std::string_view::npos ? ControlFromName(entry.substr(0, split))
		                                                     : INVALID_CONTROL;
		const auto source  = split != std::string_view::npos ? SourceFromName(entry.substr(split + 1))
		                                                     : INVALID_SOURCE;
		if (control == INVALID_CONTROL || source == INVALID_SOURCE) {
			EXIT("Invalid input mapping: %s\n", original.c_str());
		}
		const auto& info = CONTROL_INFO[control];
		if (info.button == 0 && !info.gyro && info.axis != Controller::Axis::AxisMax) {
			EXIT("Invalid input mapping: %s\n", original.c_str());
		}
		m_custom_pad = true;
		AssignPad(control, source);
	}
	void Add(const Binding& binding) {
		m_custom = true;
		for (std::size_t index = 0; index < m_size;) {
			if (Conflicts(m_bindings[index], binding)) {
				m_bindings[index] = m_bindings[--m_size];
			} else {
				index++;
			}
		}
		EXIT_IF(m_size >= m_bindings.size());
		m_bindings[m_size++] = binding;
	}

	std::array<Binding, CONTROL_INFO.size()> m_bindings {};
	std::array<std::size_t, PAD_SOURCES.size()> m_pad_controls = [] {
		std::array<std::size_t, PAD_SOURCES.size()> a {};
		a.fill(INVALID_CONTROL);
		return a;
	}();
	bool                                     m_custom = false;
	bool                                     m_custom_pad = false;
	std::size_t                              m_size              = 0;
	float                                    m_mouse_sensitivity = 1.0f;
};

const InputMap& GetInputMap() {
	static const InputMap map;
	return map;
}

void SetButton(uint32_t button, bool down) {
	if (button == Controller::PAD_BUTTON_L2) {
		Controller::SetAxis(Controller::HOST_INPUT_CONTROLLER_ID, Controller::Axis::TriggerLeft,
		                    down ? 255 : 0);
	} else if (button == Controller::PAD_BUTTON_R2) {
		Controller::SetAxis(Controller::HOST_INPUT_CONTROLLER_ID, Controller::Axis::TriggerRight,
		                    down ? 255 : 0);
	} else if (button != 0) {
		Controller::SetButton(Controller::HOST_INPUT_CONTROLLER_ID, button, down);
	}
}

void SetTouchPad(float x, bool down) {
	Controller::SetTouchPad(Controller::HOST_INPUT_CONTROLLER_ID, 0, down, x, 0.5f);
}

void SetSwipeTouchPad(bool down, float x, float y) {
	Controller::SetTouchPad(Controller::HOST_INPUT_CONTROLLER_ID, 1, down, x, y);
}

void RecordMouseTouchPadPosition(float x, float y) {
	int width  = 0;
	int height = 0;
	if (g_mouse_window != nullptr) {
		SDL_GetWindowSize(g_mouse_window, &width, &height);
	}
	if (width > 0 && height > 0) {
		x = std::clamp(x / static_cast<float>(width), 0.0f, 1.0f);
		y = std::clamp(y / static_cast<float>(height), 0.0f, 1.0f);
	} else {
		x = 0.5f;
		y = 0.5f;
	}

	std::scoped_lock lock(g_touchpad_mutex);
	if (!g_mouse_touchpad.enabled) {
		return;
	}
	g_mouse_touchpad.x              = x;
	g_mouse_touchpad.y              = y;
	g_mouse_touchpad.position_valid = true;
	g_mouse_touchpad.revision++;
}

bool TouchPadModeActive() {
	std::scoped_lock lock(g_touchpad_mutex);
	return g_mouse_touchpad.enabled || g_gamepad_touchpad.enabled;
}

uint32_t DefaultKeyboardButton(int key_code) {
	switch (NormalizeKey(static_cast<SDL_Keycode>(key_code))) {
		case SDLK_UP: return Controller::PAD_BUTTON_UP;
		case SDLK_LEFT: return Controller::PAD_BUTTON_LEFT;
		case SDLK_DOWN: return Controller::PAD_BUTTON_DOWN;
		case SDLK_RIGHT: return Controller::PAD_BUTTON_RIGHT;
		case SDLK_J: return Controller::PAD_BUTTON_CROSS;
		case SDLK_I: return Controller::PAD_BUTTON_TRIANGLE;
		case SDLK_K: return Controller::PAD_BUTTON_SQUARE;
		case SDLK_L: return Controller::PAD_BUTTON_CIRCLE;
		case SDLK_Q: return Controller::PAD_BUTTON_L1;
		case SDLK_E: return Controller::PAD_BUTTON_R1;
		case SDLK_Z: return Controller::PAD_BUTTON_L2;
		case SDLK_C: return Controller::PAD_BUTTON_R2;
		case SDLK_LSHIFT: return Controller::PAD_BUTTON_L3;
		case SDLK_LCTRL: return Controller::PAD_BUTTON_R3;
		case SDLK_RETURN:
		case SDLK_RETURN2: return Controller::PAD_BUTTON_OPTIONS;
		default: return 0;
	}
}

struct StickKeys {
	bool left  = false;
	bool right = false;
	bool up    = false;
	bool down  = false;
};

void SetStickAxis(Controller::Axis axis, bool negative, bool positive) {
	int value = 128;
	if (negative && !positive) {
		value = 0;
	} else if (positive && !negative) {
		value = 255;
	}
	Controller::SetAxis(Controller::HOST_INPUT_CONTROLLER_ID, axis, value);
}

void CycleSettingOnPress(Controller::Setting setting, bool down) {
	if (down) {
		Controller::CycleSetting(setting);
	}
}

void SetControl(std::size_t control, bool down) {
	if (control == INVALID_CONTROL) {
		return;
	}

	const auto& info = CONTROL_INFO[control];
	if (info.button == Controller::PAD_BUTTON_TOUCH_PAD) {
		SetTouchPad(info.touch_x, down);
		return;
	}
	if (info.button != 0) {
		SetButton(info.button, down);
		return;
	}
	if (info.gyro) {
		if (down) {
			Controller::ToggleGyroEmulation(Controller::HOST_INPUT_CONTROLLER_ID);
		}
		return;
	}
	if (info.touchpad_mode) {
		if (down) {
			HostInputToggleTouchPadMode();
		}
		return;
	}
	if (info.axis == Controller::Axis::AxisMax) {
		CycleSettingOnPress(info.setting, down);
		return;
	}

	struct AxisKeys {
		bool negative = false;
		bool positive = false;
	};
	static std::array<AxisKeys, 4> axes;

	const auto axis = static_cast<std::size_t>(info.axis);
	EXIT_IF(axis >= axes.size());
	if (info.positive) {
		axes[axis].positive = down;
	} else {
		axes[axis].negative = down;
	}
	SetStickAxis(info.axis, axes[axis].negative, axes[axis].positive);
}

void DefaultKeyboardInput(int key_code, bool down) {
	static StickKeys left;
	static StickKeys right;

	switch (NormalizeKey(static_cast<SDL_Keycode>(key_code))) {
		case SDLK_BACKSPACE: SetTouchPad(0.25f, down); return;
		case SDLK_F6:
			if (down) {
				HostInputToggleTouchPadMode();
			}
			return;
		case SDLK_TAB: SetTouchPad(0.75f, down); return;
		case SDLK_1: CycleSettingOnPress(Controller::Setting::SpeakerVolume, down); return;
		case SDLK_2: CycleSettingOnPress(Controller::Setting::VibrationIntensity, down); return;
		case SDLK_3: CycleSettingOnPress(Controller::Setting::TriggerEffectIntensity, down); return;
		case SDLK_A:
			left.left = down;
			SetStickAxis(Controller::Axis::LeftX, left.left, left.right);
			return;
		case SDLK_D:
			left.right = down;
			SetStickAxis(Controller::Axis::LeftX, left.left, left.right);
			return;
		case SDLK_W:
			left.up = down;
			SetStickAxis(Controller::Axis::LeftY, left.up, left.down);
			return;
		case SDLK_S:
			left.down = down;
			SetStickAxis(Controller::Axis::LeftY, left.up, left.down);
			return;
		case SDLK_F:
			right.left = down;
			SetStickAxis(Controller::Axis::RightX, right.left, right.right);
			return;
		case SDLK_H:
			right.right = down;
			SetStickAxis(Controller::Axis::RightX, right.left, right.right);
			return;
		case SDLK_T:
			right.up = down;
			SetStickAxis(Controller::Axis::RightY, right.up, right.down);
			return;
		case SDLK_G:
			right.down = down;
			SetStickAxis(Controller::Axis::RightY, right.up, right.down);
			return;
		default: SetButton(DefaultKeyboardButton(key_code), down); return;
	}
}

void MouseToJoystick(float delta_x, float delta_y) {
	const double distance = std::hypot(delta_x, delta_y);
	const double scale =
	    std::clamp(distance * GetInputMap().MouseSensitivity() + 16.0, 64.0, 128.0) / distance;
	const auto map_axis = [scale](float delta) {
		return std::clamp(128 + static_cast<int>(std::lround(delta * scale)), 0, 255);
	};
	Controller::SetRightStick(Controller::HOST_INPUT_CONTROLLER_ID, map_axis(delta_x),
	                          map_axis(delta_y));
}

void CenterMouseStick() {
	if (!g_mouse.output) {
		return;
	}
	Controller::SetRightStick(Controller::HOST_INPUT_CONTROLLER_ID, 128, 128);
	g_mouse.output = false;
}

bool SetRelativeMouseMode(bool enabled) {
	if (SDL_SetWindowRelativeMouseMode(g_mouse_window, enabled)) {
		return true;
	}
	LOGF("Mouse-to-joystick relative mode failed: %s\n", SDL_GetError());
	return false;
}

int PollMouse(uint64_t now_ms) {
	if (now_ms < g_mouse.next_poll) {
		return static_cast<int>(g_mouse.next_poll - now_ms);
	}
	g_mouse.next_poll = now_ms + MOUSE_POLL_INTERVAL_MS;

	float delta_x = 0;
	float delta_y = 0;
	SDL_GetRelativeMouseState(&delta_x, &delta_y);
	if (delta_x == 0 && delta_y == 0) {
		CenterMouseStick();
	} else {
		MouseToJoystick(delta_x, delta_y);
		g_mouse.output = true;
	}
	return MOUSE_POLL_INTERVAL_MS;
}

bool IsCursorActivity(const SDL_Event& event) {
	SDL_MouseID which;
	switch (event.type) {
		case SDL_EVENT_MOUSE_MOTION:
			if (event.motion.xrel == 0.0f && event.motion.yrel == 0.0f) {
				return false;
			}
			which = event.motion.which;
			break;
		case SDL_EVENT_MOUSE_BUTTON_DOWN:
		case SDL_EVENT_MOUSE_BUTTON_UP: which = event.button.which; break;
		case SDL_EVENT_MOUSE_WHEEL: which = event.wheel.which; break;
		default: return false;
	}
	return which != SDL_TOUCH_MOUSEID && which != SDL_PEN_MOUSEID;
}

std::array<int, 2> GamepadLeftStick(int id) {
	auto* pad = SDL_GetGamepadFromID(id);
	if (pad == nullptr) {
		return {128, 128};
	}
	const auto value = [pad](SDL_GamepadAxis axis) {
		return Controller::controller_get_axis(SDL_JOYSTICK_AXIS_MIN, SDL_JOYSTICK_AXIS_MAX,
		                                       SDL_GetGamepadAxis(pad, axis));
	};
	return {value(SDL_GAMEPAD_AXIS_LEFTX), value(SDL_GAMEPAD_AXIS_LEFTY)};
}

void ApplyPadControl(int id, std::size_t control, std::size_t source, bool down) {
	if (control == INVALID_CONTROL) {
		return;
	}
	const auto& info = CONTROL_INFO[control];
	if (info.gyro) {
		if (down) {
			Controller::ToggleGyroEmulation(id);
		}
	} else if (info.touchpad_mode) {
		if (down) {
			HostInputToggleGamepadTouchPad(id);
		}
	} else if (info.button == Controller::PAD_BUTTON_CROSS && HostInputGamepadDrawButton(id, down)) {
		return;
	} else if (info.button == Controller::PAD_BUTTON_TOUCH_PAD) {
		// Real touchpads report their own finger position; other inputs emulate a click.
		if (source != PAD_SOURCES.size() && PAD_SOURCES[source].button != SDL_GAMEPAD_BUTTON_TOUCHPAD) {
			Controller::SetTouchPad(id, 0, down, info.touch_x != 0.0f ? info.touch_x : 0.5f, 0.5f);
		}
		Controller::SetButton(id, Controller::PAD_BUTTON_TOUCH_PAD, down);
	} else if (info.button == Controller::PAD_BUTTON_L2) {
		Controller::SetAxis(id, Controller::Axis::TriggerLeft, down ? 255 : 0);
	} else if (info.button == Controller::PAD_BUTTON_R2) {
		Controller::SetAxis(id, Controller::Axis::TriggerRight, down ? 255 : 0);
	} else if (info.button != 0) {
		Controller::SetButton(id, info.button, down);
	} else if (info.axis == Controller::Axis::AxisMax) {
		CycleSettingOnPress(info.setting, down);
	}
}

int PollGamepadTouchPad(uint64_t now_ms) {
	float x       = 0;
	float y       = 0;
	int   id      = -1;
	bool  drawing = false;
	{
		std::scoped_lock lock(g_touchpad_mutex);
		if (!g_gamepad_touchpad.enabled) {
			return -1;
		}
		if (now_ms < g_gamepad_touchpad.next_poll) {
			return static_cast<int>(g_gamepad_touchpad.next_poll - now_ms);
		}

		const float dt = g_gamepad_touchpad.last_poll == 0 || now_ms <= g_gamepad_touchpad.last_poll
		                     ? 0.008f
		                     : std::min(static_cast<float>(now_ms - g_gamepad_touchpad.last_poll) *
		                                    0.001f,
		                                0.05f);
		g_gamepad_touchpad.last_poll = now_ms;
		g_gamepad_touchpad.next_poll = now_ms + GAMEPAD_TOUCHPAD_POLL_MS;
		const auto velocity          = [](int axis) {
            const int offset = axis - 128;
            if (std::abs(offset) <= GAMEPAD_TOUCHPAD_DEADZONE) {
                return 0.0f;
            }
            const int adjusted =
                offset > 0 ? offset - GAMEPAD_TOUCHPAD_DEADZONE : offset + GAMEPAD_TOUCHPAD_DEADZONE;
            return static_cast<float>(adjusted) / static_cast<float>(127 - GAMEPAD_TOUCHPAD_DEADZONE);
		};
		const float old_x = g_gamepad_touchpad.x;
		const float old_y = g_gamepad_touchpad.y;
		g_gamepad_touchpad.x = std::clamp(
		    old_x + velocity(g_gamepad_touchpad.x_axis) * GAMEPAD_TOUCHPAD_SPEED * dt, 0.0f, 1.0f);
		g_gamepad_touchpad.y = std::clamp(
		    old_y + velocity(g_gamepad_touchpad.y_axis) * GAMEPAD_TOUCHPAD_SPEED * dt, 0.0f, 1.0f);
		if (old_x != g_gamepad_touchpad.x || old_y != g_gamepad_touchpad.y) {
			g_mouse_touchpad.revision++;
		}
		id      = g_gamepad_touchpad.id;
		x       = g_gamepad_touchpad.x;
		y       = g_gamepad_touchpad.y;
		drawing = g_gamepad_touchpad.drawing;
	}
	if (drawing) {
		Controller::SetTouchPad(id, 1, true, x, y);
	}
	return GAMEPAD_TOUCHPAD_POLL_MS;
}

} // namespace

void HostInputPadButton(int id, int button, bool down) {
	const auto  source = InputMap::SourceFromButton(button);
	const auto& map    = GetInputMap();
	ApplyPadControl(id, map.PadControl(source), source, down);
}

void HostInputPadTrigger(int id, bool left, int value) {
	const auto  source     = left ? LEFT_TRIGGER_SOURCE : RIGHT_TRIGGER_SOURCE;
	const auto  control    = GetInputMap().PadControl(source);
	if (control == INVALID_CONTROL) {
		return;
	}
	const auto& info = CONTROL_INFO[control];
	if (info.button == Controller::PAD_BUTTON_L2) {
		Controller::SetAxis(id, Controller::Axis::TriggerLeft, value);
	} else if (info.button == Controller::PAD_BUTTON_R2) {
		Controller::SetAxis(id, Controller::Axis::TriggerRight, value);
	} else {
		auto& state = g_pad_trigger_pressed[id][left ? 0 : 1];
		const bool down = state ? value > 32 : value > 96;
		if (down != state) {
			state = down;
			ApplyPadControl(id, control, source, down);
		}
	}
}

void HostInputDisconnectGamepad(int id) {
	g_pad_trigger_pressed.erase(id);
	HostInputDisconnectGamepadTouchPad(id);
}

void HostInputInit(SDL_Window* window) {
	GetInputMap();
	g_mouse_window = window;
	if (Config::HideCursorEnabled()) {
		g_cursor_hide_at = SDL_GetTicks() + CURSOR_IDLE_HIDE_MS;
	}
}

void HostInputShutdown() {
	g_pad_trigger_pressed.clear();
	HostInputSetTouchPadMode(false);
	int gamepad_touch_id = -1;
	{
		std::scoped_lock lock(g_touchpad_mutex);
		gamepad_touch_id = g_gamepad_touchpad.enabled ? g_gamepad_touchpad.id : -1;
	}
	if (gamepad_touch_id >= 0) {
		HostInputDisconnectGamepadTouchPad(gamepad_touch_id);
	}
	if (g_mouse.enabled) {
		SetRelativeMouseMode(false);
		CenterMouseStick();
		g_mouse = {};
	}
	g_mouse_window   = nullptr;
	g_cursor_hide_at = 0;
	if (Config::HideCursorEnabled()) {
		SDL_ShowCursor();
	}
}

void HostInputKey(int key_code, bool down) {
	const auto& map = GetInputMap();
	if (map.Custom()) {
		SetControl(map.FindKey(key_code), down);
	} else {
		DefaultKeyboardInput(key_code, down);
	}
}

void HostInputMouseButton(uint8_t mouse_button, bool down) {
	const auto& map = GetInputMap();
	if (map.Custom() && mouse_button != 0) {
		SetControl(map.FindMouseButton(mouse_button), down);
	}
}

void HostInputMouseTouchPad(bool down, float x, float y) {
	RecordMouseTouchPadPosition(x, y);
	{
		std::scoped_lock lock(g_touchpad_mutex);
		if (!g_mouse_touchpad.enabled) {
			return;
		}
		if (down && !g_mouse_touchpad.drawing) {
			g_mouse_touchpad.gesture++;
		}
		g_mouse_touchpad.drawing = down;
		g_mouse_touchpad.revision++;
		x = g_mouse_touchpad.x;
		y = g_mouse_touchpad.y;
	}
	SetSwipeTouchPad(down, x, y);
}

void HostInputMouseMotion(float x, float y) {
	RecordMouseTouchPadPosition(x, y);
	std::scoped_lock lock(g_touchpad_mutex);
	if (g_mouse_touchpad.enabled && g_mouse_touchpad.drawing) {
		SetSwipeTouchPad(true, g_mouse_touchpad.x, g_mouse_touchpad.y);
	}
}

void HostInputSetTouchPadMode(bool enabled) {
	if (enabled) {
		int gamepad_id = -1;
		{
			std::scoped_lock lock(g_touchpad_mutex);
			if (g_gamepad_touchpad.enabled) {
				gamepad_id = g_gamepad_touchpad.id;
			}
		}
		if (gamepad_id >= 0) {
			HostInputDisconnectGamepadTouchPad(gamepad_id);
		}
	}
	{
		std::scoped_lock lock(g_touchpad_mutex);
		if (g_mouse_touchpad.enabled == enabled) {
			return;
		}
	}
	if (!enabled) {
		float x = 0;
		float y = 0;
		(void) SDL_GetMouseState(&x, &y);
		HostInputMouseTouchPad(false, x, y);
	}
	bool show_system_cursor = false;
	{
		std::scoped_lock lock(g_touchpad_mutex);
		g_mouse_touchpad.enabled        = enabled;
		g_mouse_touchpad.cursor_visible = true;
		g_mouse_touchpad.drawing        = false;
		g_mouse_touchpad.position_valid = false;
		g_mouse_touchpad.gesture++;
		g_mouse_touchpad.revision++;
		show_system_cursor = !enabled && !g_gamepad_touchpad.enabled;
	}
	if (enabled) {
		if (!SDL_HideCursor()) {
			LOGF("Failed to hide mouse cursor for touchpad mode: %s\n", SDL_GetError());
		}
	} else if (show_system_cursor && !Config::HideCursorEnabled() && !SDL_ShowCursor()) {
		LOGF("Failed to restore mouse cursor after touchpad mode: %s\n", SDL_GetError());
	}
	LOGF("Mouse touchpad mode: %s\n", enabled ? "enabled" : "disabled");
}

void HostInputToggleTouchPadMode() {
	bool enabled = false;
	{
		std::scoped_lock lock(g_touchpad_mutex);
		enabled = !g_mouse_touchpad.enabled;
	}
	HostInputSetTouchPadMode(enabled);
}

void HostInputToggleGamepadTouchPad(int id) {
	bool mouse_mode = false;
	{
		std::scoped_lock lock(g_touchpad_mutex);
		mouse_mode = g_mouse_touchpad.enabled;
	}
	if (mouse_mode) {
		HostInputSetTouchPadMode(false);
	}

	const auto stick = GamepadLeftStick(id);
	bool  enabled = false;
	float x       = 0.5f;
	float y       = 0.5f;
	{
		std::scoped_lock lock(g_touchpad_mutex);
		if (g_gamepad_touchpad.enabled && g_gamepad_touchpad.id != id) {
			return;
		}
		enabled = !g_gamepad_touchpad.enabled;
		if (enabled) {
			g_gamepad_touchpad           = {};
			g_gamepad_touchpad.enabled   = true;
			g_gamepad_touchpad.id        = id;
			g_gamepad_touchpad.x_axis    = stick[0];
			g_gamepad_touchpad.y_axis    = stick[1];
			g_gamepad_touchpad.last_poll = SDL_GetTicks();
			g_gamepad_touchpad.next_poll = g_gamepad_touchpad.last_poll + GAMEPAD_TOUCHPAD_POLL_MS;
			g_mouse_touchpad.gesture++;
		} else {
			x                          = g_gamepad_touchpad.x;
			y                          = g_gamepad_touchpad.y;
			g_gamepad_touchpad         = {};
		}
		g_mouse_touchpad.revision++;
	}
	Controller::SetAxis(id, Controller::Axis::LeftX, enabled ? 128 : stick[0]);
	Controller::SetAxis(id, Controller::Axis::LeftY, enabled ? 128 : stick[1]);
	if (enabled) {
		if (!SDL_HideCursor()) {
			LOGF("Failed to hide mouse cursor for gamepad touchpad mode: %s\n", SDL_GetError());
		}
	} else {
		Controller::SetTouchPad(id, 1, false, x, y);
		if (!Config::HideCursorEnabled() && !SDL_ShowCursor()) {
			LOGF("Failed to restore mouse cursor after gamepad touchpad mode: %s\n",
			     SDL_GetError());
		}
	}
	LOGF("Gamepad touchpad mode: %s\n", enabled ? "enabled" : "disabled");
}

bool HostInputGamepadDrawButton(int id, bool down) {
	float x = 0;
	float y = 0;
	{
		std::scoped_lock lock(g_touchpad_mutex);
		if (!g_gamepad_touchpad.enabled || g_gamepad_touchpad.id != id) {
			return false;
		}
		if (!down && !g_gamepad_touchpad.drawing) {
			return false;
		}
		if (down && !g_gamepad_touchpad.drawing) {
			g_mouse_touchpad.gesture++;
		}
		g_gamepad_touchpad.drawing = down;
		x                          = g_gamepad_touchpad.x;
		y                          = g_gamepad_touchpad.y;
		g_mouse_touchpad.revision++;
	}
	Controller::SetTouchPad(id, 1, down, x, y);
	return true;
}

bool HostInputGamepadAxis(int id, bool horizontal, int value) {
	std::scoped_lock lock(g_touchpad_mutex);
	if (!g_gamepad_touchpad.enabled || g_gamepad_touchpad.id != id) {
		return false;
	}
	(horizontal ? g_gamepad_touchpad.x_axis : g_gamepad_touchpad.y_axis) = std::clamp(value, 0, 255);
	g_gamepad_touchpad.next_poll = SDL_GetTicks();
	return true;
}

void HostInputDisconnectGamepadTouchPad(int id) {
	float x = 0;
	float y = 0;
	{
		std::scoped_lock lock(g_touchpad_mutex);
		if (!g_gamepad_touchpad.enabled || g_gamepad_touchpad.id != id) {
			return;
		}
		x                  = g_gamepad_touchpad.x;
		y                  = g_gamepad_touchpad.y;
		g_gamepad_touchpad = {};
		g_mouse_touchpad.revision++;
	}
	Controller::SetTouchPad(id, 1, false, x, y);
	const auto stick = GamepadLeftStick(id);
	Controller::SetAxis(id, Controller::Axis::LeftX, stick[0]);
	Controller::SetAxis(id, Controller::Axis::LeftY, stick[1]);
	if (!TouchPadModeActive() && !Config::HideCursorEnabled() && !SDL_ShowCursor()) {
		LOGF("Failed to restore mouse cursor after gamepad disconnect: %s\n", SDL_GetError());
	}
}

HostInputTouchPadVisualState GetHostInputTouchPadVisualState() {
	std::scoped_lock             lock(g_touchpad_mutex);
	HostInputTouchPadVisualState state {};
	if (g_gamepad_touchpad.enabled) {
		state.enabled        = true;
		state.cursor_visible = true;
		state.drawing        = g_gamepad_touchpad.drawing;
		state.x              = g_gamepad_touchpad.x;
		state.y              = g_gamepad_touchpad.y;
		state.revision       = g_mouse_touchpad.revision;
		state.gesture        = g_mouse_touchpad.gesture;
		state.position_valid = true;
		return state;
	}
	state.enabled        = g_mouse_touchpad.enabled;
	state.cursor_visible = g_mouse_touchpad.cursor_visible;
	state.drawing        = g_mouse_touchpad.drawing;
	state.x              = g_mouse_touchpad.x;
	state.y              = g_mouse_touchpad.y;
	state.revision       = g_mouse_touchpad.revision;
	state.gesture        = g_mouse_touchpad.gesture;
	state.position_valid = g_mouse_touchpad.position_valid;
	return state;
}

void HostInputToggleMouseToJoystick() {
	if (g_mouse.enabled) {
		SetRelativeMouseMode(false);
		CenterMouseStick();
		g_mouse = {};
		LOGF("Mouse to right stick: disabled\n");
		return;
	}

	if (!SetRelativeMouseMode(true)) {
		return;
	}
	g_mouse.enabled = true;
	LOGF("Mouse to right stick: enabled (F7 to release)\n");
}

bool HostInputWaitEvent(SDL_Event* event, int max_wait_ms) {
	int timeout = -1;
	if (!g_mouse.enabled || SDL_GetKeyboardFocus() != g_mouse_window) {
		g_mouse.next_poll = 0;
		CenterMouseStick();
	} else {
		if (g_mouse.next_poll == 0) {
			SDL_GetRelativeMouseState(nullptr, nullptr);
			g_mouse.next_poll = SDL_GetTicks() + MOUSE_POLL_INTERVAL_MS;
		}
		timeout = PollMouse(SDL_GetTicks());
	}

	if (g_cursor_hide_at != 0 && !TouchPadModeActive()) {
		const auto now_ms = SDL_GetTicks();
		const int  cursor_timeout =
		    now_ms < g_cursor_hide_at ? static_cast<int>(g_cursor_hide_at - now_ms) : 0;
		timeout = timeout < 0 ? cursor_timeout : std::min(timeout, cursor_timeout);
	}
	if (const int touch_timeout = PollGamepadTouchPad(SDL_GetTicks()); touch_timeout >= 0) {
		timeout = timeout < 0 ? touch_timeout : std::min(timeout, touch_timeout);
	}
	timeout              = timeout < 0 ? max_wait_ms : std::min(timeout, max_wait_ms);
	const bool has_event = SDL_WaitEventTimeout(event, timeout);

	if (Config::HideCursorEnabled() && !TouchPadModeActive()) {
		const auto now_ms = SDL_GetTicks();
		if (has_event && !g_mouse.enabled && IsCursorActivity(*event) &&
		    SDL_GetWindowFromEvent(event) == g_mouse_window) {
			SDL_ShowCursor();
			g_cursor_hide_at = now_ms + CURSOR_IDLE_HIDE_MS;
		} else if (g_cursor_hide_at != 0 && now_ms >= g_cursor_hide_at) {
			SDL_HideCursor();
			g_cursor_hide_at = 0;
		}
	}

	if (has_event && event->type == SDL_EVENT_WINDOW_FOCUS_LOST &&
	    event->window.windowID == SDL_GetWindowID(g_mouse_window)) {
		g_mouse.next_poll = 0;
		CenterMouseStick();
	}
	return has_event;
}

} // namespace Libs::Graphics
