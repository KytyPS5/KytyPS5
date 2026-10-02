// Regression tests for the NGS2 voice-control rack-type gate (ngs2.cpp, case 0x1000/0x4001).
//
// Background: Atomfall (Asura engine) owns only Mastering and CustomSubmixer racks and
// sends the sampler waveform-setup control 0x10000000 to a CustomSubmixer-rack voice.
// The gate previously required an exact rack-type match and EXITed, killing the
// emulator right after the game's intro video. SetupSampler and the block path touch
// only voice state, so every known rack type is now allowed through the gate; unknown
// future rack types still fail as a genuine "not implemented".
#include "libs/ngs2.cpp"

#include <cstdio>
#include <cstdlib>

using namespace Libs::Audio::Ngs2;

namespace {

void Check(bool condition, const char* message) {
	if (!condition) {
		std::fprintf(stderr, "Ngs2RackControlTests: %s\n", message);
		std::abort();
	}
}

// Sends the sampler waveform-setup control (0x10000000) to a voice whose rack has the
// given type — the exact pattern that previously exited for non-sampler racks.
void TestWaveformSetupOnEveryRackType() {
	for (auto type: {Ngs2RackType::Sampler, Ngs2RackType::Submixer, Ngs2RackType::Mastering,
	                 Ngs2RackType::Reverb, Ngs2RackType::CustomSubmixer,
	                 Ngs2RackType::CustomMastering, Ngs2RackType::CustomSampler}) {
		Ngs2Internal      system;
		Ngs2RackInternal  rack {};
		Ngs2VoiceInternal voice;
		system.option.sample_rate = 48000;
		rack.ngs                  = &system;
		rack.type                 = type;
		voice.rack                = &rack;
		struct Setup {
			Ngs2VoiceParamHeader header {40, 0, 0x10000000};
			Ngs2WaveformFormat   format;
			uint32_t             flags = 0, reserved = 0;
		} setup;
		setup.format = {0x12, 1, 48000};
		Check(Ngs2VoiceControl(reinterpret_cast<uintptr_t>(&voice), &setup.header) == OK,
		      "waveform setup control must be accepted on every known rack type");
	}
}

// The exact Atomfall sequence: CustomSubmixer rack, sampler setup control, then a
// non-repeating PCM block. Blocks with num_repeats == 0 are rack-independent; looping
// blocks (num_repeats != 0) intentionally remain sampler-rack-only.
void TestAtomfallCustomSubmixerSequence() {
	Ngs2Internal      system;
	Ngs2RackInternal  rack {};
	Ngs2VoiceInternal voice;
	system.option.sample_rate = 48000;
	rack.ngs                  = &system;
	rack.type                 = Ngs2RackType::CustomSubmixer;
	voice.rack                = &rack;

	struct Setup {
		Ngs2VoiceParamHeader header {40, 0, 0x10000000};
		Ngs2WaveformFormat   format;
		uint32_t             flags = 0, reserved = 0;
	} setup;
	setup.format = {0x12, 1, 48000};
	Check(Ngs2VoiceControl(reinterpret_cast<uintptr_t>(&voice), &setup.header) == OK,
	      "Atomfall waveform setup on a CustomSubmixer rack failed");

	const int16_t pcm[4] = {100, -100, 200, -200};
	Ngs2WaveformBlock block {0, sizeof(pcm), 0, 0, 4, 0, 123};
	struct Blocks {
		Ngs2VoiceParamHeader     header {32, 0, 0x10000001};
		const void*              data;
		uint32_t                 flags, count;
		const Ngs2WaveformBlock* blocks;
	} blocks {{32, 0, 0x10000001}, pcm, 0, 1, &block};
	Check(Ngs2VoiceControl(reinterpret_cast<uintptr_t>(&voice), &blocks.header) == OK,
	      "non-repeating block queue on a CustomSubmixer rack failed");
	Check(voice.blocks.size() == 1 && voice.channels == 1 &&
	          voice.sample_rate == 48000,
	      "voice setup state does not reflect the accepted controls");
}

// The gate loosening must not change sampler-rack behavior: the classic sampler setup
// still configures the voice exactly as before.
void TestSamplerRackUnchanged() {
	Ngs2Internal      system;
	Ngs2RackInternal  rack {};
	Ngs2VoiceInternal voice;
	system.option.sample_rate = 48000;
	rack.ngs                  = &system;
	rack.type                 = Ngs2RackType::Sampler;
	voice.rack                = &rack;
	struct Setup {
		Ngs2VoiceParamHeader header {40, 0, 0x10000000};
		Ngs2WaveformFormat   format;
		uint32_t             flags = 0, reserved = 0;
	} setup;
	setup.format = {0x12, 2, 44100};
	Check(Ngs2VoiceControl(reinterpret_cast<uintptr_t>(&voice), &setup.header) == OK,
	      "sampler rack setup regressed");
	Check(voice.channels == 2 && voice.sample_rate == 44100,
	      "sampler rack voice state differs after the gate change");
}

// Documented current behavior (unchanged by the gate fix): rendering a non-sampler
// voice neither consumes samples nor marks the voice empty. The render path for
// submixer-rack sources is a separate, still-open work item.
void TestCustomSubmixerRenderIsNeutral() {
	Ngs2Internal      system;
	Ngs2RackInternal  rack {};
	Ngs2VoiceInternal voice;
	system.option.sample_rate = 48000;
	rack.ngs                  = &system;
	rack.type                 = Ngs2RackType::CustomSubmixer;
	voice.rack                = &rack;
	struct Setup {
		Ngs2VoiceParamHeader header {40, 0, 0x10000000};
		Ngs2WaveformFormat   format;
		uint32_t             flags = 0, reserved = 0;
	} setup;
	setup.format = {0x12, 1, 48000};
	Check(Ngs2VoiceControl(reinterpret_cast<uintptr_t>(&voice), &setup.header) == OK,
	      "setup failed");
	voice.state = Ngs2VoicePlayState::Playing;
	Ngs2RenderVoice(voice, {}, 160);
	Check(voice.state == Ngs2VoicePlayState::Playing,
	      "non-sampler voice must not be retired by rendering");
	for (float sample: voice.samples) {
		Check(sample == 0.0f, "non-sampler voice unexpectedly produced samples");
	}
}

} // namespace

int main() {
	TestWaveformSetupOnEveryRackType();
	TestAtomfallCustomSubmixerSequence();
	TestSamplerRackUnchanged();
	TestCustomSubmixerRenderIsNeutral();
	std::printf("Ngs2RackControlTests: all passed\n");
	return 0;
}
