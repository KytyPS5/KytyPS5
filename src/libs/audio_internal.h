#ifndef EMULATOR_INCLUDE_EMULATOR_AUDIO_INTERNAL_H_
#define EMULATOR_INCLUDE_EMULATOR_AUDIO_INTERNAL_H_

#include "common/common.h"
#include "common/emulatorConfig.h"

namespace Libs::Audio::AudioInternal {

enum class Format {
	Unknown,
	Signed16bitMono,
	Signed16bitStereo,
	Signed16bit8Ch,
	FloatMono,
	FloatStereo,
	Float8Ch,
	Signed16bit8ChStd,
	Float8ChStd,
	Float12Ch,
};

struct OutputParam {
	int          handle = 0;
	const void*  data   = nullptr;
	const float* gains  = nullptr;
};

static constexpr int OUT_PORTS_MAX = 32;

// Port state output bits and channel count of the main route for the configured audio output.
struct MainOutput {
	uint16_t output;
	uint8_t  channels;
};

inline MainOutput GetMainOutput() {
	switch (Config::GetAudioOutput()) {
		case Config::AudioOutput::AVReceiver: return {1, 8};
		case Config::AudioOutput::SoundBar: return {1, 6};
		case Config::AudioOutput::Headphones: return {0x40, 2};
		default: return {1, 2};
	}
}

int      AudioOutOpen(int type, uint32_t samples_num, uint32_t freq, Format format);
void     AudioOutClose(int handle);
bool     AudioOutHasDevice(int handle);
uint32_t AudioOutOutputs(const OutputParam* params, uint32_t num, bool blocking = true);

} // namespace Libs::Audio::AudioInternal

#endif // EMULATOR_INCLUDE_EMULATOR_AUDIO_INTERNAL_H_
