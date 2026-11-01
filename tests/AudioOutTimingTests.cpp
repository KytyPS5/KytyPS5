// Exercise production output scheduling with a deterministic clock and consuming SDL queues.
#include "common/threads.h"
#include "libs/dualSenseHaptics.h"

#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <thread>
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
	std::vector<uint8_t> pcm;
};

uint64_t                             now       = 1000000;
uint32_t                             oversleep = 0, processing = 0;
std::vector<uint32_t>                sleeps;
std::vector<std::unique_ptr<Stream>> streams;
bool                                 fail_open = false, fail_put = false, stalled = false;
bool                                 pad_connected = false, pad_bluetooth = false;
uint64_t                             pad_queue_us = 0;
const float*                         queued_pad_gains = nullptr;
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

	streams.push_back(std::make_unique<Stream>(Stream {*spec, 0, now, {}}));
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

bool PutAudioStreamData(SDL_AudioStream* stream, const void* data, int bytes) {
	now += processing;
	if (fail_put) {
		return false;
	}
	auto& state = GetStream(stream);
	const auto* pcm = static_cast<const uint8_t*>(data);
	state.pcm.assign(pcm, pcm + bytes);
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
float GetSettingScale(Setting) {
	return 1.0f;
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
uint64_t Queue(Stream* stream, int, const void*, uint32_t, uint32_t, bool, const int*, float, const float* gains) {
	queued_pad_gains = gains;
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
		queued_pad_gains = nullptr;
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

void TestZeroOutputFrequency() {
	Fixture f;
	Check(Libs::Audio::AudioOut::AudioOutOpen(0, 0, 0, 256, 0, 4) ==
	          Libs::Audio::AUDIO_OUT_ERROR_INVALID_SAMPLE_FREQ,
	      "public output open accepted zero frequency");
	Check(!f.audio.AudioOutOpen(10, 256, 0, Audio::Format::FloatStereo).IsValid(),
	      "internal output open accepted zero frequency");
	Check(f.Open().ToInt() == 1, "rejected output port occupied a handle");
}

template <typename T>
void CheckOutputSamples(std::initializer_list<T> expected, const char* message) {
	const auto& pcm = streams.back()->pcm;
	Check(pcm.size() == expected.size() * sizeof(T) &&
	          std::memcmp(pcm.data(), expected.begin(), pcm.size()) == 0, message);
}

void TestChannelGains() {
	Fixture f;
	const std::array pcm {0.25f, -0.5f, 0.75f, -1.0f};
	const auto stereo = f.audio.AudioOutOpen(0, 2, 48000, Audio::Format::FloatStereo);
	std::array gains {2.0f, 0.5f};
	Audio::OutputParam output {stereo, pcm.data(), gains.data()};
	for (int push = 0; push < 2; push++) {
		f.audio.AudioOutOutputs(&output, 1, false);
		CheckOutputSamples({0.5f, -0.25f, 1.5f, -0.5f}, "float channel gains were not applied independently");
	}
	const int volume[] {16384, 32768};
	f.audio.AudioOutSetVolume(stereo, 3, volume);
	f.audio.AudioOutOutputs(&output, 1, false);
	CheckOutputSamples({0.25f, -0.25f, 0.75f, -0.5f}, "channel gain did not combine with port volume");
	gains = {0.0f, 0.0f};
	f.audio.AudioOutOutputs(&output, 1, false);
	CheckOutputSamples({0.0f, -0.0f, 0.0f, -0.0f}, "zero channel gains did not mute");
	output.gains = nullptr;
	f.audio.AudioOutOutputs(&output, 1, false);
	CheckOutputSamples({0.125f, -0.5f, 0.375f, -1.0f}, "channel scaling changed original float PCM");

	const std::array<int16_t, 2> integer_pcm {20000, -20000};
	const auto integer = f.audio.AudioOutOpen(0, 1, 48000, Audio::Format::Signed16bitStereo);
	output = {integer, integer_pcm.data(), gains.data()};
	for (const auto gain: {2.0f, std::numeric_limits<float>::max()}) {
		gains = {gain, gain};
		f.audio.AudioOutOutputs(&output, 1, false);
		CheckOutputSamples<int16_t>({32767, -32768}, "integer gain overflowed instead of saturating");
	}
	gains = {0.5f, 0.25f};
	f.audio.AudioOutOutputs(&output, 1, false);
	CheckOutputSamples<int16_t>({10000, -5000}, "integer channel gains changed original PCM");

	const std::array surround_pcm {1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f, 9.0f, 10.0f, 11.0f, 12.0f};
	for (const auto format: {Audio::Format::Float8Ch, Audio::Format::Float12Ch}) {
		const auto surround = f.audio.AudioOutOpen(0, 1, 48000, format);
		output = {surround, surround_pcm.data(), surround_pcm.data()};
		f.audio.AudioOutOutputs(&output, 1, false);
		if (format == Audio::Format::Float8Ch) {
			CheckOutputSamples({1.0f, 4.0f, 9.0f, 16.0f, 49.0f, 64.0f, 25.0f, 36.0f},
			                   "channel reordering used destination channel gains");
		} else {
			CheckOutputSamples({82.0f, 104.0f, 9.0f, 16.0f, 170.0f, 208.0f, 25.0f, 36.0f},
			                   "height downmix lost per-channel gains");
		}
	}
	pad_connected = true;
	const auto speaker = f.Open(2, 4);
	gains = {0.75f, 0.25f};
	output = {speaker, pcm.data(), gains.data()};
	f.audio.AudioOutOutputs(&output, 1, false);
	Check(queued_pad_gains != nullptr && queued_pad_gains[0] == gains[0] && queued_pad_gains[1] == gains[1],
	      "connected controller did not receive channel gains");
}

// How long a helper thread may take to reach the state a test waits for before the test fails.
constexpr std::chrono::seconds HOLD_TEST_LIMIT {10};

// Spins until done() holds or the limit passes, so a broken hold fails instead of hanging.
template <typename Done>
bool WaitFor(Done done) {
	const auto deadline = std::chrono::steady_clock::now() + HOLD_TEST_LIMIT;
	while (!done() && std::chrono::steady_clock::now() < deadline) {
		std::this_thread::yield();
	}
	return done();
}

// Stands in for the GPU command thread during a long pipeline compile, with the public AudioOut
// entry points pointed at the fixture's Audio. The Scope lives on its own thread, as in the
// emulator, because the thread that owns a Scope is never held and so could not test a hold.
class GpuCompile {
public:
	GpuCompile(Audio* audio, bool hold_enabled) {
		Libs::Audio::g_audio = audio;
		Common::GpuStall::Configure(hold_enabled, std::chrono::milliseconds(0));
		m_gpu = std::thread([this]() {
			Common::GpuStall::Scope compile;
			m_started = true;
			while (!m_finish) {
				std::this_thread::yield();
			}
		});
		Check(WaitFor([this]() { return m_started.load(); }), "GPU thread did not start compiling");
	}
	~GpuCompile() {
		Finish();
		Common::GpuStall::Configure(false);
		Libs::Audio::g_audio = nullptr;
	}
	GpuCompile(const GpuCompile&)            = delete;
	GpuCompile& operator=(const GpuCompile&) = delete;

	// Ends the compile, which lets every held submit go. Writes the test made before this are seen
	// by the held submit, because the Scope ends under the GpuStall mutex the submit retakes.
	void Finish() {
		m_finish = true;
		if (m_gpu.joinable()) {
			m_gpu.join();
		}
	}

private:
	std::atomic<bool> m_started {false};
	std::atomic<bool> m_finish {false};
	std::thread       m_gpu;
};

// Runs one guest audio submit on its own thread, as a game's audio thread would. A held submit
// only returns once its GpuCompile finishes, so a test must call Finish before this goes away.
class GuestSubmit {
public:
	template <typename Submit>
	explicit GuestSubmit(Submit submit) {
		m_thread = std::thread([this, submit]() {
			m_result   = submit();
			m_returned = true;
		});
	}
	~GuestSubmit() {
		if (m_thread.joinable()) {
			m_thread.join();
		}
	}
	GuestSubmit(const GuestSubmit&)            = delete;
	GuestSubmit& operator=(const GuestSubmit&) = delete;

	// Waits until the submit is parked in the hold, so the test acts while it is truly held.
	void CheckHeld() const {
		WaitFor([this]() { return m_returned || Common::GpuStall::WaiterCount() != 0; });
		Check(!m_returned && Common::GpuStall::WaiterCount() == 1, "audio output was not held");
	}

	// Waits for the submit to return and gives its result.
	int Result() {
		Check(WaitFor([this]() { return m_returned.load(); }), "audio output never returned");
		m_thread.join();
		return m_result;
	}

private:
	std::atomic<bool> m_returned {false};
	int               m_result = 0;
	std::thread       m_thread;
};

// Output during a long compile reaches the queue only after it ends, and the port then primes
// its queue once without pacing, the same as after any other long gap.
void TestOutputHeldDuringCompile() {
	constexpr uint32_t gap_us       = 1000000;
	constexpr uint64_t block_frames = 256; // The fixture's default port period.
	Fixture            f;
	const auto         port = f.Open(block_frames);
	f.Prime(port);
	GpuCompile  compile(&f.audio, true);
	GuestSubmit submit([&]() {
		return Libs::Audio::AudioOut::AudioOutOutput(port.ToInt(), f.pcm.data());
	});
	submit.CheckHeld();
	Drain(*streams.front());
	const auto queued_before_hold = streams.front()->frames;
	std::this_thread::yield();
	Check(streams.front()->frames == queued_before_hold,
	      "held output reached the queue during the compile");
	now += gap_us; // The game is frozen for the whole compile.
	compile.Finish();
	Check(submit.Result() >= 0, "held output failed after release");
	// The gap drained the queue, so it now holds exactly the one held block.
	Check(streams.front()->frames == block_frames * 1000000,
	      "held output did not reach the queue after release");
	const auto start = now;
	for (int i = 0; i < 7; i++) {
		f.Output(port);
	}
	Check(now == start, "output after a hold did not re-prime its queue unpaced");
	f.Output(port);
	Check(now - start >= 5333 && now - start <= 5334, "output after a hold kept an old deadline");
}

// A null buffer is a submit like any other, so it is held too.
void TestNullOutputHeld() {
	Fixture     f;
	const auto  port = f.Open();
	GpuCompile  compile(&f.audio, true);
	GuestSubmit submit([&]() { return Libs::Audio::AudioOut::AudioOutOutput(port.ToInt(), nullptr); });
	submit.CheckHeld();
	compile.Finish();
	Check(submit.Result() >= 0, "held null output failed after release");
}

// The batch entry point is held the same way, before it looks at any port.
void TestBatchOutputHeld() {
	Fixture     f;
	const auto  port = f.Open();
	GpuCompile  compile(&f.audio, true);
	GuestSubmit submit([&]() {
		Libs::Audio::AudioOut::AudioOutOutputParam param {port.ToInt(), f.pcm.data()};
		return Libs::Audio::AudioOut::AudioOutOutputs(&param, 1);
	});
	submit.CheckHeld();
	compile.Finish();
	Check(submit.Result() >= 0, "held batch output failed after release");
}

// Port queries are not held, and a port closed during the hold makes the held output fail
// cleanly instead of using the closed port.
void TestCloseDuringHold() {
	Fixture     f;
	const auto  port = f.Open();
	GpuCompile  compile(&f.audio, true);
	GuestSubmit submit([&]() {
		return Libs::Audio::AudioOut::AudioOutOutput(port.ToInt(), f.pcm.data());
	});
	submit.CheckHeld();
	Libs::Audio::AudioOut::AudioOutPortState state {};
	Check(Libs::Audio::AudioOut::AudioOutGetPortState(port.ToInt(), &state) == OK,
	      "port state query was held or failed during a hold");
	Check(Libs::Audio::AudioOut::AudioOutClose(port.ToInt()) == OK,
	      "port close failed during a hold");
	compile.Finish();
	Check(submit.Result() == Libs::Audio::AUDIO_OUT_ERROR_INVALID_PORT,
	      "output held across a port close did not report an invalid port");
}

// With the switch off, the default, a long compile changes nothing.
void TestNoHoldWhenDisabled() {
	Fixture     f;
	const auto  port = f.Open();
	GpuCompile  compile(&f.audio, false);
	GuestSubmit submit([&]() {
		return Libs::Audio::AudioOut::AudioOutOutput(port.ToInt(), f.pcm.data());
	});
	Check(submit.Result() >= 0, "output failed with the hold switched off");
	Check(streams.front()->frames != 0, "output did not reach the queue with the hold switched off");
}

// A hold drains the host queue on purpose, so the first submit after it is not an underrun. A
// real underrun on the same thread afterwards still counts, so only that one submit is exempt.
void TestHoldIsNotAnUnderrun() {
	using Common::DebugCounters::Counter;
	constexpr uint32_t gap_us        = 1000000;
	constexpr int      prime_outputs = 8; // Enough to fill the 40 ms cushion again.
	Fixture            f;
	const auto         port = f.Open();
	f.Prime(port);
	Common::DebugCounters::SetEnabled(true);
	const auto  before     = Common::DebugCounters::Get(Counter::AudioUnderruns);
	uint64_t    after_hold = 0;
	uint64_t    after_real = 0;
	GpuCompile  compile(&f.audio, true);
	GuestSubmit submit([&]() {
		const auto output = [&]() {
			return Libs::Audio::AudioOut::AudioOutOutput(port.ToInt(), f.pcm.data());
		};
		const int result = output();
		after_hold       = Common::DebugCounters::Get(Counter::AudioUnderruns);
		for (int i = 0; i < prime_outputs; i++) {
			output();
		}
		now += gap_us; // A real gap with no compile behind it.
		output();
		after_real = Common::DebugCounters::Get(Counter::AudioUnderruns);
		return result;
	});
	submit.CheckHeld();
	now += gap_us; // The game is frozen for the whole compile.
	compile.Finish();
	Check(submit.Result() >= 0, "held output failed after release");
	Common::DebugCounters::SetEnabled(false);
	Check(after_hold == before, "the first output after a hold counted an underrun");
	Check(after_real == before + 1, "a real underrun after a hold was not counted");
}

// A queue that drains with no hold behind it still counts as an underrun.
void TestUnderrunCountedWithoutHold() {
	using Common::DebugCounters::Counter;
	constexpr uint32_t gap_us = 1000000;
	Fixture            f;
	const auto         port = f.Open();
	f.Prime(port);
	Common::DebugCounters::SetEnabled(true);
	const auto before = Common::DebugCounters::Get(Counter::AudioUnderruns);
	now += gap_us;
	f.Output(port);
	const auto after = Common::DebugCounters::Get(Counter::AudioUnderruns);
	Common::DebugCounters::SetEnabled(false);
	Check(after == before + 1, "a drained queue with no hold did not count an underrun");
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
	TestZeroOutputFrequency();
	TestChannelGains();
	TestOutputHeldDuringCompile();
	TestNullOutputHeld();
	TestBatchOutputHeld();
	TestCloseDuringHold();
	TestNoHoldWhenDisabled();
	TestHoldIsNotAnUnderrun();
	TestUnderrunCountedWithoutHold();
	Check(streams.empty(), "output stream leaked");
	std::puts("AudioOutTimingTests: all cases passed");
}
