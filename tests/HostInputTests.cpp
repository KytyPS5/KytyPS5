// Exercise production host input with deterministic gamepads and guest output.
#include <SDL3/SDL.h>
#include <array>
#include <cstdio>
#include <cstdlib>

namespace {
void Check(bool condition, const char* message) {
	if (!condition) {
		std::fprintf(stderr, "HostInputTests: %s\n", message);
		std::abort();
	}
}

std::array<Sint16, 2> physical_stick {};
std::array<int, 2>    guest_stick {128, 128};
uint32_t             guest_buttons = 0;
bool                 guest_touch   = false;
} // namespace

namespace Fake {
SDL_Gamepad* GetGamepadFromID(SDL_JoystickID id) {
	return id == 1 || id == 2 ? reinterpret_cast<SDL_Gamepad*>(static_cast<uintptr_t>(id))
	                          : nullptr;
}
Sint16 GetGamepadAxis(SDL_Gamepad*, SDL_GamepadAxis axis) {
	return physical_stick[axis == SDL_GAMEPAD_AXIS_LEFTY ? 1 : 0];
}
Uint64 GetTicks() {
	return 1000;
}
bool HideCursor() {
	return true;
}
bool ShowCursor() {
	return true;
}
SDL_MouseButtonFlags GetMouseState(float* x, float* y) {
	*x = *y = 0.0f;
	return 0;
}
} // namespace Fake

#define SDL_GetGamepadFromID Fake::GetGamepadFromID
#define SDL_GetGamepadAxis   Fake::GetGamepadAxis
#define SDL_GetTicks         Fake::GetTicks
#define SDL_HideCursor       Fake::HideCursor
#define SDL_ShowCursor       Fake::ShowCursor
#define SDL_GetMouseState    Fake::GetMouseState
#include "graphics/presentation/window/hostInput.cpp"
#undef SDL_GetGamepadFromID
#undef SDL_GetGamepadAxis
#undef SDL_GetTicks
#undef SDL_HideCursor
#undef SDL_ShowCursor
#undef SDL_GetMouseState

namespace Libs::Controller {
void SetButton(int id, uint32_t button, bool down) {
	if (id == 1) {
		guest_buttons = down ? guest_buttons | button : guest_buttons & ~button;
	}
}
void SetAxis(int id, Axis axis, int value) {
	if (id == 1 && (axis == Axis::LeftX || axis == Axis::LeftY)) {
		guest_stick[axis == Axis::LeftY ? 1 : 0] = value;
	}
}
void SetTouchPad(int id, int finger, bool down, float, float) {
	if (id == 1 && finger == 1) {
		guest_touch = down;
	}
}
void SetRightStick(int, int, int) {}
void ToggleGyroEmulation(int) {}
void CycleSetting(Setting) {}
} // namespace Libs::Controller

