#include "libs/ngs2.h"

#include "common/assert.h"
#include "common/logging/log.h"
#include "common/threads.h"
#include "libs/ajm/atrac9_decoder.h"
#include "libs/errno.h"
#include "libs/libs.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <deque>
#include <limits>
#include <magic_enum.hpp>
#include <memory>
#include <vector>

#include "libatrac9.h"

namespace Libs::Audio {

namespace Ngs2 {

LIB_NAME("Ngs2", "Ngs2");

constexpr int32_t NGS2_ERROR_INVALID_OUT_ADDRESS =
    static_cast<int32_t>(0x804a8010u);
constexpr int32_t NGS2_ERROR_INVALID_WAVEFORM_DATA =
    static_cast<int32_t>(0x804a8430u);
constexpr int32_t NGS2_ERROR_INVALID_WAVEFORM_FORMAT =
    static_cast<int32_t>(0x804a8431u);
constexpr int32_t NGS2_ERROR_UNKNOWN_WAVEFORM_FORMAT =
    static_cast<int32_t>(0x804a8432u);

constexpr uint32_t NGS2_WAVEFORM_TYPE_ATRAC9 = 0x40;

struct Ngs2SystemOption {
	size_t    size                     = 0;
	char      name[64]                 = {};
	uintptr_t job_scheduler_options[4] = {};
	uint32_t  flags                    = 0;
	uint32_t  max_grain_samples        = 0;
	uint32_t  num_grain_samples        = 0;
	uint32_t  sample_rate              = 0;
	uint32_t  max_voice_channels       = 0;
	uint32_t  reserved[5]              = {};
};

struct Ngs2RackOption {
	size_t   size                   = 0;
	char     name[64]               = {};
	uint32_t flags                  = 0;
	uint32_t max_grain_samples      = 0;
	uint32_t max_voices             = 0;
	uint32_t max_input_delay_blocks = 0;
	uint32_t max_matrices           = 0;
	uint32_t max_ports              = 0;
	uint32_t max_voice_channels     = 0;
	uint32_t max_output_channels    = 0;
	uint32_t reserved[18]           = {};
};

struct Ngs2MasteringRackOption {
	Ngs2RackOption rack_option;
	uint32_t       max_channels          = 0;
	uint32_t       num_peak_meter_blocks = 0;
};

struct Ngs2SubmixerRackOption {
	Ngs2RackOption rack_option;
	uint32_t       max_channels          = 0;
	uint32_t       max_envelope_points   = 0;
	uint32_t       max_filters           = 0;
	uint32_t       max_inputs            = 0;
	uint32_t       num_peak_meter_blocks = 0;
};

struct Ngs2SamplerRackOption {
	Ngs2RackOption rack_option;
	uint32_t       max_channel_works        = 0;
	uint32_t       max_codec_caches         = 0;
	uint32_t       max_waveform_blocks      = 0;
	uint32_t       max_envelope_points      = 0;
	uint32_t       max_filters              = 0;
	uint32_t       max_atrac9_decoders      = 0;
	uint32_t       max_atrac9_channel_works = 0;
	uint32_t       max_ajm_atrac9_decoders  = 0;
	uint32_t       num_peak_meter_blocks    = 0;
};

struct Ngs2ReverbRackOption {
	Ngs2RackOption rack_option;
	uint32_t       max_channels = 0;
	uint32_t       reverb_size  = 0;
};

struct Ngs2CustomModuleOption {
	uint32_t size = 0;
};

struct Ngs2CustomRackModuleInfo {
	const Ngs2CustomModuleOption* option           = nullptr;
	uint32_t                      module_id        = 0;
	uint32_t                      source_buffer_id = 0;
	uint32_t                      extra_buffer_id  = 0;
	uint32_t                      dest_buffer_id   = 0;
	uint32_t                      state_offset     = 0;
	uint32_t                      state_size       = 0;
	uint32_t                      reserved         = 0;
	uint32_t                      reserved2        = 0;
};

struct Ngs2CustomRackPortInfo {
	uint32_t source_buffer_id = 0;
	uint32_t reserved         = 0;
};

struct Ngs2CustomRackOption {
	Ngs2RackOption           rack_option;
	uint32_t                 state_size  = 0;
	uint32_t                 num_buffers = 0;
	uint32_t                 num_modules = 0;
	uint32_t                 reserved    = 0;
	Ngs2CustomRackModuleInfo module[24];
	Ngs2CustomRackPortInfo   port[16];
};

struct Ngs2CustomSubmixerRackOption {
	Ngs2CustomRackOption custom_rack_option;
	uint32_t             max_channels = 0;
	uint32_t             max_inputs   = 0;
};

struct Ngs2CustomMasteringRackOption {
	Ngs2CustomRackOption custom_rack_option;
	uint32_t             max_channels = 0;
	uint32_t             max_inputs   = 0;
};

struct Ngs2CustomSamplerRackOption {
	Ngs2CustomRackOption custom_rack_option;
	uint32_t             max_channel_works        = 0;
	uint32_t             max_waveform_blocks      = 0;
	uint32_t             max_atrac9_decoders      = 0;
	uint32_t             max_atrac9_channel_works = 0;
	uint32_t             max_ajm_atrac9_decoders  = 0;
	uint32_t             max_codec_caches         = 0;
};

union Ngs2RackOptionUnion {
	Ngs2RackOption                common;
	Ngs2SamplerRackOption         sampler;
	Ngs2MasteringRackOption       mastering;
	Ngs2SubmixerRackOption        submixer;
	Ngs2ReverbRackOption          reverb;
	Ngs2CustomSubmixerRackOption  custom_submixer;
	Ngs2CustomMasteringRackOption custom_mastering;
	Ngs2CustomSamplerRackOption   custom_sampler;
};

struct Ngs2ContextBufferInfo {
	void*     host_buffer      = nullptr;
	size_t    host_buffer_size = 0;
	uintptr_t reserved[5]      = {};
	uintptr_t user_data        = 0;
};

struct Ngs2SystemInfo {
	char                  name[64]      = {};
	uintptr_t             system_handle = 0;
	Ngs2ContextBufferInfo buffer_info;
	uint32_t              uid               = 0;
	uint32_t              min_grain_samples = 0;
	uint32_t              max_grain_samples = 0;
	uint32_t              state_flags       = 0;
	uint32_t              rack_count        = 0;
	float                 last_render_ratio = 0.0f;
	uint64_t              last_render_tick  = 0;
	uint64_t              render_count      = 0;
	uint32_t              sample_rate       = 0;
	uint32_t              num_grain_samples = 0;
};

struct Ngs2RenderBufferInfo {
	void*    buffer        = nullptr;
	size_t   buffer_size   = 0;
	uint32_t waveform_type = 0;
	uint32_t num_channels  = 0;
};

struct Ngs2WaveformFormat {
	uint32_t waveform_type = 0;
	uint32_t num_channels  = 0;
	uint32_t sample_rate   = 0;
	uint32_t config_data   = 0;
	uint32_t frame_margin  = 0;
	uint32_t frame_offset  = 0;
};

struct Ngs2WaveformBlock {
	uintptr_t data_offset      = 0;
	size_t    data_size        = 0;
	uint32_t  num_repeats      = 0;
	uint32_t  num_skip_samples = 0;
	uint32_t  num_samples      = 0;
	uint32_t  reserved         = 0;
	uintptr_t user_data        = 0;
};

struct Ngs2WaveformInfo {
	Ngs2WaveformFormat format;
	uint32_t           data_offset              = 0;
	uint32_t           data_size                = 0;
	uint32_t           loop_begin_position      = 0;
	uint32_t           loop_end_position        = 0;
	uint32_t           num_samples              = 0;
	uint32_t           audio_unit_size          = 0;
	uint32_t           num_audio_unit_samples   = 0;
	uint32_t           num_audio_unit_per_frame = 0;
	uint32_t           audio_frame_size         = 0;
	uint32_t           num_audio_frame_samples  = 0;
	uint32_t           num_delay_samples        = 0;
	uint32_t           num_blocks               = 0;
	Ngs2WaveformBlock  blocks[4];
};

struct Ngs2PanParam {
	float angle     = 0.0f;
	float distance  = 0.0f;
	float fbw_level = 0.0f;
	float lfe_level = 0.0f;
};

struct Ngs2PanWork {
	float    speaker_angles[8] = {};
	float    unit_angle        = 0.0f;
	uint32_t num_speakers      = 0;
};

struct Ngs2GeomVector {
	float x = 0.0f;
	float y = 0.0f;
	float z = 0.0f;
};

struct Ngs2GeomCone {
	float inner_level = 0.0f;
	float inner_angle = 0.0f;
	float outer_level = 0.0f;
	float outer_angle = 0.0f;
};

struct Ngs2GeomRolloff {
	uint32_t model              = 0;
	float    max_distance       = 0.0f;
	float    rolloff_factor     = 0.0f;
	float    reference_distance = 0.0f;
};

struct Ngs2GeomListenerParam {
	Ngs2GeomVector position;
	Ngs2GeomVector orient_front;
	Ngs2GeomVector orient_up;
	Ngs2GeomVector velocity;
	float          sound_speed = 0.0f;
	uint32_t       reserved[2] = {};
};

struct Ngs2GeomListenerWork {
	float          matrix[4][4] = {};
	Ngs2GeomVector velocity;
	float          sound_speed = 0.0f;
	uint32_t       coordinate  = 0;
	uint32_t       reserved[3] = {};
};

struct Ngs2GeomSourceParam {
	Ngs2GeomVector  position;
	Ngs2GeomVector  velocity;
	Ngs2GeomVector  direction;
	Ngs2GeomCone    cone;
	Ngs2GeomRolloff rolloff;
	float           doppler_factor = 0.0f;
	float           fbw_level      = 0.0f;
	float           lfe_level      = 0.0f;
	float           max_level      = 0.0f;
	float           min_level      = 0.0f;
	float           radius         = 0.0f;
	uint32_t        num_speakers   = 0;
	uint32_t        matrix_format  = 0;
	uint32_t        reserved[2]    = {};
};

struct Ngs2GeomA3dAttribute {
	Ngs2GeomVector position;
	float          volume      = 0.0f;
	uint32_t       reserved[4] = {};
};

struct Ngs2GeomAttribute {
	float                pitch_ratio = 0.0f;
	float                level[64]   = {};
	Ngs2GeomA3dAttribute a3d_attrib;
	uint32_t             reserved[4] = {};
};

using Ngs2BufferAllocHandler = int32_t KYTY_SYSV_ABI (*)(Ngs2ContextBufferInfo*);
using Ngs2BufferFreeHandler  = int32_t  KYTY_SYSV_ABI (*)(Ngs2ContextBufferInfo*);

struct Ngs2BufferAllocator {
	Ngs2BufferAllocHandler alloc_handler = nullptr;
	Ngs2BufferFreeHandler  free_handler  = nullptr;
	uintptr_t              user_data     = 0;
};

struct Ngs2Internal {
	Ngs2SystemOption      option;
	Ngs2ContextBufferInfo buffer_info;
	Ngs2BufferAllocator   allocator;
	Ngs2Internal*         next         = nullptr;
	uint64_t              render_count = 0;
	uint32_t              uid          = 0;
	Common::Mutex         mutex;
};

enum class Ngs2RackType {
	Sampler,
	Submixer,
	Mastering,
	Reverb,
	CustomSubmixer,
	CustomMastering,
	CustomSampler,
};

struct Ngs2UserFxContext {
	void*     common;
	void*     param;
	void*     work;
	uintptr_t user_data;
	uint32_t  max_voices;
	uint32_t  voice_index;
	uint64_t  reserved[4] {};
};

struct Ngs2UserFxProcessContext {
	float**     channels;
	void*       common;
	const void* param;
	void*       work;
	void*       state;
	uintptr_t   user_data;
	uint32_t    flags;
	uint32_t    input_channels;
	uint32_t    output_channels;
	uint32_t    grain_samples;
	uint32_t    sample_rate;
	uint32_t    reserved {};
	uint64_t    reserved2[4] {};
};

struct Ngs2UserFxOption {
	Ngs2CustomModuleOption header;
	int                    KYTY_SYSV_ABI (*setup)(Ngs2UserFxContext*);
	int                    KYTY_SYSV_ABI (*cleanup)(Ngs2UserFxContext*);
	uintptr_t              control;
	int                    KYTY_SYSV_ABI (*process)(Ngs2UserFxProcessContext*);
	size_t                 common_size;
	size_t                 param_size;
	size_t                 work_size;
	uintptr_t              user_data;
};

struct Ngs2RackInternal {
	Ngs2Internal*                        ngs  = nullptr;
	Ngs2RackInternal*                    next = nullptr;
	Ngs2RackType                         type = Ngs2RackType::Sampler;
	Ngs2RackOptionUnion                  option;
	Ngs2ContextBufferInfo                buffer_info;
	Ngs2BufferAllocator                  allocator;
	std::array<std::vector<uint8_t>, 24> common;
	std::array<Ngs2UserFxOption, 24>     fx {};
};

enum class Ngs2VoicePlayState { Empty, Playing, Paused, Stopped };
enum class Ngs2VoicePlayEvent { None, Play, Pause, Resume, Stop, StopImm, Kill };

struct Ngs2VoiceInternal {
	struct Port {
		Ngs2VoiceInternal* dest   = nullptr;
		uint32_t           input  = 0;
		float              volume = 1.0f;
		int32_t            matrix = -1;
	};
	struct Block {
		const uint8_t*    data;
		Ngs2WaveformBlock info;
		uint32_t          cursor         = 0;
		size_t            data_cursor    = 0;
		uint32_t          skip_remaining = info.num_skip_samples;
	};
	struct Module {
		std::vector<uint8_t> param, work, state;
		uint32_t             flags = 1;
		bool                 control_warned = false;
		bool                 render_warned  = false;
	};
	Ngs2VoicePlayEvent              event          = Ngs2VoicePlayEvent::None;
	Ngs2VoicePlayState              state          = Ngs2VoicePlayState::Empty;
	Ngs2RackInternal*               rack           = nullptr;
	uintptr_t                       callback       = 0;
	uintptr_t                       callback_data  = 0;
	uint32_t                        callback_flags = 0;
	std::deque<Block>               blocks;
	std::vector<Port>               ports;
	std::vector<std::vector<float>> matrices;
	std::vector<Module>             modules;
	std::vector<float>              samples;
	std::vector<float>              output_matrix;
	uint32_t                        channels        = 0;
	uint32_t                        output_id       = 0;
	bool                            rendering       = false;
	bool                            rendered        = false;
	bool                            has_samples     = false;

