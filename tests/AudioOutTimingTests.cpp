// Exercise production output scheduling with a deterministic clock and consuming SDL queues.
#include "common/threads.h"
#include "libs/dualSenseHaptics.h"

#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>

namespace {
void Check(bool condition, const char* message) {
	if (!condition) {
		std::fprintf(stderr, "AudioOutTimingTests: %s\n", message);
		std::abort();
	}
}

struct Stream {
	SDL_AudioSpec spec;
	uint64_t      frames = 0; // Millionths of a frame retain exact simulated consumption.
	uint64_t      updated;
};

uint64_t                             now       = 1000000;
uint32_t                             oversleep = 0, processing = 0;
std::vector<uint32_t>                sleeps;
std::vector<std::unique_ptr<Stream>> streams;
bool                                 fail_open = false, fail_put = false, stalled = false;
bool                                 pad_connected = false, pad_bluetooth = false;
uint64_t                             pad_queue_us = 0;
int                                  clears = 0;

Stream& GetStream(SDL_AudioStream* stream) {
	return *reinterpret_cast<Stream*>(stream);
}

void Drain(Stream& stream) {
	if (!stalled) {
		const auto consumed = (now - stream.updated) * stream.spec.freq;
		stream.frames -= std::min(stream.frames, consumed);
	}
	stream.updated = now;
}
} // namespace

namespace Fake {
bool InitSubSystem(SDL_InitFlags) {
	return true;
}
void QuitSubSystem(SDL_InitFlags) {}
bool ResumeAudioStreamDevice(SDL_AudioStream*) {
	return true;
}

SDL_AudioStream* OpenAudioDeviceStream(SDL_AudioDeviceID, const SDL_AudioSpec* spec,
                                       SDL_AudioStreamCallback, void*) {
	if (fail_open) {
		return nullptr;
	}

	streams.push_back(std::make_unique<Stream>(Stream {*spec, 0, now}));
	return reinterpret_cast<SDL_AudioStream*>(streams.back().get());
}

void DestroyAudioStream(SDL_AudioStream* stream) {
	const auto it = std::find_if(streams.begin(), streams.end(), [stream](const auto& value) {
		return value.get() == &GetStream(stream);
	});
	Check(it != streams.end(), "unknown stream destroyed");
	streams.erase(it);
}

int GetAudioStreamQueued(SDL_AudioStream* stream) {
	auto& state = GetStream(stream);
	Drain(state);
	return static_cast<int>(state.frames / 1000000 * SDL_AUDIO_BYTESIZE(state.spec.format) *
	                        state.spec.channels);
}

bool PutAudioStreamData(SDL_AudioStream* stream, const void*, int bytes) {
	now += processing;
	if (fail_put) {
		return false;
	}
	auto& state = GetStream(stream);
	Drain(state);
	state.frames += static_cast<uint64_t>(bytes) * 1000000 /
	                (SDL_AUDIO_BYTESIZE(state.spec.format) * state.spec.channels);
	return true;
}

bool ClearAudioStream(SDL_AudioStream* stream) {
	auto& state   = GetStream(stream);
	state.frames  = 0;
	state.updated = now;
	clears++;
	return true;
}
} // namespace Fake

namespace Common {
class TimingThread: public Thread {
public:
	static void SleepMicro(uint32_t micros) {
		sleeps.push_back(micros);
		now += micros + oversleep;
	}
};
} // namespace Common

#define Thread                      TimingThread
#define SDL_InitSubSystem           Fake::InitSubSystem
#define SDL_QuitSubSystem           Fake::QuitSubSystem
#define SDL_OpenAudioDeviceStream   Fake::OpenAudioDeviceStream
#define SDL_ResumeAudioStreamDevice Fake::ResumeAudioStreamDevice
#define SDL_DestroyAudioStream      Fake::DestroyAudioStream
#define SDL_GetAudioStreamQueued    Fake::GetAudioStreamQueued
#define SDL_PutAudioStreamData      Fake::PutAudioStreamData
#define SDL_ClearAudioStream        Fake::ClearAudioStream
#include "libs/audio.cpp"
#undef Thread
#undef SDL_InitSubSystem
#undef SDL_QuitSubSystem
#undef SDL_OpenAudioDeviceStream
#undef SDL_ResumeAudioStreamDevice
#undef SDL_DestroyAudioStream
#undef SDL_GetAudioStreamQueued
#undef SDL_PutAudioStreamData
#undef SDL_ClearAudioStream