namespace {
using namespace Libs::Graphics;
using namespace Libs::Controller;

struct Inputs {
	Inputs() {
		physical_stick = {};
		guest_stick    = {128, 128};
		guest_buttons  = 0;
		guest_touch    = false;
		HostInputInit(nullptr);
	}
	~Inputs() { HostInputShutdown(); }
};

void TestTouchPadStickHandoff() {
	Inputs inputs;
	physical_stick = {SDL_JOYSTICK_AXIS_MAX, SDL_JOYSTICK_AXIS_MIN};
	guest_stick    = {255, 0};
	HostInputToggleGamepadTouchPad(1);
	Check(guest_stick == std::array {128, 128}, "entering touchpad mode retained guest movement");
	PollGamepadTouchPad(1008);
	const auto cursor = GetHostInputTouchPadVisualState();
	Check(cursor.x > 0.5f && cursor.y < 0.5f,
	      "entering touchpad mode did not sample an already-held stick");
	Check(HostInputGamepadAxis(1, true, 127) && HostInputGamepadAxis(1, false, 127),
	      "touchpad mode did not capture stick releases");
	Check(guest_stick == std::array {128, 128}, "touchpad movement leaked to guest axes");
	// Sample current physical axes on exit even if an event was consumed elsewhere.
	physical_stick = {SDL_JOYSTICK_AXIS_MIN, SDL_JOYSTICK_AXIS_MAX};
	HostInputToggleGamepadTouchPad(1);
	Check(guest_stick == std::array {0, 255}, "leaving touchpad mode did not restore current axes");
	Check(!HostInputGamepadAxis(1, true, 127), "leaving touchpad mode retained stick capture");
}

void TestKeyboardSwitchesFromGamepadTouchPad() {
	Inputs inputs;
	physical_stick = {SDL_JOYSTICK_AXIS_MIN, SDL_JOYSTICK_AXIS_MAX};
	HostInputToggleGamepadTouchPad(1);
	Check(HostInputGamepadDrawButton(1, true) && guest_touch, "gamepad drawing did not start");
	HostInputKey(SDLK_F6, true);
	Check(GetHostInputTouchPadVisualState().enabled && !HostInputGamepadAxis(1, true, 127),
	      "F6 failed to switch from gamepad to mouse touchpad mode");
	Check(!guest_touch && guest_stick == std::array {0, 255},
	      "switching to mouse touchpad mode retained gamepad input capture");
	HostInputKey(SDLK_F6, false);
	HostInputKey(SDLK_F6, true);
	Check(!GetHostInputTouchPadVisualState().enabled, "F6 failed to disable mouse touchpad mode");
}

void TestIndependentTriggerEdges() {
	Inputs inputs;
	HostInputPadTrigger(2, true, 255);
	HostInputPadTrigger(1, true, 255);
	Check(guest_buttons == PAD_BUTTON_CROSS, "an inactive pad suppressed the active trigger press");
	HostInputPadTrigger(2, true, 0);
	HostInputPadTrigger(1, true, 0);
	Check(guest_buttons == 0, "an inactive pad suppressed the active trigger release");
	HostInputPadTrigger(1, false, 255);
	HostInputPadTrigger(1, true, 255);
	HostInputPadTrigger(1, true, 0);
	Check(guest_buttons == PAD_BUTTON_CIRCLE, "left and right trigger edges were mixed");
	HostInputPadTrigger(1, false, 0);
	Check(guest_buttons == 0, "right trigger release was lost");
}

void TestTriggerLifecycleReset() {
	Inputs inputs;
	HostInputPadTrigger(1, true, 255);
	HostInputDisconnectGamepad(1);
	guest_buttons = 0;
	HostInputPadTrigger(1, true, 255);
	Check(guest_buttons == PAD_BUTTON_CROSS, "disconnect retained a previous trigger press");
	HostInputShutdown();
	guest_buttons = 0;
	HostInputInit(nullptr);
	HostInputPadTrigger(1, true, 255);
	Check(guest_buttons == PAD_BUTTON_CROSS, "shutdown retained a previous trigger press");
}

void TestDisabledMappingTabs() {
	Config::ConfigOptions options;
	const auto south = InputMap::SourceFromButton(SDL_GAMEPAD_BUTTON_SOUTH);
	Config::Load(options);
	const InputMap defaults;
	Check(!defaults.Custom() && defaults.PadControl(south) == ControlFromName("Cross"),
	      "empty configuration lost its default mappings");
	options.keymap = {"Keyboard=Disabled"};
	Config::Load(options);
	const InputMap keyboard_disabled;
	Check(keyboard_disabled.Custom() && keyboard_disabled.FindKey(SDLK_J) == INVALID_CONTROL &&
	          keyboard_disabled.PadControl(south) == ControlFromName("Cross"),
	      "disabling keyboard mappings changed gamepad defaults or restored keyboard input");
	for (const auto& keymap: {Config::Keymap {"Pad=Disabled"},
	                          Config::Keymap {"Keyboard=Disabled", "Pad=Disabled"}}) {
		options.keymap = keymap;
		Config::Load(options);
		const InputMap disabled;
		Check(disabled.Custom() == (keymap.size() == 2),
		      "disabling gamepad mappings changed keyboard defaults");
		for (std::size_t source = 0; source < PAD_SOURCES.size(); source++) {
			Check(disabled.PadControl(source) == INVALID_CONTROL,
			      "a disabled gamepad mapping reverted to its default");
		}
	}
	options.keymap = {"Cross=J", "Pad:Circle=South"};
	Config::Load(options);
	const InputMap mapped;
	Check(mapped.Custom() && mapped.FindKey(SDLK_J) == ControlFromName("Cross") &&
	          mapped.PadControl(south) == ControlFromName("Circle"),
	      "explicit mappings no longer override defaults");
}
} // namespace

int main() {
	Config::Initialize();
	Config::ConfigOptions options;
	options.keymap = {"Pad:Cross=LeftTrigger", "Pad:Circle=RightTrigger"};
	Config::Load(options);
	TestTouchPadStickHandoff();
	TestKeyboardSwitchesFromGamepadTouchPad();
	TestIndependentTriggerEdges();
	TestTriggerLifecycleReset();
	TestDisabledMappingTabs();
	Config::Shutdown();
	return 0;
}
