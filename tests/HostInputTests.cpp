// Exercise production host input with deterministic gamepads and guest output.
#include <SDL3/SDL.h>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {
void Check(bool condition, const char* message) {
	if (!condition) {
		std::fprintf(stderr, "HostInputTests: %s\n", message);
		std::abort();
	}
}

std::array<Sint16, 2> physical_stick {};
std::array<int, 2>    guest_stick {128, 128};
uint32_t              guest_buttons = 0;
bool                  guest_touch   = false;
Uint64                now           = 1000;
int                   wait_timeout  = -1;
SDL_Event             pending_event {};
struct TouchSample {
	bool down;
	float x, y;
	bool emulate_click;
};
std::vector<TouchSample> touches;
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
	return now;
}
SDL_WindowID GetWindowID(SDL_Window*) { return 1; }
bool WaitEventTimeout(SDL_Event* event, Sint32 timeout) {
	wait_timeout = timeout;
	*event = pending_event;
	pending_event = {};
	return event->type != 0;
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
#define SDL_GetWindowID      Fake::GetWindowID
#define SDL_WaitEventTimeout Fake::WaitEventTimeout
#define SDL_HideCursor       Fake::HideCursor
#define SDL_ShowCursor       Fake::ShowCursor
#define SDL_GetMouseState    Fake::GetMouseState
#include "graphics/presentation/window/hostInput.cpp"
#undef SDL_GetGamepadFromID
#undef SDL_GetGamepadAxis
#undef SDL_GetTicks
#undef SDL_GetWindowID
#undef SDL_WaitEventTimeout
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
void SetTouchPad(int id, int finger, bool down, float x, float y, bool emulate_click) {
	if (id == 1 && finger == 1) {
		guest_touch = down;
	}
	if (id == HOST_INPUT_CONTROLLER_ID && finger == 1) {
		touches.push_back({down, x, y, emulate_click});
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
		now            = 1000;
		pending_event  = {};
		touches.clear();
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

void PollSwipeAt(Uint64 time) {
	now = time;
	SDL_Event event {};
	(void) HostInputWaitEvent(&event, 1000);
}

void TestSwipeDirections() {
	for (const auto key: {SDLK_U, SDLK_N, SDLK_B, SDLK_M}) {
		Inputs inputs;
		HostInputKey(key, true);
		Check(touches.size() == 1 && touches.back().down,
		      "a mapped swipe did not start a touch");
		const auto start = touches.back();
		PollSwipeAt(1000);
		Check(wait_timeout == 8 && touches.size() == 1,
		      "the event loop did not schedule the swipe's next sample");
		PollSwipeAt(1100);
		Check(touches.back().down && std::abs(touches.back().x - 0.5f) < 0.001f &&
		          std::abs(touches.back().y - 0.5f) < 0.001f,
		      "a swipe did not move through the centre");
		PollSwipeAt(1200);
		const auto end = touches.back();
		Check(end.down && (key == SDLK_U ? end.y < start.y :
		                      key == SDLK_N ? end.y > start.y :
		                      key == SDLK_B ? end.x < start.x : end.x > start.x),
		      "a swipe moved in the wrong direction or released before its endpoint");
		Check(key == SDLK_U || key == SDLK_N ? end.x == start.x : end.y == start.y,
		      "a swipe drifted along its other axis");
		PollSwipeAt(1208);
		Check(!touches.back().down && touches.back().x == end.x && touches.back().y == end.y,
		      "a completed swipe did not release at its endpoint");
		for (const auto& sample: touches) {
			Check(!sample.emulate_click && sample.x >= 0 && sample.x <= 1 &&
			          sample.y >= 0 && sample.y <= 1,
			      "a swipe clicked the pad or left its coordinate range");
		}
		const auto count = touches.size();
		HostInputKey(key, true);
		PollSwipeAt(1500);
		Check(touches.size() == count, "a held swipe key repeated the gesture");
		HostInputKey(key, false);
		HostInputKey(key, true);
		Check(touches.size() == count + 1 && touches.back().down,
		      "releasing and pressing a swipe key did not start another gesture");
	}
}

void TestSwipeTapAndReplacement() {
	Inputs inputs;
	HostInputKey(SDLK_U, true);
	HostInputKey(SDLK_U, false);
	PollSwipeAt(1100);
	Check(touches.back().down && touches.back().y < touches.front().y,
	      "a quick tap cancelled its swipe");
	HostInputKey(SDLK_M, true);
	Check(touches.size() == 4 && !touches[2].down && touches[3].down &&
	          touches[3].x < 0.5f,
	      "a new direction did not release the previous gesture before starting");
	PollSwipeAt(1300);
	PollSwipeAt(1308);
	Check(!touches.back().down, "a replacement swipe never released");
}

void TestSwipeCleanupAndManualHandoff() {
	Inputs inputs;
	HostInputKey(SDLK_U, true);
	pending_event.type = SDL_EVENT_WINDOW_FOCUS_LOST;
	pending_event.window.windowID = 2;
	PollSwipeAt(1000);
	Check(touches.back().down, "another window's focus loss cancelled the swipe");
	pending_event.type = SDL_EVENT_WINDOW_FOCUS_LOST;
	pending_event.window.windowID = 1;
	PollSwipeAt(1000);
	Check(!touches.back().down, "focus loss left a swipe finger down");
	const auto count = touches.size();
	PollSwipeAt(1400);
	Check(touches.size() == count, "a swipe continued after focus loss");
	HostInputKey(SDLK_U, true);
	Check(touches.back().down, "focus loss retained a held swipe key");
	HostInputSetTouchPadMode(true);
	Check(!touches.back().down, "mouse touchpad mode did not cancel a swipe");
	HostInputMouseTouchPad(true, 0, 0);
	HostInputKey(SDLK_M, true);
	Check(!GetHostInputTouchPadVisualState().enabled && touches.back().down &&
	          !touches[touches.size() - 2].down,
	      "a keyboard swipe did not take over from mouse drawing");
	HostInputToggleGamepadTouchPad(1);
	Check(!touches.back().down, "gamepad touchpad mode did not cancel a swipe");
	Check(HostInputGamepadDrawButton(1, true), "gamepad drawing did not start");
	HostInputKey(SDLK_B, true);
	Check(!GetHostInputTouchPadVisualState().enabled && !guest_touch && touches.back().down,
	      "a keyboard swipe did not take over from gamepad drawing");
	HostInputShutdown();
	Check(!touches.back().down, "shutdown left a swipe finger down");
	HostInputInit(nullptr);
	HostInputKey(SDLK_B, true);
	Check(touches.back().down, "shutdown retained a held swipe key");
}
} // namespace

int main() {
	Config::Initialize();
	Config::ConfigOptions options;
	options.keymap = {"Pad:Cross=LeftTrigger", "Pad:Circle=RightTrigger", "TouchPadMode=F6",
	                  "TouchPadSwipeUp=U", "TouchPadSwipeDown=N", "TouchPadSwipeLeft=B",
	                  "TouchPadSwipeRight=M"};
	Config::Load(options);
	TestTouchPadStickHandoff();
	TestKeyboardSwitchesFromGamepadTouchPad();
	TestIndependentTriggerEdges();
	TestTriggerLifecycleReset();
	TestSwipeDirections();
	TestSwipeTapAndReplacement();
	TestSwipeCleanupAndManualHandoff();
	TestDisabledMappingTabs();
	Config::Shutdown();
	return 0;
}
