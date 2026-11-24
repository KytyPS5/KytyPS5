#include "common/assert.h"
#include "common/common.h"
#include "common/logging/log.h"
#include "common/threads.h"
#include "kernel/pthread.h"
#include "libs/audio.h"
#include "libs/audio_internal.h"
#include "libs/errno.h"
#include "libs/libs.h"
#include "libs/spatialRenderer.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <numbers>
#include <span>
#include <utility>
#include <vector>

namespace Libs::Audio {

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

static constexpr uint32_t AUDIOOUT2_CHANNEL_PORTS_MAX = 32;
static constexpr uint32_t AUDIOOUT2_OBJECT_PORTS_MAX  = 512;
static constexpr uint32_t AUDIOOUT2_PORT_SLOT_BITS    = 10;
static constexpr uint32_t AUDIOOUT2_CONTEXT_SLOT_BITS = 4;

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
	SpatialAttributes      spatial;
	bool                   attributes_locked = false;
	std::vector<uint8_t>   pcm_data;
};

struct AudioOut2ContextState {
	bool                   used                = false;
	AudioOut2ContextHandle handle              = 0;
	uint32_t               queue_depth         = 4;
	uint32_t               queued              = 0;
	uint32_t               reserved            = 0;
	uint32_t               num_grains          = 512;
	uint64_t               last_update         = 0;
	std::shared_ptr<SpatialRenderer> renderer;
	uint32_t               max_ports           = 0;
	// Channel slots precede object slots; each partition has its own creation limit.
	std::vector<AudioOut2PortStateEntry> ports;
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

// Per-context copy of what a push hands the backend. The backend can sleep while pacing, so a push
// holds only its context's mutex across it; closing the context's handles waits on that mutex.
struct AudioOut2Submission {
	Common::Mutex                                                   mutex;
	std::array<std::vector<uint8_t>, AudioInternal::OUT_PORTS_MAX>  pcm;
	std::array<std::array<float, 16>, AudioInternal::OUT_PORTS_MAX> gains;
	std::vector<float>                                              object_mix;
	std::vector<SpatialObject>                                      objects;
};

static Common::Mutex                              g_audioout2_state_mutex;
static std::array<AudioOut2ContextState, 16>      g_audioout2_contexts;
static std::array<AudioOut2Submission, 16>        g_audioout2_submissions;
static std::vector<AudioOut2UserHandle>           g_audioout2_users;
static Common::Mutex                              g_audioout2_speaker_array_mutex;
static std::array<AudioOut2SpeakerArrayState, 32> g_audioout2_speaker_arrays;
static Common::Mutex                              g_audioout2_latency_mutex;
static std::array<AudioOut2LatencyState, 16>      g_audioout2_latencies;

static_assert(AUDIOOUT2_CHANNEL_PORTS_MAX + AUDIOOUT2_OBJECT_PORTS_MAX <=
              (1u << AUDIOOUT2_PORT_SLOT_BITS));
static_assert(g_audioout2_contexts.size() == (1u << AUDIOOUT2_CONTEXT_SLOT_BITS));

static constexpr int AUDIO_OUT2_ERROR_NOT_READY                   = -2144960504; /* 0x80268008 */
static constexpr int AUDIO_OUT2_ERROR_PORT_FULL                   = -2144960494; /* 0x80268012 */
static constexpr int AUDIO_OUT2_ERROR_INVALID_PARAM               = -2144960511; /* 0x80268001 */
static constexpr int AUDIO_OUT2_ERROR_BUSY                        = -2144960505; /* 0x80268007 */
static constexpr int AUDIO_OUT2_ERROR_MASTERING_INVALID_API_PARAM = -2144959999; /* 0x80268201 */
static constexpr int AUDIO_OUT2_ERROR_MASTERING_INVALID_STATES_ID = -2144959996; /* 0x80268204 */
static constexpr int AUDIO_OUT2_ERROR_OUT_OF_RESOURCE             = -2144960510; /* 0x80268002 */
static constexpr int AUDIO_OUT2_ERROR_INVALID_PORT                = -2144960503; /* 0x80268009 */
static constexpr int AUDIO_OUT2_ERROR_INVALID_POINTER             = -2144960500; /* 0x8026800C */
static constexpr int AUDIO_OUT2_ERROR_INVALID_USER                = -2144960496; /* 0x80268010 */
static constexpr uint32_t AUDIO_OUT2_PORT_ATTRIBUTE_ID_PCM        = 0;
static constexpr uint32_t AUDIO_OUT2_PORT_ATTRIBUTE_ID_GAIN       = 1;
static constexpr uint32_t AUDIO_OUT2_PORT_ATTRIBUTE_ID_POSITION   = 3;
static constexpr uint32_t AUDIO_OUT2_PORT_ATTRIBUTE_ID_SPREAD     = 4;
static constexpr uint32_t AUDIO_OUT2_PORT_ATTRIBUTE_ID_AMBISONICS = 8;
static constexpr uint32_t AUDIO_OUT2_MASTERING_OUTPUT_RECORDING   = 2;
static constexpr uint32_t AUDIO_OUT2_MASTERING_STATES_ID_DEFAULT  = 1;
static constexpr uint32_t AUDIO_OUT2_MASTERING_STATES_ID_V2       = 2;
static constexpr uint32_t AUDIO_OUT2_MASTERING_STATES_STRUCT_ID_COMPRESSOR_DEFAULT = 0x01010001;
static constexpr uint32_t AUDIO_OUT2_MASTERING_STATES_STRUCT_ID_COMPRESSOR_V2      = 0x01020001;
static constexpr uint32_t AUDIO_OUT2_MASTERING_STATES_STRUCT_ID_LIMITER_DEFAULT    = 0x01010003;
static constexpr uint32_t AUDIO_OUT2_GET_SPEAKER_INFO_FLAG_FOLLOW_HEADPHONE_CONNECT = 1;

static AudioOut2ContextState* audioout2_find_context_locked(AudioOut2ContextHandle ctx) {
	for (auto& state: g_audioout2_contexts) {
		if (state.used && state.handle == ctx) {
			return &state;
		}
	}

	return nullptr;
}

static AudioOut2Submission& audioout2_submission(const AudioOut2ContextState& context) {
	return g_audioout2_submissions[&context - g_audioout2_contexts.data()];
}

// Ticks of a sixth of a microsecond: one 48 kHz sample lasts exactly 125 of them.
static uint64_t audioout2_clock() {
	return LibKernel::KernelGetProcessTime() * 6;
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

	const auto now = audioout2_clock();
	if (state->last_update == 0 || state->queued == 0) {
		state->last_update = now;
		return;
	}
	if (now <= state->last_update) {
		return;
	}

	const auto grain   = uint64_t {state->num_grains} * 125;
	const auto drained = std::min<uint64_t>(state->queued, (now - state->last_update) / grain);
	if (drained == 0) {
		return;
	}

	state->queued -= static_cast<uint32_t>(drained);
	state->last_update += drained * grain;
	if (state->queued == 0) {
		state->last_update = now;
	}
}

static AudioOut2PortStateEntry* audioout2_find_port_locked(AudioOut2PortHandle port) {
	const auto context_slot =
	    (port >> AUDIOOUT2_PORT_SLOT_BITS) & ((1u << AUDIOOUT2_CONTEXT_SLOT_BITS) - 1u);
	const auto port_slot = port & ((1u << AUDIOOUT2_PORT_SLOT_BITS) - 1u);
	auto&      context   = g_audioout2_contexts[context_slot];
	if (context.used && port_slot < context.ports.size()) {
		auto& state = context.ports[port_slot];
		if (state.used && state.handle == port) return &state;
	}

	return nullptr;
}

static size_t audioout2_pcm_size(const AudioOut2PortStateEntry& state) {
	const auto bytes_per_sample =
	    (state.data_format & 0x7fu) == 1 ? sizeof(int16_t) : sizeof(float);
	return static_cast<size_t>(state.samples_num) *
	       audioout2_data_format_channels(state.data_format) * bytes_per_sample;
}

static bool audioout2_context_has_queueable_device(const AudioOut2ContextState& context) {
	for (const auto& state: context.ports) {
		if (state.used && !state.pcm_data.empty()) {
			const int handle = audioout2_port_type_is_object(state.port_type)
			                       ? context.renderer->OutputHandle()
			                       : state.audio_handle;
			if (AudioInternal::AudioOutHasDevice(handle)) {
				return true;
			}
		}
	}
	return false;
}

static int audioout2_queue_context_audio(AudioOut2ContextHandle ctx, AudioOut2Submission& out,
                                          bool blocking) {
	Common::LockGuard submit_lock(out.mutex);
	g_audioout2_state_mutex.Lock();
	auto* context = audioout2_find_context_locked(ctx);
	if (context == nullptr) {
		g_audioout2_state_mutex.Unlock();
		return OK;
	}
	const auto renderer = context->renderer;
	out.objects.resize(context->ports.size() - context->max_ports);
	std::vector<AudioInternal::OutputParam> params;
	params.reserve(AudioInternal::OUT_PORTS_MAX);
	// A port's PCM covers one grain: the push consumes it, so a port the game stops feeding
	// falls silent.
	for (size_t i = 0; i < context->ports.size(); ++i) {
		auto& state = context->ports[i];
		if (i >= context->max_ports) {
			auto& object = out.objects[i - context->max_ports];
			object.attributes = state.spatial;
			object.gain = state.gains[0];
			object.is_float = state.audio_format == AudioInternal::Format::FloatMono;
			object.pcm.swap(state.pcm_data);
		} else if (state.audio_handle > 0 && !state.pcm_data.empty()) {
			const auto j = params.size();
			out.pcm[j].swap(state.pcm_data);
			out.gains[j] = state.gains;
			params.push_back({state.audio_handle, out.pcm[j].data(), out.gains[j].data()});
		}
		state.pcm_data.clear();
	}
	g_audioout2_state_mutex.Unlock();
	if (renderer) {
		if (!renderer->Render(out.objects, out.object_mix)) return AUDIO_OUT2_ERROR_OUT_OF_RESOURCE;
		if (!out.object_mix.empty()) params.push_back({renderer->OutputHandle(), out.object_mix.data()});
	}
	if (!params.empty()) {
		(void)AudioInternal::AudioOutOutputs(params.data(), static_cast<uint32_t>(params.size()), blocking);
	}
	return OK;
}
static void audioout2_close_audio_handle(int audio_handle) {
	if (audio_handle > 0) {
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
	params->max_ports   = 8;
	params->queue_depth = 1;
	params->num_grains  = 256;

	return OK;
}

static bool audioout2_valid_context_capacity(const AudioOut2ContextParam& params) {
	return params.max_ports <= AUDIOOUT2_CHANNEL_PORTS_MAX &&
	       params.max_object_ports <= AUDIOOUT2_OBJECT_PORTS_MAX &&
	       params.guarantee_object_ports <= params.max_object_ports;
}

int KYTY_SYSV_ABI AudioOut2ContextQueryMemory(const AudioOut2ContextParam* params,
                                              size_t*                      memory_size) {
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(params == nullptr);
	EXIT_NOT_IMPLEMENTED(memory_size == nullptr);
	if (!audioout2_valid_context_capacity(*params)) {
		return AUDIO_OUT2_ERROR_INVALID_PARAM;
	}

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
	if (!audioout2_valid_context_capacity(*params)) {
		return AUDIO_OUT2_ERROR_INVALID_PARAM;
	}

	const auto num_grains = params->num_grains == 0 ? 512u : params->num_grains;
	auto renderer = params->max_object_ports ? SpatialRenderer::Open(num_grains, params->max_object_ports) : nullptr;
	if (params->max_object_ports && !renderer) return AUDIO_OUT2_ERROR_OUT_OF_RESOURCE;

	g_audioout2_state_mutex.Lock();
	auto it = std::find_if(g_audioout2_contexts.begin(), g_audioout2_contexts.end(),
	                       [](const auto& state) { return !state.used; });
	if (it == g_audioout2_contexts.end()) {
		g_audioout2_state_mutex.Unlock();
		return AUDIO_OUT2_ERROR_OUT_OF_RESOURCE;
	}
	*ctx                       = g_audioout2_next_context.fetch_add(1, std::memory_order_relaxed);
	auto* state                = &*it;
	*state                     = AudioOut2ContextState {};
	state->used                = true;
	state->handle              = *ctx;
	state->queue_depth         = params->queue_depth == 0 ? 4u : params->queue_depth;
	state->num_grains          = num_grains;
	state->last_update         = audioout2_clock();
	state->renderer            = std::move(renderer);
	state->max_ports           = params->max_ports;
	state->ports.resize(params->max_ports + params->max_object_ports);
	g_audioout2_state_mutex.Unlock();

	LOGF("\t buffer      = 0x%016" PRIx64 "\n"
	     "\t buffer_size = 0x%016" PRIx64 "\n"
	     "\t ctx         = 0x%016" PRIx64 "\n"
	     "\t queue_depth = %" PRIu32 ", num_grains = %" PRIu32 "\n",
	     reinterpret_cast<uint64_t>(buffer), static_cast<uint64_t>(buffer_size), *ctx,
	     (params->queue_depth == 0 ? 4u : params->queue_depth), num_grains);

	return OK;
}
int KYTY_SYSV_ABI AudioOut2ContextDestroy(AudioOut2ContextHandle ctx) {
	PRINT_NAME();
	LOGF("\t ctx = 0x%016" PRIx64 "\n", ctx);

	std::array<int, AUDIOOUT2_CHANNEL_PORTS_MAX + 1> audio_handles {};
	size_t                                           audio_handles_num = 0;
	std::shared_ptr<SpatialRenderer> renderer;

	g_audioout2_state_mutex.Lock();
	auto* state = audioout2_find_context_locked(ctx);
	if (state != nullptr) {
		renderer = std::move(state->renderer);
		for (const auto& port_state: state->ports) {
			if (port_state.used && port_state.audio_handle > 0) {
				audio_handles[audio_handles_num++] = port_state.audio_handle;
			}
		}
		*state = AudioOut2ContextState {};
	}
	g_audioout2_state_mutex.Unlock();

	if (state != nullptr) {
		// Wait for an in-flight push that may still use the handles.
		Common::LockGuard wait(audioout2_submission(*state).mutex);
	}
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

// Advance reserves a grain. A full queue changes nothing and keeps the port attributes locked.
int KYTY_SYSV_ABI AudioOut2ContextAdvance(AudioOut2ContextHandle ctx) {
	Common::LockGuard lock(g_audioout2_state_mutex);
	auto*             state = audioout2_find_context_locked(ctx);
	if (state == nullptr) {
		return AUDIO_OUT2_ERROR_INVALID_PARAM;
	}
	audioout2_update_context_locked(state);
	if (state->queued + state->reserved >= state->queue_depth) {
		return AUDIO_OUT2_ERROR_NOT_READY;
	}
	state->reserved++;
	for (auto& port: state->ports) {
		port.attributes_locked = false;
	}
	return OK;
}

int KYTY_SYSV_ABI AudioOut2ContextPush(AudioOut2ContextHandle ctx, uint32_t blocking) {
	if (blocking > 1) {
		return AUDIO_OUT2_ERROR_INVALID_PARAM;
	}
	g_audioout2_state_mutex.Lock();
	auto* state = audioout2_find_context_locked(ctx);
	if (state == nullptr) {
		g_audioout2_state_mutex.Unlock();
		return AUDIO_OUT2_ERROR_INVALID_PARAM;
	}
	audioout2_update_context_locked(state);
	// A push commits the reserved grains with the ports' PCM; without a reservation the PCM waits
	// for the next grain. A synchronous push returns with a free queue entry. The SDL queue paces
	// one carrying PCM to a real device, so the model keeps an entry free for it.
	const auto reserved = std::exchange(state->reserved, 0u);
	if (reserved != 0) {
		const bool device_paced = blocking != 0 && audioout2_context_has_queueable_device(*state);
		state->queued =
		    std::min(state->queued + reserved, state->queue_depth - (device_paced ? 1u : 0u));
	}
	auto&      submission   = audioout2_submission(*state);
	const auto sleep_micros = audioout2_grain_micros(state->num_grains);
	g_audioout2_state_mutex.Unlock();
	if (reserved != 0) {
		const int result = audioout2_queue_context_audio(ctx, submission, blocking != 0);
		if (result != OK) return result;
	}

	while (blocking != 0) {
		g_audioout2_state_mutex.Lock();
		state = audioout2_find_context_locked(ctx);
		audioout2_update_context_locked(state);
		const bool full = state != nullptr && state->queued >= state->queue_depth;
		g_audioout2_state_mutex.Unlock();
		if (!full) {
			break;
		}
		Common::Thread::SleepMicro(sleep_micros);
	}
	return OK;
}

int KYTY_SYSV_ABI AudioOut2ContextGetQueueLevel(AudioOut2ContextHandle ctx, uint32_t* queue_level,
                                                uint32_t* available_queues) {
	if (queue_level == nullptr && available_queues == nullptr) {
		return AUDIO_OUT2_ERROR_INVALID_PARAM;
	}
	Common::LockGuard lock(g_audioout2_state_mutex);
	auto*             state = audioout2_find_context_locked(ctx);
	if (state == nullptr) {
		return AUDIO_OUT2_ERROR_INVALID_PARAM;
	}
	audioout2_update_context_locked(state);
	const auto level = state->queued + state->reserved;
	if (queue_level != nullptr) {
		*queue_level = level;
	}
	if (available_queues != nullptr) {
		*available_queues = state->queue_depth - level;
	}
	return level < state->queue_depth ? OK : AUDIO_OUT2_ERROR_NOT_READY;
}

// PPSA28180 calls this with rsp off the 16-byte alignment; the host device open needs it.
[[gnu::force_align_arg_pointer]] int KYTY_SYSV_ABI AudioOut2PortCreate(
    AudioOut2ContextHandle ctx, const AudioOut2PortParam* params, AudioOut2PortHandle* port) {
	EXIT_NOT_IMPLEMENTED(params == nullptr);
	EXIT_NOT_IMPLEMENTED(port == nullptr);

	const auto audio_format = audioout2_data_format_to_audio_format(params->data_format);
	const auto audio_type   = audioout2_port_type_to_audio_out_type(params->port_type);
	const bool is_object    = audioout2_port_type_is_object(params->port_type);

	g_audioout2_state_mutex.Lock();
	auto* context_state = audioout2_find_context_locked(ctx);
	if (context_state == nullptr) {
		g_audioout2_state_mutex.Unlock();
		return AUDIO_OUT2_ERROR_INVALID_PARAM;
	}
	if (is_object && (params->port_type != 0x100 || params->sampling_freq != 48000 ||
	                  (audio_format != AudioInternal::Format::FloatMono &&
	                   audio_format != AudioInternal::Format::Signed16bitMono))) {
		g_audioout2_state_mutex.Unlock();
		return AUDIO_OUT2_ERROR_INVALID_PARAM;
	}
	const auto samples_num = context_state->num_grains;

	if (std::find(g_audioout2_users.begin(), g_audioout2_users.end(), params->user_handle) ==
	    g_audioout2_users.end()) {
		g_audioout2_state_mutex.Unlock();
		return AUDIO_OUT2_ERROR_INVALID_PARAM;
	}
	auto slots = std::span(context_state->ports);
	slots =
	    is_object ? slots.subspan(context_state->max_ports) : slots.first(context_state->max_ports);
	const auto available = std::find_if(slots.begin(), slots.end(),
	                                    [](const auto& candidate) { return !candidate.used; });
	if (available == slots.end()) {
		g_audioout2_state_mutex.Unlock();
		return AUDIO_OUT2_ERROR_PORT_FULL;
	}
	const auto serial       = g_audioout2_next_port.fetch_add(1, std::memory_order_relaxed);
	const auto context_slot = static_cast<uint64_t>(context_state - g_audioout2_contexts.data());
	const auto port_slot    = static_cast<uint64_t>(&*available - context_state->ports.data());
	const auto next_port    = (serial << (AUDIOOUT2_PORT_SLOT_BITS + AUDIOOUT2_CONTEXT_SLOT_BITS)) |
	                          (context_slot << AUDIOOUT2_PORT_SLOT_BITS) | port_slot;
	auto*      port_state   = &*available;
	*port_state             = AudioOut2PortStateEntry {};
	port_state->used        = true;
	port_state->handle      = next_port;
	port_state->context     = ctx;
	port_state->user        = params->user_handle;
	port_state->port_type   = params->port_type;
	port_state->data_format = params->data_format;
	port_state->sampling_freq = params->sampling_freq;
	port_state->samples_num   = samples_num;
	port_state->audio_format  = audio_format;
	port_state->gains.fill(1.0f);
	g_audioout2_state_mutex.Unlock();

	int audio_handle = 0;

	if (audio_format != AudioInternal::Format::Unknown && !is_object) {
		audio_handle = AudioInternal::AudioOutOpen(audio_type, samples_num, params->sampling_freq,
		                                           audio_format);
	}

	g_audioout2_state_mutex.Lock();
	// Context destruction may have freed or replaced the slots while the backend opened.
	port_state          = audioout2_find_port_locked(next_port);
	const bool reserved = port_state != nullptr;
	if (reserved) {
		port_state->audio_handle = audio_handle;
	}
	g_audioout2_state_mutex.Unlock();
	if (!reserved) {
		audioout2_close_audio_handle(audio_handle);
		return AUDIO_OUT2_ERROR_INVALID_PARAM;
	}

	*port = next_port;

	if (serial <= 16 || (serial % 600) == 0) {
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

	int                          audio_handle = 0;
	const AudioOut2ContextState* context      = nullptr;
	g_audioout2_state_mutex.Lock();
	if (auto* state = audioout2_find_port_locked(port); state != nullptr) {
		audio_handle = state->audio_handle;
		context      = audioout2_find_context_locked(state->context);
		*state       = AudioOut2PortStateEntry {};
	}
	g_audioout2_state_mutex.Unlock();

	if (audio_handle > 0 && context != nullptr) {
		// Wait for an in-flight push that may still use the handle.
		Common::LockGuard wait(audioout2_submission(*context).mutex);
	}
	audioout2_close_audio_handle(audio_handle);

	return OK;
}

int KYTY_SYSV_ABI AudioOut2PortSetAttributes(AudioOut2PortHandle       port,
                                             const AudioOut2Attribute* attributes, uint32_t num) {
	if (num != 0 && attributes == nullptr) {
		return AUDIO_OUT2_ERROR_INVALID_PARAM;
	}
	Common::LockGuard lock(g_audioout2_state_mutex);
	auto*             state = audioout2_find_port_locked(port);
	if (state == nullptr) {
		return AUDIO_OUT2_ERROR_INVALID_PORT;
	}

	const void* pcm_data   = nullptr;
	auto        gains      = state->gains;
	auto        spatial    = state->spatial;
	uint32_t    seen       = 0;
	for (uint32_t i = 0; i < num; i++) {
		const auto& attribute = attributes[i];
		const auto  id        = attribute.attribute_id;
		if (id != AUDIO_OUT2_PORT_ATTRIBUTE_ID_PCM && id != AUDIO_OUT2_PORT_ATTRIBUTE_ID_GAIN &&
		    id != AUDIO_OUT2_PORT_ATTRIBUTE_ID_POSITION && id != AUDIO_OUT2_PORT_ATTRIBUTE_ID_SPREAD &&
		    id != AUDIO_OUT2_PORT_ATTRIBUTE_ID_AMBISONICS) {
			continue;
		}
		if ((seen & (1u << id)) != 0) {
			return AUDIO_OUT2_ERROR_INVALID_PARAM;
		}
		seen |= 1u << id;
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
				const auto channels = audioout2_data_format_channels(state->data_format);
				if (attribute.value_size != channels * sizeof(float)) {
					return AUDIO_OUT2_ERROR_INVALID_PARAM;
				}
				std::memcpy(gains.data(), attribute.value, attribute.value_size);
				for (uint32_t ch = 0; ch < channels; ch++) {
					if (!std::isfinite(gains[ch]) || gains[ch] < 0.0f) {
						return AUDIO_OUT2_ERROR_INVALID_PARAM;
					}
				}
				break;
			}
			case AUDIO_OUT2_PORT_ATTRIBUTE_ID_POSITION:
				if (!audioout2_port_type_is_object(state->port_type) || attribute.value_size != sizeof(spatial.position))
					return AUDIO_OUT2_ERROR_INVALID_PARAM;
				std::memcpy(&spatial.position, attribute.value, sizeof(spatial.position));
				if (!spatial.position.IsFinite()) return AUDIO_OUT2_ERROR_INVALID_PARAM;
				break;
			case AUDIO_OUT2_PORT_ATTRIBUTE_ID_SPREAD:
				if (!audioout2_port_type_is_object(state->port_type) || attribute.value_size != sizeof(spatial.spread))
					return AUDIO_OUT2_ERROR_INVALID_PARAM;
				std::memcpy(&spatial.spread, attribute.value, sizeof(spatial.spread));
				if (!std::isfinite(spatial.spread) || spatial.spread < 0) return AUDIO_OUT2_ERROR_INVALID_PARAM;
				spatial.spread = std::min(spatial.spread, 2 * std::numbers::pi_v<float>);
				break;
			case AUDIO_OUT2_PORT_ATTRIBUTE_ID_AMBISONICS:
				if (!audioout2_port_type_is_object(state->port_type) || attribute.value_size != sizeof(spatial.ambisonics))
					return AUDIO_OUT2_ERROR_INVALID_PARAM;
				std::memcpy(&spatial.ambisonics, attribute.value, sizeof(spatial.ambisonics));
				if (!SpatialAttributes::ValidAmbisonics(spatial.ambisonics)) return AUDIO_OUT2_ERROR_INVALID_PARAM;
				break;
		}
	}

	state->gains      = gains;
	state->spatial    = spatial;
	if ((seen & (1u << AUDIO_OUT2_PORT_ATTRIBUTE_ID_PCM)) != 0) {
		if (pcm_data != nullptr && state->audio_format != AudioInternal::Format::Unknown) {
			const auto* bytes = static_cast<const uint8_t*>(pcm_data);
			state->pcm_data.assign(bytes, bytes + audioout2_pcm_size(*state));
		} else {
			state->pcm_data.clear();
		}
		state->attributes_locked = audioout2_port_type_is_object(state->port_type);
	}
	return OK;
}
int KYTY_SYSV_ABI AudioOut2PortGetState(AudioOut2PortHandle port, AudioOut2PortState* state) {
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(state == nullptr);

	std::memset(state, 0, sizeof(AudioOut2PortState));
	state->output          = 1;
	state->num_channels    = 2;
	state->volume          = -1;
	state->reroute_counter = 0;
	state->flags           = 0;

	g_audioout2_state_mutex.Lock();
	if (auto* port_state = audioout2_find_port_locked(port); port_state != nullptr) {
		const auto type     = audioout2_port_type_to_audio_out_type(port_state->port_type);
		const auto route    = AudioInternal::GetPortOutput(type);
		const auto channels = audioout2_data_format_channels(port_state->data_format);
		state->output       = route.output;
		state->volume       = route.volume;
		auto route_channels = route.channels;
		if (Config::Audio3dEnabled() && (route.output & (1 | 0x40)) != 0) {
			if (type != AUDIO_OUT_PORT_TYPE_VOICE || route.output != 0x40) state->flags = 1;
			if (type == AUDIO_OUT_PORT_TYPE_MAIN || type == AUDIO_OUT_PORT_TYPE_BGM) route_channels = 8;
		}
		// Object ports report the route's channels, channel ports at most their own.
		state->num_channels = audioout2_port_type_is_object(port_state->port_type)
		                          ? route_channels : std::min(channels, route_channels);
	}
	g_audioout2_state_mutex.Unlock();

	// LOGF("\t port = 0x%016" PRIx64 "\n", port);
	// LOGF("\t num_channels = %" PRIu8 "\n", state->num_channels);

	return OK;
}

int KYTY_SYSV_ABI AudioOut2GetSystemState(AudioOut2SystemState* state) {
	PRINT_NAME();
	if (state == nullptr) {
		return AUDIO_OUT2_ERROR_INVALID_PARAM;
	}
	state->loudness = AUDIO_OUT_STATE_LOUDNESS_MIN;
	return OK;
}

int KYTY_SYSV_ABI AudioOut2UserCreate(uint32_t user_id, AudioOut2UserHandle* handle) {
	PRINT_NAME();
	if (handle == nullptr) {
		return AUDIO_OUT2_ERROR_INVALID_PARAM;
	}
	Common::LockGuard lock(g_audioout2_state_mutex);
	*handle = g_audioout2_next_user++;
	g_audioout2_users.push_back(*handle);
	LOGF("\t user_id = %" PRIu32 ", handle = 0x%016" PRIx64 "\n", user_id,
	     static_cast<uint64_t>(*handle));
	return OK;
}

int KYTY_SYSV_ABI AudioOut2UserDestroy(AudioOut2UserHandle handle) {
	PRINT_NAME();
	LOGF("\t handle = 0x%016" PRIx64 "\n", static_cast<uint64_t>(handle));
	Common::LockGuard lock(g_audioout2_state_mutex);
	const auto        it = std::find(g_audioout2_users.begin(), g_audioout2_users.end(), handle);
	if (it == g_audioout2_users.end()) {
		return AUDIO_OUT2_ERROR_INVALID_PARAM;
	}
	for (const auto& context: g_audioout2_contexts) {
		if (std::any_of(context.ports.begin(), context.ports.end(),
		                [handle](const auto& port) { return port.used && port.user == handle; })) {
			return AUDIO_OUT2_ERROR_BUSY;
		}
	}
	g_audioout2_users.erase(it);
	return OK;
}

int KYTY_SYSV_ABI AudioOut2UserGetSupportedAttributes(AudioOut2UserHandle handle,
                                                      uint32_t*           context_attributes,
                                                      uint32_t*           port_attributes) {
	if (context_attributes == nullptr && port_attributes == nullptr) {
		return AUDIO_OUT2_ERROR_INVALID_PARAM;
	}
	// Games compiled before 9.00 lack the later debug, height and proximity attributes.
	uint32_t sdk_version = 0;
	LibKernel::KernelGetProsperoCompiledSdkVersion(&sdk_version);
	const bool        sdk_9 = sdk_version >= 0x09000000;
	Common::LockGuard lock(g_audioout2_state_mutex);
	if (std::find(g_audioout2_users.begin(), g_audioout2_users.end(), handle) ==
	    g_audioout2_users.end()) {
		return AUDIO_OUT2_ERROR_INVALID_USER;
	}
	if (context_attributes != nullptr) {
		*context_attributes = sdk_9 ? 0x13 : 0x3;
	}
	if (port_attributes != nullptr) {
		*port_attributes = Config::Audio3dEnabled() ? 0x3f7f : 0xe53;
		if (!sdk_9) *port_attributes &= 0x7ff;
	}
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
	if (info == nullptr) {
		return AUDIO_OUT2_ERROR_INVALID_POINTER;
	}
	if (flags > AUDIO_OUT2_GET_SPEAKER_INFO_FLAG_FOLLOW_HEADPHONE_CONNECT) {
		return AUDIO_OUT2_ERROR_INVALID_PARAM;
	}
	// FL, FR, C, LFE, SL, SR, BL, BR; azimuth is counterclockwise, -360 means no angle.
	static constexpr std::array<AudioOut2SpeakerAngle, 8> ANGLES {
	    {{30, 0}, {-30, 0}, {0, 0}, {-360, -360}, {90, 0}, {-90, 0}, {135, 0}, {-135, 0}}};

	const auto output = Config::GetAudioOutput();
	const auto main   = AudioInternal::GetMainOutput();
	std::memset(info, 0, sizeof(AudioOut2SpeakerInfo));
	// Without the flag, headphones report the HDMI device.
	if (output != Config::AudioOutput::Headphones || flags != 0) {
		info->type = static_cast<uint8_t>(output);
	}
	const bool spatial = Config::Audio3dEnabled() && (output != Config::AudioOutput::Headphones || flags != 0);
	const auto channels = spatial ? 8 : main.channels;
	info->flags = spatial ? 1 : 0;
	info->available_bits = (1u << channels) - 1;
	for (uint32_t i = 0; i < std::size(info->speaker_angle); i++) {
		info->speaker_angle[i] = i < channels ? ANGLES[i] : AudioOut2SpeakerAngle {-360, -360};
	}
	if (info->type == 3 && !spatial) {
		info->speaker_angle[0] = {90, 0};
		info->speaker_angle[1] = {-90, 0};
	}
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
