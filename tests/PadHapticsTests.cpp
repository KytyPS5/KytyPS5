// Exercise the production module with real SDL streams and fake USB endpoints.
#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>

namespace {
void Check(bool condition, const char* text) {
	if (!condition) {
		std::fprintf(stderr, "PadHapticsTests: %s\n", text);
		std::abort();
	}
}
struct Device {
	SDL_AudioDeviceID id;
	const char*       name;
	int               channels;
};
struct Rumble {
	Uint16 large, small;
	Uint32 duration;
};
std::vector<Device>           devices;
std::vector<SDL_AudioStream*> streams;
Rumble                        rumble {};
Uint64                        now        = 1000;
int                           audio_refs = 0, opens = 0, rumble_calls = 0;
bool                          fail_open = false, fail_resume = false;
int                           actual_channels = 4;
SDL_AudioDeviceID             opened_device   = 0;
SDL_AudioSpec                 opened_spec {};
} // namespace

namespace Fake {
Uint64 GetTicks() {
	return now;
}
bool InitSubSystem(SDL_InitFlags flags) {
	const bool result = SDL_InitSubSystem(flags);
	if (result && (flags & SDL_INIT_AUDIO)) {
		audio_refs++;
	}
	return result;
}
void QuitSubSystem(SDL_InitFlags flags) {
	if (flags & SDL_INIT_AUDIO) {
		audio_refs--;
	}
	SDL_QuitSubSystem(flags);
}
SDL_AudioDeviceID* GetAudioPlaybackDevices(int* count) {
	*count    = static_cast<int>(devices.size());
	auto* ids = static_cast<SDL_AudioDeviceID*>(
	    SDL_malloc((devices.size() + 1) * sizeof(SDL_AudioDeviceID)));
	for (size_t i = 0; i < devices.size(); i++) {
		ids[i] = devices[i].id;
	}
	ids[devices.size()] = 0;
	return ids;
}
const char* GetAudioDeviceName(SDL_AudioDeviceID id) {
	for (const auto& device: devices) {
		if (device.id == id) {
			return device.name;
		}
	}
	return nullptr;
}
bool GetAudioDeviceFormat(SDL_AudioDeviceID id, SDL_AudioSpec* spec, int*) {
	if (id == 999) {
		*spec = {SDL_AUDIO_F32, actual_channels, 48000};
		return true;
	}
	for (const auto& device: devices) {
		if (device.id == id) {
			*spec = {SDL_AUDIO_F32, device.channels, 48000};
			return true;
		}
	}
	return false;
}
SDL_AudioStream* OpenAudioDeviceStream(SDL_AudioDeviceID id, const SDL_AudioSpec* spec,
                                       SDL_AudioStreamCallback callback, void* userdata) {
	opens++;
	opened_device = id;
	opened_spec   = *spec;
	if (fail_open) {
		return nullptr;
	}
	auto* stream = SDL_CreateAudioStream(spec, spec);
	Check(stream != nullptr && SDL_SetAudioStreamGetCallback(stream, callback, userdata),
	      "real SDL stream/callback creation failed");
	streams.push_back(stream);
	return stream;
}
bool ResumeAudioStreamDevice(SDL_AudioStream*) {
	return !fail_resume;
}
void DestroyAudioStream(SDL_AudioStream* stream) {
	if (stream == nullptr) {
		return;
	}
	const auto it = std::find(streams.begin(), streams.end(), stream);
	Check(it != streams.end(), "stream destroyed twice");
	streams.erase(it);
	SDL_DestroyAudioStream(stream);
}
SDL_AudioDeviceID GetAudioStreamDevice(SDL_AudioStream*) {
	return 999;
}
SDL_GamepadType GetGamepadTypeForID(SDL_JoystickID id) {
	return id == 1 ? SDL_GAMEPAD_TYPE_PS5 : SDL_GAMEPAD_TYPE_UNKNOWN;
}
SDL_Gamepad* GetGamepadFromID(SDL_JoystickID id) {
	return id == 1 ? reinterpret_cast<SDL_Gamepad*>(static_cast<uintptr_t>(id)) : nullptr;
}
bool RumbleGamepad(SDL_Gamepad*, Uint16 large, Uint16 small, Uint32 duration) {
	rumble = {large, small, duration};
	rumble_calls++;
	return true;
}
} // namespace Fake

