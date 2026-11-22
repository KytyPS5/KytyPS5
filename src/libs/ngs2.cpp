#include "libs/ngs2.h"

#include "common/assert.h"
#include "common/logging/log.h"
#include "common/threads.h"
#include "kernel/fileSystem.h"
#include "libatrac9.h"
#include "libs/ajm/atrac9_decoder.h"
#include "libs/errno.h"
#include "libs/libs.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstring>
#include <deque>
#include <fstream>
#include <limits>
#include <magic_enum.hpp>
#include <memory>
#include <numbers>
#include <thread>
#include <utility>
#include <vector>

namespace Libs::Audio {

namespace Ngs2 {

LIB_NAME("Ngs2", "Ngs2");

constexpr int32_t NGS2_ERROR_INVALID_SYSTEM_HANDLE       = static_cast<int32_t>(0x804a8201u);
constexpr int32_t NGS2_ERROR_INVALID_OUT_ADDRESS         = static_cast<int32_t>(0x804a8010u);
constexpr int32_t NGS2_ERROR_INVALID_NUM_WAVEFORM_BLOCKS = static_cast<int32_t>(0x804a8053u);
constexpr int32_t NGS2_ERROR_FULL_WAVEFORM_BLOCKS        = static_cast<int32_t>(0x804a8054u);
constexpr int32_t NGS2_ERROR_INVALID_WAVEFORM_DATA       = static_cast<int32_t>(0x804a8430u);
constexpr int32_t NGS2_ERROR_INVALID_WAVEFORM_FORMAT     = static_cast<int32_t>(0x804a8431u);
constexpr int32_t NGS2_ERROR_UNKNOWN_WAVEFORM_FORMAT     = static_cast<int32_t>(0x804a8432u);
constexpr int32_t NGS2_ERROR_INVALID_OUT_SIZE            = static_cast<int32_t>(0x804a8011u);
constexpr int32_t NGS2_ERROR_INVALID_SAMPLE_RATE         = static_cast<int32_t>(0x804a8051u);
constexpr int32_t NGS2_ERROR_INVALID_RACK_HANDLE         = static_cast<int32_t>(0x804a8202u);
constexpr int32_t NGS2_ERROR_INVALID_VOICE_HANDLE        = static_cast<int32_t>(0x804a8203u);
constexpr int32_t NGS2_ERROR_INVALID_RACK_MODULE_INDEX   = static_cast<int32_t>(0x804a8256u);
constexpr int32_t NGS2_ERROR_INVALID_RACK_QUERY_ID       = static_cast<int32_t>(0x804a8304u);
constexpr int32_t NGS2_ERROR_INVALID_NUM_GRAIN_SAMPLES   = static_cast<int32_t>(0x804a8310u);
constexpr int32_t NGS2_ERROR_INVALID_PARAM_ID            = static_cast<int32_t>(0x804a835au);
constexpr int32_t NGS2_ERROR_INVALID_SYSTEM_QUERY_ID     = static_cast<int32_t>(0x804a835bu);
constexpr int32_t NGS2_ERROR_SYSTEM_INDEX_RANGE          = static_cast<int32_t>(0x804a8410u);

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

struct Ngs2RackInfo {
	char                  name[64]    = {};
	uintptr_t             rack_handle = 0;
	Ngs2ContextBufferInfo buffer_info;
	uintptr_t             owner_system_handle       = 0;
	uint32_t              type                      = 0;
	uint32_t              rack_id                   = 0;
	uint32_t              uid                       = 0;
	uint32_t              min_grain_samples         = 0;
	uint32_t              max_grain_samples         = 0;
	uint32_t              max_voices                = 0;
	uint32_t              max_channel_works         = 0;
	uint32_t              max_inputs                = 0;
	uint32_t              max_matrices              = 0;
	uint32_t              max_ports                 = 0;
	uint32_t              max_voice_channels        = 0;
	uint32_t              max_output_channels       = 0;
	uint32_t              num_modules               = 0;
	uint32_t              num_buffers               = 0;
	uint32_t              state_flags               = 0;
	float                 last_process_ratio        = 0.0f;
	uint64_t              last_processed_tick       = 0;
	uint64_t              render_count              = 0;
	uint32_t              active_voice_count        = 0;
	uint32_t              active_channel_work_count = 0;
};

struct Ngs2CustomModuleInfo {
	uint32_t module_id        = 0;
	uint32_t source_buffer_id = 0;
	uint32_t extra_buffer_id  = 0;
	uint32_t dest_buffer_id   = 0;
	uint32_t state_offset     = 0;
	uint32_t state_size       = 0;
	uint32_t reserved         = 0;
	uint32_t reserved2        = 0;
};

struct Ngs2VoiceMatrixInfo {
	uint32_t num_levels      = 0;
	float    levels[37 * 37] = {};
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
	uint32_t frame_offset  = 0;
	uint32_t frame_margin  = 0;
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

struct Ngs2Internal {
	Ngs2SystemOption                        option;
	Ngs2ContextBufferInfo                   buffer_info;
	Ngs2BufferAllocator                     allocator;
	Ngs2Internal*                           next          = nullptr;
	uint64_t                                render_count  = 0;
	uint32_t                                uid           = 0;
	uintptr_t                               user_data     = 0;
	std::array<std::array<uint8_t, 24>, 32> slot_data     = {};
	std::array<bool, 32>                    slot_set      = {};
	float                                   unnamed_param = 0.0f;
	Common::Mutex                           mutex;
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

struct Ngs2UserFxProcessContext {
	float**                  channels;
	std::array<uintptr_t, 3> user_data;
	uint32_t                 flags;
	uint32_t                 num_channels;
	uint32_t                 grain_samples;
	uint32_t                 sample_rate;
};

struct Ngs2UserFx2Context {
	void*     common;
	void*     param;
	void*     work;
	uintptr_t user_data;
	uint32_t  max_voices;
	uint32_t  voice_index;
	uint64_t  reserved[4] {};
};

struct Ngs2UserFx2ProcessContext {
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

struct Ngs2UserFx2Option {
	Ngs2CustomModuleOption header;
	int                    KYTY_SYSV_ABI (*setup)(Ngs2UserFx2Context*);
	int                    KYTY_SYSV_ABI (*cleanup)(Ngs2UserFx2Context*);
	uintptr_t              control;
	int                    KYTY_SYSV_ABI (*process)(Ngs2UserFx2ProcessContext*);
	size_t                 common_size;
	size_t                 param_size;
	size_t                 work_size;
	uintptr_t              user_data;
};

static bool Ngs2RackIsCustom(Ngs2RackType type) {
	switch (type) {
		case Ngs2RackType::CustomSubmixer:
		case Ngs2RackType::CustomMastering:
		case Ngs2RackType::CustomSampler: return true;
		default: return false;
	}
}

struct Ngs2RackInternal {
	Ngs2Internal*                        ngs          = nullptr;
	Ngs2RackInternal*                    next         = nullptr;
	Ngs2RackType                         type         = Ngs2RackType::Sampler;
	uint32_t                             rack_id      = 0;
	uint32_t                             uid          = 0;
	char                                 name[64]     = {};
	uintptr_t                            user_data    = 0;
	std::atomic<uint32_t>                   lock_depth   = 0;
	std::atomic<std::thread::id>            lock_owner   = {};
	uint64_t                             render_count = 0;
	Ngs2RackOptionUnion                  option;
	Ngs2ContextBufferInfo                buffer_info;
	Ngs2BufferAllocator                  allocator;
	std::array<std::vector<uint8_t>, 24> common;
	std::array<Ngs2UserFx2Option, 24>    fx {};

	[[nodiscard]] uint32_t NumBuffers() const {
		return Ngs2RackIsCustom(type) ? option.custom_sampler.custom_rack_option.num_buffers : 1;
	}

	[[nodiscard]] uint32_t PortBufferId(size_t port) const {
		const auto& custom = option.custom_sampler.custom_rack_option;
		return Ngs2RackIsCustom(type) ? custom.port[port].source_buffer_id : 0;
	}
};

enum class Ngs2VoicePlayState : uint32_t { Empty = 0, Playing = 3, Paused = 5, Stopped = 0xb };

struct Ngs2SamplerFilterParam {
	uint32_t index;
	uint32_t location;
	uint32_t type;
	uint64_t channel_mask;
	float    frequency;
	float    q;
	float    level;
	uint32_t reserved[3];
};
static_assert(sizeof(Ngs2SamplerFilterParam) == 48);
static_assert(offsetof(Ngs2SamplerFilterParam, type) == 8 &&
              offsetof(Ngs2SamplerFilterParam, channel_mask) == 16 &&
              offsetof(Ngs2SamplerFilterParam, frequency) == 24);

struct Ngs2SamplerFilter {
	struct History {
		double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
	};
	bool                   enabled      = false;
	bool                   warned       = false;
	uint64_t               channel_mask = 0;
	double                 b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
	Ngs2SamplerFilterParam param {};
	uint32_t               design_rate = 0;
	std::vector<History>   history;

	void Configure(const Ngs2SamplerFilterParam& filter_param, uint32_t rate, uint32_t channels) {
		param       = filter_param;
		design_rate = rate;
		if (param.type == 0) {
			enabled = false;
			history.clear();
			return;
		}
		if (param.location != 1 || param.type != 1 || !std::isfinite(param.frequency) ||
		    param.frequency < 0 || !std::isfinite(param.q) || param.q <= 0 ||
		    !std::isfinite(param.level) || param.level < 0 || rate == 0) {
			enabled = false;
			history.clear();
			if (!warned) {
				Log::WriteToConsoleAndLog(fmt::sprintf(
				    "warning: unsupported NGS2 sampler filter (location=%u, type=%u)\n",
				    param.location, param.type));
				warned = true;
			}
			return;
		}
		channel_mask = param.channel_mask;
		history.resize(channels);
		enabled = true;
		if (param.frequency == 0 || param.frequency >= rate * 0.5) {
			b0 = param.frequency == 0 ? 0 : param.level;
			b1 = b2 = a1 = a2 = 0;
			return;
		}
		// RBJ low-pass biquad, normalized by a0 (Audio EQ Cookbook).
		const double omega      = (2.0 * std::numbers::pi) * param.frequency / rate;
		const double cosine     = std::cos(omega);
		const double alpha      = std::sin(omega) / (2.0 * param.q);
		const double inverse_a0 = 1.0 / (1.0 + alpha);
		b0                      = (1.0 - cosine) * 0.5 * inverse_a0 * param.level;
		b1                      = 2.0 * b0;
		b2                      = b0;
		a1                      = -2.0 * cosine * inverse_a0;
		a2                      = (1.0 - alpha) * inverse_a0;
	}

	// The coefficients are designed for the rate the samples are rendered at;
	// redesign them when the system rate changes after configuration.
	void EnsureRate(uint32_t rate, uint32_t channels) {
		if (param.type != 0 && design_rate != rate) {
			Configure(param, rate, channels);
		}
	}

	bool Bypasses(size_t channel) const {
		return channel < 64 && (channel_mask & (uint64_t {1} << channel)) != 0;
	}

	bool HasHistory() const {
		if (!enabled) {
			return false;
		}
		for (size_t c = 0; c < history.size(); ++c) {
			if (Bypasses(c)) {
				continue;
			}
			const auto& h = history[c];
			if (h.x1 != 0 || h.x2 != 0 || h.y1 != 0 || h.y2 != 0) {
				return true;
			}
		}
		return false;
	}

	void Process(float* samples, uint32_t channels, uint32_t grain) {
		if (!enabled) {
			return;
		}
		if (history.size() != channels) {
			history.assign(channels, {});
		}
		for (uint32_t c = 0; c < channels; ++c) {
			if (Bypasses(c)) {
				continue;
			}
			auto& h = history[c];
			for (uint32_t i = 0; i < grain; ++i) {
				const double x = samples[c * grain + i];
				double       y = b0 * x + b1 * h.x1 + b2 * h.x2 - a1 * h.y1 - a2 * h.y2;
				if (std::abs(y) < 1e-20) {
					y = 0;
				}
				h.x2                   = h.x1;
				h.x1                   = x;
				h.y2                   = h.y1;
				h.y1                   = y;
				samples[c * grain + i] = static_cast<float>(y);
			}
		}
	}

	void Process(std::vector<float>& samples, uint32_t channels, uint32_t grain) {
		Process(samples.data(), channels, grain);
	}
};

// I3DL2-style environment reverb: an early reflection plus a damped comb/allpass tail per channel.
// The tail gains follow the room/reflections/reverb levels and the decay time; diffusion and
// density are not modelled.
struct Ngs2ReverbState {
	static constexpr size_t COMBS = 6;
	struct Params {
		float    wet               = 1.0f;
		float    dry               = 0.0f;
		int32_t  room              = -10000;
		int32_t  reflections       = -10000;
		int32_t  reverb            = -10000;
		float    decay_time        = 1.0f;
		float    hf_ratio          = 0.5f;
		float    reflections_delay = 0.02f;
		float    reverb_delay      = 0.04f;
		int32_t  room_hf           = 0;
		float    hf_reference      = 5000.0f;
		uint32_t channel_mask      = 0x8;
	};
	struct Comb {
		std::vector<float> buffer;
		size_t             pos         = 0;
		float              feedback    = 0.0f;
		double             lowpass_mix = 1.0;
		double             lowpass     = 0.0;
	};
	struct Allpass {
		std::vector<float> buffer;
		size_t             pos = 0;
	};
	struct Channel {
		std::vector<float>      ring;
		size_t                  pos = 0;
		std::array<Comb, COMBS> combs;
		std::array<Allpass, 2>  allpasses;
		double                  room_lowpass = 0.0;

		void Clear() {
			std::ranges::fill(ring, 0.0f);
			pos          = 0;
			room_lowpass = 0.0;
			for (auto& comb: combs) {
				std::ranges::fill(comb.buffer, 0.0f);
				comb.pos     = 0;
				comb.lowpass = 0.0;
			}
			for (auto& allpass: allpasses) {
				std::ranges::fill(allpass.buffer, 0.0f);
				allpass.pos = 0;
			}
		}
	};

	Params               params;
	std::vector<Channel> channels;
	uint32_t             rate            = 0;
	size_t               reflect_tap     = 0;
	size_t               late_tap        = 0;
	float                reflect_gain    = 0.0f;
	float                late_gain       = 0.0f;
	double               room_filter_mix = 1.0;
	bool                 active          = false;
	size_t               quiet_samples   = 0;

	static float MillibelsToGain(int32_t millibels) {
		if (millibels <= -10000) {
			return 0.0f;
		}
		return std::pow(10.0f, static_cast<float>(std::min(millibels, 2000)) / 2000.0f);
	}

	static size_t Samples(float milliseconds, uint32_t rate) {
		return std::max<size_t>(1, static_cast<size_t>(milliseconds * 0.001f * float(rate) + 0.5f));
	}

	void Reset() {
		channels.clear();
		rate          = 0;
		active        = false;
		quiet_samples = 0;
	}

	void Prepare(uint32_t num_channels, uint32_t sample_rate) {
		if (channels.size() == num_channels && rate == sample_rate) {
			return;
		}
		static constexpr std::array<float, COMBS> comb_ms    = {29.7f, 37.1f, 41.1f,
		                                                        43.7f, 47.3f, 53.1f};
		static constexpr std::array<float, 2>     allpass_ms = {5.0f, 1.7f};
		channels.assign(num_channels, Channel {});
		rate          = sample_rate;
		active        = false;
		quiet_samples = 0;
		for (size_t c = 0; c < channels.size(); ++c) {
			auto& channel = channels[c];
			channel.ring.assign(Samples(500.0f, rate), 0.0f);
			// Slightly different delays per channel decorrelate the tails.
			const float spread = 1.0f + 0.03f * float(c);
			for (size_t i = 0; i < COMBS; ++i) {
				channel.combs[i].buffer.assign(Samples(comb_ms[i] * spread, rate), 0.0f);
			}
			for (size_t i = 0; i < channel.allpasses.size(); ++i) {
				channel.allpasses[i].buffer.assign(Samples(allpass_ms[i] * spread, rate), 0.0f);
			}
		}
		Apply();
	}

	void Apply() {
		const float decay = std::clamp(params.decay_time, 0.1f, 20.0f);
		const float ratio = std::clamp(params.hf_ratio, 0.01f, 2.0f);
		for (size_t c = 0; c < channels.size(); ++c) {
			auto& channel = channels[c];
			if (c < 32 && (params.channel_mask & (uint32_t {1} << c)) != 0) {
				channel.Clear();
			}
			for (auto& comb: channel.combs) {
				const float  seconds = float(comb.buffer.size()) / float(rate);
				const float  mid     = std::pow(10.0f, -3.0f * seconds / decay);
				const double high =
				    std::min(std::pow(10.0, -3.0 * seconds / (decay * ratio)), 0.999);
				comb.feedback = mid;
				// Store the complement directly to retain very small HF decay ratios.
				comb.lowpass_mix = 2.0 * high / (mid + high);
			}
		}
		// Unity-DC one-pole low pass with the requested attenuation at HFReference.
		// Compute its input coefficient directly in double precision, including -100 dB.
		const double gain = std::pow(10.0, std::clamp(params.room_hf, -10000, 0) / 2000.0);
		room_filter_mix   = 1.0;
		if (gain < 1.0 && rate != 0) {
			const double frequency =
			    std::min<double>(std::clamp(params.hf_reference, 20.0f, 20000.0f), rate * 0.5);
			const double k  = 1.0 - std::cos(2.0 * std::numbers::pi * frequency / rate);
			const double g2 = gain * gain;
			const double a  = g2 * k;
			room_filter_mix = 2.0 * a / (std::sqrt(a * a + 2.0 * a * (1.0 - g2)) + a);
		}
		const size_t max_tap = channels.empty() ? 0 : channels[0].ring.size() - 1;
		reflect_tap  = params.reflections_delay == 0.0f
		                   ? 0
		                   : std::min(Samples(params.reflections_delay * 1000.0f, rate), max_tap);
		late_tap     = std::min(reflect_tap + (params.reverb_delay == 0.0f
		                                           ? 0
		                                           : Samples(params.reverb_delay * 1000.0f, rate)),
		                        max_tap);
		reflect_gain = MillibelsToGain(params.room + params.reflections);
		late_gain    = MillibelsToGain(params.room + params.reverb);
	}

	// Mixes the dry signal and the reverb tail in place; returns whether there is any output.
	bool Process(std::vector<float>& samples, uint32_t grain, bool has_input) {
		if (!has_input && !active) {
			return false;
		}
		float peak = 0.0f;
		for (size_t c = 0; c < channels.size(); ++c) {
			auto& channel = channels[c];
			auto* data    = samples.data() + c * grain;
			if (c < 32 && (params.channel_mask & (uint32_t {1} << c)) != 0) {
				for (uint32_t i = 0; i < grain; ++i) {
					data[i] *= params.dry;
					peak = std::max(peak, std::abs(data[i]));
				}
				continue;
			}
			const size_t ring_size = channel.ring.size();
			for (uint32_t i = 0; i < grain; ++i) {
				const float input         = data[i];
				channel.ring[channel.pos] = input;
				const float reflection =
				    channel.ring[(channel.pos + ring_size - reflect_tap) % ring_size];
				const float late_input =
				    channel.ring[(channel.pos + ring_size - late_tap) % ring_size];
				channel.pos = (channel.pos + 1) % ring_size;

				float tail = 0.0f;
				for (auto& comb: channel.combs) {
					const float out = comb.buffer[comb.pos];
					comb.lowpass += comb.lowpass_mix * (out - comb.lowpass);
					comb.buffer[comb.pos] = late_input + comb.feedback * comb.lowpass;
					comb.pos              = (comb.pos + 1) % comb.buffer.size();
					tail += out;
				}
				tail *= 0.25f;
				for (auto& allpass: channel.allpasses) {
					const float delayed         = allpass.buffer[allpass.pos];
					const float stored          = tail + 0.5f * delayed;
					tail                        = delayed - 0.5f * stored;
					allpass.buffer[allpass.pos] = stored;
					allpass.pos                 = (allpass.pos + 1) % allpass.buffer.size();
				}
				const float wet = reflection * reflect_gain + tail * late_gain;
				channel.room_lowpass += room_filter_mix * (wet - channel.room_lowpass);
				data[i] =
				    input * params.dry + static_cast<float>(channel.room_lowpass) * params.wet;
				peak = std::max(peak, std::abs(data[i]));
			}
		}
		if (has_input || peak >= 1e-6f) {
			active        = true;
			quiet_samples = 0;
			return true;
		}
		// Nothing is delayed for longer than the ring, so a silent ring's worth means the tail is
		// over.
		quiet_samples += grain;
		if (quiet_samples > channels[0].ring.size() + Samples(100.0f, rate)) {
			active = false;
			for (auto& channel: channels) {
				channel.Clear();
			}
		}
		return active;
	}
};

// --- DSP Pitch Shift Subsystem  ---
struct Ngs2PitchShiftStep {
	uint32_t consumed   = 0;
	uint32_t produced   = 0;
	uint32_t inputTime  = 0;
	uint32_t outputTime = 0;
};

struct Ngs2PitchShiftChannel {
	std::array<float, 7>    resampleHistory {};
	std::array<float, 960>  overlapHistory {};
	std::array<float, 15>   coefficients {};
	std::array<float, 142>  filterHistory {};
	std::array<float, 3008> input {};
	std::array<float, 1088> output {};
	uint32_t                filterCursor    = 0;
	uint32_t                copyRemaining   = 0;
	uint32_t                inputCount      = 0;
	uint32_t                outputCount     = 0;
	uint32_t                inputTime       = 0;
	uint32_t                outputTime      = 0;
	uint32_t                drain_remaining = 0;
	bool                    started         = false;

	static float TableSin(float angle) {
		constexpr float PI     = std::numbers::pi_v<float>;
		constexpr float TWO_PI = 2.0f * PI;
		if (angle < 0.0f || angle > TWO_PI) {
			angle -= std::floor(angle / TWO_PI) * TWO_PI;
		}
		const float phase    = (angle / TWO_PI) * 256.0f;
		const auto  index    = static_cast<uint32_t>(phase);
		const float fraction = phase - static_cast<float>(index);
		const auto  sample   = [](uint32_t i) {
			if ((i & 127u) == 0) return 0.0f;
			return static_cast<float>(
			    std::sin(static_cast<double>(i & 255u) * (2.0 * std::numbers::pi / 256.0)));
		};
		return (1.0f - fraction) * sample(index) + fraction * sample(index + 1);
	}

	void SetPitch(float ratio) {
		constexpr float PI     = std::numbers::pi_v<float>;
		constexpr float TWO_PI = 2.0f * PI;
		coefficients.fill(0.0f);
		const float rate = ratio * 48000.0f;
		if (rate <= 48000.0f) {
			coefficients[7] = 1.0f;
			return;
		}
		const float cutoff = 15840.0009765625f / rate;
		for (uint32_t i = 0; i < coefficients.size(); ++i) {
			const float windowAngle = (static_cast<float>(i) * TWO_PI) / 14.0f;
			const float window      = -0.46f * TableSin(windowAngle + PI / 2.0f) + 0.54f;
			const auto  tap         = static_cast<int32_t>(i) - 7;
			const float sinc = tap == 0 ? 2.0f * cutoff
			                            : TableSin(static_cast<float>(tap) * (cutoff * TWO_PI)) /
			                                  (static_cast<float>(tap) * PI);
			coefficients[14 - i] = sinc * window;
		}
	}

	static float FilterSample(const Ngs2PitchShiftChannel& channel, uint32_t index) {
		float    result = 0.0f;
		uint32_t tap    = 0;
		while (tap < 15) {
			const auto           count = std::min(15u - tap, 142u - index);
			std::array<float, 4> lanes {};
			uint32_t             j = 0;
			for (; j + 4 <= count; j += 4) {
				for (uint32_t lane = 0; lane < 4; ++lane) {
					lanes[lane] += channel.coefficients[tap + j + lane] *
					               channel.filterHistory[index + j + lane];
				}
			}
			result += ((lanes[0] + lanes[1]) + lanes[2]) + lanes[3];
			for (; j < count; ++j) {
				result += channel.coefficients[tap + j] * channel.filterHistory[index + j];
			}
			tap += count;
			index = (index + count) % 142;
		}
		return result;
	}

	static uint32_t FindOverlap(const float* reference, const float* candidate, uint32_t first,
	                            uint32_t last) {
		float    best     = 2147483648.0f;
		uint32_t selected = 0;
		for (uint32_t offset = first; offset <= last; offset += 8) {
			std::array<float, 4> lanes {};
			for (uint32_t block = 0; block < 8; ++block) {
				for (uint32_t lane = 0; lane < 4; ++lane) {
					lanes[lane] += std::abs(reference[block * 16 + lane] -
					                        candidate[offset + block * 16 + lane]);
				}
			}
			const float difference = ((lanes[0] + lanes[1]) + lanes[2]) + lanes[3];
			if (best > difference) {
				best     = difference;
				selected = offset;
			}
		}
		return selected;
	}

	static void Crossfade(const float* from, const float* to, float* dest, uint32_t count) {
		const float          step = 1.0f / static_cast<float>(count);
		std::array<float, 4> fade {0.0f, step, step + step, step * 3.0f};
		for (uint32_t i = 0; i < count; i += 4) {
			for (uint32_t lane = 0; lane < 4; ++lane) {
				dest[i + lane] = (1.0f - fade[lane]) * from[i + lane] + fade[lane] * to[i + lane];
				fade[lane] += step * 4.0f;
			}
		}
	}

