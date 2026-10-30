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
#include "libs/padInput.h"
#include "libs/playerSeats.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <span>
#include <vector>

namespace Libs::Controller {

LIB_NAME("Pad", "Pad");

constexpr int PAD_TYPE_STANDARD = 0;
constexpr int PAD_TYPE_SPECIAL  = 2;
static_assert(2 * PlayerSeats::MAX_PLAYERS < PAD_HANDLE_VR_LEFT);
static_assert(PlayerSeats::MAX_PLAYERS == Config::MAX_PLAYERS);

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

class GameController {
public:
	GameController(): m_seats(PlayerUsers()) {
		for (int index = 0; index < PlayerSeats::MAX_PLAYERS; ++index) {
			m_players[index].index = index;
		}
		const auto keyboard = Config::GetKeyboardPlayer();
		(void)m_seats.SetKeyboard(keyboard == 0 ? PlayerSeats::KEYBOARD_SHARED
		                                        : static_cast<int>(keyboard) - 1);
	}

	KYTY_CLASS_NO_COPY(GameController);

	int InitPort();
	int OpenPort(int user_id, int type, int index);
	int GetPortHandle(int user_id, int type, int index);
	int ClosePort(int handle);
	int CheckPortHandle(int handle);

	void Connect(int id);
	void Disconnect(int id);
	void Seat(int id);
	void CycleKeyboardPlayer();
	void ToggleGyroEmulation(int id);
	bool IsPlayerLoggedIn(int player);
	bool Initialized();
	void Button(int id, uint32_t button, bool down);
	void Axis(int id, Axis axis, int value);
	void RightStick(int id, int x, int y);
	void TouchPad(int id, int finger, bool down, float x, float y);
	void Motion(int id, Sensor sensor, const float* data, uint64_t time_us);
	void ResetInputState();
	void ReleaseHostPads();
	int  GetActiveControllerId();
	int  GetGamepadOfPlayer(int player);
	void CycleSetting(Setting setting);
	float GetSettingScale(Setting setting) const;

	bool TakePlayerEvent(bool* login, int* user_id);
	void GetLoggedInUsers(int* user_ids);
	int  GetPlayerOfUser(int user_id);

	// For an open port: `handle` is valid (CheckPortHandle).
	void SetMotionSensorState(int handle, bool enable);
	void ResetOrientation(int handle);
	void GetConnectionInfo(int handle, bool* flag, int* count);
	void SetVibration(int handle, uint8_t large_motor, uint8_t small_motor);
	void SetLightBar(int handle, uint8_t r, uint8_t g, uint8_t b);
	bool SetTriggerEffect(int handle, const PadTriggerEffectParam& param);
	void GetTriggerEffectState(int handle, PadTriggerEffectStateInformation* info);
	void ReadState(int handle, PadState* state, bool* flag, int* count);
	int  ReadStates(int handle, std::span<PadState> states, bool* flag, int* count);

private:
	// One player: a port, the input of its sources, and the outputs the game asked for. Player 1
	// has the keyboard and every gamepad without a seat; another player has its seated gamepad.
	struct Player {
		int      index          = 0;
		bool     open           = false;
		// The special port (arcade sticks and the like). No such controller is emulated: it reads
		// as no controller, and what the game sends to it goes nowhere.
		bool     special_open   = false;
		PadInput input;
		bool     motion_enabled = true;
		uint64_t gyro_time      = 0;
		// Emulated tilt (ToggleGyroEmulation): the right stick drives pitch and roll instead of
		// the stick axes, for a pad without a gyroscope.
		bool               gyro_emulation = false;
		uint64_t           emulated_time  = 0;
		std::array<int, 2> emulated_stick {128, 128};
		// Accelerometer gravity direction in the reset frame.
		std::array<float, 3> up_reference {0.0f, 1.0f, 0.0f};
		bool                 up_reference_valid = false;
		// The game's last requests, re-sent when their intensity setting changes.
		std::array<uint8_t, 2>           vibration {};
		uint64_t                         vibration_until = 0;
		PadTriggerEffectParam            trigger_effect {};
		PadTriggerEffectStateInformation trigger_state {};

		bool& PortOpen(bool special) { return special ? special_open : open; }
		[[nodiscard]] bool PortOpen(bool special) const { return special ? special_open : open; }
		// A new session with another gamepad: what the game set for the last is forgotten. The
		// ports stay as they are.
		void StartSession() {
			motion_enabled     = true;
			gyro_time          = 0;
			gyro_emulation     = false;
			emulated_time      = 0;
			emulated_stick     = {128, 128};
			up_reference_valid = false;
			vibration          = {};
			vibration_until    = 0;
			trigger_effect     = {};
			trigger_state      = {};
		}
	};

