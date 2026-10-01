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
std::vector<Device>                 devices;
std::vector<SDL_AudioStream*>       streams;
Rumble                              rumble {};
Uint64                              now        = 1000;
int                                 audio_refs = 0, opens = 0, rumble_calls = 0;
bool                                fail_open = false, fail_resume = false;
int                                 actual_channels = 4;
SDL_AudioDeviceID                   opened_device   = 0;
SDL_AudioSpec                       opened_spec {};
std::array<Rumble, 4>               pad_rumble {};
std::array<std::vector<uint8_t>, 4> effects;
std::vector<SDL_JoystickID>         connected_pads {1};
std::array<Rumble, 4>               cached_rumble {};
bool                                fail_effect = false, wireless = false;
SDL_AudioStream*                   default_stream = nullptr;
bool                               fail_default_resume = false;
int                                active_controller = 1;
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
	if (id == SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK) {
		default_stream = stream;
	}
	return stream;
}
bool ResumeAudioStreamDevice(SDL_AudioStream* stream) {
	return !fail_resume && !(stream == default_stream && fail_default_resume);
}
void DestroyAudioStream(SDL_AudioStream* stream) {
	if (stream == nullptr) {
		return;
	}
	const auto it = std::find(streams.begin(), streams.end(), stream);
	Check(it != streams.end(), "stream destroyed twice");
	streams.erase(it);
	if (stream == default_stream) {
		default_stream = nullptr;
	}
	SDL_DestroyAudioStream(stream);
}
SDL_AudioDeviceID GetAudioStreamDevice(SDL_AudioStream*) {
	return 999;
}
// Pads 1 and 3 are DualSenses.
SDL_GamepadType GetGamepadTypeForID(SDL_JoystickID id) {
	return id == 1 || id == 3 ? SDL_GAMEPAD_TYPE_PS5 : SDL_GAMEPAD_TYPE_UNKNOWN;
}
SDL_Gamepad* GetGamepadFromID(SDL_JoystickID id) {
	return id == 1 || id == 3 ? reinterpret_cast<SDL_Gamepad*>(static_cast<uintptr_t>(id))
	                          : nullptr;
}
SDL_JoystickID* GetGamepads(int* count) {
	*count    = static_cast<int>(connected_pads.size());
	auto* ids = static_cast<SDL_JoystickID*>(
	    SDL_malloc((connected_pads.size() + 1) * sizeof(SDL_JoystickID)));
	std::copy(connected_pads.begin(), connected_pads.end(), ids);
	ids[connected_pads.size()] = 0;
	return ids;
}
SDL_JoystickConnectionState GetGamepadConnectionState(SDL_Gamepad*) {
	return wireless ? SDL_JOYSTICK_CONNECTION_WIRELESS : SDL_JOYSTICK_CONNECTION_WIRED;
}
bool RumbleGamepad(SDL_Gamepad* pad, Uint16 large, Uint16 small, Uint32 duration) {
	const auto id = reinterpret_cast<uintptr_t>(pad);
	// SDL only sends a new report when the strengths change.
	if (large != cached_rumble[id].large || small != cached_rumble[id].small) {
		pad_rumble[id] = {large, small, duration};
	}
	cached_rumble[id] = {large, small, duration};
	rumble            = pad_rumble[id];
	rumble.duration   = duration;
	rumble_calls++;
	return true;
}
bool SendGamepadEffect(SDL_Gamepad* pad, const void* data, int size) {
	if (fail_effect) {
		return false;
	}
	effects[reinterpret_cast<uintptr_t>(pad)].assign(static_cast<const uint8_t*>(data),
	                                                 static_cast<const uint8_t*>(data) + size);
	// Raw audio-routing reports clear the emulated-rumble mode on the controller,
	// without updating SDL's cached motor strengths.
	if ((static_cast<const uint8_t*>(data)[0] & 3) == 0) {
		pad_rumble[reinterpret_cast<uintptr_t>(pad)] = {};
		rumble                                       = {};
	}
	return true;
}
} // namespace Fake

