#include "libs/audio.h"
#include "libs/audio_internal.h"
#include "libs/errno.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <thread>
#include <vector>

namespace {

namespace AudioOut2 = Libs::Audio::AudioOut2;

AudioOut2::AudioOut2UserHandle     g_user_handle = 0;
std::mutex                        g_device_mutex;
std::condition_variable           g_device_cv;
std::vector<int>                  g_live_devices;
std::vector<int>                  g_device_backed_handles;
std::vector<bool>                 g_output_blocking;
std::vector<std::vector<uint8_t>> g_output_pcm;
std::vector<std::array<float, 2>> g_output_gains;
size_t                            g_capture_bytes = 0;
int                               g_next_device  = 1;
int                               g_open_waiters = 0;
bool                              g_block_opens  = false;

void Check(bool value, const char* text) {
	if (!value) {
		std::fprintf(stderr, "AudioOut2PortTests: failed: %s\n", text);
		std::abort();
	}
}

struct PortParam {
	uint16_t port_type;
	uint16_t pad;
	uint32_t data_format;
	uint32_t sampling_freq;
	uint32_t flags;
	uint64_t user_handle;
	uint32_t reserved[10];
};

struct ContextParam {
	uint32_t max_ports;
	uint32_t max_object_ports;
	uint32_t guarantee_object_ports;
	uint32_t queue_depth;
	uint32_t num_grains;
	uint32_t flags;
	uint32_t reserved[10];
};

struct PortState {
	uint16_t output;
	uint8_t  num_channels;
	uint8_t  pad1;
	int16_t  volume;
	uint16_t reroute_counter;
	uint32_t flags;
	uint32_t pad2;
	uint64_t reserved[6];
};

struct Attribute {
	uint32_t    attribute_id;
	int32_t     reserved;
	const void* value;
	size_t      value_size;
};

struct Pcm {
	const void* data;
};

const auto* AsParam(const PortParam* param) {
	return reinterpret_cast<const AudioOut2::AudioOut2PortParam*>(param);
}

const auto* AsParam(const ContextParam* param) {
	return reinterpret_cast<const AudioOut2::AudioOut2ContextParam*>(param);
}

auto* AsState(PortState* state) {
	return reinterpret_cast<AudioOut2::AudioOut2PortState*>(state);
}

const auto* AsAttribute(const Attribute* attribute) {
	return reinterpret_cast<const AudioOut2::AudioOut2Attribute*>(attribute);
}

PortParam MakeParam(uint32_t data_format = 0x200) {
	PortParam param {};
	param.data_format   = data_format;
	param.sampling_freq = 48000;
	param.user_handle   = g_user_handle;
	return param;
}

AudioOut2::AudioOut2ContextHandle CreateContext(uint32_t queue_depth = 4, uint32_t max_object_ports = 0) {
	ContextParam param {};
	param.max_object_ports                    = max_object_ports;
	param.queue_depth                         = queue_depth;
	param.num_grains                          = 512;
	AudioOut2::AudioOut2ContextHandle context = 0;
	Check(AudioOut2::AudioOut2ContextCreate(AsParam(&param), nullptr, 0, &context) == OK,
	      "context create failed");
	return context;
}

AudioOut2::AudioOut2PortHandle CreatePort(AudioOut2::AudioOut2ContextHandle context, const PortParam& param) {
	AudioOut2::AudioOut2PortHandle port = 0;
	Check(AudioOut2::AudioOut2PortCreate(context, AsParam(&param), &port) == OK, "port create failed");
	return port;
}

void TestUserSupportedAttributes() {
	constexpr int invalid_param = static_cast<int32_t>(0x80268001u);
	constexpr int busy          = static_cast<int32_t>(0x80268007u);
	AudioOut2::AudioOut2UserHandle user = 0;
	Check(AudioOut2::AudioOut2UserCreate(1000, nullptr) == invalid_param,
	      "null user handle output was accepted");
	Check(AudioOut2::AudioOut2UserCreate(1000, &user) == OK, "user create failed");
	uint32_t context_attributes = UINT32_MAX;
	uint32_t port_attributes    = UINT32_MAX;
	Check(AudioOut2::AudioOut2UserGetSupportedAttributes(user, &context_attributes,
	                                                    &port_attributes) == OK &&
	          context_attributes == 0 && port_attributes == 0x103,
	      "user capabilities do not match implemented attributes");

	context_attributes = port_attributes = UINT32_MAX;
	Check(AudioOut2::AudioOut2UserGetSupportedAttributes(user, nullptr, &port_attributes) ==
	          invalid_param &&
	          AudioOut2::AudioOut2UserGetSupportedAttributes(user, &context_attributes, nullptr) ==
	              invalid_param &&
	          context_attributes == UINT32_MAX && port_attributes == UINT32_MAX,
	      "null capability outputs were accepted or modified");
	for (const auto invalid: {AudioOut2::AudioOut2UserHandle {0}, UINTPTR_MAX}) {
		Check(AudioOut2::AudioOut2UserGetSupportedAttributes(invalid, &context_attributes,
		                                                    &port_attributes) == invalid_param &&
		          context_attributes == UINT32_MAX && port_attributes == UINT32_MAX,
		      "invalid user capabilities succeeded or modified outputs");
	}

	const auto context = CreateContext();
	auto param = MakeParam();
	param.user_handle = user;
	AudioOut2::AudioOut2PortHandle port = 0;
	Check(AudioOut2::AudioOut2PortCreate(context, AsParam(&param), &port) == OK,
	      "user port create failed");
	Check(AudioOut2::AudioOut2UserDestroy(user) == busy,
	      "user was destroyed while owning a port");
	AudioOut2::AudioOut2ContextDestroy(context);
	Check(AudioOut2::AudioOut2UserDestroy(user) == OK, "unused user destroy failed");
	Check(AudioOut2::AudioOut2UserGetSupportedAttributes(user, &context_attributes,
	                                                    &port_attributes) == invalid_param &&
	          context_attributes == UINT32_MAX && port_attributes == UINT32_MAX &&
	          AudioOut2::AudioOut2UserDestroy(user) == invalid_param,
	      "destroyed user remained valid");

	const auto new_context = CreateContext();
	Check(AudioOut2::AudioOut2PortCreate(new_context, AsParam(&param), &port) == invalid_param,
	      "destroyed user acquired a port");
	AudioOut2::AudioOut2ContextDestroy(new_context);
}

void BlockDeviceOpens() {
	std::lock_guard lock(g_device_mutex);
	g_open_waiters = 0;
	g_block_opens  = true;
}

void WaitForDeviceOpens(int count) {
	std::unique_lock lock(g_device_mutex);
	g_device_cv.wait(lock, [count]() { return g_open_waiters >= count; });
}

void ReleaseDeviceOpens() {
	std::lock_guard lock(g_device_mutex);
	g_block_opens = false;
	g_device_cv.notify_all();
}

int LiveDeviceCount() {
	std::lock_guard lock(g_device_mutex);
	return static_cast<int>(g_live_devices.size());
}

void SetPcm(AudioOut2::AudioOut2PortHandle port, const void* data) {
	const Pcm       pcm {data};
	const Attribute attribute {0, 0, &pcm, sizeof(pcm)};
	Check(AudioOut2::AudioOut2PortSetAttributes(port, AsAttribute(&attribute), 1) == OK,
	      "setting PCM failed");
}

template <typename T>
void SetAttribute(AudioOut2::AudioOut2PortHandle port, uint32_t id, const T& value) {
	const Attribute attribute {id, 0, &value, sizeof(value)};
	Check(AudioOut2::AudioOut2PortSetAttributes(port, AsAttribute(&attribute), 1) == OK,
	      "setting attribute failed");
}

void ResetOutputCalls() {
	std::lock_guard lock(g_device_mutex);
	g_output_blocking.clear();
}

std::vector<bool> OutputCalls() {
	std::lock_guard lock(g_device_mutex);
	return g_output_blocking;
}

void CaptureOutputPcm(size_t bytes) {
	std::lock_guard lock(g_device_mutex);
	g_capture_bytes = bytes;
	g_output_pcm.clear();
}

std::vector<std::vector<uint8_t>> OutputPcm() {
	std::lock_guard lock(g_device_mutex);
	return g_output_pcm;
}

std::vector<std::vector<uint8_t>> PushPcm(AudioOut2::AudioOut2ContextHandle context, size_t bytes) {
	CaptureOutputPcm(bytes);
	Check(AudioOut2::AudioOut2ContextPush(context, 1) == OK, "PCM push failed");
	auto output = OutputPcm();
	CaptureOutputPcm(0);
	return output;
}

void CheckSamples(const std::vector<uint8_t>& pcm, std::initializer_list<float> frame, const char* message) {
	Check(!pcm.empty() && pcm.size() % (frame.size() * sizeof(float)) == 0, "incorrect PCM size");
	for (size_t i = 0; i < pcm.size() / sizeof(float); i++) {
		float sample;
		std::memcpy(&sample, pcm.data() + i * sizeof(float), sizeof(float));
		Check(std::abs(sample - *(frame.begin() + i % frame.size())) < 0.000001f, message);
	}
}

void TestSlotReuse() {
	const auto context = CreateContext();
	const auto param   = MakeParam();
	for (int i = 0; i < 300; i++) {
		AudioOut2::AudioOut2PortHandle port = 0;
		Check(AudioOut2::AudioOut2PortCreate(context, AsParam(&param), &port) == OK,
		      "port slot was not reusable");
		Check(port != 0, "port handle is zero");
		AudioOut2::AudioOut2PortDestroy(port);
	}
	AudioOut2::AudioOut2ContextDestroy(context);
}

void TestFullTableRecovers() {
	const auto                                  context = CreateContext();
	const auto                                  param   = MakeParam();
	std::vector<AudioOut2::AudioOut2PortHandle> ports;
	ports.reserve(256);

	for (int i = 0; i < 256; i++) {
		AudioOut2::AudioOut2PortHandle port = 0;
		Check(AudioOut2::AudioOut2PortCreate(context, AsParam(&param), &port) == OK,
		      "port table filled early");
		ports.push_back(port);
	}

	AudioOut2::AudioOut2PortHandle overflow = 0;
	Check(AudioOut2::AudioOut2PortCreate(context, AsParam(&param), &overflow) != OK,
	      "full port table accepted another port");

	for (auto port: ports) {
		AudioOut2::AudioOut2PortDestroy(port);
	}

	AudioOut2::AudioOut2PortHandle port = 0;
	Check(AudioOut2::AudioOut2PortCreate(context, AsParam(&param), &port) == OK,
	      "port table did not recover");
	AudioOut2::AudioOut2PortDestroy(port);
	AudioOut2::AudioOut2ContextDestroy(context);
}

void TestConcurrentCreates() {
	constexpr int                               thread_count = 8;
	const auto                                  context      = CreateContext();
	const auto                                  param        = MakeParam(0x800);
	std::vector<AudioOut2::AudioOut2PortHandle> ports(thread_count);
	std::vector<int>                            results(thread_count);
	std::vector<std::thread>                    threads;

	BlockDeviceOpens();
	for (int i = 0; i < thread_count; i++) {
		threads.emplace_back([&, i]() {
			results[i] = AudioOut2::AudioOut2PortCreate(context, AsParam(&param), &ports[i]);
		});
	}
	WaitForDeviceOpens(thread_count);
	ReleaseDeviceOpens();
	for (auto& thread: threads) {
		thread.join();
	}

	for (int i = 0; i < thread_count; i++) {
		Check(results[i] == OK, "concurrent port create failed");
		PortState state {};
		AudioOut2::AudioOut2PortGetState(ports[i], AsState(&state));
		Check(state.num_channels == 8, "concurrent create lost its reserved slot");
		AudioOut2::AudioOut2PortDestroy(ports[i]);
	}
	Check(LiveDeviceCount() == 0, "concurrent create leaked a device");
	AudioOut2::AudioOut2ContextDestroy(context);
}

void TestContextDestroyCancelsPendingCreate() {
	const auto                     context = CreateContext();
	const auto                     param   = MakeParam();
	AudioOut2::AudioOut2PortHandle port    = 0;
	int                            result  = OK;

	BlockDeviceOpens();
	std::thread creator(
	    [&]() { result = AudioOut2::AudioOut2PortCreate(context, AsParam(&param), &port); });
	WaitForDeviceOpens(1);
	AudioOut2::AudioOut2ContextDestroy(context);
	ReleaseDeviceOpens();
	creator.join();

	Check(result != OK, "destroyed context retained a pending port create");
	Check(LiveDeviceCount() == 0, "cancelled port create leaked a device");
}

void TestSynchronousDevicePushBypassesModelledQueue() {
	const auto context = CreateContext(1);
	const auto param   = MakeParam();
	AudioOut2::AudioOut2PortHandle port = 0;
	Check(AudioOut2::AudioOut2PortCreate(context, AsParam(&param), &port) == OK,
	      "device port create failed");

	uint32_t pcm[512] {};
	SetPcm(port, pcm);
	ResetOutputCalls();

	Check(AudioOut2::AudioOut2ContextPush(context, 1) == OK, "first sync push failed");
	Check(AudioOut2::AudioOut2ContextPush(context, 1) == OK,
	      "device-paced sync push was blocked by modelled queue");
	const auto calls = OutputCalls();
	Check(calls.size() == 2, "sync pushes did not reach the device backend");
	Check(calls[0] && calls[1], "sync pushes lost their blocking mode");

	AudioOut2::AudioOut2PortDestroy(port);
	AudioOut2::AudioOut2ContextDestroy(context);
}

void TestFloat12ChannelPortOutputsPcm() {
	const auto context = CreateContext();
	const auto param   = MakeParam(0x0c00);
	AudioOut2::AudioOut2PortHandle port = 0;
	Check(AudioOut2::AudioOut2PortCreate(context, AsParam(&param), &port) == OK,
	      "12-channel port create failed");
	Check(LiveDeviceCount() == 1, "12-channel port did not open an audio device");

	PortState state {};
	Check(AudioOut2::AudioOut2PortGetState(port, AsState(&state)) == OK,
	      "12-channel port state query failed");
	Check(state.num_channels == 12, "12-channel port lost its guest channel count");

	float pcm[512 * 12] {};
	SetPcm(port, pcm);
	ResetOutputCalls();
	Check(AudioOut2::AudioOut2ContextPush(context, 1) == OK, "12-channel PCM push failed");
	const auto calls = OutputCalls();
	Check(calls.size() == 1 && calls[0], "12-channel PCM did not reach the device backend");

	AudioOut2::AudioOut2PortDestroy(port);
	Check(LiveDeviceCount() == 0, "12-channel port leaked its audio device");
	AudioOut2::AudioOut2ContextDestroy(context);
}

void TestAsynchronousDevicePushKeepsQueueBounded() {
	const auto context = CreateContext(1);
	const auto param   = MakeParam();
	AudioOut2::AudioOut2PortHandle port = 0;
	Check(AudioOut2::AudioOut2PortCreate(context, AsParam(&param), &port) == OK,
	      "device port create failed");

	uint32_t pcm[512] {};
	SetPcm(port, pcm);
	ResetOutputCalls();

	Check(AudioOut2::AudioOut2ContextPush(context, 0) == OK, "first async push failed");
	Check(AudioOut2::AudioOut2ContextPush(context, 0) != OK,
	      "full async queue accepted another buffer");
	const auto calls = OutputCalls();
	Check(calls.size() == 1 && !calls[0], "rejected async push reached the device backend");

	uint32_t queued    = 0;
	uint32_t available = 0;
	Check(AudioOut2::AudioOut2ContextGetQueueLevel(context, &queued, &available) == OK,
	      "queue-level query failed");
	Check(queued == 1 && available == 0, "async queue level does not match accepted pushes");

	AudioOut2::AudioOut2PortDestroy(port);
	AudioOut2::AudioOut2ContextDestroy(context);
}

void TestHandleWithoutPcmDoesNotBypassQueue() {
	const auto context = CreateContext(1);
	const auto param   = MakeParam();
	AudioOut2::AudioOut2PortHandle port = 0;
	Check(AudioOut2::AudioOut2PortCreate(context, AsParam(&param), &port) == OK,
	      "device port create failed");
	ResetOutputCalls();

	Check(AudioOut2::AudioOut2ContextPush(context, 1) == OK, "empty sync push failed");
	Check(AudioOut2::AudioOut2ContextPush(context, 0) != OK,
	      "handle without PCM bypassed queue backpressure");
	Check(OutputCalls().empty(), "empty push reached the device backend");

	AudioOut2::AudioOut2PortDestroy(port);
	AudioOut2::AudioOut2ContextDestroy(context);
}

void TestPcmCopiedBeforeScratchBufferReuse() {
	const auto context = CreateContext();
	const auto param   = MakeParam();
	AudioOut2::AudioOut2PortHandle first = 0;
	AudioOut2::AudioOut2PortHandle second = 0;
	Check(AudioOut2::AudioOut2PortCreate(context, AsParam(&param), &first) == OK,
	      "first port create failed");
	Check(AudioOut2::AudioOut2PortCreate(context, AsParam(&param), &second) == OK,
	      "second port create failed");

	std::vector<float> scratch(512 * 2);
	std::vector<float> first_pcm(scratch.size(), 0.25f);
	std::vector<float> second_pcm(scratch.size(), -0.5f);
	std::copy(first_pcm.begin(), first_pcm.end(), scratch.begin());
	SetPcm(first, scratch.data());
	std::copy(second_pcm.begin(), second_pcm.end(), scratch.begin());
	SetPcm(second, scratch.data());
	std::fill(scratch.begin(), scratch.end(), 0.0f);

	const auto pcm_bytes = scratch.size() * sizeof(float);
	CaptureOutputPcm(pcm_bytes);
	Check(AudioOut2::AudioOut2ContextPush(context, 1) == OK, "shared-buffer push failed");
	const auto output = OutputPcm();
	Check(output.size() == 2, "shared-buffer push did not output both ports");
	Check(std::memcmp(output[0].data(), first_pcm.data(), pcm_bytes) == 0,
	      "first port lost PCM when scratch buffer was reused");
	Check(std::memcmp(output[1].data(), second_pcm.data(), pcm_bytes) == 0,
	      "second port lost PCM when scratch buffer was reused");

	CaptureOutputPcm(0);
	AudioOut2::AudioOut2PortDestroy(first);
	AudioOut2::AudioOut2PortDestroy(second);
	AudioOut2::AudioOut2ContextDestroy(context);
}

void TestPortGainAndValidation() {
	const auto context = CreateContext();
	const auto port = CreatePort(context, MakeParam());
	std::vector<float> pcm(512 * 2, 0.25f);
	SetPcm(port, pcm.data());
	auto check_output = [&](const std::array<float, 2>& gain, float expected_pcm) {
		CheckSamples(PushPcm(context, pcm.size() * sizeof(float)).at(0), {expected_pcm},
		             "gain modified stored PCM");
		std::lock_guard lock(g_device_mutex);
		Check(g_output_gains.size() == 1 && g_output_gains[0] == gain,
		      "incorrect per-channel gain forwarded to backend");
	};
	check_output({1.0f, 1.0f}, 0.25f);
	for (const std::array<float, 2> gain: {std::array {2.0f, 0.5f}, {0.0f, 0.0f}, {1.0f, 1.0f}}) {
		SetAttribute(port, 1, gain);
		check_output(gain, 0.25f);
		check_output(gain, 0.25f);
	}

	const std::array gain {0.5f, 0.25f};
	SetAttribute(port, 1, gain);
	std::fill(pcm.begin(), pcm.end(), 0.75f);
	SetPcm(port, pcm.data());
	check_output(gain, 0.75f);

	std::fill(pcm.begin(), pcm.end(), -0.25f);
	const Pcm replacement {pcm.data()};
	for (const float invalid: {-1.0f, std::numeric_limits<float>::infinity(),
	                           std::numeric_limits<float>::quiet_NaN()}) {
		const std::array invalid_gain {1.0f, invalid};
		const Attribute attributes[] {{0, 0, &replacement, sizeof(replacement)},
		                              {1, 0, &invalid_gain, sizeof(invalid_gain)}};
		Check(AudioOut2::AudioOut2PortSetAttributes(port, AsAttribute(attributes), 2) ==
		          static_cast<int32_t>(0x80268001u), "invalid gain was accepted");
		check_output(gain, 0.75f);
	}
	for (const Attribute attribute: {Attribute {1, 0, nullptr, sizeof(gain)},
	                                Attribute {1, 0, &gain, sizeof(float)}}) {
		Check(AudioOut2::AudioOut2PortSetAttributes(port, AsAttribute(&attribute), 1) ==
		          static_cast<int32_t>(0x80268001u), "malformed gain was accepted");
	}
	const Attribute duplicates[] {{0, 0, &replacement, sizeof(replacement)},
	                              {1, 0, &gain, sizeof(gain)}, {1, 0, &gain, sizeof(gain)}};
	Check(AudioOut2::AudioOut2PortSetAttributes(port, AsAttribute(duplicates), 3) ==
	          static_cast<int32_t>(0x80268001u), "duplicate gain attributes were accepted");
	check_output(gain, 0.75f);
	AudioOut2::AudioOut2ContextDestroy(context);
}

void TestObjectGainAndAmbisonics() {
	constexpr float half_power = 0.70710678f;
	struct Coefficients { uint32_t channel; float left; float right; };
	constexpr Coefficients cases[] {{UINT32_MAX, half_power, half_power}, {0, 1.0f, 1.0f},
	                               {1, 0.0f, 0.0f}, {2, half_power, -half_power}, {3, 0.0f, 0.0f},
	                               {64, half_power, half_power}, {65, half_power, -half_power},
	                               {66, 0.0f, 0.0f}, {99, 0.0f, 0.0f}};
	for (const auto format: {0x100u, 0x101u}) {
		const auto context = CreateContext(1, 1);
		auto param = MakeParam(format);
		param.port_type = 0x100;
		const auto object = CreatePort(context, param);
		Check(LiveDeviceCount() == 1, "object-only context did not create one output device");
		std::vector<float> floating(512, 0.5f);
		std::vector<int16_t> integer(512, 16384);
		const void* pcm = format == 0x100 ? static_cast<const void*>(floating.data()) : integer.data();
		constexpr auto output_bytes = 512 * 2 * sizeof(float);
		SetPcm(object, pcm);
		std::fill(floating.begin(), floating.end(), 0.0f);
		std::fill(integer.begin(), integer.end(), 0);
		SetAttribute(object, 1, 0.0f);
		SetAttribute(object, 8, 65u);
		for (int push = 0; push < 2; push++) {
			const auto output = PushPcm(context, output_bytes);
			Check(output.size() == 1, "object-only push did not output exactly once");
			CheckSamples(output[0], {0.5f * half_power, 0.5f * half_power},
			             "object PCM ownership, default gain, or attribute freeze failed");
		}
		std::fill(floating.begin(), floating.end(), 0.5f);
		std::fill(integer.begin(), integer.end(), 16384);
		for (const auto& test: cases) {
			Check(AudioOut2::AudioOut2ContextAdvance(context) == OK, "context advance failed");
			const Pcm input {pcm};
			const float gain = 0.5f;
			const Attribute attributes[] {{0, 0, &input, sizeof(input)}, {1, 0, &gain, sizeof(gain)},
			                              {8, 0, &test.channel, sizeof(test.channel)}};
			Check(AudioOut2::AudioOut2PortSetAttributes(object, AsAttribute(attributes), 3) == OK,
			      "object attributes could not be set atomically with PCM first");
			for (int push = 0; push < 2; push++) {
				CheckSamples(PushPcm(context, output_bytes).at(0), {0.25f * test.left, 0.25f * test.right},
				             "object gain or ambisonics decoded incorrectly or accumulated across pushes");
			}
		}
		AudioOut2::AudioOut2ContextAdvance(context);
		SetAttribute(object, 8, UINT32_MAX);
		SetAttribute(object, 1, 0.0f);
		SetPcm(object, pcm);
		CheckSamples(PushPcm(context, output_bytes).at(0), {0.0f}, "zero gain did not mute object");
		AudioOut2::AudioOut2ContextAdvance(context);
		SetPcm(object, pcm);
		CheckSamples(PushPcm(context, output_bytes).at(0), {0.0f}, "object gain did not persist");
		AudioOut2::AudioOut2ContextDestroy(context);
		Check(LiveDeviceCount() == 0, "object context leaked its output device");
	}
}

void TestObjectBusRouting() {
	const auto context = CreateContext(1, 2);
	const auto other_context = CreateContext(1, 1);
	AudioOut2::AudioOut2UserHandle other_user = 0;
	Check(AudioOut2::AudioOut2UserCreate(2000, &other_user) == OK, "second user create failed");
	const auto first = CreatePort(context, MakeParam());
	const auto second = CreatePort(context, MakeParam());
	std::vector<float> bed(512 * 2, 0.125f);
	SetPcm(first, bed.data());
	SetPcm(second, bed.data());
	auto param = MakeParam(0x100);
	param.port_type = 0x100;
	std::vector<float> object_pcm(512, 0.25f);
	SetPcm(CreatePort(context, param), object_pcm.data());
	param.user_handle = other_user;
	SetPcm(CreatePort(context, param), object_pcm.data());
	std::fill(object_pcm.begin(), object_pcm.end(), 1.0f);
	SetPcm(CreatePort(other_context, param), object_pcm.data());
	constexpr auto bytes = 512 * 2 * sizeof(float);
	for (int push = 0; push < 2; push++) {
		const auto output = PushPcm(context, bytes);
		Check(output.size() == 3, "object bus was missing or queued more than once");
		CheckSamples(output[0], {0.125f}, "objects modified first bed");
		CheckSamples(output[1], {0.125f}, "objects modified second bed");
		CheckSamples(output[2], {0.35355339f}, "MAIN objects failed to mix across users or leaked contexts");
	}
	SetPcm(first, nullptr);
	SetPcm(second, nullptr);
	CheckSamples(PushPcm(context, bytes).at(0), {0.35355339f}, "object output depended on bed PCM");
	AudioOut2::AudioOut2ContextDestroy(context);
	Check(LiveDeviceCount() == 1, "context destroy closed another context's object device");
	CheckSamples(PushPcm(other_context, bytes).at(0), {0.70710678f}, "other context lost its object PCM");
	AudioOut2::AudioOut2ContextDestroy(other_context);
	Check(AudioOut2::AudioOut2UserDestroy(other_user) == OK, "second user destroy failed");
	Check(LiveDeviceCount() == 0, "object bus device leaked");
}

} // namespace

