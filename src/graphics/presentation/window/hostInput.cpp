#include "graphics/presentation/window/hostInput.h"

#include "SDL_keyboard.h"
#include "SDL_keycode.h"
#include "SDL_mouse.h"
#include "common/assert.h"
#include "common/emulatorConfig.h"
#include "libs/controller.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

namespace Libs::Graphics {

namespace {

struct Binding {
	SDL_Keycode key          = SDLK_UNKNOWN;
	uint8_t     mouse_button = 0;
	uint32_t    pad_button   = 0;
};

static constexpr std::array PAD_BUTTONS = {
    std::pair {std::string_view("L3"), Controller::PAD_BUTTON_L3},
    std::pair {std::string_view("R3"), Controller::PAD_BUTTON_R3},
    std::pair {std::string_view("Options"), Controller::PAD_BUTTON_OPTIONS},
    std::pair {std::string_view("Up"), Controller::PAD_BUTTON_UP},
    std::pair {std::string_view("Right"), Controller::PAD_BUTTON_RIGHT},
    std::pair {std::string_view("Down"), Controller::PAD_BUTTON_DOWN},
    std::pair {std::string_view("Left"), Controller::PAD_BUTTON_LEFT},
    std::pair {std::string_view("L2"), Controller::PAD_BUTTON_L2},
    std::pair {std::string_view("R2"), Controller::PAD_BUTTON_R2},
    std::pair {std::string_view("L1"), Controller::PAD_BUTTON_L1},
    std::pair {std::string_view("R1"), Controller::PAD_BUTTON_R1},
    std::pair {std::string_view("Triangle"), Controller::PAD_BUTTON_TRIANGLE},
    std::pair {std::string_view("Circle"), Controller::PAD_BUTTON_CIRCLE},
    std::pair {std::string_view("Cross"), Controller::PAD_BUTTON_CROSS},
    std::pair {std::string_view("Square"), Controller::PAD_BUTTON_SQUARE},
    std::pair {std::string_view("TouchPad"), Controller::PAD_BUTTON_TOUCH_PAD},
};

uint32_t PadButtonFromName(std::string_view name) {
	const auto button = std::find_if(PAD_BUTTONS.begin(), PAD_BUTTONS.end(),
	                                 [name](const auto& item) { return item.first == name; });
	return button != PAD_BUTTONS.end() ? button->second : 0;
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
	return first.pad_button == second.pad_button ||
	       (first.key != SDLK_UNKNOWN && first.key == second.key) ||
	       (first.mouse_button != 0 && first.mouse_button == second.mouse_button);
}

class InputMap {
public:
	InputMap(): m_custom(!Config::GetKeymap().empty()) {
		for (const auto& value: Config::GetKeymap()) {
			const std::string_view entry = value;
			const auto             split = entry.find('=');

			Binding binding;
			if (split != std::string_view::npos) {
				binding.pad_button    = PadButtonFromName(entry.substr(0, split));
				const auto host_input = entry.substr(split + 1);
				binding.mouse_button  = MouseButtonFromName(host_input);
				if (binding.mouse_button == 0) {
					binding.key = NormalizeKey(SDL_GetKeyFromName(std::string(host_input).c_str()));
				}
			}

			// ESC still reserved (quit). Space is jump in PC layout — pause moved to P.
			const bool reserved = binding.key == SDLK_ESCAPE || binding.key == SDLK_F1;
			if (binding.pad_button == 0 || reserved ||
			    (binding.key == SDLK_UNKNOWN && binding.mouse_button == 0)) {
				EXIT("Invalid input mapping: %s\n", value.c_str());
			}
			Add(binding);
		}
	}
	[[nodiscard]] bool Custom() const { return m_custom; }

	[[nodiscard]] uint32_t FindKey(int key_code) const {
		key_code = NormalizeKey(static_cast<SDL_Keycode>(key_code));
		const auto binding =
		    std::find_if(m_bindings.begin(), m_bindings.begin() + m_size,
		                 [key_code](const auto& item) { return item.key == key_code; });
		return binding != m_bindings.begin() + m_size ? binding->pad_button : 0;
	}

	[[nodiscard]] uint32_t FindMouseButton(uint8_t mouse_button) const {
		const auto binding = std::find_if(
		    m_bindings.begin(), m_bindings.begin() + m_size,
		    [mouse_button](const auto& item) { return item.mouse_button == mouse_button; });
		return binding != m_bindings.begin() + m_size ? binding->pad_button : 0;
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
	}