	static Ngs2PitchShiftStep Stretch(Ngs2PitchShiftChannel& channel, const float* in, float* out,
	                                  float ratio) {
		Ngs2PitchShiftStep step {};
		if (channel.copyRemaining != 0) {
			step.consumed = std::min(channel.copyRemaining, 960u);
			step.produced = step.consumed;
			channel.copyRemaining -= step.consumed;
			std::copy_n(in, step.produced, out);
		} else if (std::abs(ratio - 1.0f) < 0.01f) {
			step = {960, 960, 960, 960};
			std::copy_n(in, 960, out);
		} else if (ratio < 1.0f) {
			const auto overlap = FindOverlap(in, in, 120, 952);
			Crossfade(in, in + overlap, out, overlap);
			const auto duration = static_cast<uint32_t>(
			    ((ratio * static_cast<float>(overlap)) / (1.0f - ratio)) + 0.5f);
			channel.copyRemaining = duration - overlap;
			step                  = {overlap * 2, overlap, overlap + duration, duration};
		} else {
			const auto overlap = FindOverlap(channel.overlapHistory.data() + 840, in, 0, 832);
			const auto length  = overlap + 120;
			Crossfade(in, channel.overlapHistory.data() + 960 - length, out, length);
			const auto duration =
			    static_cast<uint32_t>((static_cast<float>(length) / (ratio - 1.0f)) + 0.5f);
			channel.copyRemaining = duration;
			return {0, length, duration, duration + length};
		}
		if (step.consumed >= 960) {
			std::copy_n(in + step.consumed - 960, 960, channel.overlapHistory.begin());
		} else if (step.consumed != 0) {
			std::move(channel.overlapHistory.begin() + step.consumed, channel.overlapHistory.end(),
			          channel.overlapHistory.begin());
			std::copy_n(in, step.consumed, channel.overlapHistory.end() - step.consumed);
		}
		return step;
	}

	static const std::array<std::array<float, 8>, 256>& ResampleKernel() {
		static const auto table = [] {
			std::array<std::array<float, 8>, 256> result {};
			for (uint32_t phase = 0; phase < 256; ++phase) {
				for (uint32_t tap = 0; tap < 8; ++tap) {
					const double x =
					    static_cast<double>(tap) - 4.0 + static_cast<double>(phase) / 256.0;
					const double sinc =
					    x == 0.0 ? 1.0 : std::sin(std::numbers::pi * x) / (std::numbers::pi * x);
					result[phase][tap] = static_cast<float>(
					    sinc * (0.5 + 0.5 * std::cos(std::numbers::pi * x / 4.0)));
				}
			}
			return result;
		}();
		return table;
	}

	static void Resample(Ngs2PitchShiftChannel& channel, float* dest, uint32_t consumed) {
		std::array<float, 1095> scratch {};
		std::copy(channel.resampleHistory.begin(), channel.resampleHistory.end(), scratch.begin());
		std::copy_n(channel.output.begin(), consumed, scratch.begin() + 7);
		std::copy_n(scratch.begin() + consumed, 7, channel.resampleHistory.begin());
		const float step   = static_cast<float>(consumed) / 64.0f;
		const auto& kernel = ResampleKernel();
		for (uint32_t i = 0; i < 64; ++i) {
			const float          position = static_cast<float>(i) * step;
			const auto           index    = static_cast<uint32_t>(position);
			const float          phase = ((static_cast<float>(index) - position) + 1.0f) * 256.0f;
			const auto           phaseIndex = static_cast<uint32_t>(phase);
			const float          fraction   = phase - static_cast<float>(phaseIndex);
			std::array<float, 4> lanes {};
			for (uint32_t lane = 0; lane < 4; ++lane) {
				const auto coefficient = [&](uint32_t tap) {
					return (1.0f - fraction) * kernel[phaseIndex & 255u][tap] +
					       fraction * kernel[(phaseIndex + 1) & 255u][tap];
				};
				lanes[lane] = coefficient(lane) * scratch[index + lane] +
				              coefficient(lane + 4) * scratch[index + lane + 4];
			}
			dest[i] = (lanes[0] + lanes[1]) + (lanes[2] + lanes[3]);
		}
	}

	// True while stretched audio is still buffered inside the channel; the
	// tail must be flushed with silence once the input stream ends.
	bool HasPending() const { return inputCount != 0 || outputCount != 0; }

	// Feeds real input while the stream plays; with a null source it feeds
	// silence until the buffered tail is exhausted, then drops the remaining
	// silent pipeline state so the channel returns to idle.
	void ProcessOrDrain(const float* source, float* dest, float ratio) {
		if (source != nullptr) {
			drain_remaining = 0;
			ProcessBlock(source, dest, ratio);
			return;
		}
		if (drain_remaining == 0) {
			if (!HasPending()) {
				std::fill_n(dest, 64, 0.0f);
				return;
			}
			drain_remaining = inputCount + outputCount + 2048;
		}
		ProcessBlock(nullptr, dest, ratio);
		if (drain_remaining > 64) {
			drain_remaining -= 64;
		} else {
			drain_remaining = 0;
			inputCount = outputCount = inputTime = outputTime = 0;
		}
	}

	// Processes one 64-sample block; a null source feeds silence so a finished
	// stream can still drain the delayed tail.
	void ProcessBlock(const float* source, float* dest, float ratio) {
		std::array<float, 64> silence {};
		const float*          in = source != nullptr ? source : silence.data();
		for (uint32_t i = 0; i < 64; ++i) {
			filterHistory[filterCursor] = in[i];
			filterCursor                = (filterCursor + 1) % 142;
		}
		if (inputCount + 64 > input.size()) {
			// Silenzio durante lo svuotamento: scarta i campioni piu' vecchi.
			std::move(input.begin() + 64, input.end(), input.begin());
			inputCount -= 64;
		}
		for (uint32_t i = 0; i < 64; ++i) {
			input[inputCount + i] = FilterSample(*this, (filterCursor + 64 + i) % 142);
		}
		inputCount += 64;
		if (inputCount <= 1984 && !started) {
			std::fill_n(dest, 64, 0.0f);
			return;
		}
		started = true;
		while (outputCount == 0 || outputCount * inputTime < outputTime * 64) {
			const auto step = Stretch(*this, input.data(), output.data() + outputCount, ratio);
			inputCount -= step.consumed;
			std::move(input.begin() + step.consumed, input.end(), input.begin());
			outputCount += step.produced;
			inputTime += step.inputTime;
			outputTime += step.outputTime;
		}
		const auto consumed = outputTime * 64 / inputTime;
		inputTime -= 64;
		outputTime -= consumed;
		Resample(*this, dest, consumed);
		outputCount -= consumed;
		std::move(output.begin() + consumed, output.end(), output.begin());
	}
};

static size_t Ngs2PcmSampleSize(uint32_t waveform_type) {
	switch (waveform_type) {
		case 0x11: return 1;
		case 0x12: return 2;
		case 0x14: return 3;
		case 0x16:
		case 0x18: return 4;
		case 0x1a: return 8;
		default: return 2;
	}
}

static float Ngs2ReadPcmSample(uint32_t waveform_type, const uint8_t* sample) {
	switch (waveform_type) {
		case 0x11: // 8-bit unsigned PCM
			return (static_cast<int>(sample[0]) - 128) / 128.0f;
		case 0x12: { // 16-bit signed PCM (I16L)
			int16_t val = 0;
			std::memcpy(&val, sample, sizeof(val));
			return val / 32768.0f;
		}
		case 0x14: { // 24-bit signed PCM (I24L)
			const uint32_t val = static_cast<uint32_t>(sample[0]) |
			                     (static_cast<uint32_t>(sample[1]) << 8) |
			                     (static_cast<uint32_t>(sample[2]) << 16);
			return (static_cast<int32_t>(val ^ 0x800000u) - 0x800000) / 8388608.0f;
		}
		case 0x16: { // 32-bit signed PCM (I32L)
			int32_t val = 0;
			std::memcpy(&val, sample, sizeof(val));
			return static_cast<float>(val / 2147483648.0);
		}
		case 0x18: { // 32-bit float PCM (F32L)
			float val = 0.0f;
			std::memcpy(&val, sample, sizeof(val));
			return val;
		}
		case 0x1a: { // 64-bit double PCM (F64L)
			double val = 0.0;
			std::memcpy(&val, sample, sizeof(val));
			return static_cast<float>(val);
		}
		default: return 0.0f;
	}
}

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
		size_t            data_cursor    = 0;
		uint32_t          repeated_count = 0;
		bool              canceled       = false;
	};
	struct Module {
		std::vector<uint8_t>     param, work, state;
		std::vector<float>       channel_matrix;
		void                     KYTY_SYSV_ABI (*handler)(Ngs2UserFxProcessContext*) = nullptr;
		std::array<uintptr_t, 3> user_data {};
		uint32_t                 flags          = 1;
		bool                     control_warned = false;
		bool                     render_warned  = false;

		// Custom Gain (0x15)
		std::array<float, 37> current_gain    = {};
		std::array<float, 37> target_gain     = {};
		bool                  gain_configured = false;

		// Custom Channel Mixer (0x17)
		bool mixer_configured = false;

		// Custom Pitch Shift (0x20)
		std::vector<Ngs2PitchShiftChannel> pitch_shift_channels;
		uint32_t                           pitch_shift_bypass_mask          = ~0u;
		float                              pitch_shift_ratio                = 1.0f;
		bool                               pitch_shift_reset                = true;
		bool                               pitch_shift_changed              = false;
		uint32_t                           pitch_shift_initialized_channels = 0;

		// Custom Filter (0x13)
		Ngs2SamplerFilter filter;
		bool              filter_configured = false;
	};
	Ngs2VoicePlayState              state          = Ngs2VoicePlayState::Empty;
	uint32_t                        state_flags    = 0;
	Ngs2RackInternal*               rack           = nullptr;
	uintptr_t                       callback       = 0;
	uintptr_t                       callback_data  = 0;
	uint32_t                        callback_flags = 0;
	std::deque<Block>               blocks;
	std::vector<Port>               ports;
	std::vector<std::vector<float>> matrices;
	std::vector<Module>             modules;
	std::vector<float>              samples;
	std::vector<float>              mix_samples;
	std::vector<float>              output_matrix;
	uint32_t                        channels  = 0;
	uint32_t                        output_id = 0;
	float                           fbw_level = 1.0f;
	float                           lfe_level = 1.0f;
	Ngs2ReverbState                 reverb;
	bool                            rendering   = false;
	bool                            rendered    = false;
	bool                            has_samples = false;

	struct MasteringLfe {
		bool     enabled       = false;
		uint32_t cutoff        = 120;
		uint32_t design_rate   = 0;
		uint32_t design_cutoff = 0;
		struct Section {
			float a1 = 0, a2 = 0, gain = 1.0f;
			struct History {
				float x1 = 0, x2 = 0, y1 = 0, y2 = 0;
			} history;
		};
		std::array<Section, 2> sections {};
	} mastering_lfe;

	struct MasteringLimiter {
		bool  enabled   = false;
		float threshold = 1.0f;
	} mastering_limiter;

	uint32_t peak_meter_mode = 0;
	float    peak_height     = 0.0f;

	uint32_t waveform_type = 0x12;
	bool     is_vag        = false;
	struct VagHistory {
		int32_t hist1 = 0;
		int32_t hist2 = 0;
	};
	std::array<VagHistory, 32> vag_history   = {};
	const uint8_t*             vag_next_data = nullptr;

	std::unique_ptr<Ajm::AjmAt9Decoder> decoder;
	std::vector<float>                  decoded_frame;
	std::vector<float>                  next_decoded_frame;
	std::vector<uint8_t>                compressed_input;
	uint32_t                            frame_cursor       = 0;
	uint32_t                            frame_samples      = 0;
	uint32_t                            next_frame_samples = 0;
	uint32_t                            next_frame_cursor  = 0;
	bool                                accepts_blocks     = true;
	uint32_t                            sample_rate        = 0;
	uint64_t                            sample_phase       = 0;
	uint64_t                            sample_step        = 0;
	uint64_t                            decoded_samples    = 0;
	uint64_t                            decoded_bytes      = 0;
	uint64_t                            source_generation  = 0;
	uint32_t                            duration_remaining = 0;
	uint32_t                            skip_remaining     = 0;
	const uint8_t*                      waveform_end       = nullptr;
	std::vector<Ngs2SamplerFilter>      filters;

	const uint8_t* WaveformData() const {
		if (blocks.empty()) {
			return waveform_end;
		}
		const auto& block = blocks.front();
		if (decoder != nullptr || is_vag) {
			return block.data + block.data_cursor;
		}
		// PCM never reads skipped samples, so the next read starts past a pending skip.
		const auto skip         = block.info.num_samples != 0 && block.data_cursor == 0
		                              ? block.info.num_skip_samples
		                              : skip_remaining;
		const auto sample_bytes = Ngs2PcmSampleSize(waveform_type);
		return block.data + std::min(block.data_cursor + size_t(skip) * channels * sample_bytes,
		                             block.info.data_size);
	}

	// Custom racks keep num_buffers consecutive channel-major buffers; other racks keep one.
	void ClearSamples(uint32_t grain) {
		samples.assign(size_t(grain) * channels * rack->NumBuffers(), 0.0f);
	}

	float* Buffer(uint32_t id, uint32_t grain) {
		return samples.data() + size_t(id) * grain * channels;
	}

	void MixChannels(const std::vector<float>& matrix, uint32_t buffer, uint32_t grain) {
		if (matrix.empty()) {
			return;
		}
		EXIT_NOT_IMPLEMENTED(channels > 37);
		auto* input = Buffer(buffer, grain);
		mix_samples.assign(size_t(channels) * grain, 0.0f);
		for (uint32_t dst = 0; dst < channels; ++dst) {
			for (uint32_t src = 0; src < channels; ++src) {
				const float level = matrix[dst * 37 + src];
				if (level == 0.0f) {
					continue;
				}
				for (uint32_t i = 0; i < grain; ++i) {
					mix_samples[dst * grain + i] += input[src * grain + i] * level;
				}
			}
		}
		std::copy(mix_samples.begin(), mix_samples.end(), input);
	}

	void ResetSetupState() {
		SetEvent(4);
		std::ranges::fill(ports, Port {});
		for (auto& matrix: matrices) {
			matrix.clear();
		}
		filters.clear();
		ResetModuleFilters();
	}

	// A channel layout change invalidates per-channel filter history kept by
	// custom modules; the guest must reconfigure them for the new layout.
	void ResetModuleFilters() {
		for (auto& module: modules) {
			module.filter            = {};
			module.filter_configured = false;
		}
	}

	void ClearWaveformBlocks() {
		++source_generation;
		blocks.clear();
		sample_phase       = 0;
		duration_remaining = 0;
		skip_remaining     = 0;
		compressed_input.clear();
		frame_cursor       = 0;
		frame_samples      = 0;
		next_frame_samples = 0;
		next_frame_cursor  = 0;
		waveform_end       = nullptr;
		vag_history        = {};
		vag_next_data      = nullptr;
	}

	void SetupSampler(const Ngs2WaveformFormat& format) {
		const bool deferred = format.waveform_type == 0 && format.num_channels == 0 &&
		                      format.sample_rate == 0 && format.config_data == 0 &&
		                      format.frame_margin == 0 && format.frame_offset == 0;
		EXIT_NOT_IMPLEMENTED(!deferred && (format.frame_margin != 0 || format.frame_offset != 0 ||
		                                   format.sample_rate == 0 || format.num_channels == 0));
		ResetSetupState();
		ClearWaveformBlocks();
		is_vag          = false;
		channels        = format.num_channels;
		sample_rate     = format.sample_rate;
		sample_step     = uint64_t(sample_rate) << 32u;
		decoded_samples = 0;
		decoded_bytes   = 0;
		decoder.reset();
		decoded_frame.clear();
		next_decoded_frame.clear();
		accepts_blocks = true;
		if (deferred) {
			return;
		}
		waveform_type = format.waveform_type;
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
			compressed_input.reserve(info.super_frame_size);
		} else if (format.waveform_type == SCE_NGS2_WAVEFORM_TYPE_VAG) {
			is_vag        = true;
			vag_history   = {};
			vag_next_data = nullptr;
			decoded_frame.resize(28 * channels);
		} else {
			EXIT_NOT_IMPLEMENTED(format.waveform_type != 0x11 && format.waveform_type != 0x12 &&
			                     format.waveform_type != 0x14 && format.waveform_type != 0x16 &&
			                     format.waveform_type != 0x18 && format.waveform_type != 0x1a);
		}
	}