#define SDL_GetTicks                  Fake::GetTicks
#define SDL_InitSubSystem             Fake::InitSubSystem
#define SDL_QuitSubSystem             Fake::QuitSubSystem
#define SDL_GetAudioPlaybackDevices   Fake::GetAudioPlaybackDevices
#define SDL_GetAudioDeviceName        Fake::GetAudioDeviceName
#define SDL_GetAudioDeviceFormat      Fake::GetAudioDeviceFormat
#define SDL_OpenAudioDeviceStream     Fake::OpenAudioDeviceStream
#define SDL_ResumeAudioStreamDevice   Fake::ResumeAudioStreamDevice
#define SDL_DestroyAudioStream        Fake::DestroyAudioStream
#define SDL_GetAudioStreamDevice      Fake::GetAudioStreamDevice
#define SDL_GetGamepadTypeForID       Fake::GetGamepadTypeForID
#define SDL_GetGamepadFromID          Fake::GetGamepadFromID
#define SDL_RumbleGamepad             Fake::RumbleGamepad
#define SDL_SendGamepadEffect         Fake::SendGamepadEffect
#define SDL_GetGamepads               Fake::GetGamepads
#define SDL_GetGamepadConnectionState Fake::GetGamepadConnectionState
#include "libs/dualSenseHaptics.cpp"
#include "libs/audio.cpp"
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
#undef SDL_SendGamepadEffect
#undef SDL_GetGamepads
#undef SDL_GetGamepadConnectionState

namespace Libs::Controller {
int GetActiveControllerId() {
	return active_controller;
}
} // namespace Libs::Controller

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
namespace Haptics = Libs::Controller::DualSenseHaptics;
using Port        = std::unique_ptr<Haptics::Stream, decltype(&Haptics::Close)>;
constexpr std::array<int, 2>   unity {32768, 32768};
constexpr std::array<float, 4> pcm {0.5f, 0.5f, 0.5f, 0.5f};

struct Fixture {
	Fixture() {
		devices        = {{10, "Speakers (DualSense Wireless Controller)", 4}};
		rumble         = {};
		pad_rumble     = {};
		cached_rumble  = {};
		effects        = {};
		connected_pads = {1};
		fail_effect = wireless = false;
		default_stream = nullptr;
		fail_default_resume = false;
		active_controller = 1;
		now                    = 1000;
		opens = rumble_calls = 0;
		fail_open = fail_resume = false;
		actual_channels         = 4;
	}
	~Fixture() {
		Haptics::Shutdown();
		Check(streams.empty() && audio_refs == 0, "stream or audio subsystem reference leaked");
	}
};

