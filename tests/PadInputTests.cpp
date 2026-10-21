// Player 1's input from the keyboard and the gamepads: the faults it must not have.
#include "libs/padInput.h"

#include <array>
#include <cstdio>
#include <cstdlib>

namespace {

using namespace Libs::Controller;

constexpr int KEYBOARD = HOST_INPUT_CONTROLLER_ID;

void Check(bool condition, const char* text) {
	if (!condition) {
		std::fprintf(stderr, "PadInputTests: %s\n", text);
		std::abort();
	}
}

int AxisOf(const PadInput& input, Axis axis) {
	return input.Latest().axes[static_cast<int>(axis)];
}

void TestKeyboardAndPadHoldTheirOwnButtons() {
	PadInput input;
	input.Connect(KEYBOARD);
	input.Connect(1);
	input.SetButton(1, PAD_BUTTON_CROSS, true, 1);
	input.SetButton(KEYBOARD, PAD_BUTTON_CROSS, true, 2);
	input.SetButton(KEYBOARD, PAD_BUTTON_CROSS, false, 3);
	Check((input.Latest().buttons & PAD_BUTTON_CROSS) != 0,
	      "releasing a key released the button the gamepad holds");
	input.SetButton(KEYBOARD, PAD_BUTTON_SQUARE, true, 4);
	input.SetButton(1, PAD_BUTTON_SQUARE, false, 5);
	Check((input.Latest().buttons & PAD_BUTTON_SQUARE) != 0,
	      "a gamepad release released the button the keyboard holds");
}

void TestTheStickTiltedFurtherWins() {
	PadInput input;
	input.Connect(KEYBOARD);
	input.Connect(1);
	input.SetAxis(KEYBOARD, Axis::LeftX, 255, 1);
	// A drifting stick sends small values all the time.
	input.SetAxis(1, Axis::LeftX, 131, 2);
	input.SetAxis(1, Axis::LeftY, 126, 3);
	Check(AxisOf(input, Axis::LeftX) == 255 && AxisOf(input, Axis::LeftY) == 128,
	      "a drifting stick overrode the keyboard's stick");
	input.SetAxis(1, Axis::LeftX, 0, 4);
	Check(AxisOf(input, Axis::LeftX) == 0, "a full gamepad tilt lost to a keyboard tilt");
	input.SetAxis(1, Axis::LeftX, 128, 5);
	input.SetAxis(KEYBOARD, Axis::LeftX, 128, 6);
	Check(AxisOf(input, Axis::LeftX) == 128, "the stick did not return to the center");
	input.SetAxis(KEYBOARD, Axis::TriggerLeft, 255, 7);
	input.SetAxis(1, Axis::TriggerLeft, 40, 8);
	Check(AxisOf(input, Axis::TriggerLeft) == 255, "a lighter trigger overrode a held key");
}

void TestTriggerNoiseIsNotAPress() {
	PadInput input;
	input.Connect(1);
	input.SetAxis(1, Axis::TriggerRight, PadInput::TRIGGER_PRESSED - 1, 1);
	Check((input.Latest().buttons & PAD_BUTTON_R2) == 0, "trigger noise pressed R2");
	input.SetAxis(1, Axis::TriggerRight, PadInput::TRIGGER_PRESSED, 2);
	Check((input.Latest().buttons & PAD_BUTTON_R2) != 0, "a pressed trigger did not press R2");
	input.SetAxis(1, Axis::TriggerRight, 0, 3);
	Check((input.Latest().buttons & PAD_BUTTON_R2) == 0, "a released trigger kept R2");
}

void TestTheGamepadThatSendsInputIsActive() {
	PadInput input;
	input.Connect(KEYBOARD);
	Check(input.ActiveId() == KEYBOARD, "the keyboard alone was not player 1");
	// The host reports one pad twice; the silent copy connects first.
	input.Connect(7);
	input.Connect(8);
	Check(input.ActiveId() == 7, "before any input, the first gamepad was not active");
	input.SetButton(8, PAD_BUTTON_CROSS, true, 1);
	Check(input.ActiveId() == 8, "the gamepad that sent input did not become active");
	Check((input.Latest().buttons & PAD_BUTTON_CROSS) != 0,
	      "the press that made a gamepad active was lost");
	input.SetAxis(7, Axis::LeftX, 140, 2);
	Check(input.ActiveId() == 8, "a drifting stick took over the active gamepad");
	input.SetButton(7, PAD_BUTTON_CIRCLE, true, 3);
	Check(input.ActiveId() == 7 && (input.Latest().buttons & PAD_BUTTON_CROSS) == 0,
	      "the other gamepad's buttons stayed after it lost player 1");
}

// A resting trigger is not input: only a trigger pulled to TRIGGER_CLAIM takes player 1.
void TestARestingTriggerDoesNotTakeOver() {
	PadInput input;
	input.Connect(7);
	input.Connect(8);
	input.SetAxis(8, Axis::TriggerLeft, PadInput::TRIGGER_CLAIM - 1, 1);
	Check(input.ActiveId() == 7, "a resting trigger took over the active gamepad");
	input.SetAxis(8, Axis::TriggerLeft, PadInput::TRIGGER_CLAIM, 2);
	Check(input.ActiveId() == 8, "a pulled trigger did not take over the active gamepad");
}

// Each new touch gets the next id, 1 to 127, and keeps it while the finger stays down.
void TestTouchesGetTheirOwnIds() {
	PadInput input;
	input.Connect(7);
	input.SetTouch(7, 0, true, 100, 100, 1);
	const auto first = input.Latest().touch[0].id;
	input.SetTouch(7, 0, true, 120, 100, 2);
	Check(first == 1 && input.Latest().touch[0].id == first, "a moving touch changed its id");
	input.SetTouch(7, 1, true, 500, 300, 3);
	Check(input.Latest().touch[1].id == 2, "a second finger did not get the next id");
	input.SetTouch(7, 0, false, 120, 100, 4);
	input.SetTouch(7, 0, true, 50, 50, 5);
	Check(input.Latest().touch[0].id == 3, "a new touch reused an old id");
	for (uint64_t time = 6; time < 6 + 2 * 124; time += 2) {
		input.SetTouch(7, 0, false, 0, 0, time);
		input.SetTouch(7, 0, true, 0, 0, time + 1);
	}
	Check(input.Latest().touch[0].id == 127, "the touch ids did not count up to 127");
	input.SetTouch(7, 0, false, 0, 0, 1000);
	input.SetTouch(7, 0, true, 0, 0, 1001);
	Check(input.Latest().touch[0].id == 1, "the touch ids did not start again at 1 after 127");
}

void TestAConnectionChangeKeepsHeldInput() {
	PadInput input;
	input.Connect(KEYBOARD);
	input.Connect(1);
	input.SetButton(1, PAD_BUTTON_R1, true, 1);
	input.SetAxis(1, Axis::LeftY, 0, 2);
	input.Connect(2);
	input.Disconnect(2);
	Check((input.Latest().buttons & PAD_BUTTON_R1) != 0 && AxisOf(input, Axis::LeftY) == 0,
	      "another gamepad's connection cleared the held input");
	input.Disconnect(99);
	Check(input.ActiveId() == 1, "an unknown gamepad's removal changed player 1");
	input.SetButton(99, PAD_BUTTON_CROSS, true, 3);
	Check((input.Latest().buttons & PAD_BUTTON_CROSS) == 0, "an unknown gamepad pressed a button");
	input.Disconnect(1);
	Check(input.ActiveId() == KEYBOARD && input.Latest().buttons == 0,
	      "a removed gamepad's buttons stayed down");
}

void TestReadGivesTheNewestUnreadStates() {
	PadInput input;
	input.Connect(1);
	std::array<PadState, 64> out {};
	(void)input.Read(out);
	for (int i = 0; i < 10; i++) {
		input.SetAxis(1, Axis::LeftX, 100 + i, 10 + i);
	}
	Check(input.Read(std::span(out).first(4)) == 4 && out[0].axes[0] == 106 &&
	          out[3].axes[0] == 109,
	      "a short read did not get the newest states, oldest first");
	Check(input.Read(out) == 0, "states were read twice");
	for (int i = 0; i < 100; i++) {
		input.SetAxis(1, Axis::LeftX, i, 100 + i);
	}
	Check(input.Read(out) == 64 && out[0].axes[0] == 36 && out[63].axes[0] == 99,
	      "a full read did not get the newest 64 states");
	input.SetAxis(1, Axis::LeftX, 99, 300);
	Check(input.Read(out) == 0, "an unchanged input added a state");
}

void TestGamepadDeviceEvents() {
	using enum GamepadDeviceEvent;
	Check(PlanGamepadDeviceEvent(Added, false) == GamepadDeviceStep::Open,
	      "a new pad is not opened");
	// SDL reports a known joystick as added again when a new mapping makes it a gamepad.
	Check(PlanGamepadDeviceEvent(Added, true) == GamepadDeviceStep::Nothing,
	      "a pad that is open is opened again");
	Check(PlanGamepadDeviceEvent(Removed, true) == GamepadDeviceStep::Close,
	      "an open pad is not closed");
	// SDL reports the removal of every pad, also of one it could not open.
	Check(PlanGamepadDeviceEvent(Removed, false) == GamepadDeviceStep::Nothing,
	      "a pad that is not open is closed");
	// SDL has loaded the new mapping before it reports a remap.
	Check(PlanGamepadDeviceEvent(Remapped, true) == GamepadDeviceStep::Nothing &&
	          PlanGamepadDeviceEvent(Remapped, false) == GamepadDeviceStep::Nothing,
	      "a remapped pad needs a step");
}

} // namespace

int main() {
	TestKeyboardAndPadHoldTheirOwnButtons();
	TestTheStickTiltedFurtherWins();
	TestTriggerNoiseIsNotAPress();
	TestTheGamepadThatSendsInputIsActive();
	TestARestingTriggerDoesNotTakeOver();
	TestTouchesGetTheirOwnIds();
	TestAConnectionChangeKeepsHeldInput();
	TestReadGivesTheNewestUnreadStates();
	TestGamepadDeviceEvents();
	std::puts("PadInputTests: passed");
	return 0;
}
