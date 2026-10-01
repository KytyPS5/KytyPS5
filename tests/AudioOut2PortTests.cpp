#include "libs/audio.h"
#include "libs/audio_internal.h"
#include "libs/errno.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
size_t                            g_capture_bytes = 0;
int                               g_next_device  = 1;
int                               g_open_waiters = 0;
bool                              g_block_opens  = false;
// Off: the fake devices play everything at once. On: they keep every grain (512 samples at
// 48 kHz) and play nothing, so the queue level grows with each output.
bool                              g_devices_keep_audio = false;
int64_t                           g_device_queued_us   = 0;

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

AudioOut2::AudioOut2ContextHandle CreateContext(uint32_t queue_depth = 4) {
	ContextParam param {};
	param.queue_depth                         = queue_depth;
	param.num_grains                          = 512;
	AudioOut2::AudioOut2ContextHandle context = 0;
	Check(AudioOut2::AudioOut2ContextCreate(AsParam(&param), nullptr, 0, &context) == OK,
	      "context create failed");
	return context;
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
	          context_attributes == 0 && port_attributes == 1,
	      "user capabilities do not match implemented PCM support");

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
	// The context's queue level (what the device still has to play) paces a push to a device, so
	// the backend takes each grain without pacing it again.
	Check(!calls[0] && !calls[1], "device-paced sync pushes were paced again by the backend");

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
	Check(calls.size() == 1 && !calls[0], "12-channel PCM did not reach the device backend");

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
	{
		std::lock_guard lock(g_device_mutex);
		g_devices_keep_audio = true;
		g_device_queued_us   = 0;
	}

	// The level is what the device still has to play beyond a 20 ms cushion: at depth 1 two
	// 512-sample grains (21.3 ms) fill it, and the third async push is refused.
	Check(AudioOut2::AudioOut2ContextPush(context, 0) == OK, "first async push failed");
	Check(AudioOut2::AudioOut2ContextPush(context, 0) == OK, "second async push failed");
	Check(AudioOut2::AudioOut2ContextPush(context, 0) != OK,
	      "full async queue accepted another buffer");
	const auto calls = OutputCalls();
	Check(calls.size() == 2 && !calls[0] && !calls[1],
	      "rejected async push reached the device backend");

	uint32_t queued    = 0;
	uint32_t available = 0;
	Check(AudioOut2::AudioOut2ContextGetQueueLevel(context, &queued, &available) == OK,
	      "queue-level query failed");
	Check(queued == 1 && available == 0, "async queue level does not match accepted pushes");
	{
		std::lock_guard lock(g_device_mutex);
		g_devices_keep_audio = false;
	}

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

int64_t AudioOutQueuedUs(int handle) {
	if (!AudioOutHasDevice(handle)) {
		return -1;
	}
	std::lock_guard lock(g_device_mutex);
	return g_devices_keep_audio ? g_device_queued_us : 0;
}

uint32_t AudioOutOutputs(const OutputParam* params, uint32_t num, bool blocking) {
	std::lock_guard lock(g_device_mutex);
	g_output_blocking.push_back(blocking);
	if (g_devices_keep_audio) {
		g_device_queued_us += 512 * 1000000 / 48000;
	}
	if (g_capture_bytes != 0) {
		for (uint32_t i = 0; i < num; i++) {
			const auto* bytes = static_cast<const uint8_t*>(params[i].data);
			g_output_pcm.emplace_back(bytes, bytes + g_capture_bytes);
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
	Check(AudioOut2::AudioOut2UserDestroy(g_user_handle) == OK, "test user destroy failed");
	std::printf("AudioOut2PortTests: all cases passed\n");
	return 0;
}
