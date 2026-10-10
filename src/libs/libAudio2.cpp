#include "common/assert.h"
#include "common/common.h"
#include "common/logging/log.h"
#include "common/threads.h"
#include "kernel/pthread.h"
#include "libs/audio.h"
#include "libs/audio_internal.h"
#include "libs/errno.h"
#include "libs/libs.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <shared_mutex>
#include <vector>

namespace Libs::Audio {

namespace {

constexpr int AUDIO_OUT_PORT_TYPE_MAIN      = 0;
constexpr int AUDIO_OUT_PORT_TYPE_BGM       = 1;
constexpr int AUDIO_OUT_PORT_TYPE_VOICE     = 2;
constexpr int AUDIO_OUT_PORT_TYPE_PERSONAL  = 3;
constexpr int AUDIO_OUT_PORT_TYPE_PADSPK    = 4;
constexpr int AUDIO_OUT_PORT_TYPE_VIBRATION = 10;
constexpr int AUDIO_OUT_PORT_TYPE_AUX       = 127;

} // namespace

namespace AudioOut2 {

LIB_NAME("AudioOut2", "AudioOut");

struct AudioOut2ContextParam {
	uint32_t max_ports;
	uint32_t max_object_ports;
	uint32_t guarantee_object_ports;
	uint32_t queue_depth;
	uint32_t num_grains;
	uint32_t flags;
	uint32_t reserved[10];
};

struct AudioOut2PortParam {
	uint16_t            port_type;
	uint16_t            pad;
	uint32_t            data_format;
	uint32_t            sampling_freq;
	uint32_t            flags;
	AudioOut2UserHandle user_handle;
	uint32_t            reserved[10];
};

struct AudioOut2Attribute {
	uint32_t    attribute_id;
	int32_t     reserved;
	const void* value;
	size_t      value_size;
};

struct AudioOut2Pcm {
	const void* data;
};

struct AudioOut2Position {
	float x;
	float y;
	float z;
};

struct AudioOut2PortState {
	uint16_t output;
	uint8_t  num_channels;
	uint8_t  pad1;
	int16_t  volume;
	uint16_t reroute_counter;
	uint32_t flags;
	uint32_t pad2;
	uint64_t reserved[6];
};

struct AudioOut2SystemState {
	float    loudness;
	uint32_t pad;
	uint64_t reserved[7];
};

struct AudioOut2SpeakerAngle {
	int16_t azimuth;
	int16_t elevation;
};

struct AudioOut2SpeakerInfo {
	uint8_t               type;
	uint8_t               pad1;
	int16_t               pad2;
	uint32_t              available_bits;
	uint32_t              flags;
	uint32_t              pad3;
	AudioOut2SpeakerAngle speaker_angle[16];
};

struct AudioOut2SystemDebugStateParam {
	uint32_t debug_state_id;
	int32_t  reserved;
	void*    param;
	size_t   param_size;
};

struct AudioOut2MasteringParamsHeader {
	uint32_t params_id;
};

struct AudioOut2MasteringStatesHeader {
	uint32_t states_id;
};

struct AudioOut2MasteringStatesDescriptor {
	uint32_t id;
	uint32_t size;
};

static constexpr uint32_t AUDIO_OUT2_MASTERING_MAX_SYSTEM_CHANNELS = 8;
static constexpr uint32_t AUDIO_OUT2_MASTERING_COMPRESSOR_BANDS    = 3;
static constexpr uint32_t AUDIO_OUT2_MASTERING_COMPRESSOR_BANDS_V2 = 4;

struct AudioOut2MasteringCompressorStates {
	AudioOut2MasteringStatesDescriptor descriptor;
	uint32_t                           reserved[2];
	float                              input_rms[AUDIO_OUT2_MASTERING_COMPRESSOR_BANDS]
	                                            [AUDIO_OUT2_MASTERING_MAX_SYSTEM_CHANNELS];
	float                              compression_coeff[AUDIO_OUT2_MASTERING_COMPRESSOR_BANDS]
	                                                    [AUDIO_OUT2_MASTERING_MAX_SYSTEM_CHANNELS];
};

struct AudioOut2MasteringCompressorStatesV2 {
	AudioOut2MasteringStatesDescriptor descriptor;
	uint32_t                           reserved[2];
	float                              input_rms[AUDIO_OUT2_MASTERING_COMPRESSOR_BANDS_V2]
	                                            [AUDIO_OUT2_MASTERING_MAX_SYSTEM_CHANNELS];
	float                              compression_coeff[AUDIO_OUT2_MASTERING_COMPRESSOR_BANDS_V2]
	                                                    [AUDIO_OUT2_MASTERING_MAX_SYSTEM_CHANNELS];
};

struct AudioOut2MasteringLimiterStates {
	AudioOut2MasteringStatesDescriptor descriptor;
	uint32_t                           reserved[2];
	float                              input_peak[AUDIO_OUT2_MASTERING_MAX_SYSTEM_CHANNELS];
	float                              output_peak[AUDIO_OUT2_MASTERING_MAX_SYSTEM_CHANNELS];
	float                              gain_peak[AUDIO_OUT2_MASTERING_MAX_SYSTEM_CHANNELS];
};

struct AudioOut2MasteringStates {
	AudioOut2MasteringStatesHeader     states_header;
	uint32_t                           reserved[3];
	AudioOut2MasteringCompressorStates compressor_states;
	AudioOut2MasteringLimiterStates    limiter_states;
};

struct AudioOut2MasteringStatesV2 {
	AudioOut2MasteringStatesHeader       states_header;
	uint32_t                             reserved[3];
	AudioOut2MasteringCompressorStatesV2 compressor_states;
	AudioOut2MasteringLimiterStates      limiter_states;
};

static std::atomic_uint64_t g_audioout2_next_context {1};
static std::atomic_uint64_t g_audioout2_next_port {1};
static AudioOut2UserHandle  g_audioout2_next_user = 1;

struct AudioOut2GrainPort {
	AudioOut2PortHandle   port         = 0;
	int                   audio_handle = 0;
	std::vector<uint8_t>  pcm;
	std::array<float, 16> gains {};
};

using AudioOut2Grain = std::vector<AudioOut2GrainPort>;

struct AudioOut2ContextState {
	bool                   used             = false;
	AudioOut2ContextHandle handle           = 0;
	uint32_t               queue_depth      = 4;
	uint32_t               queued           = 0;
	uint32_t               num_grains       = 512;
	uint64_t               last_update      = 0;
	uint64_t               last_update_frac = 0; // in 1/48000 us, so grains need not be whole us
	bool                   idle             = true;
	std::deque<AudioOut2Grain> advanced;
	bool                       uses_advance        = false;
	int                        object_audio_handle = 0;
};

struct AudioOut2PortStateEntry {
	bool                   used          = false;
	AudioOut2PortHandle    handle        = 0;
	AudioOut2ContextHandle context       = 0;
	AudioOut2UserHandle    user          = 0;
	uint16_t               port_type     = 0;
	uint32_t               data_format   = 0;
	uint32_t               sampling_freq = 48000;
	uint32_t               samples_num   = 512;
	AudioInternal::Format  audio_format  = AudioInternal::Format::Unknown;
	int                    audio_handle  = 0;
	std::array<float, 16>  gains {};
	uint32_t               ambisonics        = UINT32_MAX;
	bool                   attributes_locked = false;
	bool                   pcm_fresh         = false;
	std::vector<uint8_t>   pcm_data;
};

struct AudioOut2SpeakerArrayState {
	bool                        used          = false;
	uint32_t                    num_speakers  = 0;
	uint8_t                     is_3d         = 0;
	uint8_t                     is_ambisonics = 0;
	AudioOut2SpeakerArrayHandle handle        = nullptr;
};

struct AudioOut2LatencyState {
	bool     used       = false;
	uint32_t user_id    = 0;
	uint32_t output     = 0;
	uint32_t latency_us = 0;
};

static Common::Mutex                              g_audioout2_context_mutex;
static std::array<AudioOut2ContextState, 16>      g_audioout2_contexts;
static Common::Mutex                              g_audioout2_port_mutex;
static std::array<AudioOut2PortStateEntry, 256>   g_audioout2_ports;
static std::shared_mutex                          g_audioout2_output_mutex;
static std::vector<AudioOut2UserHandle>           g_audioout2_users;
static Common::Mutex                              g_audioout2_speaker_array_mutex;
static std::array<AudioOut2SpeakerArrayState, 32> g_audioout2_speaker_arrays;
static Common::Mutex                              g_audioout2_latency_mutex;
static std::array<AudioOut2LatencyState, 16>      g_audioout2_latencies;

static constexpr int AUDIO_OUT2_ERROR_NOT_READY                   = -2144960504; /* 0x80268008 */
static constexpr int AUDIO_OUT2_ERROR_PORT_FULL                   = -2144960494; /* 0x80268012 */
static constexpr int AUDIO_OUT2_ERROR_INVALID_PARAM               = -2144960511; /* 0x80268001 */
static constexpr int AUDIO_OUT2_ERROR_BUSY                        = -2144960505; /* 0x80268007 */
static constexpr int AUDIO_OUT2_ERROR_MASTERING_INVALID_API_PARAM = -2144959999; /* 0x80268201 */
static constexpr int AUDIO_OUT2_ERROR_MASTERING_INVALID_STATES_ID = -2144959996; /* 0x80268204 */
static constexpr int AUDIO_OUT2_ERROR_OUT_OF_RESOURCE             = -2144960510; /* 0x80268002 */
static constexpr int AUDIO_OUT2_ERROR_INVALID_PORT                = -2144960503; /* 0x80268009 */
static constexpr uint32_t AUDIO_OUT2_PORT_ATTRIBUTE_ID_PCM        = 0;
static constexpr uint32_t AUDIO_OUT2_PORT_ATTRIBUTE_ID_GAIN       = 1;
static constexpr uint32_t AUDIO_OUT2_PORT_ATTRIBUTE_ID_AMBISONICS = 8;
static constexpr uint32_t AUDIO_OUT2_MASTERING_OUTPUT_RECORDING   = 2;
static constexpr uint32_t AUDIO_OUT2_MASTERING_STATES_ID_DEFAULT  = 1;
static constexpr uint32_t AUDIO_OUT2_MASTERING_STATES_ID_V2       = 2;
static constexpr uint32_t AUDIO_OUT2_MASTERING_STATES_STRUCT_ID_COMPRESSOR_DEFAULT = 0x01010001;
static constexpr uint32_t AUDIO_OUT2_MASTERING_STATES_STRUCT_ID_COMPRESSOR_V2      = 0x01020001;
static constexpr uint32_t AUDIO_OUT2_MASTERING_STATES_STRUCT_ID_LIMITER_DEFAULT    = 0x01010003;

static AudioOut2ContextState* audioout2_find_context_locked(AudioOut2ContextHandle ctx) {
	for (auto& state: g_audioout2_contexts) {
		if (state.used && state.handle == ctx) {
			return &state;
		}
	}

	return nullptr;
}

static constexpr uint64_t AUDIO_OUT2_CLOCK_RATE = 48000;

// Grain length in 1/48000 us units, i.e. samples * 1000000.
static uint64_t audioout2_grain_scaled(uint32_t grains) {
	const auto sample_count = static_cast<uint64_t>(grains == 0 ? 512u : grains);
	return std::max<uint64_t>(sample_count * 1000000u, 1000u * AUDIO_OUT2_CLOCK_RATE);
}

static uint32_t audioout2_grain_micros(uint32_t grains) {
	const auto sample_count = (grains == 0 ? 512u : grains);
	return std::max<uint32_t>((sample_count * 1000000u) / 48000u, 1000u);
}

static uint8_t audioout2_data_format_channels(uint32_t data_format) {
	const auto channels = (data_format >> 8u) & 0xffu;
	return static_cast<uint8_t>(channels == 0 ? 2u : std::min(channels, 16u));
}

static AudioInternal::Format audioout2_data_format_to_audio_format(uint32_t data_format) {
	const auto channels  = audioout2_data_format_channels(data_format);
	const auto data_type = data_format & 0x7fu;
	const auto is_std    = (data_format & 0x80u) != 0;

	switch (data_type) {
		case 0:
			switch (channels) {
				case 1: return AudioInternal::Format::FloatMono;
				case 2: return AudioInternal::Format::FloatStereo;
				case 8:
					return is_std ? AudioInternal::Format::Float8ChStd
					              : AudioInternal::Format::Float8Ch;
				case 12:
					if (!is_std) {
						return AudioInternal::Format::Float12Ch;
					}
					break;
				default: break;
			}
			break;
		case 1:
			switch (channels) {
				case 1: return AudioInternal::Format::Signed16bitMono;
				case 2: return AudioInternal::Format::Signed16bitStereo;
				case 8:
					return is_std ? AudioInternal::Format::Signed16bit8ChStd
					              : AudioInternal::Format::Signed16bit8Ch;
				default: break;
			}
			break;
		default: break;
	}

	return AudioInternal::Format::Unknown;
}

static int audioout2_port_type_to_audio_out_type(uint16_t port_type) {
	switch (port_type & 0xffu) {
		case 0: return AUDIO_OUT_PORT_TYPE_MAIN;
		case 1: return AUDIO_OUT_PORT_TYPE_BGM;
		case 2: return AUDIO_OUT_PORT_TYPE_VOICE;
		case 3: return AUDIO_OUT_PORT_TYPE_PADSPK;
		case 4: return AUDIO_OUT_PORT_TYPE_PERSONAL;
		case 5: return AUDIO_OUT_PORT_TYPE_AUX;
		case 6: return AUDIO_OUT_PORT_TYPE_VIBRATION;
		default: return AUDIO_OUT_PORT_TYPE_MAIN;
	}
}

static bool audioout2_port_type_is_object(uint16_t port_type) {
	return (port_type & 0xff00u) == 0x0100u;
}

static void audioout2_update_context_locked(AudioOut2ContextState* state) {
	if (state == nullptr) {
		return;
	}

	// The modelled output takes one grain per tick of a free-running clock, like a hardware mixer.
	const auto now = LibKernel::KernelGetProcessTime();
	if (state->last_update == 0) {
		state->last_update = now;
		return;
	}

	const auto grain = audioout2_grain_scaled(state->num_grains);
	if (now <= state->last_update) {
		return;
	}
	const auto elapsed = (now - state->last_update) * AUDIO_OUT2_CLOCK_RATE;
	if (elapsed < state->last_update_frac + grain) {
		return;
	}
	const auto ticks = (elapsed - state->last_update_frac) / grain;

	const auto drained = std::min<uint64_t>(state->queued, ticks);
	state->queued -= static_cast<uint32_t>(drained);
	if (ticks > drained) {
		state->idle = true;
	}
	const auto advance = state->last_update_frac + ticks * grain;
	state->last_update += advance / AUDIO_OUT2_CLOCK_RATE;
	state->last_update_frac = advance % AUDIO_OUT2_CLOCK_RATE;
}

static void audioout2_accept_grain_locked(AudioOut2ContextState* state) {
	if (state->queued == 0 && state->idle) {
		// The output ran dry, so its clock restarts with this grain.
		state->last_update      = LibKernel::KernelGetProcessTime();
		state->last_update_frac = 0;
		state->idle             = false;
	}
	if (state->queued < state->queue_depth) {
		state->queued++;
	}
}

static uint64_t audioout2_micros_to_next_tick_locked(const AudioOut2ContextState& state) {
	const auto grain_micros = static_cast<uint64_t>(audioout2_grain_micros(state.num_grains));
	const auto now          = LibKernel::KernelGetProcessTime();
	const auto next_tick =
	    state.last_update + (state.last_update_frac + audioout2_grain_scaled(state.num_grains) +
	                         AUDIO_OUT2_CLOCK_RATE - 1) /
	                            AUDIO_OUT2_CLOCK_RATE;
	return next_tick > now ? std::min(next_tick - now, grain_micros + 1) : 1;
}

// A synchronous push returns once the queue can take another grain; RE Engine yield-polls
// otherwise.
static void audioout2_wait_for_free_slot(AudioOut2ContextHandle ctx) {
	for (;;) {
		uint64_t sleep_micros = 0;
		g_audioout2_context_mutex.Lock();
		if (auto* state = audioout2_find_context_locked(ctx); state != nullptr) {
			audioout2_update_context_locked(state);
			if (state->queued >= state->queue_depth) {
				sleep_micros = audioout2_micros_to_next_tick_locked(*state);
			}
		}
		g_audioout2_context_mutex.Unlock();

		if (sleep_micros == 0) {
			return;
		}
		Common::Thread::SleepMicro(static_cast<uint32_t>(sleep_micros));
	}
}

static AudioOut2PortStateEntry* audioout2_find_port_locked(AudioOut2PortHandle port) {
	for (auto& state: g_audioout2_ports) {
		if (state.used && state.handle == port) {
			return &state;
		}
	}

	return nullptr;
}

static size_t audioout2_pcm_size(const AudioOut2PortStateEntry& state) {
	const auto bytes_per_sample =
	    (state.data_format & 0x7fu) == 1 ? sizeof(int16_t) : sizeof(float);
	return static_cast<size_t>(state.samples_num) *
	       audioout2_data_format_channels(state.data_format) * bytes_per_sample;
}

struct AudioOut2ObjectBus {
	int      audio_handle = 0;
	uint32_t num_grains   = 512;
};

static AudioOut2ObjectBus audioout2_object_bus(AudioOut2ContextHandle ctx) {
	Common::LockGuard lock(g_audioout2_context_mutex);
	const auto*       state = audioout2_find_context_locked(ctx);
	return state != nullptr ? AudioOut2ObjectBus {state->object_audio_handle, state->num_grains}
	                        : AudioOut2ObjectBus {};
}

static bool audioout2_context_has_queueable_device(AudioOut2ContextHandle ctx) {
	const auto        bus = audioout2_object_bus(ctx);
	Common::LockGuard lock(g_audioout2_port_mutex);
	for (const auto& state: g_audioout2_ports) {
		if (state.used && state.context == ctx && !state.pcm_data.empty()) {
			const int handle = audioout2_port_type_is_object(state.port_type) ? bus.audio_handle
			                                                                  : state.audio_handle;
			if (handle > 0 && AudioInternal::AudioOutHasDevice(handle)) {
				return true;
			}
		}
	}
	return false;
}

using AudioOut2StereoGains = std::array<float, 2>;

using AudioOut2AmbisonicsTable = std::array<AudioOut2StereoGains, 36>;

enum class AudioOut2AmbisonicsDecoder { Wide, Sharp, Ring12, Cardioid };

// ACN/SN3D orders 0-5 to stereo through a horizontal ring of virtual speakers.
static AudioOut2AmbisonicsTable audioout2_build_ambisonics_table(AudioOut2AmbisonicsDecoder decoder,
                                                                 double front_gain) {
	constexpr int    order    = 5;
	constexpr double pi       = 3.14159265358979323846;
	const int        speakers = decoder == AudioOut2AmbisonicsDecoder::Ring12 ? 12 : 24;
	std::array<double, order + 1> sn3d {};   // horizon amplitude of the sectoral harmonics
	std::array<double, order + 1> weight {}; // max-rE, or 1 (basic) for the sharp decode
	for (int l = 0; l <= order; l++) {
		double factorial = 1.0;
		double odd       = 1.0;
		for (int i = 2; i <= 2 * l; i++) {
			factorial *= i;
		}
		for (int i = 1; i < 2 * l; i += 2) {
			odd *= i;
		}
		sn3d[l]   = l == 0 ? 1.0 : std::sqrt(2.0 / factorial) * odd;
		weight[l] = decoder == AudioOut2AmbisonicsDecoder::Sharp ? 1.0
		            : decoder == AudioOut2AmbisonicsDecoder::Cardioid
		                ? (l <= 1 ? 1.0 : 0.0)
		                : std::cos(l * pi / (2 * order + 2));
	}
	std::array<std::array<double, 2>, 36> gains {};
	std::array<std::array<double, 2>, 36> flat {};
	if (decoder == AudioOut2AmbisonicsDecoder::Cardioid) {
		gains[0] = flat[0] = {1.0, 1.0};
		gains[1] = flat[1] = {1.0, -1.0};
		gains[3]           = {front_gain - 1.0, front_gain - 1.0};
	} else {
		for (int k = 0; k < speakers; k++) {
			const double azimuth = 2.0 * pi * k / speakers; // counterclockwise, 0 = front
			double       side    = std::sin(azimuth);
			if (decoder != AudioOut2AmbisonicsDecoder::Ring12) {
				const double lateral = std::asin(std::clamp(side, -1.0, 1.0));
				side                 = std::clamp(lateral / (pi / 6.0), -1.0, 1.0);
			}
			const double pan      = (1.0 - side) * pi / 4.0;
			const double out[2]   = {std::cos(pan) / speakers, std::sin(pan) / speakers};
			const double ahead    = std::max(0.0, std::cos(azimuth));
			const double emphasis = 1.0 + (front_gain - 1.0) * ahead * ahead;
			for (int ch = 0; ch < 2; ch++) {
				std::array<double, 36> row {};
				row[0] = out[ch] * weight[0];
				for (int l = 1; l <= order; l++) {
					row[l * l + 2 * l] =
					    out[ch] * 2.0 * weight[l] * std::cos(l * azimuth) / sn3d[l];
					row[l * l] = out[ch] * 2.0 * weight[l] * std::sin(l * azimuth) / sn3d[l];
				}
				for (size_t i = 0; i < row.size(); i++) {
					flat[i][ch] += row[i];
					gains[i][ch] += row[i] * emphasis;
				}
			}
		}
	}
	double front = flat[0][0];
	for (int l = 1; l <= order; l++) {
		front += flat[l * l + 2 * l][0] * sn3d[l];
	}
	AudioOut2AmbisonicsTable result {};
	for (size_t i = 0; i < result.size(); i++) {
		for (int ch = 0; ch < 2; ch++) {
			result[i][ch] = static_cast<float>(gains[i][ch] * 0.70710678118654752 / front);
		}
	}
	return result;
}

static std::atomic<const AudioOut2AmbisonicsTable*> g_audioout2_ambisonics_table {nullptr};

bool AudioOut2SelectAmbisonicsDecoder(const char* name, double front_gain_db) {
	static constexpr const char*                NAMES[] = {"wide", "sharp", "ring12", "cardioid"};
	static constexpr AudioOut2AmbisonicsDecoder DECODERS[] = {
	    AudioOut2AmbisonicsDecoder::Wide, AudioOut2AmbisonicsDecoder::Sharp,
	    AudioOut2AmbisonicsDecoder::Ring12, AudioOut2AmbisonicsDecoder::Cardioid};
	size_t index = 0;
	bool   known = name == nullptr;
	for (size_t i = 0; i < std::size(NAMES); i++) {
		if (name != nullptr && std::strcmp(name, NAMES[i]) == 0) {
			index = i;
			known = true;
		}
	}
	const double gain = std::pow(10.0, std::clamp(front_gain_db, -12.0, 12.0) / 20.0);
	// A replaced table is never freed: a mixer thread may still be reading it.
	g_audioout2_ambisonics_table.store(
	    new AudioOut2AmbisonicsTable(audioout2_build_ambisonics_table(DECODERS[index], gain)));
	return known;
}

static const AudioOut2AmbisonicsTable& audioout2_ambisonics_stereo() {
	const auto* table = g_audioout2_ambisonics_table.load();
	if (table == nullptr) {
		static Common::Mutex mutex;
		Common::LockGuard    lock(mutex);
		table = g_audioout2_ambisonics_table.load();
		if (table == nullptr) {
			const char* name       = std::getenv("KYTY_AMBI_DECODE");
			const char* front      = std::getenv("KYTY_AMBI_FRONT_GAIN_DB");
			const auto  front_gain = front != nullptr && front[0] != '\0'
			                             ? std::strtod(front, nullptr)
			                             : AUDIO_OUT2_DEFAULT_FRONT_GAIN_DB;
			if (!AudioOut2SelectAmbisonicsDecoder(
			        name != nullptr && name[0] != '\0' ? name : nullptr, front_gain)) {
				LOGF("AudioOut2: unknown KYTY_AMBI_DECODE '%s', using wide\n", name);
			}
			table = g_audioout2_ambisonics_table.load();
		}
	}
	return *table;
}

static bool audioout2_object_stereo_gains(uint32_t ambisonics, AudioOut2StereoGains* gains) {
	const auto& acn = audioout2_ambisonics_stereo();
	if (ambisonics == UINT32_MAX) {
		*gains = {0.70710678f, 0.70710678f};
	} else if (ambisonics >= 64 && ambisonics < 64 + acn.size()) {
		*gains = acn[ambisonics - 64];
	} else if (ambisonics <= 3) {
		// FuMa W is -3 dB relative to SN3D; FuMa orders its first-order terms X, Y, Z.
		static constexpr uint32_t FUMA_TO_ACN[4] = {0, 3, 1, 2};
		*gains                                   = acn[FUMA_TO_ACN[ambisonics]];
		if (ambisonics == 0) {
			(*gains)[0] *= 1.41421356f;
			(*gains)[1] *= 1.41421356f;
		}
	} else {
		return false;
	}
	return true;
}

static void audioout2_mix_object(std::vector<float>* mix, const AudioOut2PortStateEntry& object) {
	AudioOut2StereoGains stereo {};
	if (!audioout2_object_stereo_gains(object.ambisonics, &stereo)) {
		return;
	}
	float left  = stereo[0];
	float right = stereo[1];
	left *= object.gains[0];
	right *= object.gains[0];
	const auto frames = std::min<size_t>(mix->size() / 2, object.samples_num);
	for (size_t frame = 0; frame < frames; frame++) {
		const float sample =
		    object.audio_format == AudioInternal::Format::FloatMono
		        ? reinterpret_cast<const float*>(object.pcm_data.data())[frame]
		        : reinterpret_cast<const int16_t*>(object.pcm_data.data())[frame] / 32768.0f;
		(*mix)[frame * 2] += sample * left;
		(*mix)[frame * 2 + 1] += sample * right;
	}
}

static AudioOut2Grain audioout2_snapshot_context_locked(AudioOut2ContextHandle    ctx,
                                                        const AudioOut2ObjectBus& bus,
                                                        bool                      fresh_only) {
	AudioOut2Grain     grain;
	std::vector<float> object_mix;
	for (const auto& state: g_audioout2_ports) {
		if (!state.used || state.context != ctx || state.pcm_data.empty()) {
			continue;
		}
		// A port given no new PCM since the last advance is silent for this grain.
		const bool stale = fresh_only && !state.pcm_fresh;
		if (audioout2_port_type_is_object(state.port_type)) {
			if (bus.audio_handle > 0) {
				object_mix.resize(static_cast<size_t>(bus.num_grains) * 2, 0.0f);
				if (!stale) {
					audioout2_mix_object(&object_mix, state);
				}
			}
		} else if (state.audio_handle > 0 && grain.size() < AudioInternal::OUT_PORTS_MAX) {
			grain.push_back(AudioOut2GrainPort {
			    state.handle, state.audio_handle,
			    stale ? std::vector<uint8_t>(state.pcm_data.size(), 0) : state.pcm_data,
			    state.gains});
		}
	}
	for (auto& sample: object_mix) {
		const float magnitude = std::abs(sample);
		if (magnitude > 0.9f) {
			sample = std::copysign(0.9f + 0.1f * std::tanh((magnitude - 0.9f) / 0.1f), sample);
		}
	}
	if (!object_mix.empty() && grain.size() < AudioInternal::OUT_PORTS_MAX) {
		AudioOut2GrainPort bus_entry {0, bus.audio_handle, {}, {}};
		const auto*        bytes = reinterpret_cast<const uint8_t*>(object_mix.data());
		bus_entry.pcm.assign(bytes, bytes + object_mix.size() * sizeof(float));
		bus_entry.gains.fill(1.0f);
		grain.push_back(std::move(bus_entry));
	}
	return grain;
}

static void audioout2_drop_dead_ports_locked(AudioOut2Grain* grain, int object_audio_handle) {
	std::erase_if(*grain, [object_audio_handle](const AudioOut2GrainPort& entry) {
		if (entry.port == 0) {
			return entry.audio_handle != object_audio_handle;
		}
		const auto* state = audioout2_find_port_locked(entry.port);
		return state == nullptr || state->audio_handle != entry.audio_handle;
	});
}

static bool audioout2_take_advanced_grain(AudioOut2ContextHandle ctx, AudioOut2Grain* grain,
                                          AudioOut2ObjectBus* bus, bool* uses_advance) {
	Common::LockGuard lock(g_audioout2_context_mutex);
	auto*             state = audioout2_find_context_locked(ctx);
	if (state == nullptr) {
		return false;
	}
	*bus          = AudioOut2ObjectBus {state->object_audio_handle, state->num_grains};
	*uses_advance = state->uses_advance;
	if (state->advanced.empty()) {
		return false;
	}
	*grain = std::move(state->advanced.front());
	state->advanced.pop_front();
	return true;
}

static void audioout2_queue_context_audio(AudioOut2ContextHandle ctx, bool blocking, bool cushion) {
	std::shared_lock output_lock(g_audioout2_output_mutex);

	AudioOut2Grain     grain;
	AudioOut2ObjectBus bus;
	bool               uses_advance = false;
	const bool         advanced = audioout2_take_advanced_grain(ctx, &grain, &bus, &uses_advance);
	{
		Common::LockGuard lock(g_audioout2_port_mutex);
		if (advanced) {
			audioout2_drop_dead_ports_locked(&grain, bus.audio_handle);
		} else {
			grain = audioout2_snapshot_context_locked(ctx, bus, false);
		}
	}
	if (!advanced && uses_advance) {
		// A push with no new advance is an underrun; replaying the last grain buzzes.
		for (auto& entry: grain) {
			std::fill(entry.pcm.begin(), entry.pcm.end(), uint8_t {0});
		}
	}

	std::vector<AudioInternal::OutputParam> params;
	params.reserve(grain.size());
	for (const auto& entry: grain) {
		params.push_back(
		    AudioInternal::OutputParam {entry.audio_handle, entry.pcm.data(), entry.gains.data()});
	}
	if (!params.empty()) {
		(void)AudioInternal::AudioOutOutputs(params.data(), static_cast<uint32_t>(params.size()),
		                                     blocking, cushion);
	}
}

static void audioout2_wait_for_advanced_grain(AudioOut2ContextHandle ctx) {
	const auto start = LibKernel::KernelGetProcessTime();
	for (;;) {
		uint64_t limit = 0;
		{
			Common::LockGuard lock(g_audioout2_context_mutex);
			auto*             state = audioout2_find_context_locked(ctx);
			if (state == nullptr || !state->uses_advance || !state->advanced.empty()) {
				return;
			}
			limit = 2ull * audioout2_grain_micros(state->num_grains);
		}
		if (LibKernel::KernelGetProcessTime() - start >= limit) {
			return;
		}
		Common::Thread::SleepMicro(250);
	}
}

static void audioout2_close_audio_handle(int audio_handle) {
	if (audio_handle > 0) {
		std::unique_lock output_lock(g_audioout2_output_mutex);
		AudioInternal::AudioOutClose(audio_handle);
	}
}

int KYTY_SYSV_ABI AudioOut2Initialize() {
	PRINT_NAME();

	return OK;
}

int KYTY_SYSV_ABI AudioOut2ContextResetParam(AudioOut2ContextParam* params) {
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(params == nullptr);

	std::memset(params, 0, sizeof(AudioOut2ContextParam));
	params->max_ports              = 256;
	params->max_object_ports       = 256;
	params->guarantee_object_ports = 0;
	params->queue_depth            = 4;
	params->num_grains             = 512;
	params->flags                  = 1;

	return OK;
}

int KYTY_SYSV_ABI AudioOut2ContextQueryMemory(const AudioOut2ContextParam* params,
                                              size_t*                      memory_size) {
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(params == nullptr);
	EXIT_NOT_IMPLEMENTED(memory_size == nullptr);

	const auto queue_depth = (params->queue_depth == 0 ? 4u : params->queue_depth);
	*memory_size           = 0x10000u + static_cast<size_t>(queue_depth) * 0x590u;

	LOGF("\t memory_size = 0x%016" PRIx64 "\n", static_cast<uint64_t>(*memory_size));

	return OK;
}

int KYTY_SYSV_ABI AudioOut2ContextCreate(const AudioOut2ContextParam* params, void* buffer,
                                         size_t buffer_size, AudioOut2ContextHandle* ctx) {
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(params == nullptr);
	EXIT_NOT_IMPLEMENTED(ctx == nullptr);

	const auto num_grains  = (params->num_grains == 0 ? 512u : params->num_grains);
	const auto queue_depth = (params->queue_depth == 0 ? 4u : params->queue_depth);
	const int  object_audio_handle =
	    params->max_object_ports == 0
	        ? 0
	        : AudioInternal::AudioOutOpen(AUDIO_OUT_PORT_TYPE_MAIN, num_grains, 48000,
	                                      AudioInternal::Format::FloatStereo);
	if (params->max_object_ports != 0 && object_audio_handle == 0) {
		return AUDIO_OUT2_ERROR_OUT_OF_RESOURCE;
	}

	g_audioout2_context_mutex.Lock();
	AudioOut2ContextState* state = nullptr;
	for (auto& candidate: g_audioout2_contexts) {
		if (!candidate.used) {
			state = &candidate;
			break;
		}
	}
	if (state == nullptr) {
		g_audioout2_context_mutex.Unlock();
		audioout2_close_audio_handle(object_audio_handle);
		return AUDIO_OUT2_ERROR_OUT_OF_RESOURCE;
	}
	*ctx                       = g_audioout2_next_context.fetch_add(1, std::memory_order_relaxed);
	*state                     = AudioOut2ContextState {};
	state->used                = true;
	state->handle              = *ctx;
	state->queue_depth         = queue_depth;
	state->queued              = 0;
	state->num_grains          = num_grains;
	state->last_update         = LibKernel::KernelGetProcessTime();
	state->object_audio_handle = object_audio_handle;
	g_audioout2_context_mutex.Unlock();

	LOGF("\t buffer      = 0x%016" PRIx64 "\n"
	     "\t buffer_size = 0x%016" PRIx64 "\n"
	     "\t ctx         = 0x%016" PRIx64 "\n"
	     "\t queue_depth = %" PRIu32 ", num_grains = %" PRIu32 "\n",
	     reinterpret_cast<uint64_t>(buffer), static_cast<uint64_t>(buffer_size), *ctx, queue_depth,
	     num_grains);

	return OK;
}

int KYTY_SYSV_ABI AudioOut2ContextDestroy(AudioOut2ContextHandle ctx) {
	PRINT_NAME();
	LOGF("\t ctx = 0x%016" PRIx64 "\n", ctx);

	std::array<int, 257> audio_handles {};
	size_t               audio_handles_num = 0;

	g_audioout2_context_mutex.Lock();
	if (auto* state = audioout2_find_context_locked(ctx); state != nullptr) {
		if (state->object_audio_handle > 0) {
			audio_handles[audio_handles_num++] = state->object_audio_handle;
		}
		*state = AudioOut2ContextState {};
	}
	g_audioout2_context_mutex.Unlock();

	g_audioout2_port_mutex.Lock();
	for (auto& port_state: g_audioout2_ports) {
		if (port_state.used && port_state.context == ctx) {
			if (port_state.audio_handle > 0 && audio_handles_num < audio_handles.size()) {
				audio_handles[audio_handles_num++] = port_state.audio_handle;
			}
			port_state = AudioOut2PortStateEntry {};
		}
	}
	g_audioout2_port_mutex.Unlock();

	for (size_t i = 0; i < audio_handles_num; i++) {
		audioout2_close_audio_handle(audio_handles[i]);
	}

	return OK;
}

int KYTY_SYSV_ABI AudioOut2ContextSetAttributes(AudioOut2ContextHandle    ctx,
                                                const AudioOut2Attribute* attributes,
                                                uint32_t                  num) {
	PRINT_NAME();
	LOGF("\t ctx = 0x%016" PRIx64 ", num = %" PRIu32 "\n", ctx, num);
	EXIT_NOT_IMPLEMENTED(num != 0 && attributes == nullptr);
	return OK;
}

int KYTY_SYSV_ABI AudioOut2ContextAdvance(AudioOut2ContextHandle ctx) {
	const auto     bus = audioout2_object_bus(ctx);
	AudioOut2Grain grain;
	{
		Common::LockGuard lock(g_audioout2_port_mutex);
		grain = audioout2_snapshot_context_locked(ctx, bus, true);
		for (auto& port: g_audioout2_ports) {
			if (port.used && port.context == ctx) {
				port.attributes_locked = false;
				port.pcm_fresh         = false;
			}
		}
	}

	g_audioout2_context_mutex.Lock();
	if (auto* state = audioout2_find_context_locked(ctx); state != nullptr) {
		audioout2_update_context_locked(state);
		state->uses_advance = true;
		state->advanced.push_back(std::move(grain));
		const size_t limit = 2 * std::max<size_t>(state->queue_depth, 4);
		while (state->advanced.size() > limit) {
			state->advanced.pop_front();
		}
	}
	g_audioout2_context_mutex.Unlock();

	return OK;
}

int KYTY_SYSV_ABI AudioOut2ContextPush(AudioOut2ContextHandle ctx, uint32_t blocking) {
	uint64_t sleep_micros = audioout2_grain_micros(512);

	if (blocking != 0) {
		audioout2_wait_for_advanced_grain(ctx);
	}

	for (;;) {
		// Only a synchronous submission carrying PCM to a real device can rely on the SDL queue for
		// pacing. Async pushes must retain queue-depth backpressure, and a handle without PCM (or a
		// vibration/failed-open handle) has no downstream operation that can block this call.
		const bool use_device_clock = blocking != 0 && audioout2_context_has_queueable_device(ctx);

		g_audioout2_context_mutex.Lock();
		if (auto* state = audioout2_find_context_locked(ctx); state != nullptr) {
			audioout2_update_context_locked(state);
			if (state->queued < state->queue_depth || use_device_clock) {
				audioout2_accept_grain_locked(state);
				g_audioout2_context_mutex.Unlock();
				// Without a device the modelled queue is the only clock; don't sleep twice.
				audioout2_queue_context_audio(ctx, use_device_clock, blocking == 0);
				if (blocking != 0 && !use_device_clock) {
					audioout2_wait_for_free_slot(ctx);
				}
				return OK;
			}
			sleep_micros = audioout2_micros_to_next_tick_locked(*state);
		}
		g_audioout2_context_mutex.Unlock();

		if (blocking == 0) {
			return AUDIO_OUT2_ERROR_NOT_READY;
		}

		Common::Thread::SleepMicro(static_cast<uint32_t>(sleep_micros));
	}
}

int KYTY_SYSV_ABI AudioOut2ContextGetQueueLevel(AudioOut2ContextHandle ctx, uint32_t* queue_level,
                                                uint32_t* available_queues) {
	if (queue_level != nullptr) {
		*queue_level = 0;
	}
	if (available_queues != nullptr) {
		*available_queues = 4;
	}

	g_audioout2_context_mutex.Lock();
	if (auto* state = audioout2_find_context_locked(ctx); state != nullptr) {
		audioout2_update_context_locked(state);
		const auto backlog =
		    state->advanced.size() > 1 ? static_cast<uint32_t>(state->advanced.size() - 1) : 0u;
		const auto level = std::min(state->queued + backlog, state->queue_depth);
		if (queue_level != nullptr) {
			*queue_level = level;
		}
		if (available_queues != nullptr) {
			*available_queues = state->queue_depth - level;
		}
	}
	g_audioout2_context_mutex.Unlock();

	return OK;
}

int KYTY_SYSV_ABI AudioOut2PortCreate(AudioOut2ContextHandle ctx, const AudioOut2PortParam* params,
                                      AudioOut2PortHandle* port) {
	EXIT_NOT_IMPLEMENTED(params == nullptr);
	EXIT_NOT_IMPLEMENTED(port == nullptr);

	const auto next_port    = g_audioout2_next_port.fetch_add(1, std::memory_order_relaxed);
	const auto audio_format = audioout2_data_format_to_audio_format(params->data_format);
	const auto audio_type   = audioout2_port_type_to_audio_out_type(params->port_type);

	g_audioout2_context_mutex.Lock();
	const auto* context_state = audioout2_find_context_locked(ctx);
	if (context_state == nullptr) {
		g_audioout2_context_mutex.Unlock();
		return AUDIO_OUT2_ERROR_INVALID_PARAM;
	}
	if (audioout2_port_type_is_object(params->port_type) &&
	    (params->port_type != 0x100 || context_state->object_audio_handle == 0 ||
	     params->sampling_freq != 48000 ||
	     (audio_format != AudioInternal::Format::FloatMono &&
	      audio_format != AudioInternal::Format::Signed16bitMono))) {
		g_audioout2_context_mutex.Unlock();
		return AUDIO_OUT2_ERROR_INVALID_PARAM;
	}
	const auto samples_num = context_state->num_grains == 0 ? 512u : context_state->num_grains;

	g_audioout2_port_mutex.Lock();
	if (std::find(g_audioout2_users.begin(), g_audioout2_users.end(), params->user_handle) ==
	    g_audioout2_users.end()) {
		g_audioout2_port_mutex.Unlock();
		g_audioout2_context_mutex.Unlock();
		return AUDIO_OUT2_ERROR_INVALID_PARAM;
	}
	AudioOut2PortStateEntry* port_state = nullptr;
	for (auto& candidate: g_audioout2_ports) {
		if (!candidate.used) {
			port_state = &candidate;
			break;
		}
	}
	if (port_state != nullptr) {
		*port_state               = AudioOut2PortStateEntry {};
		port_state->used          = true;
		port_state->handle        = next_port;
		port_state->context       = ctx;
		port_state->user          = params->user_handle;
		port_state->port_type     = params->port_type;
		port_state->data_format   = params->data_format;
		port_state->sampling_freq = params->sampling_freq;
		port_state->samples_num   = samples_num;
		port_state->audio_format  = audio_format;
		port_state->gains.fill(1.0f);
	}
	g_audioout2_port_mutex.Unlock();
	g_audioout2_context_mutex.Unlock();

	if (port_state == nullptr) {
		return AUDIO_OUT2_ERROR_PORT_FULL;
	}

	int audio_handle = 0;

	if (audio_format != AudioInternal::Format::Unknown &&
	    !audioout2_port_type_is_object(params->port_type)) {
		audio_handle = AudioInternal::AudioOutOpen(audio_type, samples_num, params->sampling_freq,
		                                           audio_format);
	}

	g_audioout2_port_mutex.Lock();
	const bool reserved = port_state->used && port_state->handle == next_port;
	if (reserved) {
		port_state->audio_handle = audio_handle;
	}
	g_audioout2_port_mutex.Unlock();
	if (!reserved) {
		audioout2_close_audio_handle(audio_handle);
		return AUDIO_OUT2_ERROR_INVALID_PARAM;
	}

	*port = next_port;

	if (next_port <= 16 || (next_port % 600) == 0) {
		PRINT_NAME();
		LOGF("\t ctx           = 0x%016" PRIx64 "\n"
		     "\t port          = 0x%016" PRIx64 "\n"
		     "\t port_type     = %" PRIu16 "\n"
		     "\t data_format   = 0x%08" PRIx32 "\n"
		     "\t sampling_freq = %" PRIu32 "\n",
		     ctx, *port, params->port_type, params->data_format, params->sampling_freq);
	}

	return OK;
}

int KYTY_SYSV_ABI AudioOut2PortDestroy(AudioOut2PortHandle port) {
	PRINT_NAME();
	LOGF("\t port = 0x%016" PRIx64 "\n", port);

	int audio_handle = 0;
	g_audioout2_port_mutex.Lock();
	if (auto* state = audioout2_find_port_locked(port); state != nullptr) {
		audio_handle = state->audio_handle;
		*state       = AudioOut2PortStateEntry {};
	}
	g_audioout2_port_mutex.Unlock();

	audioout2_close_audio_handle(audio_handle);

	return OK;
}

int KYTY_SYSV_ABI AudioOut2PortSetAttributes(AudioOut2PortHandle       port,
                                             const AudioOut2Attribute* attributes, uint32_t num) {
	if (num != 0 && attributes == nullptr) {
		return AUDIO_OUT2_ERROR_INVALID_PARAM;
	}
	Common::LockGuard lock(g_audioout2_port_mutex);
	auto*             state = audioout2_find_port_locked(port);
	if (state == nullptr) {
		return AUDIO_OUT2_ERROR_INVALID_PORT;
	}

	const void* pcm_data   = nullptr;
	auto        gains      = state->gains;
	auto        ambisonics = state->ambisonics;
	uint32_t    seen       = 0;
	for (uint32_t i = 0; i < num; i++) {
		const auto& attribute = attributes[i];
		const auto  id        = attribute.attribute_id;
		if (id != AUDIO_OUT2_PORT_ATTRIBUTE_ID_PCM && id != AUDIO_OUT2_PORT_ATTRIBUTE_ID_GAIN &&
		    id != AUDIO_OUT2_PORT_ATTRIBUTE_ID_AMBISONICS) {
			continue;
		}
		if ((seen & (1u << id)) != 0) {
			return AUDIO_OUT2_ERROR_INVALID_PARAM;
		}
		seen |= 1u << id;
		// An object's attributes are frozen from its PCM until the next advance.
		if (id != AUDIO_OUT2_PORT_ATTRIBUTE_ID_PCM && state->attributes_locked) {
			continue;
		}
		if (attribute.value == nullptr) {
			return AUDIO_OUT2_ERROR_INVALID_PARAM;
		}
		switch (id) {
			case AUDIO_OUT2_PORT_ATTRIBUTE_ID_PCM: {
				if (attribute.value_size < sizeof(AudioOut2Pcm)) {
					return AUDIO_OUT2_ERROR_INVALID_PARAM;
				}
				AudioOut2Pcm pcm {};
				std::memcpy(&pcm, attribute.value, sizeof(pcm));
				pcm_data = pcm.data;
				break;
			}
			case AUDIO_OUT2_PORT_ATTRIBUTE_ID_GAIN: {
				// Wwise passes an 8-float gain vector even for a mono pad-speaker port.
				const auto channels = audioout2_data_format_channels(state->data_format);
				if (attribute.value_size < channels * sizeof(float) ||
				    attribute.value_size % sizeof(float) != 0 ||
				    attribute.value_size > gains.size() * sizeof(float)) {
					return AUDIO_OUT2_ERROR_INVALID_PARAM;
				}
				std::memcpy(gains.data(), attribute.value, channels * sizeof(float));
				for (uint32_t ch = 0; ch < channels; ch++) {
					if (!std::isfinite(gains[ch]) || gains[ch] < 0.0f) {
						return AUDIO_OUT2_ERROR_INVALID_PARAM;
					}
				}
				break;
			}
			case AUDIO_OUT2_PORT_ATTRIBUTE_ID_AMBISONICS:
				if (!audioout2_port_type_is_object(state->port_type) ||
				    attribute.value_size != sizeof(ambisonics)) {
					return AUDIO_OUT2_ERROR_INVALID_PARAM;
				}
				std::memcpy(&ambisonics, attribute.value, sizeof(ambisonics));
				if (ambisonics != UINT32_MAX && ambisonics > 15 &&
				    (ambisonics < 64 || ambisonics > 99)) {
					return AUDIO_OUT2_ERROR_INVALID_PARAM;
				}
				break;
			default: break;
		}
	}

	state->gains      = gains;
	state->ambisonics = ambisonics;
	if ((seen & (1u << AUDIO_OUT2_PORT_ATTRIBUTE_ID_PCM)) != 0) {
		if (pcm_data != nullptr && state->audio_format != AudioInternal::Format::Unknown) {
			const auto* bytes = static_cast<const uint8_t*>(pcm_data);
			state->pcm_data.assign(bytes, bytes + audioout2_pcm_size(*state));
		} else {
			state->pcm_data.clear();
		}
		state->attributes_locked = audioout2_port_type_is_object(state->port_type);
		state->pcm_fresh         = !state->pcm_data.empty();
	}

	return OK;
}

int KYTY_SYSV_ABI AudioOut2PortGetState(AudioOut2PortHandle port, AudioOut2PortState* state) {
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(state == nullptr);

	std::memset(state, 0, sizeof(AudioOut2PortState));
	state->output          = 1;
	state->num_channels    = 2;
	state->volume          = 127;
	state->reroute_counter = 0;
	state->flags           = 0;

	g_audioout2_port_mutex.Lock();
	if (auto* port_state = audioout2_find_port_locked(port); port_state != nullptr) {
		state->num_channels = audioout2_data_format_channels(port_state->data_format);
	}
	g_audioout2_port_mutex.Unlock();

	// LOGF("\t port = 0x%016" PRIx64 "\n", port);
	// LOGF("\t num_channels = %" PRIu8 "\n", state->num_channels);

	return OK;
}

int KYTY_SYSV_ABI AudioOut2GetSystemState(AudioOut2SystemState* state) {
	PRINT_NAME();
	EXIT_NOT_IMPLEMENTED(state == nullptr);
	std::memset(state, 0, sizeof(AudioOut2SystemState));
	return OK;
}

int KYTY_SYSV_ABI AudioOut2UserCreate(uint32_t user_id, AudioOut2UserHandle* handle) {
	PRINT_NAME();
	if (handle == nullptr) {
		return AUDIO_OUT2_ERROR_INVALID_PARAM;
	}
	Common::LockGuard lock(g_audioout2_port_mutex);
	*handle = g_audioout2_next_user++;
	g_audioout2_users.push_back(*handle);
	LOGF("\t user_id = %" PRIu32 ", handle = 0x%016" PRIx64 "\n", user_id,
	     static_cast<uint64_t>(*handle));
	return OK;
}

int KYTY_SYSV_ABI AudioOut2UserDestroy(AudioOut2UserHandle handle) {
	PRINT_NAME();
	LOGF("\t handle = 0x%016" PRIx64 "\n", static_cast<uint64_t>(handle));
	Common::LockGuard lock(g_audioout2_port_mutex);
	const auto        it = std::find(g_audioout2_users.begin(), g_audioout2_users.end(), handle);
	if (it == g_audioout2_users.end()) {
		return AUDIO_OUT2_ERROR_INVALID_PARAM;
	}
	if (std::any_of(g_audioout2_ports.begin(), g_audioout2_ports.end(),
	                [handle](const auto& port) { return port.used && port.user == handle; })) {
		return AUDIO_OUT2_ERROR_BUSY;
	}
	g_audioout2_users.erase(it);
	return OK;
}

int KYTY_SYSV_ABI AudioOut2UserGetSupportedAttributes(AudioOut2UserHandle handle,
                                                      uint32_t*           context_attributes,
                                                      uint32_t*           port_attributes) {
	if (context_attributes == nullptr || port_attributes == nullptr) {
		return AUDIO_OUT2_ERROR_INVALID_PARAM;
	}
	Common::LockGuard lock(g_audioout2_port_mutex);
	if (std::find(g_audioout2_users.begin(), g_audioout2_users.end(), handle) ==
	    g_audioout2_users.end()) {
		return AUDIO_OUT2_ERROR_INVALID_PARAM;
	}
	*context_attributes = 0;
	*port_attributes    = (1u << AUDIO_OUT2_PORT_ATTRIBUTE_ID_PCM) |
	                      (1u << AUDIO_OUT2_PORT_ATTRIBUTE_ID_GAIN) |
	                      (1u << AUDIO_OUT2_PORT_ATTRIBUTE_ID_AMBISONICS);
	return OK;
}

size_t KYTY_SYSV_ABI AudioOut2GetSpeakerArrayMemorySize(uint32_t num_speakers, uint8_t is_3d,
                                                        uint8_t is_ambisonics) {
	PRINT_NAME();

	const auto speakers = std::clamp<uint32_t>(num_speakers, 1, 32);
	const auto size = static_cast<size_t>(0x400 + speakers * (is_ambisonics != 0 ? 0x100 : 0x40) +
	                                      (is_3d != 0 ? 0x200 : 0));

	LOGF("\t num_speakers  = %" PRIu32 "\n"
	     "\t is_3d         = %" PRIu8 "\n"
	     "\t is_ambisonics = %" PRIu8 "\n"
	     "\t memory_size   = 0x%016" PRIx64 "\n",
	     num_speakers, is_3d, is_ambisonics, static_cast<uint64_t>(size));

	return size;
}

int KYTY_SYSV_ABI AudioOut2SpeakerArrayCreate(AudioOut2SpeakerArrayHandle* handle,
                                              const void* vbap_params, const void* ambi_params) {
	PRINT_NAME();
	EXIT_NOT_IMPLEMENTED(handle == nullptr);

	*handle = nullptr;

	g_audioout2_speaker_array_mutex.Lock();
	for (auto& state: g_audioout2_speaker_arrays) {
		if (!state.used) {
			state              = AudioOut2SpeakerArrayState {};
			state.used         = true;
			state.handle       = &state;
			state.num_speakers = 2;
			*handle            = state.handle;
			break;
		}
	}
	g_audioout2_speaker_array_mutex.Unlock();

	LOGF("\t handle      = 0x%016" PRIx64 "\n"
	     "\t vbap_params = 0x%016" PRIx64 "\n"
	     "\t ambi_params = 0x%016" PRIx64 "\n",
	     reinterpret_cast<uint64_t>(*handle), reinterpret_cast<uint64_t>(vbap_params),
	     reinterpret_cast<uint64_t>(ambi_params));

	return (*handle != nullptr ? OK : AUDIO_OUT2_ERROR_PORT_FULL);
}

int KYTY_SYSV_ABI AudioOut2SpeakerArrayDestroy(AudioOut2SpeakerArrayHandle handle) {
	PRINT_NAME();
	LOGF("\t handle = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(handle));

	g_audioout2_speaker_array_mutex.Lock();
	for (auto& state: g_audioout2_speaker_arrays) {
		if (state.used && state.handle == handle) {
			state = AudioOut2SpeakerArrayState {};
			break;
		}
	}
	g_audioout2_speaker_array_mutex.Unlock();

	return OK;
}

int KYTY_SYSV_ABI AudioOut2GetSpeakerArrayCoefficients(
    AudioOut2SpeakerArrayHandle handle, AudioOut2Position pos, float spread, float* coefficients,
    uint32_t num_coefficients, uint8_t height_aware, float downmix_spread_radius) {
	PRINT_NAME();
	EXIT_NOT_IMPLEMENTED(coefficients == nullptr && num_coefficients != 0);

	if (coefficients != nullptr) {
		std::fill(coefficients, coefficients + num_coefficients, 0.0f);
		if (num_coefficients > 0) {
			coefficients[0] = 1.0f;
		}
		if (num_coefficients > 1) {
			coefficients[1] = 1.0f;
		}
	}

	LOGF("\t handle = 0x%016" PRIx64 ", coeffs = %" PRIu32
	     ", pos = (%f, %f, %f), spread = %f, height = %" PRIu8 ", downmix = %f\n",
	     reinterpret_cast<uint64_t>(handle), num_coefficients, static_cast<double>(pos.x),
	     static_cast<double>(pos.y), static_cast<double>(pos.z), static_cast<double>(spread),
	     height_aware, static_cast<double>(downmix_spread_radius));

	return OK;
}

int KYTY_SYSV_ABI AudioOut2GetSpeakerArrayAmbisonicsCoefficients(AudioOut2SpeakerArrayHandle handle,
                                                                 uint32_t ambisonics_channel,
                                                                 float*   coefficients,
                                                                 uint32_t num_coefficients) {
	PRINT_NAME();
	EXIT_NOT_IMPLEMENTED(coefficients == nullptr && num_coefficients != 0);

	if (coefficients != nullptr) {
		std::fill(coefficients, coefficients + num_coefficients, 0.0f);
		if (num_coefficients > 0) {
			coefficients[0] =
			    (ambisonics_channel == 0 || ambisonics_channel == 64 ? 0.70710677f : 1.0f);
		}
	}

	LOGF("\t handle = 0x%016" PRIx64 ", channel = %" PRIu32 ", coeffs = %" PRIu32 "\n",
	     reinterpret_cast<uint64_t>(handle), ambisonics_channel, num_coefficients);

	return OK;
}

int KYTY_SYSV_ABI AudioOut2GetSpeakerInfo(AudioOut2SpeakerInfo* info, uint32_t flags) {
	EXIT_NOT_IMPLEMENTED(info == nullptr);
	std::memset(info, 0, sizeof(AudioOut2SpeakerInfo));
	info->type             = 0;
	info->available_bits   = 0x03;
	info->flags            = 0;
	info->speaker_angle[0] = {-30, 0};
	info->speaker_angle[1] = {30, 0};

	return OK;
}

int KYTY_SYSV_ABI AudioOut2SetSystemDebugState(const AudioOut2SystemDebugStateParam* param) {
	PRINT_NAME();
	LOGF("\t param = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(param));
	return OK;
}

int KYTY_SYSV_ABI AudioOut2Set3DLatency(uint32_t user_id, uint32_t output, uint32_t latency_us) {
	// Not sure

	PRINT_NAME();
	LOGF("\t user_id = %" PRIu32 ", output = %" PRIu32 ", latency_us = %" PRIu32 "\n", user_id,
	     output, latency_us);

	if (output > AUDIO_OUT2_MASTERING_OUTPUT_RECORDING) {
		return AUDIO_OUT2_ERROR_INVALID_PARAM;
	}

	g_audioout2_latency_mutex.Lock();
	AudioOut2LatencyState* free_state = nullptr;
	for (auto& state: g_audioout2_latencies) {
		if (state.used && state.user_id == user_id && state.output == output) {
			state.latency_us = latency_us;
			g_audioout2_latency_mutex.Unlock();
			return OK;
		}
		if (!state.used && free_state == nullptr) {
			free_state = &state;
		}
	}

	if (free_state != nullptr) {
		*free_state = AudioOut2LatencyState {true, user_id, output, latency_us};
	}
	g_audioout2_latency_mutex.Unlock();

	return (free_state != nullptr ? OK : AUDIO_OUT2_ERROR_PORT_FULL);
}

int KYTY_SYSV_ABI AudioOut2MasteringInit(uint32_t flags) {
	PRINT_NAME();
	LOGF("\t flags = 0x%08" PRIx32 "\n", flags);
	return OK;
}

int KYTY_SYSV_ABI AudioOut2MasteringSetParam(const AudioOut2MasteringParamsHeader* param,
                                             uint32_t output, uint32_t flags) {
	PRINT_NAME();
	LOGF("\t param = 0x%016" PRIx64 ", output = %" PRIu32 ", flags = 0x%08" PRIx32 "\n",
	     reinterpret_cast<uint64_t>(param), output, flags);
	return OK;
}

int KYTY_SYSV_ABI AudioOut2MasteringGetState(AudioOut2MasteringStatesHeader* state, uint32_t output,
                                             AudioOut2UserHandle user) {
	PRINT_NAME();
	LOGF("\t state = 0x%016" PRIx64 ", output = %" PRIu32 ", user = 0x%016" PRIx64 "\n",
	     reinterpret_cast<uint64_t>(state), output, static_cast<uint64_t>(user));

	if (state == nullptr) {
		return AUDIO_OUT2_ERROR_MASTERING_INVALID_API_PARAM;
	}

	const auto states_id = state->states_id;
	switch (states_id) {
		case AUDIO_OUT2_MASTERING_STATES_ID_DEFAULT: {
			auto* full_state = reinterpret_cast<AudioOut2MasteringStates*>(state);
			std::memset(full_state, 0, sizeof(AudioOut2MasteringStates));
			full_state->states_header.states_id = AUDIO_OUT2_MASTERING_STATES_ID_DEFAULT;
			full_state->compressor_states.descriptor.id =
			    AUDIO_OUT2_MASTERING_STATES_STRUCT_ID_COMPRESSOR_DEFAULT;
			full_state->compressor_states.descriptor.size =
			    sizeof(AudioOut2MasteringCompressorStates);
			full_state->limiter_states.descriptor.id =
			    AUDIO_OUT2_MASTERING_STATES_STRUCT_ID_LIMITER_DEFAULT;
			full_state->limiter_states.descriptor.size = sizeof(AudioOut2MasteringLimiterStates);
			return OK;
		}
		case AUDIO_OUT2_MASTERING_STATES_ID_V2: {
			auto* full_state = reinterpret_cast<AudioOut2MasteringStatesV2*>(state);
			std::memset(full_state, 0, sizeof(AudioOut2MasteringStatesV2));
			full_state->states_header.states_id = AUDIO_OUT2_MASTERING_STATES_ID_V2;
			full_state->compressor_states.descriptor.id =
			    AUDIO_OUT2_MASTERING_STATES_STRUCT_ID_COMPRESSOR_V2;
			full_state->compressor_states.descriptor.size =
			    sizeof(AudioOut2MasteringCompressorStatesV2);
			full_state->limiter_states.descriptor.id =
			    AUDIO_OUT2_MASTERING_STATES_STRUCT_ID_LIMITER_DEFAULT;
			full_state->limiter_states.descriptor.size = sizeof(AudioOut2MasteringLimiterStates);
			return OK;
		}
		default: return AUDIO_OUT2_ERROR_MASTERING_INVALID_STATES_ID;
	}
}

int KYTY_SYSV_ABI AudioOut2MasteringTerm() {
	PRINT_NAME();
	return OK;
}

} // namespace AudioOut2
} // namespace Libs::Audio
