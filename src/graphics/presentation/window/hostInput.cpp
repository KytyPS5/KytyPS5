#include "graphics/presentation/window/hostInput.h"

#include "SDL_error.h"
#include "SDL_events.h"
#include "SDL_keyboard.h"
#include "SDL_keycode.h"
#include "SDL_mouse.h"
#include "SDL_timer.h"
#include "SDL_video.h"
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

namespace Libs::Graphics {

namespace {

struct ControlInfo {
	std::string_view name;
	uint32_t         button   = 0;
	Controller::Axis axis     = Controller::Axis::AxisMax;
	bool             positive = false;
	float            touch_x  = 0.0f;
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
};

constexpr std::size_t INVALID_CONTROL = CONTROL_INFO.size();

enum class SpecialBinding {
	None,
	AnalogModifier,
	AnalogStepDown,
	AnalogStepMiddle,
	AnalogStepUp,
	AnalogLock,
	Gyro
};

enum class AnalogTarget { None, L2, R2, Microphone, Gyro };

struct Binding {
	SDL_Keycode    key          = SDLK_UNKNOWN;
	SDL_Scancode   scancode     = SDL_SCANCODE_UNKNOWN;
	uint8_t        mouse_button = 0;
	int            wheel        = 0;
	SpecialBinding special      = SpecialBinding::None;
	std::size_t    control      = INVALID_CONTROL;
};

constexpr int              MOUSE_POLL_INTERVAL_MS = 33;
constexpr std::string_view MOUSE_SENSITIVITY      = "MouseSensitivity=";

struct MouseJoystickState {
	bool     enabled   = false;
	bool     output    = false;
	uint64_t next_poll = 0;
};

struct AxisKeys {
	bool negative = false;
	bool positive = false;
};

enum class TriggerTarget { None, Left, Right };

MouseJoystickState      g_mouse;
std::mutex              g_diagnostics_mutex;
HostInputDiagnostics    g_diagnostics;
bool                    g_caps_lock               = false;
bool                    g_swipe_mode              = false;
bool                    g_touch_active            = false;
uint32_t                g_touch_direction         = 0;
bool                    g_analog_modifier         = false;
int                     g_analog_step             = 100;
bool                    g_l2_down                 = false;
bool                    g_r2_down                 = false;
bool                    g_l2_adjusting            = false;
bool                    g_r2_adjusting            = false;
uint8_t                 g_l2_value                = 0;
uint8_t                 g_r2_value                = 0;
uint8_t                 g_l2_frozen_value         = 0;
uint8_t                 g_r2_frozen_value         = 0;
TriggerTarget           g_last_trigger_target     = TriggerTarget::None;
bool                    g_gyro_down               = false;
float                   g_gyro_value              = 0.0f;
float                   g_gyro_frozen_value       = 0.0f;
bool                    g_microphone_down         = false;
uint8_t                 g_microphone_value        = 0;
uint8_t                 g_microphone_frozen_value = 0;
bool                    g_adjustment_active       = false;
AnalogTarget            g_analog_target           = AnalogTarget::None;
std::array<AxisKeys, 4> g_axis_state {};

SDL_Scancode NormalizeScancode(SDL_Scancode scancode) {
	switch (scancode) {
		case SDL_SCANCODE_RSHIFT: return SDL_SCANCODE_LSHIFT;
		case SDL_SCANCODE_RCTRL: return SDL_SCANCODE_LCTRL;
		case SDL_SCANCODE_RALT: return SDL_SCANCODE_LALT;
		case SDL_SCANCODE_RGUI: return SDL_SCANCODE_LGUI;
		default: return scancode;
	}
}

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

int MouseWheelFromName(std::string_view name) {
	if (name == "Mouse:WheelUp") return 1;
	if (name == "Mouse:WheelDown") return -1;
	return 0;
}

SDL_Scancode ScancodeFromName(std::string_view name) {
	if (name == "Minus") return SDL_SCANCODE_MINUS;
	if (name == "Equals" || name == "Plus") return SDL_SCANCODE_EQUALS;
	if (name == "CapsLock") return SDL_SCANCODE_CAPSLOCK;
	return NormalizeScancode(SDL_GetScancodeFromName(std::string(name).c_str()));
}

SpecialBinding SpecialFromName(std::string_view name) {
	if (name == "AnalogModifier") {
		return SpecialBinding::AnalogModifier;
	}
	if (name == "AnalogStepDown") {
		return SpecialBinding::AnalogStepDown;
	}
	if (name == "AnalogStepMiddle") {
		return SpecialBinding::AnalogStepMiddle;
	}
	if (name == "AnalogStepUp") {
		return SpecialBinding::AnalogStepUp;
	}
	if (name == "AnalogLock") {
		return SpecialBinding::AnalogLock;
	}
	if (name == "Gyro") {
		return SpecialBinding::Gyro;
	}
	return SpecialBinding::None;
}

SDL_Keycode NormalizeNumpadKey(SDL_Keycode key) {
	switch (key) {
		case SDLK_KP_MINUS: return SDLK_MINUS;
		case SDLK_KP_PLUS: return SDLK_PLUS;
		case SDLK_KP_EQUALS: return SDLK_EQUALS;
		default: return key;
	}
}

bool Conflicts(const Binding& first, const Binding& second) {
	return (first.key != SDLK_UNKNOWN && first.key == second.key) ||
	       (first.scancode != SDL_SCANCODE_UNKNOWN && first.scancode == second.scancode) ||
	       (first.mouse_button != 0 && first.mouse_button == second.mouse_button) ||
	       (first.wheel != 0 && first.wheel == second.wheel);
}

class InputMap {
public:
	InputMap() {
		// Debug: log keymap loading
		if (Config::InputDebugLogEnabled()) {
			LOGF("[KeymapDebug] Loading keymap with %zu entries\n", Config::GetKeymap().size());
			for (const auto& value: Config::GetKeymap()) {
				LOGF("[KeymapDebug]   Entry: %s\n", value.c_str());
			}
		}

		for (const auto& value: Config::GetKeymap()) {
			const std::string_view entry = value;
			if (entry.starts_with(MOUSE_SENSITIVITY)) {
				const float sensitivity =
				    std::strtof(value.c_str() + MOUSE_SENSITIVITY.size(), nullptr);
				m_mouse_sensitivity =
				    std::clamp(std::isfinite(sensitivity) ? sensitivity : 1.0f, 0.1f, 5.0f);
				continue;
			}
			const auto split = entry.find('=');

			Binding binding;
			if (split != std::string_view::npos) {
				const auto control_name = entry.substr(0, split);
				binding.control         = ControlFromName(control_name);
				binding.special         = SpecialFromName(control_name);
				const auto host_input   = entry.substr(split + 1);
				binding.mouse_button    = MouseButtonFromName(host_input);
				binding.wheel           = MouseWheelFromName(host_input);
				if (binding.mouse_button == 0 && binding.wheel == 0) {
					binding.scancode = ScancodeFromName(host_input);
					binding.key = NormalizeKey(SDL_GetKeyFromName(std::string(host_input).c_str()));
				}

				// Debug: log binding parsing
				if (Config::InputDebugLogEnabled()) {
					LOGF("[KeymapDebug] Parsed binding: control=%s, special=%d, key=%d, "
					     "scancode=%d\n",
					     std::string(control_name).c_str(), static_cast<int>(binding.special),
					     binding.key, static_cast<int>(binding.scancode));
				}
			}

			const bool reserved = binding.key == SDLK_ESCAPE || binding.key == SDLK_SPACE ||
			                      binding.key == SDLK_F1 || binding.key == SDLK_F7 ||
			                      binding.key == SDLK_F11;
			if ((binding.control == INVALID_CONTROL && binding.special == SpecialBinding::None) ||
			    reserved ||
			    (binding.key == SDLK_UNKNOWN && binding.scancode == SDL_SCANCODE_UNKNOWN &&
			     binding.mouse_button == 0 && binding.wheel == 0)) {
				EXIT("Invalid input mapping: %s\n", value.c_str());
			}
			Add(binding);
		}
	}
	[[nodiscard]] bool  Custom() const { return m_size != 0; }
	[[nodiscard]] float MouseSensitivity() const { return m_mouse_sensitivity; }