namespace Libs::Controller {
int GetActiveControllerId() {
	return 0;
}

namespace DualSenseHaptics {
Stream* Open(uint32_t, bool) {
	static int pad_stream;
	return pad_connected ? reinterpret_cast<Stream*>(&pad_stream) : nullptr;
}
void     Close(Stream*) {}
bool UsesBluetooth(const Stream* stream) {
	return stream != nullptr && pad_bluetooth;
}
uint64_t Queue(Stream* stream, int, const void*, uint32_t, uint32_t, bool, const int*) {
	return stream != nullptr ? pad_queue_us : 0;
}
} // namespace DualSenseHaptics
} // namespace Libs::Controller

namespace Libs::LibKernel {
uint64_t KYTY_SYSV_ABI KernelGetProcessTime() {
	return now;
}
} // namespace Libs::LibKernel

namespace Loader::Timer {
double GetTimeMs() {
	return static_cast<double>(now) / 1000;
}
} // namespace Loader::Timer

namespace {
using Audio = Libs::Audio::Audio;

struct Fixture {
	Audio                   audio;
	std::array<float, 2048> pcm {};

	Fixture() {
		Check(streams.empty(), "stream leaked between tests");
		now        = 1000000;
		processing = oversleep = 0;
		clears                 = 0;
		fail_open = fail_put = stalled = false;
		pad_connected = pad_bluetooth = false;
		pad_queue_us = 0;
		sleeps.clear();
	}

	Audio::Id Open(uint32_t frames = 256, int type = 0) {
		const auto id = audio.AudioOutOpen(type, frames, 48000, Audio::Format::FloatStereo);
		Check(id.IsValid(), "port open failed");
		return id;
	}

	void Output(Audio::Id port, bool blocking = true) {
		Audio::OutputParam param {port, pcm.data()};
		audio.AudioOutOutputs(&param, 1, blocking);
	}

