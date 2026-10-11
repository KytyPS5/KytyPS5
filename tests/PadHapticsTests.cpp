// Exercise the production module with real SDL streams and fake USB endpoints.
#include <SDL3/SDL.h>
#include <opus.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <numeric>
#include <semaphore>
#include <thread>
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
std::vector<Device>                   devices;
std::vector<SDL_AudioStream*>         streams;
Rumble                                rumble {};
Uint64                                now        = 1000;
int                                   audio_refs = 0, opens = 0, rumble_calls = 0;
bool                                  fail_open = false, fail_resume = false;
int                                   actual_channels = 4;
SDL_AudioDeviceID                     opened_device   = 0;
SDL_AudioSpec                         opened_spec {};
std::array<Rumble, 4>                 pad_rumble {};
std::array<std::vector<uint8_t>, 4>   effects;
std::vector<SDL_JoystickID>           connected_pads {1};
std::array<Rumble, 4>                 cached_rumble {};
bool                                  fail_effect = false, wireless = false, wireless_pad3 = false;
bool                                  hid_available = false, fail_hid_write = false;
bool                                  duplicate_hid  = false;
int                                   hid_opens      = 0;
int                                   opened_hid_pad = 0;
std::vector<std::array<uint8_t, 547>> hid_reports;
std::atomic_bool                      block_opus_encode {false}, block_hid_write {false};
std::atomic_int                       hid_closes {0};
std::binary_semaphore                 encode_started {0}, allow_encode {0};
std::binary_semaphore                 write_started {0}, allow_write {0};
SDL_AudioStream*                      default_stream      = nullptr;
bool                                  fail_default_resume = false;
int                                   active_controller   = 1;
float                                 speaker_scale = 1.0f, vibration_scale = 1.0f;
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
	if (it != streams.end()) {
		streams.erase(it);
	}
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
SDL_GamepadType GetGamepadType(SDL_Gamepad* pad) {
	return GetGamepadTypeForID(static_cast<SDL_JoystickID>(reinterpret_cast<uintptr_t>(pad)));
}
Uint16 GetGamepadVendor(SDL_Gamepad*) {
	return 0x054c;
}
Uint16 GetGamepadProduct(SDL_Gamepad*) {
	return 0x0ce6;
}
const char* GetGamepadSerial(SDL_Gamepad* pad) {
	return reinterpret_cast<uintptr_t>(pad) == 3 ? "aa-bb-cc-dd-ee-03" : "aa-bb-cc-dd-ee-01";
}
Uint64 GetTicksNS() {
	return now * 1000000;
}
SDL_Thread* CreateThread(SDL_ThreadFunction, const char*, void*) {
	return reinterpret_cast<SDL_Thread*>(1);
}
void                 WaitThread(SDL_Thread*, int*) {}
SDL_hid_device_info* HidEnumerate(unsigned short, unsigned short) {
	if (!hid_available) {
		return nullptr;
	}
	static wchar_t             serial1[] = L"AA:BB:CC:DD:EE:01";
	static wchar_t             serial3[] = L"AA:BB:CC:DD:EE:03";
	static char                path1[]   = "pad-1";
	static char                path3[]   = "pad-3";
	static SDL_hid_device_info pad3 {}, pad1 {};
	pad3.path          = path3;
	pad3.serial_number = duplicate_hid ? serial1 : serial3;
	pad3.bus_type      = SDL_HID_API_BUS_BLUETOOTH;
	pad1.path          = path1;
	pad1.serial_number = serial1;
	pad1.bus_type      = SDL_HID_API_BUS_BLUETOOTH;
	pad1.next          = &pad3;
	return &pad1;
}
void            HidFreeEnumeration(SDL_hid_device_info*) {}
SDL_hid_device* HidOpenPath(const char* path) {
	hid_opens++;
	opened_hid_pad = path[4] == '1' ? 1 : 3;
	return reinterpret_cast<SDL_hid_device*>(static_cast<uintptr_t>(opened_hid_pad));
}
void HidClose(SDL_hid_device*) {
	hid_closes++;
}
int HidWrite(SDL_hid_device*, const unsigned char* data, size_t size) {
	if (block_hid_write.exchange(false)) {
		write_started.release();
		allow_write.acquire();
	}
	if (fail_hid_write || size != 547) {
		return -1;
	}
	std::array<uint8_t, 547> report {};
	std::copy_n(data, report.size(), report.begin());
	hid_reports.push_back(report);
	return static_cast<int>(size);
}
opus_int32 OpusEncodeFloat(OpusEncoder* encoder, const float* pcm, int frames,
                           unsigned char* packet, opus_int32 bytes) {
	if (block_opus_encode.exchange(false)) {
		encode_started.release();
		allow_encode.acquire();
	}
	return ::opus_encode_float(encoder, pcm, frames, packet, bytes);
}
SDL_JoystickID* GetGamepads(int* count) {
	*count    = static_cast<int>(connected_pads.size());
	auto* ids = static_cast<SDL_JoystickID*>(
	    SDL_malloc((connected_pads.size() + 1) * sizeof(SDL_JoystickID)));
	std::copy(connected_pads.begin(), connected_pads.end(), ids);
	ids[connected_pads.size()] = 0;
	return ids;
}
SDL_JoystickConnectionState GetGamepadConnectionState(SDL_Gamepad* pad) {
	return wireless || (wireless_pad3 && reinterpret_cast<uintptr_t>(pad) == 3)
	           ? SDL_JOYSTICK_CONNECTION_WIRELESS
	           : SDL_JOYSTICK_CONNECTION_WIRED;
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
#define SDL_GetGamepadType          Fake::GetGamepadType
#define SDL_GetGamepadVendor        Fake::GetGamepadVendor
#define SDL_GetGamepadProduct       Fake::GetGamepadProduct
#define SDL_GetGamepadSerial        Fake::GetGamepadSerial
#define SDL_GetTicksNS              Fake::GetTicksNS
#undef SDL_CreateThread
#define SDL_CreateThread              Fake::CreateThread
#define SDL_WaitThread                Fake::WaitThread
#define SDL_hid_enumerate             Fake::HidEnumerate
#define SDL_hid_free_enumeration      Fake::HidFreeEnumeration
#define SDL_hid_open_path             Fake::HidOpenPath
#define SDL_hid_close                 Fake::HidClose
#define SDL_hid_write                 Fake::HidWrite
#define opus_encode_float             Fake::OpusEncodeFloat
#define SDL_GetGamepadFromID          Fake::GetGamepadFromID
#define SDL_RumbleGamepad             Fake::RumbleGamepad
#define SDL_SendGamepadEffect         Fake::SendGamepadEffect
#define SDL_GetGamepads               Fake::GetGamepads
#define SDL_GetGamepadConnectionState Fake::GetGamepadConnectionState
#include "libs/dualSenseBluetooth.cpp"
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
#undef SDL_GetGamepadType
#undef SDL_GetGamepadVendor
#undef SDL_GetGamepadProduct
#undef SDL_GetGamepadSerial
#undef SDL_GetTicksNS
#undef SDL_CreateThread
#undef SDL_WaitThread
#undef SDL_hid_enumerate
#undef SDL_hid_free_enumeration
#undef SDL_hid_open_path
#undef SDL_hid_close
#undef SDL_hid_write
#undef opus_encode_float
#undef SDL_GetGamepadFromID
#undef SDL_RumbleGamepad
#undef SDL_SendGamepadEffect
#undef SDL_GetGamepads
#undef SDL_GetGamepadConnectionState

namespace Libs::Controller {
int GetActiveControllerId() {
	return active_controller;
}
bool IsActivePadNonSony() {
	return false;
}
void SubmitHapticsRumble(float, float) {}
float GetSettingScale(Setting setting) {
	return setting == Setting::SpeakerVolume ? speaker_scale : vibration_scale;
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
namespace Bluetooth = Libs::Controller::DualSenseBluetooth;
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
		fail_effect = wireless = wireless_pad3 = false;
		hid_available = fail_hid_write = false;
		duplicate_hid                  = false;
		hid_opens                      = 0;
		hid_closes                     = 0;
		opened_hid_pad                 = 0;
		hid_reports.clear();
		default_stream      = nullptr;
		fail_default_resume = false;
		active_controller   = 1;
		speaker_scale = vibration_scale = 1.0f;
		now                 = 1000;
		opens = rumble_calls = 0;
		fail_open = fail_resume = false;
		actual_channels         = 4;
	}
	~Fixture() {
		Haptics::Shutdown();
		Check(streams.empty() && audio_refs == 0, "stream or audio subsystem reference leaked");
	}
};

Port Open(bool speaker = false, uint32_t freq = 48000) {
	auto* stream = Haptics::Open(freq, speaker);
	Check(stream != nullptr, "port open failed");
	return Port(stream, Haptics::Close);
}
void Queue(const Port& port, const void* data, uint32_t frames = 2, uint32_t channels = 2,
           bool is_float = true, const int* volume = unity.data(), float gain = 1.0f) {
	Haptics::Queue(port.get(), 1, data, frames, channels, is_float, volume, gain);
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
	const std::array gains {0.25f, 2.0f};
	Haptics::Queue(port.get(), 1, stereo.data(), 2, 2, true, volume.data(), 1.0f, gains.data());
	ExpectPcm({0, 0, 0.125f, -0.25f, 0, 0, 0.25f, 0});
	const std::array<int16_t, 2> mono {16384, -32768};
	Queue(port, mono.data(), 2, 1, false, volume.data() + 1);
	ExpectPcm({0, 0, 0.25f, 0.25f, 0, 0, -0.5f, -0.5f});
	Queue(port, mono.data(), 2, 1, false, volume.data() + 1, 0.5f);
	ExpectPcm({0, 0, 0.125f, 0.125f, 0, 0, -0.25f, -0.25f});
	Haptics::Queue(port.get(), 1, mono.data(), 2, 1, false, volume.data() + 1, 1.0f, gains.data());
	ExpectPcm({0, 0, 0.0625f, 0.0625f, 0, 0, -0.125f, -0.125f});
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
	devices.clear();
	now += 2001;
	Queue(port, pcm.data());
	Check(streams.empty(), "disconnected endpoint retained its stream");
	devices = {{10, "Wireless Controller", 4}};
	now += 2001;
	Queue(port, pcm.data());
	Check(streams.size() == 1 && opened_device == 10,
	      "Linux Wireless Controller endpoint was not discovered");
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

void TestScaledRumbleDuration() {
	for (bool bluetooth: {false, true}) {
		Fixture f;
		wireless = hid_available = bluetooth;
		auto               port  = Open();
		std::vector<float> sound(2048, 0.5f);
		Haptics::SetVibration(1, 100, 50, 1000);
		Queue(port, sound.data(), 1024, 2, true, unity.data(), 0.0f);
		Check(rumble.large == 100 * 0x101, "muted haptics suppressed motor rumble");
		Queue(port, sound.data(), 1024, 2, true, unity.data(), 0.5f);
		Check(rumble.large == 0, "scaled haptics did not suppress motor rumble");
		now += 100;
		Haptics::SetVibration(1, 50, 25, 900);
		now += 200;
		if (bluetooth) {
			Haptics::UpdateBluetoothRumble();
		} else {
			Pull();
		}
		Check(rumble.large == 50 * 0x101 && rumble.small == 25 * 0x101 && rumble.duration == 700,
		      "scaled rumble lost its strength or extended its original expiry");
		Haptics::SetVibration(1, 50, 25, 0);
		Check(rumble.large == 0 && rumble.small == 0 && rumble.duration == 0,
		      "expired rumble was restarted");
	}
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

void TestBluetoothAudioAndIdentity() {
	Fixture f;
	wireless = hid_available = true;
	connected_pads           = {1, 3};
	devices.clear(); // Bluetooth does not enumerate a quad USB audio endpoint.
	auto               vibration = Open();
	auto               speaker   = Open(true);
	std::vector<float> haptic(2048, 0.5f);
	std::vector<float> sound(2048, 0.25f);
	Check(Haptics::SetVibration(1, 100, 80) && pad_rumble[1].large == 100 * 0x101,
	      "Bluetooth controller did not receive normal rumble");
	Check(Haptics::Queue(vibration.get(), 1, haptic.data(), 1024, 2, true, unity.data()) != 0,
	      "Bluetooth haptics did not open");
	Check(Haptics::Queue(speaker.get(), 1, sound.data(), 1024, 2, true, unity.data()) != 0,
	      "Bluetooth speaker did not open");
	Check(hid_opens == 1 && opened_hid_pad == 1 && effects[1][7] == 0x30,
	      "Bluetooth audio was not matched to the active controller");
	Check(pad_rumble[1].large == 0, "Bluetooth haptics did not suppress motor rumble");
	auto pending = Libs::Controller::DualSenseBluetooth::Prepare(now * 1000000);
	Check(pending.size() == 1, "Bluetooth streams were not combined in one report");
	Libs::Controller::DualSenseBluetooth::Send(pending);
	Check(hid_reports.size() == 1, "Bluetooth audio report was not sent");
	const auto& report = hid_reports.back();
	Check(report[0] == 0x39 && report[2] == 0x91 && report[3] == 6 && report[4] == 0x7e &&
	          report[10] == 0xd2 && report[11] == 64 && report[140] == 0xd3 && report[141] == 200,
	      "Bluetooth audio sub-packets are malformed");
	Check(report[12] > 0 && report[13] > 0,
	      "haptic waveform was not downsampled into the actuator channels");
	uint8_t prefix = 0xa2;
	auto    crc    = SDL_crc32(0, &prefix, 1);
	crc            = SDL_crc32(crc, report.data(), report.size() - 4);
	for (size_t i = 0; i < 4; i++) {
		Check(report[report.size() - 4 + i] == static_cast<uint8_t>(crc >> (8 * i)),
		      "Bluetooth audio CRC is invalid");
	}
	int   opus_error = OPUS_OK;
	auto* decoder    = opus_decoder_create(48000, 2, &opus_error);
	Check(decoder != nullptr && opus_error == OPUS_OK, "Opus decoder unavailable in test");
	std::array<float, 960> decoded {};
	Check(opus_decode_float(decoder, report.data() + 142, 200, decoded.data(), 480, 0) == 480,
	      "Bluetooth speaker packet could not be decoded");
	opus_decoder_destroy(decoder);
	const auto mean = std::accumulate(decoded.begin(), decoded.end(), 0.0f) / decoded.size();
	Check(mean > 0.05f && mean < 0.35f, "Bluetooth speaker audio was not preserved");
	Check(Libs::Controller::DualSenseBluetooth::Prepare(now * 1000000).empty(),
	      "Bluetooth sender ignored report pacing");
	now += 300;
	Haptics::UpdateBluetoothRumble();
	Check(pad_rumble[1].large == 100 * 0x101,
	      "Bluetooth haptics did not restore normal rumble afterward");

	// A different active DualSense has a different HID address, even if both are
	// paired.
	Check(Haptics::Queue(vibration.get(), 3, haptic.data(), 1024, 2, true, unity.data()) != 0 &&
	          opened_hid_pad == 3,
	      "Bluetooth stream followed the wrong controller");
	Check(Haptics::Queue(speaker.get(), 3, sound.data(), 1024, 2, true, unity.data()) != 0,
	      "Bluetooth speaker did not follow the selected controller");
	Check(effects[1][7] == 0, "old Bluetooth controller remained speaker-routed");
	wireless       = false;
	connected_pads = {3};
	devices        = {{20, "Speakers (DualSense Wireless Controller)", 4}};
	Check(Haptics::Queue(speaker.get(), 3, sound.data(), 1024, 2, true, unity.data()) != 0 &&
	          opened_device == 20 && effects[3][7] == 0x30,
	      "Bluetooth to USB change did not reopen the USB speaker immediately");
	speaker.reset();
	vibration.reset();
}

void TestBluetoothFailureFallsBack() {
	Fixture f;
	wireless = hid_available   = true;
	auto               speaker = Open(true);
	std::vector<float> sound(2048, 0.25f);
	Check(Haptics::Queue(speaker.get(), 1, sound.data(), 1024, 2, true, unity.data()) != 0,
	      "Bluetooth port did not initially open");
	fail_hid_write = true;
	auto pending   = Libs::Controller::DualSenseBluetooth::Prepare(now * 1000000);
	Libs::Controller::DualSenseBluetooth::Send(pending);
	Check(Haptics::Queue(speaker.get(), 1, sound.data(), 1024, 2, true, unity.data()) == 0 &&
	          effects[1][7] == 0,
	      "failed HID write did not restore the normal audio fallback");
	speaker.reset();
}

void TestBluetoothFormatsAndRoutingFailure() {
	Fixture f;
	wireless = hid_available = fail_effect = true;
	auto speaker                           = Open(true);
	Check(Haptics::Queue(speaker.get(), 1, pcm.data(), 2, 2, true, unity.data()) == 0 &&
	          !Haptics::UsesBluetooth(speaker.get()) && hid_closes == 1 && effects[1].empty(),
	      "failed Bluetooth speaker routing leaked a session or suppressed fallback");
	fail_effect = false;
	now += 2001;
	std::vector<int16_t> mono(1024, 16384);
	const int            volume = 16384;
	Queue(speaker, mono.data(), 1024, 1, false, &volume, 0.5f);
	auto pending = Bluetooth::Prepare(now * 1000000);
	Check(pending.size() == 1 && hid_opens == 2 && effects[1][7] == 0x30 &&
	          std::abs(pending[0].audio[200] - 0.125f) < 1e-4f &&
	          std::abs(pending[0].audio[201] - 0.125f) < 1e-4f,
	      "Bluetooth mono int16 volume/gain mapping or speaker routing recovery failed");
}

void TestBluetoothEncodingDoesNotBlockAudioQueue() {
	Fixture f;
	wireless = hid_available = true;
	auto speaker = Open(true);
	std::vector<float> sound(2048, 0.25f);
	Check(Haptics::Queue(speaker.get(), 1, sound.data(), 1024, 2, true, unity.data()) != 0,
	      "Bluetooth speaker did not open for concurrent encoding test");
	block_opus_encode = true;
	std::thread encoder([&] {
		auto pending = Libs::Controller::DualSenseBluetooth::Prepare(now * 1000000);
		Libs::Controller::DualSenseBluetooth::Send(pending);
	});
	encode_started.acquire();
	std::binary_semaphore queue_finished {0};
	std::atomic_bool      queue_ok {false};
	std::thread audio([&] {
		queue_ok = Haptics::Queue(speaker.get(), 1, sound.data(), 1024, 2, true,
		                          unity.data()) != 0;
		queue_finished.release();
	});
	const bool queued_without_encoder =
	    queue_finished.try_acquire_for(std::chrono::seconds(2));
	allow_encode.release();
	audio.join();
	encoder.join();
	Check(queued_without_encoder && queue_ok,
	      "Bluetooth audio queue waited for Opus encoding");
	speaker.reset();
}

void TestBluetoothCloseCancelsPreparedReport() {
	Fixture f;
	wireless = hid_available     = true;
	auto               speaker   = Open(true);
	auto               vibration = Open();
	std::vector<float> sound(2048, 0.25f);
	Queue(speaker, sound.data(), 1024);
	Queue(vibration, sound.data(), 1024);
	const auto base    = now * 1000000;
	auto       pending = Bluetooth::Prepare(base);
	Check(pending.size() == 1, "Bluetooth report was not prepared before stream close");
	speaker.reset();
	Bluetooth::Send(pending);
	Check(hid_reports.empty(), "closed speaker's prepared report was sent");
	Queue(vibration, sound.data(), 1024);
	Bluetooth::Send(Bluetooth::Prepare(base + Bluetooth::PERIOD_NS));
	Check(hid_reports.size() == 1 && hid_reports.back()[140] == 0,
	      "closing the speaker prevented the remaining haptic stream from sending");
}

void TestBluetoothCloseDuringEncoding() {
	Fixture f;
	wireless = hid_available   = true;
	auto               speaker = Open(true);
	std::vector<float> sound(2048, 0.25f);
	Queue(speaker, sound.data(), 1024);
	block_opus_encode = true;
	std::thread encoder([&] { Bluetooth::Send(Bluetooth::Prepare(now * 1000000)); });
	encode_started.acquire();
	std::binary_semaphore close_finished {0};
	std::thread           closer([&] {
		speaker.reset();
		close_finished.release();
	});
	const bool closed_without_encoder  = close_finished.try_acquire_for(std::chrono::seconds(2));
	const bool hid_alive_during_encode = hid_closes == 0;
	allow_encode.release();
	closer.join();
	encoder.join();
	Check(closed_without_encoder && hid_alive_during_encode && hid_closes == 1 &&
	          hid_reports.empty(),
	      "closing during encoding blocked, destroyed a live session, or sent stale audio");
}

void TestBluetoothCloseWaitsForWriteWithoutBlockingQueue() {
	Fixture f;
	wireless = hid_available     = true;
	auto               speaker   = Open(true);
	auto               vibration = Open();
	std::vector<float> sound(2048, 0.25f);
	Queue(speaker, sound.data(), 1024);
	Queue(vibration, sound.data(), 1024);
	auto pending = Bluetooth::Prepare(now * 1000000);
	Check(pending.size() == 1, "Bluetooth report was not prepared before blocked write");
	block_hid_write = true;
	std::thread sender([&] { Bluetooth::Send(pending); });
	write_started.acquire();
	std::binary_semaphore close_started {0}, close_finished {0}, queue_finished {0};
	std::thread           closer([&] {
		close_started.release();
		speaker.reset();
		close_finished.release();
	});
	close_started.acquire();
	const bool closed_before_write = close_finished.try_acquire_for(std::chrono::milliseconds(50));
	std::atomic_bool queue_ok {false};
	std::thread      audio([&] {
		queue_ok =
		    Haptics::Queue(vibration.get(), 1, sound.data(), 1024, 2, true, unity.data()) != 0;
		queue_finished.release();
	});
	const bool       queued_without_write = queue_finished.try_acquire_for(std::chrono::seconds(2));
	allow_write.release();
	audio.join();
	closer.join();
	sender.join();
	Check(!closed_before_write && queued_without_write && queue_ok && hid_reports.size() == 1 &&
	          effects[1][7] == 0,
	      "close failed to drain the HID write independently of the audio queue");
}

void TestBluetoothSpeakerWaitsForItsAudioBlock() {
	Fixture f;
	wireless = hid_available = true;
	auto vibration = Open();
	auto speaker = Open(true);
	std::vector<float> haptic(2048, 0.5f);
	std::vector<float> sound(2048, 0.25f);
	Check(Haptics::Queue(vibration.get(), 1, haptic.data(), 1024, 2, true, unity.data()) != 0,
	      "Bluetooth vibration stream did not open");
	Check(Libs::Controller::DualSenseBluetooth::Prepare(now * 1000000).empty(),
	      "haptics sent a silent speaker packet before the audio batch arrived");
	Check(Haptics::Queue(speaker.get(), 1, sound.data(), 256, 2, true, unity.data()) != 0,
	      "Bluetooth speaker did not accept a short block");
	Check(Libs::Controller::DualSenseBluetooth::Prepare(now * 1000000).empty(),
	      "short Bluetooth speaker block was padded with silence immediately");
	Check(Haptics::Queue(speaker.get(), 1, sound.data(), 768, 2, true, unity.data()) != 0,
	      "Bluetooth speaker did not accept the remainder of its block");
	auto reports = Libs::Controller::DualSenseBluetooth::Prepare(now * 1000000);
	Check(reports.size() == 1 && reports[0].audio[100] > 0.2f && reports[0].data[12] > 0,
	      "Bluetooth speaker and haptics were not combined into a complete report");
	speaker.reset();
	vibration.reset();
}

void TestBluetoothShortHapticBlock() {
	Fixture f;
	wireless = hid_available     = true;
	auto               vibration = Open();
	std::vector<float> sound(2048, 0.25f);
	Queue(vibration, sound.data(), 256);
	const auto base = now * 1000000;
	Check(Bluetooth::Prepare(base).empty() && Bluetooth::Prepare(base + 1000000).empty(),
	      "short haptic block was padded with silence before its remainder arrived");
	Queue(vibration, sound.data(), 768);
	Check(Bluetooth::Prepare(base + 1000000).empty(),
	      "completed haptic block skipped its speaker coalescing window");
	auto pending = Bluetooth::Prepare(base + 2000000);
	Check(pending.size() == 1 && pending[0].data[12 + 100] > 0,
	      "complete haptic block did not play after its remainder arrived");
}

void TestBluetoothIncrementalBatchCoalesces() {
	Fixture f;
	wireless = hid_available = true;
	auto                   vibration = Open();
	auto speaker = Open(true);
	std::array<float, 512> sound;
	sound.fill(0.25f);
	for (int block = 0; block < 3; ++block) {
		Queue(vibration, sound.data(), 256);
		Queue(speaker, sound.data(), 256);
		Check(Bluetooth::Prepare(now * 1000000).empty(),
		      "incremental Bluetooth batch was padded before it was complete");
		now += 5;
	}
	Queue(vibration, sound.data(), 256);
	Check(Bluetooth::Prepare(now * 1000000).empty(),
	      "completed haptics sent before the same batch's final speaker block arrived");
	Queue(speaker, sound.data(), 256);
	auto pending = Bluetooth::Prepare(now * 1000000);
	Check(pending.size() == 1 && pending[0].audio[1800] > 0.2f && pending[0].data[12 + 100] > 0,
	      "completed incremental speaker and haptics batch did not play immediately");
}

void TestBluetoothLowRateCompleteBlock() {
	Fixture f;
	wireless = hid_available           = true;
	auto                       speaker = Open(true, 8000);
	std::array<float, 171 * 2> sound;
	sound.fill(0.25f);
	Queue(speaker, sound.data(), 171);
	auto pending = Bluetooth::Prepare(now * 1000000);
	Check(pending.size() == 1 && pending[0].audio[1000] > 0.2f,
	      "resampler lookahead delayed a complete low-rate speaker block");
}

void TestBluetoothCompleteStreamDoesNotWaitForPartialPeer() {
	Fixture f;
	wireless = hid_available    = true;
	auto               complete = Open(true);
	auto               partial  = Open(true);
	std::vector<float> sound(2048, 0.25f);
	Queue(complete, sound.data(), 1024);
	Queue(partial, sound.data(), 256);
	auto pending = Bluetooth::Prepare(now * 1000000);
	Check(pending.size() == 1 && pending[0].audio[100] > 0.4f && pending[0].audio[1000] > 0.2f,
	      "complete speaker stream waited for a partial peer instead of mixing available audio");
}

void TestBluetoothShortFinalBlock() {
	for (bool speaker: {false, true}) {
		Fixture f;
		wireless = hid_available = true;
		auto               port  = Open(speaker);
		std::vector<float> sound(2048, 0.25f);
		Queue(port, sound.data(), 256);
		const auto base = now * 1000000;
		Check(Bluetooth::Prepare(base).empty(),
		      "short final Bluetooth block sent before more audio could arrive");
		Check(Bluetooth::Prepare(base + Bluetooth::PERIOD_NS).size() == 1,
		      "short final Bluetooth block was never played");
	}
}

void TestBluetoothOverflowKeepsRecentAudio() {
	Fixture f;
	wireless = hid_available = true;
	auto speaker = Open(true);
	std::vector<float> sound(2048);
	for (int block = 1; block <= 4; ++block) {
		std::fill(sound.begin(), sound.end(), block * 0.1f);
		Check(Haptics::Queue(speaker.get(), 1, sound.data(), 1024, 2, true,
		                     unity.data()) != 0,
		      "Bluetooth speaker overflow queue failed");
	}
	auto first = Libs::Controller::DualSenseBluetooth::Prepare(now * 1000000);
	Check(first.size() == 1 && first[0].audio[200] > 0.15f && first[0].audio[200] < 0.25f,
	      "Bluetooth queue overflow lost all buffered audio instead of the oldest block");
	auto second = Libs::Controller::DualSenseBluetooth::Prepare(now * 1000000 +
	                                                        Libs::Controller::DualSenseBluetooth::PERIOD_NS);
	Check(second.size() == 1 && second[0].audio[200] > 0.25f && second[0].audio[200] < 0.35f,
	      "Bluetooth queue overflow did not preserve the next audio block");
	speaker.reset();
}

void TestBluetoothHapticOverflowKeepsRecentAudio() {
	Fixture f;
	wireless = hid_available = true;
	auto vibration = Open();
	std::vector<float> haptic(2048);
	for (int block = 1; block <= 4; ++block) {
		std::fill(haptic.begin(), haptic.end(), block * 0.1f);
		Check(Haptics::Queue(vibration.get(), 1, haptic.data(), 1024, 2, true,
		                     unity.data()) != 0,
		      "Bluetooth haptic overflow queue failed");
	}
	const auto base = now * 1000000;
	Check(Libs::Controller::DualSenseBluetooth::Prepare(base).empty(),
	      "haptic-only report skipped its coalescing window");
	auto first = Libs::Controller::DualSenseBluetooth::Prepare(base + 1000000);
	Check(first.size() == 1 && first[0].data[12 + 32] > 18 && first[0].data[12 + 32] < 32,
	      "Bluetooth haptic overflow discarded buffered waveform instead of one report");
	vibration.reset();
}

void TestBluetoothRecoversAfterLateWrite() {
	Fixture f;
	wireless = hid_available = true;
	auto speaker = Open(true);
	std::vector<float> sound(2048, 0.25f);
	for (int i = 0; i < 3; ++i) {
		Check(Haptics::Queue(speaker.get(), 1, sound.data(), 1024, 2, true,
		                     unity.data()) != 0,
		      "Bluetooth speaker could not buffer three reports");
	}
	const auto base = now * 1000000;
	const auto period = Libs::Controller::DualSenseBluetooth::PERIOD_NS;
	Check(Libs::Controller::DualSenseBluetooth::Prepare(base).size() == 1,
	      "first Bluetooth report was not prepared");
	Check(Libs::Controller::DualSenseBluetooth::Prepare(base + period + 10000000).size() == 1,
	      "late Bluetooth report was not prepared");
	Check(Libs::Controller::DualSenseBluetooth::Prepare(base + 2 * period).size() == 1,
	      "Bluetooth sender did not recover its schedule after a late write");
	speaker.reset();
}

void TestUsbOverflowKeepsRecentAudio() {
	Fixture f;
	auto speaker = Open(true);
	std::vector<float> sound(2048);
	for (int block = 1; block <= 4; ++block) {
		std::fill(sound.begin(), sound.end(), block * 0.1f);
		Check(Haptics::Queue(speaker.get(), 1, sound.data(), 1024, 2, true,
		                     unity.data()) != 0,
		      "USB speaker overflow queue failed");
	}
	std::array<float, 4> first {};
	Check(!streams.empty() &&
	          SDL_GetAudioStreamData(streams.back(), first.data(), sizeof(first)) ==
	              sizeof(first) &&
	          first[0] > 0.15f && first[0] < 0.25f,
	      "USB queue overflow did not preserve the newest buffered audio");
	// A single oversized input must also preserve only its newest 80 ms.
	sound.assign(4096 * 2, 0.1f);
	std::fill(sound.begin() + 256 * 2, sound.end(), 0.5f);
	const auto queued = Haptics::Queue(speaker.get(), 1, sound.data(), 4096, 2, true, unity.data());
	Check(queued == 80000 && SDL_GetAudioStreamQueued(streams.back()) == 3840 * 4 * sizeof(float) &&
	          SDL_GetAudioStreamData(streams.back(), first.data(), sizeof(first)) ==
	              sizeof(first) &&
	          first[0] == 0.5f && first[1] == 0.5f,
	      "oversized USB block did not retain only the newest 80 ms");
	speaker.reset();
}

void TestBluetoothAmbiguousDevice() {
	Fixture f;
	wireless = hid_available = duplicate_hid = true;
	auto speaker                             = Open(true);
	Check(Haptics::Queue(speaker.get(), 1, pcm.data(), 2, 2, true, unity.data()) == 0 &&
	          hid_opens == 0 && effects[1].empty(),
	      "ambiguous Bluetooth devices were routed arbitrarily");
	speaker.reset();
}

void TestUsbAndBluetoothFeatureParity() {
	Fixture f;
	connected_pads = {1, 3};
	wireless_pad3 = hid_available       = true;
	auto               wired_speaker    = Open(true);
	auto               wireless_speaker = Open(true);
	std::vector<float> sound(2048, 0.25f);
	Check(Haptics::Queue(wired_speaker.get(), 1, sound.data(), 1024, 2, true, unity.data()) != 0 &&
	          opened_device == 10,
	      "Bluetooth pad disabled the wired pad speaker");
	Check(Haptics::Queue(wireless_speaker.get(), 3, sound.data(), 1024, 2, true, unity.data()) !=
	              0 &&
	          opened_hid_pad == 3,
	      "wired pad disabled the Bluetooth speaker");
	Check(Haptics::Queue(wired_speaker.get(), 1, sound.data(), 1024, 2, true, unity.data()) != 0 &&
	          effects[1][7] == 0x30 && effects[3][7] == 0,
	      "returning to the wired speaker left Bluetooth routing active");
	wireless_speaker.reset();
	wired_speaker.reset();
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
	speaker_scale = 0.5f;
	audio.AudioOutOutputs(&output, 1, false);
	ExpectPcm({0.25f, 0.25f, 0, 0, 0.25f, 0.25f, 0, 0});
	speaker_scale = 2.0f;
	audio.AudioOutOutputs(&output, 1, false);
	ExpectPcm({1.0f, 1.0f, 0, 0, 1.0f, 1.0f, 0, 0});
	speaker_scale = 0.0f;
	audio.AudioOutOutputs(&output, 1, false);
	ExpectPcm({});
	active_controller = 2;
	audio.AudioOutOutputs(&output, 1, false);
	std::array<float, 4> fallback {};
	Check(SDL_GetAudioStreamData(default_stream, fallback.data(), sizeof(fallback)) ==
	              sizeof(fallback) &&
	          fallback == std::array<float, 4> {},
	      "unplugging the controller unmuted its speaker fallback");
}

void TestAudioFallbackGain() {
	for (bool is_float: {false, true}) {
		Fixture            f;
		Libs::Audio::Audio audio;
		active_controller = 2;
		speaker_scale     = 2.0f;
		const std::array<int16_t, 4> integer_pcm {16384, 16384, 16384, 16384};
		const auto  format = is_float ? Libs::Audio::Audio::Format::FloatStereo
		                              : Libs::Audio::Audio::Format::Signed16bitStereo;
		const void* data   = is_float ? static_cast<const void*>(pcm.data()) : integer_pcm.data();
		for (int type: {0, 4}) {
			const auto               port = audio.AudioOutOpen(type, 2, 48000, format);
			const std::array<int, 2> volume {16384, 16384};
			audio.AudioOutSetVolume(port, 3, volume.data());
			Libs::Audio::Audio::OutputParam output {port, data};
			audio.AudioOutOutputs(&output, 1, false);
			const float          expected = type == 4 ? 0.5f : 0.25f;
			std::array<float, 4> actual {};
			if (is_float) {
				Check(SDL_GetAudioStreamData(default_stream, actual.data(), sizeof(actual)) ==
				          sizeof(actual),
				      "float fallback output is missing");
			} else {
				std::array<int16_t, 4> integers {};
				Check(SDL_GetAudioStreamData(default_stream, integers.data(), sizeof(integers)) ==
				          sizeof(integers),
				      "integer fallback output is missing");
				std::transform(integers.begin(), integers.end(), actual.begin(),
				               [](int16_t value) { return value / 32768.0f; });
			}
			Check(std::all_of(actual.begin(), actual.end(),
			                  [expected](float value) { return value == expected; }),
			      "speaker setting gain was lost or applied to ordinary audio");
			audio.AudioOutClose(port);
		}
	}
}

void TestAudioVibrationGain() {
	for (bool bluetooth: {false, true}) {
		Fixture f;
		wireless = hid_available = bluetooth;
		Libs::Audio::Audio audio;
		vibration_scale = 0.5f;
		speaker_scale   = 0.0f;
		const auto port =
		    audio.AudioOutOpen(10, 1024, 48000, Libs::Audio::Audio::Format::FloatStereo);
		std::vector<float>              sound(2048, 0.5f);
		Libs::Audio::Audio::OutputParam output {port, sound.data()};
		audio.AudioOutOutputs(&output, 1, false);
		Check(default_stream == nullptr, "vibration port opened ordinary audio");
		if (bluetooth) {
			Check(Bluetooth::Prepare(now * 1000000).empty(),
			      "Bluetooth vibration skipped its coalescing window");
			const auto pending = Bluetooth::Prepare((now + 1) * 1000000);
			Check(pending.size() == 1 && pending[0].data[76] == 32,
			      "Bluetooth vibration did not apply its own setting gain");
		} else {
			ExpectPcm({0, 0, 0.25f, 0.25f, 0, 0, 0.25f, 0.25f});
		}
	}
}

void TestBluetoothAudioSpeakerRouting() {
	Fixture f;
	wireless = hid_available = true;
	devices.clear();
	Libs::Audio::Audio audio;
	const auto port = audio.AudioOutOpen(4, 1024, 48000, Libs::Audio::Audio::Format::FloatStereo);
	Check(port.IsValid() && default_stream != nullptr, "Bluetooth pad speaker port did not open");
	std::vector<float> sound(2048, 0.25f);
	speaker_scale = 2.0f;
	Libs::Audio::Audio::OutputParam output {port, sound.data()};
	Check(audio.AudioOutOutputs(&output, 1, false) == 1024 && hid_opens == 1 &&
	          SDL_GetAudioStreamQueued(default_stream) == 0,
	      "Bluetooth pad speaker also played on the host output");
	fail_hid_write = true;
	auto pending = Libs::Controller::DualSenseBluetooth::Prepare(now * 1000000);
	Check(pending.size() == 1 && std::abs(pending[0].audio[200] - 0.5f) < 1e-4f,
	      "Bluetooth speaker did not apply its setting gain");
	Libs::Controller::DualSenseBluetooth::Send(pending);
	Check(audio.AudioOutOutputs(&output, 1, false) == 1024 &&
	          SDL_GetAudioStreamQueued(default_stream) > 0,
	      "Bluetooth write failure did not use the normal speaker output");
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
	Config::Initialize();
	TestAudioSpeakerFallback();
	TestAudioSpeakerRouting();
	TestAudioFallbackGain();
	TestAudioVibrationGain();
	TestBluetoothAudioSpeakerRouting();
	TestAudioDefaultResumeFailure();
	TestFormatsAndVolume();
	TestDiscoveryAndHotplug();
	TestSpeaker();
	TestSpeakerUnplug();
	TestSpeakerSwitchAndFailures();
	TestSpeakerRequiresUnambiguousUsbDevice();
	TestBluetoothAudioAndIdentity();
	TestBluetoothFailureFallsBack();
	TestBluetoothFormatsAndRoutingFailure();
	TestBluetoothEncodingDoesNotBlockAudioQueue();
	TestBluetoothCloseCancelsPreparedReport();
	TestBluetoothCloseDuringEncoding();
	TestBluetoothCloseWaitsForWriteWithoutBlockingQueue();
	TestBluetoothSpeakerWaitsForItsAudioBlock();
	TestBluetoothShortHapticBlock();
	TestBluetoothIncrementalBatchCoalesces();
	TestBluetoothLowRateCompleteBlock();
	TestBluetoothCompleteStreamDoesNotWaitForPartialPeer();
	TestBluetoothShortFinalBlock();
	TestBluetoothOverflowKeepsRecentAudio();
	TestBluetoothHapticOverflowKeepsRecentAudio();
	TestBluetoothRecoversAfterLateWrite();
	TestUsbOverflowKeepsRecentAudio();
	TestBluetoothAmbiguousDevice();
	TestUsbAndBluetoothFeatureParity();
	TestFailuresAndBoundedQueue();
	TestRumbleLeaseAndDuration();
	TestScaledRumbleDuration();
	TestSwitchStopsOldRumble();
	TestCloseRestoresRumble();
	std::printf("PadHapticsTests: all cases passed\n");
}