	[[nodiscard]] std::size_t FindKey(int key_code) const {
		key_code = NormalizeKey(static_cast<SDL_Keycode>(key_code));
		key_code = NormalizeNumpadKey(static_cast<SDL_Keycode>(key_code));
		const auto binding =
		    std::find_if(m_bindings.begin(), m_bindings.begin() + m_size,
		                 [key_code](const auto& item) { return item.key == key_code; });
		return binding != m_bindings.begin() + m_size ? binding->control : INVALID_CONTROL;
	}

	[[nodiscard]] std::size_t FindScancode(int scan_code) const {
		scan_code = static_cast<int>(NormalizeScancode(static_cast<SDL_Scancode>(scan_code)));
		const auto binding = std::find_if(
		    m_bindings.begin(), m_bindings.begin() + m_size, [scan_code](const auto& item) {
			    return item.scancode == static_cast<SDL_Scancode>(scan_code);
		    });
		return binding != m_bindings.begin() + m_size ? binding->control : INVALID_CONTROL;
	}

	[[nodiscard]] std::size_t FindMouseButton(uint8_t mouse_button) const {
		const auto binding = std::find_if(
		    m_bindings.begin(), m_bindings.begin() + m_size,
		    [mouse_button](const auto& item) { return item.mouse_button == mouse_button; });
		return binding != m_bindings.begin() + m_size ? binding->control : INVALID_CONTROL;
	}

	[[nodiscard]] std::size_t FindMouseWheel(int direction) const {
		const auto binding =
		    std::find_if(m_bindings.begin(), m_bindings.begin() + m_size,
		                 [direction](const auto& item) { return item.wheel == direction; });
		return binding != m_bindings.begin() + m_size ? binding->control : INVALID_CONTROL;
	}

	[[nodiscard]] SpecialBinding FindSpecial(int scan_code) const {
		scan_code = static_cast<int>(NormalizeScancode(static_cast<SDL_Scancode>(scan_code)));
		const auto binding = std::find_if(
		    m_bindings.begin(), m_bindings.begin() + m_size, [scan_code](const auto& item) {
			    return item.special != SpecialBinding::None &&
			           item.scancode == static_cast<SDL_Scancode>(scan_code);
		    });
		return binding != m_bindings.begin() + m_size ? binding->special : SpecialBinding::None;
	}

private:
	void Add(const Binding& binding) {
		for (std::size_t index = 0; index < m_size;) {
			if (Conflicts(m_bindings[index], binding)) {
				m_bindings[index] = m_bindings[--m_size];
			} else {
				index++;
			}
		}
		EXIT_IF(m_size >= m_bindings.size());
		m_bindings[m_size++] = binding;

		// Automatically add numpad equivalents for +/-/= (non-recursive)
		Binding numpad_binding = binding;
		if (binding.key == SDLK_MINUS) {
			numpad_binding.key = SDLK_KP_MINUS;
			// Check if numpad binding conflicts with existing bindings
			bool numpad_conflicts = false;
			for (std::size_t index = 0; index < m_size; ++index) {
				if (Conflicts(m_bindings[index], numpad_binding)) {
					numpad_conflicts = true;
					break;
				}
			}
			if (!numpad_conflicts) {
				EXIT_IF(m_size >= m_bindings.size());
				m_bindings[m_size++] = numpad_binding;
			}
		} else if (binding.key == SDLK_EQUALS || binding.key == SDLK_PLUS) {
			numpad_binding.key = (binding.key == SDLK_EQUALS) ? SDLK_KP_EQUALS : SDLK_KP_PLUS;
			// Check if numpad binding conflicts with existing bindings
			bool numpad_conflicts = false;
			for (std::size_t index = 0; index < m_size; ++index) {
				if (Conflicts(m_bindings[index], numpad_binding)) {
					numpad_conflicts = true;
					break;
				}
			}
			if (!numpad_conflicts) {
				EXIT_IF(m_size >= m_bindings.size());
				m_bindings[m_size++] = numpad_binding;
			}
		}
	}