#define SDL_GetTicks                Fake::GetTicks
#define SDL_InitSubSystem           Fake::InitSubSystem
#define SDL_QuitSubSystem           Fake::QuitSubSystem
#define SDL_GetAudioPlaybackDevices Fake::GetAudioPlaybackDevices
#define SDL_GetAudioDeviceName      Fake::GetAudioDeviceName
#define SDL_GetAudioDeviceFormat    Fake::GetAudioDeviceFormat
#define SDL_OpenAudioDeviceStream   Fake::OpenAudioDeviceStream
#define SDL_ResumeAudioStreamDevice Fake::ResumeAudioStreamDevice
#define SDL_DestroyAudioStream      Fake::DestroyAudioStream
#define SDL_GetAudioStreamDevice    Fake::GetAudioStreamDevice
#define SDL_GetGamepadTypeForID     Fake::GetGamepadTypeForID
#define SDL_GetGamepadFromID        Fake::GetGamepadFromID
#define SDL_RumbleGamepad           Fake::RumbleGamepad
#include "libs/dualSenseHaptics.cpp"
#undef SDL_GetTicks
#undef SDL_InitSubSystem
#undef SDL_QuitSubSystem
#undef SDL_GetAudioPlaybackDevices
#undef SDL_GetAudioDeviceName
#undef SDL_GetAudioDeviceFormat
#undef SDL_OpenAudioDeviceStream
#undef SDL_ResumeAudioStreamDevice
#undef SDL_DestroyAudioStream
#undef SDL_GetAudioStreamDevice
#undef SDL_GetGamepadTypeForID
#undef SDL_GetGamepadFromID
#undef SDL_RumbleGamepad