	int AddWaveformBlocks(const uint8_t* data, uint32_t flags, uint32_t count,
	                      const Ngs2WaveformBlock* waveform_blocks) {
		const auto capacity = rack->type == Ngs2RackType::Sampler
		                          ? rack->option.sampler.max_waveform_blocks
		                          : rack->option.custom_sampler.max_waveform_blocks;
		if (count > capacity) {
			return NGS2_ERROR_INVALID_NUM_WAVEFORM_BLOCKS;
		}
		const bool clear     = (flags & 4u) != 0;
		const bool only_data = (flags & 2u) != 0;
		const bool join      = only_data && (flags & 0x10u) == 0;
		// CLEAR reopens a list that the previous addition closed.
		EXIT_NOT_IMPLEMENTED((!accepts_blocks && !clear) || (flags & ~0x1fu) != 0);
		const auto         retained = clear ? 0 : blocks.size();
		Block              tail     = retained == 0 ? Block {} : blocks.back();
		std::vector<Block> pending;
		pending.reserve(count);
		for (uint32_t i = 0; i < count; ++i) {
			auto info = waveform_blocks[i];
			if (!only_data && info.num_samples == 0 && info.data_size == 0) {
				continue;
			}
			EXIT_NOT_IMPLEMENTED(!only_data && info.num_samples == 0);
			if (only_data) {
				info.num_repeats      = 0;
				info.num_skip_samples = 0;
				info.num_samples      = 0;
			}
			Block block {data + info.data_offset, info};
			auto* previous = pending.empty() ? (retained == 0 ? nullptr : &tail) : &pending.back();
			if (join && previous != nullptr &&
			    previous->data + previous->info.data_size == block.data) {
				previous->info.data_size += block.info.data_size;
			} else {
				pending.push_back(block);
			}
		}
		// Validate the complete addition before clearing, extending, or closing the stream.
		if (retained + pending.size() > capacity) {
			return NGS2_ERROR_FULL_WAVEFORM_BLOCKS;
		}
		if (clear) {
			ClearWaveformBlocks();
		} else if (retained != 0) {
			blocks.back().info.data_size = tail.info.data_size;
		}
		// CANCEL marks the outstanding blocks; a marked block that has not started decodes as data
		// only.
		if ((flags & 8u) != 0) {
			for (auto& block: blocks) {
				block.canceled = true;
			}
		}
		blocks.insert(blocks.end(), pending.begin(), pending.end());
		accepts_blocks = (flags & 1u) != 0;
		return OK;
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
			case 1:
				if (state == Ngs2VoicePlayState::Empty || state == Ngs2VoicePlayState::Stopped) {
					state = Ngs2VoicePlayState::Playing;
					// Reserve the voice immediately; publish playback flags after rendering.
					state_flags |= 1;
				}
				break;
			case 2:
				if (state == Ngs2VoicePlayState::Playing || state == Ngs2VoicePlayState::Paused) {
					state = Ngs2VoicePlayState::Stopped;
					reverb.Reset();
				}
				break;
			case 4:
			case 8:
				state = Ngs2VoicePlayState::Empty;
				reverb.Reset();
				break;
			case 16:
				if (state == Ngs2VoicePlayState::Playing) {
					state = Ngs2VoicePlayState::Paused;
				}
				break;
			case 32:
				if (state == Ngs2VoicePlayState::Paused) {
					state = Ngs2VoicePlayState::Playing;
				}
				break;
			default: EXIT("unknown event_id: 0x%08" PRIx32 "\n", id);
		}
	}
};

struct Ngs2VoiceEventParam {
	Ngs2VoiceParamHeader header;
	uint32_t             event_id;
};

struct Ngs2SubmixerVoiceSetupParam {
	Ngs2VoiceParamHeader header;
	uint32_t             num_io_channels;
	uint32_t             flags;
};
static_assert(sizeof(Ngs2SubmixerVoiceSetupParam) == 16);

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

struct Ngs2CustomSamplerVoiceState {
	Ngs2VoiceState voice_state;
	const void*    waveform_data;
	uint64_t       num_decoded_samples;
	uint64_t       decoded_data_size;
	uint64_t       user_data;
	uint32_t       reserved;
	uint32_t       reserved2;
};

static Ngs2Internal*        g_ngs_list     = nullptr;
static Ngs2RackInternal*    g_racks_list   = nullptr;
static std::atomic_uint32_t g_next_ngs_uid = 1;
static Common::Mutex        g_racks_mutex;
static Common::Mutex        g_ngs_mutex;

static_assert(sizeof(Ngs2UserFxProcessContext) == 48);
static_assert(sizeof(Ngs2UserFx2Context) == 72);
static_assert(sizeof(Ngs2UserFx2ProcessContext) == 104);
static_assert(sizeof(Ngs2UserFx2Option) == 72);
static_assert(sizeof(Ngs2SystemOption) == 144);
static_assert(sizeof(Ngs2SystemInfo) == 184);
static_assert(sizeof(Ngs2RackOption) == 176);
static_assert(sizeof(Ngs2VoiceState) == 8);
static_assert(sizeof(Ngs2SubmixerVoiceState) == 20);
static_assert(sizeof(Ngs2CustomMasteringVoiceState) == 16);
static_assert(sizeof(Ngs2SamplerVoiceState) == 56);
static_assert(sizeof(Ngs2CustomSamplerVoiceState) == 48);
static_assert(sizeof(Ngs2WaveformFormat) == 24);
static_assert(offsetof(Ngs2WaveformFormat, frame_offset) == 16 &&
              offsetof(Ngs2WaveformFormat, frame_margin) == 20);
static_assert(sizeof(Ngs2WaveformBlock) == 40);
static_assert(sizeof(Ngs2WaveformInfo) == 232);

static uint32_t Ngs2GetStateFlags(const Ngs2VoiceInternal* voice) {
	return voice->state_flags;
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
	{
		Common::LockGuard lock(g_ngs_mutex);
		ngs->next  = g_ngs_list;
		g_ngs_list = ngs;
	}

	return ngs;
}

static Ngs2Internal* Ngs2FindSystem(uintptr_t system_handle) {
	if (system_handle < 0x10000) {
		return nullptr;
	}
	Common::LockGuard lock(g_ngs_mutex);
	for (auto* ngs = g_ngs_list; ngs != nullptr; ngs = ngs->next) {
		if (reinterpret_cast<uintptr_t>(ngs) == system_handle) {
			return ngs;
		}
	}
	return reinterpret_cast<Ngs2Internal*>(system_handle);
}

static Ngs2Internal* Ngs2FindRegisteredSystem(uintptr_t system_handle) {
	if (system_handle < 0x10000) {
		return nullptr;
	}
	Common::LockGuard lock(g_ngs_mutex);
	for (auto* ngs = g_ngs_list; ngs != nullptr; ngs = ngs->next) {
		if (reinterpret_cast<uintptr_t>(ngs) == system_handle) {
			return ngs;
		}
	}
	return nullptr;
}

// Lookup variant for callers that already hold g_racks_mutex.
static Ngs2RackInternal* Ngs2FindRackLocked(uintptr_t handle) {
	for (auto* rack = g_racks_list; rack != nullptr; rack = rack->next) {
		if (reinterpret_cast<uintptr_t>(rack) == handle) {
			return rack;
		}
	}
	return nullptr;
}

static Ngs2RackInternal* Ngs2FindRack(uintptr_t handle) {
	if (handle < 0x10000) {
		return nullptr;
	}
	Common::LockGuard racks_lock(g_racks_mutex);
	return Ngs2FindRackLocked(handle);
}

static Ngs2VoiceInternal* Ngs2VoiceFromHandle(uintptr_t handle) {
	if (handle == 0) {
		return nullptr;
	}
	return reinterpret_cast<Ngs2VoiceInternal*>(handle);
}

static bool Ngs2IsValidSampleRate(uint32_t rate) {
	switch (rate) {
		case 11025:
		case 12000:
		case 22050:
		case 24000:
		case 44100:
		case 48000:
		case 88200:
		case 96000:
		case 176400:
		case 192000: return true;
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
	constexpr int32_t ERROR_INVALID_OUT_ADDRESS = static_cast<int32_t>(0x804a8010u);
	constexpr int32_t ERROR_INVALID_OUT_SIZE    = static_cast<int32_t>(0x804a8011u);

	if (info == nullptr) {
		return ERROR_INVALID_OUT_ADDRESS;
	}
	// Larger buffers are accepted; only sizeof(Ngs2SystemInfo) is written.
	if (info_size < sizeof(Ngs2SystemInfo)) {
		return ERROR_INVALID_OUT_SIZE;
	}

	auto* ngs = Ngs2FindSystem(system_handle);
	if (ngs == nullptr) {
		return NGS2_ERROR_INVALID_SYSTEM_HANDLE;
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

	if (system_handle == 0) {
		return NGS2_ERROR_INVALID_SYSTEM_HANDLE;
	}

	auto* target = reinterpret_cast<Ngs2Internal*>(system_handle);
	{
		Common::LockGuard lock(g_ngs_mutex);
		auto**            link = &g_ngs_list;
		while (*link != nullptr && *link != target) {
			link = &(*link)->next;
		}
		if (*link != nullptr) {
			*link = (*link)->next;
		}
	}

	return OK;
}

static void Ngs2InitModules(Ngs2RackInternal& rack) {
	if (!Ngs2RackIsCustom(rack.type)) {
		return;
	}
	const auto& custom = rack.option.custom_sampler.custom_rack_option;
	EXIT_NOT_IMPLEMENTED(custom.num_buffers == 0 || custom.num_buffers > 4 ||
	                     rack.option.common.max_ports > 16);
	for (uint32_t p = 0; p < rack.option.common.max_ports; ++p) {
		EXIT_NOT_IMPLEMENTED(custom.port[p].source_buffer_id >= custom.num_buffers);
	}
	auto* voices = reinterpret_cast<Ngs2VoiceInternal*>(&rack + 1);
	for (uint32_t m = 0; m < custom.num_modules; ++m) {
		const auto& info = custom.module[m];
		// Modules process their source buffer in place, so skipped modules pass audio through.
		EXIT_NOT_IMPLEMENTED(info.source_buffer_id != info.dest_buffer_id ||
		                     std::max(info.source_buffer_id, info.extra_buffer_id) >=
		                         custom.num_buffers);
		if (info.module_id == SCE_NGS2_CUSTOM_MODULE_ID_GAIN) {
			for (uint32_t i = 0; i < rack.option.common.max_voices; ++i) {
				auto& module = voices[i].modules[m];
				module.current_gain.fill(1.0f);
				module.target_gain.fill(1.0f);
				module.gain_configured = false;
			}
			continue;
		}
		if (info.module_id == SCE_NGS2_CUSTOM_MODULE_ID_PITCH_SHIFT) {
			for (uint32_t i = 0; i < rack.option.common.max_voices; ++i) {
				auto& module = voices[i].modules[m];
				module.pitch_shift_channels.resize(
				    std::min<uint32_t>(rack.option.common.max_voice_channels, 32u));
				module.pitch_shift_bypass_mask = ~0u;
				module.pitch_shift_ratio       = 1.0f;
				module.pitch_shift_reset       = true;
			}
			continue;
		}
		if (info.module_id != 0x1f) {
			continue;
		}
		auto& fx = rack.fx[m];
		fx       = *reinterpret_cast<const Ngs2UserFx2Option*>(info.option);
		rack.common[m].resize(fx.common_size);
		for (uint32_t i = 0; i < rack.option.common.max_voices; ++i) {
			auto& module = voices[i].modules[m];
			module.param.resize(fx.param_size);
			module.work.resize(fx.work_size);
			module.state.resize(custom.module[m].state_size);
			if (fx.setup != nullptr) {
				Ngs2UserFx2Context context {
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

	rack->rack_id = rack_id;
	rack->uid     = ngs->uid;
	std::memcpy(rack->name, option->name, sizeof(rack->name));
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
					auto&              module = voices[i].modules[m];
					Ngs2UserFx2Context context {rack->common[m].data(),
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

	Ngs2Internal* ngs = nullptr;
	{
		Common::LockGuard racks_lock(g_racks_mutex);
		auto*           rack = Ngs2FindRackLocked(rack_handle);
		if (rack == nullptr || rack->ngs == nullptr) {
			return NGS2_ERROR_INVALID_RACK_HANDLE;
		}
		ngs = rack->ngs;
	}

	ngs->mutex.Lock();
	{
		// The rack may have been destroyed while waiting for the system lock;
		// the registry order is system mutex first, racks mutex second.
		Common::LockGuard racks_lock(g_racks_mutex);
		auto*           rack = Ngs2FindRackLocked(rack_handle);
		if (rack == nullptr || rack->ngs != ngs) {
			ngs->mutex.Unlock();
			return NGS2_ERROR_INVALID_RACK_HANDLE;
		}
		rack->lock_depth.fetch_add(1, std::memory_order_relaxed);
		rack->lock_owner.store(std::this_thread::get_id(), std::memory_order_relaxed);
	}

	return OK;
}

int KYTY_SYSV_ABI Ngs2RackUnlock(uintptr_t rack_handle) {
	PRINT_NAME();
	LOGF("\t rack_handle = 0x%016" PRIx64 "\n", static_cast<uint64_t>(rack_handle));

	// The owner already holds ngs->mutex, so the depth and owner are read
	// atomically instead of taking the system mutex again.
	Common::LockGuard racks_lock(g_racks_mutex);
	auto*           rack = Ngs2FindRackLocked(rack_handle);
	if (rack == nullptr || rack->ngs == nullptr) {
		return NGS2_ERROR_INVALID_RACK_HANDLE;
	}

	if (rack->lock_depth.load(std::memory_order_relaxed) == 0 ||
	    rack->lock_owner.load(std::memory_order_relaxed) != std::this_thread::get_id()) {
		return NGS2_ERROR_INVALID_RACK_HANDLE;
	}

	const bool last = rack->lock_depth.fetch_sub(1, std::memory_order_relaxed) == 1;
	if (last) {
		rack->lock_owner.store({}, std::memory_order_relaxed);
	}
	rack->ngs->mutex.Unlock();

	return OK;
}

struct Ngs2VoiceCallbackInfo {
	uintptr_t   data, voice;
	uint32_t    flag, reserved;
	uintptr_t   user;
	const void* block_data;
	uint32_t    size, repeats, attributes, reserved2;
};
static_assert(sizeof(Ngs2VoiceCallbackInfo) == 56);
static_assert(offsetof(Ngs2VoiceCallbackInfo, size) == 40 &&
              offsetof(Ngs2VoiceCallbackInfo, repeats) == 44 &&
              offsetof(Ngs2VoiceCallbackInfo, attributes) == 48);

static bool Ngs2FinishBlock(Ngs2VoiceInternal& voice) {
	const auto generation = voice.source_generation;
	auto&      block      = voice.blocks.front();
	const bool repeat     = block.info.num_repeats != 0;
	voice.waveform_end    = voice.WaveformData();
	if (repeat) {
		if (block.info.num_repeats != UINT32_MAX) {
			--block.info.num_repeats;
		}
		++block.repeated_count;
		block.data_cursor = 0;
		if (voice.decoder != nullptr) {
			voice.compressed_input.clear();
			voice.decoder->Reset();
		}
	}
	const Ngs2VoiceCallbackInfo info {voice.callback_data,
	                                  reinterpret_cast<uintptr_t>(&voice),
	                                  repeat ? 2u : 1u,
	                                  0,
	                                  block.info.user_data,
	                                  block.data,
	                                  static_cast<uint32_t>(block.info.data_size),
	                                  block.repeated_count,
	                                  static_cast<uint32_t>(block.canceled),
	                                  0};
	if (!repeat) {
		voice.blocks.pop_front();
	}
	if (voice.callback != 0 && (voice.callback_flags & info.flag) != 0) {
		reinterpret_cast<void KYTY_SYSV_ABI (*)(const Ngs2VoiceCallbackInfo*)>(voice.callback)(
		    &info);
	}
	return voice.source_generation == generation;
}

static bool Ngs2DecodeFrame(Ngs2VoiceInternal& voice, std::vector<float>& frame,
                            uint32_t& frame_samples) {
	frame_samples = 0;
	while (voice.state == Ngs2VoicePlayState::Playing && !voice.blocks.empty()) {
		auto& block = voice.blocks.front();
		if (block.canceled && block.data_cursor == 0 && block.repeated_count == 0) {
			block.info.num_samples = 0;
			block.info.num_repeats = 0;
		}
		if (block.info.num_samples != 0 && block.data_cursor == 0) {
			voice.duration_remaining = block.info.num_samples;
			voice.skip_remaining     = block.info.num_skip_samples;
		}
		if (voice.duration_remaining == 0) {
			if (!Ngs2FinishBlock(voice)) {
				return false;
			}
			continue;
		}
		if (voice.is_vag) {
			const auto frame_bytes = voice.channels * 16u;
			if (block.data_cursor + frame_bytes > block.info.data_size) {
				if (!Ngs2FinishBlock(voice)) {
					return false;
				}
				continue;
			}
			if (voice.vag_next_data != block.data + block.data_cursor) {
				voice.vag_history = {};
			}
			static constexpr int32_t VAG_COEFF[5][2] = {
			    {0, 0}, {60, 0}, {115, -52}, {98, -55}, {122, -60},
			};
			frame.resize(28 * voice.channels);
			for (uint32_t ch = 0; ch < voice.channels; ++ch) {
				const auto*    unit      = block.data + block.data_cursor + ch * 16u;
				const uint32_t shift     = unit[0] & 0x0fu;
				const uint32_t predictor = (unit[0] >> 4u);
				auto&          hist      = voice.vag_history[ch];
				for (uint32_t s = 0; s < 28; ++s) {
					const uint8_t byte_val = unit[2 + s / 2];
					const int32_t nibble   = (s & 1) == 0 ? (byte_val & 0x0f) : (byte_val >> 4);
					const int32_t s_nibble = (nibble >= 8) ? (nibble - 16) : nibble;
					int32_t       sample   = (s_nibble << 12) >> shift;
					if (predictor < 5) {
						sample += (hist.hist1 * VAG_COEFF[predictor][0] +
						           hist.hist2 * VAG_COEFF[predictor][1]) >>
						          6;
					}
					sample                         = std::clamp(sample, -32768, 32767);
					hist.hist2                     = hist.hist1;
					hist.hist1                     = sample;
					frame[s * voice.channels + ch] = sample / 32768.0f;
				}
			}
			block.data_cursor += frame_bytes;
			voice.decoded_bytes += frame_bytes;
			voice.vag_next_data = block.data + block.data_cursor;

			const uint32_t skip = std::min(voice.skip_remaining, 28u);
			voice.skip_remaining -= skip;
			voice.frame_cursor       = skip;
			const uint32_t available = 28u - skip;
			const uint32_t count     = std::min(available, voice.duration_remaining);
			voice.duration_remaining -= count;
			voice.decoded_samples += count;
			frame_samples = skip + count;

			if (voice.duration_remaining == 0 ||
			    block.info.data_size - block.data_cursor < frame_bytes) {
				if (!Ngs2FinishBlock(voice)) {
					return false;
				}
			}
			if (count != 0) {
				return true;
			}
			continue;
		}
		if (voice.decoder == nullptr) {
			// PCM has no encoded frame size, so stage at most one system grain.
			const auto quantum      = std::max(voice.rack->ngs->option.num_grain_samples, 1u);
			const auto sample_bytes = Ngs2PcmSampleSize(voice.waveform_type);
			const auto frame_bytes  = voice.channels * sample_bytes;
			const auto available    = (block.info.data_size - block.data_cursor) / frame_bytes;
			const auto skip         = std::min<size_t>(voice.skip_remaining, available);
			const auto count =
			    std::min<size_t>({available - skip, voice.duration_remaining, quantum});
			frame.resize(size_t(quantum) * voice.channels);
			const auto* pcm_data = block.data + block.data_cursor + skip * frame_bytes;
			for (size_t s = 0; s < count; ++s) {
				for (size_t ch = 0; ch < voice.channels; ++ch) {
					frame[s * voice.channels + ch] = Ngs2ReadPcmSample(
					    voice.waveform_type, pcm_data + (s * voice.channels + ch) * sample_bytes);
				}
			}
			voice.skip_remaining -= skip;
			voice.duration_remaining -= count;
			block.data_cursor += (skip + count) * frame_bytes;
			voice.decoded_bytes += (skip + count) * frame_bytes;
			voice.decoded_samples += count;
			frame_samples = static_cast<uint32_t>(count);
			if (voice.duration_remaining == 0 ||
			    block.info.data_size - block.data_cursor < frame_bytes) {
				// A repeating block restarts its duration, so it must hold all of it.
				EXIT_NOT_IMPLEMENTED(
				    voice.duration_remaining != 0 &&
				    (block.data_cursor != block.info.data_size || block.info.num_repeats != 0));
				if (!Ngs2FinishBlock(voice)) {
					return false;
				}
			}
			if (count != 0) {
				return true;
			}
			continue;
		}
		const auto skip =
		    std::min(voice.skip_remaining, static_cast<uint32_t>(frame.size() / voice.channels));
		Ajm::AjmGaplessState gapless;
		gapless.Set({voice.duration_remaining, static_cast<uint16_t>(skip), 0}, true);
		if (!voice.compressed_input.empty()) {
			Ajm::AjmSidebandDecAt9CodecInfo info {};
			voice.decoder->WriteCodecInfo(&info, sizeof(info), {});
			EXIT_NOT_IMPLEMENTED(info.next_frame_size < voice.compressed_input.size());
			const auto bytes =
			    std::min<size_t>(info.next_frame_size - voice.compressed_input.size(),
			                     block.info.data_size - block.data_cursor);
			voice.compressed_input.insert(voice.compressed_input.end(),
			                              block.data + block.data_cursor,
			                              block.data + block.data_cursor + bytes);
			block.data_cursor += bytes;
			voice.decoded_bytes += bytes;
			if (voice.compressed_input.size() < info.next_frame_size) {
				if (!Ngs2FinishBlock(voice)) {
					return false;
				}
				continue;
			}
		}
		const bool pending = !voice.compressed_input.empty();
		const auto result  = voice.decoder->Decode(
		    pending ? voice.compressed_input.data() : block.data + block.data_cursor,
		    pending ? voice.compressed_input.size() : block.info.data_size - block.data_cursor,
		    frame.data(), frame.size() * sizeof(float), false, &gapless);
		if (!pending && result.result == Ajm::AJM_RESULT_PARTIAL_INPUT &&
		    result.input_consumed == 0) {
			voice.compressed_input.insert(voice.compressed_input.end(),
			                              block.data + block.data_cursor,
			                              block.data + block.info.data_size);
			voice.decoded_bytes += block.info.data_size - block.data_cursor;
			block.data_cursor = block.info.data_size;
			if (!Ngs2FinishBlock(voice)) {
				return false;
			}
			continue;
		}
		EXIT_NOT_IMPLEMENTED(result.result != OK || result.input_consumed == 0);
		if (pending) {
			voice.compressed_input.erase(voice.compressed_input.begin(),
			                             voice.compressed_input.begin() + result.input_consumed);
		} else {
			block.data_cursor += result.input_consumed;
			voice.decoded_bytes += result.input_consumed;
		}
		voice.skip_remaining -= skip - gapless.current.skip_samples;
		const auto decoded_samples =
		    static_cast<uint32_t>(result.output_written / (sizeof(float) * voice.channels));
		if (decoded_samples != 0) {
			EXIT_NOT_IMPLEMENTED(decoded_samples > voice.duration_remaining);
			frame_samples = decoded_samples;
			voice.duration_remaining -= decoded_samples;
			voice.decoded_samples += decoded_samples;
		}
		if (voice.duration_remaining == 0 ||
		    (block.data_cursor == block.info.data_size && voice.compressed_input.empty())) {
			if (voice.duration_remaining == 0) {
				voice.compressed_input.clear();
			}
			if (!Ngs2FinishBlock(voice)) {
				return false;
			}
		}
		if (decoded_samples != 0) {
			return true;
		}
	}
	return false;
}

static bool Ngs2CurrentFrame(Ngs2VoiceInternal& voice) {
	if (voice.frame_cursor == voice.frame_samples) {
		voice.frame_cursor  = 0;
		voice.frame_samples = 0;
		if (voice.next_frame_samples != 0) {
			std::swap(voice.decoded_frame, voice.next_decoded_frame);
			voice.frame_cursor       = voice.next_frame_cursor;
			voice.next_frame_cursor  = 0;
			voice.frame_samples      = voice.next_frame_samples;
			voice.next_frame_samples = 0;
		} else if (!Ngs2DecodeFrame(voice, voice.decoded_frame, voice.frame_samples)) {
			// No playable input: drop the staging state so the boundary sample
			// keeps its current value until decodable input arrives.
			voice.next_frame_samples = 0;
			voice.next_frame_cursor  = 0;
			return false;
		}
	}
	return voice.state == Ngs2VoicePlayState::Playing && voice.frame_cursor < voice.frame_samples;
}

static void Ngs2AdvanceSamples(Ngs2VoiceInternal& voice) {
	const auto output_rate = uint64_t(voice.rack->ngs->option.sample_rate) << 32u;
	while (voice.state == Ngs2VoicePlayState::Playing && voice.sample_phase >= output_rate) {
		if (!Ngs2CurrentFrame(voice)) {
			break;
		}
		const auto advance = std::min(voice.sample_phase / output_rate,
		                              uint64_t(voice.frame_samples - voice.frame_cursor));
		voice.frame_cursor += static_cast<uint32_t>(advance);
		voice.sample_phase -= advance * output_rate;
	}
	if (voice.blocks.empty() && voice.frame_cursor == voice.frame_samples &&
	    voice.next_frame_samples == 0 && !voice.accepts_blocks) {
		voice.sample_phase = 0;
	}
}

static void Ngs2ConsumeSamples(Ngs2VoiceInternal& voice, uint32_t grain) {
	const auto generation  = voice.source_generation;
	uint32_t   output      = 0;
	const auto output_rate = uint64_t(voice.rack->ngs->option.sample_rate) << 32u;
	while (voice.state == Ngs2VoicePlayState::Playing && output < grain &&
	       (!voice.blocks.empty() || voice.frame_cursor < voice.frame_samples ||
	        voice.next_frame_samples != 0)) {
		Ngs2AdvanceSamples(voice);
		if (voice.source_generation != generation || !Ngs2CurrentFrame(voice)) {
			break;
		}
		if (voice.frame_cursor + 1 == voice.frame_samples && voice.sample_phase != 0) {
			if (voice.next_frame_samples == 0 && !voice.blocks.empty()) {
				if (voice.next_decoded_frame.empty()) {
					voice.next_decoded_frame.resize(voice.decoded_frame.size());
				}
				// The decode reports the staged frame's start cursor through
				// frame_cursor/frame_samples; keep them with the staged frame
				// and restore the cursor of the frame still being played.
				const auto saved_cursor  = voice.frame_cursor;
				const auto saved_samples = voice.frame_samples;
				voice.frame_cursor       = 0;
				voice.frame_samples      = 0;
				const bool staged =
				    Ngs2DecodeFrame(voice, voice.next_decoded_frame, voice.next_frame_samples);
				if (voice.source_generation == generation) {
					if (staged) {
						voice.next_frame_cursor = voice.frame_cursor;
					} else {
						voice.next_frame_samples = 0;
						voice.next_frame_cursor  = 0;
					}
					voice.frame_cursor  = saved_cursor;
					voice.frame_samples = saved_samples;
				} else {
					voice.next_frame_samples = 0;
					voice.next_frame_cursor  = 0;
				}
			}
			if (voice.source_generation != generation ||
			    voice.state != Ngs2VoicePlayState::Playing ||
			    voice.frame_cursor >= voice.frame_samples) {
				break;
			}
		}
		const float* current = voice.decoded_frame.data() + voice.frame_cursor * voice.channels;
		const float* next    = current;
		if (voice.frame_cursor + 1 < voice.frame_samples) {
			next = current + voice.channels;
		} else if (voice.next_frame_samples != 0 && !voice.next_decoded_frame.empty()) {
			// A staged frame may start past skipped samples; interpolate toward
			// its first playable sample, not the skipped prefix.
			const auto next_index = std::min(voice.next_frame_cursor, voice.next_frame_samples - 1);
			if (size_t(next_index) * voice.channels < voice.next_decoded_frame.size()) {
				next = voice.next_decoded_frame.data() + next_index * voice.channels;
			}
		}
		const float  fraction =
		    static_cast<float>(voice.sample_phase) / static_cast<float>(output_rate);
		for (uint32_t c = 0; c < voice.channels; ++c) {
			voice.samples[c * grain + output] = std::lerp(current[c], next[c], fraction);
		}
		voice.sample_phase += voice.sample_step;
		++output;
	}
	if (voice.source_generation == generation) {
		Ngs2AdvanceSamples(voice);
	}
	if (voice.source_generation != generation) {
		// A callback replaced the source, possibly changing its channel layout.
		voice.ClearSamples(grain);
		output = 0;
	}
	voice.has_samples = output != 0;
}

// Mono and stereo convert at -3 dB, mono feeds the surround front center, others map 1:1.
static float Ngs2DefaultMatrixLevel(uint32_t src_channels, uint32_t src, uint32_t dst_channels,
                                    size_t dst) {
	if ((src_channels == 1 && dst_channels == 2) || (src_channels == 2 && dst_channels == 1)) {
		return std::numbers::sqrt2_v<float> / 2;
	}
	if (src_channels == 1 && dst_channels > 2) {
		return dst == 2 ? 1.0f : 0.0f;
	}
	return src == dst ? 1.0f : 0.0f;
}

static void ConfigureMasteringLfe(Ngs2VoiceInternal::MasteringLfe& filter, uint32_t design_rate) {
	constexpr double ripple_db = 0.1;
	const double     epsilon   = std::sqrt(std::pow(10.0, ripple_db / 10.0) - 1.0);
	const double     mu        = std::asinh(1.0 / epsilon) / 4.0;
	const double     cutoff    = std::tan(std::numbers::pi * filter.cutoff / design_rate);
	double           gain      = std::pow(10.0, -ripple_db / 20.0);
	for (size_t i = 0; i < filter.sections.size(); ++i) {
		const double               angle = (i == 0 ? 3.0 : 1.0) * std::numbers::pi / 8.0;
		const std::complex<double> analog_pole(-std::sinh(mu) * std::sin(angle),
		                                       std::cosh(mu) * std::cos(angle));
		const auto   pole     = (1.0 + cutoff * analog_pole) / (1.0 - cutoff * analog_pole);
		const double a1       = 2.0 * pole.real();
		const double a2       = -std::norm(pole);
		filter.sections[i].a1 = static_cast<float>(a1);
		filter.sections[i].a2 = static_cast<float>(a2);
		gain *= (1.0 - a1 - a2) * 0.25;
	}
	filter.sections[0].gain = static_cast<float>(gain);
	filter.sections[1].gain = 1.0f;
	filter.design_rate      = design_rate;
	filter.design_cutoff    = filter.cutoff;
}

static void Ngs2RenderVoice(Ngs2VoiceInternal& voice, const std::vector<Ngs2VoiceInternal*>& voices,
                            uint32_t grain) {
	if (voice.rendered) {
		return;
	}
	if (voice.channels == 0) {
		voice.has_samples = false;
		voice.rendered    = true;
		return;
	}
	EXIT_NOT_IMPLEMENTED(voice.rendering);
	voice.rendering = true;
	voice.ClearSamples(grain);
	voice.has_samples = false;
	if (voice.state == Ngs2VoicePlayState::Playing) {
		if (voice.rack->type == Ngs2RackType::Sampler ||
		    voice.rack->type == Ngs2RackType::CustomSampler) {
			Ngs2ConsumeSamples(voice, grain);
			// A playing stream can have no source data but still have a filter tail.
			for (auto& filter: voice.filters) {
				if (voice.has_samples || filter.HasHistory()) {
					filter.EnsureRate(voice.rack->ngs->option.sample_rate, voice.channels);
					filter.Process(voice.samples, voice.channels, grain);
					voice.has_samples = true;
				}
			}
			if (voice.state == Ngs2VoicePlayState::Playing && voice.blocks.empty() &&
			    voice.frame_cursor == voice.frame_samples && voice.next_frame_samples == 0 &&
			    !voice.accepts_blocks &&
			    std::ranges::none_of(voice.filters,
			                         [](const auto& filter) { return filter.HasHistory(); })) {
				voice.state = Ngs2VoicePlayState::Empty;
			}
		}
		for (auto* source: voices) {
			for (size_t p = 0; p < source->ports.size(); ++p) {
				const auto& port = source->ports[p];
				if (port.dest != &voice || port.volume == 0.0f) {
					continue;
				}
				Ngs2RenderVoice(*source, voices, grain);
				if (!source->has_samples) {
					continue;
				}
				// Input N of a custom rack voice feeds its buffer N.
				EXIT_NOT_IMPLEMENTED(port.input >= voice.rack->NumBuffers());
				const auto* input  = source->Buffer(source->rack->PortBufferId(p), grain);
				auto*       output = voice.Buffer(port.input, grain);
				// A port without a matrix, or whose matrix has no levels, uses the default mix.
				const auto* matrix = port.matrix < 0 ? nullptr : &source->matrices.at(port.matrix);
				if (matrix != nullptr && matrix->empty()) {
					matrix = nullptr;
				}
				const size_t output_channels =
				    matrix == nullptr
				        ? voice.channels
				        : std::min<size_t>(voice.channels, matrix->size() / source->channels);
				for (size_t dst = 0; dst < output_channels; ++dst) {
					for (uint32_t src = 0; src < source->channels; ++src) {
						const float level =
						    port.volume * (matrix == nullptr
						                       ? Ngs2DefaultMatrixLevel(source->channels, src,
						                                                voice.channels, dst)
						                       : (*matrix)[dst * source->channels + src]);
						for (uint32_t i = 0; i < grain; ++i) {
							output[dst * grain + i] += input[src * grain + i] * level;
						}
					}
				}
				voice.has_samples = true;
			}
		}
		if (voice.rack->type == Ngs2RackType::Reverb) {
			voice.reverb.Prepare(voice.channels, voice.rack->ngs->option.sample_rate);
			voice.has_samples = voice.reverb.Process(voice.samples, grain, voice.has_samples);
		}
		if (voice.rack->type == Ngs2RackType::Mastering) {
			const auto rate = voice.rack->ngs->option.sample_rate;
			// Mastering LFE filter
			if (voice.mastering_lfe.enabled && voice.channels >= 4) {
				auto&      filter      = voice.mastering_lfe;
				const auto design_rate = rate == 96000 ? 88200u : rate;
				if (filter.design_rate != design_rate || filter.design_cutoff != filter.cutoff) {
					ConfigureMasteringLfe(filter, design_rate);
				}
				float* samples = voice.samples.data() + 3 * grain;
				for (auto& sec: filter.sections) {
					auto& h = sec.history;
					for (uint32_t i = 0; i < grain; ++i) {
						const float x = samples[i];
						const float y = sec.gain * x + (2.0f * sec.gain) * h.x1 + sec.gain * h.x2 +
						                sec.a1 * h.y1 + sec.a2 * h.y2;
						h             = {x, h.x1, y, h.y1};
						samples[i]    = y;
					}
				}
			}
			// Mastering Limiter
			if (voice.mastering_limiter.enabled) {
				const float limit = voice.mastering_limiter.threshold;
				for (size_t i = 0; i < size_t(voice.channels) * grain; ++i) {
					voice.samples[i] = std::clamp(voice.samples[i], -limit, limit);
				}
			}
			// Mastering Peak Meter
			if (voice.peak_meter_mode == 1) {
				float peak = 0.0f;
				for (size_t i = 0; i < size_t(voice.channels) * grain; ++i) {
					peak = std::max(peak, std::abs(voice.samples[i]));
				}
				voice.peak_height = peak;
			} else if (voice.peak_meter_mode == 2) {
				float energy = 0.0f;
				for (size_t i = 0; i < size_t(voice.channels) * grain; ++i) {
					energy += voice.samples[i] * voice.samples[i];
				}
				voice.peak_height = (voice.channels > 0 && grain > 0)
				                        ? std::sqrt(energy / (voice.channels * grain))
				                        : 0.0f;
			}
		}
		const bool had_samples = voice.has_samples;
		// Pitch-shift channels buffer a delayed tail that must keep rendering
		// after the input ends; bypassed channels and paused/stopped voices
		// stay on the direct path.
		bool pitch_pending = false;
		if (grain % 64 == 0 && voice.channels < 32 && voice.state != Ngs2VoicePlayState::Paused &&
		    voice.state != Ngs2VoicePlayState::Stopped) {
			const auto& custom = voice.rack->option.custom_sampler.custom_rack_option;
			for (size_t m = 0; m < voice.modules.size(); ++m) {
				if (custom.module[m].module_id != SCE_NGS2_CUSTOM_MODULE_ID_PITCH_SHIFT) {
					continue;
				}
				const auto& module = voice.modules[m];
				const auto  count =
				    std::min<size_t>(voice.channels, module.pitch_shift_channels.size());
				for (size_t c = 0; c < count; ++c) {
					if (module.pitch_shift_channels[c].HasPending() &&
					    (module.pitch_shift_bypass_mask & (1u << c)) == 0) {
						pitch_pending = true;
						break;
					}
				}
				if (pitch_pending) {
					break;
				}
			}
		}
		if (voice.has_samples || pitch_pending) {
			const auto& custom = voice.rack->option.custom_sampler.custom_rack_option;
			const auto  rate   = voice.rack->ngs->option.sample_rate;
			for (size_t m = 0; m < voice.modules.size(); ++m) {
				const auto& info   = custom.module[m];
				auto&       module = voice.modules[m];
				if (info.module_id == SCE_NGS2_CUSTOM_MODULE_ID_GAIN) {
					auto* dest   = voice.Buffer(info.dest_buffer_id, grain);
					auto* source = voice.Buffer(info.source_buffer_id, grain);
					for (uint32_t c = 0; c < voice.channels; ++c) {
						const float  from   = module.current_gain[c];
						const float  to     = module.target_gain[c];
						const size_t offset = static_cast<size_t>(c) * grain;
						if (from == to) {
							if (to == 0.0f) {
								std::fill_n(dest + offset, grain, 0.0f);
							} else if (to == 1.0f) {
								if (source != dest) {
									std::copy_n(source + offset, grain, dest + offset);
								}
							} else {
								for (uint32_t i = 0; i < grain; ++i) {
									dest[offset + i] = source[offset + i] * to;
								}
							}
						} else {
							const float step = (to - from) / static_cast<float>(grain);
							for (uint32_t i = 0; i < grain; ++i) {
								dest[offset + i] =
								    source[offset + i] * (from + step * static_cast<float>(i));
							}
						}
						module.current_gain[c] = to;
					}
					voice.has_samples = true;
					continue;
				}
				if (info.module_id == SCE_NGS2_CUSTOM_MODULE_ID_CHANNEL_MIXER) {
					voice.MixChannels(module.channel_matrix, info.source_buffer_id, grain);
					continue;
				}
				if (info.module_id == SCE_NGS2_CUSTOM_MODULE_ID_PITCH_SHIFT) {
					if (grain % 64 == 0 && voice.channels < 32) {
						const uint32_t active =
						    ((1u << voice.channels) - 1u) & ~module.pitch_shift_bypass_mask;
						if (active != 0) {
							if (module.pitch_shift_channels.size() < voice.channels) {
								module.pitch_shift_channels.resize(voice.channels);
							}
							for (uint32_t c = 0; c < voice.channels; ++c) {
								if ((active & (1u << c)) == 0) continue;
								auto& channel = module.pitch_shift_channels[c];
								if (module.pitch_shift_reset) {
									channel = {};
								}
								if (module.pitch_shift_reset || module.pitch_shift_changed) {
									channel.SetPitch(module.pitch_shift_ratio);
								}
								auto* samples = voice.Buffer(info.source_buffer_id, grain) +
								                static_cast<size_t>(c) * grain;
								for (uint32_t i = 0; i < grain; i += 64) {
									channel.ProcessOrDrain(had_samples ? samples + i : nullptr,
									                       samples + i, module.pitch_shift_ratio);
								}
							}
							module.pitch_shift_reset   = false;
							module.pitch_shift_changed = false;
							voice.has_samples          = true;
						}
					}
					continue;
				}
				if (info.module_id == SCE_NGS2_CUSTOM_MODULE_ID_FILTER) {
					auto* source = voice.Buffer(info.source_buffer_id, grain);
					auto* dest   = voice.Buffer(info.dest_buffer_id, grain);
					if (source != dest) {
						std::copy_n(source, size_t(voice.channels) * grain, dest);
					}
					if (module.filter_configured) {
						module.filter.EnsureRate(rate, voice.channels);
						if (module.filter.enabled) {
							module.filter.Process(dest, voice.channels, grain);
							voice.has_samples = true;
						}
					}
					continue;
				}
				if (info.module_id != 0x19 && info.module_id != 0x1f) {
					if (!module.render_warned) {
						Log::WriteToConsoleAndLog(
						    fmt::sprintf("warning: NGS2 custom module 0x%02x at index %zu is "
						                 "ignored during render\n",
						                 info.module_id, m));
						module.render_warned = true;
					}
					continue;
				}
				if (info.module_id == 0x19 && module.handler == nullptr) {
					continue;
				}
				// Handlers receive the source channels followed by the extra buffer's channels.
				std::vector<float*> channels(2 * size_t(voice.channels));
				auto*               source = voice.Buffer(info.source_buffer_id, grain);
				auto*               extra  = voice.Buffer(info.extra_buffer_id, grain);
				for (uint32_t c = 0; c < voice.channels; ++c) {
					channels[c]                  = source + c * grain;
					channels[voice.channels + c] = extra + c * grain;
				}
				if (info.module_id == 0x19) {
					Ngs2UserFxProcessContext context {channels.data(), module.user_data,
					                                  module.flags,    voice.channels,
					                                  grain,           rate};
					module.handler(&context);
				} else {
					const auto&               fx = voice.rack->fx[m];
					Ngs2UserFx2ProcessContext context {channels.data(),
					                                   voice.rack->common[m].data(),
					                                   module.param.data(),
					                                   module.work.data(),
					                                   module.state.data(),
					                                   fx.user_data,
					                                   module.flags,
					                                   voice.channels,
					                                   voice.channels,
					                                   grain,
					                                   rate};
					EXIT_NOT_IMPLEMENTED(fx.process(&context) != OK);
				}
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
	auto*                           ngs = reinterpret_cast<Ngs2Internal*>(system_handle);
	Common::LockGuard               lock(ngs->mutex);
	std::vector<std::vector<float>> output_mix(num_buffer_info);
	std::vector<Ngs2VoiceInternal*> voices;
	{
		Common::LockGuard racks_lock(g_racks_mutex);
		for (auto* rack = g_racks_list; rack != nullptr; rack = rack->next) {
			if (rack->ngs != ngs) {
				continue;
			}
			// One render pass per rack, not per voice.
			++rack->render_count;
			auto* items = reinterpret_cast<Ngs2VoiceInternal*>(rack + 1);
			for (uint32_t i = 0; i < rack->option.common.max_voices; ++i) {
				items[i].rendered = false;
				voices.push_back(items + i);
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
		const bool mastering = voice->rack->type == Ngs2RackType::Mastering ||
		                       voice->rack->type == Ngs2RackType::CustomMastering;
		if (!mastering || !voice->has_samples) {
			continue;
		}
		EXIT_NOT_IMPLEMENTED(voice->output_id >= num_buffer_info);
		const auto& output = buffer_info[voice->output_id];
		EXIT_NOT_IMPLEMENTED(output.buffer == nullptr);
		EXIT_NOT_IMPLEMENTED(output.waveform_type != 0x12 && output.waveform_type != 0x18);
		// Surround inputs mixed into fewer output channels use their own downmix levels.
		EXIT_NOT_IMPLEMENTED(voice->rack->type == Ngs2RackType::Mastering && voice->channels > 2 &&
		                     output.num_channels < voice->channels);
		EXIT_NOT_IMPLEMENTED(output.buffer_size <
		                     grain * output.num_channels *
		                         (output.waveform_type == 0x12 ? sizeof(int16_t) : sizeof(float)));
		float* pcm = nullptr;
		if (output.waveform_type == 0x12) {
			auto& mix = output_mix[voice->output_id];
			if (mix.empty()) {
				mix.resize(grain * output.num_channels);
			}
			pcm = mix.data();
		} else {
			pcm = static_cast<float*>(output.buffer);
		}
		// The stored matrix mixes 2.0ch into 2.0ch; other mastering layouts use the default levels.
		const bool stored_matrix =
		    !voice->output_matrix.empty() && voice->channels == 2 && output.num_channels == 2;
		for (uint32_t dst = 0; dst < output.num_channels; ++dst) {
			// Output channel 3 is LFE; every other channel is full band.
			const float gain = dst == 3 ? voice->lfe_level : voice->fbw_level;
			for (uint32_t src = 0; src < voice->channels; ++src) {
				float level = static_cast<float>(src == dst);
				if (stored_matrix) {
					level = voice->output_matrix.at(src * output.num_channels + dst);
				} else if (voice->rack->type == Ngs2RackType::Mastering) {
					level = Ngs2DefaultMatrixLevel(voice->channels, src, output.num_channels, dst);
				}
				level *= gain;
				if (level == 0.0f) {
					continue;
				}
				for (uint32_t i = 0; i < grain; ++i) {
					pcm[i * output.num_channels + dst] += voice->samples[src * grain + i] * level;
				}
			}
		}
	}
	for (uint32_t i = 0; i < num_buffer_info; ++i) {
		const auto& mix = output_mix[i];
		if (mix.empty()) {
			continue;
		}
		auto* pcm = static_cast<int16_t*>(buffer_info[i].buffer);
		for (size_t sample = 0; sample < mix.size(); ++sample) {
			pcm[sample] = static_cast<int16_t>(std::clamp(
			    std::lrintf(std::clamp(mix[sample], -1.0f, 1.0f) * 32768.0f), -32768l, 32767l));
		}
	}
	for (auto* voice: voices) {
		// No envelope is configured, so a release stop finishes in this render.
		if (voice->state == Ngs2VoicePlayState::Stopped) {
			voice->state = Ngs2VoicePlayState::Empty;
		}
		voice->state_flags = static_cast<uint32_t>(voice->state);
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

static bool Ngs2GetAtrac9CodecInfo(const std::array<uint8_t, ATRAC9_CONFIG_DATA_SIZE>& config,
                                   Atrac9CodecInfo&                                    codec) {
	return Ajm::AjmAt9GetCodecInfo(config.data(), codec);
}

static int Ngs2ParseRiffWaveform(const void* data, size_t data_size, Ngs2WaveformInfo* info) {
	static constexpr uint8_t ATRAC9_GUID[16] = {0xd2, 0x42, 0xe1, 0x47, 0xba, 0x36, 0x8d, 0x4d,
	                                            0x88, 0xfc, 0x61, 0x65, 0x4f, 0x8c, 0x83, 0x6c};
	static constexpr uint8_t PCM_GUID[16]    = {0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00,
	                                            0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71};
	const auto*              bytes           = static_cast<const uint8_t*>(data);
	if (bytes == nullptr || data_size < 12) {
		return NGS2_ERROR_INVALID_WAVEFORM_DATA;
	}
	if (!Ngs2FourCcEquals(bytes, "RIFF") || !Ngs2FourCcEquals(bytes + 8, "WAVE")) {
		return NGS2_ERROR_UNKNOWN_WAVEFORM_FORMAT;
	}

	const uint64_t riff_end64 = 8ull + Ngs2ReadLe32(bytes + 4);
	if (riff_end64 < 12) {
		return NGS2_ERROR_INVALID_WAVEFORM_DATA;
	}
	const auto riff_end = static_cast<size_t>(riff_end64);

	const uint8_t* format          = nullptr;
	size_t         format_size     = 0;
	const uint8_t* fact            = nullptr;
	size_t         fact_size       = 0;
	const uint8_t* smpl            = nullptr;
	size_t         smpl_size       = 0;
	size_t         waveform_offset = 0;
	uint32_t       waveform_size   = 0;

	for (size_t offset = 12; offset + 8 <= std::min(riff_end, data_size);) {
		const auto*    chunk      = bytes + offset;
		const auto     chunk_size = static_cast<size_t>(Ngs2ReadLe32(chunk + 4));
		const auto     payload    = offset + 8;
		const auto     is_data    = Ngs2FourCcEquals(chunk, "data");
		const uint64_t next       = static_cast<uint64_t>(payload) + chunk_size + (chunk_size & 1u);
		if (next > riff_end || (next > data_size && !is_data)) {
			return NGS2_ERROR_INVALID_WAVEFORM_DATA;
		}

		if (Ngs2FourCcEquals(chunk, "fmt ")) {
			format      = bytes + payload;
			format_size = chunk_size;
		} else if (Ngs2FourCcEquals(chunk, "fact")) {
			fact      = bytes + payload;
			fact_size = chunk_size;
		} else if (Ngs2FourCcEquals(chunk, "smpl")) {
			smpl      = bytes + payload;
			smpl_size = chunk_size;
		} else if (is_data) {
			if (payload > std::numeric_limits<uint32_t>::max()) {
				return NGS2_ERROR_INVALID_WAVEFORM_DATA;
			}
			waveform_offset = payload;
			waveform_size   = static_cast<uint32_t>(chunk_size);
		}

		if (next > data_size) {
			break;
		}
		offset = static_cast<size_t>(next);
	}

	if (format == nullptr || format_size < 16 || waveform_offset == 0) {
		return NGS2_ERROR_INVALID_WAVEFORM_FORMAT;
	}

	uint16_t tag = Ngs2ReadLe16(format);
	if (tag == 0xfffe) {
		// WAVE_FORMAT_EXTENSIBLE
		if (format_size < 40) {
			return NGS2_ERROR_UNKNOWN_WAVEFORM_FORMAT;
		}
		if (std::memcmp(format + 24, ATRAC9_GUID, sizeof(ATRAC9_GUID)) == 0) {
			if (format_size < 52 || fact == nullptr || fact_size < 12) {
				return NGS2_ERROR_INVALID_WAVEFORM_FORMAT;
			}
			std::array<uint8_t, ATRAC9_CONFIG_DATA_SIZE> config {};
			std::memcpy(config.data(), format + 44, config.size());
			Atrac9CodecInfo codec {};
			const bool      valid_codec   = Ngs2GetAtrac9CodecInfo(config, codec);
			const uint64_t  frame_samples = static_cast<uint64_t>(codec.frameSamples) *
			                                static_cast<uint64_t>(codec.framesInSuperframe);
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
			info->format.config_data   = (static_cast<uint32_t>(config[0]) << 24u) |
			                             (static_cast<uint32_t>(config[1]) << 16u) |
			                             (static_cast<uint32_t>(config[2]) << 8u) | config[3];
			info->data_offset          = static_cast<uint32_t>(waveform_offset);
			info->data_size            = waveform_size;
			info->num_samples          = Ngs2ReadLe32(fact);
			info->audio_unit_size =
			    static_cast<uint32_t>(codec.superframeSize / codec.framesInSuperframe);
			info->num_audio_unit_samples     = static_cast<uint32_t>(codec.frameSamples);
			info->num_audio_unit_per_frame   = static_cast<uint32_t>(codec.framesInSuperframe);
			info->audio_frame_size           = static_cast<uint32_t>(codec.superframeSize);
			info->num_audio_frame_samples    = static_cast<uint32_t>(frame_samples);
			info->num_delay_samples          = Ngs2ReadLe32(fact + 4);
			info->num_blocks                 = 1;
			info->blocks[0].data_offset      = waveform_offset;
			info->blocks[0].data_size        = waveform_size;
			info->blocks[0].num_skip_samples = Ngs2ReadLe32(fact + 8);
			info->blocks[0].num_samples      = info->num_samples;
			return OK;
		}
		// The SubFormat GUID selects the real codec: KSDATAFORMAT_SUBTYPE_PCM
		// (tag 1) or KSDATAFORMAT_SUBTYPE_IEEE_FLOAT (tag 3); the remaining
		// GUID bytes are identical for both.
		const uint16_t sub_tag = Ngs2ReadLe16(format + 24);
		if ((sub_tag != 1 && sub_tag != 3) ||
		    std::memcmp(format + 26, PCM_GUID + 2, sizeof(PCM_GUID) - 2) != 0) {
			return NGS2_ERROR_UNKNOWN_WAVEFORM_FORMAT;
		}
		tag = sub_tag;
	}

	// PCM or IEEE Float
	if (tag == 1 || tag == 3) {
		const bool     is_float  = (tag == 3);
		const uint16_t bits      = Ngs2ReadLe16(format + 14);
		uint32_t       wave_type = 0;
		if (is_float) {
			wave_type = (bits == 32 ? 0x18u : (bits == 64 ? 0x1au : 0u));
		} else {
			wave_type =
			    (bits == 8
			         ? 0x11u
			         : (bits == 16 ? 0x12u : (bits == 24 ? 0x14u : (bits == 32 ? 0x16u : 0u))));
		}
		if (wave_type == 0) {
			return NGS2_ERROR_UNKNOWN_WAVEFORM_FORMAT;
		}

		const uint32_t channels        = Ngs2ReadLe16(format + 2);
		const uint32_t sample_rate     = Ngs2ReadLe32(format + 4);
		const uint32_t bytes_per_frame = channels * (bits / 8u);
		if (channels == 0 || channels > 8 || sample_rate == 0 || sample_rate > 192000 ||
		    Ngs2ReadLe16(format + 12) != bytes_per_frame) {
			return NGS2_ERROR_INVALID_WAVEFORM_FORMAT;
		}
		if (bytes_per_frame == 0 || (waveform_size % bytes_per_frame) != 0) {
			return NGS2_ERROR_INVALID_WAVEFORM_DATA;
		}

		info->format.waveform_type       = wave_type;
		info->format.num_channels        = channels;
		info->format.sample_rate         = sample_rate;
		info->format.config_data         = 0;
		info->data_offset                = static_cast<uint32_t>(waveform_offset);
		info->data_size                  = waveform_size;
		info->num_samples                = waveform_size / bytes_per_frame;
		info->audio_unit_size            = bytes_per_frame;
		info->num_audio_unit_samples     = 1;
		info->num_audio_unit_per_frame   = 1;
		info->audio_frame_size           = bytes_per_frame;
		info->num_audio_frame_samples    = 1;
		info->num_delay_samples          = 0;
		info->num_blocks                 = 1;
		info->blocks[0].data_offset      = waveform_offset;
		info->blocks[0].data_size        = waveform_size;
		info->blocks[0].num_skip_samples = 0;
		info->blocks[0].num_samples      = info->num_samples;

		if (smpl != nullptr && smpl_size >= 36) {
			const uint32_t num_loops = Ngs2ReadLe32(smpl + 28);
			if (num_loops > 0 && smpl_size >= 52) {
				const uint32_t loop_start = Ngs2ReadLe32(smpl + 44);
				const uint32_t loop_end   = Ngs2ReadLe32(smpl + 48) + 1u;
				info->loop_begin_position = loop_start;
				info->loop_end_position   = loop_end;
				if (loop_end > loop_start && loop_end <= info->num_samples) {
					info->num_blocks = 0;
					if (loop_start != 0) {
						auto& b       = info->blocks[info->num_blocks++];
						b.data_offset = waveform_offset;
						b.data_size   = static_cast<uint64_t>(loop_start) * bytes_per_frame;
						b.num_samples = loop_start;
					}
					auto& b_loop = info->blocks[info->num_blocks++];
					b_loop.data_offset =
					    waveform_offset + static_cast<uint64_t>(loop_start) * bytes_per_frame;
					b_loop.data_size =
					    static_cast<uint64_t>(loop_end - loop_start) * bytes_per_frame;
					b_loop.num_repeats = UINT32_MAX;
					b_loop.num_samples = loop_end - loop_start;
					if (loop_end != info->num_samples) {
						auto& b_end = info->blocks[info->num_blocks++];
						b_end.data_offset =
						    waveform_offset + static_cast<uint64_t>(loop_end) * bytes_per_frame;
						b_end.data_size =
						    static_cast<uint64_t>(info->num_samples - loop_end) * bytes_per_frame;
						b_end.num_samples = info->num_samples - loop_end;
					}
				}
			}
		}
		return OK;
	}

	return NGS2_ERROR_UNKNOWN_WAVEFORM_FORMAT;
}

static int Ngs2ParseVag(const void* data, size_t data_size, Ngs2WaveformInfo* info) {
	static constexpr uint32_t VAG_FRAME_SAMPLES   = 28;
	static constexpr uint32_t VAG_FRAME_BYTES     = 16;
	static constexpr uint32_t VAG_HEADER_SIZE     = 0x30;
	static constexpr uint8_t  VAG_FLAG_LOOP_END   = 3;
	static constexpr uint8_t  VAG_FLAG_LOOP_START = 6;

	const auto* bytes = static_cast<const uint8_t*>(data);
	if (bytes == nullptr || data_size < VAG_HEADER_SIZE) {
		return NGS2_ERROR_INVALID_WAVEFORM_DATA;
	}
	if (std::memcmp(bytes, "VAGp", 4) != 0) {
		return NGS2_ERROR_UNKNOWN_WAVEFORM_FORMAT;
	}

	const uint32_t channels    = bytes[0x1e] != 0 ? bytes[0x1e] : 1u;
	const uint32_t frame_bytes = VAG_FRAME_BYTES * channels;
	const uint32_t data_size_val =
	    (static_cast<uint32_t>(bytes[0x0c]) << 24u) | (static_cast<uint32_t>(bytes[0x0d]) << 16u) |
	    (static_cast<uint32_t>(bytes[0x0e]) << 8u) | static_cast<uint32_t>(bytes[0x0f]);
	const uint32_t sample_rate =
	    (static_cast<uint32_t>(bytes[0x10]) << 24u) | (static_cast<uint32_t>(bytes[0x11]) << 16u) |
	    (static_cast<uint32_t>(bytes[0x12]) << 8u) | static_cast<uint32_t>(bytes[0x13]);

	const size_t payload = data_size - VAG_HEADER_SIZE;
	if (data_size_val > payload || payload < static_cast<size_t>(frame_bytes) + VAG_FRAME_BYTES) {
		return NGS2_ERROR_INVALID_WAVEFORM_DATA;
	}
	if (channels > 8 || sample_rate == 0 || sample_rate > 192000 ||
	    (data_size_val % frame_bytes) != 0) {
		return NGS2_ERROR_INVALID_WAVEFORM_DATA;
	}

	const bool     drops_frame = (bytes[VAG_HEADER_SIZE + frame_bytes + 1] & 2) == 0;
	const uint64_t last_frame  = data_size_val != 0 ? (data_size_val - 1u) / frame_bytes : 0;
	const uint64_t scan_bytes  = last_frame * frame_bytes + VAG_FRAME_BYTES;
	if (!drops_frame && data_size_val != 0 && scan_bytes > payload) {
		return NGS2_ERROR_INVALID_WAVEFORM_DATA;
	}

	info->format.waveform_type     = SCE_NGS2_WAVEFORM_TYPE_VAG;
	info->format.num_channels      = channels;
	info->format.sample_rate       = sample_rate;
	info->format.config_data       = 0;
	info->data_offset              = VAG_HEADER_SIZE;
	info->data_size                = data_size_val;
	info->num_samples              = (data_size_val / frame_bytes) * VAG_FRAME_SAMPLES;
	info->audio_unit_size          = frame_bytes;
	info->num_audio_unit_samples   = VAG_FRAME_SAMPLES;
	info->num_audio_unit_per_frame = 1;
	info->audio_frame_size         = frame_bytes;
	info->num_audio_frame_samples  = VAG_FRAME_SAMPLES;
	info->num_delay_samples        = 0;

	if (drops_frame) {
		if (info->num_samples < VAG_FRAME_SAMPLES) {
			return NGS2_ERROR_INVALID_WAVEFORM_DATA;
		}
		info->num_samples -= VAG_FRAME_SAMPLES;
	} else if (data_size_val != 0) {
		const auto* frames = bytes + VAG_HEADER_SIZE;
		for (uint64_t frame = 0; frame <= last_frame; ++frame) {
			const uint8_t flag = frames[frame * frame_bytes + 1] & 0x0fu;
			if (flag == VAG_FLAG_LOOP_END) {
				info->loop_end_position = static_cast<uint32_t>(frame + 1) * VAG_FRAME_SAMPLES;
			} else if (flag == VAG_FLAG_LOOP_START) {
				info->loop_begin_position = static_cast<uint32_t>(frame) * VAG_FRAME_SAMPLES;
			}
		}
	}

	const auto make_vag_block = [&](uint32_t pos, uint32_t count, uint32_t repeats) {
		const uint64_t first = pos / VAG_FRAME_SAMPLES;
		const uint64_t past  = (uint64_t(pos) + count + VAG_FRAME_SAMPLES - 1u) / VAG_FRAME_SAMPLES;
		const uint64_t offset = first * frame_bytes;
		Ngs2WaveformBlock b {};
		b.data_offset      = VAG_HEADER_SIZE + offset;
		b.data_size        = past * frame_bytes - offset;
		b.num_samples      = static_cast<uint32_t>(std::min<uint64_t>(
		    (past - first) * VAG_FRAME_SAMPLES, std::numeric_limits<uint32_t>::max()));
		b.num_skip_samples = 0;
		b.num_repeats      = repeats;
		return b;
	};

	const uint32_t begin = info->loop_begin_position;
	const uint32_t end   = info->loop_end_position;
	info->num_blocks     = 0;
	if (end <= begin || end > info->num_samples) {
		info->blocks[info->num_blocks++] = make_vag_block(0, info->num_samples, 0);
		return OK;
	}
	if (begin != 0) {
		info->blocks[info->num_blocks++] = make_vag_block(0, begin, 0);
	}
	info->blocks[info->num_blocks++] = make_vag_block(begin, end - begin, UINT32_MAX);
	if (end != info->num_samples) {
		info->blocks[info->num_blocks++] = make_vag_block(end, info->num_samples - end, 0);
	}
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
	if (data != nullptr && data_size >= 4 && std::memcmp(data, "VAGp", 4) == 0) {
		return Ngs2ParseVag(data, data_size, info);
	}
	return Ngs2ParseRiffWaveform(data, data_size, info);
}

int KYTY_SYSV_ABI Ngs2ParseWaveformFile(const char* path, uint32_t offset, Ngs2WaveformInfo* info) {
	PRINT_NAME();
	LOGF("\t path = %s, offset = %" PRIu32 "\n", path != nullptr ? path : "(null)", offset);

	if (info == nullptr) {
		return NGS2_ERROR_INVALID_OUT_ADDRESS;
	}
	std::memset(info, 0, sizeof(Ngs2WaveformInfo));

	if (path == nullptr) {
		return NGS2_ERROR_INVALID_WAVEFORM_DATA;
	}

	// Guest paths go through the same mount resolver as the other file APIs;
	// an unmounted path must not fall through to the host filesystem.
	const auto real_path = Libs::LibKernel::FileSystem::GetRealFilename(path);
	if (real_path.empty()) {
		return NGS2_ERROR_INVALID_WAVEFORM_DATA;
	}
	std::ifstream file(real_path, std::ios::binary | std::ios::ate);
	if (!file) {
		return NGS2_ERROR_INVALID_WAVEFORM_DATA;
	}
	const auto file_size = static_cast<uint64_t>(file.tellg());
	if (static_cast<uint64_t>(offset) > file_size) {
		return NGS2_ERROR_INVALID_WAVEFORM_DATA;
	}

	// Read the leading header first and bound the payload read to the extent
	// declared by the container instead of copying the whole remaining file.
	// The probe itself is bounded by the remaining file size: a short file
	// must not leave the stream in a failed state for the payload read.
	uint8_t header[0x30] {};
	file.seekg(static_cast<std::streamoff>(offset));
	const auto probe_size = static_cast<std::streamsize>(
	    std::min<uint64_t>(sizeof(header), file_size - static_cast<uint64_t>(offset)));
	file.read(reinterpret_cast<char*>(header), probe_size);
	const auto header_size = static_cast<size_t>(file.gcount());
	if (header_size != static_cast<size_t>(probe_size)) {
		return NGS2_ERROR_INVALID_WAVEFORM_DATA;
	}
	file.clear();

	uint64_t extent_end = file_size;
	if (header_size >= 12 && std::memcmp(header, "RIFF", 4) == 0 &&
	    std::memcmp(header + 8, "WAVE", 4) == 0) {
		const uint64_t riff_size =
		    (static_cast<uint64_t>(header[4]) << 0) | (static_cast<uint64_t>(header[5]) << 8) |
		    (static_cast<uint64_t>(header[6]) << 16) | (static_cast<uint64_t>(header[7]) << 24);
		const uint64_t end = static_cast<uint64_t>(offset) + 8 + riff_size;
		if (end < static_cast<uint64_t>(offset) + 12 || end > file_size) {
			return NGS2_ERROR_INVALID_WAVEFORM_DATA;
		}
		extent_end = end;
	} else if (header_size >= 0x1f && std::memcmp(header, "VAGp", 4) == 0) {
		const uint64_t vag_size    = (static_cast<uint64_t>(header[0x0c]) << 24) |
		                             (static_cast<uint64_t>(header[0x0d]) << 16) |
		                             (static_cast<uint64_t>(header[0x0e]) << 8) |
		                             static_cast<uint64_t>(header[0x0f]);
		const uint32_t channels    = header[0x1e] != 0 ? header[0x1e] : 1u;
		const uint64_t frame_bytes = 16ull * channels;
		// The parser needs the declared payload plus one frame of slack.
		const uint64_t payload = std::max<uint64_t>(vag_size, frame_bytes + 16);
		const uint64_t end     = static_cast<uint64_t>(offset) + 0x30 + payload;
		if (end > file_size) {
			return NGS2_ERROR_INVALID_WAVEFORM_DATA;
		}
		extent_end = end;
	} else {
		// Unknown container: the parser only inspects the leading bytes.
		extent_end = std::min<uint64_t>(file_size, static_cast<uint64_t>(offset) + 16);
	}

	const auto           length = static_cast<size_t>(extent_end - static_cast<uint64_t>(offset));
	std::vector<uint8_t> buffer(length);
	file.seekg(static_cast<std::streamoff>(offset));
	if (!file.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(length))) {
		return NGS2_ERROR_INVALID_WAVEFORM_DATA;
	}

	const int result = Ngs2ParseWaveformData(buffer.data(), length, info);
	if (result == 0) {
		// Absolute offsets are computed in 64 bits: the file offset may push
		// the payload past what the 32-bit ABI field can represent.
		const uint64_t absolute_data_offset =
		    static_cast<uint64_t>(offset) + info->data_offset;
		if (absolute_data_offset > std::numeric_limits<uint32_t>::max()) {
			return NGS2_ERROR_INVALID_WAVEFORM_DATA;
		}
		info->data_offset = static_cast<uint32_t>(absolute_data_offset);
		for (uint32_t i = 0; i < info->num_blocks; i++) {
			const uint64_t absolute_block_offset =
			    static_cast<uint64_t>(offset) + info->blocks[i].data_offset;
			if (absolute_block_offset > std::numeric_limits<uintptr_t>::max()) {
				return NGS2_ERROR_INVALID_WAVEFORM_DATA;
			}
			info->blocks[i].data_offset = static_cast<uintptr_t>(absolute_block_offset);
		}
	}
	return result;
}

int KYTY_SYSV_ABI Ngs2CalcWaveformBlock(const Ngs2WaveformFormat* format, uint32_t sample_pos,
                                        uint32_t num_samples, Ngs2WaveformBlock* block) {
	PRINT_NAME();
	LOGF("\t format = 0x%016" PRIx64 ", sample_pos = %" PRIu32 ", num_samples = %" PRIu32 "\n",
	     reinterpret_cast<uint64_t>(format), sample_pos, num_samples);

	EXIT_NOT_IMPLEMENTED(block == nullptr);

	std::memset(block, 0, sizeof(Ngs2WaveformBlock));
	// ATRAC9 channel counts are validated against the codec below; the other
	// formats are limited to the eight channels the RIFF parser accepts.
	if (format == nullptr || format->num_channels == 0 || format->sample_rate == 0 ||
	    format->sample_rate > 192000 ||
	    (format->waveform_type != NGS2_WAVEFORM_TYPE_ATRAC9 && format->num_channels > 8)) {
		return NGS2_ERROR_INVALID_WAVEFORM_FORMAT;
	}
	if (format->waveform_type == NGS2_WAVEFORM_TYPE_ATRAC9) {
		std::array<uint8_t, ATRAC9_CONFIG_DATA_SIZE> config {
		    static_cast<uint8_t>(format->config_data >> 24u),
		    static_cast<uint8_t>(format->config_data >> 16u),
		    static_cast<uint8_t>(format->config_data >> 8u),
		    static_cast<uint8_t>(format->config_data)};
		Atrac9CodecInfo codec {};
		if (!Ngs2GetAtrac9CodecInfo(config, codec) ||
		    codec.channels != static_cast<int>(format->num_channels) ||
		    codec.samplingRate != static_cast<int>(format->sample_rate)) {
			return NGS2_ERROR_INVALID_WAVEFORM_FORMAT;
		}
		const uint64_t frame_samples = uint64_t(codec.frameSamples) * codec.framesInSuperframe;
		const uint64_t skip          = sample_pos % frame_samples;
		block->data_offset           = uint64_t(sample_pos / frame_samples) * codec.superframeSize;
		block->data_size =
		    num_samples == 0
		        ? 0
		        : ((skip + num_samples + frame_samples - 1) / frame_samples) * codec.superframeSize;
		block->num_skip_samples = num_samples == 0 ? 0 : static_cast<uint32_t>(skip);
	} else if (format->waveform_type == SCE_NGS2_WAVEFORM_TYPE_VAG) {
		constexpr uint32_t VAG_FRAME_SAMPLES = 28;
		constexpr uint32_t VAG_FRAME_BYTES   = 16;
		const uint64_t     frame_bytes       = uint64_t(VAG_FRAME_BYTES) * format->num_channels;
		const uint64_t     first             = sample_pos / VAG_FRAME_SAMPLES;
		const uint64_t     past =
		    (uint64_t(sample_pos) + num_samples + VAG_FRAME_SAMPLES - 1u) / VAG_FRAME_SAMPLES;
		const uint64_t offset = first * frame_bytes;
		const uint64_t end    = past * frame_bytes;
		if (end > std::numeric_limits<uintptr_t>::max() ||
		    end - offset > std::numeric_limits<size_t>::max()) {
			return NGS2_ERROR_INVALID_WAVEFORM_DATA;
		}
		block->data_offset      = offset;
		block->data_size        = num_samples == 0 ? 0 : end - offset;
		block->num_skip_samples = num_samples == 0 ? 0 : (sample_pos % VAG_FRAME_SAMPLES);
	} else if (format->waveform_type == 0x11 || format->waveform_type == 0x12 ||
	           format->waveform_type == 0x14 || format->waveform_type == 0x16 ||
	           format->waveform_type == 0x18 || format->waveform_type == 0x1a) {
		// PCM has fixed-size frames, so the block starts exactly at the sample.
		const uint64_t frame_size =
		    uint64_t(format->num_channels) * Ngs2PcmSampleSize(format->waveform_type);
		block->data_offset = uint64_t(sample_pos) * frame_size;
		block->data_size   = uint64_t(num_samples) * frame_size;
	} else {
		return NGS2_ERROR_UNKNOWN_WAVEFORM_FORMAT;
	}
	block->num_samples = num_samples;
	return OK;
}

// Interpolate along either arc between the stereo speakers. The optional positions use the
// same angle units as the source; geometry uses the default L=270, R=90 degree placement.
static void PanStereoGains(const Ngs2PanParam& param, float unit_angle, float* left, float* right,
                           const float* speaker_angles = nullptr) {
	constexpr float pi = std::numbers::pi_v<float>;
	// Preserve distinct float speaker positions when converting them to turns.
	const auto wrap = [](double turns) {
		const double value = std::fmod(turns, 1.0);
		return value < 0.0f ? value + 1.0f : value;
	};
	const auto turns = [unit_angle](float angle) {
		return std::remainder(double(angle), double(unit_angle)) / double(unit_angle);
	};
	const double left_angle  = speaker_angles == nullptr ? 0.75 : turns(speaker_angles[0]);
	const double right_angle = speaker_angles == nullptr ? 0.25 : turns(speaker_angles[1]);
	const double span        = wrap(right_angle - left_angle);

	auto pan = [&](double angle, float weight, float* l, float* r) {
		const double offset = wrap(angle - left_angle);
		const double t      = offset <= span ? offset / span : (1.0f - offset) / (1.0f - span);
		*l += static_cast<float>(std::max(0.0, std::cos(t * pi * 0.5)) * weight);
		*r += static_cast<float>(std::sin(t * pi * 0.5) * weight);
	};

	const float  distance = std::clamp(param.distance, -1.0f, 1.0f);
	const float  toward   = std::sin((distance + 1.0f) * 0.25f * pi);
	const float  opposite = std::sin((1.0f - distance) * 0.25f * pi);
	const double angle    = turns(param.angle);
	float        l = 0.0f, r = 0.0f;
	pan(angle, toward, &l, &r);
	pan(wrap(angle + 0.5f), opposite, &l, &r);
	*left  = l * param.fbw_level;
	*right = r * param.fbw_level;
}

int KYTY_SYSV_ABI Ngs2PanInit(Ngs2PanWork* work, const float* speaker_angles, float unit_angle,
                              uint32_t num_speakers) {
	PRINT_NAME();
	LOGF("\t work = 0x%016" PRIx64 ", num_speakers = %" PRIu32 "\n",
	     reinterpret_cast<uint64_t>(work), num_speakers);

	if (work == nullptr) {
		return NGS2_ERROR_INVALID_OUT_ADDRESS;
	}
	if (unit_angle != 360.0f && unit_angle != 2.0f * std::numbers::pi_v<float>) {
		return static_cast<int32_t>(0x804a8450u); // Invalid pan angle unit.
	}
	static constexpr float stereo[]        = {270.0f, 90.0f};
	static constexpr float quad[]          = {320.0f, 40.0f, 250.0f, 110.0f};
	static constexpr float surround[]      = {320.0f, 40.0f, 0.0f, 250.0f, 110.0f};
	static constexpr float surround_back[] = {320.0f, 40.0f, 0.0f, 250.0f, 110.0f, 210.0f, 150.0f};
	const float*           defaults        = nullptr;
	switch (num_speakers) {
		case 2: defaults = stereo; break;
		case 4: defaults = quad; break;
		case 5: defaults = surround; break;
		case 7: defaults = surround_back; break;
		default: return static_cast<int32_t>(0x804a8451u); // Invalid pan speaker.
	}
	Ngs2PanWork result {};
	result.unit_angle   = unit_angle;
	result.num_speakers = num_speakers;
	for (uint32_t i = 0; i < num_speakers; i++) {
		const float angle =
		    speaker_angles == nullptr ? defaults[i] / 360.0f * unit_angle : speaker_angles[i];
		if (!std::isfinite(angle)) {
			return static_cast<int32_t>(0x804a8451u);
		}
		result.speaker_angles[i] = angle;
		for (uint32_t j = 0; j < i; j++) {
			const double difference =
			    std::remainder(double(angle), double(unit_angle)) -
			    std::remainder(double(result.speaker_angles[j]), double(unit_angle));
			if (std::remainder(difference, double(unit_angle)) == 0.0) {
				return static_cast<int32_t>(0x804a8451u);
			}
		}
	}
	*work = result;
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

	if (work != nullptr && work->num_speakers == 2 && matrix_format == 2 && params != nullptr) {
		for (uint32_t p = 0; p < num_params; p++) {
			float left = 0.0f, right = 0.0f;
			PanStereoGains(params[p], work->unit_angle, &left, &right, work->speaker_angles);
			out_volume_matrix[0 * num_params + p] = left;
			out_volume_matrix[1 * num_params + p] = right;
		}
		return OK;
	}

	const auto channels = (matrix_format == 0 ? 2u : std::min<uint32_t>(matrix_format, 8));
	for (uint32_t p = 0; p < num_params; p++) {
		for (uint32_t c = 0; c < channels; c++) {
			out_volume_matrix[p * channels + c] = (c == 0 ? 1.0f : 0.0f);
		}
	}

	return OK;
}

static Ngs2GeomVector GeomSub(const Ngs2GeomVector& a, const Ngs2GeomVector& b) {
	return {a.x - b.x, a.y - b.y, a.z - b.z};
}

static Ngs2GeomVector GeomScale(const Ngs2GeomVector& a, float k) {
	return {a.x * k, a.y * k, a.z * k};
}

static float GeomDot(const Ngs2GeomVector& a, const Ngs2GeomVector& b) {
	return a.x * b.x + a.y * b.y + a.z * b.z;
}

static float GeomLength(const Ngs2GeomVector& a) {
	return std::sqrt(GeomDot(a, a));
}

static Ngs2GeomVector GeomCross(const Ngs2GeomVector& a, const Ngs2GeomVector& b) {
	return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

static Ngs2GeomVector GeomNormalize(const Ngs2GeomVector& a, const Ngs2GeomVector& fallback) {
	const float length = GeomLength(a);
	return length > 1e-6f ? GeomScale(a, 1.0f / length) : fallback;
}

int KYTY_SYSV_ABI Ngs2GeomResetListenerParam(Ngs2GeomListenerParam* out_listener_param) {
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(out_listener_param == nullptr);

	std::memset(out_listener_param, 0, sizeof(Ngs2GeomListenerParam));
	out_listener_param->orient_front.z = -1.0f;
	out_listener_param->orient_up.y    = 1.0f;
	out_listener_param->sound_speed    = 340.29f;

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

	// The work area is private to this implementation: the rows hold the listener's right, up and
	// front axes followed by its position. Flag bit 0 selects left-handed coordinates.
	const bool right_handed = (flags & 0x1u) == 0;
	auto front = GeomNormalize(param->orient_front, {0.0f, 0.0f, right_handed ? -1.0f : 1.0f});
	auto right =
	    right_handed ? GeomCross(front, param->orient_up) : GeomCross(param->orient_up, front);
	right                        = GeomNormalize(right, {1.0f, 0.0f, 0.0f});
	const auto           up      = right_handed ? GeomCross(right, front) : GeomCross(front, right);
	const Ngs2GeomVector rows[4] = {right, up, front, param->position};
	for (uint32_t i = 0; i < 4; i++) {
		out_work->matrix[i][0] = rows[i].x;
		out_work->matrix[i][1] = rows[i].y;
		out_work->matrix[i][2] = rows[i].z;
	}
	out_work->velocity    = param->velocity;
	out_work->sound_speed = (param->sound_speed > 0.0f ? param->sound_speed : 340.29f);
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
	out_attrib->pitch_ratio = 1.0f;

	const Ngs2GeomVector listener_pos {listener->matrix[3][0], listener->matrix[3][1],
	                                   listener->matrix[3][2]};
	const Ngs2GeomVector right {listener->matrix[0][0], listener->matrix[0][1],
	                            listener->matrix[0][2]};
	const Ngs2GeomVector front {listener->matrix[2][0], listener->matrix[2][1],
	                            listener->matrix[2][2]};

	const auto  offset   = GeomSub(source->position, listener_pos);
	const float distance = GeomLength(offset);

	// Models 3..5 clamp distance; models 0..2 continue attenuating beyond max_distance.
	const float reference     = std::max(source->rolloff.reference_distance, 1e-6f);
	const float max_dist      = std::max(source->rolloff.max_distance, reference);
	const bool  clamped_model = source->rolloff.model >= 3 && source->rolloff.model <= 5;
	const float rolloff_distance =
	    clamped_model ? std::clamp(distance, reference, max_dist) : distance;
	const float factor = source->rolloff.rolloff_factor;
	float       gain   = 1.0f;
	switch (source->rolloff.model) {
		case 1:
		case 4:
			gain = (max_dist > reference)
			           ? 1.0f - factor * (rolloff_distance - reference) / (max_dist - reference)
			           : 1.0f;
			break;
		case 2:
		case 5: gain = std::pow(rolloff_distance / reference, -factor); break;
		default: gain = reference / (reference + factor * (rolloff_distance - reference)); break;
	}
	gain = std::max(gain, 0.0f);

	// CALC_SOURCE_ANGLE opts into source cone directionality, including 360-degree outer cones.
	if ((flags & 0x1u) != 0 && distance > 0.0f && GeomLength(source->direction) > 1e-6f) {
		const auto  axis        = GeomNormalize(source->direction, {0.0f, 0.0f, 1.0f});
		const auto  to_listen   = GeomScale(offset, -1.0f / distance);
		const float angle       = std::acos(std::clamp(GeomDot(axis, to_listen), -1.0f, 1.0f)) *
		                          (180.0f / std::numbers::pi_v<float>)*2.0f;
		const float inner_level = std::clamp(source->cone.inner_level, 0.0f, 2.0f);
		const float outer_level = std::clamp(source->cone.outer_level, 0.0f, 2.0f);
		float       cone        = 1.0f;
		if (angle <= source->cone.inner_angle) {
			cone = inner_level;
		} else if (angle >= source->cone.outer_angle) {
			cone = outer_level;
		} else {
			const float t = (angle - source->cone.inner_angle) /
			                (source->cone.outer_angle - source->cone.inner_angle);
			cone          = inner_level + t * (outer_level - inner_level);
		}
		gain = cone == 0.0f ? 0.0f : gain * cone;
	}

	// Bound the gain after cone amplification, before scaling, so a muted colocated source
	// remains finite even when an unclamped rolloff has an infinite gain at distance zero.
	gain              = std::clamp(gain, 0.0f, 1.0f);
	const float level = source->min_level + (source->max_level - source->min_level) * gain;
	if ((flags & 0x8u) != 0) { // CALC_A3D_ATTRIBUTE
		out_attrib->a3d_attrib.position = source->position;
		out_attrib->a3d_attrib.volume   = level;
	}

	// CALC_SOURCE_DOPLLER opts into the velocity-dependent pitch shift.
	if ((flags & 0x2u) != 0 && distance > 0.0f && source->doppler_factor > 0.0f) {
		const float sound = std::max(listener->sound_speed, 1.0f);
		// Source-to-listener direction, as in the DS3D/OpenAL Doppler model.
		const auto  axis      = GeomScale(offset, -1.0f / distance);
		const float max_speed = sound * (1.0f - 1e-6f);
		const float vl =
		    std::min(source->doppler_factor * GeomDot(listener->velocity, axis), max_speed);
		const float vs =
		    std::min(source->doppler_factor * GeomDot(source->velocity, axis), max_speed);
		out_attrib->pitch_ratio = std::clamp((sound - vl) / (sound - vs), 0.0f, 4.0f);
	}

	if ((flags & 0x4u) == 0) { // CALC_VOLUME_MATRIX
		return OK;
	}

	// Azimuth of the source from the listener's front, positive to the right.
	float angle  = 0.0f;
	float spread = 0.0f;
	if (distance > source->radius && distance > 1e-6f) {
		angle = std::atan2(GeomDot(offset, right), GeomDot(offset, front)) *
		        (180.0f / std::numbers::pi_v<float>);
	} else {
		spread = 1.0f; // At the listener or inside the source's radius: no direction.
	}

	// Geometry returns a single input row with matrix_format output channels.
	const uint32_t outputs =
	    (source->matrix_format == 0 ? 2u : std::min<uint32_t>(source->matrix_format, 8));
	if (outputs == 1) {
		out_attrib->level[0] = source->fbw_level * level;
	} else if (source->num_speakers == 2 || source->num_speakers == 0) {
		float        left = 0.0f, right_gain = 0.0f;
		Ngs2PanParam pan {angle, 1.0f, source->fbw_level, source->lfe_level};
		PanStereoGains(pan, 360.0f, &left, &right_gain);
		if (spread > 0.0f) {
			left = right_gain = source->fbw_level * std::numbers::sqrt2_v<float> / 2;
		}
		out_attrib->level[0] = left * level;
		out_attrib->level[1] = right_gain * level;
	} else {
		for (uint32_t ch = 0; ch < outputs; ch++) {
			out_attrib->level[ch * 8 + ch] = level;
		}
	}

	return OK;
}

int KYTY_SYSV_ABI Ngs2RackGetVoiceHandle(uintptr_t rack_handle, uint32_t voice_id,
                                         uintptr_t* handle) {
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(handle == nullptr);
	EXIT_NOT_IMPLEMENTED(rack_handle == 0);

	LOGF("\t voice_id = %u\n", voice_id);

	// Lookup and all rack/voice reads happen under g_racks_mutex so a rack
	// cannot be destroyed between the lookup and the handle computation.
	Common::LockGuard racks_lock(g_racks_mutex);
	auto*           rack = Ngs2FindRackLocked(rack_handle);
	if (rack == nullptr) {
		return NGS2_ERROR_INVALID_RACK_HANDLE;
	}
	auto* voices = reinterpret_cast<Ngs2VoiceInternal*>(rack + 1);

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

// A patch to a rack handle connects to voice #0 of that rack.
static Ngs2VoiceInternal* Ngs2FindPatchDestination(uintptr_t handle) {
	Common::LockGuard racks_lock(g_racks_mutex);
	for (auto* rack = g_racks_list; rack != nullptr; rack = rack->next) {
		if (reinterpret_cast<uintptr_t>(rack) == handle) {
			return reinterpret_cast<Ngs2VoiceInternal*>(rack + 1);
		}
	}
	return reinterpret_cast<Ngs2VoiceInternal*>(handle);
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
						    Ngs2FindPatchDestination(patch->dest_handle);
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
			case 0x2000: {
				EXIT_NOT_IMPLEMENTED(voice->rack->type != Ngs2RackType::Submixer &&
				                     voice->rack->type != Ngs2RackType::CustomSubmixer);
				if ((param->id & 0xffffu) == 0) {
					EXIT_NOT_IMPLEMENTED(param->size != sizeof(Ngs2SubmixerVoiceSetupParam));
					const auto& setup =
					    *reinterpret_cast<const Ngs2SubmixerVoiceSetupParam*>(param);
					const uint32_t max_channels =
					    (voice->rack->type == Ngs2RackType::CustomSubmixer)
					        ? voice->rack->option.custom_submixer.max_channels
					        : voice->rack->option.submixer.max_channels;
					EXIT_NOT_IMPLEMENTED(setup.num_io_channels == 0 ||
					                     setup.num_io_channels > max_channels || setup.flags != 0);
					voice->ResetSetupState();
					voice->channels = setup.num_io_channels;
				}
				break;
			}
			case 0x2001: {
				EXIT_NOT_IMPLEMENTED(voice->rack->type != Ngs2RackType::Reverb);
				const auto* values = reinterpret_cast<const uint32_t*>(param + 1);
				switch (param->id & 0xffffu) {
					case 0:
						// Input and output channels, flags; channel conversion stays silent.
						EXIT_NOT_IMPLEMENTED(values[0] == 0 || values[2] != 0);
						voice->ResetSetupState();
						voice->reverb.params = {};
						voice->channels      = values[1] == values[0] ? values[0] : 0;
						break;
					case 1: {
						// The reverb parameters start with the wet and dry levels.
						const auto* levels = reinterpret_cast<const float*>(values);
						auto&       params = voice->reverb.params;
						params.wet         = std::clamp(levels[0], 0.0f, 32.0f);
						params.dry         = std::clamp(levels[1], 0.0f, 32.0f);
						params.room        = std::clamp(static_cast<int32_t>(values[2]), -10000, 0);
						params.room_hf     = std::clamp(static_cast<int32_t>(values[3]), -10000, 0);
						params.decay_time  = std::clamp(levels[5], 0.1f, 20.0f);
						params.hf_ratio    = std::clamp(levels[6], 0.01f, 2.0f);
						params.reflections =
						    std::clamp(static_cast<int32_t>(values[7]), -10000, 1000);
						params.reflections_delay = std::clamp(levels[8], 0.0f, 0.3f);
						params.reverb = std::clamp(static_cast<int32_t>(values[9]), -10000, 2000);
						params.reverb_delay = std::clamp(levels[10], 0.0f, 0.1f);
						params.hf_reference = std::clamp(levels[13], 20.0f, 20000.0f);
						params.channel_mask = values[14];
						voice->reverb.Apply();
						break;
					}
					default: EXIT("unsupported reverb control: 0x%08" PRIx32 "\n", param->id);
				}
				break;
			}
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
					case 2: {
						// SCE_NGS2_MASTERING_VOICE_PARAM_LFE
						EXIT_NOT_IMPLEMENTED(param->size < sizeof(Ngs2MasteringVoiceEffectParam));
						const auto* effect =
						    reinterpret_cast<const Ngs2MasteringVoiceEffectParam*>(param);
						voice->mastering_lfe.enabled = effect->enable_flag != 0;
						voice->mastering_lfe.cutoff  = effect->value <= 80 ? 80u : 120u;
						break;
					}
					case 3: {
						// SCE_NGS2_MASTERING_VOICE_PARAM_LIMITER
						EXIT_NOT_IMPLEMENTED(param->size < sizeof(Ngs2MasteringVoiceEffectParam));
						const auto* effect =
						    reinterpret_cast<const Ngs2MasteringVoiceEffectParam*>(param);
						float threshold = 1.0f;
						std::memcpy(&threshold, &effect->value, sizeof(threshold));
						voice->mastering_limiter.enabled =
						    (effect->enable_flag != 0) && std::isfinite(threshold);
						voice->mastering_limiter.threshold = std::clamp(threshold, 0.0f, 1.0f);
						break;
					}
					case 4: {
						const auto* levels = reinterpret_cast<const float*>(values);
						voice->fbw_level   = std::clamp(levels[0], 0.0f, 4.0f);
						voice->lfe_level   = std::clamp(levels[1], 0.0f, 4.0f);
						break;
					}
					case 5: voice->output_id = values[0]; break;
					case 6: {
						// SCE_NGS2_MASTERING_VOICE_PARAM_PEAK_METER
						EXIT_NOT_IMPLEMENTED(param->size < sizeof(Ngs2MasteringVoiceEffectParam));
						const auto* effect =
						    reinterpret_cast<const Ngs2MasteringVoiceEffectParam*>(param);
						voice->peak_meter_mode = std::min(effect->enable_flag, 2u);
						break;
					}
					default: EXIT("unsupported mastering control: 0x%08" PRIx32 "\n", param->id);
				}
				break;
			}
			case 0x4000: {
				EXIT_NOT_IMPLEMENTED(!Ngs2RackIsCustom(voice->rack->type));
				auto index = param->id & 0x1fu;
				if (voice->rack->type == Ngs2RackType::CustomSampler && index > 0 &&
				    (index - 1) < voice->modules.size() &&
				    voice->rack->option.custom_sampler.custom_rack_option.module[index - 1]
				            .module_id == ((param->id >> 8u) & 0xffu)) {
					index--;
				}
				EXIT_NOT_IMPLEMENTED(index >= voice->modules.size());
				const auto module_id =
				    voice->rack->option.custom_sampler.custom_rack_option.module[index].module_id;
				auto& module = voice->modules[index];
				if (module_id != SCE_NGS2_CUSTOM_MODULE_ID_GAIN &&
				    module_id != SCE_NGS2_CUSTOM_MODULE_ID_CHANNEL_MIXER &&
				    module_id != SCE_NGS2_CUSTOM_MODULE_ID_PITCH_SHIFT &&
				    module_id != SCE_NGS2_CUSTOM_MODULE_ID_FILTER && module_id != 0x19 &&
				    module_id != 0x1f) {
					if (!module.control_warned) {
						Log::WriteToConsoleAndLog(fmt::sprintf(
						    "warning: NGS2 custom control 0x%08" PRIx32 " is ignored\n",
						    param->id));
						module.control_warned = true;
					}
					break;
				}
				if (module_id == SCE_NGS2_CUSTOM_MODULE_ID_FILTER) {
					EXIT_NOT_IMPLEMENTED(param->size < sizeof(Ngs2CustomVoiceFilterParam));
					const auto* fp = reinterpret_cast<const Ngs2CustomVoiceFilterParam*>(param);
					Ngs2SamplerFilterParam sfp {};
					sfp.channel_mask = fp->channel_mask;
					sfp.type         = fp->type;
					sfp.frequency    = fp->param.fcq.frequency;
					sfp.q            = fp->param.fcq.q;
					sfp.level        = fp->param.fcq.level;
					sfp.location     = 1;
					// Custom modules run on audio already resampled to the system
					// rate, so the filter must be designed for that rate.
					module.filter.Configure(sfp, voice->rack->ngs->option.sample_rate,
					                        voice->channels);
					module.filter_configured = true;
					break;
				}
				if (module_id == SCE_NGS2_CUSTOM_MODULE_ID_GAIN) {
					EXIT_NOT_IMPLEMENTED(param->size < sizeof(Ngs2CustomVoiceGainParam));
					const auto* gain_param =
					    reinterpret_cast<const Ngs2CustomVoiceGainParam*>(param);
					std::copy(gain_param->gain, gain_param->gain + 37, module.target_gain.begin());
					if (!module.gain_configured) {
						module.current_gain    = module.target_gain;
						module.gain_configured = true;
					}
					break;
				}
				if (module_id == SCE_NGS2_CUSTOM_MODULE_ID_CHANNEL_MIXER) {
					EXIT_NOT_IMPLEMENTED(param->size < sizeof(Ngs2VoiceParamHeader));
					const auto*  levels = reinterpret_cast<const float*>(param + 1);
					const size_t count  = std::min<size_t>(
					    (param->size - sizeof(Ngs2VoiceParamHeader)) / sizeof(float), 37 * 37);
					module.channel_matrix.resize(37 * 37);
					for (size_t i = 0; i < count; ++i) {
						module.channel_matrix[i] = std::clamp(levels[i], -32.0f, 32.0f);
					}
					module.mixer_configured = true;
					break;
				}
				if (module_id == SCE_NGS2_CUSTOM_MODULE_ID_PITCH_SHIFT) {
					EXIT_NOT_IMPLEMENTED(param->size < sizeof(Ngs2CustomVoicePitchShiftParam));
					const auto* pitch =
					    reinterpret_cast<const Ngs2CustomVoicePitchShiftParam*>(param);
					module.pitch_shift_ratio = std::exp2(
					    static_cast<float>(std::clamp(pitch->cents, -1200, 1200)) / 1200.0f);
					module.pitch_shift_bypass_mask = pitch->bypass_mask;
					module.pitch_shift_changed     = true;
					break;
				}
				if (module_id == 0x19) {
					struct FxParam {
						Ngs2VoiceParamHeader     header;
						decltype(module.handler) handler;
						std::array<uintptr_t, 3> user_data;
					};
					EXIT_NOT_IMPLEMENTED(param->size != sizeof(FxParam));
					const auto& fx = *reinterpret_cast<const FxParam*>(param);
					// Enabling the handler requests a reset; later updates mark user data dirty.
					module.flags |= module.handler == nullptr ? 1u : 2u;
					module.handler   = fx.handler;
					module.user_data = fx.user_data;
					break;
				}
				struct Fx2Param {
					Ngs2VoiceParamHeader header;
					const void*          data;
					size_t               size;
				};
				const auto& fx = *reinterpret_cast<const Fx2Param*>(param);
				EXIT_NOT_IMPLEMENTED(voice->rack->fx[index].control != 0 ||
				                     fx.size != module.param.size());
				std::memcpy(module.param.data(), fx.data, fx.size);
				module.flags |= 2;
				break;
			}
			case 0x1000:
			case 0x4001: {
				EXIT_NOT_IMPLEMENTED(
				    voice->rack->type !=
				    (rack_id == 0x1000 ? Ngs2RackType::Sampler : Ngs2RackType::CustomSampler));
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
						const auto  result = voice->AddWaveformBlocks(blocks.data, blocks.flags,
						                                              blocks.count, blocks.blocks);
						if (result != OK) {
							return result;
						}
						break;
					}
					case 4: {
						for (auto& block: voice->blocks) {
							if (block.info.num_repeats != 0) {
								block.info.num_repeats = 0;
								break;
							}
						}
						break;
					}
					case 5: {
						const float ratio = *reinterpret_cast<const float*>(param + 1);
						EXIT_NOT_IMPLEMENTED(!std::isfinite(ratio));
						const float clamped_ratio = std::clamp(ratio, 0.0f, 4.0f);
						voice->sample_step        = static_cast<uint64_t>(
						    std::llround(static_cast<double>(voice->sample_rate) * clamped_ratio *
						                 4294967296.0));
						break;
					}
					case 0xa: {
						EXIT_NOT_IMPLEMENTED(voice->rack->type != Ngs2RackType::Sampler);
						EXIT_NOT_IMPLEMENTED(param->size != sizeof(Ngs2VoiceParamHeader) +
						                                        sizeof(Ngs2SamplerFilterParam));
						const auto& filter =
						    *reinterpret_cast<const Ngs2SamplerFilterParam*>(param + 1);
						EXIT_NOT_IMPLEMENTED(filter.index >=
						                     voice->rack->option.sampler.max_filters);
						if (voice->filters.size() <= filter.index) {
							voice->filters.resize(filter.index + 1);
						}
						voice->filters[filter.index].Configure(
						    filter, voice->rack->ngs->option.sample_rate, voice->channels);
						break;
					}
					default: EXIT("unsupported sampler control: 0x%08" PRIx32 "\n", param->id);
				}
				break;
			}
			case 0x4002: {
				EXIT_NOT_IMPLEMENTED(voice->rack->type != Ngs2RackType::CustomSubmixer ||
				                     (param->id & 0xffffu) != 0);
				const auto* channels = reinterpret_cast<const uint32_t*>(param + 1);
				EXIT_NOT_IMPLEMENTED(channels[0] != channels[1]);
				if (voice->channels != channels[0]) {
					voice->channels = channels[0];
					voice->ResetModuleFilters();
				}
				break;
			}
			case 0x4003: {
				// SceNgs2CustomMasteringVoiceSetupParam {numInputChannels, flags} or
				// SceNgs2CustomMasteringVoiceOutputParam {outputId, reserved}.
				EXIT_NOT_IMPLEMENTED(voice->rack->type != Ngs2RackType::CustomMastering ||
				                     param->size != sizeof(Ngs2VoiceParamHeader) + 8u);
				const auto* values = reinterpret_cast<const uint32_t*>(param + 1);
				switch (param->id & 0xffffu) {
					case 0:
						EXIT_NOT_IMPLEMENTED(values[0] == 0 ||
						                     values[0] >
						                         voice->rack->option.common.max_voice_channels ||
						                     values[1] != 0);
						if (voice->channels != values[0]) {
							voice->channels = values[0];
							voice->ResetModuleFilters();
						}
						break;
					case 1: voice->output_id = values[0]; break;
					default:
						EXIT("unsupported custom mastering control: 0x%08" PRIx32 "\n", param->id);
				}
				break;
			}
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

template <typename State>
static void Ngs2FillSamplerState(const Ngs2VoiceInternal& voice, State& state) {
	state.voice_state.state_flags = Ngs2GetStateFlags(&voice);
	state.num_decoded_samples     = voice.decoded_samples;
	state.decoded_data_size       = voice.decoded_bytes;
	state.waveform_data           = voice.WaveformData();
	state.user_data               = voice.blocks.empty() ? 0 : voice.blocks.front().info.user_data;
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
		case Ngs2RackType::CustomMastering:
		case Ngs2RackType::CustomSampler: {
			const bool sampler = voice->rack->type == Ngs2RackType::CustomSampler;
			const auto configured_size =
			    voice->rack->option.custom_sampler.custom_rack_option.state_size;
			EXIT_NOT_IMPLEMENTED(configured_size < (sampler
			                                            ? sizeof(Ngs2CustomSamplerVoiceState)
			                                            : sizeof(Ngs2CustomMasteringVoiceState)));
			if (state_size < configured_size) {
				return static_cast<int32_t>(0x804a8011u);
			}
			std::memset(state, 0, configured_size);
			if (sampler) {
				Ngs2FillSamplerState(*voice,
				                     *reinterpret_cast<Ngs2CustomSamplerVoiceState*>(state));
			} else {
				state->state_flags = Ngs2GetStateFlags(voice);
			}
			break;
		}
		case Ngs2RackType::Sampler: {
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

			auto* sampler            = reinterpret_cast<Ngs2SamplerVoiceState*>(state);
			sampler->envelope_height = 1.0f;
			sampler->peak_height     = 0.0f;
			sampler->reserved        = 0;
			Ngs2FillSamplerState(*voice, *sampler);
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

int KYTY_SYSV_ABI Ngs2VoiceQueryInfo(uintptr_t voice_handle, uint32_t query_id, void* info,
                                     size_t info_size) {
	PRINT_NAME();

	struct SetupInfo {
		uint32_t channels, flags;
	};
	EXIT_NOT_IMPLEMENTED(voice_handle == 0 || info == nullptr);
	EXIT_NOT_IMPLEMENTED(query_id != 0x4001 || info_size != sizeof(SetupInfo));

	auto* voice = reinterpret_cast<Ngs2VoiceInternal*>(voice_handle);

	Common::LockGuard lock(voice->rack->ngs->mutex);

	*static_cast<SetupInfo*>(info) = {voice->channels, 0};

	return OK;
}

int KYTY_SYSV_ABI Ngs2VoiceGetPortInfo(uintptr_t voice_handle, uint32_t port, void* info,
                                       size_t info_size) {
	PRINT_NAME();

	struct PortInfo {
		int32_t   matrix;
		float     volume;
		uint32_t  delay_samples, input;
		uintptr_t dest;
	};
	EXIT_NOT_IMPLEMENTED(voice_handle == 0 || info == nullptr || info_size != sizeof(PortInfo));

	auto* voice = reinterpret_cast<Ngs2VoiceInternal*>(voice_handle);

	Common::LockGuard lock(voice->rack->ngs->mutex);

	EXIT_NOT_IMPLEMENTED(port >= voice->ports.size());
	const auto& p = voice->ports[port];

	*static_cast<PortInfo*>(info) = {p.matrix, p.volume, 0, p.input,
	                                 reinterpret_cast<uintptr_t>(p.dest)};

	return OK;
}

int KYTY_SYSV_ABI Ngs2SystemLock(uintptr_t system_handle) {
	PRINT_NAME();

	auto* ngs = Ngs2FindSystem(system_handle);
	if (ngs == nullptr) {
		return NGS2_ERROR_INVALID_SYSTEM_HANDLE;
	}

	ngs->mutex.Lock();

	return OK;
}

int KYTY_SYSV_ABI Ngs2SystemUnlock(uintptr_t system_handle) {
	PRINT_NAME();

	auto* ngs = Ngs2FindSystem(system_handle);
	if (ngs == nullptr) {
		return NGS2_ERROR_INVALID_SYSTEM_HANDLE;
	}

	ngs->mutex.Unlock();

	return OK;
}

int KYTY_SYSV_ABI Ngs2SystemSetUserData(uintptr_t system_handle, uintptr_t user_data) {
	PRINT_NAME();
	auto* ngs = Ngs2FindSystem(system_handle);
	if (ngs == nullptr) {
		return NGS2_ERROR_INVALID_SYSTEM_HANDLE;
	}
	Common::LockGuard lock(ngs->mutex);
	ngs->user_data = user_data;
	return OK;
}

int KYTY_SYSV_ABI Ngs2SystemGetUserData(uintptr_t system_handle, uintptr_t* user_data) {
	PRINT_NAME();
	if (user_data == nullptr) {
		return NGS2_ERROR_INVALID_OUT_ADDRESS;
	}
	auto* ngs = Ngs2FindSystem(system_handle);
	if (ngs == nullptr) {
		return NGS2_ERROR_INVALID_SYSTEM_HANDLE;
	}
	Common::LockGuard lock(ngs->mutex);
	*user_data = ngs->user_data;
	return OK;
}

int KYTY_SYSV_ABI Ngs2SystemSetSampleRate(uintptr_t system_handle, uint32_t sample_rate) {
	PRINT_NAME();
	auto* ngs = Ngs2FindSystem(system_handle);
	if (ngs == nullptr) {
		return NGS2_ERROR_INVALID_SYSTEM_HANDLE;
	}
	if (!Ngs2IsValidSampleRate(sample_rate)) {
		return NGS2_ERROR_INVALID_SAMPLE_RATE;
	}
	Common::LockGuard lock(ngs->mutex);
	ngs->option.sample_rate = sample_rate;
	return OK;
}

struct Ngs2CommandRecord {
	uint32_t id;
	uint8_t  flags;
	uint8_t  type;
	uint16_t count;
	union {
		uint64_t    u64;
		uint32_t    u32;
		int32_t     i32;
		float       f;
		const void* ptr;
	} value;
};
static_assert(sizeof(Ngs2CommandRecord) == 16);

int KYTY_SYSV_ABI Ngs2SystemRunCommands(uintptr_t system_handle, const void* commands,
                                        size_t count) {
	PRINT_NAME();
	auto* ngs = Ngs2FindSystem(system_handle);
	if (ngs == nullptr) {
		return NGS2_ERROR_INVALID_SYSTEM_HANDLE;
	}
	if (count == 0) {
		return OK;
	}
	if (commands == nullptr) {
		return NGS2_ERROR_INVALID_OUT_ADDRESS;
	}
	const auto* records = static_cast<const Ngs2CommandRecord*>(commands);
	for (size_t i = 0; i < count; ++i) {
		const auto&    rec = records[i];
		const uint32_t cmd = rec.id & 0x00ffffffu;
		if (cmd == 0xf00000 || cmd == 0xf00001 || cmd == 0xf00002 || cmd == 0xf00006) {
			continue;
		}
		if (cmd == 0xf00003) {
			if (rec.id >= 0x20000000u) {
				return NGS2_ERROR_SYSTEM_INDEX_RANGE;
			}
		} else if (cmd == 0xf00004) {
			if (!Ngs2IsValidSampleRate(rec.value.u32)) {
				return NGS2_ERROR_INVALID_SAMPLE_RATE;
			}
		} else if (cmd == 0xf00005) {
			const uint32_t grain = rec.value.u32;
			if (grain < 64 || grain > ngs->option.max_grain_samples || (grain % 64) != 0) {
				return NGS2_ERROR_INVALID_NUM_GRAIN_SAMPLES;
			}
		} else {
			return NGS2_ERROR_INVALID_PARAM_ID;
		}
	}

	for (size_t i = 0; i < count; ++i) {
		const auto&    rec = records[i];
		const uint32_t cmd = rec.id & 0x00ffffffu;
		if (cmd == 0xf00000) {
			ngs->mutex.Lock();
		} else if (cmd == 0xf00001) {
			ngs->mutex.Unlock();
		} else if (cmd == 0xf00002) {
			Common::LockGuard lock(ngs->mutex);
			ngs->user_data = rec.value.u64;
		} else if (cmd == 0xf00004) {
			Common::LockGuard lock(ngs->mutex);
			ngs->option.sample_rate = rec.value.u32;
		} else if (cmd == 0xf00005) {
			Common::LockGuard lock(ngs->mutex);
			ngs->option.num_grain_samples = rec.value.u32;
		} else if (cmd == 0xf00006) {
			Common::LockGuard lock(ngs->mutex);
			ngs->unnamed_param = rec.value.f;
		} else if (cmd == 0xf00003) {
			const uint32_t slot = rec.id >> 24;
			if (slot < ngs->slot_data.size()) {
				Common::LockGuard lock(ngs->mutex);
				if (rec.value.ptr == nullptr) {
					ngs->slot_set[slot] = false;
					ngs->slot_data[slot].fill(0);
				} else {
					uint64_t leading_word = 0;
					std::memcpy(&leading_word, rec.value.ptr, sizeof(leading_word));
					if (leading_word == 0) {
						ngs->slot_set[slot] = false;
						ngs->slot_data[slot].fill(0);
					} else {
						ngs->slot_set[slot] = true;
						std::memcpy(ngs->slot_data[slot].data(), rec.value.ptr, 24);
					}
				}
			}
		}
	}
	return OK;
}

int KYTY_SYSV_ABI Ngs2SystemQueryInfo(uintptr_t system_handle, uint32_t info_id, void* info,
                                      size_t info_size) {
	PRINT_NAME();
	auto* ngs = Ngs2FindSystem(system_handle);
	if (ngs == nullptr) {
		return NGS2_ERROR_INVALID_SYSTEM_HANDLE;
	}
	switch (info_id & 0x00ffffffu) {
		case 0xf04000: {
			if (info == nullptr) return NGS2_ERROR_INVALID_OUT_ADDRESS;
			if (info_size < sizeof(Ngs2SystemInfo)) return NGS2_ERROR_INVALID_OUT_SIZE;
			return Ngs2SystemGetInfo(system_handle, static_cast<Ngs2SystemInfo*>(info), info_size);
		}
		case 0xf04001: {
			if (info == nullptr) return NGS2_ERROR_INVALID_OUT_ADDRESS;
			if (info_size < sizeof(uint64_t)) return NGS2_ERROR_INVALID_OUT_SIZE;
			Common::LockGuard lock(ngs->mutex);
			std::memcpy(info, &ngs->user_data, sizeof(uint64_t));
			return OK;
		}
		case 0xf04002: {
			if (info_id >= 0x20000000u) return NGS2_ERROR_SYSTEM_INDEX_RANGE;
			if (info == nullptr) return NGS2_ERROR_INVALID_OUT_ADDRESS;
			if (info_size < 24) return NGS2_ERROR_INVALID_OUT_SIZE;
			const uint32_t          slot = info_id >> 24;
			std::array<uint8_t, 24> out_data {};
			{
				Common::LockGuard lock(ngs->mutex);
				if (slot < ngs->slot_data.size() && ngs->slot_set[slot]) {
					std::memcpy(out_data.data(), ngs->slot_data[slot].data(), 24);
				}
			}
			std::memcpy(info, out_data.data(), 24);
			return OK;
		}
		case 0xf04003: {
			if (info == nullptr && info_size >= sizeof(uintptr_t))
				return NGS2_ERROR_INVALID_OUT_ADDRESS;
			uint32_t count = 0;
			{
				Common::LockGuard lock(g_racks_mutex);
				for (auto* r = g_racks_list; r != nullptr; r = r->next) {
					if (r->ngs == ngs) {
						if (info != nullptr && count < (info_size / sizeof(uintptr_t))) {
							reinterpret_cast<uintptr_t*>(info)[count] =
							    reinterpret_cast<uintptr_t>(r);
						}
						++count;
					}
				}
			}
			return static_cast<int>(count);
		}
		default: return NGS2_ERROR_INVALID_SYSTEM_QUERY_ID;
	}
}

int KYTY_SYSV_ABI Ngs2SystemEnumHandles(uintptr_t* out_handles, uint32_t max_handles) {
	PRINT_NAME();
	constexpr int32_t invalid_argument = static_cast<int32_t>(0x804a0053u);
	if (max_handles != 0 && out_handles == nullptr) return invalid_argument;
	uint32_t          count = 0;
	Common::LockGuard lock(g_ngs_mutex);
	for (auto* ngs = g_ngs_list; ngs != nullptr; ngs = ngs->next) {
		if (out_handles != nullptr && count < max_handles) {
			out_handles[count] = reinterpret_cast<uintptr_t>(ngs);
		}
		++count;
	}
	return static_cast<int>(count);
}

int KYTY_SYSV_ABI Ngs2SystemEnumRackHandles(uintptr_t system_handle, uintptr_t* out_handles,
                                            uint32_t max_handles) {
	PRINT_NAME();
	constexpr int32_t invalid_argument      = static_cast<int32_t>(0x804a0053u);
	constexpr int32_t invalid_system_handle = static_cast<int32_t>(0x804a0230u);
	if (max_handles != 0 && out_handles == nullptr) return invalid_argument;
	auto* ngs = Ngs2FindSystem(system_handle);
	if (ngs == nullptr) return invalid_system_handle;
	uint32_t          count = 0;
	Common::LockGuard lock(g_racks_mutex);
	for (auto* rack = g_racks_list; rack != nullptr; rack = rack->next) {
		if (rack->ngs == ngs) {
			if (out_handles != nullptr && count < max_handles) {
				out_handles[count] = reinterpret_cast<uintptr_t>(rack);
			}
			++count;
		}
	}
	return static_cast<int>(count);
}

int KYTY_SYSV_ABI Ngs2RackSetUserData(uintptr_t rack_handle, uintptr_t user_data) {
	PRINT_NAME();
	Common::LockGuard racks_lock(g_racks_mutex);
	auto*           rack = Ngs2FindRackLocked(rack_handle);
	if (rack == nullptr) {
		return NGS2_ERROR_INVALID_RACK_HANDLE;
	}
	rack->user_data = user_data;
	return OK;
}

int KYTY_SYSV_ABI Ngs2RackGetUserData(uintptr_t rack_handle, uintptr_t* user_data) {
	PRINT_NAME();
	if (user_data == nullptr) {
		return NGS2_ERROR_INVALID_OUT_ADDRESS;
	}
	Common::LockGuard racks_lock(g_racks_mutex);
	auto*           rack = Ngs2FindRackLocked(rack_handle);
	if (rack == nullptr) {
		return NGS2_ERROR_INVALID_RACK_HANDLE;
	}
	*user_data = rack->user_data;
	return OK;
}

int KYTY_SYSV_ABI Ngs2RackRunCommands(uintptr_t rack_handle, const void* commands, size_t count) {
	PRINT_NAME();
	if (Ngs2FindRack(rack_handle) == nullptr) {
		return NGS2_ERROR_INVALID_RACK_HANDLE;
	}
	if (count == 0) {
		return OK;
	}
	if (commands == nullptr) {
		return NGS2_ERROR_INVALID_OUT_ADDRESS;
	}
	const auto* records = static_cast<const Ngs2CommandRecord*>(commands);
	for (size_t i = 0; i < count; ++i) {
		const uint32_t cmd = records[i].id & 0x00ffffffu;
		if (cmd < 0xf10000 || cmd > 0xf10002) {
			return NGS2_ERROR_INVALID_PARAM_ID;
		}
	}
	for (size_t i = 0; i < count; ++i) {
		const auto&    rec = records[i];
		const uint32_t cmd = rec.id & 0x00ffffffu;
		if (cmd == 0xf10000) {
			int lock_res = Ngs2RackLock(rack_handle);
			if (lock_res != OK) return lock_res;
		} else if (cmd == 0xf10001) {
			int unlock_res = Ngs2RackUnlock(rack_handle);
			if (unlock_res != OK) return unlock_res;
		} else if (cmd == 0xf10002) {
			Common::LockGuard racks_lock(g_racks_mutex);
			auto*           target = Ngs2FindRackLocked(rack_handle);
			if (target == nullptr) {
				return NGS2_ERROR_INVALID_RACK_HANDLE;
			}
			target->user_data = rec.value.u64;
		}
	}
	return OK;
}

int KYTY_SYSV_ABI Ngs2RackGetInfo(uintptr_t rack_handle, Ngs2RackInfo* info, size_t info_size) {
	PRINT_NAME();
	if (info == nullptr) {
		return NGS2_ERROR_INVALID_OUT_ADDRESS;
	}
	if (info_size < sizeof(Ngs2RackInfo)) {
		return NGS2_ERROR_INVALID_OUT_SIZE;
	}
	Ngs2Internal* ngs = nullptr;
	{
		Common::LockGuard racks_lock(g_racks_mutex);
		auto*           rack = Ngs2FindRackLocked(rack_handle);
		if (rack == nullptr || rack->ngs == nullptr) {
			return NGS2_ERROR_INVALID_RACK_HANDLE;
		}
		ngs = rack->ngs;
	}
	Common::LockGuard lock(ngs->mutex);
	Common::LockGuard racks_lock(g_racks_mutex);
	auto*           rack = Ngs2FindRackLocked(rack_handle);
	if (rack == nullptr || rack->ngs != ngs) {
		return NGS2_ERROR_INVALID_RACK_HANDLE;
	}

	*info = {};
	std::memcpy(info->name, rack->name, sizeof(info->name));
	info->rack_handle         = rack_handle;
	info->buffer_info         = rack->buffer_info;
	info->owner_system_handle = reinterpret_cast<uintptr_t>(rack->ngs);
	info->type =
	    (rack->type == Ngs2RackType::Sampler || rack->type == Ngs2RackType::CustomSampler) ? 0
	    : (rack->type == Ngs2RackType::Mastering || rack->type == Ngs2RackType::CustomMastering)
	        ? 2
	        : 1;
	info->rack_id           = rack->rack_id;
	info->uid               = rack->uid;
	info->min_grain_samples = 64;
	info->max_grain_samples = rack->option.common.max_grain_samples;
	info->max_voices        = rack->option.common.max_voices;
	info->max_channel_works = (rack->type == Ngs2RackType::Sampler)
	                              ? rack->option.sampler.max_channel_works
	                          : (rack->type == Ngs2RackType::CustomSampler)
	                              ? rack->option.custom_sampler.max_channel_works
	                              : 0;
	info->max_inputs =
	    (rack->type == Ngs2RackType::Submixer)          ? rack->option.submixer.max_inputs
	    : (rack->type == Ngs2RackType::CustomSubmixer)  ? rack->option.custom_submixer.max_inputs
	    : (rack->type == Ngs2RackType::CustomMastering) ? rack->option.custom_mastering.max_inputs
	                                                    : 0;
	info->max_matrices        = rack->option.common.max_matrices;
	info->max_ports           = rack->option.common.max_ports;
	info->max_voice_channels  = rack->option.common.max_voice_channels;
	info->max_output_channels = rack->option.common.max_output_channels;
	info->num_modules         = Ngs2RackIsCustom(rack->type)
	                                ? rack->option.custom_sampler.custom_rack_option.num_modules
	                                : 0;
	info->num_buffers         = rack->NumBuffers();
	info->state_flags         = (rack->lock_depth.load(std::memory_order_relaxed) == 0) ? 1 : 5;
	info->last_process_ratio  = 0.0f;
	info->last_processed_tick = 0;
	info->render_count        = rack->render_count;
	info->active_voice_count  = 0;
	info->active_channel_work_count = 0;

	auto* voices = reinterpret_cast<Ngs2VoiceInternal*>(rack + 1);
	for (uint32_t i = 0; i < rack->option.common.max_voices; ++i) {
		if (voices[i].state == Ngs2VoicePlayState::Playing ||
		    voices[i].state == Ngs2VoicePlayState::Paused) {
			++info->active_voice_count;
			info->active_channel_work_count += voices[i].channels;
		}
	}
	return OK;
}

int KYTY_SYSV_ABI Ngs2RackQueryInfo(uintptr_t rack_handle, uint32_t info_id, void* info,
                                    size_t info_size) {
	PRINT_NAME();
	auto* rack = Ngs2FindRack(rack_handle);
	if (rack == nullptr) {
		return NGS2_ERROR_INVALID_RACK_HANDLE;
	}
	switch (info_id & 0x00ffffffu) {
		case 0xf14000: {
			if (info == nullptr) return NGS2_ERROR_INVALID_OUT_ADDRESS;
			if (info_size < sizeof(Ngs2RackInfo)) return NGS2_ERROR_INVALID_OUT_SIZE;
			return Ngs2RackGetInfo(rack_handle, static_cast<Ngs2RackInfo*>(info), info_size);
		}
		case 0xf14001: {
			if (info_size < sizeof(uint64_t)) return NGS2_ERROR_INVALID_OUT_SIZE;
			if (info == nullptr) return NGS2_ERROR_INVALID_OUT_ADDRESS;
			Common::LockGuard racks_lock(g_racks_mutex);
			auto*           target = Ngs2FindRackLocked(rack_handle);
			if (target == nullptr) {
				return NGS2_ERROR_INVALID_RACK_HANDLE;
			}
			std::memcpy(info, &target->user_data, sizeof(uint64_t));
			return OK;
		}
		case 0xf14002: {
			if (info_size < sizeof(Ngs2CustomModuleInfo)) return NGS2_ERROR_INVALID_OUT_SIZE;
			if (info == nullptr) return NGS2_ERROR_INVALID_OUT_ADDRESS;
			const uint32_t module_idx = info_id >> 24;
			Common::LockGuard racks_lock(g_racks_mutex);
			auto*           target = Ngs2FindRackLocked(rack_handle);
			if (target == nullptr) {
				return NGS2_ERROR_INVALID_RACK_HANDLE;
			}
			if (!Ngs2RackIsCustom(target->type) ||
			    module_idx >= target->option.custom_sampler.custom_rack_option.num_modules) {
				return NGS2_ERROR_INVALID_RACK_MODULE_INDEX;
			}
			const auto& mod = target->option.custom_sampler.custom_rack_option.module[module_idx];
			auto*       out = static_cast<Ngs2CustomModuleInfo*>(info);
			out->module_id  = mod.module_id;
			out->source_buffer_id = mod.source_buffer_id;
			out->extra_buffer_id  = mod.extra_buffer_id;
			out->dest_buffer_id   = mod.dest_buffer_id;
			out->state_offset     = mod.state_offset;
			out->state_size       = mod.state_size;
			out->reserved         = 0;
			out->reserved2        = 0;
			return OK;
		}
		default: return NGS2_ERROR_INVALID_RACK_QUERY_ID;
	}
}

int KYTY_SYSV_ABI Ngs2CustomRackGetModuleInfo(uintptr_t rack_handle, uint32_t module_index,
                                              Ngs2CustomModuleInfo* info, size_t info_size) {
	PRINT_NAME();
	if (info == nullptr) return NGS2_ERROR_INVALID_OUT_ADDRESS;
	if (info_size < sizeof(Ngs2CustomModuleInfo)) return NGS2_ERROR_INVALID_OUT_SIZE;
	Common::LockGuard racks_lock(g_racks_mutex);
	auto*           rack = Ngs2FindRackLocked(rack_handle);
	if (rack == nullptr) return NGS2_ERROR_INVALID_RACK_HANDLE;
	if (!Ngs2RackIsCustom(rack->type) ||
	    module_index >= rack->option.custom_sampler.custom_rack_option.num_modules) {
		return NGS2_ERROR_INVALID_RACK_MODULE_INDEX;
	}
	const auto& mod        = rack->option.custom_sampler.custom_rack_option.module[module_index];
	info->module_id        = mod.module_id;
	info->source_buffer_id = mod.source_buffer_id;
	info->extra_buffer_id  = mod.extra_buffer_id;
	info->dest_buffer_id   = mod.dest_buffer_id;
	info->state_offset     = mod.state_offset;
	info->state_size       = mod.state_size;
	info->reserved         = 0;
	info->reserved2        = 0;
	return OK;
}

int KYTY_SYSV_ABI Ngs2VoiceGetOwner(uintptr_t voice_handle, uintptr_t* owner_rack,
                                    uint32_t* voice_index) {
	PRINT_NAME();
	if (voice_handle == 0) return NGS2_ERROR_INVALID_VOICE_HANDLE;
	if (owner_rack == nullptr || voice_index == nullptr) return NGS2_ERROR_INVALID_OUT_ADDRESS;

	auto* voice = Ngs2VoiceFromHandle(voice_handle);
	if (voice == nullptr || voice->rack == nullptr) return NGS2_ERROR_INVALID_VOICE_HANDLE;

	auto* rack              = voice->rack;
	*owner_rack             = reinterpret_cast<uintptr_t>(rack);
	const auto* voices_base = reinterpret_cast<const Ngs2VoiceInternal*>(rack + 1);
	*voice_index            = static_cast<uint32_t>(voice - voices_base);
	return OK;
}

int KYTY_SYSV_ABI Ngs2VoiceGetMatrixInfo(uintptr_t voice_handle, uint32_t matrix_id,
                                         Ngs2VoiceMatrixInfo* info, size_t info_size) {
	PRINT_NAME();
	if (info == nullptr) return NGS2_ERROR_INVALID_OUT_ADDRESS;
	if (info_size < sizeof(Ngs2VoiceMatrixInfo)) return NGS2_ERROR_INVALID_OUT_SIZE;
	if (voice_handle == 0) return NGS2_ERROR_INVALID_VOICE_HANDLE;

	auto* voice = Ngs2VoiceFromHandle(voice_handle);
	if (voice == nullptr) return NGS2_ERROR_INVALID_VOICE_HANDLE;

	Common::LockGuard lock(voice->rack->ngs->mutex);
	if (matrix_id >= voice->matrices.size()) return NGS2_ERROR_INVALID_PARAM_ID;

	const auto& m = voice->matrices[matrix_id];
	*info         = {};
	if (!m.empty()) {
		const auto count = std::min(m.size(), size_t(37 * 37));
		info->num_levels = static_cast<uint32_t>(count);
		std::memcpy(info->levels, m.data(), count * sizeof(float));
	}
	return OK;
}

// DSP FFT & Waveform Frame Info / ParseWaveformUser

// Size Chart FFT: length 8..12 -> samples 64, 128, 256, 512, 1024
static constexpr uint32_t kFftSamples[5] = {64u, 128u, 256u, 512u, 1024u};

static bool Ngs2FftLengthValid(int length) {
	return length >= 8 && length <= 12;
}

// Radix-2 Cooley-Tukey in-place FFT on complex data (double).
// inverse=false -> forward transform (negative sign and exponent),
// inverse=true  -> inverse transform (positive sign, unnormalized).
static void Ngs2Radix2Fft(std::vector<std::complex<double>>& data, bool inverse) {
	const size_t n = data.size();
	// Bit-reversal permutation
	for (size_t i = 1, j = 0; i < n; ++i) {
		size_t bit = n >> 1u;
		for (; j & bit; bit >>= 1u) {
			j ^= bit;
		}
		j ^= bit;
		if (i < j) {
			std::swap(data[i], data[j]);
		}
	}
	// Butterfly stages
	const double sign = inverse ? 1.0 : -1.0;
	for (size_t len = 2; len <= n; len <<= 1u) {
		const double angle = sign * (2.0 * std::numbers::pi) / static_cast<double>(len);
		const std::complex<double> step(std::cos(angle), std::sin(angle));
		for (size_t i = 0; i < n; i += len) {
			std::complex<double> w(1.0, 0.0);
			const size_t         half = len >> 1u;
			for (size_t j2 = 0; j2 < half; ++j2) {
				const std::complex<double> u = data[i + j2];
				const std::complex<double> v = data[i + j2 + half] * w;
				data[i + j2]                 = u + v;
				data[i + j2 + half]          = u - v;
				w *= step;
			}
		}
	}
}

// Calculate the index in the “internal order” buffer used by sceNgs2FftProcess.
static size_t Ngs2FftInternalBinIndex(size_t q, size_t r, size_t j, size_t quarterBins) {
	switch (r) {
		case 0: return 4 * q + j;
		case 1: return (q == 0 && j == 0) ? quarterBins : (2 * quarterBins - 4 * q - j);
		case 2: return 2 * quarterBins + 4 * q + j;
		default: return (q == 0 && j == 0) ? 3 * quarterBins : (4 * quarterBins - 4 * q - j);
	}
}

// Serialize bins in “internal order” (flags=2, mode bit2=1).
static void Ngs2FftPackInternal(const std::vector<std::pair<float, float>>& bins, size_t points,
                                std::vector<float>& out) {
	const size_t quarterBins = points / 8u;
	const size_t quadGroups  = quarterBins / 4u;
	out.assign(points, 0.0f);
	for (size_t q = 0; q < quadGroups; ++q) {
		for (size_t r = 0; r < 4; ++r) {
			const size_t base = (4 * q + r) * 8u;
			for (size_t j = 0; j < 4; ++j) {
				const size_t bin  = Ngs2FftInternalBinIndex(q, r, j, quarterBins);
				out[base + j]     = bins[bin].first;
				out[base + 4 + j] = bins[bin].second;
			}
		}
	}
}

// Deserialize bins in “internal order.”
static void Ngs2FftUnpackInternal(const std::vector<float>& in, size_t points,
                                  std::vector<std::pair<float, float>>& bins) {
	const size_t quarterBins = points / 8u;
	const size_t quadGroups  = quarterBins / 4u;
	bins.assign(points / 2u, {0.0f, 0.0f});
	for (size_t q = 0; q < quadGroups; ++q) {
		for (size_t r = 0; r < 4; ++r) {
			const size_t base = (4 * q + r) * 8u;
			for (size_t j = 0; j < 4; ++j) {
				const size_t bin = Ngs2FftInternalBinIndex(q, r, j, quarterBins);
				bins[bin]        = {in[base + j], in[base + 4 + j]};
			}
		}
	}
}

// Serialize bins in “natural order” (flags=2, mode bit2=0).
static void Ngs2FftPackNatural(const std::vector<std::pair<float, float>>& bins, size_t points,
                               std::vector<float>& out) {
	const size_t quarterBins = points / 8u;
	const size_t blockStride = points / 2u;
	out.assign(points * 2u, 0.0f);
	for (size_t q = 0; q < 4; ++q) {
		const size_t binStart  = q * quarterBins;
		const size_t blockBase = q * blockStride;
		for (size_t r = 0; r < quarterBins; ++r) {
			const size_t dstOffset             = ((q & 1u) != 0 && r > 0) ? (quarterBins - r) : r;
			const auto&  bin                   = bins[binStart + r];
			out[blockBase + dstOffset * 2]     = bin.first;
			out[blockBase + dstOffset * 2 + 1] = bin.second;
		}
	}
}

// Deserialize bins in “natural order”: the exact inverse of
// Ngs2FftPackNatural, so a forward/inverse pair round-trips.
static void Ngs2FftUnpackNatural(const std::vector<float>& in, size_t points,
                                 std::vector<std::pair<float, float>>& bins) {
	const size_t quarterBins = points / 8u;
	const size_t blockStride = points / 2u;
	bins.assign(points / 2u, {0.0f, 0.0f});
	for (size_t q = 0; q < 4; ++q) {
		const size_t blockBase = q * blockStride;
		for (size_t d = 0; d < quarterBins; ++d) {
			const size_t r            = ((q & 1u) != 0 && d > 0) ? (quarterBins - d) : d;
			bins[q * quarterBins + r] = {in[blockBase + d * 2], in[blockBase + d * 2 + 1]};
		}
	}
}

// ---- Public FFT APIs ----

int KYTY_SYSV_ABI Ngs2FftQuerySize(uint32_t length, uint32_t flags, uint64_t* out_size) {
	PRINT_NAME();
	if (out_size == nullptr || length < 8 || length > 12) {
		return -1;
	}
	uint32_t count = kFftSamples[length - 8];
	if ((flags & 2u) != 0) {
		count /= 2u;
	}
	// Formula: count * 32 + 64 (each 32-byte entry = 2 float pairs × 2 channels × 4 bytes)
	*out_size = static_cast<uint64_t>(count) * 32u + 64u;
	return OK;
}

int KYTY_SYSV_ABI Ngs2FftInit(void* work, size_t size, int length, uint32_t flags,
                              void** out_handle) {
	PRINT_NAME();
	if (!Ngs2FftLengthValid(length) || work == nullptr || out_handle == nullptr) {
		return -1;
	}
	uint32_t count = kFftSamples[static_cast<size_t>(length - 8)];
	if ((flags & 2u) != 0) {
		count /= 2u;
	}
	const size_t required = static_cast<size_t>(count) * 32u + 64u;
	if (size < required) {
		return -1;
	}

	auto* bytes = static_cast<uint8_t*>(work);
	std::memset(bytes, 0, required);

	// 64-byte table alignment
	const uintptr_t base      = reinterpret_cast<uintptr_t>(work);
	const uintptr_t tableAddr = (base + 0x57u) & ~uintptr_t {0x3fu};
	const size_t    tableOff  = static_cast<size_t>(tableAddr - base);
	const uintptr_t secondAddr =
	    tableAddr + (((flags & 2u) != 0) ? static_cast<size_t>(count) * 16u : 0u);

	// Header with an offset of 0x00..0x17
	std::memcpy(bytes + 0x00, &tableAddr, sizeof(uint64_t));
	std::memcpy(bytes + 0x08, &secondAddr, sizeof(uint64_t));
	const int32_t len32 = static_cast<int32_t>(length);
	std::memcpy(bytes + 0x10, &len32, sizeof(int32_t));
	std::memcpy(bytes + 0x14, &flags, sizeof(uint32_t));

	// Pre-calculated sine/cosine table
	if (tableOff < required) {
		const size_t points    = size_t {1} << length;
		const size_t maxFloats = (required - tableOff) / sizeof(float);
		for (size_t idx = 0; idx + 1 < maxFloats; idx += 2) {
			const size_t k = idx / 2u;
			const double angle =
			    (-2.0 * std::numbers::pi * static_cast<double>(k)) / static_cast<double>(points);
			const float c = static_cast<float>(std::cos(angle));
			const float s = static_cast<float>(std::sin(angle));
			std::memcpy(bytes + tableOff + idx * sizeof(float), &c, sizeof(float));
			std::memcpy(bytes + tableOff + (idx + 1) * sizeof(float), &s, sizeof(float));
		}
	}

	*out_handle = work;
	return OK;
}

int KYTY_SYSV_ABI Ngs2FftProcess(void* handle, const float* in, float* out, float* work_buf,
                                 uintptr_t unused, uint32_t mode) {
	(void)unused;
	PRINT_NAME();
	if (handle == nullptr || in == nullptr || out == nullptr) {
		return -1;
	}

	const auto* hdr    = static_cast<const uint8_t*>(handle);
	int32_t     length = 0;
	uint32_t    flags  = 0;
	std::memcpy(&length, hdr + 0x10, sizeof(length));
	std::memcpy(&flags, hdr + 0x14, sizeof(flags));
	if (!Ngs2FftLengthValid(length)) {
		return -1;
	}

	const size_t points = size_t {1} << length;

	// flags=0: “pass-through” mode — copies work_buf to out (or to in if work_buf==nullptr)
	if ((flags & 2u) == 0) {
		const size_t totalFloats = points * 2u;
		if (work_buf != nullptr) {
			std::memcpy(out, work_buf, totalFloats * sizeof(float));
		} else {
			std::memcpy(out, in, totalFloats * sizeof(float));
		}
		return OK;
	}

	// flags=2: trigonometric table mode; supports direct and inverse FFT.
	const float scale = 1.0f / static_cast<float>(points);

	if ((mode & 1u) != 0) {
		// ---- Forward FFT: real -> compressed spectrum ----
		std::vector<float> input(points);
		std::memcpy(input.data(), in, points * sizeof(float));

		std::vector<std::complex<double>> spectrum(points);
		for (size_t n2 = 0; n2 < points; ++n2) {
			spectrum[n2] = {static_cast<double>(input[n2]), 0.0};
		}
		Ngs2Radix2Fft(spectrum, false);

		const size_t                         half = points / 2u;
		std::vector<std::pair<float, float>> bins(half);
		bins[0] = {static_cast<float>(spectrum[0].real()),
		           static_cast<float>(spectrum[half].real())};
		for (size_t k = 1; k < half; ++k) {
			bins[k] = {static_cast<float>(spectrum[k].real()),
			           static_cast<float>(spectrum[k].imag())};
		}

		std::vector<float> output;
		if ((mode & 4u) != 0) {
			// internal order
			Ngs2FftPackInternal(bins, points, output);
			if ((mode & 8u) != 0) {
				for (float& v: output) {
					v *= scale;
				}
			}
		} else {
			// natural order
			Ngs2FftPackNatural(bins, points, output);
			if ((mode & 8u) != 0) {
				for (float& v: output) {
					v *= scale;
				}
			}
		}
		std::memcpy(out, output.data(), output.size() * sizeof(float));
		return OK;
	}

	// ---- Inverse FFT: compressed spectrum -> real ----
	// Internal order packs `points` floats; natural order packs `points * 2`
	// (four quarter-spectrum blocks of complex pairs).
	const bool         internal_order = (mode & 4u) != 0;
	std::vector<float> input(points * (internal_order ? 1u : 2u));
	std::memcpy(input.data(), in, input.size() * sizeof(float));

	std::vector<std::pair<float, float>> bins;
	if (internal_order) {
		Ngs2FftUnpackInternal(input, points, bins);
	} else {
		Ngs2FftUnpackNatural(input, points, bins);
	}

	const size_t                      half = points / 2u;
	std::vector<std::complex<double>> spectrum(points);
	spectrum[0]    = {static_cast<double>(bins[0].first), 0.0};
	spectrum[half] = {static_cast<double>(bins[0].second), 0.0};
	for (size_t k = 1; k < half; ++k) {
		const std::complex<double> v(static_cast<double>(bins[k].first),
		                             static_cast<double>(bins[k].second));
		spectrum[k]          = v;
		spectrum[points - k] = std::conj(v);
	}
	Ngs2Radix2Fft(spectrum, true);

	std::vector<float> output(points);
	for (size_t n2 = 0; n2 < points; ++n2) {
		float v = static_cast<float>(spectrum[n2].real());
		if ((mode & 8u) != 0) {
			v *= scale;
		}
		output[n2] = v;
	}
	std::memcpy(out, output.data(), points * sizeof(float));
	return OK;
}

// Ngs2GetWaveformFrameInfo Retrieves frame size, num_frame_samples, units_per_frame,
// and num_delay_samples from the waveform format (PCM or ATRAC9).

int KYTY_SYSV_ABI Ngs2GetWaveformFrameInfo(const Ngs2WaveformFormat* format,
                                           uint32_t*                 out_frame_size,
                                           uint32_t*                 out_num_frame_samples,
                                           uint32_t*                 out_units_per_frame,
                                           uint32_t*                 out_num_delay_samples) {
	PRINT_NAME();

	// All output pointers must be valid (AnyPS5: SCE_NGS2_ERROR_INVALID_OUT_ADDRESS)
	if (out_frame_size == nullptr || out_num_frame_samples == nullptr ||
	    out_units_per_frame == nullptr || out_num_delay_samples == nullptr) {
		return NGS2_ERROR_INVALID_OUT_ADDRESS;
	}
	// If the format itself is null or invalid; ATRAC9 channel counts are
	// validated against the codec below.
	if (format == nullptr || format->num_channels == 0 || format->sample_rate == 0 ||
	    format->sample_rate > 192000 ||
	    (format->waveform_type != NGS2_WAVEFORM_TYPE_ATRAC9 && format->num_channels > 8)) {
		return NGS2_ERROR_INVALID_WAVEFORM_FORMAT;
	}

	uint32_t frameSize     = 0;
	uint32_t frameSamples  = 0;
	uint32_t unitsPerFrame = 0;
	uint32_t delaySamples  = 0;

	if (format->waveform_type == NGS2_WAVEFORM_TYPE_ATRAC9) {
		// ATRAC9: Extracting information from the codec
		std::array<uint8_t, ATRAC9_CONFIG_DATA_SIZE> config {
		    static_cast<uint8_t>(format->config_data >> 24u),
		    static_cast<uint8_t>(format->config_data >> 16u),
		    static_cast<uint8_t>(format->config_data >> 8u),
		    static_cast<uint8_t>(format->config_data)};
		Atrac9CodecInfo codec {};
		if (!Ngs2GetAtrac9CodecInfo(config, codec) ||
		    codec.channels != static_cast<int>(format->num_channels) ||
		    codec.samplingRate != static_cast<int>(format->sample_rate)) {
			return NGS2_ERROR_INVALID_WAVEFORM_FORMAT;
		}
		frameSize     = static_cast<uint32_t>(codec.superframeSize);
		frameSamples  = static_cast<uint32_t>(static_cast<uint64_t>(codec.frameSamples) *
		                                      codec.framesInSuperframe);
		unitsPerFrame = static_cast<uint32_t>(codec.framesInSuperframe);
		delaySamples  = static_cast<uint32_t>(codec.frameSamples);
	} else if (format->waveform_type == SCE_NGS2_WAVEFORM_TYPE_VAG) {
		frameSize     = format->num_channels * 16u;
		frameSamples  = 28;
		unitsPerFrame = 1;
		delaySamples  = 0;
	} else {
		// PCM (all linear types): frame = one sample for all channels
		uint32_t bytesPerSample = 0;
		switch (format->waveform_type) {
			case 0x11u: bytesPerSample = 1; break; // 8-bit PCM
			case 0x12u: bytesPerSample = 2; break; // 16-bit PCM (I16L)
			case 0x14u: bytesPerSample = 3; break; // 24-bit PCM
			case 0x16u: bytesPerSample = 4; break; // 32-bit PCM int
			case 0x18u: bytesPerSample = 4; break; // 32-bit float
			case 0x1au: bytesPerSample = 8; break; // 64-bit double
			default: return NGS2_ERROR_UNKNOWN_WAVEFORM_FORMAT;
		}
		frameSize     = format->num_channels * bytesPerSample;
		frameSamples  = 1;
		unitsPerFrame = 1;
		delaySamples  = 0;
	}

	*out_frame_size        = frameSize;
	*out_num_frame_samples = frameSamples;
	*out_units_per_frame   = unitsPerFrame;
	*out_num_delay_samples = delaySamples;
	return OK;
}

// Ngs2ParseWaveformUser Calls a handler (PS5 callback) to read RIFF/WAVE data
// and extract Ngs2WaveformInfo (delegates to Ngs2ParseWaveformData).
// Callback signature: int32_t handler(uintptr_t user_data, uint32_t offset, void* buf, size_t size)

// Type for the reading callback
using Ngs2ParseReadHandler = int32_t(KYTY_SYSV_ABI*)(uintptr_t user_data, uint32_t offset,
                                                     void* data, size_t size);

int KYTY_SYSV_ABI Ngs2ParseWaveformUser(uintptr_t handler, uintptr_t user_data,
                                        Ngs2WaveformInfo* out_info) {
	PRINT_NAME();

	if (out_info == nullptr) {
		return NGS2_ERROR_INVALID_OUT_ADDRESS;
	}
	if (handler == 0) {
		return NGS2_ERROR_INVALID_SYSTEM_HANDLE; // Null handler = invalid handle
	}

	const auto read = reinterpret_cast<Ngs2ParseReadHandler>(handler);

	// Read the first bytes of the header
	uint8_t header[48] = {};
	if (read(user_data, 0, header, 12) < 0) {
		return NGS2_ERROR_INVALID_SYSTEM_HANDLE;
	}
	if (std::memcmp(header, "VAGp", 4) == 0) {
		if (read(user_data, 12, header + 12, 36) < 0) {
			return NGS2_ERROR_INVALID_SYSTEM_HANDLE;
		}
		const uint32_t     vag_size          = (static_cast<uint32_t>(header[0x0c]) << 24u) |
		                                       (static_cast<uint32_t>(header[0x0d]) << 16u) |
		                                       (static_cast<uint32_t>(header[0x0e]) << 8u) |
		                                       static_cast<uint32_t>(header[0x0f]);
		constexpr uint64_t kMaxWaveformBytes = 256ull * 1024 * 1024;
		if (vag_size > kMaxWaveformBytes) {
			return NGS2_ERROR_INVALID_WAVEFORM_DATA;
		}
		const size_t         total = 48 + vag_size;
		std::vector<uint8_t> bytes(total);
		std::memcpy(bytes.data(), header, 48);
		if (vag_size > 0 && read(user_data, 48, bytes.data() + 48, vag_size) < 0) {
			return NGS2_ERROR_INVALID_SYSTEM_HANDLE;
		}
		return Ngs2ParseWaveformData(bytes.data(), total, out_info);
	}
	if (!Ngs2FourCcEquals(header, "RIFF") || !Ngs2FourCcEquals(header + 8, "WAVE")) {
		return NGS2_ERROR_UNKNOWN_WAVEFORM_FORMAT;
	}

	const uint32_t     riff_size         = Ngs2ReadLe32(header + 4);
	const uint64_t     riff_end          = 8ull + riff_size;
	constexpr uint64_t kMaxWaveformBytes = 256ull * 1024 * 1024; // 256 MiB
	if (riff_end < 12 || riff_end > kMaxWaveformBytes) {
		return NGS2_ERROR_INVALID_WAVEFORM_DATA;
	}

	// Read the entire RIFF file into a local buffer
	const size_t         total = static_cast<size_t>(riff_end);
	std::vector<uint8_t> bytes(total);
	if (read(user_data, 0, bytes.data(), total) < 0) {
		return NGS2_ERROR_INVALID_SYSTEM_HANDLE;
	}

	// Delegate to the existing RIFF parser
	return Ngs2ParseWaveformData(bytes.data(), total, out_info);
}

// Stream API & Scheduler / Modules
static inline bool IsValidPointer(const void* ptr, size_t size = 1) {
	if (ptr == nullptr) return false;
	const uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);
	if (addr < 0x10000) return false;
	if (addr + size < addr) return false;
	return true;
}

static inline bool ReadGuestBytes(void* local, const void* guest, size_t bytes) {
	if (bytes == 0) return true;
	if (!IsValidPointer(guest, bytes) || local == nullptr) return false;
	std::memcpy(local, guest, bytes);
	return true;
}

static inline bool WriteGuestBytes(void* guest, const void* local, size_t bytes) {
	if (bytes == 0) return true;
	if (!IsValidPointer(guest, bytes) || local == nullptr) return false;
	std::memcpy(guest, local, bytes);
	return true;
}

template <typename T>
static inline bool ReadGuest(const T* guest, T& local) {
	return ReadGuestBytes(&local, guest, sizeof(local));
}

template <typename T>
static inline bool WriteGuest(T* guest, const T& local) {
	return WriteGuestBytes(guest, &local, sizeof(local));
}

static int CheckStreamOption(const void* option, Ngs2StreamOption& checked) {
	if (option == nullptr) {
		checked               = {};
		checked.size          = sizeof(Ngs2StreamOption);
		checked.num_blocks    = 4;
		checked.buffer_size_1 = 0x100;
		checked.buffer_size_2 = 0x10;
		checked.buffer_size_3 = 0x10000;
		checked.default_param = 0.5f;
		return 0;
	}
	// The record size is validated before the rest of the struct: a wrong
	// size must report INVALID_OPTION_SIZE even when the other fields are
	// out of range too.
	uint64_t opt_size = 0;
	if (!ReadGuestBytes(&opt_size, option, sizeof(opt_size)) ||
	    opt_size != sizeof(Ngs2StreamOption)) {
		return SCE_NGS2_ERROR_INVALID_OPTION_SIZE;
	}
	Ngs2StreamOption opt {};
	if (!ReadGuest(static_cast<const Ngs2StreamOption*>(option), opt)) {
		return SCE_NGS2_ERROR_INVALID_OPTION_SIZE;
	}
	if (opt.num_blocks < 1 || opt.num_blocks > 32) {
		return SCE_NGS2_ERROR_INVALID_NUM_BLOCKS;
	}
	if (opt.buffer_size_3 == 0) {
		return SCE_NGS2_ERROR_STREAM_BUFFER_SIZE_3_ZERO;
	}
	if (opt.buffer_size_1 == 0) {
		return SCE_NGS2_ERROR_STREAM_BUFFER_SIZE_1_ZERO;
	}
	if (opt.buffer_size_2 == 0) {
		return SCE_NGS2_ERROR_STREAM_BUFFER_SIZE_2_ZERO;
	}
	checked = opt;
	return 0;
}

static uint64_t ComputeStreamBufferSize(const Ngs2StreamOption& opt) {
	auto         align8 = [](size_t val) -> size_t { return (val + 7u) & ~size_t {7u}; };
	const size_t guard  = (opt.flags & 0x80000000u) ? 16u : 0u;
	const size_t n      = opt.num_blocks;
	const size_t total  = align8(0x368 + guard) + align8(0x28 * n + guard) +
	                      align8(0x28 * n + guard) + align8(opt.buffer_size_3 + guard) +
	                      align8(opt.buffer_size_2 + guard) + align8(opt.buffer_size_1 + guard) +
	                      align8(opt.buffer_size_1 + guard) + 8;
	return static_cast<uint64_t>(total);
}

struct Ngs2StreamInternal {
	uintptr_t             handle = 0;
	Ngs2Internal*         system = nullptr;
	Ngs2StreamOption      option {};
	Ngs2ContextBufferInfo buffer_info {};
	Ngs2BufferAllocator   allocator {};
	bool                  has_allocator = false;
	std::vector<uint8_t>  managed_buffer;
	Common::Mutex         mutex;
};

static Common::Mutex                                    g_ngs2_streams_mutex;
static std::vector<std::unique_ptr<Ngs2StreamInternal>> g_ngs2_streams;

static Ngs2StreamInternal* Ngs2FindStream(uintptr_t handle) {
	if (handle == 0) return nullptr;
	Common::LockGuard lock(g_ngs2_streams_mutex);
	for (auto& s: g_ngs2_streams) {
		if (s && s->handle == handle) {
			return s.get();
		}
	}
	return nullptr;
}

int KYTY_SYSV_ABI Ngs2StreamResetOption(void* option) {
	PRINT_NAME();
	if (option == nullptr) {
		return NGS2_ERROR_INVALID_OUT_ADDRESS;
	}
	Ngs2StreamOption opt {};
	opt.size = sizeof(Ngs2StreamOption);
	std::memset(opt.name, 0, sizeof(opt.name));
	opt.flags         = 0;
	opt.num_blocks    = 4;
	opt.buffer_size_1 = 0x100;
	opt.reserved1     = 0;
	opt.buffer_size_2 = 0x10;
	opt.reserved2     = 0;
	opt.buffer_size_3 = 0x10000;
	opt.reserved3     = 0;
	opt.default_param = 0.5f;
	opt.reserved4     = 0;
	std::memset(opt.reserved_tail, 0, sizeof(opt.reserved_tail));
	if (!WriteGuest(static_cast<Ngs2StreamOption*>(option), opt)) {
		return NGS2_ERROR_INVALID_OUT_ADDRESS;
	}
	return OK;
}

int KYTY_SYSV_ABI Ngs2StreamQueryBufferSize(const void* option, void* buffer_info) {
	PRINT_NAME();
	if (buffer_info == nullptr || !IsValidPointer(buffer_info, sizeof(Ngs2ContextBufferInfo))) {
		return NGS2_ERROR_INVALID_OUT_ADDRESS;
	}
	const Ngs2ContextBufferInfo cleared {};
	if (!WriteGuestBytes(buffer_info, &cleared, offsetof(Ngs2ContextBufferInfo, user_data))) {
		return NGS2_ERROR_INVALID_OUT_ADDRESS;
	}
	Ngs2StreamOption checked {};
	const int        error = CheckStreamOption(option, checked);
	if (error != 0) {
		return error;
	}
	const uint64_t size = ComputeStreamBufferSize(checked);
	if (!WriteGuestBytes(static_cast<uint8_t*>(buffer_info) +
	                         offsetof(Ngs2ContextBufferInfo, host_buffer_size),
	                     &size, sizeof(size))) {
		return NGS2_ERROR_INVALID_OUT_ADDRESS;
	}
	return OK;
}

int KYTY_SYSV_ABI Ngs2StreamCreate(uintptr_t system_handle, const void* option,
                                   const void* buffer_info, uintptr_t* handle) {
	PRINT_NAME();
	if (buffer_info == nullptr) {
		return SCE_NGS2_ERROR_INVALID_BUFFER_INFO;
	}
	Ngs2ContextBufferInfo localBufferInfo {};
	if (!ReadGuest(static_cast<const Ngs2ContextBufferInfo*>(buffer_info), localBufferInfo) ||
	    localBufferInfo.host_buffer == nullptr) {
		return SCE_NGS2_ERROR_INVALID_BUFFER_INFO;
	}
	if (handle == nullptr || !IsValidPointer(handle, sizeof(uintptr_t))) {
		return NGS2_ERROR_INVALID_OUT_ADDRESS;
	}
	Ngs2StreamOption checked {};
	const int        optError = CheckStreamOption(option, checked);
	if (optError != 0) {
		return optError;
	}
	auto* system = Ngs2FindRegisteredSystem(system_handle);
	if (system == nullptr) {
		return NGS2_ERROR_INVALID_SYSTEM_HANDLE;
	}

	auto stream         = std::make_unique<Ngs2StreamInternal>();
	stream->system      = system;
	stream->option      = checked;
	stream->buffer_info = localBufferInfo;
	stream->handle      = reinterpret_cast<uintptr_t>(stream.get());

	*handle = stream->handle;
	{
		Common::LockGuard lock(g_ngs2_streams_mutex);
		g_ngs2_streams.push_back(std::move(stream));
	}
	return OK;
}

int KYTY_SYSV_ABI Ngs2StreamCreateWithAllocator(uintptr_t system_handle, const void* option,
                                                const void* allocator, uintptr_t* handle) {
	PRINT_NAME();
	if (allocator == nullptr) {
		return SCE_NGS2_ERROR_INVALID_BUFFER_ALLOCATOR;
	}
	Ngs2BufferAllocator localAllocator {};
	if (!ReadGuest(static_cast<const Ngs2BufferAllocator*>(allocator), localAllocator) ||
	    localAllocator.alloc_handler == nullptr) {
		return SCE_NGS2_ERROR_INVALID_BUFFER_ALLOCATOR;
	}
	if (handle == nullptr || !IsValidPointer(handle, sizeof(uintptr_t))) {
		return NGS2_ERROR_INVALID_OUT_ADDRESS;
	}
	Ngs2StreamOption checked {};
	const int        optError = CheckStreamOption(option, checked);
	if (optError != 0) {
		return optError;
	}
	auto* system = Ngs2FindRegisteredSystem(system_handle);
	if (system == nullptr) {
		return NGS2_ERROR_INVALID_SYSTEM_HANDLE;
	}

	auto stream           = std::make_unique<Ngs2StreamInternal>();
	stream->system        = system;
	stream->option        = checked;
	stream->allocator     = localAllocator;
	stream->has_allocator = true;

	const uint64_t req_size              = ComputeStreamBufferSize(checked);
	stream->buffer_info.host_buffer_size = req_size;
	stream->buffer_info.user_data        = localAllocator.user_data;
	int32_t alloc_res                    = localAllocator.alloc_handler(&stream->buffer_info);
	if (alloc_res < 0 || stream->buffer_info.host_buffer == nullptr) {
		if (stream->buffer_info.host_buffer != nullptr && localAllocator.free_handler != nullptr) {
			localAllocator.free_handler(&stream->buffer_info);
		}
		stream->has_allocator = false;
		stream->managed_buffer.resize(req_size, 0);
		// Rebuild the buffer info from the managed storage: the allocator
		// callback may have left stale size metadata behind.
		stream->buffer_info.host_buffer      = stream->managed_buffer.data();
		stream->buffer_info.host_buffer_size = req_size;
		stream->buffer_info.user_data        = localAllocator.user_data;
	}
	stream->handle = reinterpret_cast<uintptr_t>(stream.get());

	*handle = stream->handle;
	{
		Common::LockGuard lock(g_ngs2_streams_mutex);
		g_ngs2_streams.push_back(std::move(stream));
	}
	return OK;
}

int KYTY_SYSV_ABI Ngs2StreamDestroy(uintptr_t stream_handle, void* buffer_info) {
	PRINT_NAME();
	if (stream_handle == 0) {
		return SCE_NGS2_ERROR_INVALID_STREAM_HANDLE;
	}
	std::unique_ptr<Ngs2StreamInternal> detached_stream;
	{
		Common::LockGuard lock(g_ngs2_streams_mutex);
		for (auto it = g_ngs2_streams.begin(); it != g_ngs2_streams.end(); ++it) {
			if ((*it) && (*it)->handle == stream_handle) {
				detached_stream = std::move(*it);
				g_ngs2_streams.erase(it);
				break;
			}
		}
	}
	if (!detached_stream) {
		return SCE_NGS2_ERROR_INVALID_STREAM_HANDLE;
	}
	if (buffer_info != nullptr && IsValidPointer(buffer_info, sizeof(Ngs2ContextBufferInfo))) {
		WriteGuest(static_cast<Ngs2ContextBufferInfo*>(buffer_info), detached_stream->buffer_info);
	}
	if (detached_stream->has_allocator && detached_stream->allocator.free_handler != nullptr) {
		detached_stream->allocator.free_handler(&detached_stream->buffer_info);
	}
	return OK;
}

int KYTY_SYSV_ABI Ngs2StreamRunCommands(uintptr_t stream_handle, const void* commands,
                                        size_t count) {
	PRINT_NAME();
	if (stream_handle == 0) {
		return SCE_NGS2_ERROR_INVALID_STREAM_HANDLE;
	}
	if (commands == nullptr && count != 0) {
		return SCE_NGS2_ERROR_INVALID_STREAM_COMMANDS;
	}
	if (count == 0) {
		return OK;
	}
	std::vector<Ngs2CommandRecord> records(count);
	if (!ReadGuestBytes(records.data(), commands, count * sizeof(Ngs2CommandRecord))) {
		return SCE_NGS2_ERROR_INVALID_STREAM_COMMANDS;
	}
	for (size_t i = 0; i < count; ++i) {
		const auto&    rec = records[i];
		const uint32_t cmd = rec.id & 0x00FFFFFFu;
		if (cmd == 0xf20000 || cmd == 0xf20004) {
			continue;
		}
		if (cmd == 0xf20001) {
			if (rec.value.ptr == nullptr) {
				return SCE_NGS2_ERROR_INVALID_STREAM_COMMANDS;
			}
			std::array<const void*, 3> ptrs {};
			if (!ReadGuestBytes(ptrs.data(), rec.value.ptr, sizeof(ptrs))) {
				return SCE_NGS2_ERROR_INVALID_STREAM_COMMANDS;
			}
			if (ptrs[0] == nullptr) {
				return SCE_NGS2_ERROR_INVALID_STREAM_COMMANDS;
			}
			if (ptrs[2] == nullptr) {
				return NGS2_ERROR_INVALID_VOICE_HANDLE;
			}
		} else if (cmd == 0xf20002) {
			const uint32_t val = rec.value.u32;
			if (val != 1 && val != 2 && val != 16 && val != 32) {
				return SCE_NGS2_ERROR_INVALID_STREAM_STATE_COMMAND;
			}
		} else if (cmd == 0xf20003) {
			if (rec.value.ptr == nullptr) {
				return SCE_NGS2_ERROR_NULL_STREAM_BLOCK_PTR;
			}
		} else {
			return SCE_NGS2_ERROR_INVALID_STREAM_COMMAND_ID;
		}
	}
	auto* stream = Ngs2FindStream(stream_handle);
	if (stream == nullptr) {
		return SCE_NGS2_ERROR_INVALID_STREAM_HANDLE;
	}
	return OK;
}

int KYTY_SYSV_ABI Ngs2StreamQueryInfo(uintptr_t stream_handle, uint32_t info_id, void* info,
                                      size_t info_size) {
	PRINT_NAME();
	if (stream_handle == 0) {
		return SCE_NGS2_ERROR_INVALID_STREAM_HANDLE;
	}
	const uint32_t cmd = info_id & 0x00FFFFFFu;
	if (cmd == 0xf24001) {
		if (info_size < 8) return NGS2_ERROR_INVALID_OUT_SIZE;
		if (info == nullptr || !IsValidPointer(info, info_size))
			return NGS2_ERROR_INVALID_OUT_ADDRESS;
	} else if (cmd == 0xf24002) {
		if (info_size != 0x30) return NGS2_ERROR_INVALID_OUT_SIZE;
		if (info == nullptr || !IsValidPointer(info, info_size))
			return NGS2_ERROR_INVALID_OUT_ADDRESS;
	} else if (cmd == 0xf24003) {
		if (info_size < 4) return NGS2_ERROR_INVALID_OUT_SIZE;
		if (info == nullptr || !IsValidPointer(info, info_size))
			return NGS2_ERROR_INVALID_OUT_ADDRESS;
	} else if (cmd == 0xf24004) {
		if (info_size < 8) return NGS2_ERROR_INVALID_OUT_SIZE;
		if (info == nullptr || !IsValidPointer(info, info_size))
			return NGS2_ERROR_INVALID_OUT_ADDRESS;
	} else if (cmd == 0xf24005) {
		if (info_size < 0xe8) return NGS2_ERROR_INVALID_OUT_SIZE;
		if (info == nullptr || !IsValidPointer(info, info_size))
			return NGS2_ERROR_INVALID_OUT_ADDRESS;
	} else {
		return SCE_NGS2_ERROR_INVALID_STREAM_QUERY_ID;
	}
	auto* stream = Ngs2FindStream(stream_handle);
	if (stream == nullptr) {
		return SCE_NGS2_ERROR_INVALID_STREAM_HANDLE;
	}
	// The recognized query ids are not implemented; report that explicitly
	// instead of returning success without writing the output buffer.
	return SCE_NGS2_ERROR_INVALID_STREAM_QUERY_ID;
}

int KYTY_SYSV_ABI Ngs2JobSchedulerResetOption(void* option) {
	PRINT_NAME();
	if (option == nullptr || !IsValidPointer(option, sizeof(Ngs2JobSchedulerOption))) {
		return NGS2_ERROR_INVALID_OUT_ADDRESS;
	}
	Ngs2JobSchedulerOption opt {};
	if (!WriteGuest(static_cast<Ngs2JobSchedulerOption*>(option), opt)) {
		return NGS2_ERROR_INVALID_OUT_ADDRESS;
	}
	return OK;
}

int KYTY_SYSV_ABI Ngs2ModuleEnumConfigs(void** out, uint32_t max) {
	PRINT_NAME();
	(void)out;
	(void)max;
	return 0;
}

int KYTY_SYSV_ABI Ngs2ModuleArrayEnumItems(const void* array, void* out, uint32_t max) {
	PRINT_NAME();
	if (array == nullptr) return 0;
	std::array<uint8_t, 0x20> header {};
	if (!ReadGuestBytes(header.data(), array, header.size())) return 0;
	uint32_t itemSize = 0;
	std::memcpy(&itemSize, header.data() + 0xc, sizeof(itemSize));
	const uint8_t flag  = header[8];
	uint32_t      count = 0;
	std::memcpy(&count, header.data() + 0x14 + 4 * (flag != 0 ? 1 : 0), sizeof(count));
	const uint32_t toCopy = std::min(count, max);
	if (toCopy > 0 && out != nullptr && itemSize > 0) {
		size_t offset = 0x20;
		if (flag == 0) {
			uint32_t baseSkip = 0;
			std::memcpy(&baseSkip, header.data() + 0x10, sizeof(baseSkip));
			offset += static_cast<size_t>(baseSkip) * itemSize;
		}
		std::vector<uint8_t> buffer(static_cast<size_t>(toCopy) * itemSize);
		if (ReadGuestBytes(buffer.data(), static_cast<const uint8_t*>(array) + offset,
		                   buffer.size())) {
			WriteGuestBytes(out, buffer.data(), buffer.size());
		}
	}
	return static_cast<int>(count);
}

int KYTY_SYSV_ABI Ngs2ModuleQueueEnumItems(const void* queue, void* out, uint32_t max) {
	PRINT_NAME();
	if (queue == nullptr) return 0;
	constexpr size_t QueueSize  = 0x58;
	constexpr size_t LockOffset = 0x48;

	const auto queueAddress = reinterpret_cast<uintptr_t>(queue);
	if (queueAddress > UINTPTR_MAX - LockOffset) return 0;
	if (!IsValidPointer(queue, QueueSize)) return 0;

	auto*                     lockAddress = reinterpret_cast<uint32_t*>(queueAddress + LockOffset);
	std::atomic_ref<uint32_t> queueLock(*lockAddress);
	uint32_t                  retries = 0;
	for (;;) {
		uint32_t expected = 0;
		if (queueLock.compare_exchange_weak(expected, 1, std::memory_order_acquire,
		                                    std::memory_order_relaxed))
			break;
		++retries;
		if (retries >= 17) {
			constexpr std::array<uint32_t, 4> Backoff {100, 200, 500, 1000};
			const size_t index = std::min<size_t>(retries - 17, Backoff.size() - 1);
			std::this_thread::sleep_for(std::chrono::microseconds(Backoff[index]));
		}
	}
	struct QueueUnlock {
		std::atomic_ref<uint32_t>& lock;
		~QueueUnlock() { lock.store(0, std::memory_order_release); }
	} unlock {queueLock};

	std::array<uint8_t, QueueSize> queueHeader {};
	if (!ReadGuestBytes(queueHeader.data(), queue, queueHeader.size())) return 0;
	const uint8_t ring = queueHeader[0x4c];
	if (ring > 1) return 0;
	const size_t         recordOffset = 8 + 32 * ring;
	const uint8_t* const record       = queueHeader.data() + recordOffset;
	const void*          items        = nullptr;
	uint64_t             count        = 0;
	uint64_t             first        = 0;
	uint32_t             itemSize     = 0;
	uint32_t             ringLength   = 0;
	std::memcpy(&items, record, sizeof(items));
	std::memcpy(&count, record + 0x10, sizeof(count));
	std::memcpy(&first, record + 0x18, sizeof(first));
	std::memcpy(&itemSize, queueHeader.data() + 0x50, sizeof(itemSize));
	std::memcpy(&ringLength, queueHeader.data() + 0x54, sizeof(ringLength));
	if (count == 0 || max == 0) return static_cast<int32_t>(static_cast<uint32_t>(count));
	const uint32_t copiedCount = static_cast<uint32_t>(std::min<uint64_t>(count, max));
	if (items == nullptr || itemSize == 0 || ringLength == 0 ||
	    copiedCount > std::vector<uint8_t>().max_size() / itemSize) {
		return static_cast<int32_t>(static_cast<uint32_t>(count));
	}
	std::vector<uint8_t> copied(static_cast<size_t>(copiedCount) * itemSize);
	const auto           itemsAddress = reinterpret_cast<uintptr_t>(items);
	bool                 copiedAll    = true;
	for (uint32_t index = 0; index < copiedCount; ++index) {
		const uint64_t slot = (first % ringLength + index) % ringLength;
		if (slot > (UINTPTR_MAX - itemsAddress) / itemSize) {
			copiedAll = false;
			break;
		}
		const void* source =
		    reinterpret_cast<const void*>(itemsAddress + static_cast<uintptr_t>(slot * itemSize));
		if (!ReadGuestBytes(copied.data() + static_cast<size_t>(index) * itemSize, source,
		                    itemSize)) {
			copiedAll = false;
			break;
		}
	}
	if (copiedAll) WriteGuestBytes(out, copied.data(), copied.size());
	return static_cast<int32_t>(static_cast<uint32_t>(count));
}

int KYTY_SYSV_ABI Ngs2ReportRegisterHandler(const void* handler, void* user_data) {
	PRINT_NAME();
	(void)handler;
	(void)user_data;
	return 0;
}

int KYTY_SYSV_ABI Ngs2ReportUnregisterHandler(const void* handler) {
	PRINT_NAME();
	(void)handler;
	return 0;
}

} // namespace Ngs2

} // namespace Libs::Audio