	// The handle of a player's standard port is its index plus one; its special port follows the
	// standard ports.
	static int  HandleOf(int player, bool special) {
		return player + 1 + (special ? PlayerSeats::MAX_PLAYERS : 0);
	}
	static bool IsSpecial(int handle) { return handle > PlayerSeats::MAX_PLAYERS; }
	// The player of a valid handle (CheckPortHandle), whichever port it names.
	Player&       PlayerOfHandle(int handle) {
		return m_players[(handle - 1) % PlayerSeats::MAX_PLAYERS];
	}
	const Player& PlayerOfHandle(int handle) const {
		return m_players[(handle - 1) % PlayerSeats::MAX_PLAYERS];
	}
	// The player whose gamepad a valid handle reads and drives, or nullptr for a special port.
	Player* GamepadPlayer(int handle) { return IsSpecial(handle) ? nullptr : &PlayerOfHandle(handle); }
	// The input a valid handle reads: a special port reads no controller.
	PadInput& InputOf(int handle) {
		return IsSpecial(handle) ? m_no_controller : PlayerOfHandle(handle).input;
	}
	// The player a source plays as: the keyboard is its player's (player 1's unless the keyboard
	// has a player of its own); a gamepad without a seat is nobody's (nullptr).
	Player* PlayerOfSource(int id) {
		if (id == HOST_INPUT_CONTROLLER_ID) {
			return &m_players[m_seats.KeyboardPlayer()];
		}
		const int player = m_seats.PlayerOf(id);
		return player != PlayerSeats::NO_PLAYER ? &m_players[player] : nullptr;
	}
	// A press of gamepad `id`: it takes a seat when it has none. Caller holds m_mutex.
	void SeatLocked(int id);
	// The game's outputs stop on gamepad `id`: rumble and trigger effects.
	void StopOutputs(int id);

	int CheckPortHandleLocked(int handle) const;

	// After a change of a player's input: a new active gamepad starts without the outputs of the
	// last, and the trigger effect states follow the triggers. Caller holds m_mutex.
	void AfterInput(Player& player, int previous_active_id);
	// Apply cached output without changing its game-requested lifetime; caller holds m_mutex.
	void ApplyVibration(Player& player);
	bool SendTriggerEffect(Player& player, const PadTriggerEffectParam& param);
	void UpdateTriggerEffectState(Player& player);
	void PrepareGamepad(int id);
	// Sets the emulated tilt from player.emulated_stick. Caller holds m_mutex.
	void UpdateEmulatedTilt(Player& player);

	static std::array<int32_t, PlayerSeats::MAX_PLAYERS> PlayerUsers() {
		std::array<int32_t, PlayerSeats::MAX_PLAYERS> users {};
		for (int player = 0; player < PlayerSeats::MAX_PLAYERS; ++player) {
			users[player] = Config::GetPlayerUserId(player);
		}
		return users;
	}