Port Open(bool speaker = false) {
	auto* stream = Haptics::Open(48000, speaker);
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

void TestSpeaker() {
	Fixture f;
	devices      = {{10, "DualSense Wireless Controller", 2}};
	auto speaker = Open(true);
	Check(Haptics::Queue(speaker.get(), 1, pcm.data(), 2, 2, true, unity.data()) == 0 &&
	          streams.empty(),
	      "speaker played without a quad DualSense device");
	devices = {{10, "Speakers (DualSense Wireless Controller)", 4}};
	now += 2001;
	Check(Haptics::SetVibration(1, 100, 50), "set rumble failed");
	const std::array<float, 4> stereo {0.5f, -0.25f, 1.0f, 0.0f};
	Queue(speaker, stereo.data());
	ExpectPcm({0.5f, -0.25f, 0, 0, 1.0f, 0, 0, 0});
	Check(rumble.large == 100 * 0x101, "speaker audio stopped the rumble");
	const auto routed = effects[1];
	Check(routed.size() == 38 && routed[0] == 0xa0 && routed[1] == 0x80 && routed[5] == 0x64 &&
	          routed[7] == 0x30 && routed[37] == 0x02,
	      "speaker not routed like the Linux driver");
	Check(Haptics::Queue(speaker.get(), 1, stereo.data(), 2, 2, true, unity.data()) ==
	          2 * 1000000 / 48000,
	      "queued speaker playback time is wrong");
	auto other     = Open(true);
	connected_pads = {3};
	Haptics::Queue(other.get(), 3, pcm.data(), 2, 2, true, unity.data());
	Check(effects[1][0] == 0x80 && effects[1][1] == 0 && effects[1][7] == 0 && effects[3] == routed,
	      "switching pads left the old speaker routed");
	speaker.reset();
	Check(effects[3] == routed, "closing one speaker port unrouted the other");
	other.reset();
	Check(effects[3][0] == 0x80 && effects[3][7] == 0,
	      "closing the last speaker port left it routed");
	auto last = Open(true);
	Haptics::Queue(last.get(), 3, pcm.data(), 2, 2, true, unity.data());
	Check(effects[3] == routed, "reopened speaker port was not routed");
	Haptics::Shutdown();
	Check(effects[3][0] == 0x80 && effects[3][7] == 0, "shutdown left the speaker routed");
}

void TestSpeakerUnplug() {
	Fixture f;
	auto    speaker = Open(true);
	Check(Haptics::Queue(speaker.get(), 1, pcm.data(), 2, 2, true, unity.data()) != 0,
	      "speaker did not play on the DualSense");
	const auto routed = effects[1];
	// Unplugged: another controller becomes active, and the port must fall back
	// to the main output.
	Check(Haptics::Queue(speaker.get(), 2, pcm.data(), 2, 2, true, unity.data()) == 0 &&
	          streams.empty(),
	      "unplugged pad kept the speaker port");
	Check(effects[1][0] == 0x80 && effects[1][7] == 0, "unplugging left the speaker routed");
	// Plugged back in: the port returns to the controller at once.
	effects = {};
	Check(Haptics::Queue(speaker.get(), 1, pcm.data(), 2, 2, true, unity.data()) != 0 &&
	          streams.size() == 1 && effects[1] == routed,
	      "replugged pad did not take the speaker back");
	// The audio endpoint vanishes while a DualSense stays active.
	devices.clear();
	now += 2001;
	Check(Haptics::Queue(speaker.get(), 1, pcm.data(), 2, 2, true, unity.data()) == 0 &&
	          streams.empty(),
	      "lost audio endpoint kept the speaker port");
	Check(effects[1][0] == 0x80 && effects[1][7] == 0,
	      "lost audio endpoint left the speaker routed");
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

void TestSwitchStopsOldRumble() {
	Fixture f;
	auto    port = Open();
	Haptics::SetVibration(1, 100, 50);
	Queue(port, pcm.data());
	Check(pad_rumble[1].large == 0, "haptics did not suppress rumble");
	Haptics::SetVibration(3, 10, 10);
	Check(pad_rumble[1].large == 0 && pad_rumble[1].small == 0,
	      "switching controllers restarted the old pad's rumble");
	Check(pad_rumble[3].large == 10 * 0x101, "new pad did not rumble");
}

void TestCloseRestoresRumble() {
	Fixture f;
	auto    speaker = Open(true);
	Queue(speaker, pcm.data());
	auto first = Open(), second = Open();
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

void TestSpeakerSwitchAndFailures() {
	Fixture f;
	auto    speaker = Open(true);
	auto    queue   = [&](int controller) {
		return Haptics::Queue(speaker.get(), controller, pcm.data(), 2, 2, true, unity.data());
	};
	fail_effect = true;
	Check(queue(1) == 0 && streams.empty(), "failed speaker routing suppressed fallback");
	fail_effect = false;
	now += 2001;
	Check(queue(1) != 0, "speaker did not recover after routing failed");
	connected_pads = {3};
	devices        = {{20, "Speakers (DualSense Wireless Controller)", 4}};
	Check(queue(3) != 0 && opened_device == 20 && effects[1][7] == 0 && effects[3][7] == 0x30,
	      "persistent speaker port did not follow the new controller immediately");
	// An idle stream on the old pad must not keep the new pad routed after its
	// last port closes.
	connected_pads = {1};
	auto other     = Open(true);
	Check(Haptics::Queue(other.get(), 1, pcm.data(), 2, 2, true, unity.data()) != 0,
	      "second speaker port failed");
	other.reset();
	Check(effects[1][7] == 0, "old controller stream kept the new speaker routed");
}

void TestSpeakerRequiresUnambiguousUsbDevice() {
	Fixture f;
	auto    speaker = Open(true);
	auto    queue   = [&] {
		return Haptics::Queue(speaker.get(), 1, pcm.data(), 2, 2, true, unity.data());
	};
	wireless = true;
	Check(queue() == 0 && streams.empty(), "Bluetooth controller stole another pad's USB audio");
	wireless       = false;
	connected_pads = {1, 3};
	Check(queue() == 0 && streams.empty(), "multiple DualSenses chose an arbitrary controller");
	connected_pads = {1};
	devices.push_back({20, "DualSense Wireless Controller", 4});
	Check(queue() == 0 && streams.empty(), "multiple audio endpoints chose an arbitrary device");
	devices.pop_back();
	now += 2001;
	Check(queue() != 0, "unambiguous USB device was not recovered");
	wireless = true;
	Check(queue() == 0 && streams.empty() && effects[1][7] == 0,
	      "USB to Bluetooth change retained the USB speaker");
}

void TestAudioSpeakerFallback() {
	Fixture f;
	Libs::Audio::Audio audio;
	active_controller = 2; // A different controller is active while a USB DualSense is attached.
	const auto port = audio.AudioOutOpen(4, 2, 48000, Libs::Audio::Audio::Format::FloatStereo);
	Check(port.IsValid() && default_stream != nullptr, "pad speaker fallback did not open");
	Libs::Audio::Audio::OutputParam output {port, pcm.data()};
	Check(audio.AudioOutOutputs(&output, 1, false) == 2, "pad speaker output count is wrong");
	Check(streams.size() == 1 && opens == 1 && effects[1].empty(),
	      "another controller's pad speaker opened or routed the DualSense");
	std::array<float, 4> actual {};
	Check(SDL_GetAudioStreamData(default_stream, actual.data(), sizeof(actual)) == sizeof(actual) &&
	          actual == pcm,
	      "pad speaker did not fall back to the main output");
}

void TestAudioSpeakerRouting() {
	Fixture f;
	Libs::Audio::Audio audio;
	const auto port = audio.AudioOutOpen(4, 2, 48000, Libs::Audio::Audio::Format::FloatStereo);
	Check(port.IsValid() && default_stream != nullptr, "pad speaker port did not open");
	Libs::Audio::Audio::OutputParam output {port, pcm.data()};
	audio.AudioOutOutputs(&output, 1, false);
	Check(streams.size() == 2 && opened_device == 10 &&
	          SDL_GetAudioStreamQueued(default_stream) == 0,
	      "successful DualSense playback also queued audio on the main output");
	ExpectPcm({0.5f, 0.5f, 0, 0, 0.5f, 0.5f, 0, 0});
}

void TestAudioDefaultResumeFailure() {
	Fixture f;
	Libs::Audio::Audio audio;
	fail_default_resume = true;
	const auto port = audio.AudioOutOpen(4, 2, 48000, Libs::Audio::Audio::Format::FloatStereo);
	Check(port.IsValid() && opens == 1 && default_stream == nullptr,
	      "default-output resume failure was not exercised");
	Libs::Audio::Audio::OutputParam output {port, pcm.data()};
	audio.AudioOutOutputs(&output, 1, false);
	Check(streams.size() == 1 && opened_device == 10,
	      "default-output resume failure disabled the DualSense speaker");
	ExpectPcm({0.5f, 0.5f, 0, 0, 0.5f, 0.5f, 0, 0});
}
} // namespace

int main() {
	SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy");
	TestAudioSpeakerFallback();
	TestAudioSpeakerRouting();
	TestAudioDefaultResumeFailure();
	TestFormatsAndVolume();
	TestDiscoveryAndHotplug();
	TestSpeaker();
	TestSpeakerUnplug();
	TestSpeakerSwitchAndFailures();
	TestSpeakerRequiresUnambiguousUsbDevice();
	TestFailuresAndBoundedQueue();
	TestRumbleLeaseAndDuration();
	TestSwitchStopsOldRumble();
	TestCloseRestoresRumble();
	std::printf("PadHapticsTests: all cases passed\n");
}
