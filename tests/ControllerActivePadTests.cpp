// Exercise active-pad selection: the pad with the most recent real input is active, the
// keyboard keeps working alongside, and a pad without input is active only until another pad
// has input. Drives the production controller with deterministic SDL fakes.
#include <SDL3/SDL.h>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {
void Check(bool condition, const char* text) {
	if (!condition) {
		std::fprintf(stderr, "ControllerActivePadTests: %s\n", text);
		std::abort();
	}
}

struct Output {
	uintptr_t pad;
	Uint16    large, small;
	Uint8     enable_bits;
};
std::vector<Output> rumble;
std::vector<Output> effects;
} // namespace

namespace Fake {
SDL_Gamepad* GetGamepadFromID(SDL_JoystickID id) {
	return id == 1 || id == 2 ? reinterpret_cast<SDL_Gamepad*>(static_cast<uintptr_t>(id))
	                          : nullptr;
}
SDL_GamepadType GetGamepadType(SDL_Gamepad*) {
	return SDL_GAMEPAD_TYPE_PS5;
}
bool SetGamepadLED(SDL_Gamepad*, Uint8, Uint8, Uint8) {
	return true;
}
bool GamepadHasSensor(SDL_Gamepad*, SDL_SensorType) {
	return false;
}
bool SetGamepadSensorEnabled(SDL_Gamepad*, SDL_SensorType, bool) {
	return true;
}
bool RumbleGamepad(SDL_Gamepad* pad, Uint16 large, Uint16 small, Uint32) {
	rumble.push_back({reinterpret_cast<uintptr_t>(pad), large, small, 0});
	return true;
}
bool SendGamepadEffect(SDL_Gamepad* pad, const void* data, int) {
	effects.push_back({reinterpret_cast<uintptr_t>(pad), 0, 0, static_cast<const Uint8*>(data)[0]});
	return true;
}
void        CloseGamepad(SDL_Gamepad*) {}
void        Delay(Uint32) {}
const char* GetError() {
	return "fake SDL error";
}
} // namespace Fake

#define SDL_GetGamepadFromID        Fake::GetGamepadFromID
#define SDL_GetGamepadType          Fake::GetGamepadType
#define SDL_SetGamepadLED           Fake::SetGamepadLED
#define SDL_GamepadHasSensor        Fake::GamepadHasSensor
#define SDL_SetGamepadSensorEnabled Fake::SetGamepadSensorEnabled
#define SDL_RumbleGamepad           Fake::RumbleGamepad
#define SDL_SendGamepadEffect       Fake::SendGamepadEffect
#define SDL_CloseGamepad            Fake::CloseGamepad
#define SDL_Delay                   Fake::Delay
#define SDL_GetError                Fake::GetError
#include "libs/controller.cpp"
#undef SDL_GetGamepadFromID
#undef SDL_GetGamepadType
#undef SDL_SetGamepadLED
#undef SDL_GamepadHasSensor
#undef SDL_SetGamepadSensorEnabled
#undef SDL_RumbleGamepad
#undef SDL_SendGamepadEffect
#undef SDL_CloseGamepad
#undef SDL_Delay
#undef SDL_GetError

namespace Libs::Controller::DualSenseHaptics {
bool SetVibration(int, uint8_t, uint8_t, uint32_t) {
	return false;
}
void Shutdown() {}
} // namespace Libs::Controller::DualSenseHaptics

namespace Libs::LibKernel {
uint64_t KYTY_SYSV_ABI KernelGetProcessTime() {
	return 0;
}
} // namespace Libs::LibKernel

namespace Loader::Timer {
double GetTimeMs() {
	return 0.0;
}
} // namespace Loader::Timer

