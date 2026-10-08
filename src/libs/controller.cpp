#include "libs/controller.h"

#include <SDL3/SDL.h>
#include "common/assert.h"
#include "common/common.h"
#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/stringUtils.h"
#include "common/threads.h"
#include "kernel/pthread.h"
#include "libs/dualSenseHaptics.h"
#include "libs/errno.h"
#include "libs/libs.h"
#include "libs/padData.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <vector>

namespace Libs::Controller {

LIB_NAME("Pad", "Pad");

constexpr int PAD_ERROR_INVALID_ARG     = -2137915391; /* 0x80920001 */
constexpr int PAD_ERROR_INVALID_HANDLE  = -2137915389; /* 0x80920003 */
constexpr int PAD_ERROR_ALREADY_OPENED  = -2137915388; /* 0x80920004 */
constexpr int PAD_ERROR_NOT_INITIALIZED = -2137915387; /* 0x80920005 */
constexpr int PAD_ERROR_NO_HANDLE       = -2137915384; /* 0x80920008 */
constexpr int PAD_HANDLE                = 1;

constexpr uint32_t RUMBLE_DURATION_MS = 0xffff;
constexpr uint32_t RELEASE_FLUSH_MS   = 50;

struct PadControllerInformation {
	float    touch_pixel_density;
	uint16_t touch_resolution_x;
	uint16_t touch_resolution_y;
	uint8_t  stick_dead_zone_left;
	uint8_t  stick_dead_zone_right;
	uint8_t  connection_type;
	uint8_t  connected_count;
	bool     connected;
	int      device_class;
	uint8_t  reserve[8];
};

struct PadLightBarParam {
	uint8_t r;
	uint8_t g;
	uint8_t b;
	uint8_t reserve;
};

struct PadVibrationParam {
	uint8_t large_motor;
	uint8_t small_motor;
};

struct PadTriggerEffectCommand {
	uint32_t mode;
	uint8_t  reserve[4];
	uint8_t  data[48];
};

struct PadTriggerEffectParam {
	uint8_t                 trigger_mask;
	uint8_t                 reserve[7];
	PadTriggerEffectCommand command[2];
};

struct PadTriggerEffectStateInformation {
	int32_t state[2];
};

static_assert(sizeof(PadTriggerEffectCommand) == 56);
static_assert(sizeof(PadTriggerEffectParam) == 120);
static_assert(sizeof(PadTriggerEffectStateInformation) == 8);

struct DualSenseEffects {
	uint8_t enable_bits;
	uint8_t reserve[9];
	uint8_t right_trigger[11];
	uint8_t left_trigger[11];
};

static_assert(sizeof(DualSenseEffects) == 32);

struct ControllerState {
	struct Touch {
		uint8_t  id   = 0;
		bool     down = false;
		uint16_t x    = 0;
		uint16_t y    = 0;
	};

	uint64_t time                                  = 0;
	uint32_t buttons                               = 0;
	int      axes[static_cast<int>(Axis::AxisMax)] = {128, 128, 128, 128, 0, 0};
	Touch    touch[2];
	std::array<float, 3> accel {0.0f, 1.0f, 0.0f};
	std::array<float, 3> gyro {};
	std::array<float, 4> orientation {0.0f, 0.0f, 0.0f, 1.0f};
};

class GameController {
public:
	GameController() = default;

	KYTY_CLASS_NO_COPY(GameController);

	int InitPort();
	int OpenPort(int user_id, int type, int index);
	int GetPortHandle(int user_id, int type, int index);
	int ClosePort(int handle);
	int CheckPortHandle(int handle);

	void Connect(int id);
	void Disconnect(int id);
	void Button(int id, uint32_t button, bool down);
	void Axis(int id, Axis axis, int value);
	void RightStick(int id, int x, int y);
	void TouchPad(int id, int finger, bool down, float x, float y);
	void Motion(int id, Sensor sensor, const float* data, uint64_t time_us);
	void SetMotionSensorState(bool enable);
	void ResetOrientation();
	void ResetInputState();
	void ReleaseHostPads();
	void GetConnectionInfo(bool* flag, int* count);
	void SetVibration(uint8_t large_motor, uint8_t small_motor);
	int  GetActiveControllerId();
	void SetLightBar(uint8_t r, uint8_t g, uint8_t b);
	bool SetTriggerEffect(const PadTriggerEffectParam& param);
	void  GetTriggerEffectState(PadTriggerEffectStateInformation* info);
	void  CycleSetting(Setting setting);
	float GetSettingScale(Setting setting) const;
	void ReadState(ControllerState* state, bool* flag, int* count);
	int  ReadStates(ControllerState* states, int states_num, bool* flag, int* count);

private:
	static constexpr uint32_t STATES_MAX = 64;
	enum class PortState { Uninitialized, Closed, Open };

	int CheckPortHandleLocked(int handle) const;

	void CheckActive();
	void AddState();
	// Apply cached output without changing its game-requested lifetime; caller holds m_mutex.
	void ApplyVibration();
	bool SendTriggerEffect(const PadTriggerEffectParam& param);
	void UpdateTriggerEffectState();