	std::unique_ptr<Ajm::AjmAt9Decoder> decoder;
	std::vector<float>                 decoded_frame;
	bool                               accepts_blocks = true;

	void SetupSampler(const Ngs2WaveformFormat& format) {
		EXIT_NOT_IMPLEMENTED(format.sample_rate != rack->ngs->option.sample_rate ||
		                     format.frame_margin != 0 || format.frame_offset != 0);
		channels = format.num_channels;
		blocks.clear();
		decoder.reset();
		decoded_frame.clear();
		accepts_blocks = true;
		if (format.waveform_type == NGS2_WAVEFORM_TYPE_ATRAC9) {
			decoder = std::make_unique<Ajm::AjmAt9Decoder>(channels, format.sample_rate,
			                                               Ajm::AjmSampleEncoding::Float, 0);
			const std::array<uint8_t, ATRAC9_CONFIG_DATA_SIZE> config = {
			    static_cast<uint8_t>(format.config_data >> 24u),
			    static_cast<uint8_t>(format.config_data >> 16u),
			    static_cast<uint8_t>(format.config_data >> 8u),
			    static_cast<uint8_t>(format.config_data)};
			const auto result = decoder->Initialize(config.data(), config.size());
			EXIT_NOT_IMPLEMENTED(result.result != OK);
			EXIT_NOT_IMPLEMENTED(result.format.channel_num != channels ||
			                     result.format.sampling_frequency != format.sample_rate);
			Ajm::AjmSidebandDecAt9CodecInfo info {};
			decoder->WriteCodecInfo(&info, sizeof(info), result);
			EXIT_NOT_IMPLEMENTED(info.frame_samples > std::numeric_limits<uint16_t>::max());
			decoded_frame.resize(info.frame_samples * channels);
		} else {
			EXIT_NOT_IMPLEMENTED(format.waveform_type != 0x12);
		}
	}

