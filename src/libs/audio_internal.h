#ifndef EMULATOR_INCLUDE_EMULATOR_AUDIO_INTERNAL_H_
#define EMULATOR_INCLUDE_EMULATOR_AUDIO_INTERNAL_H_

#include "common/common.h"
#include "common/emulatorConfig.h"

namespace Libs::Audio {

constexpr int AUDIO_OUT_PORT_TYPE_MAIN      = 0;
constexpr int AUDIO_OUT_PORT_TYPE_BGM       = 1;
constexpr int AUDIO_OUT_PORT_TYPE_VOICE     = 2;
constexpr int AUDIO_OUT_PORT_TYPE_PERSONAL  = 3;
constexpr int AUDIO_OUT_PORT_TYPE_PADSPK    = 4;
constexpr int AUDIO_OUT_PORT_TYPE_VIBRATION = 10;
constexpr int AUDIO_OUT_PORT_TYPE_AUDIO3D   = 126;
constexpr int AUDIO_OUT_PORT_TYPE_AUX       = 127;

// System loudness reads its minimum unless a loudness measurement is started.
constexpr float AUDIO_OUT_STATE_LOUDNESS_MIN = -70.0f;

} // namespace Libs::Audio

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

// Port state output bits, channel count and volume of a route for the configured audio output.
struct PortOutput {
	uint16_t output;
	uint8_t  channels;
	int16_t  volume = -1; // Only pad speaker ports report a volume.
};

inline PortOutput GetMainOutput() {
	switch (Config::GetAudioOutput()) {
		case Config::AudioOutput::AVReceiver: return {1, 8};
		case Config::AudioOutput::SoundBar: return {1, 6};
		case Config::AudioOutput::Headphones: return {0x40, 2};
		default: return {1, 2};
	}
}

inline PortOutput GetPortOutput(int type) {
	const auto main = GetMainOutput();
	switch (type) {
		// Personal ports play only on headphones.
		case AUDIO_OUT_PORT_TYPE_PERSONAL: return main.output == 0x40 ? main : PortOutput {0, 0};
		case AUDIO_OUT_PORT_TYPE_PADSPK: return {4, 1, 127};
		case AUDIO_OUT_PORT_TYPE_VIBRATION: return {4, 2};
		case AUDIO_OUT_PORT_TYPE_AUX: return {1, main.channels};
		default: return main;
	}
}

int      AudioOutOpen(int type, uint32_t samples_num, uint32_t freq, Format format);
void     AudioOutClose(int handle);
bool     AudioOutHasDevice(int handle);
uint32_t AudioOutOutputs(const OutputParam* params, uint32_t num, bool blocking = true);

} // namespace Libs::Audio::AudioInternal

#endif // EMULATOR_INCLUDE_EMULATOR_AUDIO_INTERNAL_H_