namespace {
namespace Haptics = Libs::Controller::DualSenseHaptics;
using Port        = std::unique_ptr<Haptics::Stream, decltype(&Haptics::Close)>;
constexpr std::array<int, 2>   unity {32768, 32768};
constexpr std::array<float, 4> pcm {0.5f, 0.5f, 0.5f, 0.5f};

struct Fixture {
	Fixture() {
		devices = {{10, "Speakers (DualSense Wireless Controller)", 4}};
		rumble  = {};
		now     = 1000;
		opens = rumble_calls = 0;
		fail_open = fail_resume = false;
		actual_channels         = 4;
	}
	~Fixture() {
		Haptics::Shutdown();
		Check(streams.empty() && audio_refs == 0, "stream or audio subsystem reference leaked");
	}
};

Port Open() {
	auto* stream = Haptics::Open(48000);
	Check(stream != nullptr, "port open failed");
	return Port(stream, Haptics::Close);
}
void Queue(const Port& port, const void* data, uint32_t frames = 2, uint32_t channels = 2,
           bool is_float = true, const int* volume = unity.data()) {
	Haptics::Queue(port.get(), 1, data, frames, channels, is_float, volume);
}
void Pull() {
	std::array<float, 64> buffer {};
	Check(!streams.empty() &&
	          SDL_GetAudioStreamData(streams.back(), buffer.data(), sizeof(buffer)) >= 0,
	      "audio device pull failed");
}
void ExpectPcm(std::array<float, 8> expected) {
	std::array<float, 8> actual {};
	Check(!streams.empty() && SDL_GetAudioStreamData(streams.back(), actual.data(),
	                                                 sizeof(actual)) == sizeof(actual),
	      "wrong output byte count");
	for (size_t i = 0; i < actual.size(); i++) {
		Check(std::abs(actual[i] - expected[i]) < 1e-6f, "haptics mapping or volume is wrong");
	}
}

void TestFormatsAndVolume() {
	Fixture                    f;
	auto                       port = Open();
	const std::array<int, 2>   volume {32768, 16384};
	const std::array<float, 4> stereo {0.5f, -0.25f, 1.0f, 0.0f};
	Queue(port, stereo.data(), 2, 2, true, volume.data());
	Check(opened_device == 10 && opened_spec.channels == 4 && opened_spec.format == SDL_AUDIO_F32,
	      "did not select quad DualSense PCM");
	ExpectPcm({0, 0, 0.5f, -0.125f, 0, 0, 1.0f, 0});
	const std::array<int16_t, 2> mono {16384, -32768};
	Queue(port, mono.data(), 2, 1, false, volume.data() + 1);
	ExpectPcm({0, 0, 0.25f, 0.25f, 0, 0, -0.5f, -0.5f});
	std::array<float, 24> multichannel {};
	multichannel.fill(0.9f);
	multichannel[0]  = 0.1f;
	multichannel[1]  = 0.2f;
	multichannel[12] = 0.3f;
	multichannel[13] = 0.4f;
	Queue(port, multichannel.data(), 2, 12);
	ExpectPcm({0, 0, 0.1f, 0.2f, 0, 0, 0.3f, 0.4f});
}

void TestDiscoveryAndHotplug() {
	Fixture f;
	devices   = {{10, "DualSense Wireless Controller", 2}, {20, "Other speakers", 4}};
	auto port = Open();
	Queue(port, pcm.data());
	Check(streams.empty() && opens == 0, "stereo controller or unrelated speakers selected");
	devices[0].channels = 4;
	now += 2001;
	Queue(port, pcm.data());
	Check(streams.size() == 1 && opened_device == 10, "late USB connection was not discovered");
	devices.clear();
	now += 2001;
	Queue(port, pcm.data());
	Check(streams.empty(), "disconnected endpoint retained its stream");
}

void TestFailuresAndBoundedQueue() {
	Fixture                f;
	auto                   port = Open();
	std::array<float, 128> block {};
	block.fill(0.5f);
	fail_open = true;
	Queue(port, block.data(), 64);
	Check(streams.empty() && audio_refs == 1, "failed open leaked a stream/reference");
	fail_open   = false;
	fail_resume = true;
	now += 2001;
	Queue(port, block.data(), 64);
	Check(streams.empty() && audio_refs == 1, "failed resume leaked a stream/reference");
	fail_resume     = false;
	actual_channels = 2;
	now += 2001;
	Queue(port, block.data(), 64);
	Check(streams.empty(), "post-open stereo downgrade accepted");
	actual_channels = 4;
	now += 2001;
	for (int i = 0; i < 70; i++) {
		Queue(port, block.data(), 64);
	}
	constexpr int block_bytes = 64 * 4 * sizeof(float);
	constexpr int max_queue   = 48000 * 4 * sizeof(float) * 80 / 1000;
	Check(streams.size() == 1 &&
	          SDL_GetAudioStreamQueued(streams.back()) <= max_queue + block_bytes,
	      "queue grows indefinitely when its clock stalls");
}

void TestRumbleLeaseAndDuration() {
	Fixture f;
	auto    port = Open();
	Check(Haptics::SetVibration(1, 100, 50), "set rumble failed");
	Check(rumble.large == 100 * 0x101 && rumble.duration == 65535, "ordinary rumble changed");
	Queue(port, pcm.data());
	Check(rumble.large == 0 && rumble.small == 0, "rumble was not stopped before haptics");
	now = 1100;
	const std::array<float, 4> silence {};
	Queue(port, silence.data());
	now = 1249;
	Pull();
	Check(rumble.large == 0, "rumble resumed before lease expiration");
	now = 1250;
	Pull();
	Check(rumble.large == 100 * 0x101 && rumble.small == 50 * 0x101 && rumble.duration == 65285,
	      "idle haptics did not restore remaining rumble duration");
	now = 66500;
	Queue(port, pcm.data());
	now = 66751;
	Pull();
	Check(rumble.large == 0 && rumble.duration == 0, "expired rumble restarted after haptics");
	Haptics::Shutdown();
	const int calls = rumble_calls;
	Pull();
	Check(rumble_calls == calls, "audio pull touched rumble after shutdown");
}

void TestCloseRestoresRumble() {
	Fixture f;
	auto    first = Open(), second = Open();
	Haptics::SetVibration(1, 100, 50);
	Queue(first, pcm.data());
	Queue(second, pcm.data());
	Check(rumble.large == 0, "haptics did not suppress rumble");
	first.reset();
	Check(rumble.large == 0, "closing one of two streams restored rumble early");
	second.reset();
	Check(rumble.large == 100 * 0x101 && rumble.small == 50 * 0x101,
	      "closing the final stream left rumble suppressed");
}
} // namespace

int main() {
	SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy");
	TestFormatsAndVolume();
	TestDiscoveryAndHotplug();
	TestFailuresAndBoundedQueue();
	TestRumbleLeaseAndDuration();
	TestCloseRestoresRumble();
	std::printf("PadHapticsTests: all cases passed\n");
}