	void SetMatrix(uint32_t index, const float* levels, uint32_t count) {
		EXIT_NOT_IMPLEMENTED(index >= matrices.size());
		matrices[index].assign(levels, levels + count);
	}
	void SetVolume(uint32_t index, float volume) {
		EXIT_NOT_IMPLEMENTED(index >= ports.size());
		ports[index].volume = volume;
	}
	void SetPortMatrix(uint32_t index, int32_t matrix) {
		EXIT_NOT_IMPLEMENTED(index >= ports.size());
		ports[index].matrix = matrix;
	}
	void SetEvent(uint32_t id) {
		switch (id) {
			case 1: event = Ngs2VoicePlayEvent::Play; break;
			case 2: event = Ngs2VoicePlayEvent::Stop; break;
			case 4: event = Ngs2VoicePlayEvent::StopImm; break;
			case 8: event = Ngs2VoicePlayEvent::Kill; break;
			case 16: event = Ngs2VoicePlayEvent::Pause; break;
			case 32: event = Ngs2VoicePlayEvent::Resume; break;
			default: EXIT("unknown event_id: 0x%08" PRIx32 "\n", id);
		}
	}
};

struct Ngs2VoiceParamHeader {
	uint16_t size;
	int16_t  next;
	uint32_t id;
};

struct Ngs2VoiceEventParam {
	Ngs2VoiceParamHeader header;
	uint32_t             event_id;
};

struct Ngs2VoicePatchParam {
	Ngs2VoiceParamHeader header;
	uint32_t             port;
	uint32_t             dest_input_id;
	uintptr_t            dest_handle;
};

struct Ngs2VoiceMatrixLevelsParam {
	Ngs2VoiceParamHeader header;
	uint32_t             matrix_id;
	uint32_t             num_levels;
	const float*         levels;
};

struct Ngs2VoicePortMatrixParam {
	Ngs2VoiceParamHeader header;
	uint32_t             port;
	int32_t              matrix_id;
};

struct Ngs2VoicePortVolumeParam {
	Ngs2VoiceParamHeader header;
	uint32_t             port;
	float                level;
};

struct Ngs2VoicePortDelayParam {
	Ngs2VoiceParamHeader header;
	uint32_t             port;
	uint32_t             num_samples;
};

struct Ngs2VoiceCallbackParam {
	Ngs2VoiceParamHeader header;
	uintptr_t            callback;
	uintptr_t            callback_data;
	uint32_t             flags;
	uint32_t             reserved;
};

struct Ngs2VoiceState {
	uint32_t state_flags;
	int32_t  error_code;
};

struct Ngs2SubmixerVoiceState {
	Ngs2VoiceState voice_state;
	float          envelope_height;
	float          peak_height;
	float          compressor_height;
};

struct Ngs2CustomMasteringVoiceState {
	Ngs2VoiceState voice_state;
	uint32_t       reserved;
	uint32_t       reserved2;
};

struct Ngs2SamplerVoiceState {
	Ngs2VoiceState voice_state;
	float          envelope_height;
	float          peak_height;
	uint32_t       reserved;
	uint64_t       num_decoded_samples;
	uint64_t       decoded_data_size;
	uint64_t       user_data;
	const void*    waveform_data;
};

static Ngs2Internal*        g_ngs_list     = nullptr;
static Ngs2RackInternal*    g_racks_list   = nullptr;
static std::atomic_uint32_t g_next_ngs_uid = 1;
static Common::Mutex        g_racks_mutex;

static_assert(sizeof(Ngs2UserFxContext) == 72);
static_assert(sizeof(Ngs2UserFxProcessContext) == 104);
static_assert(sizeof(Ngs2UserFxOption) == 72);
static_assert(sizeof(Ngs2SystemOption) == 144);
static_assert(sizeof(Ngs2SystemInfo) == 184);
static_assert(sizeof(Ngs2RackOption) == 176);
static_assert(sizeof(Ngs2VoiceState) == 8);
static_assert(sizeof(Ngs2SubmixerVoiceState) == 20);
static_assert(sizeof(Ngs2CustomMasteringVoiceState) == 16);
static_assert(sizeof(Ngs2SamplerVoiceState) == 56);
static_assert(sizeof(Ngs2WaveformFormat) == 24);
static_assert(sizeof(Ngs2WaveformBlock) == 40);
static_assert(sizeof(Ngs2WaveformInfo) == 232);

static uint32_t Ngs2GetStateFlags(const Ngs2VoiceInternal* voice) {
	switch (voice->state) {
		case Ngs2VoicePlayState::Empty: return 0;
		case Ngs2VoicePlayState::Playing: return 0x3;
		case Ngs2VoicePlayState::Paused: return 0x5;
		case Ngs2VoicePlayState::Stopped: return 0xb;
	}

	return 0;
}

static Ngs2SystemOption Ngs2DefaultSystemOption() {
	Ngs2SystemOption option {};
	option.size              = sizeof(Ngs2SystemOption);
	option.max_grain_samples = 512;
	option.num_grain_samples = 256;
	option.sample_rate       = 48000;
	return option;
}

int KYTY_SYSV_ABI Ngs2SystemResetOption(Ngs2SystemOption* option) {
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(option == nullptr);

	*option = Ngs2DefaultSystemOption();
	return OK;
}

static Ngs2Internal* Ngs2CreateSystemInternal(const Ngs2SystemOption*      option,
                                              const Ngs2ContextBufferInfo* buffer_info) {
	auto* ngs = new (buffer_info->host_buffer) Ngs2Internal;

	ngs->option      = *option;
	ngs->buffer_info = *buffer_info;
	ngs->uid         = g_next_ngs_uid.fetch_add(1, std::memory_order_relaxed);
	ngs->next        = g_ngs_list;
	g_ngs_list       = ngs;

	return ngs;
}

static bool Ngs2RackIsCustom(Ngs2RackType type) {
	switch (type) {
		case Ngs2RackType::CustomSubmixer:
		case Ngs2RackType::CustomMastering:
		case Ngs2RackType::CustomSampler: return true;
		default: return false;
	}
}

int KYTY_SYSV_ABI Ngs2SystemQueryBufferSize(const Ngs2SystemOption* option,
                                            Ngs2ContextBufferInfo*  buffer_info) {
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(buffer_info == nullptr);

	auto default_option = Ngs2DefaultSystemOption();
	if (option == nullptr) {
		option = &default_option;
		LOGF("\t option            = nullptr, using reset defaults\n");
	}

	EXIT_NOT_IMPLEMENTED(option->size != sizeof(Ngs2SystemOption));

	std::memset(buffer_info, 0, sizeof(Ngs2ContextBufferInfo));
	buffer_info->host_buffer_size = sizeof(Ngs2Internal);

	return OK;
}

int KYTY_SYSV_ABI Ngs2SystemCreate(const Ngs2SystemOption*      option,
                                   const Ngs2ContextBufferInfo* buffer_info, uintptr_t* handle) {
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(buffer_info == nullptr);
	EXIT_NOT_IMPLEMENTED(handle == nullptr);
	EXIT_NOT_IMPLEMENTED(buffer_info->host_buffer == nullptr);
	EXIT_NOT_IMPLEMENTED(buffer_info->host_buffer_size < sizeof(Ngs2Internal));

	auto default_option = Ngs2DefaultSystemOption();
	if (option == nullptr) {
		option = &default_option;
		LOGF("\t option            = nullptr, using reset defaults\n");
	}

	EXIT_NOT_IMPLEMENTED(option->size != sizeof(Ngs2SystemOption));

	auto* ngs = Ngs2CreateSystemInternal(option, buffer_info);

	*handle = reinterpret_cast<uintptr_t>(ngs);

	return OK;
}

static void Ngs2FillDefaultRackOption(uint32_t rack_id, Ngs2RackOptionUnion* option) {
	EXIT_NOT_IMPLEMENTED(option == nullptr);

	*option = {};

	switch (rack_id) {
		case 0x1000:
			option->sampler.rack_option.size                   = sizeof(Ngs2SamplerRackOption);
			option->sampler.rack_option.max_grain_samples      = 512;
			option->sampler.rack_option.max_voices             = 256;
			option->sampler.rack_option.max_input_delay_blocks = 0;
			option->sampler.rack_option.max_matrices           = 1;
			option->sampler.rack_option.max_ports              = 8;
			option->sampler.max_channel_works                  = 256;
			option->sampler.max_codec_caches                   = 32;
			option->sampler.max_waveform_blocks                = 4;
			option->sampler.max_envelope_points                = 4;
			option->sampler.max_filters                        = 8;
			option->sampler.max_atrac9_decoders                = 256;
			option->sampler.max_atrac9_channel_works           = 256;
			option->sampler.max_ajm_atrac9_decoders            = 0;
			option->sampler.num_peak_meter_blocks              = 8;
			break;
		case 0x2000:
			option->submixer.rack_option.size                   = sizeof(Ngs2SubmixerRackOption);
			option->submixer.rack_option.max_grain_samples      = 512;
			option->submixer.rack_option.max_voices             = 1;
			option->submixer.rack_option.max_input_delay_blocks = 1;
			option->submixer.rack_option.max_matrices           = 1;
			option->submixer.rack_option.max_ports              = 8;
			option->submixer.max_channels                       = 8;
			option->submixer.max_envelope_points                = 4;
			option->submixer.max_filters                        = 8;
			option->submixer.max_inputs                         = 1;
			option->submixer.num_peak_meter_blocks              = 8;
			break;
		case 0x2001:
			option->reverb.rack_option.size                   = sizeof(Ngs2ReverbRackOption);
			option->reverb.rack_option.max_grain_samples      = 512;
			option->reverb.rack_option.max_voices             = 1;
			option->reverb.rack_option.max_input_delay_blocks = 1;
			option->reverb.rack_option.max_matrices           = 1;
			option->reverb.rack_option.max_ports              = 8;
			option->reverb.max_channels                       = 8;
			option->reverb.reverb_size                        = 1;
			break;
		case 0x3000:
			option->mastering.rack_option.size                   = sizeof(Ngs2MasteringRackOption);
			option->mastering.rack_option.max_grain_samples      = 512;
			option->mastering.rack_option.max_voices             = 1;
			option->mastering.rack_option.max_input_delay_blocks = 1;
			option->mastering.rack_option.max_matrices           = 0;
			option->mastering.rack_option.max_ports              = 0;
			option->mastering.max_channels                       = 8;
			option->mastering.num_peak_meter_blocks              = 8;
			break;
		case 0x4002:
			// FIXME: Temporary PS5 progress fallback. This mirrors Prospero reset helper's
			// common custom-submixer defaults, but the custom module internals are still stubbed.
			option->custom_submixer.custom_rack_option.rack_option.size =
			    sizeof(Ngs2CustomSubmixerRackOption);
			option->custom_submixer.custom_rack_option.rack_option.max_grain_samples      = 512;
			option->custom_submixer.custom_rack_option.rack_option.max_voices             = 1;
			option->custom_submixer.custom_rack_option.rack_option.max_input_delay_blocks = 1;
			option->custom_submixer.custom_rack_option.rack_option.max_matrices           = 1;
			option->custom_submixer.custom_rack_option.rack_option.max_ports              = 8;
			option->custom_submixer.custom_rack_option.num_buffers                        = 1;
			option->custom_submixer.max_channels                                          = 8;
			option->custom_submixer.max_inputs                                            = 1;
			break;
		case 0x4001:
			option->custom_sampler.custom_rack_option.rack_option.size =
			    sizeof(Ngs2CustomSamplerRackOption);
			option->custom_sampler.custom_rack_option.rack_option.max_grain_samples      = 512;
			option->custom_sampler.custom_rack_option.rack_option.max_voices             = 256;
			option->custom_sampler.custom_rack_option.rack_option.max_input_delay_blocks = 0;
			option->custom_sampler.custom_rack_option.rack_option.max_matrices           = 1;
			option->custom_sampler.custom_rack_option.rack_option.max_ports              = 8;
			option->custom_sampler.custom_rack_option.num_buffers                        = 1;
			option->custom_sampler.max_channel_works                                     = 256;
			option->custom_sampler.max_waveform_blocks                                   = 4;
			option->custom_sampler.max_atrac9_decoders                                   = 256;
			option->custom_sampler.max_atrac9_channel_works                              = 256;
			option->custom_sampler.max_ajm_atrac9_decoders                               = 0;
			option->custom_sampler.max_codec_caches                                      = 32;
			break;
		default: EXIT("unknown rack_id for default option: 0x%" PRIx32 "\n", rack_id);
	}
}

int KYTY_SYSV_ABI Ngs2RackQueryBufferSize(uint32_t rack_id, const Ngs2RackOption* option,
                                          Ngs2ContextBufferInfo* buffer_info) {
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(buffer_info == nullptr);

	Ngs2RackOptionUnion default_option {};
	if (option == nullptr) {
		Ngs2FillDefaultRackOption(rack_id, &default_option);
		option = &default_option.common;
		LOGF("\t option     = nullptr, using reset defaults for rack_id 0x%" PRIx32 "\n", rack_id);
	}

	LOGF("\t rack_id    = 0x%" PRIx32 "\n"
	     "\t max_voices = %u\n",
	     rack_id, option->max_voices);

	buffer_info->host_buffer_size =
	    sizeof(Ngs2RackInternal) + sizeof(Ngs2VoiceInternal) * option->max_voices;

	return OK;
}

int KYTY_SYSV_ABI Ngs2SystemCreateWithAllocator(const Ngs2SystemOption*    option,
                                                const Ngs2BufferAllocator* allocator,
                                                uintptr_t*                 handle) {
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(allocator == nullptr);
	EXIT_NOT_IMPLEMENTED(handle == nullptr);
	EXIT_NOT_IMPLEMENTED(allocator->alloc_handler == nullptr);
	EXIT_NOT_IMPLEMENTED(allocator->free_handler == nullptr);

	auto default_option = Ngs2DefaultSystemOption();
	if (option == nullptr) {
		option = &default_option;
		LOGF("\t option            = nullptr, using reset defaults\n");
	}

	EXIT_NOT_IMPLEMENTED(option->size != sizeof(Ngs2SystemOption));

	LOGF("\t name              = %.64s\n"
	     "\t flags             = %u\n"
	     "\t max_grain_samples = %u\n"
	     "\t num_grain_samples = %u\n"
	     "\t sample_rate       = %u\n"
	     "\t max_voice_channels = %u\n"
	     "\t alloc_handler     = 0x%016" PRIx64 "\n"
	     "\t free_handler      = 0x%016" PRIx64 "\n"
	     "\t user_data         = 0x%016" PRIx64 "\n",
	     option->name, option->flags, option->max_grain_samples, option->num_grain_samples,
	     option->sample_rate, option->max_voice_channels,
	     reinterpret_cast<uint64_t>(allocator->alloc_handler),
	     reinterpret_cast<uint64_t>(allocator->free_handler),
	     static_cast<uint64_t>(allocator->user_data));

	Ngs2ContextBufferInfo buf {};
	buf.host_buffer      = nullptr;
	buf.host_buffer_size = sizeof(Ngs2Internal);
	buf.user_data        = allocator->user_data;

	int result = allocator->alloc_handler(&buf);

	EXIT_NOT_IMPLEMENTED(result != OK);
	EXIT_NOT_IMPLEMENTED(buf.host_buffer == nullptr);

	auto* ngs      = Ngs2CreateSystemInternal(option, &buf);
	ngs->allocator = *allocator;

	*handle = reinterpret_cast<uintptr_t>(ngs);

	return OK;
}

int KYTY_SYSV_ABI Ngs2SystemGetInfo(uintptr_t system_handle, Ngs2SystemInfo* info,
                                    size_t info_size) {
	constexpr int32_t ERROR_INVALID_OUT_ADDRESS   = static_cast<int32_t>(0x804a8010u);
	constexpr int32_t ERROR_INVALID_OUT_SIZE      = static_cast<int32_t>(0x804a8011u);
	constexpr int32_t ERROR_INVALID_SYSTEM_HANDLE = static_cast<int32_t>(0x804a8201u);

	if (info == nullptr) {
		return ERROR_INVALID_OUT_ADDRESS;
	}
	if (info_size != sizeof(Ngs2SystemInfo)) {
		return ERROR_INVALID_OUT_SIZE;
	}

	auto* ngs     = reinterpret_cast<Ngs2Internal*>(system_handle);
	auto* current = g_ngs_list;
	while (current != nullptr && current != ngs) {
		current = current->next;
	}
	if (current == nullptr) {
		return ERROR_INVALID_SYSTEM_HANDLE;
	}

	Common::LockGuard lock(ngs->mutex);

	*info = {};
	std::memcpy(info->name, ngs->option.name, sizeof(info->name));
	info->system_handle     = system_handle;
	info->buffer_info       = ngs->buffer_info;
	info->uid               = ngs->uid;
	info->min_grain_samples = 64;
	info->max_grain_samples = ngs->option.max_grain_samples;
	info->state_flags       = 1;
	info->render_count      = ngs->render_count;
	info->sample_rate       = ngs->option.sample_rate;
	info->num_grain_samples = ngs->option.num_grain_samples;

	Common::LockGuard racks_lock(g_racks_mutex);
	for (auto* rack = g_racks_list; rack != nullptr; rack = rack->next) {
		if (rack->ngs == ngs) {
			info->rack_count++;
		}
	}

	return OK;
}

int KYTY_SYSV_ABI Ngs2SystemSetGrainSamples(uintptr_t system_handle, uint32_t num_samples) {
	PRINT_NAME();
	LOGF("\t system_handle = 0x%016" PRIx64 "\n"
	     "\t num_samples   = %u\n",
	     static_cast<uint64_t>(system_handle), num_samples);

	EXIT_NOT_IMPLEMENTED(system_handle == 0);

	auto* ngs                     = reinterpret_cast<Ngs2Internal*>(system_handle);
	ngs->option.num_grain_samples = num_samples;

	return OK;
}

int KYTY_SYSV_ABI Ngs2SystemDestroy(uintptr_t system_handle, Ngs2ContextBufferInfo* buffer_info) {
	PRINT_NAME();
	LOGF("\t system_handle = 0x%016" PRIx64 "\n", static_cast<uint64_t>(system_handle));

	if (buffer_info != nullptr) {
		std::memset(buffer_info, 0, sizeof(Ngs2ContextBufferInfo));
	}

	return OK;
}

static void Ngs2InitModules(Ngs2RackInternal& rack) {
	if (!Ngs2RackIsCustom(rack.type)) {
		return;
	}
	const auto& custom = rack.option.custom_sampler.custom_rack_option;
	EXIT_NOT_IMPLEMENTED(custom.num_buffers != 1);
	auto* voices = reinterpret_cast<Ngs2VoiceInternal*>(&rack + 1);
	for (uint32_t m = 0; m < custom.num_modules; ++m) {
		if (custom.module[m].module_id != 0x1f) {
			continue;
		}
		auto& fx = rack.fx[m];
		fx       = *reinterpret_cast<const Ngs2UserFxOption*>(custom.module[m].option);
		rack.common[m].resize(fx.common_size);
		for (uint32_t i = 0; i < rack.option.common.max_voices; ++i) {
			auto& module = voices[i].modules[m];
			module.param.resize(fx.param_size);
			module.work.resize(fx.work_size);
			module.state.resize(custom.module[m].state_size);
			if (fx.setup != nullptr) {
				Ngs2UserFxContext context {
				    rack.common[m].data(), module.param.data(),           module.work.data(),
				    fx.user_data,          rack.option.common.max_voices, i};
				EXIT_NOT_IMPLEMENTED(fx.setup(&context) != OK);
			}
		}
	}
}

int KYTY_SYSV_ABI Ngs2RackCreate(uintptr_t system_handle, uint32_t rack_id,
                                 const Ngs2RackOption*        option,
                                 const Ngs2ContextBufferInfo* buffer_info, uintptr_t* handle) {
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(buffer_info == nullptr);
	EXIT_NOT_IMPLEMENTED(handle == nullptr);
	EXIT_NOT_IMPLEMENTED(buffer_info->host_buffer == nullptr);
	EXIT_NOT_IMPLEMENTED(buffer_info->host_buffer_size == 0);
	EXIT_NOT_IMPLEMENTED(system_handle == 0);

	Ngs2RackOptionUnion default_option {};
	if (option == nullptr) {
		Ngs2FillDefaultRackOption(rack_id, &default_option);
		option = &default_option.common;
		LOGF("\t option                 = nullptr, using reset defaults for rack_id 0x%" PRIx32
		     "\n",
		     rack_id);
	}

	EXIT_NOT_IMPLEMENTED(option->size < sizeof(Ngs2RackOption));

	LOGF("\t rack_id                = 0x%" PRIx32 "\n"
	     "\t name                   = %.64s\n"
	     "\t flags                  = %u\n"
	     "\t max_grain_samples      = %u\n"
	     "\t max_voices             = %u\n"
	     "\t max_input_delay_blocks = %u\n"
	     "\t max_matrices           = %u\n"
	     "\t max_ports              = %u\n"
	     "\t max_voice_channels     = %u\n"
	     "\t max_output_channels    = %u\n"
	     "\t host_buffer            = 0x%016" PRIx64 "\n"
	     "\t host_buffer_size      = 0x%016" PRIx64 "\n",
	     rack_id, option->name, option->flags, option->max_grain_samples, option->max_voices,
	     option->max_input_delay_blocks, option->max_matrices, option->max_ports,
	     option->max_voice_channels, option->max_output_channels,
	     reinterpret_cast<uint64_t>(buffer_info->host_buffer),
	     static_cast<uint64_t>(buffer_info->host_buffer_size));

	auto* ngs    = reinterpret_cast<Ngs2Internal*>(system_handle);
	auto* rack   = new (buffer_info->host_buffer) Ngs2RackInternal {};
	auto* voices = reinterpret_cast<Ngs2VoiceInternal*>(rack + 1);

	Common::LockGuard lock(ngs->mutex);

	switch (rack_id) {
		case 0x1000:
			EXIT_NOT_IMPLEMENTED(option->size != sizeof(Ngs2SamplerRackOption));
			rack->option.sampler = *reinterpret_cast<const Ngs2SamplerRackOption*>(option);
			rack->type           = Ngs2RackType::Sampler;
			break;
		case 0x2000:
			EXIT_NOT_IMPLEMENTED(option->size != sizeof(Ngs2SubmixerRackOption));
			rack->option.submixer = *reinterpret_cast<const Ngs2SubmixerRackOption*>(option);
			rack->type            = Ngs2RackType::Submixer;
			break;
		case 0x2001:
			EXIT_NOT_IMPLEMENTED(option->size != sizeof(Ngs2ReverbRackOption));
			rack->option.reverb = *reinterpret_cast<const Ngs2ReverbRackOption*>(option);
			rack->type          = Ngs2RackType::Reverb;
			break;
		case 0x3000:
			EXIT_NOT_IMPLEMENTED(option->size != sizeof(Ngs2MasteringRackOption));
			rack->option.mastering = *reinterpret_cast<const Ngs2MasteringRackOption*>(option);
			rack->type             = Ngs2RackType::Mastering;
			break;
		case 0x4002:
			EXIT_NOT_IMPLEMENTED(option->size != sizeof(Ngs2CustomSubmixerRackOption));
			rack->option.custom_submixer =
			    *reinterpret_cast<const Ngs2CustomSubmixerRackOption*>(option);
			rack->type = Ngs2RackType::CustomSubmixer;
			break;
		case 0x4003:
			EXIT_NOT_IMPLEMENTED(option->size != sizeof(Ngs2CustomMasteringRackOption));
			rack->option.custom_mastering =
			    *reinterpret_cast<const Ngs2CustomMasteringRackOption*>(option);
			rack->type = Ngs2RackType::CustomMastering;
			break;
		case 0x4001:
			EXIT_NOT_IMPLEMENTED(option->size != sizeof(Ngs2CustomSamplerRackOption));
			rack->option.custom_sampler =
			    *reinterpret_cast<const Ngs2CustomSamplerRackOption*>(option);
			rack->type = Ngs2RackType::CustomSampler;
			break;
		default: EXIT("unknown rack_id: 0x%" PRIx32 "\n", rack_id);
	}

	LOGF("\t type                   = %s\n", magic_enum::enum_name(rack->type));

	rack->allocator   = Ngs2BufferAllocator();
	rack->buffer_info = *buffer_info;
	rack->ngs         = ngs;

	{
		Common::LockGuard racks_lock(g_racks_mutex);
		rack->next   = g_racks_list;
		g_racks_list = rack;
	}

	for (uint32_t i = 0; i < option->max_voices; i++) {
		auto& voice = *std::construct_at(voices + i);
		voice.rack  = rack;
		voice.ports.resize(option->max_ports);
		voice.matrices.resize(option->max_matrices);
		if (Ngs2RackIsCustom(rack->type)) {
			voice.modules.resize(rack->option.custom_sampler.custom_rack_option.num_modules);
		}
	}
	Ngs2InitModules(*rack);

	*handle = reinterpret_cast<uintptr_t>(rack);

	return OK;
}

int KYTY_SYSV_ABI Ngs2RackCreateWithAllocator(uintptr_t system_handle, uint32_t rack_id,
                                              const Ngs2RackOption*      option,
                                              const Ngs2BufferAllocator* allocator,
                                              uintptr_t*                 handle) {
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(allocator == nullptr);
	EXIT_NOT_IMPLEMENTED(handle == nullptr);
	EXIT_NOT_IMPLEMENTED(allocator->alloc_handler == nullptr);
	EXIT_NOT_IMPLEMENTED(allocator->free_handler == nullptr);
	EXIT_NOT_IMPLEMENTED(system_handle == 0);

	Ngs2RackOptionUnion default_option {};
	if (option == nullptr) {
		Ngs2FillDefaultRackOption(rack_id, &default_option);
		option = &default_option.common;
		LOGF("\t option                 = nullptr, using reset defaults for rack_id 0x%" PRIx32
		     "\n",
		     rack_id);
	}

	EXIT_NOT_IMPLEMENTED(option->size < sizeof(Ngs2RackOption));

	LOGF("\t alloc_handler          = 0x%016" PRIx64 "\n"
	     "\t free_handler           = 0x%016" PRIx64 "\n"
	     "\t user_data              = 0x%016" PRIx64 "\n",
	     reinterpret_cast<uint64_t>(allocator->alloc_handler),
	     reinterpret_cast<uint64_t>(allocator->free_handler),
	     static_cast<uint64_t>(allocator->user_data));

	Ngs2ContextBufferInfo buf {};
	buf.host_buffer      = nullptr;
	buf.host_buffer_size = 0;
	buf.user_data        = allocator->user_data;

	Ngs2RackQueryBufferSize(rack_id, option, &buf);

	EXIT_NOT_IMPLEMENTED(buf.host_buffer_size == 0);

	int result = allocator->alloc_handler(&buf);

	EXIT_NOT_IMPLEMENTED(result != OK);
	EXIT_NOT_IMPLEMENTED(buf.host_buffer == nullptr);

	result = Ngs2RackCreate(system_handle, rack_id, option, &buf, handle);

	if (result == OK) {
		auto* rack      = static_cast<Ngs2RackInternal*>(buf.host_buffer);
		rack->allocator = *allocator;
	}

	return result;
}

int KYTY_SYSV_ABI Ngs2RackDestroy(uintptr_t rack_handle, Ngs2ContextBufferInfo* buffer_info) {
	constexpr int32_t ERROR_INVALID_RACK_HANDLE = static_cast<int32_t>(0x804a8202u);

	PRINT_NAME();
	LOGF("\t rack_handle = 0x%016" PRIx64 "\n", static_cast<uint64_t>(rack_handle));

	if (buffer_info != nullptr) {
		*buffer_info = {};
	}
	if (rack_handle == 0) {
		return ERROR_INVALID_RACK_HANDLE;
	}

	auto*         rack = reinterpret_cast<Ngs2RackInternal*>(rack_handle);
	Ngs2Internal* ngs  = nullptr;
	{
		Common::LockGuard racks_lock(g_racks_mutex);
		for (auto* current = g_racks_list; current != nullptr; current = current->next) {
			if (current == rack) {
				ngs = current->ngs;
				break;
			}
		}
	}
	if (ngs == nullptr) {
		return ERROR_INVALID_RACK_HANDLE;
	}
	Ngs2ContextBufferInfo context_buffer;
	Ngs2BufferAllocator   allocator;
	{
		Common::LockGuard lock(ngs->mutex);
		{
			Common::LockGuard racks_lock(g_racks_mutex);
			auto**            link = &g_racks_list;
			while (*link != nullptr && *link != rack) {
				link = &(*link)->next;
			}
			if (*link == nullptr) {
				return ERROR_INVALID_RACK_HANDLE;
			}
			*link = rack->next;
		}
		auto* voices = reinterpret_cast<Ngs2VoiceInternal*>(rack + 1);
		for (uint32_t i = 0; i < rack->option.common.max_voices; ++i) {
			for (size_t m = 0; m < voices[i].modules.size(); ++m) {
				const auto& fx = rack->fx[m];
				if (fx.cleanup != nullptr) {
					auto&             module = voices[i].modules[m];
					Ngs2UserFxContext context {rack->common[m].data(),
					                           module.param.data(),
					                           module.work.data(),
					                           fx.user_data,
					                           rack->option.common.max_voices,
					                           i};
					EXIT_NOT_IMPLEMENTED(fx.cleanup(&context) != OK);
				}
			}
			std::destroy_at(voices + i);
		}
		context_buffer = rack->buffer_info;
		allocator      = rack->allocator;
		std::destroy_at(rack);
	}

	if (allocator.free_handler != nullptr) {
		return allocator.free_handler(&context_buffer);
	}
	if (buffer_info != nullptr) {
		*buffer_info = context_buffer;
	}

	return OK;
}

int KYTY_SYSV_ABI Ngs2RackLock(uintptr_t rack_handle) {
	PRINT_NAME();
	LOGF("\t rack_handle = 0x%016" PRIx64 "\n", static_cast<uint64_t>(rack_handle));

	EXIT_NOT_IMPLEMENTED(rack_handle == 0);

	auto* rack = reinterpret_cast<Ngs2RackInternal*>(rack_handle);

	EXIT_NOT_IMPLEMENTED(rack->ngs == nullptr);

	rack->ngs->mutex.Lock();

	return OK;
}

int KYTY_SYSV_ABI Ngs2RackUnlock(uintptr_t rack_handle) {
	PRINT_NAME();
	LOGF("\t rack_handle = 0x%016" PRIx64 "\n", static_cast<uint64_t>(rack_handle));

	EXIT_NOT_IMPLEMENTED(rack_handle == 0);

	auto* rack = reinterpret_cast<Ngs2RackInternal*>(rack_handle);

	EXIT_NOT_IMPLEMENTED(rack->ngs == nullptr);

	rack->ngs->mutex.Unlock();

	return OK;
}

static void Ngs2ApplyEvent(Ngs2VoiceInternal& voice) {
	switch (voice.event) {
		case Ngs2VoicePlayEvent::None: break;
		case Ngs2VoicePlayEvent::Play:
			if (voice.state == Ngs2VoicePlayState::Empty) {
				voice.state = Ngs2VoicePlayState::Playing;
			}
			break;
		case Ngs2VoicePlayEvent::Pause:
			if (voice.state == Ngs2VoicePlayState::Playing) {
				voice.state = Ngs2VoicePlayState::Paused;
			}
			break;
		case Ngs2VoicePlayEvent::Resume:
			if (voice.state == Ngs2VoicePlayState::Paused) {
				voice.state = Ngs2VoicePlayState::Playing;
			}
			break;
		case Ngs2VoicePlayEvent::Stop:
			if (voice.state == Ngs2VoicePlayState::Playing) {
				voice.state = Ngs2VoicePlayState::Stopped;
			}
			break;
		case Ngs2VoicePlayEvent::StopImm:
		case Ngs2VoicePlayEvent::Kill: voice.state = Ngs2VoicePlayState::Empty; break;
	}
	voice.event = Ngs2VoicePlayEvent::None;
}

static void Ngs2ConsumeSamples(Ngs2VoiceInternal& voice, uint32_t grain) {
	uint32_t output = 0;
	while (output < grain && !voice.blocks.empty()) {
		auto&          block   = voice.blocks.front();
		auto           count   = std::min(grain - output, block.info.num_samples - block.cursor);
		const int16_t* pcm     = nullptr;
		const float*   decoded = nullptr;
		if (voice.decoder != nullptr) {
			const auto skip =
			    std::min(block.skip_remaining,
			             static_cast<uint32_t>(voice.decoded_frame.size() / voice.channels));
			Ajm::AjmGaplessState gapless;
			gapless.Set({block.info.num_samples - block.cursor, static_cast<uint16_t>(skip), 0},
			            true);
			const auto result = voice.decoder->Decode(
			    block.data + block.data_cursor, block.info.data_size - block.data_cursor,
			    voice.decoded_frame.data(), voice.decoded_frame.size() * sizeof(float), false,
			    &gapless);
			EXIT_NOT_IMPLEMENTED(result.result != OK || result.input_consumed == 0);
			block.data_cursor += result.input_consumed;
			block.skip_remaining -= skip - gapless.current.skip_samples;
			const auto decoded_samples =
			    static_cast<uint32_t>(result.output_written / (sizeof(float) * voice.channels));
			if (decoded_samples == 0) {
				continue;
			}
			EXIT_NOT_IMPLEMENTED(decoded_samples > count);
			count   = decoded_samples;
			decoded = voice.decoded_frame.data();
		} else {
			pcm = reinterpret_cast<const int16_t*>(block.data) +
			      (block.info.num_skip_samples + block.cursor) * voice.channels;
		}
		for (uint32_t c = 0; c < voice.channels; ++c) {
			for (uint32_t i = 0; i < count; ++i) {
				const auto index = i * voice.channels + c;
				voice.samples[c * grain + output + i] =
				    decoded != nullptr ? decoded[index] : pcm[index] / 32768.0f;
			}
		}
		output += count;
		block.cursor += count;
		if (block.cursor == block.info.num_samples) {
			struct CallbackInfo {
				uintptr_t   data, voice;
				uint32_t    flag, reserved;
				uintptr_t   user;
				const void* block_data;
				size_t      size;
				uint32_t    repeats, attributes;
			} info {voice.callback_data,
			        reinterpret_cast<uintptr_t>(&voice),
			        1,
			        0,
			        block.info.user_data,
			        block.data,
			        block.info.data_size,
			        0,
			        0};
			static_assert(sizeof(CallbackInfo) == 56);
			voice.blocks.pop_front();
			if (voice.blocks.empty() && !voice.accepts_blocks) {
				voice.state = Ngs2VoicePlayState::Empty;
			}
			if (voice.callback != 0 && (voice.callback_flags & 1u) != 0) {
				reinterpret_cast<void KYTY_SYSV_ABI (*)(const CallbackInfo*)>(voice.callback)(
				    &info);
			}
		}
	}
	voice.has_samples = output != 0;
}

static void Ngs2RenderVoice(Ngs2VoiceInternal& voice, const std::vector<Ngs2VoiceInternal*>& voices,
                            uint32_t grain) {
	if (voice.rendered) {
		return;
	}
	EXIT_NOT_IMPLEMENTED(voice.rendering);
	voice.rendering = true;
	voice.samples.assign(grain * voice.channels, 0.0f);
	voice.has_samples = false;
	if (voice.state == Ngs2VoicePlayState::Playing) {
		if (voice.rack->type == Ngs2RackType::CustomSampler) {
			Ngs2ConsumeSamples(voice, grain);
		}
		for (auto* source: voices) {
			for (const auto& port: source->ports) {
				if (port.dest != &voice || port.volume == 0.0f) {
					continue;
				}
				Ngs2RenderVoice(*source, voices, grain);
				if (!source->has_samples) {
					continue;
				}
				EXIT_NOT_IMPLEMENTED(port.input != 0);
				const auto* matrix = port.matrix < 0 ? nullptr : &source->matrices.at(port.matrix);
				for (uint32_t dst = 0; dst < voice.channels; ++dst) {
					for (uint32_t src = 0; src < source->channels; ++src) {
						const float level =
						    port.volume * (matrix == nullptr
						                       ? (src == dst ? 1.0f : 0.0f)
						                       : matrix->at(dst * source->channels + src));
						for (uint32_t i = 0; i < grain; ++i) {
							voice.samples[dst * grain + i] +=
							    source->samples[src * grain + i] * level;
						}
					}
				}
				voice.has_samples = true;
			}
		}
		if (voice.has_samples) {
			const auto& custom = voice.rack->option.custom_sampler.custom_rack_option;
			for (size_t m = 0; m < voice.modules.size(); ++m) {
				if (custom.module[m].module_id != 0x1f) {
					if (!voice.modules[m].render_warned) {
						Log::WriteToConsoleAndLog(fmt::sprintf(
						    "warning: NGS2 custom module 0x%02x at index %zu is ignored during render\n",
						    custom.module[m].module_id, m));
						voice.modules[m].render_warned = true;
					}
					continue;
				}
				EXIT_NOT_IMPLEMENTED(custom.module[m].source_buffer_id != 0 ||
				                     custom.module[m].dest_buffer_id != 0);
				auto&               module = voice.modules[m];
				const auto&         fx     = voice.rack->fx[m];
				std::vector<float*> channels(voice.channels);
				for (uint32_t c = 0; c < voice.channels; ++c) {
					channels[c] = voice.samples.data() + c * grain;
				}
				Ngs2UserFxProcessContext context {channels.data(),
				                                  voice.rack->common[m].data(),
				                                  module.param.data(),
				                                  module.work.data(),
				                                  module.state.data(),
				                                  fx.user_data,
				                                  module.flags,
				                                  voice.channels,
				                                  voice.channels,
				                                  grain,
				                                  voice.rack->ngs->option.sample_rate};
				EXIT_NOT_IMPLEMENTED(fx.process(&context) != OK);
				module.flags = 0;
			}
		}
	}
	voice.rendering = false;
	voice.rendered  = true;
}

int KYTY_SYSV_ABI Ngs2SystemRender(uintptr_t system_handle, const Ngs2RenderBufferInfo* buffer_info,
                                   uint32_t num_buffer_info) {
	EXIT_NOT_IMPLEMENTED(buffer_info == nullptr || system_handle == 0 || num_buffer_info == 0);
	auto* ngs = reinterpret_cast<Ngs2Internal*>(system_handle);
	Common::LockGuard lock(ngs->mutex);
	std::vector<Ngs2VoiceInternal*> voices;
	{
		Common::LockGuard racks_lock(g_racks_mutex);
		for (auto* rack = g_racks_list; rack != nullptr; rack = rack->next) {
			if (rack->ngs != ngs) {
				continue;
			}
			auto* items = reinterpret_cast<Ngs2VoiceInternal*>(rack + 1);
			for (uint32_t i = 0; i < rack->option.common.max_voices; ++i) {
				Ngs2ApplyEvent(items[i]);
				items[i].rendered = false;
				if (items[i].channels != 0) {
					voices.push_back(items + i);
				}
			}
		}
	}
	for (uint32_t i = 0; i < num_buffer_info; ++i) {
		if (buffer_info[i].buffer != nullptr && buffer_info[i].buffer_size != 0) {
			std::memset(buffer_info[i].buffer, 0, buffer_info[i].buffer_size);
		}
	}
	const auto grain = ngs->option.num_grain_samples;
	for (auto* voice: voices) {
		Ngs2RenderVoice(*voice, voices, grain);
		if (voice->rack->type != Ngs2RackType::Mastering || !voice->has_samples) {
			continue;
		}
		EXIT_NOT_IMPLEMENTED(voice->output_id >= num_buffer_info);
		const auto& output = buffer_info[voice->output_id];
		EXIT_NOT_IMPLEMENTED(output.buffer == nullptr);
		EXIT_NOT_IMPLEMENTED(output.waveform_type != 0x18 ||
		                     output.num_channels != voice->channels);
		EXIT_NOT_IMPLEMENTED(output.buffer_size < grain * output.num_channels * sizeof(float));
		auto* pcm = static_cast<float*>(output.buffer);
		for (uint32_t dst = 0; dst < output.num_channels; ++dst) {
			for (uint32_t src = 0; src < voice->channels; ++src) {
				const float level = voice->output_matrix.empty()
				                        ? (src == dst ? 1.0f : 0.0f)
				                        : voice->output_matrix.at(src * output.num_channels + dst);
				for (uint32_t i = 0; i < grain; ++i) {
					pcm[i * output.num_channels + dst] += voice->samples[src * grain + i] * level;
				}
			}
		}
	}
	++ngs->render_count;
	return OK;
}

static uint16_t Ngs2ReadLe16(const uint8_t* data) {
	return static_cast<uint16_t>(data[0]) | (static_cast<uint16_t>(data[1]) << 8u);
}

static uint32_t Ngs2ReadLe32(const uint8_t* data) {
	return static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8u) |
	       (static_cast<uint32_t>(data[2]) << 16u) | (static_cast<uint32_t>(data[3]) << 24u);
}

static bool Ngs2FourCcEquals(const uint8_t* data, const char* four_cc) {
	return std::memcmp(data, four_cc, 4) == 0;
}

static int Ngs2ParseAtrac9Riff(const void* data, size_t data_size, Ngs2WaveformInfo* info) {
	static constexpr uint8_t ATRAC9_GUID[16] = {0xd2, 0x42, 0xe1, 0x47, 0xba, 0x36,
	                                            0x8d, 0x4d, 0x88, 0xfc, 0x61, 0x65,
	                                            0x4f, 0x8c, 0x83, 0x6c};
	const auto* bytes = static_cast<const uint8_t*>(data);
	if (bytes == nullptr || data_size < 12) {
		return NGS2_ERROR_INVALID_WAVEFORM_DATA;
	}
	if (!Ngs2FourCcEquals(bytes, "RIFF") || !Ngs2FourCcEquals(bytes + 8, "WAVE")) {
		return NGS2_ERROR_UNKNOWN_WAVEFORM_FORMAT;
	}

	const uint64_t riff_end64 = 8ull + Ngs2ReadLe32(bytes + 4);
	if (riff_end64 < 12 || riff_end64 > data_size) {
		return NGS2_ERROR_INVALID_WAVEFORM_DATA;
	}
	const auto riff_end = static_cast<size_t>(riff_end64);

	const uint8_t* format           = nullptr;
	const uint8_t* fact             = nullptr;
	size_t         waveform_offset  = 0;
	uint32_t       waveform_size    = 0;

	for (size_t offset = 12; offset + 8 <= riff_end;) {
		const auto* chunk       = bytes + offset;
		const auto  chunk_size  = static_cast<size_t>(Ngs2ReadLe32(chunk + 4));
		const auto  payload     = offset + 8;
		if (chunk_size > riff_end - payload) {
			return NGS2_ERROR_INVALID_WAVEFORM_DATA;
		}

		if (Ngs2FourCcEquals(chunk, "fmt ")) {
			if (chunk_size < 52) {
				return NGS2_ERROR_INVALID_WAVEFORM_FORMAT;
			}
			format = bytes + payload;
		} else if (Ngs2FourCcEquals(chunk, "fact")) {
			if (chunk_size < 12) {
				return NGS2_ERROR_INVALID_WAVEFORM_FORMAT;
			}
			fact = bytes + payload;
		} else if (Ngs2FourCcEquals(chunk, "data")) {
			if (payload > std::numeric_limits<uint32_t>::max()) {
				return NGS2_ERROR_INVALID_WAVEFORM_DATA;
			}
			waveform_offset = payload;
			waveform_size   = static_cast<uint32_t>(chunk_size);
		}

		const uint64_t next = static_cast<uint64_t>(payload) + chunk_size + (chunk_size & 1u);
		if (next > riff_end) {
			return NGS2_ERROR_INVALID_WAVEFORM_DATA;
		}
		offset = static_cast<size_t>(next);
	}

	if (format == nullptr || fact == nullptr || waveform_offset == 0) {
		return NGS2_ERROR_INVALID_WAVEFORM_FORMAT;
	}
	if (Ngs2ReadLe16(format) != 0xfffe ||
	    std::memcmp(format + 24, ATRAC9_GUID, sizeof(ATRAC9_GUID)) != 0) {
		return NGS2_ERROR_UNKNOWN_WAVEFORM_FORMAT;
	}

	std::array<uint8_t, ATRAC9_CONFIG_DATA_SIZE> config {};
	std::memcpy(config.data(), format + 44, config.size());
	if (config[0] != 0xfe || (config[1] & 1u) != 0 || ((config[1] >> 1u) & 7u) >= 6u) {
		return NGS2_ERROR_INVALID_WAVEFORM_FORMAT;
	}

	Atrac9CodecInfo codec {};
	void*           decoder = Atrac9GetHandle();
	const bool valid_codec =
	    decoder != nullptr && Atrac9InitDecoder(decoder, config.data()) == 0 &&
	    Atrac9GetCodecInfo(decoder, &codec) == 0 && codec.channels > 0 &&
	    codec.samplingRate > 0 && codec.superframeSize > 0 && codec.framesInSuperframe > 0 &&
	    codec.frameSamples > 0 && codec.superframeSize % codec.framesInSuperframe == 0;
	if (decoder != nullptr) {
		Atrac9ReleaseHandle(decoder);
	}
	const uint64_t frame_samples =
	    static_cast<uint64_t>(codec.frameSamples) * static_cast<uint64_t>(codec.framesInSuperframe);
	if (!valid_codec || codec.channels != Ngs2ReadLe16(format + 2) ||
	    codec.samplingRate != static_cast<int>(Ngs2ReadLe32(format + 4)) ||
	    codec.superframeSize != Ngs2ReadLe16(format + 12) ||
	    frame_samples != Ngs2ReadLe16(format + 18) ||
	    frame_samples > std::numeric_limits<uint32_t>::max()) {
		return NGS2_ERROR_INVALID_WAVEFORM_FORMAT;
	}

	info->format.waveform_type = NGS2_WAVEFORM_TYPE_ATRAC9;
	info->format.num_channels  = static_cast<uint32_t>(codec.channels);
	info->format.sample_rate   = static_cast<uint32_t>(codec.samplingRate);
	info->format.config_data = (static_cast<uint32_t>(config[0]) << 24u) |
	                           (static_cast<uint32_t>(config[1]) << 16u) |
	                           (static_cast<uint32_t>(config[2]) << 8u) | config[3];
	info->data_offset              = static_cast<uint32_t>(waveform_offset);
	info->data_size                = waveform_size;
	info->num_samples              = Ngs2ReadLe32(fact);
	info->audio_unit_size          = static_cast<uint32_t>(codec.superframeSize /
	                                                       codec.framesInSuperframe);
	info->num_audio_unit_samples   = static_cast<uint32_t>(codec.frameSamples);
	info->num_audio_unit_per_frame = static_cast<uint32_t>(codec.framesInSuperframe);
	info->audio_frame_size         = static_cast<uint32_t>(codec.superframeSize);
	info->num_audio_frame_samples  = static_cast<uint32_t>(frame_samples);
	info->num_delay_samples        = Ngs2ReadLe32(fact + 4);
	info->num_blocks               = 1;
	info->blocks[0].data_offset    = waveform_offset;
	info->blocks[0].data_size      = waveform_size;
	info->blocks[0].num_skip_samples = Ngs2ReadLe32(fact + 8);
	info->blocks[0].num_samples      = info->num_samples;
	return OK;
}

int KYTY_SYSV_ABI Ngs2ParseWaveformData(const void* data, size_t data_size,
                                        Ngs2WaveformInfo* info) {
	PRINT_NAME();
	LOGF("\t data = 0x%016" PRIx64 ", data_size = 0x%016" PRIx64 "\n",
	     reinterpret_cast<uint64_t>(data), static_cast<uint64_t>(data_size));

	if (info == nullptr) {
		return NGS2_ERROR_INVALID_OUT_ADDRESS;
	}

	std::memset(info, 0, sizeof(Ngs2WaveformInfo));
	return Ngs2ParseAtrac9Riff(data, data_size, info);
}

int KYTY_SYSV_ABI Ngs2CalcWaveformBlock(const Ngs2WaveformFormat* format, uint32_t sample_pos,
                                        uint32_t num_samples, Ngs2WaveformBlock* block) {
	PRINT_NAME();
	LOGF("\t format = 0x%016" PRIx64 ", sample_pos = %" PRIu32 ", num_samples = %" PRIu32 "\n",
	     reinterpret_cast<uint64_t>(format), sample_pos, num_samples);

	EXIT_NOT_IMPLEMENTED(block == nullptr);

	std::memset(block, 0, sizeof(Ngs2WaveformBlock));
	block->num_samples = num_samples;
	return OK;
}

int KYTY_SYSV_ABI Ngs2PanInit(Ngs2PanWork* work, const float* speaker_angles, float unit_angle,
                              uint32_t num_speakers) {
	PRINT_NAME();
	LOGF("\t work = 0x%016" PRIx64 ", num_speakers = %" PRIu32 "\n",
	     reinterpret_cast<uint64_t>(work), num_speakers);

	EXIT_NOT_IMPLEMENTED(work == nullptr);

	std::memset(work, 0, sizeof(Ngs2PanWork));
	work->unit_angle   = unit_angle;
	work->num_speakers = std::min<uint32_t>(num_speakers, 8);
	if (speaker_angles != nullptr) {
		for (uint32_t i = 0; i < work->num_speakers; i++) {
			work->speaker_angles[i] = speaker_angles[i];
		}
	}
	return OK;
}

int KYTY_SYSV_ABI Ngs2PanGetVolumeMatrix(Ngs2PanWork* work, const Ngs2PanParam* params,
                                         uint32_t num_params, uint32_t matrix_format,
                                         float* out_volume_matrix) {
	PRINT_NAME();
	LOGF("\t work = 0x%016" PRIx64 ", params = 0x%016" PRIx64 ", num_params = %" PRIu32
	     ", matrix_format = %" PRIu32 "\n",
	     reinterpret_cast<uint64_t>(work), reinterpret_cast<uint64_t>(params), num_params,
	     matrix_format);

	EXIT_NOT_IMPLEMENTED(out_volume_matrix == nullptr && num_params != 0);

	const auto channels = (matrix_format == 0 ? 2u : std::min<uint32_t>(matrix_format, 8));
	for (uint32_t p = 0; p < num_params; p++) {
		for (uint32_t c = 0; c < channels; c++) {
			out_volume_matrix[p * channels + c] = (c == 0 ? 1.0f : 0.0f);
		}
	}

	return OK;
}

int KYTY_SYSV_ABI Ngs2GeomResetListenerParam(Ngs2GeomListenerParam* out_listener_param) {
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(out_listener_param == nullptr);

	std::memset(out_listener_param, 0, sizeof(Ngs2GeomListenerParam));
	out_listener_param->orient_front.z = 1.0f;
	out_listener_param->orient_up.y    = 1.0f;
	out_listener_param->sound_speed    = 343.0f;

	return OK;
}

int KYTY_SYSV_ABI Ngs2GeomResetSourceParam(Ngs2GeomSourceParam* out_source_param) {
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(out_source_param == nullptr);

	std::memset(out_source_param, 0, sizeof(Ngs2GeomSourceParam));
	out_source_param->direction.z                = 1.0f;
	out_source_param->cone.inner_level           = 1.0f;
	out_source_param->cone.inner_angle           = 360.0f;
	out_source_param->cone.outer_level           = 1.0f;
	out_source_param->cone.outer_angle           = 360.0f;
	out_source_param->rolloff.model              = 0;
	out_source_param->rolloff.max_distance       = 1000000.0f;
	out_source_param->rolloff.rolloff_factor     = 1.0f;
	out_source_param->rolloff.reference_distance = 1.0f;
	out_source_param->doppler_factor             = 1.0f;
	out_source_param->fbw_level                  = 1.0f;
	out_source_param->lfe_level                  = 1.0f;
	out_source_param->max_level                  = 1.0f;
	out_source_param->min_level                  = 0.0f;
	out_source_param->num_speakers               = 2;
	out_source_param->matrix_format              = 2;

	return OK;
}

int KYTY_SYSV_ABI Ngs2GeomCalcListener(const Ngs2GeomListenerParam* param,
                                       Ngs2GeomListenerWork* out_work, uint32_t flags) {
	PRINT_NAME();
	LOGF("\t flags = 0x%08" PRIx32 "\n", flags);

	EXIT_NOT_IMPLEMENTED(param == nullptr);
	EXIT_NOT_IMPLEMENTED(out_work == nullptr);

	std::memset(out_work, 0, sizeof(Ngs2GeomListenerWork));
	for (uint32_t i = 0; i < 4; i++) {
		out_work->matrix[i][i] = 1.0f;
	}
	out_work->velocity    = param->velocity;
	out_work->sound_speed = (param->sound_speed > 0.0f ? param->sound_speed : 343.0f);
	out_work->coordinate  = flags & 0x1u;

	return OK;
}

int KYTY_SYSV_ABI Ngs2GeomApply(const Ngs2GeomListenerWork* listener,
                                const Ngs2GeomSourceParam* source, Ngs2GeomAttribute* out_attrib,
                                uint32_t flags) {
	PRINT_NAME();
	LOGF("\t flags = 0x%08" PRIx32 "\n", flags);

	EXIT_NOT_IMPLEMENTED(listener == nullptr);
	EXIT_NOT_IMPLEMENTED(source == nullptr);
	EXIT_NOT_IMPLEMENTED(out_attrib == nullptr);

	std::memset(out_attrib, 0, sizeof(Ngs2GeomAttribute));
	out_attrib->pitch_ratio         = 1.0f;
	out_attrib->a3d_attrib.position = source->position;
	out_attrib->a3d_attrib.volume   = std::max(source->min_level, source->max_level);

	const auto channels =
	    std::min<uint32_t>((source->matrix_format == 0 ? 2u : source->matrix_format), 8);
	const auto level = (source->max_level > 0.0f ? source->max_level : 1.0f);
	for (uint32_t ch = 0; ch < channels; ch++) {
		out_attrib->level[ch * 8 + ch] = level;
	}

	return OK;
}

int KYTY_SYSV_ABI Ngs2RackGetVoiceHandle(uintptr_t rack_handle, uint32_t voice_id,
                                         uintptr_t* handle) {
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(handle == nullptr);
	EXIT_NOT_IMPLEMENTED(rack_handle == 0);

	LOGF("\t voice_id = %u\n", voice_id);

	auto* rack   = reinterpret_cast<Ngs2RackInternal*>(rack_handle);
	auto* voices = reinterpret_cast<Ngs2VoiceInternal*>(rack_handle + sizeof(Ngs2RackInternal));

	if (voice_id >= rack->option.common.max_voices) {
		LOGF("\t warning: voice_id %u >= max_voices %u, using last available stub voice\n",
		     voice_id, rack->option.common.max_voices);
		if (rack->option.common.max_voices == 0) {
			return -1;
		}
		voice_id = rack->option.common.max_voices - 1;
	}

	EXIT_IF(voices[voice_id].rack != rack);

	*handle = reinterpret_cast<uintptr_t>(voices + voice_id);

	return OK;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
int KYTY_SYSV_ABI Ngs2VoiceControl(uintptr_t voice_handle, const Ngs2VoiceParamHeader* param_list) {
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(param_list == nullptr);
	EXIT_NOT_IMPLEMENTED(voice_handle == 0);

	auto* voice = reinterpret_cast<Ngs2VoiceInternal*>(voice_handle);

	Common::LockGuard lock(voice->rack->ngs->mutex);

	const auto* param = param_list;

	for (;;) {
		LOGF("\t id   = 0x%08" PRIx32 "\n"
		     "\t size = %" PRIu16 "\n"
		     "\t next = %" PRId16 "\n",
		     param->id, param->size, param->next);

		auto rack_id = param->id >> 16u;

		EXIT_NOT_IMPLEMENTED(((param->id >> 15u) & 0x1u) != 0);

		switch (rack_id) {
			case 0x0000: {
				auto cid = param->id & 0x7fffu;
				switch (cid) {
					case 0x0001: {
						EXIT_NOT_IMPLEMENTED(param->size != sizeof(Ngs2VoiceMatrixLevelsParam));
						const auto* ml = reinterpret_cast<const Ngs2VoiceMatrixLevelsParam*>(param);
						voice->SetMatrix(ml->matrix_id, ml->levels, ml->num_levels);
						LOGF("\t matrix_id  = %u\n"
						     "\t num_levels = %u\n"
						     "\t levels     = 0x%016" PRIx64 "\n",
						     ml->matrix_id, ml->num_levels, reinterpret_cast<uint64_t>(ml->levels));
						break;
					}
					case 0x0002: {
						EXIT_NOT_IMPLEMENTED(param->size != sizeof(Ngs2VoicePortVolumeParam));
						const auto* volume =
						    reinterpret_cast<const Ngs2VoicePortVolumeParam*>(param);
						voice->SetVolume(volume->port, volume->level);
						LOGF("\t port  = %u\n"
						     "\t level = %f\n",
						     volume->port, volume->level);
						break;
					}
					case 0x0003: {
						EXIT_NOT_IMPLEMENTED(param->size != sizeof(Ngs2VoicePortMatrixParam));
						const auto* pm = reinterpret_cast<const Ngs2VoicePortMatrixParam*>(param);
						voice->SetPortMatrix(pm->port, pm->matrix_id);
						LOGF("\t port      = %u\n"
						     "\t matrix_id = %d\n",
						     pm->port, pm->matrix_id);
						break;
					}
					case 0x0004: {
						EXIT_NOT_IMPLEMENTED(param->size != sizeof(Ngs2VoicePortDelayParam));
						const auto* delay = reinterpret_cast<const Ngs2VoicePortDelayParam*>(param);
						LOGF("\t port        = %u\n"
						     "\t num_samples = %u\n",
						     delay->port, delay->num_samples);
						break;
					}
					case 0x0005: {
						EXIT_NOT_IMPLEMENTED(param->size != sizeof(Ngs2VoicePatchParam));
						const auto* patch = reinterpret_cast<const Ngs2VoicePatchParam*>(param);
						EXIT_NOT_IMPLEMENTED(patch->port >= voice->ports.size());
						voice->ports[patch->port].dest =
						    reinterpret_cast<Ngs2VoiceInternal*>(patch->dest_handle);
						voice->ports[patch->port].input = patch->dest_input_id;
						LOGF("\t connect->port          = %u\n"
						     "\t connect->dest_input_id = %u\n"
						     "\t connect->dest_handle   = 0x%016" PRIx64 "\n",
						     patch->port, patch->dest_input_id, patch->dest_handle);
						break;
					}
					case 0x0006: {
						EXIT_NOT_IMPLEMENTED(param->size != sizeof(Ngs2VoiceEventParam));
						const auto* event = reinterpret_cast<const Ngs2VoiceEventParam*>(param);
						voice->SetEvent(event->event_id);
						LOGF("\t event = %u\n", event->event_id);
						break;
					}
					case 0x0007: {
						EXIT_NOT_IMPLEMENTED(param->size != sizeof(Ngs2VoiceCallbackParam));
						const auto* callback =
						    reinterpret_cast<const Ngs2VoiceCallbackParam*>(param);
						voice->callback       = callback->callback;
						voice->callback_data  = callback->callback_data;
						voice->callback_flags = callback->flags;
						LOGF("\t callback      = 0x%016" PRIx64 "\n"
						     "\t callback_data = 0x%016" PRIx64 "\n"
						     "\t flags         = 0x%08" PRIx32 "\n",
						     static_cast<uint64_t>(voice->callback),
						     static_cast<uint64_t>(voice->callback_data), voice->callback_flags);
						break;
					}
					default: EXIT("unknown id: 0x%04" PRIx32 "\n", cid);
				}
				break;
			}
			case 0x1000: EXIT_NOT_IMPLEMENTED(voice->rack->type != Ngs2RackType::Sampler); break;
			case 0x2000: EXIT_NOT_IMPLEMENTED(voice->rack->type != Ngs2RackType::Submixer); break;
			case 0x2001: EXIT_NOT_IMPLEMENTED(voice->rack->type != Ngs2RackType::Reverb); break;
			case 0x3000: {
				EXIT_NOT_IMPLEMENTED(voice->rack->type != Ngs2RackType::Mastering);
				const auto* values = reinterpret_cast<const uint32_t*>(param + 1);
				switch (param->id & 0xffffu) {
					case 0: voice->channels = values[0]; break;
					case 1: {
						const auto* matrix =
						    reinterpret_cast<const Ngs2VoiceMatrixLevelsParam*>(param);
						EXIT_NOT_IMPLEMENTED(matrix->matrix_id != 5);
						voice->output_matrix.assign(matrix->levels,
						                            matrix->levels + matrix->num_levels);
						break;
					}
					case 5: voice->output_id = values[0]; break;
					default: EXIT("unsupported mastering control: 0x%08" PRIx32 "\n", param->id);
				}
				break;
			}
			case 0x4000: {
				EXIT_NOT_IMPLEMENTED(!Ngs2RackIsCustom(voice->rack->type));
				const auto index = param->id & 0x1fu;
				EXIT_NOT_IMPLEMENTED(index >= voice->modules.size());
				if ((param->id & 0xffffffe0u) != 0x40001f00u ||
				    voice->rack->option.custom_sampler.custom_rack_option.module[index].module_id !=
				        0x1f) {
					if (!voice->modules[index].control_warned) {
						Log::WriteToConsoleAndLog(fmt::sprintf(
						    "warning: NGS2 custom control 0x%08" PRIx32 " is ignored\n",
						    param->id));
						voice->modules[index].control_warned = true;
					}
					break;
				}
				struct FxParam {
					Ngs2VoiceParamHeader header;
					const void*          data;
					size_t               size;
				};
				const auto& fx     = *reinterpret_cast<const FxParam*>(param);
				auto&       module = voice->modules[index];
				EXIT_NOT_IMPLEMENTED(voice->rack->fx[index].control != 0 ||
				                     fx.size != module.param.size());
				std::memcpy(module.param.data(), fx.data, fx.size);
				module.flags |= 2;
				break;
			}
			case 0x4001: {
				EXIT_NOT_IMPLEMENTED(voice->rack->type != Ngs2RackType::CustomSampler);
				switch (param->id & 0xffffu) {
					case 0: {
						const auto& format =
						    *reinterpret_cast<const Ngs2WaveformFormat*>(param + 1);
						voice->SetupSampler(format);
						break;
					}
					case 1: {
						struct BlocksParam {
							Ngs2VoiceParamHeader     header;
							const uint8_t*           data;
							uint32_t                 flags, count;
							const Ngs2WaveformBlock* blocks;
						};
						const auto& blocks = *reinterpret_cast<const BlocksParam*>(param);
						EXIT_NOT_IMPLEMENTED(!voice->accepts_blocks ||
						                     (blocks.flags != 0 && blocks.flags != 0x11));
						voice->accepts_blocks = (blocks.flags & 1u) != 0;
						for (uint32_t i = 0; i < blocks.count; ++i) {
							const auto& block = blocks.blocks[i];
							EXIT_NOT_IMPLEMENTED(block.num_repeats != 0 || block.num_samples == 0);
							EXIT_NOT_IMPLEMENTED(
							    voice->decoder == nullptr &&
							    (uint64_t(block.num_skip_samples) + block.num_samples) *
							            voice->channels * 2 >
							        block.data_size);
							voice->blocks.push_back({blocks.data + block.data_offset, block});
						}
						break;
					}
					case 5:
						EXIT_NOT_IMPLEMENTED(*reinterpret_cast<const float*>(param + 1) != 1.0f);
						break;
					default: EXIT("unsupported sampler control: 0x%08" PRIx32 "\n", param->id);
				}
				break;
			}
			case 0x4002: {
				EXIT_NOT_IMPLEMENTED(voice->rack->type != Ngs2RackType::CustomSubmixer ||
				                     (param->id & 0xffffu) != 0);
				const auto* channels   = reinterpret_cast<const uint32_t*>(param + 1);
				EXIT_NOT_IMPLEMENTED(channels[0] != channels[1]);
				voice->channels = channels[0];
				break;
			}
			case 0x4003:
				EXIT_NOT_IMPLEMENTED(voice->rack->type != Ngs2RackType::CustomMastering);
				break;
			default: EXIT("unknown rack_id: 0x%" PRIx32 "\n", rack_id);
		}

		if (param->next == 0) {
			break;
		}
		param = reinterpret_cast<const Ngs2VoiceParamHeader*>(reinterpret_cast<uintptr_t>(param) +
		                                                      param->next);
	}

	return OK;
}

int KYTY_SYSV_ABI Ngs2VoiceRunCommands(uintptr_t voice_handle, const void* commands,
                                       size_t num_commands) {
	struct Command {
		uint32_t id;
		uint8_t  flags, type;
		uint16_t count;
		union {
			uint32_t     u;
			int32_t      i;
			float        f;
			const float* levels;
		} value;
	};
	auto*             voice = reinterpret_cast<Ngs2VoiceInternal*>(voice_handle);
	Common::LockGuard lock(voice->rack->ngs->mutex);
	const auto*       params = static_cast<const Command*>(commands);
	for (size_t i = 0; i < num_commands; ++i) {
		const auto& p     = params[i];
		const auto  index = p.id >> 24;
		switch (p.id & 0xffffffu) {
			case 2:
				EXIT_NOT_IMPLEMENTED(p.type != 4);
				voice->SetEvent(p.value.u);
				break;
			case 5:
				EXIT_NOT_IMPLEMENTED(p.type != 0x11);
				voice->SetMatrix(index, p.value.levels, p.count);
				break;
			case 6:
				EXIT_NOT_IMPLEMENTED(p.type != 1);
				voice->SetVolume(index, p.value.f);
				break;
			case 7:
				EXIT_NOT_IMPLEMENTED(p.type != 3);
				voice->SetPortMatrix(index, p.value.i);
				break;
			default: EXIT("unknown command: 0x%08" PRIx32 "\n", p.id);
		}
	}
	return OK;
}

int KYTY_SYSV_ABI Ngs2VoiceGetState(uintptr_t voice_handle, Ngs2VoiceState* state,
                                    size_t state_size) {
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(state == nullptr);
	EXIT_NOT_IMPLEMENTED(voice_handle == 0);

	auto* voice = reinterpret_cast<Ngs2VoiceInternal*>(voice_handle);

	Common::LockGuard lock(voice->rack->ngs->mutex);

	switch (voice->rack->type) {
		case Ngs2RackType::Submixer: {
			EXIT_NOT_IMPLEMENTED(state_size != sizeof(Ngs2SubmixerVoiceState));
			auto* submixer                    = reinterpret_cast<Ngs2SubmixerVoiceState*>(state);
			*submixer                         = {};
			submixer->voice_state.state_flags = Ngs2GetStateFlags(voice);
			break;
		}
		case Ngs2RackType::CustomMastering: {
			const auto configured_size =
			    voice->rack->option.custom_mastering.custom_rack_option.state_size;
			EXIT_NOT_IMPLEMENTED(configured_size < sizeof(Ngs2CustomMasteringVoiceState));
			EXIT_NOT_IMPLEMENTED(state_size != configured_size);
			std::memset(state, 0, state_size);
			auto* mastering = reinterpret_cast<Ngs2CustomMasteringVoiceState*>(state);
			mastering->voice_state.state_flags = Ngs2GetStateFlags(voice);
			break;
		}
		case Ngs2RackType::Sampler:
		case Ngs2RackType::CustomSampler: {
			if (state_size != sizeof(Ngs2SamplerVoiceState)) {
				LOGF("\t warning: sampler state_size = 0x%016" PRIx64 ", expected 0x%016" PRIx64
				     "\n",
				     static_cast<uint64_t>(state_size),
				     static_cast<uint64_t>(sizeof(Ngs2SamplerVoiceState)));
			}
			std::memset(state, 0, state_size);

			state->state_flags = Ngs2GetStateFlags(voice);
			if (state_size < sizeof(Ngs2SamplerVoiceState)) {
				break;
			}

			auto* sampler                = reinterpret_cast<Ngs2SamplerVoiceState*>(state);
			sampler->envelope_height     = 1.0f;
			sampler->peak_height         = 0.0f;
			sampler->reserved            = 0;
			sampler->num_decoded_samples = 0;
			sampler->user_data           = 0;
			sampler->waveform_data       = nullptr;
			break;
		}
		default: EXIT("unknown type: %s\n", magic_enum::enum_name(voice->rack->type));
	}

	return OK;
}

int KYTY_SYSV_ABI Ngs2VoiceGetStateFlags(uintptr_t voice_handle, uint32_t* state_flags) {
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(state_flags == nullptr);
	EXIT_NOT_IMPLEMENTED(voice_handle == 0);

	auto* voice = reinterpret_cast<Ngs2VoiceInternal*>(voice_handle);

	Common::LockGuard lock(voice->rack->ngs->mutex);

	*state_flags = Ngs2GetStateFlags(voice);

	return OK;
}

} // namespace Ngs2

} // namespace Libs::Audio