	std::array<Binding, PAD_BUTTONS.size()> m_bindings {};
	std::size_t                             m_size = 0;
	bool                                    m_custom;
};

const InputMap& GetInputMap() {
	static const InputMap map;
	return map;
}

void SetButton(uint32_t button, bool down) {
	if (button == Controller::PAD_BUTTON_L2) {
		Controller::ControllerAxis(Controller::HOST_INPUT_CONTROLLER_ID,
		                           Controller::Axis::TriggerLeft, down ? 255 : 0);
	} else if (button == Controller::PAD_BUTTON_R2) {
		Controller::ControllerAxis(Controller::HOST_INPUT_CONTROLLER_ID,
		                           Controller::Axis::TriggerRight, down ? 255 : 0);
	} else if (button != 0) {
		Controller::ControllerButton(Controller::HOST_INPUT_CONTROLLER_ID, button, down);
	}
}

// PC Minecraft-style face/triggers → DualSense used by PS5 Minecraft.
uint32_t DefaultKeyboardButton(int key_code) {
	switch (NormalizeKey(static_cast<SDL_Keycode>(key_code))) {
		case SDLK_UP: return Controller::PAD_BUTTON_UP;
		case SDLK_LEFT: return Controller::PAD_BUTTON_LEFT;
		case SDLK_DOWN: return Controller::PAD_BUTTON_DOWN;
		case SDLK_RIGHT: return Controller::PAD_BUTTON_RIGHT;
		case SDLK_SPACE: return Controller::PAD_BUTTON_CROSS;      // jump
		case SDLK_q: return Controller::PAD_BUTTON_CIRCLE;         // drop
		case SDLK_e: return Controller::PAD_BUTTON_TOUCH_PAD;      // inventory
		case SDLK_c:
		case SDLK_f: return Controller::PAD_BUTTON_SQUARE;         // craft / use alt
		case SDLK_LSHIFT: return Controller::PAD_BUTTON_R3;        // sneak
		case SDLK_LCTRL: return Controller::PAD_BUTTON_L3;         // sprint-click / stick click
		case SDLK_TAB: return Controller::PAD_BUTTON_TRIANGLE;     // perspective-ish
		case SDLK_RETURN:
		case SDLK_RETURN2: return Controller::PAD_BUTTON_OPTIONS;  // menu/pause game
		case SDLK_COMMA:
		case SDLK_LEFTBRACKET: return Controller::PAD_BUTTON_L1;   // hotbar left
		case SDLK_PERIOD:
		case SDLK_RIGHTBRACKET: return Controller::PAD_BUTTON_R1;  // hotbar right
		// Legacy IJKL face (still available)
		case SDLK_j: return Controller::PAD_BUTTON_CROSS;
		case SDLK_i: return Controller::PAD_BUTTON_TRIANGLE;
		case SDLK_k: return Controller::PAD_BUTTON_SQUARE;
		case SDLK_l: return Controller::PAD_BUTTON_CIRCLE;
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
	const int value = negative == positive ? 128 : negative ? 0 : 255;
	Controller::ControllerAxis(Controller::HOST_INPUT_CONTROLLER_ID, axis, value);
}

void ApplyAxisValue(Controller::Axis axis, int value) {
	value = std::clamp(value, 0, 255);
	Controller::ControllerAxis(Controller::HOST_INPUT_CONTROLLER_ID, axis, value);
}

struct MouseLookState {
	float    look_x        = 0.0f; // -1..1
	float    look_y        = 0.0f;
	uint32_t idle_frames   = 0;
	bool     keyboard_look = false; // arrow-keys optional look blocks mouse decay
};

MouseLookState& LookState() {
	static MouseLookState state;
	return state;
}

void PublishLookStick() {
	auto& s = LookState();
	// DualSense right stick: X right positive, Y typically up = low values on hosts
	const int x = static_cast<int>(std::lround(128.0f + s.look_x * 127.0f));
	const int y = static_cast<int>(std::lround(128.0f + s.look_y * 127.0f));
	ApplyAxisValue(Controller::Axis::RightX, x);
	ApplyAxisValue(Controller::Axis::RightY, y);
}

void DefaultKeyboardInput(int key_code, bool down) {
	static StickKeys left;

	switch (NormalizeKey(static_cast<SDL_Keycode>(key_code))) {
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
		default: SetButton(DefaultKeyboardButton(key_code), down); return;
	}
}

uint32_t DefaultMouseButton(uint8_t mouse_button) {
	switch (mouse_button) {
		case SDL_BUTTON_LEFT: return Controller::PAD_BUTTON_L2;   // attack / break
		case SDL_BUTTON_RIGHT: return Controller::PAD_BUTTON_R2;  // use / place
		case SDL_BUTTON_MIDDLE: return Controller::PAD_BUTTON_TOUCH_PAD;
		default: return 0;
	}
}

} // namespace

void HostInputInit() {
	GetInputMap();
}

void HostInputKey(int key_code, bool down) {
	const auto& map = GetInputMap();
	if (map.Custom()) {
		const auto pad = map.FindKey(key_code);
		if (pad != 0) {
			SetButton(pad, down);
			return;
		}
	}
	DefaultKeyboardInput(key_code, down);
}

void HostInputMouseButton(uint8_t mouse_button, bool down) {
	const auto& map = GetInputMap();
	if (map.Custom() && mouse_button != 0) {
		const auto mapped = map.FindMouseButton(mouse_button);
		if (mapped != 0) {
			SetButton(mapped, down);
			return;
		}
	}
	SetButton(DefaultMouseButton(mouse_button), down);
}

void HostInputMouseMotion(int dx, int dy) {
	if (dx == 0 && dy == 0) {
		return;
	}
	auto& s = LookState();
	// Original feel: accumulate mouse deltas into virtual right stick.
	constexpr float kSens = 0.035f;
	s.look_x              = std::clamp(s.look_x + static_cast<float>(dx) * kSens, -1.0f, 1.0f);
	s.look_y              = std::clamp(s.look_y + static_cast<float>(dy) * kSens, -1.0f, 1.0f);
	s.idle_frames         = 0;
	PublishLookStick();
}

void HostInputFrame() {
	auto& s = LookState();
	if (s.keyboard_look) {
		return;
	}
	s.idle_frames++;
	// Soft decay after a short idle (first working version — small lag, smooth feel).
	if (s.idle_frames < 2) {
		return;
	}
	constexpr float kDecay = 0.55f;
	s.look_x *= kDecay;
	s.look_y *= kDecay;
	if (std::fabs(s.look_x) < 0.02f && std::fabs(s.look_y) < 0.02f) {
		s.look_x = 0.0f;
		s.look_y = 0.0f;
	}
	PublishLookStick();
}

} // namespace Libs::Graphics