	std::array<Binding, CONTROL_INFO.size() * 2> m_bindings {};
	std::size_t                                  m_size              = 0;
	float                                        m_mouse_sensitivity = 1.0f;
};

const InputMap& GetInputMap() {
	static const InputMap map;
	// Debug: log when GetInputMap is first called
	static bool logged = false;
	if (!logged && Config::InputDebugLogEnabled()) {
		LOGF("[KeymapDebug] GetInputMap called first time, map.Custom()=%s\n",
		     map.Custom() ? "true" : "false");
		logged = true;
	}
	return map;
}

void SetGyro(float value);
void SetMicrophone(uint8_t value);
void SetTriggerValue(Controller::Axis axis, uint8_t value);
void SetButton(uint32_t button, bool down);

void SetTriggerValue(Controller::Axis axis, uint8_t value) {
	if (axis == Controller::Axis::TriggerLeft) {
		g_l2_value = value;
		if (g_caps_lock) {
			g_l2_frozen_value = value;
		}
	} else {
		g_r2_value = value;
		if (g_caps_lock) {
			g_r2_frozen_value = value;
		}
	}
	{
		std::scoped_lock lock(g_diagnostics_mutex);
		if (axis == Controller::Axis::TriggerLeft) {
			g_diagnostics.left_trigger = static_cast<float>(value) / 255.0f;
		} else {
			g_diagnostics.right_trigger = static_cast<float>(value) / 255.0f;
		}
		g_diagnostics.revision++;
	}
	Controller::SetAxis(Controller::HOST_INPUT_CONTROLLER_ID, axis, value);
}

void SetButton(uint32_t button, bool down) {
	if (button == Controller::PAD_BUTTON_L2) {
		g_l2_down             = down;
		g_last_trigger_target = TriggerTarget::Left;
		if (g_analog_modifier) {
			g_l2_adjusting = down;
			if (down) {
				if (g_l2_value == 0) {
					SetTriggerValue(Controller::Axis::TriggerLeft, 255);
				}
			} else if (!g_caps_lock) {
				SetTriggerValue(Controller::Axis::TriggerLeft, 0);
			}
		} else {
			if (down) {
				SetTriggerValue(Controller::Axis::TriggerLeft, 255);
			} else {
				SetTriggerValue(Controller::Axis::TriggerLeft, g_caps_lock ? g_l2_frozen_value : 0);
			}
		}
	} else if (button == Controller::PAD_BUTTON_R2) {
		g_r2_down             = down;
		g_last_trigger_target = TriggerTarget::Right;
		if (g_analog_modifier) {
			g_r2_adjusting = down;
			if (down) {
				if (g_r2_value == 0) {
					SetTriggerValue(Controller::Axis::TriggerRight, 255);
				}
			} else if (!g_caps_lock) {
				SetTriggerValue(Controller::Axis::TriggerRight, 0);
			}
		} else {
			if (down) {
				SetTriggerValue(Controller::Axis::TriggerRight, 255);
			} else {
				SetTriggerValue(Controller::Axis::TriggerRight,
				                g_caps_lock ? g_r2_frozen_value : 0);
			}
		}
	} else if (button != 0) {
		Controller::SetButton(Controller::HOST_INPUT_CONTROLLER_ID, button, down);
	}
}

void ApplyAnalogStep(uint8_t step) {
	switch (g_analog_target) {
		case AnalogTarget::L2: SetTriggerValue(Controller::Axis::TriggerLeft, step); break;
		case AnalogTarget::R2: SetTriggerValue(Controller::Axis::TriggerRight, step); break;
		case AnalogTarget::Microphone: SetMicrophone(step); break;
		case AnalogTarget::Gyro: SetGyro(step > 128 ? 1.0f : (step < 128 ? -1.0f : 0.0f)); break;
		case AnalogTarget::None:
		default:
			// Do nothing when no target is selected
			break;
	}
}

void SetGyro(float value) {
	g_gyro_value = value;
	if (g_caps_lock) {
		g_gyro_frozen_value = value;
	}
	Controller::SetMotion(Controller::HOST_INPUT_CONTROLLER_ID, 0.0f, 0.0f, value);
	{
		std::scoped_lock lock(g_diagnostics_mutex);
		g_diagnostics.gyro        = std::abs(value);
		g_diagnostics.gyro_active = (value != 0.0f);
		g_diagnostics.revision++;
	}
	// Debug: log when gyro value changes
	if (Config::InputDebugLogEnabled()) {
		LOGF("[GyroDebug] SetGyro called with value: %.3f, gyro_active: %s\n", value,
		     g_diagnostics.gyro_active ? "true" : "false");
	}
}

void SetMicrophone(uint8_t value) {
	g_microphone_value = value;
	if (g_caps_lock) {
		g_microphone_frozen_value = value;
	}
	Controller::SetMicrophoneLevel(Controller::HOST_INPUT_CONTROLLER_ID, value);
	{
		std::scoped_lock lock(g_diagnostics_mutex);
		g_diagnostics.microphone = static_cast<float>(value) / 255.0f;
		g_diagnostics.revision++;
	}
}

void SetTouchPadAt(float x, float y, bool down);

void ResetAnalogInputs() {
	g_l2_down                 = false;
	g_r2_down                 = false;
	g_l2_adjusting            = false;
	g_r2_adjusting            = false;
	g_l2_value                = 0;
	g_r2_value                = 0;
	g_l2_frozen_value         = 0;
	g_r2_frozen_value         = 0;
	g_last_trigger_target     = TriggerTarget::None;
	g_gyro_down               = false;
	g_gyro_value              = 0.0f;
	g_gyro_frozen_value       = 0.0f;
	g_microphone_down         = false;
	g_microphone_value        = 0;
	g_microphone_frozen_value = 0;
	g_adjustment_active       = false;
	g_analog_target           = AnalogTarget::None;
	Controller::SetAxis(Controller::HOST_INPUT_CONTROLLER_ID, Controller::Axis::TriggerLeft, 0);
	Controller::SetAxis(Controller::HOST_INPUT_CONTROLLER_ID, Controller::Axis::TriggerRight, 0);
	Controller::SetMotion(Controller::HOST_INPUT_CONTROLLER_ID, 0.0f, 0.0f, 0.0f);
	Controller::SetMicrophoneLevel(Controller::HOST_INPUT_CONTROLLER_ID, 0);
	{
		std::scoped_lock lock(g_diagnostics_mutex);
		g_diagnostics.left_trigger  = 0.0f;
		g_diagnostics.right_trigger = 0.0f;
		g_diagnostics.microphone    = 0.0f;
		g_diagnostics.gyro          = 0.0f;
		g_diagnostics.gyro_active   = false;
		g_diagnostics.revision++;
	}
}

void ToggleAnalogLock() {
	g_caps_lock = !g_caps_lock;
	{
		std::scoped_lock lock(g_diagnostics_mutex);
		g_diagnostics.analog_lock = g_caps_lock;
		g_diagnostics.revision++;
	}
	if (g_caps_lock) {
		g_l2_frozen_value         = g_l2_value;
		g_r2_frozen_value         = g_r2_value;
		g_gyro_frozen_value       = g_gyro_value;
		g_microphone_frozen_value = g_microphone_value;
	} else {
		ResetAnalogInputs();
	}
}

void VirtualSwipe(uint32_t button) {
	SetTouchPadAt(0.5f, 0.5f, true);
	SetButton(button, true);
	SetButton(button, false);
	SetTouchPadAt(0.5f, 0.5f, false);
}

void SetTouchPad(float x, bool down) {
	Controller::SetTouchPad(Controller::HOST_INPUT_CONTROLLER_ID, 0, down, x, 0.5f);
}

void SetTouchPadAt(float x, float y, bool down) {
	Controller::SetTouchPad(Controller::HOST_INPUT_CONTROLLER_ID, 0, down,
	                        std::clamp(x, 0.0f, 1.0f), std::clamp(y, 0.0f, 1.0f));
}

bool HandleSpecialBinding(SpecialBinding binding, bool down) {
	if (binding == SpecialBinding::None) {
		return false;
	}
	if (binding == SpecialBinding::AnalogModifier) {
		g_analog_modifier = down;
		if (!down && !g_caps_lock) {
			g_l2_adjusting = false;
			g_r2_adjusting = false;
			if (!g_l2_down) {
				SetTriggerValue(Controller::Axis::TriggerLeft, 0);
			}
			if (!g_r2_down) {
				SetTriggerValue(Controller::Axis::TriggerRight, 0);
			}
		}
	} else if (binding == SpecialBinding::AnalogStepDown && down) {
		if (g_analog_modifier) {
			g_analog_step = 64;
			ApplyAnalogStep(64);
		}
	} else if (binding == SpecialBinding::AnalogStepMiddle && down) {
		if (g_analog_modifier) {
			g_analog_step = 128;
			ApplyAnalogStep(128);
		}
	} else if (binding == SpecialBinding::AnalogStepUp && down) {
		if (g_analog_modifier) {
			g_analog_step = 192;
			ApplyAnalogStep(192);
		}
	} else if (binding == SpecialBinding::AnalogLock && down) {
		ToggleAnalogLock();
	} else if (binding == SpecialBinding::Gyro) {
		g_gyro_down         = down;
		g_adjustment_active = down || g_analog_modifier;
		if (down) {
			g_analog_target = AnalogTarget::Gyro;
			// Toggle gyro: if already active, reset to 0; otherwise set small initial value
			if (g_gyro_value != 0.0f) {
				SetGyro(0.0f);
			} else {
				SetGyro(0.1f);
			}
		} else {
			g_analog_target = AnalogTarget::None;
		}
	}
	return true;
}

uint32_t DefaultKeyboardButton(int key_code) {
	switch (NormalizeKey(static_cast<SDL_Keycode>(key_code))) {
		case SDLK_UP: return Controller::PAD_BUTTON_UP;
		case SDLK_LEFT: return Controller::PAD_BUTTON_LEFT;
		case SDLK_DOWN: return Controller::PAD_BUTTON_DOWN;
		case SDLK_RIGHT: return Controller::PAD_BUTTON_RIGHT;
		case SDLK_j: return Controller::PAD_BUTTON_SQUARE;
		case SDLK_i: return Controller::PAD_BUTTON_TRIANGLE;
		case SDLK_k: return Controller::PAD_BUTTON_CROSS;
		case SDLK_l: return Controller::PAD_BUTTON_CIRCLE;
		case SDLK_q: return Controller::PAD_BUTTON_L1;
		case SDLK_e: return Controller::PAD_BUTTON_R1;
		case SDLK_DELETE: return Controller::PAD_BUTTON_PS;
		case SDLK_BACKSLASH: return Controller::PAD_BUTTON_CREATE;
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
	// Nota: caps lock NON blocca le levette (solo i valori analogici trigger/mic/gyro)
	int value = 128;
	if (negative && !positive) {
		value = 0;
	} else if (positive && !negative) {
		value = 255;
	}
	Controller::SetAxis(Controller::HOST_INPUT_CONTROLLER_ID, axis, value);
}

void SetControl(std::size_t control, bool down) {
	if (control == INVALID_CONTROL) {
		return;
	}

	const auto& info = CONTROL_INFO[control];
	// Nota: caps lock blocca solo L2/R2 (tramite SetButton), non le levette analogiche
	if (info.button == Controller::PAD_BUTTON_TOUCH_PAD) {
		SetTouchPad(info.touch_x, down);
		return;
	}
	if (info.button != 0) {
		SetButton(info.button, down);
		return;
	}

	const auto axis = static_cast<std::size_t>(info.axis);
	EXIT_IF(axis >= g_axis_state.size());
	if (info.positive) {
		g_axis_state[axis].positive = down;
	} else {
		g_axis_state[axis].negative = down;
	}
	SetStickAxis(info.axis, g_axis_state[axis].negative, g_axis_state[axis].positive);
}

void DefaultKeyboardInputLegacy(int key_code, bool down) {
	static StickKeys left;
	static StickKeys right;

	switch (NormalizeKey(static_cast<SDL_Keycode>(key_code))) {
		case SDLK_BACKSPACE: SetTouchPad(0.25f, down); return;
		case SDLK_TAB: SetTouchPad(0.75f, down); return;
		case SDLK_a:
			left.left = down;
			SetStickAxis(Controller::Axis::LeftX, left.left, left.right);
			return;
		case SDLK_d:
			left.right = down;
			SetStickAxis(Controller::Axis::LeftX, left.left, left.right);
			return;
		case SDLK_w:
			left.up = down;
			SetStickAxis(Controller::Axis::LeftY, left.up, left.down);
			return;
		case SDLK_s:
			left.down = down;
			SetStickAxis(Controller::Axis::LeftY, left.up, left.down);
			return;
		case SDLK_f:
			right.left = down;
			SetStickAxis(Controller::Axis::RightX, right.left, right.right);
			return;
		case SDLK_h:
			right.right = down;
			SetStickAxis(Controller::Axis::RightX, right.left, right.right);
			return;
		case SDLK_t:
			right.up = down;
			SetStickAxis(Controller::Axis::RightY, right.up, right.down);
			return;
		case SDLK_g:
			right.down = down;
			SetStickAxis(Controller::Axis::RightY, right.up, right.down);
			return;
		case SDLK_DELETE: SetButton(Controller::PAD_BUTTON_PS, down); return;
		case SDLK_BACKSLASH: SetButton(Controller::PAD_BUTTON_CREATE, down); return;
		default: SetButton(DefaultKeyboardButton(key_code), down); return;
	}
}

bool DefaultFallbackInput(int key_code, bool down) {
	static StickKeys right;

	switch (NormalizeKey(static_cast<SDL_Keycode>(key_code))) {
		case SDLK_h: g_swipe_mode = down; return true;
		case SDLK_m:
			g_microphone_down   = down;
			g_adjustment_active = down || g_analog_modifier;
			if (down) {
				g_analog_target = AnalogTarget::Microphone;
				SetMicrophone(g_microphone_value == 0 ? 255 : g_microphone_value);
			} else {
				g_analog_target = AnalogTarget::None;
				if (!g_caps_lock) {
					SetMicrophone(0);
				}
			}
			return true;
		case SDLK_SLASH:
			g_gyro_down         = down;
			g_adjustment_active = down || g_analog_modifier;
			if (down) {
				g_analog_target = AnalogTarget::Gyro;
				// Toggle gyro: if already active, reset to 0; otherwise set small initial value
				if (g_gyro_value != 0.0f) {
					SetGyro(0.0f);
				} else {
					SetGyro(0.1f);
				}
			} else {
				g_analog_target = AnalogTarget::None;
			}
			return true;
		case SDLK_y:
			if (g_swipe_mode && down) VirtualSwipe(Controller::PAD_BUTTON_UP);
			return g_swipe_mode;
		case SDLK_u:
			if (g_swipe_mode && down) VirtualSwipe(Controller::PAD_BUTTON_RIGHT);
			return g_swipe_mode;
		case SDLK_g:
			if (g_swipe_mode && down) VirtualSwipe(Controller::PAD_BUTTON_LEFT);
			return g_swipe_mode;
		case SDLK_b:
			if (g_swipe_mode && down) VirtualSwipe(Controller::PAD_BUTTON_DOWN);
			return g_swipe_mode;
		case SDLK_UP:
			if (g_swipe_mode && down) VirtualSwipe(Controller::PAD_BUTTON_UP);
			return g_swipe_mode;
		case SDLK_RIGHT:
			if (g_swipe_mode && down) VirtualSwipe(Controller::PAD_BUTTON_RIGHT);
			return g_swipe_mode;
		case SDLK_LEFT:
			if (g_swipe_mode && down) VirtualSwipe(Controller::PAD_BUTTON_LEFT);
			return g_swipe_mode;
		case SDLK_DOWN:
			if (g_swipe_mode && down) VirtualSwipe(Controller::PAD_BUTTON_DOWN);
			return g_swipe_mode;
		case SDLK_TAB:
			g_analog_modifier = down;
			if (!down && !g_caps_lock) {
				g_l2_adjusting  = false;
				g_r2_adjusting  = false;
				g_analog_target = AnalogTarget::None;
				if (!g_l2_down) SetTriggerValue(Controller::Axis::TriggerLeft, 0);
				if (!g_r2_down) SetTriggerValue(Controller::Axis::TriggerRight, 0);
			}
			return true;
		case SDLK_MINUS:
			if (down && g_analog_modifier) {
				g_analog_step = 64;
				ApplyAnalogStep(64);
			} else if (down && g_microphone_down) {
				SetMicrophone(64);
			} else if (down && g_gyro_down) {
				SetGyro(-0.25f);
			}
			return true;
		case SDLK_EQUALS:
			if (down && g_analog_modifier) {
				g_analog_step = 128;
				ApplyAnalogStep(128);
			} else if (down && g_microphone_down) {
				SetMicrophone(128);
			} else if (down && g_gyro_down) {
				SetGyro(0.0f);
			}
			return true;
		case SDLK_PLUS:
			if (down && g_analog_modifier) {
				g_analog_step = 192;
				ApplyAnalogStep(192);
			} else if (down && g_microphone_down) {
				SetMicrophone(192);
			} else if (down && g_gyro_down) {
				SetGyro(0.25f);
			}
			return true;
		case SDLK_q:
			if (g_analog_modifier) {
				g_l2_adjusting        = down;
				g_l2_down             = down;
				g_last_trigger_target = TriggerTarget::Left;
				if (down) {
					g_analog_target = AnalogTarget::L2;
					if (g_l2_value == 0) SetTriggerValue(Controller::Axis::TriggerLeft, 255);
				} else if (!g_caps_lock) {
					SetTriggerValue(Controller::Axis::TriggerLeft, 0);
				}
				return true;
			}
			SetButton(Controller::PAD_BUTTON_L1, down);
			return true;
		case SDLK_r:
			// Tab+R = seleziona L2 analogico (alternativa a Tab+Q)
			if (g_analog_modifier) {
				g_l2_adjusting        = down;
				g_l2_down             = down;
				g_last_trigger_target = TriggerTarget::Left;
				if (down) {
					g_analog_target = AnalogTarget::L2;
					if (g_l2_value == 0) SetTriggerValue(Controller::Axis::TriggerLeft, 255);
				} else if (!g_caps_lock) {
					SetTriggerValue(Controller::Axis::TriggerLeft, 0);
				}
				return true;
			}
			return false; // non gestito senza Tab: lascia ai profili normali
		case SDLK_e:
			if (g_analog_modifier) {
				g_r2_adjusting        = down;
				g_r2_down             = down;
				g_last_trigger_target = TriggerTarget::Right;
				if (down) {
					g_analog_target = AnalogTarget::R2;
					if (g_r2_value == 0) SetTriggerValue(Controller::Axis::TriggerRight, 255);
				} else if (!g_caps_lock) {
					SetTriggerValue(Controller::Axis::TriggerRight, 0);
				}
				return true;
			}
			SetButton(Controller::PAD_BUTTON_R1, down);
			return true;
		case SDLK_f:
			// Tab+F = seleziona R2 analogico (alternativa a Tab+E)
			if (g_analog_modifier) {
				g_r2_adjusting        = down;
				g_r2_down             = down;
				g_last_trigger_target = TriggerTarget::Right;
				if (down) {
					g_analog_target = AnalogTarget::R2;
					if (g_r2_value == 0) SetTriggerValue(Controller::Axis::TriggerRight, 255);
				} else if (!g_caps_lock) {
					SetTriggerValue(Controller::Axis::TriggerRight, 0);
				}
				return true;
			}
			return false; // non gestito senza Tab: lascia ai profili normali
		case SDLK_BACKSPACE: SetTouchPad(0.5f, down); return true;
		case SDLK_RETURN:
		case SDLK_RETURN2: SetButton(Controller::PAD_BUTTON_OPTIONS, down); return true;
		case SDLK_DELETE: SetButton(Controller::PAD_BUTTON_PS, down); return true;
		case SDLK_BACKSLASH: SetButton(Controller::PAD_BUTTON_CREATE, down); return true;
		case SDLK_1: SetButton(Controller::PAD_BUTTON_UP, down); return true;
		case SDLK_2: SetButton(Controller::PAD_BUTTON_LEFT, down); return true;
		case SDLK_3: SetButton(Controller::PAD_BUTTON_DOWN, down); return true;
		case SDLK_4: SetButton(Controller::PAD_BUTTON_RIGHT, down); return true;
		case SDLK_KP_8:
			right.up = down;
			SetStickAxis(Controller::Axis::RightY, right.up, right.down);
			return true;
		case SDLK_KP_2:
			right.down = down;
			SetStickAxis(Controller::Axis::RightY, right.up, right.down);
			return true;
		case SDLK_KP_4:
			right.left = down;
			SetStickAxis(Controller::Axis::RightX, right.left, right.right);
			return true;
		case SDLK_KP_6:
			right.right = down;
			SetStickAxis(Controller::Axis::RightX, right.left, right.right);
			return true;
		case SDLK_z: SetButton(Controller::PAD_BUTTON_SQUARE, down); return true;
		case SDLK_x: SetButton(Controller::PAD_BUTTON_CIRCLE, down); return true;
		case SDLK_c: SetButton(Controller::PAD_BUTTON_TRIANGLE, down); return true;
		case SDLK_LSHIFT: SetButton(Controller::PAD_BUTTON_CROSS, down); return true;
		default: return false;
	}
}

void DefaultKeyboardInput(int key_code, bool down) {
	const auto& profile = Config::GetInputProfile();
	if (DefaultFallbackInput(key_code, down)) {
		return;
	}
	if (profile == "legacy") {
		DefaultKeyboardInputLegacy(key_code, down);
		return;
	}

	static StickKeys left;
	static StickKeys right;

	switch (NormalizeKey(static_cast<SDL_Keycode>(key_code))) {
		case SDLK_UP: SetButton(Controller::PAD_BUTTON_UP, down); return;
		case SDLK_DOWN: SetButton(Controller::PAD_BUTTON_DOWN, down); return;
		case SDLK_LEFT: SetButton(Controller::PAD_BUTTON_LEFT, down); return;
		case SDLK_RIGHT: SetButton(Controller::PAD_BUTTON_RIGHT, down); return;
		case SDLK_a:
			left.left = down;
			SetStickAxis(Controller::Axis::LeftX, left.left, left.right);
			return;
		case SDLK_d:
			left.right = down;
			SetStickAxis(Controller::Axis::LeftX, left.left, left.right);
			return;
		case SDLK_w:
			left.up = down;
			SetStickAxis(Controller::Axis::LeftY, left.up, left.down);
			return;
		case SDLK_s:
			left.down = down;
			SetStickAxis(Controller::Axis::LeftY, left.up, left.down);
			return;
		case SDLK_t:
			right.up = down;
			SetStickAxis(Controller::Axis::RightY, right.up, right.down);
			return;
		case SDLK_g:
			right.down = down;
			SetStickAxis(Controller::Axis::RightY, right.up, right.down);
			return;
		case SDLK_f:
			right.left = down;
			SetStickAxis(Controller::Axis::RightX, right.left, right.right);
			return;
		case SDLK_h:
			right.right = down;
			SetStickAxis(Controller::Axis::RightX, right.left, right.right);
			return;
		case SDLK_q:
			if (g_analog_modifier) {
				g_l2_adjusting        = down;
				g_l2_down             = down;
				g_last_trigger_target = TriggerTarget::Left;
				if (down) {
					g_analog_target = AnalogTarget::L2;
					if (g_l2_value == 0) SetTriggerValue(Controller::Axis::TriggerLeft, 255);
				} else if (!g_caps_lock) {
					SetTriggerValue(Controller::Axis::TriggerLeft, 0);
				}
				return;
			}
			SetButton(Controller::PAD_BUTTON_L1, down);
			return;
		case SDLK_e:
			if (g_analog_modifier) {
				g_r2_adjusting        = down;
				g_r2_down             = down;
				g_last_trigger_target = TriggerTarget::Right;
				if (down) {
					g_analog_target = AnalogTarget::R2;
					if (g_r2_value == 0) SetTriggerValue(Controller::Axis::TriggerRight, 255);
				} else if (!g_caps_lock) {
					SetTriggerValue(Controller::Axis::TriggerRight, 0);
				}
				return;
			}
			SetButton(Controller::PAD_BUTTON_R1, down);
			return;
		case SDLK_i: SetButton(Controller::PAD_BUTTON_TRIANGLE, down); return;
		case SDLK_j: SetButton(Controller::PAD_BUTTON_SQUARE, down); return;
		case SDLK_k: SetButton(Controller::PAD_BUTTON_CROSS, down); return;
		case SDLK_l: SetButton(Controller::PAD_BUTTON_CIRCLE, down); return;
		case SDLK_BACKSPACE: SetTouchPad(0.25f, down); return;
		case SDLK_TAB:
			g_analog_modifier = down;
			if (!down && !g_caps_lock) {
				g_l2_adjusting  = false;
				g_r2_adjusting  = false;
				g_analog_target = AnalogTarget::None;
				if (!g_l2_down) SetTriggerValue(Controller::Axis::TriggerLeft, 0);
				if (!g_r2_down) SetTriggerValue(Controller::Axis::TriggerRight, 0);
			}
			return;
		case SDLK_RETURN:
		case SDLK_RETURN2: SetButton(Controller::PAD_BUTTON_OPTIONS, down); return;
		case SDLK_DELETE: SetButton(Controller::PAD_BUTTON_PS, down); return;
		case SDLK_BACKSLASH: SetButton(Controller::PAD_BUTTON_CREATE, down); return;
		default: SetButton(DefaultKeyboardButton(key_code), down); return;
	}
}

void MouseToJoystick(int delta_x, int delta_y) {
	const double distance = std::hypot(delta_x, delta_y);
	const double scale =
	    std::clamp(distance * GetInputMap().MouseSensitivity() + 16.0, 64.0, 128.0) / distance;
	const auto map_axis = [scale](int delta) {
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
	if (SDL_SetRelativeMouseMode(enabled ? SDL_TRUE : SDL_FALSE) == 0) {
		return true;
	}
	LOGF("Mouse-to-joystick relative mode failed: %s\n", SDL_GetError());
	return false;
}
} // namespace

void HostInputInit() {
	{
		std::scoped_lock lock(g_diagnostics_mutex);
		g_diagnostics.capabilities = 0x01u | 0x02u | 0x04u;
		if (SDL_GetNumTouchDevices() > 0) {
			g_diagnostics.capabilities |= 0x08u;
		}
	}
	GetInputMap();
}

void HostInputKey(int key_code, bool down) {
	HostInputScancode(SDL_GetScancodeFromKey(static_cast<SDL_Keycode>(key_code)), key_code, down);
}

void HostInputScancode(int scan_code, int key_code, bool down) {
	{
		std::scoped_lock lock(g_diagnostics_mutex);
		g_diagnostics.source = HostInputSource::Keyboard;
		g_diagnostics.revision++;
	}
	const auto  normalized_scan = NormalizeScancode(static_cast<SDL_Scancode>(scan_code));
	const auto& map             = GetInputMap();

	if (Config::InputDebugLogEnabled()) {
		const char* key_name  = SDL_GetKeyName(static_cast<SDL_Keycode>(key_code));
		const char* scan_name = SDL_GetScancodeName(static_cast<SDL_Scancode>(scan_code));
		LOGF("[InputDebug] Key %s (%s / scan=%d) %s, map.Custom()=%s\n", key_name, scan_name,
		     scan_code, down ? "DOWN" : "UP", map.Custom() ? "true" : "false");
	}

	if (HandleSpecialBinding(map.FindSpecial(static_cast<int>(normalized_scan)), down)) {
		if (Config::InputDebugLogEnabled()) {
			LOGF("[InputDebug]  -> Special binding handled\n");
		}
		return;
	}
	if (normalized_scan == SDL_SCANCODE_CAPSLOCK) {
		if (down) {
			ToggleAnalogLock();
			if (Config::InputDebugLogEnabled()) {
				LOGF("[InputDebug]  -> CapsLock: AnalogLock toggled -> %s\n",
				     g_caps_lock ? "ON" : "OFF");
			}
		}
		return;
	}

	if (map.Custom()) {
		const auto control = map.FindScancode(static_cast<int>(normalized_scan));
		if (control != INVALID_CONTROL) {
			if (Config::InputDebugLogEnabled()) {
				LOGF("[InputDebug]  -> Custom scancode mapping -> %s\n",
				     CONTROL_INFO[control].name.data());
			}
			SetControl(control, down);
			return;
		}
		const auto key_control = map.FindKey(key_code);
		if (key_control != INVALID_CONTROL) {
			if (Config::InputDebugLogEnabled()) {
				LOGF("[InputDebug]  -> Custom key mapping -> %s\n",
				     CONTROL_INFO[key_control].name.data());
			}
			SetControl(key_control, down);
			return;
		}
		if (Config::InputDebugLogEnabled()) {
			LOGF("[InputDebug]  -> DefaultFallbackInput (custom profile, no binding found)\n");
		}
		DefaultFallbackInput(key_code, down);
		return;
	}
	if (Config::InputDebugLogEnabled()) {
		LOGF("[InputDebug]  -> DefaultKeyboardInput\n");
	}
	DefaultKeyboardInput(key_code, down);
}

[[nodiscard]] bool HostInputCapsLockEnabled() {
	return g_caps_lock;
}

void HostInputMouseButton(uint8_t mouse_button, bool down) {
	{
		std::scoped_lock lock(g_diagnostics_mutex);
		g_diagnostics.source = HostInputSource::Mouse;
		g_diagnostics.revision++;
	}
	const auto& map = GetInputMap();
	if (!map.Custom()) {
		switch (mouse_button) {
			case SDL_BUTTON_RIGHT: SetButton(Controller::PAD_BUTTON_L2, down); return;
			case SDL_BUTTON_LEFT: SetButton(Controller::PAD_BUTTON_R2, down); return;
			default: break;
		}
	}
	if (map.Custom() && mouse_button != 0) {
		const auto control = map.FindMouseButton(mouse_button);
		if (control != INVALID_CONTROL) {
			SetControl(control, down);
			return;
		}
		if (mouse_button == SDL_BUTTON_RIGHT) {
			SetButton(Controller::PAD_BUTTON_L2, down);
		} else if (mouse_button == SDL_BUTTON_LEFT) {
			SetButton(Controller::PAD_BUTTON_R2, down);
		}
	}
}

void HostInputMouseWheel(int direction) {
	{
		std::scoped_lock lock(g_diagnostics_mutex);
		g_diagnostics.source = HostInputSource::Mouse;
		g_diagnostics.revision++;
	}
	const auto& map = GetInputMap();
	if (g_microphone_down) {
		const int min_mic  = g_caps_lock ? 1 : 0;
		const int next_mic = std::clamp(
		    static_cast<int>(g_microphone_value) + (direction > 0 ? 25 : -25), min_mic, 255);
		SetMicrophone(static_cast<uint8_t>(next_mic));
		return;
	}
	if (g_gyro_down) {
		const float delta     = direction > 0 ? 0.25f : -0.25f;
		const float next_gyro = std::clamp(g_gyro_value + delta, -1.0f, 1.0f);
		SetGyro(next_gyro);
		return;
	}
	if (g_analog_modifier) {
		const int min_val  = g_caps_lock ? 1 : 0;
		const int delta    = direction > 0 ? 25 : -25;
		bool      adjusted = false;

		const bool adjust_l2 =
		    g_l2_down || g_l2_adjusting ||
		    (g_last_trigger_target == TriggerTarget::Left && !g_r2_down && !g_r2_adjusting);
		const bool adjust_r2 =
		    g_r2_down || g_r2_adjusting ||
		    (g_last_trigger_target == TriggerTarget::Right && !g_l2_down && !g_l2_adjusting);

		if (adjust_l2) {
			const int current  = static_cast<int>(g_l2_value);
			const int next_val = std::clamp(current + delta, min_val, 255);
			SetTriggerValue(Controller::Axis::TriggerLeft, static_cast<uint8_t>(next_val));
			adjusted = true;
		}
		if (adjust_r2) {
			const int current  = static_cast<int>(g_r2_value);
			const int next_val = std::clamp(current + delta, min_val, 255);
			SetTriggerValue(Controller::Axis::TriggerRight, static_cast<uint8_t>(next_val));
			adjusted = true;
		}
		if (!adjusted) {
			const int current  = static_cast<int>(g_l2_value);
			const int next_val = std::clamp(current + delta, min_val, 255);
			SetTriggerValue(Controller::Axis::TriggerLeft, static_cast<uint8_t>(next_val));
		}
		return;
	}
	if (!map.Custom()) {
		return;
	}
	const auto control = map.FindMouseWheel(direction > 0 ? 1 : -1);
	if (control != INVALID_CONTROL) {
		SetControl(control, true);
		SetControl(control, false);
	}
}

void HostInputFinger(bool down, bool up, bool motion, float x, float y, float dx, float dy) {
	{
		std::scoped_lock lock(g_diagnostics_mutex);
		g_diagnostics.source = HostInputSource::Touch;
		g_diagnostics.revision++;
	}
	if (down) {
		g_touch_active    = true;
		g_touch_direction = 0;
		SetTouchPadAt(x, y, true);
		return;
	}
	if (up) {
		g_touch_active = false;
		if (g_touch_direction != 0) {
			SetButton(g_touch_direction, false);
			g_touch_direction = 0;
		}
		SetTouchPadAt(x, y, false);
		return;
	}
	if (!motion || !g_touch_active) {
		return;
	}
	if (g_swipe_mode) {
		if (g_touch_direction != 0) {
			SetButton(g_touch_direction, false);
			g_touch_direction = 0;
		}
		SetTouchPadAt(x, y, true);
		return;
	}
	uint32_t direction = 0;
	if (std::abs(dx) >= std::abs(dy) && std::abs(dx) > 0.02f) {
		direction = dx > 0.0f ? Controller::PAD_BUTTON_RIGHT : Controller::PAD_BUTTON_LEFT;
	} else if (std::abs(dy) > 0.02f) {
		direction = dy > 0.0f ? Controller::PAD_BUTTON_DOWN : Controller::PAD_BUTTON_UP;
	}
	if (direction != g_touch_direction) {
		if (g_touch_direction != 0) {
			SetButton(g_touch_direction, false);
		}
		g_touch_direction = direction;
		if (g_touch_direction != 0) {
			SetButton(g_touch_direction, true);
		}
	}
}

HostInputDiagnostics HostInputGetDiagnostics() {
	std::scoped_lock lock(g_diagnostics_mutex);
	auto             result  = g_diagnostics;
	result.analog_lock       = g_caps_lock;
	result.adjustment_active = g_analog_modifier || g_l2_adjusting || g_r2_adjusting ||
	                           g_gyro_down || g_microphone_down || g_adjustment_active;
	return result;
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
	int ignored_x = 0;
	int ignored_y = 0;
	SDL_GetRelativeMouseState(&ignored_x, &ignored_y);
	g_mouse.enabled   = true;
	g_mouse.next_poll = SDL_GetTicks64() + MOUSE_POLL_INTERVAL_MS;
	LOGF("Mouse to right stick: enabled (F7 to release)\n");
}

int PollMouse(uint64_t now_ms) {
	if (now_ms < g_mouse.next_poll) {
		return static_cast<int>(g_mouse.next_poll - now_ms);
	}
	g_mouse.next_poll = now_ms + MOUSE_POLL_INTERVAL_MS;

	int delta_x = 0;
	int delta_y = 0;
	SDL_GetRelativeMouseState(&delta_x, &delta_y);
	if (delta_x == 0 && delta_y == 0) {
		CenterMouseStick();
		return MOUSE_POLL_INTERVAL_MS;
	}

	MouseToJoystick(delta_x, delta_y);
	g_mouse.output = true;
	return MOUSE_POLL_INTERVAL_MS;
}

bool HostInputWaitEvent(SDL_Event* event) {
	if (!g_mouse.enabled || SDL_GetKeyboardFocus() == nullptr) {
		CenterMouseStick();
		if (SDL_WaitEvent(event) == 0) {
			EXIT("%s\n", SDL_GetError());
		}
		return true;
	}

	const int timeout_ms = PollMouse(SDL_GetTicks64());
	SDL_ClearError();
	if (SDL_WaitEventTimeout(event, timeout_ms) != 0) {
		return true;
	}
	if (SDL_GetError()[0] != '\0') {
		EXIT("%s\n", SDL_GetError());
	}
	return false;
}

} // namespace Libs::Graphics