	Common::Mutex m_mutex;
	bool          m_initialized = false;
	PlayerSeats   m_seats;
	// The host gamepads that are connected, seated or not.
	std::vector<int> m_gamepads;
	std::array<Player, PlayerSeats::MAX_PLAYERS> m_players;
	// What a special port reads: an input with no source.
	PadInput m_no_controller;
	// Setting changes share the output lock; audio only needs an atomic scale snapshot.
	std::array<std::atomic<uint32_t>, 3> m_setting_steps {};
};

static GameController* g_controller = nullptr;

static void pad_fill_data(PadData* data, const PadState& state, bool connected,
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
	data->acceleration_x     = state.motion.accel[0];
	data->acceleration_y     = state.motion.accel[1];
	data->acceleration_z     = state.motion.accel[2];
	data->angular_velocity_x = state.motion.gyro[0];
	data->angular_velocity_y = state.motion.gyro[1];
	data->angular_velocity_z = state.motion.gyro[2];
	data->orientation_x      = state.motion.orientation[0];
	data->orientation_y      = state.motion.orientation[1];
	data->orientation_z      = state.motion.orientation[2];
	data->orientation_w      = state.motion.orientation[3];
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
	for (int index = 0; index < PlayerSeats::MAX_PLAYERS; ++index) {
		if (!m_seats.IsLoggedIn(index)) {
			continue;
		}
		auto& player = m_players[index];
		if (setting == Setting::VibrationIntensity) {
			ApplyVibration(player);
		} else if (setting == Setting::TriggerEffectIntensity) {
			(void)SendTriggerEffect(player, player.trigger_effect);
		}
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

void GameController::PrepareGamepad(int id) {
	auto* pad = SDL_GetGamepadFromID(static_cast<SDL_JoystickID>(id));
	if (pad == nullptr) {
		return;
	}
	if (const auto& color = Config::GetControllerColor()) {
		(void)SDL_SetGamepadLED(pad, (*color)[0], (*color)[1], (*color)[2]);
	}
	for (auto sensor: {SDL_SENSOR_ACCEL, SDL_SENSOR_GYRO}) {
		if (SDL_GamepadHasSensor(pad, sensor) && !SDL_SetGamepadSensorEnabled(pad, sensor, true)) {
			LOGF("\t enabling controller sensor failed: %s\n", SDL_GetError());
		}
	}
}

void GameController::Connect(int id) {
	Common::LockGuard lock(m_mutex);
	if (id == HOST_INPUT_CONTROLLER_ID) {
		auto&     player             = m_players[m_seats.KeyboardPlayer()];
		const int previous_active_id = player.input.ActiveId();
		if (player.input.Connect(id)) {
			AfterInput(player, previous_active_id);
		}
		return;
	}
	// A gamepad plays from its first button press, which gives it a seat (SeatLocked).
	if (std::ranges::find(m_gamepads, id) == m_gamepads.end()) {
		m_gamepads.push_back(id);
		PrepareGamepad(id);
	}
}

void GameController::Disconnect(int id) {
	Common::LockGuard lock(m_mutex);
	std::erase(m_gamepads, id);
	auto* player = PlayerOfSource(id);
	if (player == nullptr) {
		return;
	}
	const int previous_active_id = player->input.ActiveId();
	player->input.Disconnect(id);
	AfterInput(*player, previous_active_id);
	if (const int left = m_seats.Leave(id); left > 0) {
		m_players[left].StartSession();
		LOGF("Controller: player %d logged out with gamepad %d\n", left + 1, id);
	}
}

void GameController::Seat(int id) {
	Common::LockGuard lock(m_mutex);
	SeatLocked(id);
}

void GameController::SeatLocked(int id) {
	if (id == HOST_INPUT_CONTROLLER_ID || m_seats.PlayerOf(id) != PlayerSeats::NO_PLAYER ||
	    std::ranges::find(m_gamepads, id) == m_gamepads.end()) {
		return;
	}
	const int seat = m_seats.Seat(id);
	if (seat == PlayerSeats::NO_PLAYER) {
		LOGF("Controller: gamepad %d plays as nobody: every seat is taken\n", id);
		return;
	}
	auto& player = m_players[seat];
	if (seat != 0) {
		player.StartSession();
	}
	const int previous_active = player.input.ActiveId();
	(void)player.input.Connect(id);
	AfterInput(player, previous_active);
	LOGF("Controller: player %d plays with gamepad %d\n", seat + 1, id);
}

void GameController::CycleKeyboardPlayer() {
	Common::LockGuard lock(m_mutex);
	// Shared with player 1, then players 1 to 4 on their own.
	const int  keyboard = m_seats.Keyboard();
	const int  next     = keyboard == PlayerSeats::KEYBOARD_SHARED ? 0
	                      : keyboard + 1 < PlayerSeats::MAX_PLAYERS ? keyboard + 1
	                                                              : PlayerSeats::KEYBOARD_SHARED;
	auto&      old_player = m_players[m_seats.KeyboardPlayer()];
	const auto move       = m_seats.SetKeyboard(next);
	auto&      new_player = m_players[m_seats.KeyboardPlayer()];
	if (&old_player != &new_player) {
		const int previous_active_id = old_player.input.ActiveId();
		old_player.input.Disconnect(HOST_INPUT_CONTROLLER_ID);
		AfterInput(old_player, previous_active_id);
	}
	if (move.pad != PlayerSeats::NO_PAD) {
		auto&     from               = m_players[move.from];
		const int previous_active_id = from.input.ActiveId();
		from.input.Disconnect(move.pad);
		AfterInput(from, previous_active_id);
		if (move.to != PlayerSeats::NO_PLAYER) {
			auto&     to         = m_players[move.to];
			const int to_active  = to.input.ActiveId();
			to.StartSession();
			(void)to.input.Connect(move.pad);
			AfterInput(to, to_active);
		}
		LOGF("Controller: gamepad %d moves from player %d to player %d\n", move.pad,
		     move.from + 1, move.to + 1);
	}
	if (&old_player != &new_player) {
		const int previous_active_id = new_player.input.ActiveId();
		(void)new_player.input.Connect(HOST_INPUT_CONTROLLER_ID);
		AfterInput(new_player, previous_active_id);
	}
	if (next == PlayerSeats::KEYBOARD_SHARED) {
		LOGF("Controller: the keyboard shares player 1\n");
	} else {
		LOGF("Controller: the keyboard plays as player %d\n", next + 1);
	}
}

bool GameController::IsPlayerLoggedIn(int player) {
	Common::LockGuard lock(m_mutex);
	return m_seats.IsLoggedIn(player);
}

bool GameController::Initialized() {
	Common::LockGuard lock(m_mutex);
	return m_initialized;
}

void GameController::AfterInput(Player& player, int previous_active_id) {
	if (player.input.ActiveId() != previous_active_id) {
		LOGF("Controller: player %d uses gamepad %d\n",
		     player.index + 1, player.input.ActiveId());
		player.gyro_time          = 0;
		player.up_reference_valid = false;
		player.gyro_emulation     = false;
		player.emulated_time      = 0;
		player.emulated_stick     = {128, 128};
		player.vibration          = {};
		player.vibration_until    = 0;
		player.trigger_effect     = {};
		player.trigger_state      = {};
	}
	UpdateTriggerEffectState(player);
}

void GameController::Button(int id, uint32_t button, bool down) {
	Common::LockGuard lock(m_mutex);
	// The press that gives a gamepad its seat is the first one its player gets.
	if (down) {
		SeatLocked(id);
	}
	auto* player = PlayerOfSource(id);
	if (player == nullptr) {
		return;
	}
	const int previous_active_id = player->input.ActiveId();
	player->input.SetButton(id, button, down, LibKernel::KernelGetProcessTime());
	AfterInput(*player, previous_active_id);
}

void GameController::Axis(int id, Controller::Axis axis, int value) {
	Common::LockGuard lock(m_mutex);
	auto*             player = PlayerOfSource(id);
	if (player == nullptr) {
		return;
	}
	if (player->gyro_emulation && (axis == Controller::Axis::RightX || axis == Controller::Axis::RightY)) {
		player->emulated_stick[axis == Controller::Axis::RightY ? 1 : 0] = value;
		UpdateEmulatedTilt(*player);
		return;
	}
	const int previous_active_id = player->input.ActiveId();
	player->input.SetAxis(id, axis, value, LibKernel::KernelGetProcessTime());
	AfterInput(*player, previous_active_id);
}

void GameController::RightStick(int id, int x, int y) {
	Common::LockGuard lock(m_mutex);
	auto*             player = PlayerOfSource(id);
	if (player == nullptr) {
		return;
	}
	if (player->gyro_emulation) {
		player->emulated_stick = {x, y};
		UpdateEmulatedTilt(*player);
		return;
	}
	const int previous_active_id = player->input.ActiveId();
	player->input.SetRightStick(id, x, y, LibKernel::KernelGetProcessTime());
	AfterInput(*player, previous_active_id);
}

void GameController::TouchPad(int id, int finger, bool down, float x, float y) {
	Common::LockGuard lock(m_mutex);
	auto*             player = PlayerOfSource(id);
	if (player == nullptr) {
		return;
	}
	const int previous_active_id = player->input.ActiveId();
	player->input.SetTouch(id, finger, down,
	                      static_cast<uint16_t>(std::clamp(x, 0.0f, 1.0f) * 1920.0f),
	                      static_cast<uint16_t>(std::clamp(y, 0.0f, 1.0f) * 943.0f),
	                      LibKernel::KernelGetProcessTime());
	AfterInput(*player, previous_active_id);
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

Quat QuatMultiply(const Quat& a, const Quat& b) {
	return {a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
	        a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
	        a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3],
	        a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2]};
}

float Length(const Vec3& v) {
	return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

} // namespace

void GameController::Motion(int id, Sensor sensor, const float* data, uint64_t time_us) {
	Common::LockGuard lock(m_mutex);
	auto*             source_player = PlayerOfSource(id);
	if (source_player == nullptr) {
		return;
	}
	auto& player = *source_player;
	if (id != player.input.ActiveId() || !player.motion_enabled) {
		return;
	}
	PadMotion motion = player.input.Latest().motion;

	// Convert acceleration to G; angular velocity is already in rad/s.
	if (sensor == Sensor::Accel) {
		for (int i = 0; i < 3; i++) {
			motion.accel[i] = data[i] / SDL_STANDARD_GRAVITY;
		}
		// Capture gravity near 1 G to limit interference from linear acceleration.
		const float magnitude = Length(motion.accel);
		if (!player.up_reference_valid && magnitude > 0.9f && magnitude < 1.1f) {
			const Vec3 up {motion.accel[0] / magnitude, motion.accel[1] / magnitude,
			               motion.accel[2] / magnitude};
			player.up_reference       = QuatRotate(motion.orientation, up);
			player.up_reference_valid = true;
		}
	} else {
		std::copy_n(data, 3, motion.gyro.begin());
		// Mahony tilt correction; heading is unobservable and reported gyro values stay raw.
		auto        rate      = motion.gyro;
		const float magnitude = Length(motion.accel);
		if (player.up_reference_valid && magnitude > 0.8f && magnitude < 1.2f) {
			const Vec3 measured {motion.accel[0] / magnitude, motion.accel[1] / magnitude,
			                     motion.accel[2] / magnitude};
			const Vec3 predicted =
			    QuatRotate(QuatConjugate(motion.orientation), player.up_reference);
			rate[0] += measured[1] * predicted[2] - measured[2] * predicted[1];
			rate[1] += measured[2] * predicted[0] - measured[0] * predicted[2];
			rate[2] += measured[0] * predicted[1] - measured[1] * predicted[0];
		}
		// Do not extrapolate a single sample across lost reports (e.g. loss of window focus).
		constexpr uint64_t max_gyro_interval_us = 100000;
		if (player.gyro_time != 0 && time_us > player.gyro_time &&
		    time_us - player.gyro_time <= max_gyro_interval_us) {
			const float dt         = static_cast<float>(time_us - player.gyro_time) * 0.000001f;
			const float speed      = Length(rate);
			const float half_angle = speed * dt * 0.5f;
			const float scale      = speed > 0.0f ? std::sin(half_angle) / speed : 0.0f;
			const float x          = rate[0] * scale;
			const float y          = rate[1] * scale;
			const float z          = rate[2] * scale;
			const float w          = std::cos(half_angle);
			const auto  q          = motion.orientation;
			// Accumulate body-local rotation relative to connection / orientation reset.
			motion.orientation = {q[3] * x + q[0] * w + q[1] * z - q[2] * y,
			                      q[3] * y - q[0] * z + q[1] * w + q[2] * x,
			                      q[3] * z + q[0] * y - q[1] * x + q[2] * w,
			                      q[3] * w - q[0] * x - q[1] * y - q[2] * z};
			float length       = 0.0f;
			for (float value: motion.orientation) {
				length += value * value;
			}
			length = std::sqrt(length);
			for (float& value: motion.orientation) {
				value /= length;
			}
		}
		player.gyro_time = time_us;
	}
	player.input.SetMotion(motion, LibKernel::KernelGetProcessTime());
}

void GameController::UpdateEmulatedTilt(Player& player) {
	constexpr float max_angle = 0.7853982f; // 45 degrees at full stick deflection
	const auto      angle     = [](int value) {
        return std::clamp((value - 128) / 127.0f, -1.0f, 1.0f) * max_angle;
	};
	const float roll  = angle(player.emulated_stick[0]);
	const float pitch = angle(player.emulated_stick[1]);

	const Quat q_pitch {std::sin(pitch * 0.5f), 0.0f, 0.0f, std::cos(pitch * 0.5f)};
	const Quat q_roll {0.0f, 0.0f, std::sin(-roll * 0.5f), std::cos(-roll * 0.5f)};
	const Quat q = QuatMultiply(q_roll, q_pitch);

	const uint64_t now = LibKernel::KernelGetProcessTime();
	PadMotion      motion;
	if (player.emulated_time != 0 && now > player.emulated_time) {
		const float dt = std::clamp(static_cast<float>(now - player.emulated_time) * 0.000001f,
		                            0.001f, 0.1f);
		auto delta = QuatMultiply(QuatConjugate(player.input.Latest().motion.orientation), q);
		if (delta[3] < 0.0f) {
			for (auto& v: delta) {
				v = -v;
			}
		}
		motion.gyro = {2.0f * delta[0] / dt, 2.0f * delta[1] / dt, 2.0f * delta[2] / dt};
	}
	player.emulated_time = now;
	motion.orientation   = q;
	motion.accel         = QuatRotate(QuatConjugate(q), {0.0f, 1.0f, 0.0f});
	player.input.SetMotion(motion, now);
	// Angular velocity is only nonzero while the tilt changes.
	if (motion.gyro != Vec3 {}) {
		motion.gyro = {};
		player.input.SetMotion(motion, now);
	}
}

void GameController::ToggleGyroEmulation(int id) {
	Common::LockGuard lock(m_mutex);
	auto*             source_player = PlayerOfSource(id);
	if (source_player == nullptr) {
		return;
	}
	auto&     player    = *source_player;
	const int active_id = player.input.ActiveId();
	// Pads with a real gyroscope report it directly.
	if (active_id != HOST_INPUT_CONTROLLER_ID) {
		auto* pad = SDL_GetGamepadFromID(static_cast<SDL_JoystickID>(active_id));
		if (pad != nullptr && SDL_GamepadHasSensor(pad, SDL_SENSOR_GYRO)) {
			return;
		}
	}

	const auto now        = LibKernel::KernelGetProcessTime();
	player.gyro_emulation = !player.gyro_emulation;
	player.emulated_time  = 0;
	const auto& axes      = player.input.Latest().axes;
	if (player.gyro_emulation) {
		player.emulated_stick = {axes[static_cast<int>(Axis::RightX)],
		                         axes[static_cast<int>(Axis::RightY)]};
		for (const int source: player.input.SourceIds()) {
			player.input.SetRightStick(source, 128, 128, now);
		}
		UpdateEmulatedTilt(player);
	} else {
		player.input.SetMotion(PadMotion {}, now);
		player.input.SetRightStick(id, player.emulated_stick[0], player.emulated_stick[1], now);
	}
	LOGF("Emulated gyro tilt: %s\n", player.gyro_emulation ? "enabled" : "disabled");
}

void GameController::SetMotionSensorState(int handle, bool enable) {
	Common::LockGuard lock(m_mutex);
	auto*             player = GamepadPlayer(handle);
	if (player != nullptr && player->motion_enabled != enable) {
		player->motion_enabled     = enable;
		player->gyro_time          = 0;
		player->up_reference_valid = false;
		PadMotion motion;
		motion.orientation = player->input.Latest().motion.orientation;
		player->input.SetMotion(motion, LibKernel::KernelGetProcessTime());
	}
}

void GameController::ResetOrientation(int handle) {
	Common::LockGuard lock(m_mutex);
	auto*             player = GamepadPlayer(handle);
	if (player == nullptr) {
		return;
	}
	PadMotion motion           = player->input.Latest().motion;
	motion.orientation         = PadMotion {}.orientation;
	player->gyro_time          = 0;
	player->up_reference_valid = false;
	player->input.SetMotion(motion, LibKernel::KernelGetProcessTime());
}

void GameController::ResetInputState() {
	Common::LockGuard lock(m_mutex);
	for (auto& player: m_players) {
		player.input.Reset(LibKernel::KernelGetProcessTime());
		UpdateTriggerEffectState(player);
	}
}

void GameController::StopOutputs(int id) {
	(void)DualSenseHaptics::SetVibration(id, 0, 0, 0);
	auto* pad = SDL_GetGamepadFromID(static_cast<SDL_JoystickID>(id));
	if (pad == nullptr) {
		return;
	}
	if (SDL_GetGamepadType(pad) == SDL_GAMEPAD_TYPE_PS5) {
		DualSenseEffects effect {};
		effect.enable_bits      = 0x0c;
		effect.right_trigger[0] = 0x05;
		effect.left_trigger[0]  = 0x05;
		(void)SDL_SendGamepadEffect(pad, &effect, sizeof(effect));
	}
	(void)SDL_RumbleGamepad(pad, 0, 0, 0);
}

void GameController::ReleaseHostPads() {
	Common::LockGuard lock(m_mutex);
	DualSenseHaptics::Shutdown();

	std::vector<SDL_Gamepad*> pads;
	for (const auto id: m_gamepads) {
		if (auto* pad = SDL_GetGamepadFromID(static_cast<SDL_JoystickID>(id)); pad != nullptr) {
			StopOutputs(id);
			(void)SDL_SetGamepadLED(pad, 0, 0, 0);
			pads.push_back(pad);
		}
	}
	m_gamepads.clear();

	if (!pads.empty()) {
		SDL_Delay(RELEASE_FLUSH_MS);
		for (auto* pad: pads) {
			SDL_CloseGamepad(pad);
		}
	}

	for (auto& player: m_players) {
		const int previous_active_id = player.input.ActiveId();
		player.input.DisconnectAll();
		AfterInput(player, previous_active_id);
	}
}

void GameController::SetVibration(int handle, uint8_t large_motor, uint8_t small_motor) {
	Common::LockGuard lock(m_mutex);
	if (auto* player = GamepadPlayer(handle); player != nullptr) {
		player->vibration       = {large_motor, small_motor};
		player->vibration_until = SDL_GetTicks() + RUMBLE_DURATION_MS;
		ApplyVibration(*player);
	}
}

void GameController::ApplyVibration(Player& player) {
	const auto now = SDL_GetTicks();
	const auto duration =
	    player.vibration_until > now ? static_cast<uint32_t>(player.vibration_until - now) : 0;
	const auto scale       = duration != 0 ? GetSettingScale(Setting::VibrationIntensity) : 0.0f;
	const auto large_motor = Scale(player.vibration[0], scale);
	const auto small_motor = Scale(player.vibration[1], scale);
	const int  active_id   = player.input.ActiveId();
	// The DualSense haptics backend drives a single pad, so only player 1 uses it; other pads,
	// DualSense or not, use their rumble motors.
	if (player.index == 0 &&
	    DualSenseHaptics::SetVibration(active_id, large_motor, small_motor, duration)) {
		return;
	}

	if (active_id == HOST_INPUT_CONTROLLER_ID) {
		return;
	}

	auto* pad = SDL_GetGamepadFromID(static_cast<SDL_JoystickID>(active_id));
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
	return m_players[0].input.ActiveId();
}

int GameController::GetGamepadOfPlayer(int player) {
	if (player < 0 || player >= PlayerSeats::MAX_PLAYERS) {
		return PlayerSeats::NO_PAD;
	}
	Common::LockGuard lock(m_mutex);
	return m_seats.PadOf(player);
}

bool GameController::TakePlayerEvent(bool* login, int* user_id) {
	Common::LockGuard   lock(m_mutex);
	PlayerSeats::Event event;
	if (!m_seats.TakeEvent(&event)) {
		return false;
	}
	*login   = event.login;
	*user_id = m_seats.UserOf(event.player);
	return true;
}

void GameController::GetLoggedInUsers(int* user_ids) {
	Common::LockGuard lock(m_mutex);
	// The logged-in users first, in seat order, then -1: no gap where a player left. That the
	// console's list has no gaps is assumed, not known.
	int count = 0;
	for (int player = 0; player < PlayerSeats::MAX_PLAYERS; ++player) {
		if (m_seats.IsLoggedIn(player)) {
			user_ids[count++] = m_seats.UserOf(player);
		}
	}
	std::fill(user_ids + count, user_ids + PlayerSeats::MAX_PLAYERS, -1);
}

int GameController::GetPlayerOfUser(int user_id) {
	Common::LockGuard lock(m_mutex);
	return m_seats.PlayerOfUser(user_id);
}

void GameController::SetLightBar(int handle, uint8_t r, uint8_t g, uint8_t b) {
	Common::LockGuard lock(m_mutex);
	const auto*       player = GamepadPlayer(handle);
	if (player == nullptr) {
		return;
	}
	if (const auto& color = Config::GetControllerColor()) {
		r = (*color)[0];
		g = (*color)[1];
		b = (*color)[2];
	}
	if (auto* pad = SDL_GetGamepadFromID(static_cast<SDL_JoystickID>(player->input.ActiveId()));
	    pad != nullptr) {
		(void)SDL_SetGamepadLED(pad, r, g, b);
	}
}

bool GameController::SetTriggerEffect(int handle, const PadTriggerEffectParam& param) {
	Common::LockGuard lock(m_mutex);
	auto*             player = GamepadPlayer(handle);
	if (player == nullptr) {
		return (param.trigger_mask & ~0x03u) == 0;
	}
	if (!SendTriggerEffect(*player, param)) {
		return false;
	}
	for (int i = 0; i < 2; i++) {
		if ((param.trigger_mask & (1u << i)) != 0) {
			player->trigger_effect.trigger_mask |= static_cast<uint8_t>(1u << i);
			player->trigger_effect.command[i] = param.command[i];
		}
	}
	UpdateTriggerEffectState(*player);
	return true;
}

void GameController::GetTriggerEffectState(int handle, PadTriggerEffectStateInformation* info) {
	Common::LockGuard lock(m_mutex);
	const auto*       player = GamepadPlayer(handle);
	*info = player != nullptr ? player->trigger_state : PadTriggerEffectStateInformation {};
}

void GameController::UpdateTriggerEffectState(Player& player) {
	// Track every input event so a weapon remains fired even when the game polls after release
	// starts.
	const auto& axes = player.input.Latest().axes;
	for (int i = 0; i < 2; i++) {
		const int value                = axes[static_cast<int>(Axis::TriggerLeft) + i];
		player.trigger_state.state[i] = TriggerEffectState(player.trigger_effect.command[i], value,
		                                                   player.trigger_state.state[i]);
	}
}

bool GameController::SendTriggerEffect(Player& player, const PadTriggerEffectParam& param) {
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

	auto* pad = SDL_GetGamepadFromID(static_cast<SDL_JoystickID>(player.input.ActiveId()));
	if (pad != nullptr && SDL_GetGamepadType(pad) == SDL_GAMEPAD_TYPE_PS5) {
		(void)SDL_SendGamepadEffect(pad, &effect, sizeof(effect));
	}
	return true;
}

void GameController::GetConnectionInfo(int handle, bool* flag, int* count) {
	EXIT_IF(flag == nullptr);
	EXIT_IF(count == nullptr);

	Common::LockGuard lock(m_mutex);
	const auto&       input = InputOf(handle);

	*flag  = input.Connected();
	*count = input.ConnectedCount();
}

void GameController::ReadState(int handle, PadState* state, bool* flag, int* count) {
	EXIT_IF(flag == nullptr);
	EXIT_IF(count == nullptr);
	EXIT_IF(state == nullptr);

	Common::LockGuard lock(m_mutex);
	const auto&       input = InputOf(handle);

	*flag  = input.Connected();
	*count = input.ConnectedCount();
	*state = input.Latest();
}

int GameController::ReadStates(int handle, std::span<PadState> states, bool* flag, int* count) {
	EXIT_IF(flag == nullptr);
	EXIT_IF(count == nullptr);

	Common::LockGuard lock(m_mutex);
	auto&             input = InputOf(handle);

	*flag  = input.Connected();
	*count = input.ConnectedCount();

	return input.Connected() ? input.Read(states) : 0;
}

void Connect(int id) {
	g_controller->Connect(id);
}

void Disconnect(int id) {
	g_controller->Disconnect(id);
}

void SeatGamepad(int id) {
	g_controller->Seat(id);
}

void CycleKeyboardPlayer() {
	if (g_controller != nullptr) {
		g_controller->CycleKeyboardPlayer();
	}
}

void ToggleGyroEmulation(int id) {
	if (g_controller != nullptr) {
		g_controller->ToggleGyroEmulation(id);
	}
}

bool IsPlayerConnected(int player) {
	return g_controller != nullptr && g_controller->IsPlayerLoggedIn(player);
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

int GetGamepadOfPlayer(int player) {
	return g_controller != nullptr ? g_controller->GetGamepadOfPlayer(player)
	                               : PlayerSeats::NO_PAD;
}

bool TakePlayerEvent(bool* login, int* user_id) {
	return g_controller != nullptr && g_controller->TakePlayerEvent(login, user_id);
}

void GetLoggedInUsers(int* user_ids) {
	if (g_controller != nullptr) {
		g_controller->GetLoggedInUsers(user_ids);
		return;
	}
	user_ids[0] = Config::GetUserId();
	std::fill_n(user_ids + 1, PlayerSeats::MAX_PLAYERS - 1, -1);
}

int GetPlayerOfUser(int user_id) {
	if (g_controller != nullptr) {
		return g_controller->GetPlayerOfUser(user_id);
	}
	return user_id == Config::GetUserId() ? 0 : -1;
}

int GameController::InitPort() {
	Common::LockGuard lock(m_mutex);
	m_initialized = true;
	return OK;
}

int GameController::OpenPort(int user_id, int type, int index) {
	Common::LockGuard lock(m_mutex);
	if (!m_initialized) {
		return PAD_ERROR_NOT_INITIALIZED;
	}
	// A standard port and a special port for each logged-in user.
	const int player = m_seats.PlayerOfUser(user_id);
	if (player == PlayerSeats::NO_PLAYER ||
	    (type != PAD_TYPE_STANDARD && type != PAD_TYPE_SPECIAL) || index != 0) {
		return PAD_ERROR_INVALID_ARG;
	}
	const bool special = type == PAD_TYPE_SPECIAL;
	auto&      open    = m_players[player].PortOpen(special);
	if (open) {
		return PAD_ERROR_ALREADY_OPENED;
	}
	open = true;
	return HandleOf(player, special);
}

int GameController::GetPortHandle(int user_id, int type, int index) {
	Common::LockGuard lock(m_mutex);
	if (!m_initialized) {
		return PAD_ERROR_NOT_INITIALIZED;
	}
	const int  player  = m_seats.PlayerOfUser(user_id);
	const bool special = type == PAD_TYPE_SPECIAL;
	if (player == PlayerSeats::NO_PLAYER || (type != PAD_TYPE_STANDARD && !special) || index != 0) {
		return PAD_ERROR_NO_HANDLE;
	}
	return m_players[player].PortOpen(special) ? HandleOf(player, special) : PAD_ERROR_NO_HANDLE;
}

int GameController::CheckPortHandleLocked(int handle) const {
	if (!m_initialized) {
		return PAD_ERROR_NOT_INITIALIZED;
	}
	if (handle < 1 || handle > 2 * PlayerSeats::MAX_PLAYERS) {
		return PAD_ERROR_INVALID_HANDLE;
	}
	return PlayerOfHandle(handle).PortOpen(IsSpecial(handle)) ? OK : PAD_ERROR_INVALID_HANDLE;
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
	PlayerOfHandle(handle).PortOpen(IsSpecial(handle)) = false;
	return OK;
}

// The Sense controller ports of the virtual headset (padVr.cpp) read and drive player 1's
// standard port.
static int PortOf(int handle) {
	return PadVrHand(handle) >= 0 ? 1 : handle;
}

int PadCheckHandle(int handle) {
	if (g_controller == nullptr) {
		return PAD_ERROR_NOT_INITIALIZED;
	}
	if (PadVrHand(handle) >= 0) {
		return g_controller->Initialized() ? PadVrCheckHandle(handle) : PAD_ERROR_NOT_INITIALIZED;
	}
	return g_controller->CheckPortHandle(handle);
}

bool PadInitialized() {
	return g_controller != nullptr && g_controller->Initialized();
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

	if (const int hand = PadVrPortHand(type); hand >= 0) {
		return PadVrPort(user_id, hand, index, true);
	}
	return g_controller->OpenPort(user_id, type, index);
}

int KYTY_SYSV_ABI PadGetHandle(int user_id, int type, int index) {
	PRINT_NAME();

	LOGF("\t user_id = %d\n"
	     "\t type    = %d\n"
	     "\t index   = %d\n",
	     user_id, type, index);

	if (const int hand = PadVrPortHand(type); hand >= 0) {
		return PadVrPort(user_id, hand, index, false);
	}
	return g_controller->GetPortHandle(user_id, type, index);
}

int KYTY_SYSV_ABI PadClose(int handle) {
	PRINT_NAME();
	LOGF("\t handle = %d\n", handle);
	if (PadVrHand(handle) >= 0) {
		return PadVrClose(handle);
	}
	return g_controller->ClosePort(handle);
}

int KYTY_SYSV_ABI PadSetMotionSensorState(int handle, bool enable) {
	PRINT_NAME();

	if (const auto error = PadCheckHandle(handle); error != OK) {
		return error;
	}

	LOGF("\t enable = %s\n", (enable ? "true" : "false"));
	g_controller->SetMotionSensorState(PortOf(handle), enable);

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

	g_controller->ResetOrientation(PortOf(handle));
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
	g_controller->GetConnectionInfo(PortOf(handle), &connected, &connected_count);

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

	int      connected_count = 0;
	bool     connected       = false;
	PadState state;

	g_controller->ReadState(PortOf(handle), &state, &connected, &connected_count);

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

	int                      connected_count = 0;
	bool                     connected       = false;
	std::array<PadState, 64> states {};

	int ret_num = g_controller->ReadStates(PortOf(handle), std::span(states).first(static_cast<size_t>(num)),
	                                       &connected, &connected_count);

	if (!connected || ret_num == 0) {
		if (connected) {
			g_controller->ReadState(PortOf(handle), &states[0], &connected, &connected_count);
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

	g_controller->SetVibration(PortOf(handle), param->large_motor, param->small_motor);

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

	g_controller->SetLightBar(PortOf(handle), param->r, param->g, param->b);

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

	return g_controller->SetTriggerEffect(PortOf(handle), *param) ? OK : PAD_ERROR_INVALID_ARG;
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

	g_controller->GetTriggerEffectState(PortOf(handle), info);

	return OK;
}

} // namespace Libs::Controller