	void Prime(Audio::Id port) {
		const auto start = now;
		for (int i = 0; i < 8; i++) {
			Output(port);
		}
		Check(now == start, "priming was paced before the 40 ms cushion filled");
		Output(port);
		Check(now - start >= 5333 && now - start <= 5334,
		      "priming accumulated a multi-block deadline");
		Drain(*streams.front());
		Check(streams.front()->frames >= uint64_t {7 * 256} * 1000000,
		      "first paced output consumed the priming cushion");
	}
};

void TestPrimingAndSteadyCadence() {
	Fixture    f;
	const auto port = f.Open();
	f.Prime(port);
	processing = 125;
	oversleep  = 175;
	for (int i = 0; i < 10; i++) {
		f.Output(port);
	}
	const auto start = now;
	for (int i = 0; i < 3000; i++) {
		f.Output(port);
	}
	Check(now - start >= 15999999 && now - start <= 16000001,
	      "processing, sleep overshoot, or fractional periods accumulated timing drift");
}

void TestUnderrunAndStalledQueueRecovery() {
	Fixture    f;
	const auto port = f.Open();
	f.Prime(port);
	now += 1000000;
	f.Prime(port);

	// A stopped device must clear its stale queue and restart priming, not catch up old deadlines.
	stalled = true;
	for (int i = 0; i < 3 && clears == 0; i++) {
		f.Output(port);
	}
	Check(clears == 1, "stalled playback queue was not cleared");
	stalled          = false;
	const auto start = now;
	for (int i = 0; i < 7; i++) {
		f.Output(port);
	}
	Check(now == start, "queue clear did not restart unpaced priming");
	f.Output(port);
	Check(now - start >= 5333 && now - start <= 5334,
	      "queue clear retained an old pacing deadline");
}

void TestFallbackClockAndLongGap() {
	Fixture f;
	fail_open       = true;
	const auto port = f.Open();
	f.Output(port);
	const auto start = now;
	oversleep        = 175;
	for (int i = 0; i < 3000; i++) {
		now += 125;
		f.Output(port);
	}
	Check(now - start >= 16000174 && now - start <= 16000176,
	      "device-free output accumulated timing drift");
	now += 1000000;
	f.Output(port);
	const auto resumed = now;
	f.Output(port);
	Check(now - resumed >= 5333 && now - resumed <= 5509,
	      "long interruption caused a burst of device-free catch-up submissions");
}

void TestAsyncDoesNotAccumulateDeadlines() {
	Fixture    f;
	const auto port  = f.Open();
	const auto start = now;
	for (int i = 0; i < 8; i++) {
		f.Output(port, false);
	}
	Check(now == start, "asynchronous output slept");
	f.Output(port);
	Check(now - start <= 5334, "asynchronous submissions accumulated a blocking deadline");
	for (int i = 0; i < 3; i++) {
		f.Output(port);
	}
	now += 1000000;
	f.Output(port, false);
	const auto resumed = now;
	for (int i = 0; i < 7; i++) {
		f.Output(port);
	}
	Check(now == resumed, "async-to-blocking transition did not reset an emptied queue");
}

void TestSynchronizedBatchAndInactivePorts() {
	Fixture            f;
	const auto         vibration  = f.Open(256, 10);
	const auto         main       = f.Open();
	const auto         background = f.Open();
	Audio::OutputParam batch[] {
	    {vibration, f.pcm.data()}, {main, f.pcm.data()}, {background, f.pcm.data()}};
	const auto start = now;
	for (int i = 0; i < 8; i++) {
		f.audio.AudioOutOutputs(batch, 3);
	}
	Check(now == start, "vibration fallback slowed a device-backed batch's priming");
	for (int i = 0; i < 3; i++) {
		sleeps.clear();
		f.audio.AudioOutOutputs(batch, 3);
		Check(sleeps.size() == 1 && sleeps.front() >= 5333 && sleeps.front() <= 5334,
		      "synchronized streams accumulated separate pacing waits");
	}

	Audio::OutputParam inactive[] {{main, nullptr}, {vibration, f.pcm.data()}};
	now += 1000000;
	f.audio.AudioOutOutputs(inactive, 2);
	const auto resumed = now;
	f.audio.AudioOutOutputs(inactive, 2);
	Check(now - resumed >= 5333 && now - resumed <= 5334,
	      "a device without PCM disabled fallback pacing");
}

void TestFallbackUsesEachPortsPeriod() {
	Fixture f;
	fail_open                     = true;
	const auto         short_port = f.Open(128);
	const auto         long_port  = f.Open(256);
	Audio::OutputParam batch[] {{short_port, f.pcm.data()}, {long_port, f.pcm.data()}};
	f.audio.AudioOutOutputs(batch, 2);
	const auto start = now;
	f.audio.AudioOutOutputs(batch, 2);
	Check(now - start >= 5333 && now - start <= 5334,
	      "fallback batch used the first port's period for every port");
}

void TestFailedQueueUsesFallbackClock() {
	Fixture    f;
	const auto port = f.Open();
	fail_put        = true;
	f.Output(port);
	const auto start = now;
	for (int i = 0; i < 3; i++) {
		f.Output(port);
	}
	Check(now - start == 16000, "repeated queue failures reset fallback pacing");
}

void TestControllerSpeakerPacing() {
	Fixture f;
	fail_open     = true; // No PC audio device paces the pad speaker.
	pad_connected = true;
	pad_queue_us  = 80000;
	const auto speaker = f.Open(256, 4);

	pad_bluetooth = true;
	f.Output(speaker);
	const auto bluetooth_start = now;
	f.Output(speaker);
	Check(now - bluetooth_start >= 5333 && now - bluetooth_start <= 5334,
	      "Bluetooth speaker waited on its HID queue instead of the sample clock");

	pad_bluetooth = false;
	const auto usb_start = now;
	f.Output(speaker);
	Check(now - usb_start >= 5333 && now - usb_start <= 5334,
	      "stalled USB speaker waited longer than one audio block");
}

void TestInvalidBatchSize() {
	Libs::Audio::AudioOut::AudioOutOutputParam param {};
	for (const auto count: {0u, 33u}) {
		Check(Libs::Audio::AudioOut::AudioOutOutputs(&param, count) ==
		          Libs::Audio::AUDIO_OUT_ERROR_INVALID_SIZE,
		      "invalid batch size was not rejected before accessing ports");
	}
}
} // namespace

int main() {
	TestPrimingAndSteadyCadence();
	TestUnderrunAndStalledQueueRecovery();
	TestFallbackClockAndLongGap();
	TestAsyncDoesNotAccumulateDeadlines();
	TestSynchronizedBatchAndInactivePorts();
	TestFallbackUsesEachPortsPeriod();
	TestFailedQueueUsesFallbackClock();
	TestControllerSpeakerPacing();
	TestInvalidBatchSize();
	Check(streams.empty(), "output stream leaked");
	std::puts("AudioOutTimingTests: all cases passed");
}
