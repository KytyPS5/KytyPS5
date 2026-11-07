#ifndef EMULATOR_SRC_LIBS_PADINPUT_H_
#define EMULATOR_SRC_LIBS_PADINPUT_H_

#include "libs/controller.h"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace Libs::Controller {

constexpr int PAD_AXES = static_cast<int>(Axis::AxisMax);
// The axes of a pad that nobody touches: sticks at the center, triggers released.
constexpr std::array<int, PAD_AXES> PAD_AXES_AT_REST {128, 128, 128, 128, 0, 0};

// The motion sensor values of the active gamepad.
struct PadMotion {
	std::array<float, 3> accel {0.0f, 1.0f, 0.0f};
	std::array<float, 3> gyro {};
	std::array<float, 4> orientation {0.0f, 0.0f, 0.0f, 1.0f};

	bool operator==(const PadMotion&) const = default;
};

// What the game reads for player 1 at one moment.
struct PadState {
	struct Touch {
		uint8_t  id   = 0;
		bool     down = false;
		uint16_t x    = 0;
		uint16_t y    = 0;
	};

	uint64_t                  time    = 0;
	uint32_t                  buttons = 0;
	std::array<int, PAD_AXES> axes    = PAD_AXES_AT_REST;
	std::array<Touch, 2>      touch {};
	PadMotion                 motion;
};

// The input of player 1, made from the host keyboard and mouse (HOST_INPUT_CONTROLLER_ID) and from
// the connected gamepads.
//
// Each source keeps its own buttons, sticks, triggers and touches, so one source never releases
// what another holds. The game sees the keyboard and one gamepad, the active one: the connected
// gamepad that last sent real input (a button press, a stick tilted past STICK_CLAIM or a trigger
// pressed past TRIGGER_CLAIM). Until one does, it is the first one connected. A gamepad that the
// host reports twice therefore cannot hide the copy that sends input. Input of the other gamepads
// only decides which one is active.
//
// The buttons of the keyboard and the active gamepad are combined. Each stick comes from the source
// that tilts it further, each trigger from the source that presses it further. L2 and R2 are down
// while their trigger is pressed past TRIGGER_PRESSED.
//
// Every change is kept in a history of HISTORY states for Read(). Times are given by the caller.
// The class is not thread-safe: the caller serializes the calls.
class PadInput {
public:
	static constexpr uint32_t HISTORY = 64;
	// A trigger of 0 to 255 counts as pressed from this value on: lower values are sensor noise.
	static constexpr int TRIGGER_PRESSED = 16;
	// A gamepad becomes active when a stick moves this far from the center (128), or a trigger
	// this far. A drifting stick or a resting trigger stays below it.
	static constexpr int STICK_CLAIM   = 64;
	static constexpr int TRIGGER_CLAIM = 64;

	PadInput() = default;

	// A connection change keeps the time of the last state. Connect() returns false when the source
	// was already connected; Disconnect() ignores an unknown id.
	bool Connect(int id);
	void Disconnect(int id);
	void DisconnectAll();

	// A change of one control. Input of an unknown id is ignored. A button of L2 or R2 is ignored:
	// they follow their trigger axis.
	void SetButton(int id, uint32_t button, bool down, uint64_t time);
	void SetAxis(int id, Axis axis, int value, uint64_t time);
	void SetRightStick(int id, int x, int y, uint64_t time);
	// x is 0 to 1919, y is 0 to 942. The keyboard's touch also presses the touch pad button.
	void SetTouch(int id, int finger, bool down, uint16_t x, uint16_t y, uint64_t time);
	// The motion values of the active gamepad. The caller drops the samples of other gamepads.
	void SetMotion(const PadMotion& motion, uint64_t time);
	// Every source lets go of every control, as on a pad that was just connected.
	void Reset(uint64_t time);

	// The active gamepad; HOST_INPUT_CONTROLLER_ID when only the keyboard is connected; -1 when
	// nothing is.
	[[nodiscard]] int  ActiveId() const { return m_active_id; }
	[[nodiscard]] bool Connected() const { return !m_sources.empty(); }
	// How many times player 1 went from no source to a connected one.
	[[nodiscard]] int              ConnectedCount() const { return m_connected_count; }
	[[nodiscard]] std::vector<int> SourceIds() const;
	[[nodiscard]] const PadState&  Latest() const { return m_latest; }

	// Copies the states the game has not read yet into `out`, oldest first, and returns their
	// number. When more are waiting than `out` holds, the newest ones: a game that reads fewer
	// states than it is given never falls behind the input.
	int Read(std::span<PadState> out);

private:
	struct Source {
		int                            id      = 0;
		uint32_t                       buttons = 0;
		std::array<int, PAD_AXES>      axes    = PAD_AXES_AT_REST;
		std::array<PadState::Touch, 2> touch {};
		// When this gamepad last sent real input, in calls of Claim(); 0 for never.
		uint64_t claimed = 0;
	};

	Source* Find(int id);
	void    Claim(Source& source);
	void    ChooseActive();
	void    Update(uint64_t time);

	std::vector<Source> m_sources;
	int                 m_active_id       = -1;
	int                 m_connected_count = 0;
	uint64_t            m_claims          = 0;
	uint8_t             m_next_touch_id   = 1;
	PadMotion           m_motion;
	PadState            m_latest;

	std::array<PadState, HISTORY> m_history {};
	// States written and read so far; the newest written state is m_written - 1.
	uint64_t m_written = 0;
	uint64_t m_read    = 0;
};

// A gamepad device event from SDL, and what the window does with it.
enum class GamepadDeviceEvent : uint8_t {
	Added,
	Removed,
	// SDL has loaded the pad's new mapping; the button and axis events that follow use it.
	Remapped,
};

enum class GamepadDeviceStep : uint8_t {
	// Open the pad and connect it as a source.
	Open,
	// Disconnect the source and close the pad.
	Close,
	Nothing,
};

// `open`: the window has the pad open. SDL reports a known joystick as added again when a new
// mapping makes it a gamepad, and reports the removal of every pad, also of one it could not open.
[[nodiscard]] constexpr GamepadDeviceStep PlanGamepadDeviceEvent(GamepadDeviceEvent event,
                                                                 bool open) noexcept {
	switch (event) {
		case GamepadDeviceEvent::Added:
			return open ? GamepadDeviceStep::Nothing : GamepadDeviceStep::Open;
		case GamepadDeviceEvent::Removed:
			return open ? GamepadDeviceStep::Close : GamepadDeviceStep::Nothing;
		case GamepadDeviceEvent::Remapped: return GamepadDeviceStep::Nothing;
	}
	return GamepadDeviceStep::Nothing;
}

} // namespace Libs::Controller

#endif /* EMULATOR_SRC_LIBS_PADINPUT_H_ */
