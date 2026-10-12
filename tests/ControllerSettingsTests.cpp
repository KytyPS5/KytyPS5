// Exercise the production controller with deterministic host output and time.
#include <SDL3/SDL.h>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {
void Check(bool condition, const char* text) {
	if (!condition) {
		std::fprintf(stderr, "ControllerSettingsTests: %s\n", text);
		std::abort();
	}
}

struct Rumble {
	Uint16 large, small;
	Uint32 duration;
};
Uint64                             now = 1000;
std::vector<Rumble>                rumble;
std::vector<Rumble>                haptics;
std::vector<std::array<Uint8, 32>> effects;
bool                               haptics_handles_rumble = false;
} // namespace

namespace Fake {
Uint64 GetTicks() {
	return now;
}
SDL_Gamepad* GetGamepadFromID(SDL_JoystickID id) {
	return id == 1 || id == 2 ? reinterpret_cast<SDL_Gamepad*>(static_cast<uintptr_t>(id))
	                          : nullptr;
}
SDL_GamepadType GetGamepadType(SDL_Gamepad*) {
	return SDL_GAMEPAD_TYPE_PS5;
}
bool RumbleGamepad(SDL_Gamepad*, Uint16 large, Uint16 small, Uint32 duration) {
	rumble.push_back({large, small, duration});
	return true;
}
bool SendGamepadEffect(SDL_Gamepad*, const void* data, int size) {
	Check(size == 32, "unexpected DualSense effect size");
	std::array<Uint8, 32> effect {};
	std::memcpy(effect.data(), data, effect.size());
	effects.push_back(effect);
	return true;
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
void CloseGamepad(SDL_Gamepad*) {}
void Delay(Uint32) {}
} // namespace Fake

#define SDL_GetTicks                Fake::GetTicks
#define SDL_GetGamepadFromID        Fake::GetGamepadFromID
#define SDL_GetGamepadType          Fake::GetGamepadType
#define SDL_RumbleGamepad           Fake::RumbleGamepad
#define SDL_SendGamepadEffect       Fake::SendGamepadEffect
#define SDL_SetGamepadLED           Fake::SetGamepadLED
#define SDL_GamepadHasSensor        Fake::GamepadHasSensor
#define SDL_SetGamepadSensorEnabled Fake::SetGamepadSensorEnabled
#define SDL_CloseGamepad            Fake::CloseGamepad
#define SDL_Delay                   Fake::Delay
#include "libs/controller.cpp"
#undef SDL_GetTicks
#undef SDL_GetGamepadFromID
#undef SDL_GetGamepadType
#undef SDL_RumbleGamepad
#undef SDL_SendGamepadEffect
#undef SDL_SetGamepadLED
#undef SDL_GamepadHasSensor
#undef SDL_SetGamepadSensorEnabled
#undef SDL_CloseGamepad
#undef SDL_Delay

namespace Libs::Controller::DualSenseHaptics {
bool SetVibration(int, uint8_t large_motor, uint8_t small_motor, uint32_t duration_ms) {
	haptics.push_back({large_motor, small_motor, duration_ms});
	return haptics_handles_rumble;
}
void Shutdown() {}
} // namespace Libs::Controller::DualSenseHaptics

namespace Libs::LibKernel {
uint64_t KYTY_SYSV_ABI KernelGetProcessTime() {
	return now * 1000;
}
} // namespace Libs::LibKernel

namespace Loader::Timer {
double GetTimeMs() {
	return static_cast<double>(now);
}
} // namespace Loader::Timer

namespace {
using namespace Libs::Controller;

struct Controller {
	Controller() {
		now                    = 1000;
		haptics_handles_rumble = false;
		Initialize();
		Check(PadInit() == 0 && PadOpen(Config::GetUserId(), 0, 0, nullptr) == 1,
		      "the standard controller port did not open");
		Connect(1);
		Check(GetSettingScale(Setting::SpeakerVolume) ==
		              Config::GetControllerSpeakerVolume() / 50.0f &&
		          GetSettingScale(Setting::VibrationIntensity) ==
		              Config::GetControllerVibrationIntensity() / 100.0f &&
		          GetSettingScale(Setting::TriggerEffectIntensity) == 1.0f,
		      "controller initialization retained old settings");
		rumble.clear();
		haptics.clear();
		effects.clear();
	}
	~Controller() { Shutdown(); }
};

DualSenseEffects LastEffect() {
	Check(!effects.empty(), "missing trigger output");
	DualSenseEffects effect {};
	std::memcpy(&effect, effects.back().data(), sizeof(effect));
	return effect;
}

int ZoneStrength(const uint8_t* effect, int zone) {
	const unsigned active = effect[1] | (effect[2] << 8u);
	const uint32_t packed = effect[3] | (effect[4] << 8u) | (effect[5] << 16u) |
	                        (static_cast<uint32_t>(effect[6]) << 24u);
	return (active & (1u << zone)) != 0 ? 1 + ((packed >> (3 * zone)) & 7u) : 0;
}

void SetRumble(uint8_t large, uint8_t small) {
	const PadVibrationParam param {large, small};
	Check(PadSetVibration(1, &param) == 0, "vibration request failed");
}

void TestPadPortOwnership() {
	Initialize();
	const int user = Config::GetUserId();
	constexpr auto not_initialized = static_cast<int32_t>(0x80920005u);
	constexpr auto already_opened = static_cast<int32_t>(0x80920004u);
	constexpr auto invalid_handle = static_cast<int32_t>(0x80920003u);
	constexpr auto no_handle = static_cast<int32_t>(0x80920008u);
	PadData data {};
	Check(PadOpen(user, 0, 0, nullptr) == not_initialized &&
	          PadGetHandle(user, 0, 0) == not_initialized &&
	          PadReadState(1, &data) == not_initialized,
	      "an uninitialized library exposed a controller port");
	Check(PadInit() == 0 && PadGetHandle(user, 0, 0) == no_handle,
	      "initializing the library opened a controller port");
	const int handle = PadOpen(user, 0, 0, nullptr);
	Check(handle == 1 && PadInit() == 0 && PadGetHandle(user, 0, 0) == handle,
	      "reinitializing the library lost the open port");
	for (int duplicate = 0; duplicate < 4; duplicate++) {
		const int rejected = PadOpen(user, 0, 0, nullptr);
		Check(rejected == already_opened,
		      "opening one pad five times exposed duplicate connected controllers");
		Check(PadClose(rejected) == invalid_handle && PadGetHandle(user, 0, 0) == handle,
		      "closing a rejected duplicate released the original owner");
	}
	Check(PadGetHandle(user + 1, 0, 0) == no_handle &&
	          PadGetHandle(user, 2, 0) == no_handle && PadGetHandle(user, 0, 1) == no_handle,
	      "a different port tuple aliased the open standard pad");
	SetButton(HOST_INPUT_CONTROLLER_ID, PAD_BUTTON_CROSS, true);
	Check(PadReadState(handle, &data) == 0 && data.connected &&
	          data.buttons == PAD_BUTTON_CROSS,
	      "keyboard Cross did not reach the owned port");
	SetButton(HOST_INPUT_CONTROLLER_ID, PAD_BUTTON_CROSS, false);
	Connect(1);
	SetButton(1, PAD_BUTTON_CROSS, true);
	Check(PadReadState(handle, &data) == 0 && data.buttons == PAD_BUTTON_CROSS,
	      "the physical controller did not share the owned port");
	Disconnect(1);
	Check(PadGetHandle(user, 0, 0) == handle && PadOpen(user, 0, 0, nullptr) == already_opened,
	      "host disconnection released guest port ownership");
	Check(PadClose(handle) == 0 && PadGetHandle(user, 0, 0) == no_handle &&
	          PadReadState(handle, &data) == invalid_handle &&
	          PadRead(handle, &data, 1) == invalid_handle && PadClose(handle) == invalid_handle,
	      "a closed port retained a usable handle");
	Check(PadOpen(user, 0, 0, nullptr) == handle && PadReadState(handle, &data) == 0,
	      "a closed port could not reopen");
	Shutdown();
}

void TestSettingCycles() {
	Controller controller;
	for (auto setting:
	     {Setting::SpeakerVolume, Setting::VibrationIntensity, Setting::TriggerEffectIntensity}) {
		Check(GetSettingScale(setting) == 1.0f, "initial setting is not strong");
		const std::vector<float> levels = setting == Setting::SpeakerVolume
		                                      ? std::vector<float> {0.0f, 0.02f, 0.12f, 0.42f, 1.0f}
		                                      : std::vector<float> {0.0f, 0.33f, 0.66f, 1.0f};
		for (int cycle = 0; cycle < 2; ++cycle) {
			for (float level: levels) {
				CycleSetting(setting);
				Check(GetSettingScale(setting) == level, "setting did not cycle or wrap");
			}
		}
	}
	CycleSetting(Setting::SpeakerVolume);
	CycleSetting(Setting::VibrationIntensity);
	CycleSetting(Setting::TriggerEffectIntensity);
}

void TestGlobalControllerLevels() {
	Config::ConfigOptions options;
	options.controller_speaker_volume      = 25;
	options.controller_vibration_intensity = 25;
	Config::Load(options);
	{
		Controller controller;
		Check(GetSettingScale(Setting::SpeakerVolume) == 0.5f &&
		          GetSettingScale(Setting::VibrationIntensity) == 0.25f &&
		          GetSettingScale(Setting::TriggerEffectIntensity) == 1.0f,
		      "global controller levels did not scale their outputs independently");
		SetRumble(200, 100);
		Check(rumble.back().large == 50 * 257 && rumble.back().small == 25 * 257,
		      "global vibration level did not scale rumble");
		CycleSetting(Setting::SpeakerVolume);
		CycleSetting(Setting::VibrationIntensity);
		Check(GetSettingScale(Setting::SpeakerVolume) == 0.0f &&
		          GetSettingScale(Setting::VibrationIntensity) == 0.0f,
		      "cycle hotkeys did not mute globally scaled outputs");
		CycleSetting(Setting::SpeakerVolume);
		CycleSetting(Setting::VibrationIntensity);
		Check(GetSettingScale(Setting::SpeakerVolume) == 0.01f &&
		          GetSettingScale(Setting::VibrationIntensity) == 0.0825f,
		      "cycle hotkeys did not multiply the global controller levels");
		Check(rumble.back().large == 17 * 257 && rumble.back().small == 8 * 257,
		      "cycling vibration did not apply the combined intensity to cached rumble");
	}
	options.controller_speaker_volume = 50;
	Config::Load(options);
	{
		Controller controller;
		Check(GetSettingScale(Setting::SpeakerVolume) == 1.0f,
		      "speaker midpoint did not preserve the previous default level");
	}
	options.controller_speaker_volume = 100;
	Config::Load(options);
	{
		Controller controller;
		Check(GetSettingScale(Setting::SpeakerVolume) == 2.0f,
		      "speaker maximum did not add 6 dB of gain");
		CycleSetting(Setting::SpeakerVolume);
		Check(GetSettingScale(Setting::SpeakerVolume) == 0.0f,
		      "speaker maximum prevented the hotkey from muting");
	}
	options.controller_speaker_volume      = 0;
	options.controller_vibration_intensity = 0;
	Config::Load(options);
	{
		Controller controller;
		Check(GetSettingScale(Setting::SpeakerVolume) == 0.0f &&
		          GetSettingScale(Setting::VibrationIntensity) == 0.0f,
		      "zero global controller levels did not mute outputs");
		SetRumble(200, 100);
		Check(rumble.back().large == 0 && rumble.back().small == 0,
		      "zero global vibration level did not mute motors");
	}
	Config::Load(Config::ConfigOptions {});
}

void TestVibrationLifetime() {
	Controller controller;
	SetRumble(255, 1);
	Check(rumble.size() == 1 && rumble.back().large == 65535 && rumble.back().small == 257 &&
	          rumble.back().duration == 65535,
	      "full vibration changed");
	now += 100;
	CycleSetting(Setting::VibrationIntensity);
	Check(rumble.back().large == 0 && rumble.back().small == 0, "muting did not stop vibration");
	now += 100;
	CycleSetting(Setting::VibrationIntensity);
	Check(rumble.back().large == 84 * 257 && rumble.back().small == 257 &&
	          rumble.back().duration == 65335,
	      "changing intensity lost a small motor or extended the vibration deadline");
	Check(effects.empty(), "vibration intensity resent triggers");
	now              = 1000 + 65535;
	const auto calls = rumble.size();
	CycleSetting(Setting::VibrationIntensity);
	CycleSetting(Setting::VibrationIntensity);
	Check(rumble.size() == calls || (rumble.back().large == 0 && rumble.back().small == 0),
	      "expired vibration was resurrected");
	SetRumble(0, 0);
	const auto stopped = rumble.size();
	CycleSetting(Setting::VibrationIntensity);
	CycleSetting(Setting::VibrationIntensity);
	Check(rumble.size() == stopped || (rumble.back().large == 0 && rumble.back().small == 0),
	      "explicitly stopped vibration was resurrected");
}

void TestMaskedTriggersAndValidation() {
	Controller            controller;
	PadTriggerEffectParam param {};
	param.trigger_mask       = 3;
	param.command[0].mode    = 1;
	param.command[0].data[0] = 2;
	param.command[0].data[1] = 8;
	param.command[1].mode    = 2;
	param.command[1].data[0] = 2;
	param.command[1].data[1] = 7;
	param.command[1].data[2] = 6;
	Check(PadSetTriggerEffect(1, &param) == 0, "initial trigger request failed");
	CycleSetting(Setting::TriggerEffectIntensity);
	Check(LastEffect().left_trigger[0] == 5 && LastEffect().right_trigger[0] == 5,
	      "muting did not disable both triggers");
	for (uint32_t mode = 1; mode <= 7; ++mode) {
		auto invalid            = param;
		invalid.command[1]      = {};
		invalid.command[1].mode = mode;
		std::memset(invalid.command[1].data, 255, sizeof(invalid.command[1].data));
		const auto calls = effects.size();
		Check(PadSetTriggerEffect(1, &invalid) == PAD_ERROR_INVALID_ARG && effects.size() == calls,
		      "muting bypassed trigger validation or sent a partial invalid request");
	}
	param.trigger_mask = 4;
	Check(PadSetTriggerEffect(1, &param) == PAD_ERROR_INVALID_ARG, "invalid trigger mask accepted");
	param.trigger_mask       = 1;
	param.command[0].data[1] = 6;
	Check(PadSetTriggerEffect(1, &param) == 0 && LastEffect().enable_bits == 8,
	      "left-only update touched the right trigger");
	CycleSetting(Setting::TriggerEffectIntensity);
	auto effect = LastEffect();
	Check(effect.enable_bits == 12 && effect.left_trigger[0] == 0x21 &&
	          ZoneStrength(effect.left_trigger, 1) == 0 &&
	          ZoneStrength(effect.left_trigger, 2) == 2 && effect.right_trigger[0] == 0x25 &&
	          effect.right_trigger[1] == 0x84 && effect.right_trigger[3] == 1,
	      "masked updates lost a cached trigger or failed to scale its strength");
	CycleSetting(Setting::TriggerEffectIntensity);
	CycleSetting(Setting::TriggerEffectIntensity);
	effect = LastEffect();
	Check(ZoneStrength(effect.left_trigger, 2) == 6 && effect.right_trigger[3] == 5,
	      "restoring strong intensity did not recover original trigger strengths");
	Check(rumble.empty() && haptics.empty(), "trigger intensity resent vibration");
}

void TestIndependentOutputsAndPadSwitch() {
	Controller controller;
	haptics_handles_rumble = true;
	SetRumble(200, 100);
	now += 100;
	CycleSetting(Setting::VibrationIntensity);
	CycleSetting(Setting::VibrationIntensity);
	Check(rumble.empty() && haptics.back().large == 66 && haptics.back().small == 33 &&
	          haptics.back().duration == 65435,
	      "haptics rumble did not receive scaled motors and remaining duration");
	PadTriggerEffectParam param {};
	param.trigger_mask       = 1;
	param.command[0].mode    = 1;
	param.command[0].data[1] = 8;
	Check(PadSetTriggerEffect(1, &param) == 0, "trigger request failed");
	const auto vibration_calls = haptics.size();
	const auto trigger_calls   = effects.size();
	CycleSetting(Setting::SpeakerVolume);
	Check(haptics.size() == vibration_calls && effects.size() == trigger_calls,
	      "speaker volume resent controller effects");
	Connect(2);
	Disconnect(1);
	Check(GetActiveControllerId() == 2, "active controller did not change");
	haptics.clear();
	effects.clear();
	CycleSetting(Setting::VibrationIntensity);
	CycleSetting(Setting::TriggerEffectIntensity);
	CycleSetting(Setting::TriggerEffectIntensity);
	Check(effects.empty(), "previous controller's trigger effects leaked to new pad");
	for (const auto& request: haptics) {
		Check(request.large == 0 && request.small == 0,
		      "previous controller's vibration leaked to new pad");
	}
	SetRumble(200, 100);
	Check(PadSetTriggerEffect(1, &param) == 0, "replacement pad trigger request failed");
	EmergencyShutdown();
	Check(GetActiveControllerId() == -1, "released controller remained active");
	haptics.clear();
	effects.clear();
	CycleSetting(Setting::VibrationIntensity);
	CycleSetting(Setting::TriggerEffectIntensity);
	Check(effects.empty(), "released controller received a trigger request");
	for (const auto& request: haptics) {
		Check(request.large == 0 && request.small == 0,
		      "released controller retained a cached vibration");
	}
}

void TestTriggerEffectState() {
	Controller                       controller;
	PadTriggerEffectStateInformation info {{-1, -1}};
	const auto check_state = [&](int32_t left, int32_t right, const char* message) {
		Check(PadGetTriggerEffectState(1, &info) == OK && info.state[0] == left &&
		          info.state[1] == right,
		      message);
	};
	const auto axis = [&](int left, int right) {
		SetAxis(1, Axis::TriggerLeft, left);
		SetAxis(1, Axis::TriggerRight, right);
	};
	PadTriggerEffectParam param {};
	const auto            set_effect = [&] {
		Check(PadSetTriggerEffect(1, &param) == OK, "trigger request failed");
	};
	Check(PadGetTriggerEffectState(2, &info) == PAD_ERROR_INVALID_HANDLE && info.state[0] == -1 &&
	          info.state[1] == -1,
	      "invalid trigger handle changed the output");
	Check(PadGetTriggerEffectState(2, nullptr) == PAD_ERROR_INVALID_HANDLE,
	      "trigger state checked the output before the handle");
	Check(PadGetTriggerEffectState(1, nullptr) == PAD_ERROR_INVALID_ARG,
	      "trigger state accepted a null output");
	check_state(0, 0, "unset trigger effects did not report off");

	param.trigger_mask       = 3;
	param.command[0].mode    = 1;
	param.command[0].data[0] = 7;
	param.command[0].data[1] = 8;
	param.command[1].mode    = 2;
	param.command[1].data[0] = 2;
	param.command[1].data[1] = 7;
	param.command[1].data[2] = 6;
	set_effect();
	axis(191, 63);
	check_state(1, 3, "triggers activated before the documented start positions");
	axis(192, 64);
	check_state(2, 4, "triggers did not activate at the documented start positions");
	axis(255, 223);
	check_state(2, 4, "weapon fired before its end position");
	SetAxis(1, Axis::TriggerRight, 224);
	SetAxis(1, Axis::TriggerRight, 100);
	check_state(2, 5, "unpolled weapon firing was lost on partial release");
	check_state(2, 5, "reading trigger state changed the weapon latch");
	set_effect();
	check_state(2, 5, "repeating a weapon request rearmed a held trigger");
	CycleSetting(Setting::TriggerEffectIntensity);
	check_state(2, 5, "muting trigger effects changed their reported states");

	// A rejected two-trigger request must leave both cached commands and states intact.
	auto invalid               = param;
	invalid.command[0].mode    = 0;
	invalid.command[1].data[1] = 2;
	Check(PadSetTriggerEffect(1, &invalid) == PAD_ERROR_INVALID_ARG,
	      "invalid weapon end position was accepted");
	SetAxis(1, Axis::TriggerLeft, 255);
	check_state(2, 5, "an invalid request changed cached trigger commands or states");
	SetAxis(1, Axis::TriggerRight, 63);
	SetAxis(1, Axis::TriggerRight, 100);
	check_state(2, 4, "unpolled release did not rearm the weapon");
	param.trigger_mask       = 2;
	param.command[1].data[1] = 8;
	set_effect();
	axis(255, 253);
	check_state(2, 4, "weapon end position 8 fired before output 254");
	SetAxis(1, Axis::TriggerRight, 254);
	check_state(2, 5, "weapon end position 8 did not fire at output 254");

	// Masked updates preserve the other trigger, including its fired state.
	param.trigger_mask       = 1;
	param.command[0].data[0] = 9;
	set_effect();
	SetAxis(1, Axis::TriggerLeft, 254);
	check_state(1, 5, "feedback position 9 activated before full travel");
	SetAxis(HOST_INPUT_CONTROLLER_ID, Axis::TriggerLeft, 255);
	check_state(2, 5, "keyboard trigger travel did not activate feedback");
	// The key is released: the keyboard holds its trigger apart from the gamepad's.
	SetAxis(HOST_INPUT_CONTROLLER_ID, Axis::TriggerLeft, 0);
	param.command[0].data[0] = 0;
	set_effect();
	SetAxis(1, Axis::TriggerLeft, 0);
	check_state(2, 5, "feedback at position 0 was suppressed by a dead zone");
	param.command[0].data[1] = 0;
	set_effect();
	check_state(1, 5, "zero-strength feedback reported active");

	param.command[0]         = {};
	param.command[0].mode    = 4;
	param.command[0].data[0] = 4;
	param.command[0].data[2] = 4;
	param.command[0].data[8] = 4;
	set_effect();
	const int feedback_cases[][2] = {{0, 2},  {1, 1},   {31, 1},  {32, 2},  {63, 2},
	                                 {64, 1}, {223, 1}, {224, 2}, {254, 2}, {255, 1}};
	for (const auto& test: feedback_cases) {
		SetAxis(1, Axis::TriggerLeft, test[0]);
		check_state(test[1], 5, "multi-position feedback used the wrong position");
	}

	param.command[0]         = {};
	param.command[0].mode    = 5;
	param.command[0].data[0] = 3;
	param.command[0].data[1] = 7;
	param.command[0].data[2] = 2;
	param.command[0].data[3] = 6;
	set_effect();
	const int slope_cases[][2] = {{63, 1}, {64, 2}, {191, 2}, {192, 1}, {255, 1}};
	for (const auto& test: slope_cases) {
		SetAxis(1, Axis::TriggerLeft, test[0]);
		check_state(test[1], 5, "slope feedback did not respect its start and end");
	}
	param.command[0].data[0] = 0;
	set_effect();
	SetAxis(1, Axis::TriggerLeft, 0);
	check_state(2, 5, "slope feedback at position 0 was suppressed");

	param.command[0]         = {};
	param.command[0].mode    = 3;
	param.command[0].data[0] = 6;
	param.command[0].data[1] = 4;
	param.command[0].data[2] = 20;
	set_effect();
	SetAxis(1, Axis::TriggerLeft, 159);
	check_state(6, 5, "vibration activated before position 6");
	SetAxis(1, Axis::TriggerLeft, 160);
	check_state(7, 5, "vibration did not activate at position 6");
	param.command[0].data[0] = 0;
	set_effect();
	SetAxis(1, Axis::TriggerLeft, 0);
	check_state(7, 5, "vibration at position 0 was suppressed");
	param.command[0].data[2] = 0;
	set_effect();
	check_state(6, 5, "zero-frequency vibration reported active");

	param.command[0]          = {};
	param.command[0].mode     = 6;
	param.command[0].data[0]  = 20;
	param.command[0].data[10] = 4;
	set_effect();
	SetAxis(1, Axis::TriggerLeft, 254);
	check_state(6, 5, "multi-position vibration activated before position 9");
	SetAxis(1, Axis::TriggerLeft, 255);
	check_state(7, 5, "multi-position vibration did not activate at position 9");
	param.command[0].data[0] = 0;
	set_effect();
	check_state(6, 5, "zero-frequency multi-position vibration reported active");

	ResetInputState();
	check_state(6, 3, "input reset retained a fired weapon");
	param.command[0].mode = 0;
	set_effect();
	check_state(0, 3, "off mode retained an effect state");
	Connect(2);
	Disconnect(1);
	check_state(0, 0, "switching controllers retained effect states");
}

} // namespace

int main() {
	Config::Initialize();
	TestPadPortOwnership();
	TestSettingCycles();
	TestGlobalControllerLevels();
	// Each following test initializes another controller and requires strong defaults.
	TestVibrationLifetime();
	TestMaskedTriggersAndValidation();
	TestIndependentOutputsAndPadSwitch();
	TestTriggerEffectState();
	Config::Shutdown();
	return 0;
}