	Common::Mutex    m_mutex;
	PortState        m_port_state      = PortState::Uninitialized;
	std::vector<int> m_connected_ids;
	int              m_active_id       = -1;
	bool             m_connected       = false;
	int              m_connected_count = 0;
	bool             m_motion_enabled  = true;
	uint64_t         m_gyro_time       = 0;
	// Accelerometer gravity direction in the reset frame.
	std::array<float, 3> m_up_reference {0.0f, 1.0f, 0.0f};
	bool                 m_up_reference_valid = false;
	ControllerState  m_state;
	ControllerState  m_states[STATES_MAX];
	bool             m_obtained[STATES_MAX] {};
	uint32_t         m_states_num    = 0;
	uint32_t         m_first_state   = 0;
	uint8_t          m_next_touch_id = 1;
	// The game's last requests, re-sent when their intensity setting changes.
	std::array<uint8_t, 2> m_vibration {};
	uint64_t               m_vibration_until = 0;
	PadTriggerEffectParam  m_trigger_effect {};
	PadTriggerEffectStateInformation m_trigger_state {};
	// Setting changes share the output lock; audio only needs an atomic scale snapshot.
	std::array<std::atomic<uint32_t>, 3> m_setting_steps {};
};

static GameController* g_controller = nullptr;

static void pad_fill_data(PadData* data, const ControllerState& state, bool connected,
                          int connected_count) {
	EXIT_IF(data == nullptr);

	std::memset(data, 0, sizeof(*data));

	data->buttons           = state.buttons;
	data->left_stick_x      = state.axes[static_cast<int>(Axis::LeftX)];
	data->left_stick_y      = state.axes[static_cast<int>(Axis::LeftY)];
	data->right_stick_x     = state.axes[static_cast<int>(Axis::RightX)];
	data->right_stick_y     = state.axes[static_cast<int>(Axis::RightY)];
	data->analog_buttons_l2 = state.axes[static_cast<int>(Axis::TriggerLeft)];
	data->analog_buttons_r2 = state.axes[static_cast<int>(Axis::TriggerRight)];
	data->acceleration_x     = state.accel[0];
	data->acceleration_y     = state.accel[1];
	data->acceleration_z     = state.accel[2];
	data->angular_velocity_x = state.gyro[0];
	data->angular_velocity_y = state.gyro[1];
	data->angular_velocity_z = state.gyro[2];
	data->orientation_x      = state.orientation[0];
	data->orientation_y      = state.orientation[1];
	data->orientation_z      = state.orientation[2];
	data->orientation_w      = state.orientation[3];
	for (const auto& touch: state.touch) {
		if (touch.down) {
			auto& output = data->touch_data.touch[data->touch_data.touch_num++];
			output.x     = touch.x;
			output.y     = touch.y;
			output.id    = touch.id;
		}
	}
	data->connected              = connected;
	data->timestamp              = state.time;
	data->connected_count        = static_cast<uint8_t>(std::min(connected_count, 255));
	data->device_unique_data_len = 0;
}

// In cycle order, starting at the PS5 default.
constexpr std::array SPEAKER_VOLUME = {1.0f, 0.0f, 0.02f, 0.12f, 0.42f};
constexpr std::array INTENSITY      = {1.0f, 0.0f, 0.33f, 0.66f};

void CycleSetting(Setting setting) {
	if (g_controller != nullptr) {
		g_controller->CycleSetting(setting);
	}
}

float GetSettingScale(Setting setting) {
	return g_controller != nullptr ? g_controller->GetSettingScale(setting) : 1.0f;
}

void GameController::CycleSetting(Setting setting) {
	Common::LockGuard lock(m_mutex);
	auto&             step = m_setting_steps[static_cast<size_t>(setting)];
	const auto count = setting == Setting::SpeakerVolume ? SPEAKER_VOLUME.size() : INTENSITY.size();
	step.store((step.load(std::memory_order_relaxed) + 1) % count, std::memory_order_relaxed);
	if (setting == Setting::VibrationIntensity) {
		ApplyVibration();
	} else if (setting == Setting::TriggerEffectIntensity) {
		(void)SendTriggerEffect(m_trigger_effect);
	}
}

float GameController::GetSettingScale(Setting setting) const {
	const auto step = m_setting_steps[static_cast<size_t>(setting)].load(std::memory_order_relaxed);
	if (setting == Setting::SpeakerVolume) {
		// The old speaker level matches the PS5 slider's midpoint. Give the upper
		// half of the global slider up to 6 dB of extra software gain.
		return SPEAKER_VOLUME[step] * (Config::GetControllerSpeakerVolume() / 50.0f);
	}
	if (setting == Setting::VibrationIntensity) {
		return INTENSITY[step] * (Config::GetControllerVibrationIntensity() / 100.0f);
	}
	return INTENSITY[step];
}

static uint8_t Scale(uint8_t value, float scale) {
	if (value == 0 || scale == 0.0f) {
		return 0;
	}
	return static_cast<uint8_t>(std::max(1L, std::lround(value * scale)));
}

static bool trigger_effect_zones(uint8_t* effect, const uint8_t* strengths, float scale,
                                 uint8_t type, uint8_t frequency = 0) {
	uint16_t active = 0;
	uint32_t packed = 0;
	for (int i = 0; i < 10; i++) {
		if (strengths[i] > 8) {
			return false;
		}
		const auto strength = Scale(strengths[i], scale);
		if (strength != 0) {
			active |= static_cast<uint16_t>(1u << i);
			packed |= static_cast<uint32_t>(strength - 1u) << (3 * i);
		}
	}

	effect[0] = active != 0 && (type != 0x26 || frequency != 0) ? type : 0x05;
	effect[1] = static_cast<uint8_t>(active);
	effect[2] = static_cast<uint8_t>(active >> 8u);
	effect[3] = static_cast<uint8_t>(packed);
	effect[4] = static_cast<uint8_t>(packed >> 8u);
	effect[5] = static_cast<uint8_t>(packed >> 16u);
	effect[6] = static_cast<uint8_t>(packed >> 24u);
	effect[9] = frequency;
	return true;
}

static bool trigger_effect_to_dualsense(const PadTriggerEffectCommand& command, uint8_t* effect,
                                        float scale) {
	std::memset(effect, 0, 11);
	effect[0] = 0x05;

	uint8_t strengths[10] = {};
	switch (command.mode) {
		case 0: return true;
		case 1:
			if (command.data[0] > 9 || command.data[1] > 8) {
				return false;
			}
			std::fill(strengths + command.data[0], strengths + 10, command.data[1]);
			return trigger_effect_zones(effect, strengths, scale, 0x21);
		case 2: {
			const auto start    = command.data[0];
			const auto end      = command.data[1];
			const auto strength = command.data[2];
			if (start < 2 || start > 7 || end <= start || end > 8 || strength > 8) {
				return false;
			}
			const auto scaled = Scale(strength, scale);
			if (scaled == 0) {
				return true;
			}
			const uint16_t zones = static_cast<uint16_t>((1u << start) | (1u << end));
			effect[0]            = 0x25;
			effect[1]            = static_cast<uint8_t>(zones);
			effect[2]            = static_cast<uint8_t>(zones >> 8u);
			effect[3]            = scaled - 1u;
			return true;
		}
		case 3:
			if (command.data[0] > 9 || command.data[1] > 8) {
				return false;
			}
			std::fill(strengths + command.data[0], strengths + 10, command.data[1]);
			return trigger_effect_zones(effect, strengths, scale, 0x26, command.data[2]);
		case 4: return trigger_effect_zones(effect, command.data, scale, 0x21);
		case 5: {
			const int start          = command.data[0];
			const int end            = command.data[1];
			const int start_strength = command.data[2];
			const int end_strength   = command.data[3];
			if (start > 8 || end <= start || end > 9 || start_strength < 1 || start_strength > 8 ||
			    end_strength < 1 || end_strength > 8) {
				return false;
			}
			for (int i = start; i < 10; i++) {
				const int delta  = (end_strength - start_strength) * (i - start);
				const int length = end - start;
				strengths[i]     = static_cast<uint8_t>(
				    i >= end ? end_strength
				             : start_strength +
				                   (delta + (delta < 0 ? -length / 2 : length / 2)) / length);
			}
			return trigger_effect_zones(effect, strengths, scale, 0x21);
		}
		case 6: return trigger_effect_zones(effect, command.data + 1, scale, 0x26, command.data[0]);
		default: return false;
	}
}

enum TriggerState : int32_t {
	TRIGGER_STATE_OFF               = 0,
	TRIGGER_STATE_FEEDBACK_STANDBY  = 1,
	TRIGGER_STATE_FEEDBACK_ACTIVE   = 2,
	TRIGGER_STATE_WEAPON_STANDBY    = 3,
	TRIGGER_STATE_WEAPON_PULLING    = 4,
	TRIGGER_STATE_WEAPON_FIRED      = 5,
	TRIGGER_STATE_VIBRATION_STANDBY = 6,
	TRIGGER_STATE_VIBRATION_ACTIVE  = 7,
};

static int32_t TriggerEffectState(const PadTriggerEffectCommand& command, int value,
                                  int32_t previous) {
	// Trigger positions use nonuniform travel ranges, with endpoints at 0 and 255.
	const int pos = value == 0 ? 0 : value == 255 ? 9 : 1 + value / 32;
	switch (command.mode) {
		case 1: // feedback: resistance from position data[0], strength data[1]
			return command.data[1] != 0 && pos >= command.data[0] ? TRIGGER_STATE_FEEDBACK_ACTIVE
			                                                      : TRIGGER_STATE_FEEDBACK_STANDBY;
		case 2: // weapon: resistance from data[0], released at data[1]
			if (value < command.data[0] * 32) {
				return TRIGGER_STATE_WEAPON_STANDBY;
			}
			return previous == TRIGGER_STATE_WEAPON_FIRED ||
			               value >= std::min(command.data[1] * 32, 254)
			           ? TRIGGER_STATE_WEAPON_FIRED
			           : TRIGGER_STATE_WEAPON_PULLING;
		case 3: // vibration from position data[0], amplitude data[1], frequency data[2]
			return command.data[1] != 0 && command.data[2] != 0 && pos >= command.data[0]
			           ? TRIGGER_STATE_VIBRATION_ACTIVE
			           : TRIGGER_STATE_VIBRATION_STANDBY;
		case 4: // multiple-position feedback: strength per position
			return command.data[pos] != 0 ? TRIGGER_STATE_FEEDBACK_ACTIVE
			                              : TRIGGER_STATE_FEEDBACK_STANDBY;
		case 5: // slope feedback between data[0] and data[1]
			return pos >= command.data[0] && pos < command.data[1] ? TRIGGER_STATE_FEEDBACK_ACTIVE
			                                                       : TRIGGER_STATE_FEEDBACK_STANDBY;
		case 6: // multiple-position vibration: frequency data[0], amplitude per position
			return command.data[0] != 0 && command.data[1 + pos] != 0
			           ? TRIGGER_STATE_VIBRATION_ACTIVE
			           : TRIGGER_STATE_VIBRATION_STANDBY;
		default: return TRIGGER_STATE_OFF;
	}
}

void Initialize() {
	EXIT_IF(g_controller != nullptr);

	g_controller = new GameController;
	g_controller->Connect(HOST_INPUT_CONTROLLER_ID);
}

void Shutdown() {
	EmergencyShutdown();
	delete g_controller;
	g_controller = nullptr;
}

void EmergencyShutdown() {
	if (g_controller != nullptr) {
		g_controller->ReleaseHostPads();
	}
}

void GameController::Connect(int id) {
	Common::LockGuard lock(m_mutex);

	if (std::find(m_connected_ids.begin(), m_connected_ids.end(), id) != m_connected_ids.end()) {
		return;
	}

	m_connected_ids.push_back(id);

	if (id != HOST_INPUT_CONTROLLER_ID) {
		if (auto* pad = SDL_GetGamepadFromID(static_cast<SDL_JoystickID>(id));
		    pad != nullptr) {
			if (const auto& color = Config::GetControllerColor()) {
				(void)SDL_SetGamepadLED(pad, (*color)[0], (*color)[1], (*color)[2]);
			}
			for (auto sensor: {SDL_SENSOR_ACCEL, SDL_SENSOR_GYRO}) {
				if (SDL_GamepadHasSensor(pad, sensor) &&
				    !SDL_SetGamepadSensorEnabled(pad, sensor, true)) {
					LOGF("\t enabling controller sensor failed: %s\n", SDL_GetError());
				}
			}
		}
	}

	CheckActive();
}

void GameController::Disconnect(int id) {
	Common::LockGuard lock(m_mutex);

	const auto it = std::find(m_connected_ids.begin(), m_connected_ids.end(), id);
	EXIT_IF(it == m_connected_ids.end());

	m_connected_ids.erase(it);

	CheckActive();
}

void GameController::CheckActive() {
	int  new_active_id = -1;
	bool new_connected = false;

	if (!m_connected_ids.empty()) {
		new_active_id = m_connected_ids[0];
		for (const auto id: m_connected_ids) {
			if (id != HOST_INPUT_CONTROLLER_ID) {
				new_active_id = id;
				break;
			}
		}
		new_connected = true;
	}

	if (m_connected == new_connected && m_active_id == new_active_id) {
		return;
	}
	if (!m_connected && new_connected) {
		m_connected_count++;
	}
	m_active_id     = new_active_id;
	m_connected     = new_connected;
	m_state         = {};
	m_gyro_time     = 0;
	m_up_reference_valid = false;
	m_states_num    = 0;
	m_first_state   = 0;
	m_next_touch_id = 1;
	m_vibration          = {};
	m_vibration_until    = 0;
	m_trigger_effect     = {};
	m_trigger_state      = {};
}

void GameController::AddState() {
	if (m_states_num >= STATES_MAX) {
		m_states_num  = STATES_MAX - 1;
		m_first_state = (m_first_state + 1) % STATES_MAX;
	}

	const auto index  = (m_first_state + m_states_num) % STATES_MAX;
	m_states[index]   = m_state;
	m_obtained[index] = false;
	m_states_num++;
}

void GameController::Button(int id, uint32_t button, bool down) {
	Common::LockGuard lock(m_mutex);

	// The keyboard shares the player-1 pad with the active gamepad.
	if (m_active_id == id || id == HOST_INPUT_CONTROLLER_ID) {
		m_state.time = LibKernel::KernelGetProcessTime();

		m_state.buttons = down ? m_state.buttons | button : m_state.buttons & ~button;

		AddState();
	}
}

void GameController::Axis(int id, Controller::Axis axis, int value) {
	Common::LockGuard lock(m_mutex);

	if (m_active_id == id || id == HOST_INPUT_CONTROLLER_ID) {
		m_state.time = LibKernel::KernelGetProcessTime();

		int axis_id = static_cast<int>(axis);

		EXIT_IF(axis_id < 0 || axis_id >= static_cast<int>(Controller::Axis::AxisMax));

		m_state.axes[axis_id] = value;

		uint32_t trigger = 0;
		if (axis == Controller::Axis::TriggerLeft) {
			trigger = PAD_BUTTON_L2;
		} else if (axis == Controller::Axis::TriggerRight) {
			trigger = PAD_BUTTON_R2;
		}
		if (trigger != 0) {
			m_state.buttons = value > 0 ? m_state.buttons | trigger : m_state.buttons & ~trigger;
			UpdateTriggerEffectState();
		}

		AddState();
	}
}

void GameController::RightStick(int id, int x, int y) {
	Common::LockGuard lock(m_mutex);

	if (m_active_id == id || id == HOST_INPUT_CONTROLLER_ID) {
		m_state.time                                 = LibKernel::KernelGetProcessTime();
		m_state.axes[static_cast<int>(Axis::RightX)] = x;
		m_state.axes[static_cast<int>(Axis::RightY)] = y;
		AddState();
	}
}

void GameController::TouchPad(int id, int finger, bool down, float x, float y) {
	if (finger < 0 || finger >= 2) {
		return;
	}

	Common::LockGuard lock(m_mutex);
	if (m_active_id == id || id == HOST_INPUT_CONTROLLER_ID) {
		auto& touch  = m_state.touch[finger];
		m_state.time = LibKernel::KernelGetProcessTime();
		if (down && !touch.down) {
			touch.id        = m_next_touch_id;
			m_next_touch_id = m_next_touch_id == 127 ? 1 : m_next_touch_id + 1;
		}
		touch.down = down;
		touch.x    = static_cast<uint16_t>(std::clamp(x, 0.0f, 1.0f) * 1920.0f);
		touch.y    = static_cast<uint16_t>(std::clamp(y, 0.0f, 1.0f) * 943.0f);
		if (id == HOST_INPUT_CONTROLLER_ID) {
			m_state.buttons = down ? m_state.buttons | PAD_BUTTON_TOUCH_PAD
			                       : m_state.buttons & ~PAD_BUTTON_TOUCH_PAD;
		}
		AddState();
	}
}

namespace {

using Vec3 = std::array<float, 3>;
using Quat = std::array<float, 4>; // x, y, z, w

Vec3 QuatRotate(const Quat& q, const Vec3& v) {
	const Vec3 t {2.0f * (q[1] * v[2] - q[2] * v[1]), 2.0f * (q[2] * v[0] - q[0] * v[2]),
	              2.0f * (q[0] * v[1] - q[1] * v[0])};
	return {v[0] + q[3] * t[0] + (q[1] * t[2] - q[2] * t[1]),
	        v[1] + q[3] * t[1] + (q[2] * t[0] - q[0] * t[2]),
	        v[2] + q[3] * t[2] + (q[0] * t[1] - q[1] * t[0])};
}

Quat QuatConjugate(const Quat& q) {
	return {-q[0], -q[1], -q[2], q[3]};
}

float Length(const Vec3& v) {
	return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

} // namespace

void GameController::Motion(int id, Sensor sensor, const float* data, uint64_t time_us) {
	Common::LockGuard lock(m_mutex);
	if (id != m_active_id || !m_motion_enabled) {
		return;
	}

	// Convert acceleration to G; angular velocity is already in rad/s.
	if (sensor == Sensor::Accel) {
		for (int i = 0; i < 3; i++) {
			m_state.accel[i] = data[i] / SDL_STANDARD_GRAVITY;
		}
		// Capture gravity near 1 G to limit interference from linear acceleration.
		const float magnitude = Length(m_state.accel);
		if (!m_up_reference_valid && magnitude > 0.9f && magnitude < 1.1f) {
			const Vec3 up {m_state.accel[0] / magnitude, m_state.accel[1] / magnitude,
			               m_state.accel[2] / magnitude};
			m_up_reference       = QuatRotate(m_state.orientation, up);
			m_up_reference_valid = true;
		}
	} else {
		std::copy_n(data, 3, m_state.gyro.begin());
		// Mahony tilt correction; heading is unobservable and reported gyro values stay raw.
		auto        rate      = m_state.gyro;
		const float magnitude = Length(m_state.accel);
		if (m_up_reference_valid && magnitude > 0.8f && magnitude < 1.2f) {
			const Vec3 measured {m_state.accel[0] / magnitude, m_state.accel[1] / magnitude,
			                     m_state.accel[2] / magnitude};
			const Vec3 predicted = QuatRotate(QuatConjugate(m_state.orientation), m_up_reference);
			rate[0] += measured[1] * predicted[2] - measured[2] * predicted[1];
			rate[1] += measured[2] * predicted[0] - measured[0] * predicted[2];
			rate[2] += measured[0] * predicted[1] - measured[1] * predicted[0];
		}
		// Do not extrapolate a single sample across lost reports (e.g. loss of window focus).
		constexpr uint64_t max_gyro_interval_us = 100000;
		if (m_gyro_time != 0 && time_us > m_gyro_time &&
		    time_us - m_gyro_time <= max_gyro_interval_us) {
			const float dt         = static_cast<float>(time_us - m_gyro_time) * 0.000001f;
			const float speed      = Length(rate);
			const float half_angle = speed * dt * 0.5f;
			const float scale      = speed > 0.0f ? std::sin(half_angle) / speed : 0.0f;
			const float x          = rate[0] * scale;
			const float y          = rate[1] * scale;
			const float z          = rate[2] * scale;
			const float w          = std::cos(half_angle);
			const auto  q          = m_state.orientation;
			// Accumulate body-local rotation relative to connection / orientation reset.
			m_state.orientation = {q[3] * x + q[0] * w + q[1] * z - q[2] * y,
			                       q[3] * y - q[0] * z + q[1] * w + q[2] * x,
			                       q[3] * z + q[0] * y - q[1] * x + q[2] * w,
			                       q[3] * w - q[0] * x - q[1] * y - q[2] * z};
			float length        = 0.0f;
			for (float value: m_state.orientation) {
				length += value * value;
			}
			length = std::sqrt(length);
			for (float& value: m_state.orientation) {
				value /= length;
			}
		}
		m_gyro_time = time_us;
	}
	m_state.time = LibKernel::KernelGetProcessTime();
	AddState();
}

void GameController::SetMotionSensorState(bool enable) {
	Common::LockGuard lock(m_mutex);
	if (m_motion_enabled != enable) {
		m_motion_enabled     = enable;
		m_gyro_time          = 0;
		m_state.accel        = {0.0f, 1.0f, 0.0f};
		m_state.gyro         = {};
		m_up_reference_valid = false;
		m_state.time         = LibKernel::KernelGetProcessTime();
		AddState();
	}
}

void GameController::ResetOrientation() {
	Common::LockGuard lock(m_mutex);
	m_state.orientation  = {0.0f, 0.0f, 0.0f, 1.0f};
	m_gyro_time          = 0;
	m_up_reference_valid = false;
	m_state.time        = LibKernel::KernelGetProcessTime();
	AddState();
}

void GameController::ResetInputState() {
	Common::LockGuard lock(m_mutex);
	m_state.buttons = 0;
	std::fill_n(m_state.axes, 4, 128);
	std::fill_n(m_state.axes + 4, 2, 0);
	UpdateTriggerEffectState();
	for (auto& touch: m_state.touch) {
		touch = {};
	}
	m_state.time    = LibKernel::KernelGetProcessTime();
	m_states_num    = 0;
	m_first_state   = 0;
	m_next_touch_id = 1;
	AddState();
}

void GameController::ReleaseHostPads() {
	Common::LockGuard lock(m_mutex);
	DualSenseHaptics::Shutdown();

	std::vector<SDL_Gamepad*> pads;
	for (const auto id: m_connected_ids) {
		if (id == HOST_INPUT_CONTROLLER_ID) {
			continue;
		}
		if (auto* pad = SDL_GetGamepadFromID(static_cast<SDL_JoystickID>(id));
		    pad != nullptr) {
			if (SDL_GetGamepadType(pad) == SDL_GAMEPAD_TYPE_PS5) {
				DualSenseEffects effect {};
				effect.enable_bits     = 0x0c;
				effect.right_trigger[0] = 0x05;
				effect.left_trigger[0]  = 0x05;
				(void)SDL_SendGamepadEffect(pad, &effect, sizeof(effect));
			}
			(void)SDL_RumbleGamepad(pad, 0, 0, 0);
			(void)SDL_SetGamepadLED(pad, 0, 0, 0);
			pads.push_back(pad);
		}
	}

	if (!pads.empty()) {
		SDL_Delay(RELEASE_FLUSH_MS);
		for (auto* pad: pads) {
			SDL_CloseGamepad(pad);
		}
	}

	m_connected_ids.clear();
	CheckActive();
}

void GameController::SetVibration(uint8_t large_motor, uint8_t small_motor) {
	Common::LockGuard lock(m_mutex);
	m_vibration       = {large_motor, small_motor};
	m_vibration_until = SDL_GetTicks() + RUMBLE_DURATION_MS;
	ApplyVibration();
}

void GameController::ApplyVibration() {
	const auto now = SDL_GetTicks();
	const auto duration =
	    m_vibration_until > now ? static_cast<uint32_t>(m_vibration_until - now) : 0;
	const auto scale       = duration != 0 ? GetSettingScale(Setting::VibrationIntensity) : 0.0f;
	const auto large_motor = Scale(m_vibration[0], scale);
	const auto small_motor = Scale(m_vibration[1], scale);
	if (DualSenseHaptics::SetVibration(m_active_id, large_motor, small_motor, duration)) {
		return;
	}

	if (m_active_id == HOST_INPUT_CONTROLLER_ID) {
		return;
	}

	auto* pad = SDL_GetGamepadFromID(static_cast<SDL_JoystickID>(m_active_id));
	if (pad == nullptr) {
		return;
	}

	const auto large = static_cast<uint16_t>(large_motor * 0x101U);
	const auto small = static_cast<uint16_t>(small_motor * 0x101U);
	if (!SDL_RumbleGamepad(pad, large, small, duration)) {
		LOGF("\t rumble failed: %s\n", SDL_GetError());
	}
}

int GameController::GetActiveControllerId() {
	Common::LockGuard lock(m_mutex);
	return m_active_id;
}

void GameController::SetLightBar(uint8_t r, uint8_t g, uint8_t b) {
	Common::LockGuard lock(m_mutex);
	if (const auto& color = Config::GetControllerColor()) {
		r = (*color)[0];
		g = (*color)[1];
		b = (*color)[2];
	}
	if (auto* pad = SDL_GetGamepadFromID(static_cast<SDL_JoystickID>(m_active_id));
	    pad != nullptr) {
		(void)SDL_SetGamepadLED(pad, r, g, b);
	}
}

bool GameController::SetTriggerEffect(const PadTriggerEffectParam& param) {
	Common::LockGuard lock(m_mutex);
	if (!SendTriggerEffect(param)) {
		return false;
	}
	for (int i = 0; i < 2; i++) {
		if ((param.trigger_mask & (1u << i)) != 0) {
			m_trigger_effect.trigger_mask |= static_cast<uint8_t>(1u << i);
			m_trigger_effect.command[i] = param.command[i];
		}
	}
	UpdateTriggerEffectState();
	return true;
}

void GameController::GetTriggerEffectState(PadTriggerEffectStateInformation* info) {
	Common::LockGuard lock(m_mutex);
	*info = m_trigger_state;
}

void GameController::UpdateTriggerEffectState() {
	// Track every input event so a weapon remains fired even when the game polls after release
	// starts.
	for (int i = 0; i < 2; i++) {
		m_trigger_state.state[i] = TriggerEffectState(
		    m_trigger_effect.command[i], m_state.axes[static_cast<int>(Axis::TriggerLeft) + i],
		    m_trigger_state.state[i]);
	}
}

bool GameController::SendTriggerEffect(const PadTriggerEffectParam& param) {
	if ((param.trigger_mask & ~0x03u) != 0) {
		return false;
	}

	DualSenseEffects effect {};
	const auto       scale = GetSettingScale(Setting::TriggerEffectIntensity);
	if ((param.trigger_mask & 0x01u) != 0) {
		effect.enable_bits |= 0x08;
		if (!trigger_effect_to_dualsense(param.command[0], effect.left_trigger, scale)) {
			return false;
		}
	}
	if ((param.trigger_mask & 0x02u) != 0) {
		effect.enable_bits |= 0x04;
		if (!trigger_effect_to_dualsense(param.command[1], effect.right_trigger, scale)) {
			return false;
		}
	}
	if (effect.enable_bits == 0) {
		return true;
	}

	auto* pad = SDL_GetGamepadFromID(static_cast<SDL_JoystickID>(m_active_id));
	if (pad != nullptr && SDL_GetGamepadType(pad) == SDL_GAMEPAD_TYPE_PS5) {
		(void)SDL_SendGamepadEffect(pad, &effect, sizeof(effect));
	}
	return true;
}

void GameController::GetConnectionInfo(bool* flag, int* count) {
	EXIT_IF(flag == nullptr);
	EXIT_IF(count == nullptr);

	Common::LockGuard lock(m_mutex);

	*flag  = m_connected;
	*count = m_connected_count;
}

void GameController::ReadState(ControllerState* state, bool* flag, int* count) {
	EXIT_IF(flag == nullptr);
	EXIT_IF(count == nullptr);
	EXIT_IF(state == nullptr);

	Common::LockGuard lock(m_mutex);

	*flag  = m_connected;
	*count = m_connected_count;
	*state = m_state;
}

int GameController::ReadStates(ControllerState* states, int states_num, bool* flag, int* count) {
	EXIT_IF(flag == nullptr);
	EXIT_IF(count == nullptr);
	EXIT_IF(states == nullptr);
	EXIT_IF(states_num < 1 || states_num > STATES_MAX);

	Common::LockGuard lock(m_mutex);

	*flag  = m_connected;
	*count = m_connected_count;

	int ret_num = 0;

	if (m_connected) {
		if (m_states_num != 0) {
			for (uint32_t i = 0; i < m_states_num; i++) {
				if (ret_num >= states_num) {
					break;
				}
				auto index = (m_first_state + i) % STATES_MAX;
				if (!m_obtained[index]) {
					m_obtained[index] = true;

					states[ret_num++] = m_states[index];
				}
			}
		}
	}

	return ret_num;
}

void Connect(int id) {
	g_controller->Connect(id);
}

void Disconnect(int id) {
	g_controller->Disconnect(id);
}

void SetButton(int id, uint32_t button, bool down) {
	g_controller->Button(id, button, down);
}

void SetAxis(int id, Axis axis, int value) {
	g_controller->Axis(id, axis, value);
}

void SetRightStick(int id, int x, int y) {
	g_controller->RightStick(id, x, y);
}

void SetTouchPad(int id, int finger, bool down, float x, float y) {
	g_controller->TouchPad(id, finger, down, x, y);
}

void SetSensor(int id, Sensor sensor, const float* data, uint64_t time_us) {
	g_controller->Motion(id, sensor, data, time_us);
}

void ResetInputState() {
	g_controller->ResetInputState();
}

int GetActiveControllerId() {
	return g_controller != nullptr ? g_controller->GetActiveControllerId() : -1;
}

static bool PadOpenArgsAreValid(int user_id, int type, int index) {
	// The host keyboard and gamepad feed one standard port for the configured user.
	return user_id == Config::GetUserId() && type == 0 && index == 0;
}

int GameController::InitPort() {
	Common::LockGuard lock(m_mutex);
	if (m_port_state == PortState::Uninitialized) {
		m_port_state = PortState::Closed;
	}
	return OK;
}

int GameController::OpenPort(int user_id, int type, int index) {
	Common::LockGuard lock(m_mutex);
	if (m_port_state == PortState::Uninitialized) {
		return PAD_ERROR_NOT_INITIALIZED;
	}
	if (!PadOpenArgsAreValid(user_id, type, index)) {
		return PAD_ERROR_INVALID_ARG;
	}
	if (m_port_state == PortState::Open) {
		return PAD_ERROR_ALREADY_OPENED;
	}
	m_port_state = PortState::Open;
	return PAD_HANDLE;
}

int GameController::GetPortHandle(int user_id, int type, int index) {
	Common::LockGuard lock(m_mutex);
	if (m_port_state == PortState::Uninitialized) {
		return PAD_ERROR_NOT_INITIALIZED;
	}
	return m_port_state == PortState::Open && PadOpenArgsAreValid(user_id, type, index) ? PAD_HANDLE
	                                                                               : PAD_ERROR_NO_HANDLE;
}

int GameController::CheckPortHandleLocked(int handle) const {
	if (m_port_state == PortState::Uninitialized) {
		return PAD_ERROR_NOT_INITIALIZED;
	}
	return m_port_state == PortState::Open && handle == PAD_HANDLE ? OK : PAD_ERROR_INVALID_HANDLE;
}

int GameController::CheckPortHandle(int handle) {
	Common::LockGuard lock(m_mutex);
	return CheckPortHandleLocked(handle);
}

int GameController::ClosePort(int handle) {
	Common::LockGuard lock(m_mutex);
	if (const auto error = CheckPortHandleLocked(handle); error != OK) {
		return error;
	}
	m_port_state = PortState::Closed;
	return OK;
}

int PadCheckHandle(int handle) {
	return g_controller->CheckPortHandle(handle);
}

int KYTY_SYSV_ABI PadInit() {
	PRINT_NAME();
	return g_controller->InitPort();
}

int KYTY_SYSV_ABI PadOpen(int user_id, int type, int index, const void* param) {
	PRINT_NAME();

	LOGF("\t user_id = %d\n"
	     "\t type    = %d\n"
	     "\t index   = %d\n"
	     "\t param   = 0x%016" PRIx64 "\n",
	     user_id, type, index, reinterpret_cast<uint64_t>(param));

	return g_controller->OpenPort(user_id, type, index);
}

int KYTY_SYSV_ABI PadGetHandle(int user_id, int type, int index) {
	PRINT_NAME();

	LOGF("\t user_id = %d\n"
	     "\t type    = %d\n"
	     "\t index   = %d\n",
	     user_id, type, index);

	return g_controller->GetPortHandle(user_id, type, index);
}

int KYTY_SYSV_ABI PadClose(int handle) {
	PRINT_NAME();
	LOGF("\t handle = %d\n", handle);
	return g_controller->ClosePort(handle);
}

int KYTY_SYSV_ABI PadSetMotionSensorState(int handle, bool enable) {
	PRINT_NAME();

	if (const auto error = PadCheckHandle(handle); error != OK) {
		return error;
	}

	LOGF("\t enable = %s\n", (enable ? "true" : "false"));
	g_controller->SetMotionSensorState(enable);

	return OK;
}

int KYTY_SYSV_ABI PadSetAngularVelocityDeadbandState(int handle, bool enable) {
	PRINT_NAME();

	if (const auto error = PadCheckHandle(handle); error != OK) {
		return error;
	}

	LOGF("\t enable = %s\n", (enable ? "true" : "false"));

	return OK;
}

int KYTY_SYSV_ABI PadResetOrientation(int handle) {
	PRINT_NAME();

	if (const auto error = PadCheckHandle(handle); error != OK) {
		return error;
	}

	g_controller->ResetOrientation();
	return OK;
}

int KYTY_SYSV_ABI PadGetControllerInformation(int handle, PadControllerInformation* info) {
	PRINT_NAME();

	if (const auto error = PadCheckHandle(handle); error != OK) {
		return error;
	}
	if (info == nullptr) {
		return PAD_ERROR_INVALID_ARG;
	}

	int  connected_count = 0;
	bool connected       = false;
	g_controller->GetConnectionInfo(&connected, &connected_count);

	std::memset(info, 0, sizeof(*info));

	info->touch_pixel_density   = 44.86f;
	info->touch_resolution_x    = 1920;
	info->touch_resolution_y    = 943;
	info->stick_dead_zone_left  = controller_get_axis(-32768, 32767, 8000) - 128;
	info->stick_dead_zone_right = controller_get_axis(-32768, 32767, 8000) - 128;
	info->connection_type       = 0;
	info->connected_count       = static_cast<uint8_t>(std::min(connected_count, 255));
	info->connected             = connected;
	info->device_class          = 0;

	return OK;
}

int KYTY_SYSV_ABI PadIsRemoteController(int handle, bool* is_remote) {
	PRINT_NAME();

	if (const auto error = PadCheckHandle(handle); error != OK) {
		return error;
	}
	if (is_remote == nullptr) {
		return PAD_ERROR_INVALID_ARG;
	}

	*is_remote = false;
	return OK;
}

int KYTY_SYSV_ABI PadReadState(int handle, PadData* data) {
	PRINT_NAME();

	if (const auto error = PadCheckHandle(handle); error != OK) {
		return error;
	}
	if (data == nullptr) {
		return PAD_ERROR_INVALID_ARG;
	}

	int             connected_count = 0;
	bool            connected       = false;
	ControllerState state;

	g_controller->ReadState(&state, &connected, &connected_count);

	pad_fill_data(data, state, connected, connected_count);

	return OK;
}

int KYTY_SYSV_ABI PadRead(int handle, PadData* data, int num) {
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(num < 1 || num > 64);
	if (const auto error = PadCheckHandle(handle); error != OK) {
		return error;
	}
	if (data == nullptr) {
		return PAD_ERROR_INVALID_ARG;
	}

	std::memset(data, 0, sizeof(PadData) * static_cast<size_t>(num));

	int             connected_count = 0;
	bool            connected       = false;
	ControllerState states[64]      = {};

	int ret_num = g_controller->ReadStates(states, num, &connected, &connected_count);

	if (!connected || ret_num == 0) {
		if (connected) {
			g_controller->ReadState(&states[0], &connected, &connected_count);
		}
		ret_num = 1;
	}

	for (int i = 0; i < ret_num; i++) {
		pad_fill_data(&data[i], states[i], connected, connected_count);
	}

	return ret_num;
}

int KYTY_SYSV_ABI PadSetVibration(int handle, const PadVibrationParam* param) {
	PRINT_NAME();

	if (const auto error = PadCheckHandle(handle); error != OK) {
		return error;
	}
	if (param == nullptr) {
		return PAD_ERROR_INVALID_ARG;
	}

	LOGF("\t large_motor = %d\n"
	     "\t small_motor = %d\n",
	     static_cast<int>(param->large_motor), static_cast<int>(param->small_motor));

	g_controller->SetVibration(param->large_motor, param->small_motor);

	return OK;
}

int KYTY_SYSV_ABI PadResetLightBar(int handle) {
	PRINT_NAME();

	if (const auto error = PadCheckHandle(handle); error != OK) {
		return error;
	}

	return OK;
}

int KYTY_SYSV_ABI PadSetLightBar(int handle, const PadLightBarParam* param) {
	PRINT_NAME();

	if (const auto error = PadCheckHandle(handle); error != OK) {
		return error;
	}
	if (param == nullptr) {
		return PAD_ERROR_INVALID_ARG;
	}

	g_controller->SetLightBar(param->r, param->g, param->b);

	return OK;
}

int KYTY_SYSV_ABI PadSetTriggerEffect(int handle, const PadTriggerEffectParam* param) {
	PRINT_NAME();

	if (const auto error = PadCheckHandle(handle); error != OK) {
		return error;
	}
	if (param == nullptr) {
		return PAD_ERROR_INVALID_ARG;
	}

	return g_controller->SetTriggerEffect(*param) ? OK : PAD_ERROR_INVALID_ARG;
}

int KYTY_SYSV_ABI PadGetTriggerEffectState(int handle, PadTriggerEffectStateInformation* info) {
	PRINT_NAME();

	LOGF("\t handle = %d\n", handle);

	if (const auto error = PadCheckHandle(handle); error != OK) {
		return error;
	}
	if (info == nullptr) {
		return PAD_ERROR_INVALID_ARG;
	}

	g_controller->GetTriggerEffectState(info);

	return OK;
}

} // namespace Libs::Controller