namespace Libs::Audio::AudioInternal {

int AudioOutOpen(int type, uint32_t /*samples_num*/, uint32_t /*freq*/, Format /*format*/) {
	std::unique_lock lock(g_device_mutex);
	const int        handle = g_next_device++;
	g_live_devices.push_back(handle);
	if (type != 10) {
		g_device_backed_handles.push_back(handle);
	}
	g_open_waiters++;
	g_device_cv.notify_all();
	g_device_cv.wait(lock, []() { return !g_block_opens; });
	return handle;
}

void AudioOutClose(int handle) {
	std::lock_guard lock(g_device_mutex);
	const auto      it = std::find(g_live_devices.begin(), g_live_devices.end(), handle);
	if (it != g_live_devices.end()) {
		g_live_devices.erase(it);
	}
	const auto device_it =
	    std::find(g_device_backed_handles.begin(), g_device_backed_handles.end(), handle);
	if (device_it != g_device_backed_handles.end()) {
		g_device_backed_handles.erase(device_it);
	}
}

bool AudioOutHasDevice(int handle) {
	std::lock_guard lock(g_device_mutex);
	return std::find(g_device_backed_handles.begin(), g_device_backed_handles.end(), handle) !=
	       g_device_backed_handles.end();
}

uint32_t AudioOutOutputs(const OutputParam* params, uint32_t num, bool blocking) {
	std::lock_guard lock(g_device_mutex);
	g_output_blocking.push_back(blocking);
	g_output_gains.clear();
	if (g_capture_bytes != 0) {
		for (uint32_t i = 0; i < num; i++) {
			const auto* bytes = static_cast<const uint8_t*>(params[i].data);
			g_output_pcm.emplace_back(bytes, bytes + g_capture_bytes);
			g_output_gains.push_back(params[i].gains != nullptr
			                             ? std::array {params[i].gains[0], params[i].gains[1]}
			                             : std::array {1.0f, 1.0f});
		}
	}
	return 0;
}

} // namespace Libs::Audio::AudioInternal

namespace Libs::LibKernel {

uint64_t KYTY_SYSV_ABI KernelGetProcessTime() {
	static std::atomic_uint64_t now {0};
	return now.fetch_add(1000);
}

} // namespace Libs::LibKernel

int main() {
	Check(AudioOut2::AudioOut2UserCreate(1000, &g_user_handle) == OK, "test user create failed");
	TestUserSupportedAttributes();
	TestSlotReuse();
	TestFullTableRecovers();
	TestConcurrentCreates();
	TestContextDestroyCancelsPendingCreate();
	TestSynchronousDevicePushBypassesModelledQueue();
	TestFloat12ChannelPortOutputsPcm();
	TestAsynchronousDevicePushKeepsQueueBounded();
	TestHandleWithoutPcmDoesNotBypassQueue();
	TestPcmCopiedBeforeScratchBufferReuse();
	TestPortGainAndValidation();
	TestObjectGainAndAmbisonics();
	TestObjectBusRouting();
	Check(AudioOut2::AudioOut2UserDestroy(g_user_handle) == OK, "test user destroy failed");
	std::printf("AudioOut2PortTests: all cases passed\n");
	return 0;
}