namespace {
using namespace Libs::Controller;

PadActivity Pad(int id, uint64_t last_input) {
	PadActivity pad;
	pad.id         = id;
	pad.last_input = last_input;
	return pad;
}

void TestPureSelection() {
	Check(SelectActivePad({}) == -1, "empty list selected a pad");
	Check(SelectActivePad({Pad(HOST_INPUT_CONTROLLER_ID, 0)}) == HOST_INPUT_CONTROLLER_ID,
	      "keyboard-only list did not keep the keyboard");
	Check(SelectActivePad({Pad(HOST_INPUT_CONTROLLER_ID, 0), Pad(1, 0), Pad(2, 0)}) == 1,
	      "without input the first connected pad must hold the slot");
	Check(SelectActivePad({Pad(HOST_INPUT_CONTROLLER_ID, 0), Pad(1, 0), Pad(2, 9)}) == 2,
	      "the pad with the newest input did not win");
	Check(SelectActivePad({Pad(1, 9), Pad(2, 3)}) == 1, "older input displaced newer input");
	Check(SelectActivePad({Pad(1, 7), Pad(2, 7), Pad(3, 8)}) == 3,
	      "the pad with the newest input lost a tie-break");
	Check(SelectActivePad({Pad(HOST_INPUT_CONTROLLER_ID, 41), Pad(1, 0)}) == 1,
	      "keyboard input selected the keyboard");
}

struct Controller {
	Controller() {
		Initialize();
		Connect(1);
		Connect(2);
	}
	~Controller() { Shutdown(); }
};

void TestActivePadFollowsInput() {
	Controller controller;
	Check(GetActiveControllerId() == 1, "the first connected pad is not the bootstrap active pad");

	// A button press on the other pad moves the slot.
	SetButton(2, PAD_BUTTON_CROSS, true);
	Check(GetActiveControllerId() == 2, "a button press did not move the active pad");
	SetButton(2, PAD_BUTTON_CROSS, false);

	// A stick deflection inside the deadzone is noise, not input.
	SetAxis(1, Axis::LeftX, 128 + INPUT_DEADZONE);
	Check(GetActiveControllerId() == 2, "a stick on the deadzone boundary moved the active pad");
	SetAxis(1, Axis::LeftX, 128);
	SetAxis(2, Axis::LeftX, 128 + INPUT_DEADZONE + 1);
	Check(GetActiveControllerId() == 2, "the active pad did not stay active");

	SetAxis(1, Axis::LeftX, 128 + INPUT_DEADZONE + 1);
	Check(GetActiveControllerId() == 1, "a stick deflection did not move the active pad");

	// The keyboard never takes the slot from a pad, and still drives the shared state.
	SetButton(HOST_INPUT_CONTROLLER_ID, PAD_BUTTON_CROSS, true);
	Check(GetActiveControllerId() == 1, "keyboard input stole the active pad slot");
	PadData data;
	Check(PadReadState(1, &data) == 0, "state read failed");
	Check((data.buttons & PAD_BUTTON_CROSS) != 0, "keyboard button did not reach the pad state");
	SetButton(HOST_INPUT_CONTROLLER_ID, PAD_BUTTON_CROSS, false);

	// A trigger inside the deadzone is noise; past it, it moves the slot.
	SetAxis(2, Axis::TriggerRight, INPUT_DEADZONE);
	Check(GetActiveControllerId() == 1, "a trigger on the deadzone boundary moved the active pad");
	SetAxis(2, Axis::TriggerRight, INPUT_DEADZONE + 1);
	Check(GetActiveControllerId() == 2, "a trigger pull did not move the active pad");

	// Disconnecting the active pad falls back to the most recent input among the rest.
	Disconnect(2);
	Check(GetActiveControllerId() == 1, "disconnect did not fall back to the remaining pad");
	Disconnect(1);
	Check(GetActiveControllerId() == HOST_INPUT_CONTROLLER_ID,
	      "with no pad connected the keyboard must be active");
	SetButton(HOST_INPUT_CONTROLLER_ID, PAD_BUTTON_CIRCLE, true);
	Check(GetActiveControllerId() == HOST_INPUT_CONTROLLER_ID,
	      "keyboard-only input moved the active pad");
	SetButton(HOST_INPUT_CONTROLLER_ID, PAD_BUTTON_CIRCLE, false);

	// A pad reconnecting takes the empty pad slot; keyboard input does not steal it back.
	Connect(2);
	Check(GetActiveControllerId() == 2, "a reconnected pad did not take the empty pad slot");
	SetButton(HOST_INPUT_CONTROLLER_ID, PAD_BUTTON_SQUARE, true);
	Check(GetActiveControllerId() == 2, "keyboard input stole the slot from a reconnected pad");
	SetButton(HOST_INPUT_CONTROLLER_ID, PAD_BUTTON_SQUARE, false);
	Disconnect(2);
}

void TestSwitchResetsOldPad() {
	Controller controller;
	Check(GetActiveControllerId() == 1, "the first connected pad is not active");

	// A button held on the old pad is released when another pad takes the slot.
	SetButton(1, PAD_BUTTON_TRIANGLE, true);
	PadData data;
	Check(PadReadState(1, &data) == 0 && (data.buttons & PAD_BUTTON_TRIANGLE) != 0,
	      "the held button did not reach the pad state");
	SetButton(2, PAD_BUTTON_CROSS, true);
	Check(GetActiveControllerId() == 2, "a button press did not move the active pad");
	Check(PadReadState(1, &data) == 0 && (data.buttons & PAD_BUTTON_TRIANGLE) == 0 &&
	          (data.buttons & PAD_BUTTON_CROSS) != 0,
	      "the old pad's held button survived the switch");
	SetButton(2, PAD_BUTTON_CROSS, false);
	SetButton(1, PAD_BUTTON_TRIANGLE, false);

	// The pad losing the slot stops the game's rumble and trigger effects.
	const PadVibrationParam vibration {200, 100};
	Check(PadSetVibration(1, &vibration) == 0, "vibration request failed");
	PadTriggerEffectParam trigger {};
	trigger.trigger_mask       = 1;
	trigger.command[0].mode    = 1;
	trigger.command[0].data[1] = 8;
	Check(PadSetTriggerEffect(1, &trigger) == 0, "trigger request failed");
	Check(!rumble.empty() && rumble.back().pad == 2 && rumble.back().large != 0,
	      "the game's rumble did not reach the active pad");
	rumble.clear();
	effects.clear();
	SetAxis(1, Axis::LeftX, 128 + INPUT_DEADZONE + 1);
	Check(GetActiveControllerId() == 1, "a stick deflection did not move the active pad");
	Check(!rumble.empty() && rumble.back().pad == 2 && rumble.back().large == 0 &&
	          rumble.back().small == 0,
	      "the old pad kept rumbling after the switch");
	Check(!effects.empty() && effects.back().pad == 2 && effects.back().enable_bits == 0x0c,
	      "the old pad kept its trigger effect after the switch");

	// A stick that stays outside the deadzone, drifting, does not take the slot back; it has to
	// return near centre first.
	SetButton(2, PAD_BUTTON_CROSS, true);
	SetButton(2, PAD_BUTTON_CROSS, false);
	Check(GetActiveControllerId() == 2, "a button press did not move the active pad back");
	SetAxis(1, Axis::LeftX, 128 + INPUT_DEADZONE + 5);
	SetAxis(1, Axis::LeftX, 128 + INPUT_DEADZONE + 1);
	SetAxis(1, Axis::LeftX, 128 + INPUT_DEADZONE / 2 + 1);
	SetAxis(1, Axis::LeftX, 128 + INPUT_DEADZONE + 1);
	Check(GetActiveControllerId() == 2, "stick drift outside the deadzone moved the active pad");
	SetAxis(1, Axis::LeftX, 128);
	SetAxis(1, Axis::LeftX, 128 + INPUT_DEADZONE + 1);
	Check(GetActiveControllerId() == 1, "a stick leaving the deadzone again did not count");
}
} // namespace

int main() {
	Config::Initialize();
	TestPureSelection();
	TestActivePadFollowsInput();
	TestSwitchResetsOldPad();
	Config::Shutdown();
	return 0;
}
