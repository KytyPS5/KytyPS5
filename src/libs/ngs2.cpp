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
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <deque>
#include <limits>
#include <magic_enum.hpp>
#include <memory>
#include <numbers>
#include <numeric>
#include <optional>
#include <string_view>
#include <tuple>
#include <vector>

#include "libatrac9.h"

namespace Libs::Audio {

namespace Ngs2 {

LIB_NAME("Ngs2", "Ngs2");

constexpr int32_t NGS2_ERROR_INVALID_SYSTEM_HANDLE =
    static_cast<int32_t>(0x804a8201u);
constexpr int32_t NGS2_ERROR_INVALID_RACK_HANDLE =
    static_cast<int32_t>(0x804a8202u);
constexpr int32_t NGS2_ERROR_INVALID_OUT_ADDRESS =
    static_cast<int32_t>(0x804a8010u);
constexpr int32_t NGS2_ERROR_INVALID_OUT_SIZE =
    static_cast<int32_t>(0x804a8011u);
constexpr int32_t NGS2_ERROR_INVALID_WAVEFORM_TYPE =
    static_cast<int32_t>(0x804a8050u);
constexpr int32_t NGS2_ERROR_INVALID_NUM_CHANNELS =
    static_cast<int32_t>(0x804a8052u);
constexpr int32_t NGS2_ERROR_FULL_WAVEFORM_BLOCKS =
    static_cast<int32_t>(0x804a8054u);
constexpr int32_t NGS2_ERROR_INCOMPLETE_WAVEFORM_DATA =
    static_cast<int32_t>(0x804a8056u);
constexpr int32_t NGS2_ERROR_INVALID_WAVEFORM_BLOCK_ADDRESS =
    static_cast<int32_t>(0x804a8058u);
constexpr int32_t NGS2_ERROR_INVALID_FILTER_TYPE =
    static_cast<int32_t>(0x804a8060u);
constexpr int32_t NGS2_ERROR_INVALID_PEAK_METER_NUM_BLOCKS =
    static_cast<int32_t>(0x804a8092u);
constexpr int32_t NGS2_ERROR_INVALID_PEAK_METER_DETECT_TYPE =
    static_cast<int32_t>(0x804a8093u);
constexpr int32_t NGS2_ERROR_INVALID_MATRIX_TYPE =
    static_cast<int32_t>(0x804a809au);
constexpr int32_t NGS2_ERROR_INVALID_RENDER_BUFFER_ADDRESS =
    static_cast<int32_t>(0x804a8251u);
constexpr int32_t NGS2_ERROR_INVALID_RENDER_BUFFER_SIZE =
    static_cast<int32_t>(0x804a8252u);
constexpr int32_t NGS2_ERROR_INVALID_VOICE_INDEX =
    static_cast<int32_t>(0x804a8305u);
constexpr int32_t NGS2_ERROR_INVALID_MODULE_LOCATION =
    static_cast<int32_t>(0x804a8308u);
constexpr int32_t NGS2_ERROR_INVALID_NUM_BUFFERS =
    static_cast<int32_t>(0x804a830bu);
constexpr int32_t NGS2_ERROR_INVALID_VOICE_PARAM_ID =
    static_cast<int32_t>(0x804a835au);
constexpr int32_t NGS2_ERROR_INVALID_VOICE_CONTROL_SIZE =
    static_cast<int32_t>(0x804a8361u);
constexpr int32_t NGS2_ERROR_INVALID_VOICE_CONTROL_ID =
    static_cast<int32_t>(0x804a8362u);
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

// A block without skip or samples only carries data for the samples of the blocks before it.
static bool Ngs2IsDataOnly(const Ngs2WaveformBlock& block) {
	return block.num_skip_samples == 0 && block.num_samples == 0;
}

// Bytes per sample of the PCM waveform types 0x10..0x1b, 0 for other types.
static uint32_t Ngs2PcmSampleBytes(uint32_t type) {
	constexpr std::array<uint8_t, 12> BYTES = {1, 1, 2, 2, 3, 3, 4, 4, 4, 4, 8, 8};
	return type - 0x10 < BYTES.size() ? BYTES[type - 0x10] : 0;
}

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

// The lock a game holds around a batch of calls; rendering and voice calls do not take it. Its
// mutex is held only inside Lock and Unlock, so a locked system or rack can be destroyed.
struct Ngs2UserLock {
	Common::Mutex   mutex;
	Common::CondVar released;
	int             owner = 0;
	uint32_t        depth = 0;

	void Lock() {
		const int         self = Common::Thread::GetThreadIdUnique();
		Common::LockGuard lock(mutex);
		while (depth != 0 && owner != self) {
			released.Wait(&mutex);
		}
		owner = self;
		depth++;
	}

	// An unlock by a thread that does not hold the lock is ignored.
	void Unlock() {
		Common::LockGuard lock(mutex);
		if (depth == 0 || owner != Common::Thread::GetThreadIdUnique()) {
			return;
		}
		if (--depth == 0) {
			released.Signal();
		}
	}
};

struct Ngs2Internal {
	Ngs2SystemOption      option;
	Ngs2ContextBufferInfo buffer_info;
	Ngs2BufferAllocator   allocator;
	Ngs2Internal*         next         = nullptr;
	uint64_t              render_count = 0;
	uint32_t              uid          = 0;
	Common::Mutex         mutex;
	Ngs2UserLock          user_lock;
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
	Ngs2Internal*                        ngs  = nullptr;
	Ngs2RackInternal*                    next = nullptr;
	Ngs2RackType                         type = Ngs2RackType::Sampler;
	Ngs2RackOptionUnion                  option;
	Ngs2ContextBufferInfo                buffer_info;
	Ngs2BufferAllocator                  allocator;
	std::array<std::vector<uint8_t>, 24> common;
	std::array<Ngs2UserFx2Option, 24>    fx {};
	Ngs2UserLock                         user_lock;

	[[nodiscard]] uint32_t NumBuffers() const {
		return Ngs2RackIsCustom(type) ? option.custom_sampler.custom_rack_option.num_buffers : 1;
	}

	[[nodiscard]] uint32_t PortBufferId(size_t port) const {
		const auto& custom = option.custom_sampler.custom_rack_option;
		return Ngs2RackIsCustom(type) ? custom.port[port].source_buffer_id : 0;
	}
};

enum class Ngs2VoicePlayState : uint32_t { Empty = 0, Playing = 3, Paused = 5, Stopped = 0xb };

// A filter: channel_mask bits mark the channels that pass unfiltered; DIRECT (type 11) takes its
// coefficients verbatim, the other types design them from fc, q and level.
struct Ngs2FilterParam {
	uint64_t channel_mask;
	uint32_t type;
	union {
		struct {
			float i0, i1, i2, o1, o2;
		} direct;
		struct {
			float    fc, q, level;
			uint32_t reserved, reserved2;
		} fcq;
	} param;
};
static_assert(sizeof(Ngs2FilterParam) == 32);

struct Ngs2SamplerFilterParam {
	uint32_t        index;
	uint32_t        location;
	Ngs2FilterParam filter;
	uint32_t        reserved3;
};
static_assert(sizeof(Ngs2SamplerFilterParam) == 48);

// A biquad in direct form I: y = i0 * x + i1 * x1 + i2 * x2 + o1 * y1 + o2 * y2, in float.
struct Ngs2SamplerFilter {
	uint32_t location     = 0;
	uint32_t type         = 0;
	uint64_t channel_mask = 0;
	float    i0 = 1, i1 = 0, i2 = 0, o1 = 0, o2 = 0;
	// Per channel {x1, x2, y1, y2}; empty until the filter is set.
	std::vector<std::array<float, 4>> history;

	// Designs the filter at `rate`. The cutoff is clamped to 0.0001..0.499 of the rate and Q to
	// 0.0001..32; only the shelves and peaking read the level, a linear gain.
	void Design(const Ngs2FilterParam& param, uint32_t rate) {
		type         = param.type;
		channel_mask = param.channel_mask;
		if (type == 11) {
			const auto& direct = param.param.direct;
			std::tie(i0, i1, i2, o1, o2) =
			    std::tie(direct.i0, direct.i1, direct.i2, direct.o1, direct.o2);
			return;
		}
		const auto& fcq   = param.param.fcq;
		const float w     = std::clamp(fcq.fc / static_cast<float>(rate), 1e-4f, 0.499f) * 2 *
		                    std::numbers::pi_v<float>;
		const float q     = std::clamp(fcq.q, 1e-4f, 32.0f);
		const float s     = std::sin(w);
		const float c     = std::cos(w);
		const float alpha = s / (2 * q);
		const float gain  = std::sqrt(std::max(1e-4f, fcq.level));
		float       b0 = 0, b1 = 0, b2 = 0, a0 = 1 + alpha, a1 = -2 * c, a2 = 1 - alpha;
		switch (type) {
			case 1: // Low-pass.
				b0 = b2 = (1 - c) * 0.5f;
				b1      = 1 - c;
				break;
			case 2: // High-pass.
				b0 = b2 = (1 + c) * 0.5f;
				b1      = -(1 + c);
				break;
			case 3: // Band-pass with a peak gain of Q.
				b0 = s * 0.5f;
				b2 = -b0;
				break;
			case 4: // Band-pass with a unity peak.
				b0 = alpha;
				b2 = -alpha;
				break;
			case 5: // Notch.
				b0 = b2 = 1;
				b1      = a1;
				break;
			case 6:
			case 7: {
				// The high shelf is the low shelf mirrored around half the rate.
				const float sign = type == 6 ? 1.0f : -1.0f;
				const float e    = sign * c;
				const float k    = std::sqrt(gain) / q * s;
				b0               = gain * ((gain + 1) - (gain - 1) * e + k);
				b1               = sign * 2 * gain * ((gain - 1) - (gain + 1) * e);
				b2               = gain * ((gain + 1) - (gain - 1) * e - k);
				a0               = (gain + 1) + (gain - 1) * e + k;
				a1               = -sign * 2 * ((gain - 1) + (gain + 1) * e);
				a2               = (gain + 1) + (gain - 1) * e - k;
				break;
			}
			case 8: // Peaking.
				b0 = 1 + alpha * gain;
				b1 = a1;
				b2 = 1 - alpha * gain;
				a0 = 1 + alpha / gain;
				a2 = 1 - alpha / gain;
				break;
			case 9: // One-pole low-pass.
				b0 = b1 = s;
				a0      = (s + c) + 1;
				a1      = (s - c) - 1;
				a2      = 0;
				break;
			default: // One-pole high-pass (10); OFF (0) bypasses every channel.
				b0 = 1 + c;
				b1 = -b0;
				a0 = s + (1 + c);
				a1 = s - (1 + c);
				a2 = 0;
		}
		const float inverse = 1 / a0;
		i0                  = b0 * inverse;
		i1                  = b1 * inverse;
		i2                  = b2 * inverse;
		o1                  = -a1 * inverse;
		o2                  = -a2 * inverse;
	}

	bool Bypasses(size_t channel) const {
		return type == 0 || (channel < 64 && (channel_mask & (uint64_t {1} << channel)) != 0);
	}

	// Whether the filter has history left; a bypassed channel's follows its input to zero.
	bool HasTail() const {
		return std::ranges::any_of(history,
		                           [](const auto& h) { return h != std::array<float, 4> {}; });
	}

	// Filters channel-major samples in place. A bypassed channel, every one for OFF, passes
	// through, its history following the input.
	void Process(float* samples, uint32_t grain) {
		for (size_t c = 0; c < history.size(); ++c) {
			auto* x                = samples + c * grain;
			auto& [x1, x2, y1, y2] = history[c];
			if (Bypasses(c)) {
				x2 = y2 = grain > 1 ? x[grain - 2] : x1;
				x1 = y1 = x[grain - 1];
				continue;
			}
			for (uint32_t i = 0; i < grain; ++i) {
				float y = i0 * x[i] + i1 * x1 + i2 * x2 + o1 * y1 + o2 * y2;
				// A decayed tail ends at zero.
				if (std::abs(y) < 1e-20f) {
					y = 0;
				}
				x2   = x1;
				x1   = x[i];
				y2   = y1;
				y1   = y;
				x[i] = y;
			}
		}
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

// The channel converter keeps one level field per input count and field output count (1, 2, 4, 6
// or 8), in this order; an output count reads the first rows of the smallest field with at least
// as many channels. Levels are dst-major rows of one level per input: 0, 1, h = 0.70710677,
// q = 0.5, e = 0.35355339.
struct Ngs2MasteringField {
	uint32_t         inputs;
	uint32_t         outputs;
	std::string_view levels;
};
// clang-format off
constexpr std::array<Ngs2MasteringField, 40> NGS2_MASTERING_FIELDS = {{
    {1, 1, "1"},
    {1, 2, "hh"},
    {1, 4, "hh00"},
    {1, 6, "001000"},
    {1, 8, "00100000"},
    {2, 1, "hh"},
    {2, 2, "10" "01"},
    {2, 4, "10" "01" "00" "00"},
    {2, 6, "10" "01" "00" "00" "00" "00"},
    {2, 8, "10" "01" "00" "00" "00" "00" "00" "00"},
    {3, 1, "hh1"},
    {3, 2, "10h" "01h"},
    {3, 4, "10h" "01h" "000" "000"},
    {3, 6, "100" "010" "001" "000" "000" "000"},
    {3, 8, "100" "010" "001" "000" "000" "000" "000" "000"},
    {4, 1, "hhqq"},
    {4, 2, "10h0" "010h"},
    {4, 4, "1000" "0100" "0010" "0001"},
    {4, 6, "1000" "0100" "0000" "0000" "0010" "0001"},
    {4, 8, "1000" "0100" "0000" "0000" "0010" "0001" "0000" "0000"},
    {5, 1, "hh1qq"},
    {5, 2, "10hh0" "01h0h"},
    {5, 4, "10h00" "01h00" "00010" "00001"},
    {5, 6, "10000" "01000" "00100" "00000" "00010" "00001"},
    {5, 8, "10000" "01000" "00100" "00000" "00010" "00001" "00000" "00000"},
    {6, 1, "hh10qq"},
    {6, 2, "10h0h0" "01h00h"},
    {6, 4, "10h000" "01h000" "000010" "000001"},
    {6, 6, "100000" "010000" "001000" "000100" "000010" "000001"},
    {6, 8, "100000" "010000" "001000" "000100" "000010" "000001" "000000" "000000"},
    {7, 1, "hh10qqe"},
    {7, 2, "10h0h0q" "01h00hq"},
    {7, 4, "10h0000" "01h0000" "000010h" "000001h"},
    {7, 6, "1000000" "0100000" "0010000" "0001000" "000010h" "000001h"},
    {7, 8, "1000000" "0100000" "0010000" "0001000" "0000100" "0000010" "000000h" "000000h"},
    {8, 1, "hh10hhhh"},
    {8, 2, "10h0h0h0" "01h00h0h"},
    {8, 4, "10000000" "01000000" "00001010" "00000101"},
    {8, 6, "10000000" "01000000" "00100000" "00010000" "00001010" "00000101"},
    {8, 8, "10000000" "01000000" "00100000" "00010000" "00001000" "00000100" "00000010" "00000001"},
}};
// clang-format on

// Offset of the levels that convert `inputs` into `outputs` channels.
static constexpr size_t Ngs2MasteringLevelsOffset(uint32_t inputs, uint32_t outputs) {
	size_t offset = 0;
	for (const auto& field: NGS2_MASTERING_FIELDS) {
		if (field.inputs == inputs && field.outputs >= outputs) {
			break;
		}
		offset += field.levels.size();
	}
	return offset;
}

constexpr auto NGS2_MASTERING_DEFAULT_LEVELS = [] {
	constexpr float h = std::numbers::sqrt2_v<float> / 2;

	std::array<float, Ngs2MasteringLevelsOffset(8, 8) + 8 * 8> levels {};
	for (size_t n = 0; const auto& field: NGS2_MASTERING_FIELDS) {
		for (const char level: field.levels) {
			switch (level) {
				case '1': levels[n] = 1.0f; break;
				case 'h': levels[n] = h; break;
				case 'q': levels[n] = 0.5f; break;
				case 'e': levels[n] = h / 2; break;
				default: break;
			}
			++n;
		}
	}
	return levels;
}();

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
	};
	struct Module {
		std::vector<uint8_t>     param, work, state;
		std::vector<float>       channel_matrix;
		void                     KYTY_SYSV_ABI (*handler)(Ngs2UserFxProcessContext*) = nullptr;
		std::array<uintptr_t, 3> user_data {};
		uint32_t                 flags          = 1;
		bool                     control_warned = false;
		bool                     render_warned  = false;
	};
	// Mastering voice module state.
	struct Mastering {
		std::array<float, NGS2_MASTERING_DEFAULT_LEVELS.size()> levels =
		    NGS2_MASTERING_DEFAULT_LEVELS;
		// The converter levels of the previous grain and their shape; inputs 0 skips the ramp.
		std::array<float, 64> previous_levels {};
		uint32_t              previous_inputs  = 0;
		uint32_t              previous_outputs = 0;
		// LFE filter on channel 3: two sections y = b0 * x + b1 * x1 + b2 * x2 + a1 * y1 + a2 * y2,
		// (b0, b1, b2) = gain * (1, 2, 1). An LFE control enables the stage until Setup; the first
		// grain after a grain without it, or after PLAY, starts from zero history. A cleared enable
		// flag leaves channel 3 and the history as they are.
		struct LfeSection {
			float gain = 0.0f, a1 = 0.0f, a2 = 0.0f;
			struct History {
				float x1 = 0.0f, x2 = 0.0f, y1 = 0.0f, y2 = 0.0f;
			} history;
		};
		std::array<LfeSection, 2> lfe {};
		bool                      lfe_enable_flag = false;
		bool                      lfe_enabled     = false;
		// Limiter: each block of 32 samples takes the peak of every channel, and the gain moves
		// toward threshold / peak above the threshold, else 1, with the attack coefficient while
		// above that target and the release coefficient otherwise. A limiter control enables the
		// stage until Setup; a grain without it, a cleared enable flag or PLAY restore unity gain.
		// The state is the last block's peak and the gain after it, {0, 1} with a cleared enable
		// flag and zero until the stage renders after Setup.
		float                limiter_threshold = 0.0f;
		float                limiter_attack    = 0.0f;
		float                limiter_release   = 0.0f;
		float                limiter_gain      = 1.0f;
		std::array<float, 2> limiter_state {};
		bool                 limiter_enable_flag = false;
		bool                 limiter_enabled     = false;
		// Full band and LFE gains. A gain control enables the gain until Setup; the first grain
		// after a grain without it, or after PLAY, starts at the new gains.
		std::array<float, 2> gain {};
		std::array<float, 2> previous_gain {};
		bool                 gain_enabled  = false;
		bool                 gain_rendered = false;
		// Peak meters on the N channels before the converter and after the gain, sharing a detect
		// type and a window of the rack's blocks. A peak meter control enables both until Setup;
		// a detect type change, a grain without them and PLAY restart the window. Each grain
		// writes its peak (detect type 1) or energy (2) per channel into the window's next block,
		// and the meters report the window's peak or RMS per channel, none for detect type 0 or
		// after Setup. Mode 0 means no window. Each channel keeps the input then the output
		// meter's window.
		std::vector<float>                   meter_blocks;
		std::array<std::array<float, 37>, 2> meter_peaks {};
		uint32_t                             meter_detect_type = 0;
		uint32_t                             meter_mode        = 0;
		uint32_t                             meter_index       = 0;
		uint32_t                             meter_count       = 0;
		uint32_t                             meter_channels    = 0;
		bool                                 meters_enabled    = false;
	};
	Ngs2VoicePlayState              state          = Ngs2VoicePlayState::Empty;
	uint32_t                        state_flags    = 0;
	// Set by PLAY until the next render publishes the state flags; meanwhile the voice's modules
	// report nothing.
	bool                            play_pending   = false;
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
	uint32_t                        channels        = 0;
	uint32_t                        output_id       = 0;
	std::unique_ptr<Mastering>      mastering;
	Ngs2ReverbState                 reverb;
	bool                            rendering       = false;
	bool                            rendered        = false;
	bool                            has_samples     = false;

	std::unique_ptr<Ajm::AjmAt9Decoder> decoder;
	std::vector<float>                  decoded_frame;
	std::vector<uint8_t>                compressed_input;
	uint32_t                            frame_cursor       = 0;
	uint32_t                            frame_samples      = 0;
	bool                                accepts_blocks     = true;
	// Set when a straddling frame continues into a block with samples, which silences the grain.
	bool                                incomplete_frame   = false;
	uint32_t                            waveform_type      = 0;
	uint32_t                            sample_rate        = 0;
	// The resampler's pitch ratio, its phase in 1/16384 source samples and up to 4 source samples
	// per channel kept for the next grain.
	float                               pitch_ratio        = 1.0f;
	uint16_t                            resample_phase     = 0;
	uint32_t                            history_samples    = 0;
	std::vector<float>                  history;
	std::vector<float>                  resample_input;
	uint64_t                            decoded_samples    = 0;
	uint64_t                            decoded_bytes      = 0;
	uint64_t                            source_generation  = 0;
	uint32_t                            duration_remaining = 0;
	uint32_t                            skip_remaining     = 0;
	const uint8_t*                      waveform_end       = nullptr;
	std::vector<Ngs2SamplerFilter>      filters;

	const uint8_t* WaveformData() const {
		return blocks.empty() ? waveform_end : blocks.front().data + blocks.front().data_cursor;
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

	// Runs the filters at `location`, in index order, on channel-major samples.
	void RunFilters(uint32_t location, float* data, uint32_t grain) {
		for (auto& filter: filters) {
			if (filter.location == location) {
				filter.Process(data, grain);
			}
		}
	}

	void ResetSetupState() {
		SetEvent(4);
		std::ranges::fill(ports, Port {});
		for (auto& matrix: matrices) {
			matrix.clear();
		}
		filters.clear();
	}

	void ClearWaveformBlocks() {
		++source_generation;
		blocks.clear();
		duration_remaining = 0;
		skip_remaining     = 0;
		compressed_input.clear();
		frame_cursor       = 0;
		frame_samples      = 0;
		waveform_end       = nullptr;
	}

	// Setup and every PLAY start the resampler at phase 0 with 4 zero samples of history.
	void ResetResampler() {
		history.assign(size_t(channels) * 4, 0.0f);
		history_samples = 4;
		resample_phase  = 0;
	}

	void SetupSampler(const Ngs2WaveformFormat& format) {
		const bool deferred = format.waveform_type == 0 && format.num_channels == 0 &&
		                      format.sample_rate == 0 && format.config_data == 0 &&
		                      format.frame_margin == 0 && format.frame_offset == 0;
		EXIT_NOT_IMPLEMENTED(!deferred &&
		                     (format.frame_margin != 0 || format.frame_offset != 0 ||
		                      format.sample_rate == 0 || format.num_channels == 0));
		ResetSetupState();
		ClearWaveformBlocks();
		channels        = format.num_channels;
		waveform_type   = format.waveform_type;
		sample_rate     = format.sample_rate;
		pitch_ratio     = 1.0f;
		ResetResampler();
		decoded_samples = 0;
		decoded_bytes   = 0;
		decoder.reset();
		decoded_frame.clear();
		accepts_blocks = true;
		if (deferred) {
			return;
		}
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
		} else {
			EXIT_NOT_IMPLEMENTED(Ngs2PcmSampleBytes(format.waveform_type) == 0);
		}
	}

	int AddWaveformBlocks(const uint8_t* data, uint32_t flags, uint32_t count,
	                      const Ngs2WaveformBlock* waveform_blocks) {
		const auto capacity = rack->type == Ngs2RackType::Sampler
		                          ? rack->option.sampler.max_waveform_blocks
		                          : rack->option.custom_sampler.max_waveform_blocks;
		const bool clear    = (flags & 4u) != 0;
		const bool cancel   = (flags & 8u) != 0;
		// Only CLEAR reopens a list that the previous addition closed.
		if (!clear && !accepts_blocks) {
			return NGS2_ERROR_FULL_WAVEFORM_BLOCKS;
		}
		// The capacity counts added blocks before they join; CANCEL keeps room for the front block.
		size_t kept = cancel ? 1 : blocks.size();
		if (clear) {
			kept = 0;
		}
		if (kept + count > capacity) {
			return NGS2_ERROR_FULL_WAVEFORM_BLOCKS;
		}
		if (clear) {
			ClearWaveformBlocks();
		} else if (cancel) {
			// CANCEL drops every block behind the front one, started or not, without a callback.
			blocks.resize(std::min<size_t>(blocks.size(), 1));
		}
		const bool only_data = (flags & 2u) != 0;
		const bool join      = (flags & 0x10u) == 0;
		for (uint32_t i = 0; i < count; ++i) {
			auto info = waveform_blocks[i];
			if (only_data) {
				info.num_repeats      = 0;
				info.num_skip_samples = 0;
				info.num_samples      = 0;
			}
			const auto* block_data = data + info.data_offset;
			// A data-only block with a repeat count of 0 extends a contiguous last block of the
			// same user data.
			if (join && !blocks.empty() && Ngs2IsDataOnly(info) && info.num_repeats == 0 &&
			    blocks.back().data + blocks.back().info.data_size == block_data &&
			    blocks.back().info.user_data == info.user_data) {
				blocks.back().info.data_size += info.data_size;
			} else {
				blocks.push_back({block_data, info});
			}
		}
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
				}
				// Every PLAY reads as in use alone until the next render publishes the voice.
				state_flags  = 1;
				play_pending = true;
				// Every PLAY restarts the resampler, filters and mastering modules without a ramp or
				// history.
				ResetResampler();
				for (auto& filter: filters) {
					std::ranges::fill(filter.history, std::array<float, 4> {});
				}
				if (mastering != nullptr) {
					mastering->previous_inputs = 0;
					mastering->lfe[0].history  = {};
					mastering->lfe[1].history  = {};
					mastering->limiter_gain    = 1.0f;
					mastering->gain_rendered   = false;
					mastering->meter_mode      = 0;
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

struct Ngs2VoiceParamHeader {
	uint16_t size;
	int16_t  next;
	uint32_t id;
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

struct Ngs2MasteringVoiceState {
	Ngs2VoiceState voice_state;
	float          limiter_peak_level;
	float          limiter_press_level;
	float          input_peak_height[37];
	float          output_peak_height[37];
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
// Guards g_ngs_list; no other lock is taken while it is held.
static Common::Mutex g_ngs_mutex;

static_assert(sizeof(Ngs2UserFxProcessContext) == 48);
static_assert(sizeof(Ngs2UserFx2Context) == 72);
static_assert(sizeof(Ngs2UserFx2ProcessContext) == 104);
static_assert(sizeof(Ngs2UserFx2Option) == 72);
static_assert(sizeof(Ngs2SystemOption) == 144);
static_assert(sizeof(Ngs2SystemInfo) == 184);
static_assert(sizeof(Ngs2RackOption) == 176);
static_assert(sizeof(Ngs2VoiceState) == 8);
static_assert(sizeof(Ngs2SubmixerVoiceState) == 20);
static_assert(sizeof(Ngs2MasteringVoiceState) == 0x138);
static_assert(sizeof(Ngs2CustomMasteringVoiceState) == 16);
static_assert(sizeof(Ngs2SamplerVoiceState) == 56);
static_assert(sizeof(Ngs2CustomSamplerVoiceState) == 48);
static_assert(sizeof(Ngs2WaveformFormat) == 24);
static_assert(offsetof(Ngs2WaveformFormat, frame_margin) == 16 &&
              offsetof(Ngs2WaveformFormat, frame_offset) == 20);
static_assert(sizeof(Ngs2WaveformBlock) == 40);
static_assert(sizeof(Ngs2WaveformInfo) == 232);

static Ngs2SystemOption Ngs2DefaultSystemOption() {
	Ngs2SystemOption option {};
	option.size              = sizeof(Ngs2SystemOption);
	option.max_grain_samples = 512;
	option.num_grain_samples = 256;
	option.sample_rate       = 48000;
	return option;
}

// The system of racks whose system was destroyed. The racks stay valid handles that never render;
// their voice calls apply as usual, at the default sample rate.
static Ngs2Internal g_detached_system {.option = Ngs2DefaultSystemOption()};

// Unlinks and returns the list entry with this handle, or nullptr.
template <typename T>
static T* Ngs2Unlink(T*& list, uintptr_t handle) {
	auto** link = &list;
	while (*link != nullptr && reinterpret_cast<uintptr_t>(*link) != handle) {
		link = &(*link)->next;
	}
	auto* entry = *link;
	if (entry != nullptr) {
		*link = entry->next;
	}
	return entry;
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

	Common::LockGuard lock(g_ngs_mutex);
	ngs->next  = g_ngs_list;
	g_ngs_list = ngs;

	return ngs;
}

static Ngs2Internal* Ngs2FindSystem(uintptr_t system_handle) {
	Common::LockGuard lock(g_ngs_mutex);
	for (auto* ngs = g_ngs_list; ngs != nullptr; ngs = ngs->next) {
		if (reinterpret_cast<uintptr_t>(ngs) == system_handle) {
			return ngs;
		}
	}
	return nullptr;
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

// Checks the rack options that sizing and creating a rack both reject.
static int Ngs2CheckRackOption(uint32_t rack_id, const Ngs2RackOption& option) {
	if (rack_id != 0x3000) {
		return OK;
	}
	EXIT_NOT_IMPLEMENTED(option.size != sizeof(Ngs2MasteringRackOption));
	const auto& mastering = reinterpret_cast<const Ngs2MasteringRackOption&>(option);
	if (mastering.max_channels - 1 >= 37) {
		return NGS2_ERROR_INVALID_NUM_CHANNELS;
	}
	return mastering.num_peak_meter_blocks - 1 >= 256 ? NGS2_ERROR_INVALID_PEAK_METER_NUM_BLOCKS
	                                                  : OK;
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

	const auto result = Ngs2CheckRackOption(rack_id, *option);
	if (result != OK) {
		return result;
	}

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

	if (info == nullptr) {
		return ERROR_INVALID_OUT_ADDRESS;
	}
	if (info_size != sizeof(Ngs2SystemInfo)) {
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

	auto*             ngs = reinterpret_cast<Ngs2Internal*>(system_handle);
	Common::LockGuard lock(ngs->mutex);
	ngs->option.num_grain_samples = num_samples;

	return OK;
}

// Disconnects the ports of the racks of `ngs` whose destination lies in [first, first + size).
// The caller holds g_racks_mutex.
static void Ngs2DisconnectPorts(const Ngs2Internal* ngs, uintptr_t first, uintptr_t size) {
	for (auto* rack = g_racks_list; rack != nullptr; rack = rack->next) {
		if (rack->ngs != ngs) {
			continue;
		}
		auto* voices = reinterpret_cast<Ngs2VoiceInternal*>(rack + 1);
		for (uint32_t i = 0; i < rack->option.common.max_voices; ++i) {
			for (auto& port: voices[i].ports) {
				if (reinterpret_cast<uintptr_t>(port.dest) - first < size) {
					port.dest = nullptr;
				}
			}
		}
	}
}

int KYTY_SYSV_ABI Ngs2SystemDestroy(uintptr_t system_handle, Ngs2ContextBufferInfo* buffer_info) {
	PRINT_NAME();
	LOGF("\t system_handle = 0x%016" PRIx64 "\n", static_cast<uint64_t>(system_handle));

	Ngs2Internal* ngs = nullptr;
	{
		Common::LockGuard lock(g_ngs_mutex);
		ngs = Ngs2Unlink(g_ngs_list, system_handle);
	}
	if (ngs == nullptr) {
		return NGS2_ERROR_INVALID_SYSTEM_HANDLE;
	}
	{
		// Waits for a render in progress. The racks are disconnected from every patch.
		Common::LockGuard lock(ngs->mutex);
		Common::LockGuard racks_lock(g_racks_mutex);
		Ngs2DisconnectPorts(ngs, 0, UINTPTR_MAX);
		for (auto* rack = g_racks_list; rack != nullptr; rack = rack->next) {
			if (rack->ngs == ngs) {
				rack->ngs = &g_detached_system;
			}
		}
	}
	auto       context_buffer = ngs->buffer_info;
	const auto free_handler   = ngs->allocator.free_handler;
	std::destroy_at(ngs);
	if (buffer_info != nullptr) {
		*buffer_info = context_buffer;
	}
	// The result of the free handler is ignored.
	if (free_handler != nullptr) {
		free_handler(&context_buffer);
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
	const auto result = Ngs2CheckRackOption(rack_id, *option);
	if (result != OK) {
		return result;
	}

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
		if (rack->type == Ngs2RackType::Mastering) {
			voice.mastering = std::make_unique<Ngs2VoiceInternal::Mastering>();
			voice.mastering->meter_blocks.resize(size_t(2) * rack->option.mastering.max_channels *
			                                     rack->option.mastering.num_peak_meter_blocks);
		}
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

	int result = Ngs2RackQueryBufferSize(rack_id, option, &buf);
	if (result != OK) {
		return result;
	}

	EXIT_NOT_IMPLEMENTED(buf.host_buffer_size == 0);

	result = allocator->alloc_handler(&buf);

	EXIT_NOT_IMPLEMENTED(result != OK);
	EXIT_NOT_IMPLEMENTED(buf.host_buffer == nullptr);

	result = Ngs2RackCreate(system_handle, rack_id, option, &buf, handle);

	if (result == OK) {
		auto* rack      = static_cast<Ngs2RackInternal*>(buf.host_buffer);
		rack->allocator = *allocator;
	}

	return result;
}

// The created rack with this handle, or nullptr.
static Ngs2RackInternal* Ngs2FindRack(uintptr_t handle) {
	Common::LockGuard racks_lock(g_racks_mutex);
	for (auto* rack = g_racks_list; rack != nullptr; rack = rack->next) {
		if (reinterpret_cast<uintptr_t>(rack) == handle) {
			return rack;
		}
	}
	return nullptr;
}

int KYTY_SYSV_ABI Ngs2RackDestroy(uintptr_t rack_handle, Ngs2ContextBufferInfo* buffer_info) {
	PRINT_NAME();
	LOGF("\t rack_handle = 0x%016" PRIx64 "\n", static_cast<uint64_t>(rack_handle));

	Ngs2RackInternal* rack = nullptr;
	{
		Common::LockGuard racks_lock(g_racks_mutex);
		rack = Ngs2Unlink(g_racks_list, rack_handle);
	}
	if (rack == nullptr) {
		return NGS2_ERROR_INVALID_RACK_HANDLE;
	}
	auto* voices = reinterpret_cast<Ngs2VoiceInternal*>(rack + 1);
	{
		// Waits for a render in progress, which collected the rack before it was unlinked, and
		// disconnects the ports whose destination is a voice of the rack.
		Common::LockGuard lock(rack->ngs->mutex);
		Common::LockGuard racks_lock(g_racks_mutex);
		Ngs2DisconnectPorts(rack->ngs, reinterpret_cast<uintptr_t>(voices),
		                    sizeof(Ngs2VoiceInternal) * rack->option.common.max_voices);
	}
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
	auto       context_buffer = rack->buffer_info;
	const auto free_handler   = rack->allocator.free_handler;
	std::destroy_at(rack);
	if (buffer_info != nullptr) {
		*buffer_info = context_buffer;
	}
	// The result of the free handler is ignored.
	if (free_handler != nullptr) {
		free_handler(&context_buffer);
	}

	return OK;
}

int KYTY_SYSV_ABI Ngs2RackLock(uintptr_t rack_handle) {
	PRINT_NAME();
	LOGF("\t rack_handle = 0x%016" PRIx64 "\n", static_cast<uint64_t>(rack_handle));

	auto* rack = Ngs2FindRack(rack_handle);
	if (rack == nullptr) {
		return NGS2_ERROR_INVALID_RACK_HANDLE;
	}

	rack->user_lock.Lock();

	return OK;
}

int KYTY_SYSV_ABI Ngs2RackUnlock(uintptr_t rack_handle) {
	PRINT_NAME();
	LOGF("\t rack_handle = 0x%016" PRIx64 "\n", static_cast<uint64_t>(rack_handle));

	auto* rack = Ngs2FindRack(rack_handle);
	if (rack == nullptr) {
		return NGS2_ERROR_INVALID_RACK_HANDLE;
	}

	rack->user_lock.Unlock();

	return OK;
}

struct Ngs2VoiceCallbackInfo {
	uintptr_t   data, voice;
	uint32_t    flag, reserved;
	uintptr_t   user;
	const void* block_data;
	size_t      size;
	uint32_t    repeats, attributes;
};
static_assert(sizeof(Ngs2VoiceCallbackInfo) == 0x38);
static_assert(offsetof(Ngs2VoiceCallbackInfo, size) == 0x28 &&
              offsetof(Ngs2VoiceCallbackInfo, repeats) == 0x30 &&
              offsetof(Ngs2VoiceCallbackInfo, attributes) == 0x34);

// A block plays max(1, num_repeats) passes; UINT32_MAX repeats it endlessly.
static bool Ngs2LoopsAgain(const Ngs2VoiceInternal::Block& block) {
	return block.info.num_repeats == UINT32_MAX ||
	       block.repeated_count + 1 < block.info.num_repeats;
}

static bool Ngs2FinishBlock(Ngs2VoiceInternal& voice) {
	const auto generation = voice.source_generation;
	auto&      block      = voice.blocks.front();
	const bool repeat     = Ngs2LoopsAgain(block);
	// Every pass and every dropped block counts its whole data as decoded.
	voice.decoded_bytes += block.info.data_size - block.data_cursor;
	voice.waveform_end = voice.WaveformData();
	if (repeat) {
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
	                                  block.info.data_size,
	                                  repeat ? block.repeated_count : 0,
	                                  0};
	if (!repeat) {
		voice.blocks.pop_front();
	}
	// A block without data reports nothing.
	if (voice.callback != 0 && (voice.callback_flags & info.flag) != 0 && info.size != 0) {
		reinterpret_cast<void KYTY_SYSV_ABI (*)(const Ngs2VoiceCallbackInfo*)>(voice.callback)(
		    &info);
	}
	return voice.source_generation == generation;
}

template <typename T>
static T Ngs2Load(const uint8_t* data) {
	T value;
	std::memcpy(&value, data, sizeof(T));
	return value;
}

template <typename T>
static T Ngs2LoadBigEndian(const uint8_t* data) {
	T value = 0;
	for (size_t i = 0; i < sizeof(T); ++i) {
		value = static_cast<T>(value << 8u | data[i]);
	}
	return value;
}

// Converts `count` interleaved samples of PCM waveform `type` to floats without clamping:
// integers scale by a power of two, floats are copied bit for bit and doubles rounded once.
static void Ngs2ConvertPcm(uint32_t type, const uint8_t* data, size_t count, float* out) {
	const auto convert = [&](size_t bytes, auto sample) {
		for (size_t i = 0; i < count; ++i) {
			out[i] = sample(data + i * bytes);
		}
	};
	switch (type) {
		case 0x10: convert(1, [](auto p) { return float(int8_t(p[0])) * 0x1p-7f; }); break;
		case 0x11: convert(1, [](auto p) { return (float(p[0]) - 128.0f) * 0x1p-7f; }); break;
		case 0x12: convert(2, [](auto p) { return float(Ngs2Load<int16_t>(p)) * 0x1p-15f; }); break;
		case 0x13:
			convert(2, [](auto p) {
				return float(int16_t(Ngs2LoadBigEndian<uint16_t>(p))) * 0x1p-15f;
			});
			break;
		case 0x14:
			convert(3, [](auto p) {
				return float(int32_t(uint32_t(p[0]) << 8u | uint32_t(p[1]) << 16u |
				                     uint32_t(p[2]) << 24u)) *
				       0x1p-31f;
			});
			break;
		case 0x15:
			convert(3, [](auto p) {
				return float(int32_t(uint32_t(p[2]) << 8u | uint32_t(p[1]) << 16u |
				                     uint32_t(p[0]) << 24u)) *
				       0x1p-31f;
			});
			break;
		case 0x16: convert(4, [](auto p) { return float(Ngs2Load<int32_t>(p)) * 0x1p-31f; }); break;
		case 0x17:
			convert(4, [](auto p) {
				return float(int32_t(Ngs2LoadBigEndian<uint32_t>(p))) * 0x1p-31f;
			});
			break;
		case 0x18: std::memcpy(out, data, count * sizeof(float)); break;
		case 0x19:
			convert(4, [](auto p) { return std::bit_cast<float>(Ngs2LoadBigEndian<uint32_t>(p)); });
			break;
		case 0x1a: convert(8, [](auto p) { return float(Ngs2Load<double>(p)); }); break;
		case 0x1b:
			convert(8, [](auto p) {
				return float(std::bit_cast<double>(Ngs2LoadBigEndian<uint64_t>(p)));
			});
			break;
	}
}

// Gathers one frame of `size` bytes that starts in the front block's tail and continues from the
// start of the following blocks; a block without bytes for it is passed over. Returns the bytes
// gathered, fewer than `size` until enough blocks are queued, or INCOMPLETE_WAVEFORM_DATA when a
// block that would contribute has samples of its own.
static int Ngs2GatherFrame(const Ngs2VoiceInternal& voice, uint32_t size, uint8_t* out) {
	uint32_t gathered = 0;
	for (size_t i = 0; i < voice.blocks.size() && gathered < size; ++i) {
		const auto&  block = voice.blocks[i];
		const size_t start = i == 0 ? block.data_cursor : 0;
		const auto   bytes =
		    static_cast<uint32_t>(std::min<size_t>(size - gathered, block.info.data_size - start));
		if (bytes == 0) {
			continue;
		}
		if (i != 0 && !Ngs2IsDataOnly(block.info)) {
			return NGS2_ERROR_INCOMPLETE_WAVEFORM_DATA;
		}
		std::memcpy(out + gathered, block.data + start, bytes);
		gathered += bytes;
	}
	return static_cast<int>(gathered);
}

// The front block becoming current starts the extent of its samples; outside any extent it is
// dropped, whatever its repeats.
static void Ngs2EnterBlock(Ngs2VoiceInternal& voice) {
	auto& block = voice.blocks.front();
	if (!Ngs2IsDataOnly(block.info) && block.data_cursor == 0) {
		// PCM plays the skip prefix; the decoder drops it.
		voice.duration_remaining =
		    block.info.num_samples + (voice.decoder == nullptr ? block.info.num_skip_samples : 0);
		voice.skip_remaining = block.info.num_skip_samples;
	}
	if (voice.duration_remaining == 0) {
		block.info.num_repeats = 0;
	}
}

// Decodes the next frame of the front blocks into decoded_frame and frame_samples, which the
// caller has emptied.
static bool Ngs2DecodeFrame(Ngs2VoiceInternal& voice) {
	auto& frame = voice.decoded_frame;
	while (voice.state == Ngs2VoicePlayState::Playing && !voice.blocks.empty()) {
		Ngs2EnterBlock(voice);
		if (voice.duration_remaining == 0) {
			if (!Ngs2FinishBlock(voice)) {
				return false;
			}
			continue;
		}
		auto& block = voice.blocks.front();
		if (voice.decoder == nullptr) {
			// PCM has no encoded frame size, so stage at most one system grain.
			const auto  quantum     = std::max(voice.rack->ngs->option.num_grain_samples, 1u);
			const auto  frame_bytes = voice.channels * Ngs2PcmSampleBytes(voice.waveform_type);
			const auto  left        = block.info.data_size - block.data_cursor;
			const auto* data        = block.data + block.data_cursor;
			auto count = std::min<size_t>({left / frame_bytes, voice.duration_remaining, quantum});
			std::array<uint8_t, 8 * sizeof(double)> straddle;
			if (left != 0 && left < frame_bytes) {
				// A frame that straddles the block's end continues in the following blocks; the
				// voice starves until they are queued.
				EXIT_NOT_IMPLEMENTED(Ngs2LoopsAgain(block) || frame_bytes > straddle.size());
				const int gathered = Ngs2GatherFrame(voice, frame_bytes, straddle.data());
				voice.incomplete_frame |= gathered == NGS2_ERROR_INCOMPLETE_WAVEFORM_DATA;
				if (gathered != static_cast<int>(frame_bytes)) {
					return false;
				}
				data  = straddle.data();
				count = 1;
			}
			frame.resize(size_t(quantum) * voice.channels);
			Ngs2ConvertPcm(voice.waveform_type, data, count * voice.channels, frame.data());
			// The skip prefix does not count as decoded.
			const auto skipped = std::min<size_t>(voice.skip_remaining, count);
			voice.skip_remaining -= skipped;
			voice.duration_remaining -= count;
			voice.decoded_bytes += count * frame_bytes;
			voice.decoded_samples += count - skipped;
			voice.frame_samples = static_cast<uint32_t>(count);
			// The frames' bytes are consumed across the blocks they came from.
			for (auto consumed = count * frame_bytes;;) {
				auto&      front = voice.blocks.front();
				const auto bytes = std::min(consumed, front.info.data_size - front.data_cursor);
				front.data_cursor += bytes;
				consumed -= bytes;
				if (voice.duration_remaining == 0 || front.data_cursor == front.info.data_size) {
					// A repeating block restarts its duration, so it must hold all of it.
					EXIT_NOT_IMPLEMENTED(voice.duration_remaining != 0 && Ngs2LoopsAgain(front));
					if (!Ngs2FinishBlock(voice)) {
						return false;
					}
				}
				if (consumed == 0 || voice.blocks.empty()) {
					break;
				}
				// The finished block's successor holds the frame's other bytes.
				Ngs2EnterBlock(voice);
			}
			if (count != 0) {
				return true;
			}
			continue;
		}
		const auto skip =
		    std::min(voice.skip_remaining,
		             static_cast<uint32_t>(frame.size() / voice.channels));
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
		    frame.data(), frame.size() * sizeof(float), false,
		    &gapless);
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
			voice.frame_samples = decoded_samples;
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
		if (!Ngs2DecodeFrame(voice)) {
			return false;
		}
	}
	return voice.state == Ngs2VoicePlayState::Playing &&
	       voice.frame_cursor < voice.frame_samples;
}

// Phases 0 to 256 of the 512 phases of 4 taps, a Hann-windowed sinc normalized to unit gain.
// Phase 512 - p holds the taps of phase p in reverse order.
static constexpr float NGS2_RESAMPLER_HALF_TAPS[257][4] = {
    {0x0p+0f, 0x1p+0f, 0x0p+0f, -0x0p+0f},
    {-0x1.fd6e3p-11f, 0x1.fffeb8p-1f, 0x1.014938p-10f, -0x1.3c1c0ep-29f},
    {-0x1.fadd04p-10f, 0x1.fffadcp-1f, 0x1.0292bcp-9f, -0x1.3c1c0ep-26f},
    {-0x1.7a395ep-9f, 0x1.fff47p-1f, 0x1.85cacep-9f, -0x1.0b25ap-24f},
    {-0x1.f5bca6p-9f, 0x1.ffeb74p-1f, 0x1.0526ap-8f, -0x1.3cf7f6p-23f},
    {-0x1.37fc7p-8f, 0x1.ffdfe8p-1f, 0x1.480d3ep-8f, -0x1.35e1c4p-22f},
    {-0x1.74774ep-8f, 0x1.ffd1ccp-1f, 0x1.8b9972p-8f, -0x1.0bf3c8p-21f},
    {-0x1.b04f34p-8f, 0x1.ffc122p-1f, 0x1.cfcb6ap-8f, -0x1.a9e652p-21f},
    {-0x1.eb847p-8f, 0x1.ffadeap-1f, 0x1.0a51aap-7f, -0x1.3e2652p-20f},
    {-0x1.130ba6p-7f, 0x1.ff9824p-1f, 0x1.2d10bp-7f, -0x1.c55c54p-20f},
    {-0x1.30040ep-7f, 0x1.ff7fd2p-1f, 0x1.5022d8p-7f, -0x1.3735ecp-19f},
    {-0x1.4cab9ap-7f, 0x1.ff64f6p-1f, 0x1.738838p-7f, -0x1.9e8eecp-19f},
    {-0x1.690272p-7f, 0x1.ff478cp-1f, 0x1.9740e2p-7f, -0x1.0d524p-18f},
    {-0x1.8508c4p-7f, 0x1.ff279ap-1f, 0x1.bb4ce8p-7f, -0x1.56af48p-18f},
    {-0x1.a0bebcp-7f, 0x1.ff052p-1f, 0x1.dfac5ap-7f, -0x1.ac55f4p-18f},
    {-0x1.bc2488p-7f, 0x1.fee01cp-1f, 0x1.022fa4p-6f, -0x1.079d1ap-17f},
    {-0x1.d73a58p-7f, 0x1.feb892p-1f, 0x1.14b2dep-6f, -0x1.402a94p-17f},
    {-0x1.f2005cp-7f, 0x1.fe8e82p-1f, 0x1.275fe6p-6f, -0x1.804e3ep-17f},
    {-0x1.063b62p-6f, 0x1.fe61eep-1f, 0x1.3a36bep-6f, -0x1.c883c8p-17f},
    {-0x1.134ee4p-6f, 0x1.fe32d4p-1f, 0x1.4d376ep-6f, -0x1.0ca3ep-16f},
    {-0x1.203acap-6f, 0x1.fe013ap-1f, 0x1.6061fap-6f, -0x1.398a12p-16f},
    {-0x1.2cff32p-6f, 0x1.fdcd1cp-1f, 0x1.73b668p-6f, -0x1.6b339ap-16f},
    {-0x1.399c36p-6f, 0x1.fd968p-1f, 0x1.8734bap-6f, -0x1.a1de52p-16f},
    {-0x1.4611fp-6f, 0x1.fd5d64p-1f, 0x1.9adcf4p-6f, -0x1.ddc814p-16f},
    {-0x1.52607ep-6f, 0x1.fd21cap-1f, 0x1.aeaf1ap-6f, -0x1.0f975cp-15f},
    {-0x1.5e87fcp-6f, 0x1.fce3b4p-1f, 0x1.c2ab2cp-6f, -0x1.3328bp-15f},
    {-0x1.6a8888p-6f, 0x1.fca322p-1f, 0x1.d6d13p-6f, -0x1.59b6f4p-15f},
    {-0x1.76623ep-6f, 0x1.fc6016p-1f, 0x1.eb2124p-6f, -0x1.83614ap-15f},
    {-0x1.82153cp-6f, 0x1.fc1a92p-1f, 0x1.ff9b0ap-6f, -0x1.b046ap-15f},
    {-0x1.8da1a4p-6f, 0x1.fbd298p-1f, 0x1.0a1f7p-5f, -0x1.e0864ep-15f},
    {-0x1.990794p-6f, 0x1.fb8828p-1f, 0x1.148656p-5f, -0x1.0a1fbcp-14f},
    {-0x1.a44728p-6f, 0x1.fb3b44p-1f, 0x1.1f0234p-5f, -0x1.25c888p-14f},
    {-0x1.af6086p-6f, 0x1.faebeep-1f, 0x1.29930ap-5f, -0x1.434cfcp-14f},
    {-0x1.ba53cap-6f, 0x1.fa9a26p-1f, 0x1.3438d8p-5f, -0x1.62bce6p-14f},
    {-0x1.c5211ap-6f, 0x1.fa45fp-1f, 0x1.3ef39ap-5f, -0x1.842782p-14f},
    {-0x1.cfc894p-6f, 0x1.f9ef4cp-1f, 0x1.49c352p-5f, -0x1.a79c62p-14f},
    {-0x1.da4a5ap-6f, 0x1.f9963cp-1f, 0x1.54a7fep-5f, -0x1.cd2b34p-14f},
    {-0x1.e4a692p-6f, 0x1.f93ac2p-1f, 0x1.5fa19ap-5f, -0x1.f4e31cp-14f},
    {-0x1.eedd5cp-6f, 0x1.f8dcep-1f, 0x1.6ab026p-5f, -0x1.0f69e4p-13f},
    {-0x1.f8eedcp-6f, 0x1.f87c96p-1f, 0x1.75d39ep-5f, -0x1.25863ap-13f},
    {-0x1.016d9cp-5f, 0x1.f819e6p-1f, 0x1.810cp-5f, -0x1.3cce5ap-13f},
    {-0x1.06514ap-5f, 0x1.f7b4d4p-1f, 0x1.8c594ap-5f, -0x1.5549d6p-13f},
    {-0x1.0b228ap-5f, 0x1.f74d62p-1f, 0x1.97bb76p-5f, -0x1.6f0068p-13f},
    {-0x1.0fe17p-5f, 0x1.f6e38ep-1f, 0x1.a33284p-5f, -0x1.89f9b2p-13f},
    {-0x1.148e0ep-5f, 0x1.f6775ep-1f, 0x1.aebe6ep-5f, -0x1.a63d6p-13f},
    {-0x1.192876p-5f, 0x1.f608d2p-1f, 0x1.ba5f3p-5f, -0x1.c3d2f6p-13f},
    {-0x1.1db0bcp-5f, 0x1.f597ecp-1f, 0x1.c614c8p-5f, -0x1.e2c212p-13f},
    {-0x1.2226f4p-5f, 0x1.f524aep-1f, 0x1.d1df3p-5f, -0x1.018924p-12f},
    {-0x1.268b32p-5f, 0x1.f4af1ap-1f, 0x1.ddbe64p-5f, -0x1.126596p-12f},
    {-0x1.2add8cp-5f, 0x1.f43732p-1f, 0x1.e9b25ep-5f, -0x1.23fa18p-12f},
    {-0x1.2f1e12p-5f, 0x1.f3bcf8p-1f, 0x1.f5bb18p-5f, -0x1.364a74p-12f},
    {-0x1.334cdap-5f, 0x1.f3407p-1f, 0x1.00ec48p-4f, -0x1.495a5ep-12f},
    {-0x1.3769fap-5f, 0x1.f2c19ap-1f, 0x1.07055ep-4f, -0x1.5d2d92p-12f},
    {-0x1.3b7584p-5f, 0x1.f24078p-1f, 0x1.0d28cep-4f, -0x1.71c7c4p-12f},
    {-0x1.3f6f9p-5f, 0x1.f1bd0cp-1f, 0x1.135692p-4f, -0x1.872cap-12f},
    {-0x1.43583p-5f, 0x1.f1375ap-1f, 0x1.198ea8p-4f, -0x1.9d5fe4p-12f},
    {-0x1.472f7ep-5f, 0x1.f0af64p-1f, 0x1.1fd10cp-4f, -0x1.b46526p-12f},
    {-0x1.4af58ap-5f, 0x1.f0252ap-1f, 0x1.261dbcp-4f, -0x1.cc4014p-12f},
    {-0x1.4eaa6cp-5f, 0x1.ef98bp-1f, 0x1.2c74b2p-4f, -0x1.e4f456p-12f},
    {-0x1.524e3cp-5f, 0x1.ef09f6p-1f, 0x1.32d5ecp-4f, -0x1.fe857ep-12f},
    {-0x1.55e10cp-5f, 0x1.ee7902p-1f, 0x1.394168p-4f, -0x1.0c7b96p-11f},
    {-0x1.5962f6p-5f, 0x1.ede5d6p-1f, 0x1.3fb71ep-4f, -0x1.1a2674p-11f},
    {-0x1.5cd40ep-5f, 0x1.ed507p-1f, 0x1.46370ep-4f, -0x1.284526p-11f},
    {-0x1.60346ap-5f, 0x1.ecb8d8p-1f, 0x1.4cc13p-4f, -0x1.36d97p-11f},
    {-0x1.638424p-5f, 0x1.ec1f0cp-1f, 0x1.535582p-4f, -0x1.45e516p-11f},
    {-0x1.66c34ep-5f, 0x1.eb831p-1f, 0x1.59f4p-4f, -0x1.5569d2p-11f},
    {-0x1.69f204p-5f, 0x1.eae4e6p-1f, 0x1.609ca4p-4f, -0x1.656964p-11f},
    {-0x1.6d1058p-5f, 0x1.ea4492p-1f, 0x1.674f6ap-4f, -0x1.75e588p-11f},
    {-0x1.701e66p-5f, 0x1.e9a214p-1f, 0x1.6e0c4ep-4f, -0x1.86dffap-11f},
    {-0x1.731c42p-5f, 0x1.e8fd72p-1f, 0x1.74d348p-4f, -0x1.985a64p-11f},
    {-0x1.760a04p-5f, 0x1.e856aap-1f, 0x1.7ba458p-4f, -0x1.aa567cp-11f},
    {-0x1.78e7c6p-5f, 0x1.e7adc4p-1f, 0x1.827f74p-4f, -0x1.bcd5f6p-11f},
    {-0x1.7bb59ep-5f, 0x1.e702bep-1f, 0x1.89649ap-4f, -0x1.cfda76p-11f},
    {-0x1.7e73a4p-5f, 0x1.e6559cp-1f, 0x1.9053c4p-4f, -0x1.e365acp-11f},
    {-0x1.8121fp-5f, 0x1.e5a66p-1f, 0x1.974cecp-4f, -0x1.f7793cp-11f},
    {-0x1.83c09cp-5f, 0x1.e4f50ep-1f, 0x1.9e500ep-4f, -0x1.060b62p-10f},
    {-0x1.864fbep-5f, 0x1.e441a8p-1f, 0x1.a55d2p-4f, -0x1.109ff2p-10f},
    {-0x1.88cf6ep-5f, 0x1.e38c3p-1f, 0x1.ac7422p-4f, -0x1.1b7b1ep-10f},
    {-0x1.8b3fc8p-5f, 0x1.e2d4aap-1f, 0x1.b3950ap-4f, -0x1.269daep-10f},
    {-0x1.8da0ep-5f, 0x1.e21b18p-1f, 0x1.babfd4p-4f, -0x1.320872p-10f},
    {-0x1.8ff2d4p-5f, 0x1.e15f7cp-1f, 0x1.c1f478p-4f, -0x1.3dbc2ep-10f},
    {-0x1.9235b8p-5f, 0x1.e0a1dap-1f, 0x1.c932f2p-4f, -0x1.49b9aap-10f},
    {-0x1.9469a8p-5f, 0x1.dfe234p-1f, 0x1.d07b3ap-4f, -0x1.5601aap-10f},
    {-0x1.968ebcp-5f, 0x1.df208ep-1f, 0x1.d7cd4ap-4f, -0x1.6294f4p-10f},
    {-0x1.98a50ep-5f, 0x1.de5ce8p-1f, 0x1.df291cp-4f, -0x1.6f7446p-10f},
    {-0x1.9aacb6p-5f, 0x1.dd9746p-1f, 0x1.e68ea8p-4f, -0x1.7ca062p-10f},
    {-0x1.9ca5cep-5f, 0x1.dccfacp-1f, 0x1.edfde8p-4f, -0x1.8a1a02p-10f},
    {-0x1.9e907p-5f, 0x1.dc061ep-1f, 0x1.f576d6p-4f, -0x1.97e1e4p-10f},
    {-0x1.a06cb6p-5f, 0x1.db3a9ap-1f, 0x1.fcf96ap-4f, -0x1.a5f8cp-10f},
    {-0x1.a23ab8p-5f, 0x1.da6d28p-1f, 0x1.0242dp-3f, -0x1.b45f4cp-10f},
    {-0x1.a3fa9p-5f, 0x1.d99dc6p-1f, 0x1.060db6p-3f, -0x1.c31642p-10f},
    {-0x1.a5ac58p-5f, 0x1.d8cc7cp-1f, 0x1.09dd64p-3f, -0x1.d21e4ep-10f},
    {-0x1.a7502cp-5f, 0x1.d7f94ap-1f, 0x1.0db1d6p-3f, -0x1.e17828p-10f},
    {-0x1.a8e622p-5f, 0x1.d72432p-1f, 0x1.118b0ap-3f, -0x1.f12478p-10f},
    {-0x1.aa6e56p-5f, 0x1.d64d38p-1f, 0x1.1568fap-3f, -0x1.0091f8p-9f},
    {-0x1.abe8e4p-5f, 0x1.d5746p-1f, 0x1.194ba4p-3f, -0x1.08bb9cp-9f},
    {-0x1.ad55e2p-5f, 0x1.d499acp-1f, 0x1.1d3304p-3f, -0x1.110f7cp-9f},
    {-0x1.aeb56ep-5f, 0x1.d3bd2p-1f, 0x1.211f14p-3f, -0x1.198deap-9f},
    {-0x1.b007ap-5f, 0x1.d2debcp-1f, 0x1.250fd2p-3f, -0x1.22373ap-9f},
    {-0x1.b14c94p-5f, 0x1.d1fe86p-1f, 0x1.290538p-3f, -0x1.2b0bbap-9f},
    {-0x1.b28464p-5f, 0x1.d11c8p-1f, 0x1.2cff44p-3f, -0x1.340bbcp-9f},
    {-0x1.b3af28p-5f, 0x1.d038aep-1f, 0x1.30fdf2p-3f, -0x1.3d378cp-9f},
    {-0x1.b4ccfep-5f, 0x1.cf531p-1f, 0x1.35013cp-3f, -0x1.468f78p-9f},
    {-0x1.b5ddfep-5f, 0x1.ce6bacp-1f, 0x1.39091ep-3f, -0x1.5013ccp-9f},
    {-0x1.b6e244p-5f, 0x1.cd8284p-1f, 0x1.3d1596p-3f, -0x1.59c4d4p-9f},
    {-0x1.b7d9eap-5f, 0x1.cc979ap-1f, 0x1.41269cp-3f, -0x1.63a2d6p-9f},
    {-0x1.b8c50ap-5f, 0x1.cbaaf4p-1f, 0x1.453c2ep-3f, -0x1.6dae1cp-9f},
    {-0x1.b9a3cp-5f, 0x1.cabc9p-1f, 0x1.495648p-3f, -0x1.77e6ecp-9f},
    {-0x1.ba7624p-5f, 0x1.c9cc78p-1f, 0x1.4d74e4p-3f, -0x1.824d8ep-9f},
    {-0x1.bb3c56p-5f, 0x1.c8daa8p-1f, 0x1.5197fcp-3f, -0x1.8ce242p-9f},
    {-0x1.bbf66cp-5f, 0x1.c7e728p-1f, 0x1.55bf9p-3f, -0x1.97a55p-9f},
    {-0x1.bca482p-5f, 0x1.c6f1fap-1f, 0x1.59eb98p-3f, -0x1.a296f4p-9f},
    {-0x1.bd46b4p-5f, 0x1.c5fb1ep-1f, 0x1.5e1c0ep-3f, -0x1.adb774p-9f},
    {-0x1.bddd1ap-5f, 0x1.c5029cp-1f, 0x1.6250f2p-3f, -0x1.b9070ap-9f},
    {-0x1.be67d4p-5f, 0x1.c40874p-1f, 0x1.668a3ap-3f, -0x1.c485f6p-9f},
    {-0x1.bee6f8p-5f, 0x1.c30caap-1f, 0x1.6ac7e6p-3f, -0x1.d03474p-9f},
    {-0x1.bf5aa2p-5f, 0x1.c20f42p-1f, 0x1.6f09eep-3f, -0x1.dc12bep-9f},
    {-0x1.bfc2eep-5f, 0x1.c1103cp-1f, 0x1.73504ep-3f, -0x1.e8210ep-9f},
    {-0x1.c01ff6p-5f, 0x1.c00fap-1f, 0x1.779bp-3f, -0x1.f45f9ep-9f},
    {-0x1.c071d4p-5f, 0x1.bf0d6cp-1f, 0x1.7beap-3f, -0x1.00675p-8f},
    {-0x1.c0b8a6p-5f, 0x1.be09a6p-1f, 0x1.803d48p-3f, -0x1.06b726p-8f},
    {-0x1.c0f484p-5f, 0x1.bd0452p-1f, 0x1.8494d6p-3f, -0x1.0d1f6cp-8f},
    {-0x1.c1258ap-5f, 0x1.bbfd7p-1f, 0x1.88f0ap-3f, -0x1.13a03ap-8f},
    {-0x1.c14bd4p-5f, 0x1.baf508p-1f, 0x1.8d50a6p-3f, -0x1.1a39a8p-8f},
    {-0x1.c1677ap-5f, 0x1.b9eb18p-1f, 0x1.91b4dep-3f, -0x1.20ebcep-8f},
    {-0x1.c1789ap-5f, 0x1.b8dfa6p-1f, 0x1.961d46p-3f, -0x1.27b6c2p-8f},
    {-0x1.c17f4cp-5f, 0x1.b7d2b4p-1f, 0x1.9a89d6p-3f, -0x1.2e9a9cp-8f},
    {-0x1.c17bacp-5f, 0x1.b6c446p-1f, 0x1.9efa8cp-3f, -0x1.35977p-8f},
    {-0x1.c16dd6p-5f, 0x1.b5b46p-1f, 0x1.a36f6p-3f, -0x1.3cad54p-8f},
    {-0x1.c155e4p-5f, 0x1.b4a304p-1f, 0x1.a7e84cp-3f, -0x1.43dc5ap-8f},
    {-0x1.c133fp-5f, 0x1.b39034p-1f, 0x1.ac654ep-3f, -0x1.4b2498p-8f},
    {-0x1.c10814p-5f, 0x1.b27bf6p-1f, 0x1.b0e65cp-3f, -0x1.52861ep-8f},
    {-0x1.c0d26ep-5f, 0x1.b1664cp-1f, 0x1.b56b74p-3f, -0x1.5a00fep-8f},
    {-0x1.c09316p-5f, 0x1.b04f38p-1f, 0x1.b9f48ep-3f, -0x1.61954ap-8f},
    {-0x1.c04a28p-5f, 0x1.af36bep-1f, 0x1.be81a6p-3f, -0x1.69431p-8f},
    {-0x1.bff7bep-5f, 0x1.ae1ce4p-1f, 0x1.c312b6p-3f, -0x1.710a6p-8f},
    {-0x1.bf9bf4p-5f, 0x1.ad01a8p-1f, 0x1.c7a7b8p-3f, -0x1.78eb4ap-8f},
    {-0x1.bf36e2p-5f, 0x1.abe51p-1f, 0x1.cc40a6p-3f, -0x1.80e5dap-8f},
    {-0x1.bec8a4p-5f, 0x1.aac72p-1f, 0x1.d0dd78p-3f, -0x1.88fa1cp-8f},
    {-0x1.be5154p-5f, 0x1.a9a7dap-1f, 0x1.d57e2cp-3f, -0x1.91281ep-8f},
    {-0x1.bdd10ep-5f, 0x1.a88742p-1f, 0x1.da22bap-3f, -0x1.996feap-8f},
    {-0x1.bd47ecp-5f, 0x1.a7655ap-1f, 0x1.decb1ep-3f, -0x1.a1d18ap-8f},
    {-0x1.bcb606p-5f, 0x1.a64226p-1f, 0x1.e3774ep-3f, -0x1.aa4d0ap-8f},
    {-0x1.bc1b7ap-5f, 0x1.a51daap-1f, 0x1.e82748p-3f, -0x1.b2e26ep-8f},
    {-0x1.bb785ep-5f, 0x1.a3f7e8p-1f, 0x1.ecdb04p-3f, -0x1.bb91c2p-8f},
    {-0x1.baccdp-5f, 0x1.a2d0e4p-1f, 0x1.f1927cp-3f, -0x1.c45b0cp-8f},
    {-0x1.ba18e8p-5f, 0x1.a1a8ap-1f, 0x1.f64daap-3f, -0x1.cd3e52p-8f},
    {-0x1.b95cc2p-5f, 0x1.a07f22p-1f, 0x1.fb0c88p-3f, -0x1.d63b98p-8f},
    {-0x1.b89876p-5f, 0x1.9f546ap-1f, 0x1.ffcf1p-3f, -0x1.df52e4p-8f},
    {-0x1.b7cc2p-5f, 0x1.9e287cp-1f, 0x1.024a9ep-2f, -0x1.e8843ap-8f},
    {-0x1.b6f7d8p-5f, 0x1.9cfb5cp-1f, 0x1.04af82p-2f, -0x1.f1cf9ap-8f},
    {-0x1.b61bbap-5f, 0x1.9bcd0ep-1f, 0x1.07163p-2f, -0x1.fb3508p-8f},
    {-0x1.b537dep-5f, 0x1.9a9d92p-1f, 0x1.097ea8p-2f, -0x1.025a42p-7f},
    {-0x1.b44c5ep-5f, 0x1.996cfp-1f, 0x1.0be8e4p-2f, -0x1.072708p-7f},
    {-0x1.b35956p-5f, 0x1.983b28p-1f, 0x1.0e54e2p-2f, -0x1.0c00d4p-7f},
    {-0x1.b25edcp-5f, 0x1.97083cp-1f, 0x1.10c29ep-2f, -0x1.10e7a6p-7f},
    {-0x1.b15d0cp-5f, 0x1.95d434p-1f, 0x1.133216p-2f, -0x1.15db7ep-7f},
    {-0x1.b054p-5f, 0x1.949f0ep-1f, 0x1.15a346p-2f, -0x1.1adc58p-7f},
    {-0x1.af43dp-5f, 0x1.9368d2p-1f, 0x1.18162ap-2f, -0x1.1fea32p-7f},
    {-0x1.ae2c94p-5f, 0x1.92317ep-1f, 0x1.1a8abep-2f, -0x1.25050cp-7f},
    {-0x1.ad0e68p-5f, 0x1.90f91ap-1f, 0x1.1d01p-2f, -0x1.2a2cep-7f},
    {-0x1.abe964p-5f, 0x1.8fbfa6p-1f, 0x1.1f78eep-2f, -0x1.2f61aap-7f},
    {-0x1.aabda2p-5f, 0x1.8e8526p-1f, 0x1.21f282p-2f, -0x1.34a36ap-7f},
    {-0x1.a98b3ap-5f, 0x1.8d499ep-1f, 0x1.246dbap-2f, -0x1.39f216p-7f},
    {-0x1.a85246p-5f, 0x1.8c0d12p-1f, 0x1.26ea92p-2f, -0x1.3f4daep-7f},
    {-0x1.a712dcp-5f, 0x1.8acf82p-1f, 0x1.296908p-2f, -0x1.44b62ap-7f},
    {-0x1.a5cd1ap-5f, 0x1.8990f6p-1f, 0x1.2be916p-2f, -0x1.4a2b86p-7f},
    {-0x1.a48114p-5f, 0x1.88516cp-1f, 0x1.2e6abap-2f, -0x1.4fadb8p-7f},
    {-0x1.a32ee4p-5f, 0x1.8710eap-1f, 0x1.30edfp-2f, -0x1.553cbep-7f},
    {-0x1.a1d6a4p-5f, 0x1.85cf72p-1f, 0x1.3372b4p-2f, -0x1.5ad88cp-7f},
    {-0x1.a0786ap-5f, 0x1.848d08p-1f, 0x1.35f904p-2f, -0x1.60811ep-7f},
    {-0x1.9f145p-5f, 0x1.8349bp-1f, 0x1.3880dcp-2f, -0x1.66366ap-7f},
    {-0x1.9daa6ep-5f, 0x1.82056cp-1f, 0x1.3b0a38p-2f, -0x1.6bf866p-7f},
    {-0x1.9c3adcp-5f, 0x1.80c04p-1f, 0x1.3d9516p-2f, -0x1.71c70cp-7f},
    {-0x1.9ac5b2p-5f, 0x1.7f7a2ep-1f, 0x1.40216ep-2f, -0x1.77a25p-7f},
    {-0x1.994b0ap-5f, 0x1.7e3338p-1f, 0x1.42af42p-2f, -0x1.7d8a28p-7f},
    {-0x1.97caf8p-5f, 0x1.7ceb64p-1f, 0x1.453e8ap-2f, -0x1.837e8ap-7f},
    {-0x1.964596p-5f, 0x1.7ba2b6p-1f, 0x1.47cf44p-2f, -0x1.897f6ap-7f},
    {-0x1.94bafcp-5f, 0x1.7a592cp-1f, 0x1.4a616cp-2f, -0x1.8f8cbep-7f},
    {-0x1.932b42p-5f, 0x1.790ecep-1f, 0x1.4cf5p-2f, -0x1.95a67ap-7f},
    {-0x1.91967cp-5f, 0x1.77c39ep-1f, 0x1.4f89fap-2f, -0x1.9bcc8ep-7f},
    {-0x1.8ffcc6p-5f, 0x1.76779ep-1f, 0x1.522056p-2f, -0x1.a1fefp-7f},
    {-0x1.8e5e36p-5f, 0x1.752adp-1f, 0x1.54b812p-2f, -0x1.a83d92p-7f},
    {-0x1.8cbaep-5f, 0x1.73dd3ap-1f, 0x1.57512ap-2f, -0x1.ae8866p-7f},
    {-0x1.8b12dep-5f, 0x1.728edep-1f, 0x1.59eb9ap-2f, -0x1.b4df5cp-7f},
    {-0x1.896648p-5f, 0x1.713fcp-1f, 0x1.5c875ep-2f, -0x1.bb4266p-7f},
    {-0x1.87b532p-5f, 0x1.6fefep-1f, 0x1.5f2472p-2f, -0x1.c1b174p-7f},
    {-0x1.85ffb4p-5f, 0x1.6e9f44p-1f, 0x1.61c2dp-2f, -0x1.c82c76p-7f},
    {-0x1.8445e4p-5f, 0x1.6d4dfp-1f, 0x1.64627ap-2f, -0x1.ceb35cp-7f},
    {-0x1.8287dcp-5f, 0x1.6bfbe4p-1f, 0x1.670366p-2f, -0x1.d54614p-7f},
    {-0x1.80c5acp-5f, 0x1.6aa924p-1f, 0x1.69a594p-2f, -0x1.dbe48ep-7f},
    {-0x1.7eff7p-5f, 0x1.6955b4p-1f, 0x1.6c48fep-2f, -0x1.e28eb6p-7f},
    {-0x1.7d353cp-5f, 0x1.680196p-1f, 0x1.6eedap-2f, -0x1.e9447ap-7f},
    {-0x1.7b6726p-5f, 0x1.66accep-1f, 0x1.719378p-2f, -0x1.f005c6p-7f},
    {-0x1.799544p-5f, 0x1.65575ep-1f, 0x1.743a8p-2f, -0x1.f6d288p-7f},
    {-0x1.77bfaap-5f, 0x1.64014ap-1f, 0x1.76e2b6p-2f, -0x1.fdaaaap-7f},
    {-0x1.75e672p-5f, 0x1.62aa96p-1f, 0x1.798c14p-2f, -0x1.02470ep-6f},
    {-0x1.7409aep-5f, 0x1.615342p-1f, 0x1.7c3696p-2f, -0x1.05be6p-6f},
    {-0x1.722976p-5f, 0x1.5ffb54p-1f, 0x1.7ee23ap-2f, -0x1.093b44p-6f},
    {-0x1.7045dep-5f, 0x1.5ea2cep-1f, 0x1.818efap-2f, -0x1.0cbdaep-6f},
    {-0x1.6e5efap-5f, 0x1.5d49b2p-1f, 0x1.843cd2p-2f, -0x1.104592p-6f},
    {-0x1.6c74e2p-5f, 0x1.5bf006p-1f, 0x1.86ebcp-2f, -0x1.13d2e4p-6f},
    {-0x1.6a87a8p-5f, 0x1.5a95c8p-1f, 0x1.899bbep-2f, -0x1.17659ap-6f},
    {-0x1.689764p-5f, 0x1.593bp-1f, 0x1.8c4cc8p-2f, -0x1.1afda4p-6f},
    {-0x1.66a428p-5f, 0x1.57dfaep-1f, 0x1.8efedap-2f, -0x1.1e9afap-6f},
    {-0x1.64ae0ap-5f, 0x1.5683d4p-1f, 0x1.91b1fp-2f, -0x1.223d8cp-6f},
    {-0x1.62b51ep-5f, 0x1.55277ap-1f, 0x1.946606p-2f, -0x1.25e55p-6f},
    {-0x1.60b978p-5f, 0x1.53ca9ep-1f, 0x1.971b18p-2f, -0x1.299236p-6f},
    {-0x1.5ebb2ep-5f, 0x1.526d44p-1f, 0x1.99d12p-2f, -0x1.2d4432p-6f},
    {-0x1.5cba54p-5f, 0x1.510f7p-1f, 0x1.9c881cp-2f, -0x1.30fb36p-6f},
    {-0x1.5ab6fcp-5f, 0x1.4fb126p-1f, 0x1.9f4008p-2f, -0x1.34b734p-6f},
    {-0x1.58b13ap-5f, 0x1.4e5266p-1f, 0x1.a1f8dcp-2f, -0x1.38781ep-6f},
    {-0x1.56a924p-5f, 0x1.4cf336p-1f, 0x1.a4b298p-2f, -0x1.3c3de4p-6f},
    {-0x1.549eccp-5f, 0x1.4b9396p-1f, 0x1.a76d36p-2f, -0x1.400878p-6f},
    {-0x1.529246p-5f, 0x1.4a338ap-1f, 0x1.aa28bp-2f, -0x1.43d7ccp-6f},
    {-0x1.5083a4p-5f, 0x1.48d316p-1f, 0x1.ace506p-2f, -0x1.47abdp-6f},
    {-0x1.4e72fcp-5f, 0x1.47723cp-1f, 0x1.afa22ep-2f, -0x1.4b8474p-6f},
    {-0x1.4c605ep-5f, 0x1.4610fep-1f, 0x1.b2602ap-2f, -0x1.4f61a8p-6f},
    {-0x1.4a4bep-5f, 0x1.44af62p-1f, 0x1.b51efp-2f, -0x1.53435ap-6f},
    {-0x1.483592p-5f, 0x1.434d66p-1f, 0x1.b7de7ep-2f, -0x1.57297ep-6f},
    {-0x1.461d8ap-5f, 0x1.41eb12p-1f, 0x1.ba9ecep-2f, -0x1.5b14p-6f},
    {-0x1.4403d8p-5f, 0x1.408864p-1f, 0x1.bd5fep-2f, -0x1.5f02dp-6f},
    {-0x1.41e88ep-5f, 0x1.3f2562p-1f, 0x1.c021aap-2f, -0x1.62f5dcp-6f},
    {-0x1.3fcbcp-5f, 0x1.3dc21p-1f, 0x1.c2e42ap-2f, -0x1.66ed14p-6f},
    {-0x1.3dad8p-5f, 0x1.3c5e6cp-1f, 0x1.c5a75cp-2f, -0x1.6ae864p-6f},
    {-0x1.3b8ddep-5f, 0x1.3afa7ep-1f, 0x1.c86b3cp-2f, -0x1.6ee7bcp-6f},
    {-0x1.396cfp-5f, 0x1.399646p-1f, 0x1.cb2fc4p-2f, -0x1.72eb08p-6f},
    {-0x1.374ac4p-5f, 0x1.3831c6p-1f, 0x1.cdf4eep-2f, -0x1.76f238p-6f},
    {-0x1.35276ep-5f, 0x1.36cd04p-1f, 0x1.d0babap-2f, -0x1.7afd36p-6f},
    {-0x1.3302fep-5f, 0x1.3568p-1f, 0x1.d3811ep-2f, -0x1.7f0bfp-6f},
    {-0x1.30dd84p-5f, 0x1.3402bep-1f, 0x1.d6481ap-2f, -0x1.831e52p-6f},
    {-0x1.2eb716p-5f, 0x1.329d4p-1f, 0x1.d90fa6p-2f, -0x1.873448p-6f},
    {-0x1.2c8fcp-5f, 0x1.31378ap-1f, 0x1.dbd7cp-2f, -0x1.8b4dcp-6f},
    {-0x1.2a6796p-5f, 0x1.2fd19ep-1f, 0x1.dea062p-2f, -0x1.8f6aa6p-6f},
    {-0x1.283ea8p-5f, 0x1.2e6b7ep-1f, 0x1.e16988p-2f, -0x1.938ae2p-6f},
    {-0x1.261508p-5f, 0x1.2d052ep-1f, 0x1.e4332cp-2f, -0x1.97ae62p-6f},
    {-0x1.23eac4p-5f, 0x1.2b9ebp-1f, 0x1.e6fd4ap-2f, -0x1.9bd51p-6f},
    {-0x1.21bfeep-5f, 0x1.2a3808p-1f, 0x1.e9c7dcp-2f, -0x1.9ffed6p-6f},
    {-0x1.1f9496p-5f, 0x1.28d136p-1f, 0x1.ec92e2p-2f, -0x1.a42b9ep-6f},
    {-0x1.1d68ccp-5f, 0x1.276a3ep-1f, 0x1.ef5e52p-2f, -0x1.a85b54p-6f},
    {-0x1.1b3cap-5f, 0x1.260324p-1f, 0x1.f22a28p-2f, -0x1.ac8dep-6f},
    {-0x1.191022p-5f, 0x1.249beap-1f, 0x1.f4f662p-2f, -0x1.b0c32ep-6f},
    {-0x1.16e362p-5f, 0x1.233494p-1f, 0x1.f7c2f8p-2f, -0x1.b4fb24p-6f},
    {-0x1.14b67p-5f, 0x1.21cd2p-1f, 0x1.fa8fe8p-2f, -0x1.b935acp-6f},
    {-0x1.128958p-5f, 0x1.206596p-1f, 0x1.fd5d2ap-2f, -0x1.bd72bp-6f},
    {-0x1.105c2ep-5f, 0x1.1efdf6p-1f, 0x1.00155ep-1f, -0x1.c1b216p-6f},
    {-0x1.0e2fp-5f, 0x1.1d9642p-1f, 0x1.017c4cp-1f, -0x1.c5f3c8p-6f},
    {-0x1.0c01dcp-5f, 0x1.1c2e7ep-1f, 0x1.02e35ep-1f, -0x1.ca37acp-6f},
    {-0x1.09d4dp-5f, 0x1.1ac6acp-1f, 0x1.044a8ep-1f, -0x1.ce7dacp-6f},
    {-0x1.07a7ecp-5f, 0x1.195edp-1f, 0x1.05b1dcp-1f, -0x1.d2c5aep-6f},
    {-0x1.057b4p-5f, 0x1.17f6ecp-1f, 0x1.071946p-1f, -0x1.d70f9ap-6f},
    {-0x1.034ed8p-5f, 0x1.168fp-1f, 0x1.0880c8p-1f, -0x1.db5b54p-6f},
    {-0x1.0122c4p-5f, 0x1.152712p-1f, 0x1.09e86p-1f, -0x1.dfa8c4p-6f},
    {-0x1.fdee22p-6f, 0x1.13bf24p-1f, 0x1.0b500cp-1f, -0x1.e3f7d2p-6f},
    {-0x1.f9979ap-6f, 0x1.125736p-1f, 0x1.0cb7cap-1f, -0x1.e8486p-6f},
    {-0x1.f5420ep-6f, 0x1.10ef4ep-1f, 0x1.0e1f94p-1f, -0x1.ec9a56p-6f},
    {-0x1.f0ed9ap-6f, 0x1.0f876cp-1f, 0x1.0f876cp-1f, -0x1.f0ed9ap-6f},
};

static constexpr auto NGS2_RESAMPLER_TAPS = [] {
	std::array<std::array<float, 4>, 512> taps {};
	for (size_t p = 0; p < taps.size(); ++p) {
		for (size_t k = 0; k < 4; ++k) {
			taps[p][k] = p <= 256 ? NGS2_RESAMPLER_HALF_TAPS[p][k]
			                      : NGS2_RESAMPLER_HALF_TAPS[512 - p][3 - k];
		}
	}
	return taps;
}();

// Fills a window of `advance + 4` samples per channel, the kept history first, and interpolates
// each output between the middle two of 4 window samples, in steps of 1/16384 sample: the output
// lags the source by 3 samples. Returns whether the source ran short of the window.
static bool Ngs2ConsumeSamples(Ngs2VoiceInternal& voice, uint32_t grain) {
	const auto generation = voice.source_generation;
	const auto channels   = voice.channels;
	const auto phase      = voice.resample_phase;
	// The pitch truncates to 1/16384 below 4.0; NaN and negative ratios give 0.
	const float q14 = static_cast<float>(voice.sample_rate) /
	                  static_cast<float>(voice.rack->ngs->option.sample_rate) * voice.pitch_ratio *
	                  16384.0f;
	const uint32_t pitch   = q14 >= 0.0f ? static_cast<uint32_t>(std::min(q14, 65535.0f)) : 0;
	const uint32_t advance = (pitch * grain + phase) >> 14u;
	const uint32_t needed  = advance + 4;
	voice.resample_input.resize(size_t(needed) * channels);
	float*   input = voice.resample_input.data();
	uint32_t got   = voice.history_samples;
	for (uint32_t c = 0; c < channels; ++c) {
		std::copy_n(voice.history.data() + size_t(c) * 4, got, input + size_t(c) * needed);
	}
	voice.incomplete_frame = false;
	while (got < needed && Ngs2CurrentFrame(voice) && voice.source_generation == generation) {
		const auto   count = std::min(needed - got, voice.frame_samples - voice.frame_cursor);
		const float* frame = voice.decoded_frame.data() + size_t(voice.frame_cursor) * channels;
		for (uint32_t c = 0; c < channels; ++c) {
			for (uint32_t i = 0; i < count; ++i) {
				input[size_t(c) * needed + got + i] = frame[size_t(i) * channels + c];
			}
		}
		voice.frame_cursor += count;
		got += count;
	}
	if (voice.source_generation != generation || voice.incomplete_frame) {
		// A callback replaced the source, possibly changing its channel layout, or a straddling
		// frame ran into a block with samples, which drops the history.
		if (voice.source_generation == generation) {
			voice.history_samples = 0;
		}
		voice.ClearSamples(grain);
		return false;
	}
	const bool copy = pitch == 0x4000 && phase == 0;
	for (uint32_t c = 0; c < channels; ++c) {
		float* x = input + size_t(c) * needed;
		// A shortfall plays as zeros.
		std::fill(x + got, x + needed, 0.0f);
		float* out = voice.samples.data() + size_t(c) * grain;
		if (copy) {
			std::copy_n(x + 1, grain, out);
			continue;
		}
		uint32_t position = phase;
		for (uint32_t n = 0; n < grain; ++n, position += pitch) {
			const float* s = x + (position >> 14u);
			const auto&  t = NGS2_RESAMPLER_TAPS[(position >> 5u) & 511u];
			// Every product and sum rounds to float: the result depends on no FMA contraction.
			out[n] = (s[0] * t[0] + s[1] * t[1]) + (s[2] * t[2] + s[3] * t[3]);
		}
	}
	voice.resample_phase  = static_cast<uint16_t>((phase + pitch * grain) & 0x3fffu);
	voice.history_samples = got > advance ? got - advance : 0;
	for (uint32_t c = 0; c < channels; ++c) {
		std::copy_n(input + size_t(c) * needed + advance, voice.history_samples,
		            voice.history.data() + size_t(c) * 4);
	}
	voice.has_samples = got != 0;
	return got < needed;
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
			const bool short_read = Ngs2ConsumeSamples(voice, grain);
			// A playing stream can have no source data but still have a filter tail, at any
			// location. The front filters run before the rear ones, each in index order.
			if (voice.has_samples ||
			    std::ranges::any_of(voice.filters, &Ngs2SamplerFilter::HasTail)) {
				voice.RunFilters(0, voice.samples.data(), grain);
				voice.RunFilters(1, voice.samples.data(), grain);
				voice.has_samples = true;
			}
			// The voice ends in the grain its reader runs short of a closed, empty list; the filter
			// tails end with it.
			if (short_read && voice.state == Ngs2VoicePlayState::Playing && voice.blocks.empty() &&
			    !voice.accepts_blocks) {
				voice.state = Ngs2VoicePlayState::Empty;
			}
		}
		for (auto* source: voices) {
			for (size_t p = 0; p < source->ports.size(); ++p) {
				const auto& port = source->ports[p];
				if (port.dest != &voice) {
					continue;
				}
				Ngs2RenderVoice(*source, voices, grain);
				if (!source->has_samples) {
					continue;
				}
				const auto* input = source->Buffer(source->rack->PortBufferId(p), grain);
				// The pre-send filters of port N (location 2 + N) run on a copy of its send, also
				// when the port is muted.
				const auto location = static_cast<uint32_t>(p + 2);
				if (std::ranges::find(source->filters, location, &Ngs2SamplerFilter::location) !=
				    source->filters.end()) {
					source->mix_samples.assign(input, input + size_t(source->channels) * grain);
					source->RunFilters(location, source->mix_samples.data(), grain);
					input = source->mix_samples.data();
				}
				if (port.volume == 0.0f) {
					continue;
				}
				// Input N of a custom rack voice feeds its buffer N.
				EXIT_NOT_IMPLEMENTED(port.input >= voice.rack->NumBuffers());
				auto* output = voice.Buffer(port.input, grain);
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
		if (voice.has_samples) {
			const auto& custom = voice.rack->option.custom_sampler.custom_rack_option;
			const auto  rate   = voice.rack->ngs->option.sample_rate;
			for (size_t m = 0; m < voice.modules.size(); ++m) {
				const auto& info   = custom.module[m];
				auto&       module = voice.modules[m];
				if (info.module_id == 0x17) {
					voice.MixChannels(module.channel_matrix, info.source_buffer_id, grain);
					continue;
				}
				if (info.module_id != 0x19 && info.module_id != 0x1f) {
					// A custom mastering voice's Output module is written after its chain.
					const bool output = info.module_id == 0x1a &&
					                    voice.rack->type == Ngs2RackType::CustomMastering;
					if (!output && !module.render_warned) {
						Log::WriteToConsoleAndLog(fmt::sprintf(
						    "warning: NGS2 custom module 0x%02x at index %zu is ignored during render\n",
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

// A sample clamped to -1..1, scaled by 32767 and truncated; NaN gives 0.
static int16_t Ngs2ToInt16(float sample) {
	return std::isnan(sample) ? int16_t {0}
	                          : static_cast<int16_t>(std::clamp(sample, -1.0f, 1.0f) * 32767.0f);
}

// The render buffers of one render; bit i of `written` is set once a voice wrote buffer i.
struct Ngs2RenderBuffers {
	const Ngs2RenderBufferInfo* info    = nullptr;
	uint32_t                    count   = 0;
	uint32_t                    written = 0;
};

// Writes the voice's first channels of `samples`, as many as both have, into render buffer
// output_id, zero in its other channels, over one grain of frames. An int16 buffer takes this
// voice's grain alone; a float buffer takes the first writer's and adds the later ones. A
// big-endian buffer counts as written but keeps its bytes. An output_id past the render buffers
// writes nothing.
static void Ngs2MixOutput(const Ngs2VoiceInternal& voice, const float* samples,
                          Ngs2RenderBuffers& buffers, uint32_t grain) {
	if (voice.output_id >= buffers.count) {
		return;
	}
	const auto&    output = buffers.info[voice.output_id];
	const uint32_t bit    = 1u << voice.output_id;
	const bool     first  = (buffers.written & bit) == 0;
	buffers.written |= bit;
	const uint32_t frame    = output.num_channels;
	const uint32_t channels = std::min(voice.channels, frame);
	if (output.waveform_type == 0x12) {
		auto* pcm = static_cast<int16_t*>(output.buffer);
		std::fill_n(pcm, size_t(grain) * frame, int16_t {0});
		for (uint32_t c = 0; c < channels; ++c) {
			for (uint32_t i = 0; i < grain; ++i) {
				pcm[i * frame + c] = Ngs2ToInt16(samples[c * grain + i]);
			}
		}
		return;
	}
	if (output.waveform_type != 0x18) {
		return;
	}
	auto* pcm = static_cast<float*>(output.buffer);
	if (first) {
		std::fill_n(pcm, size_t(grain) * frame, 0.0f);
	}
	for (uint32_t c = 0; c < channels; ++c) {
		for (uint32_t i = 0; i < grain; ++i) {
			auto& out = pcm[i * frame + c];
			out       = first ? samples[c * grain + i] : out + samples[c * grain + i];
		}
	}
}

// The rate the LFE filter is designed at for a system rate: the next listed rate, the 88200 design
// up to 96000, and the 11025 design above 192000.
static uint32_t Ngs2MasteringLfeDesignRate(uint32_t rate) {
	constexpr std::array<uint32_t, 9> limits = {11025, 12000, 22050,  24000, 44100,
	                                            48000, 96000, 176400, 192000};
	const auto                        limit  = std::ranges::lower_bound(limits, rate);
	if (limit == limits.end()) {
		return 11025;
	}
	return *limit == 96000 ? 88200 : *limit;
}

// Sets the LFE sections to the 4th-order 0.1 dB Chebyshev low pass at `cutoff` Hz, designed at the
// system rate's design rate.
static void Ngs2SetMasteringLfe(Ngs2VoiceInternal::Mastering& mastering, uint32_t cutoff,
                                uint32_t system_rate) {
	constexpr double ripple_db = 0.1;
	const double     epsilon   = std::sqrt(std::pow(10.0, ripple_db / 10.0) - 1.0);
	const double     mu        = std::asinh(1.0 / epsilon) / 4.0;
	const double     warped =
	    std::tan(std::numbers::pi * cutoff / Ngs2MasteringLfeDesignRate(system_rate));
	double gain = std::pow(10.0, -ripple_db / 20.0);
	for (size_t i = 0; i < mastering.lfe.size(); ++i) {
		// The bilinear transform of the prewarped analog pole re + j im.
		const double angle  = (i == 0 ? 3.0 : 1.0) * std::numbers::pi / 8.0;
		const double re     = -warped * std::sinh(mu) * std::sin(angle);
		const double im     = warped * std::cosh(mu) * std::cos(angle);
		const double d      = (1.0 - re) * (1.0 - re) + im * im;
		mastering.lfe[i].a1 = static_cast<float>(2.0 * (1.0 - re * re - im * im) / d);
		mastering.lfe[i].a2 = static_cast<float>(-((1.0 + re) * (1.0 + re) + im * im) / d);
		gain *= (re * re + im * im) / d;
	}
	// The first section carries the gain of both.
	mastering.lfe[0].gain = static_cast<float>(gain);
	mastering.lfe[1].gain = 1.0f;
}

// The limiter coefficient of a time constant of `time` seconds, counted in whole samples.
static float Ngs2LimiterCoefficient(uint32_t rate, float time) {
	const auto samples = static_cast<uint32_t>(static_cast<float>(rate) * time);
	return samples == 0 ? 0.0f : std::exp(-1.0f / static_cast<float>(samples));
}

// Limits `channels` channels of `grain` samples in place, in blocks of 32 samples.
static void Ngs2Limit(Ngs2VoiceInternal::Mastering& mastering, float* samples, uint32_t channels,
                      uint32_t grain) {
	auto& g = mastering.limiter_gain;
	for (uint32_t start = 0; start < grain; start += 32) {
		const uint32_t length = std::min(grain - start, 32u);
		float          peak   = 0.0f;
		for (uint32_t c = 0; c < channels; ++c) {
			const auto* data = samples + size_t(c) * grain + start;
			for (uint32_t i = 0; i < length; ++i) {
				peak = std::max(peak, std::abs(data[i]));
			}
		}
		const float threshold = mastering.limiter_threshold;
		const float target    = peak > threshold ? threshold / peak : 1.0f;
		const float a         = g > target ? mastering.limiter_attack : mastering.limiter_release;
		// g = a * g + (1 - a) * target per sample, four samples at a time: the first four from the
		// block's start gain, every next four from the previous four. Samples past the last whole
		// four are left as they are.
		const float          a2   = a * a;
		const float          a3   = a2 * a;
		const float          a4   = a2 * a2;
		const float          b    = (1.0f - a) * target;
		const float          c4   = b * (1.0f + a + a2 + a3);
		std::array<float, 4> step = {g * a + b, g * a2 + b * (1.0f + a),
		                             g * a3 + b * (1.0f + a + a2), g * a4 + c4};

		std::array<float, 32> gains;
		const uint32_t        limited = length / 4 * 4;
		for (uint32_t i = 0; i < limited; i += 4) {
			std::copy(step.begin(), step.end(), gains.begin() + i);
			for (auto& gain: step) {
				gain = std::fma(gain, a4, c4);
			}
		}
		for (uint32_t c = 0; c < channels; ++c) {
			auto* data = samples + size_t(c) * grain + start;
			for (uint32_t i = 0; i < limited; ++i) {
				data[i] *= gains[i];
			}
		}
		g = limited == 0 ? step[3] : gains[limited - 1];
		// The gain takes the target once a step no longer moves it.
		if (std::abs(target - g) < 0.01f && target + a * (g - target) == g) {
			g = target;
		}
		mastering.limiter_state = {peak, g};
	}
}

// Writes this grain's level of each channel into the window of peak meter `meter`, and reports
// the window's peak or RMS per channel.
static void Ngs2MeterPeaks(Ngs2VoiceInternal::Mastering& mastering, size_t meter,
                           const float* samples, uint32_t channels, uint32_t grain,
                           uint32_t blocks) {
	const bool peak = mastering.meter_mode == 1;
	for (uint32_t c = 0; c < channels; ++c) {
		const auto* data  = samples + size_t(c) * grain;
		float       level = 0.0f;
		for (uint32_t i = 0; i < grain; ++i) {
			level = peak ? std::max(level, std::abs(data[i])) : level + data[i] * data[i];
		}
		auto* window = mastering.meter_blocks.data() + (size_t(c) * 2 + meter) * blocks;
		window[mastering.meter_index] = level;
		auto* end                     = window + mastering.meter_count;
		mastering.meter_peaks[meter][c] =
		    peak ? *std::max_element(window, end)
			     : std::sqrt(std::accumulate(window, end, 0.0f) /
			                 static_cast<float>(mastering.meter_count * grain));
	}
}

// Runs a playing mastering voice's input peak meter, channel converter, LFE filter, limiter, gain,
// output peak meter and output in place on its N input channels.
static void Ngs2RenderMastering(Ngs2VoiceInternal& voice, Ngs2RenderBuffers& buffers,
                                uint32_t grain) {
	auto&          mastering = *voice.mastering;
	const uint32_t inputs    = voice.channels;
	const uint32_t blocks    = voice.rack->option.mastering.num_peak_meter_blocks;
	const uint32_t mode      = mastering.meters_enabled ? mastering.meter_detect_type : 0;
	if (mastering.meter_mode != mode) {
		mastering.meter_mode  = mode;
		mastering.meter_index = 0;
		mastering.meter_count = 1;
	}
	const bool metering = mastering.meter_mode != 0;
	if (metering) {
		Ngs2MeterPeaks(mastering, 0, voice.samples.data(), inputs, grain, blocks);
	}
	// The converter writes the output buffer's channels, at most N, and zeroes the rest.
	const uint32_t outputs = std::min(inputs, voice.output_id < buffers.count
	                                              ? buffers.info[voice.output_id].num_channels
	                                              : inputs);
	const float*   levels  = mastering.levels.data() + Ngs2MasteringLevelsOffset(inputs, outputs);
	// Levels ramp from the previous grain's unless the shape changed or the voice was played.
	const float* start =
	    mastering.previous_inputs == inputs && mastering.previous_outputs == outputs
	        ? mastering.previous_levels.data()
	        : levels;
	// Every output reads all inputs, so the converter mixes from the inputs swapped into
	// mix_samples.
	std::swap(voice.samples, voice.mix_samples);
	voice.samples.assign(voice.mix_samples.size(), 0.0f);
	for (uint32_t d = 0; d < outputs; ++d) {
		auto* out = voice.samples.data() + size_t(d) * grain;
		for (uint32_t s = 0; s < inputs; ++s) {
			const float from = start[d * inputs + s];
			const float to   = levels[d * inputs + s];
			// A level that is not positive at either end contributes nothing.
			if (!(from > 0.0f || to > 0.0f)) {
				continue;
			}
			const auto* in   = voice.mix_samples.data() + size_t(s) * grain;
			const float step = (to - from) / static_cast<float>(grain);
			for (uint32_t i = 0; i < grain; ++i) {
				out[i] += in[i] * (from + static_cast<float>(i) * step);
			}
		}
	}
	std::copy_n(levels, size_t(inputs) * outputs, mastering.previous_levels.begin());
	mastering.previous_inputs  = inputs;
	mastering.previous_outputs = outputs;

	// The LFE filter runs in place on channel 3, which voices of fewer than 4 channels lack; a
	// grain without the filter zeroes its history.
	if (!mastering.lfe_enabled) {
		mastering.lfe[0].history = mastering.lfe[1].history = {};
	} else if (mastering.lfe_enable_flag && inputs >= 4) {
		auto* samples = voice.samples.data() + size_t(3) * grain;
		for (auto& section: mastering.lfe) {
			auto [gain, a1, a2, h] = section;
			for (uint32_t i = 0; i < grain; ++i) {
				const float x = samples[i];
				const float y = gain * x + 2.0f * gain * h.x1 + gain * h.x2 + a1 * h.y1 + a2 * h.y2;
				h             = {x, h.x1, y, h.y1};
				samples[i]    = y;
			}
			section.history = h;
		}
	}

	if (mastering.limiter_enabled && mastering.limiter_enable_flag) {
		Ngs2Limit(mastering, voice.samples.data(), inputs, grain);
	} else {
		mastering.limiter_gain = 1.0f;
		if (mastering.limiter_enabled) {
			mastering.limiter_state = {0.0f, 1.0f};
		}
	}

	// Channel 3 is LFE, every other channel full band; a changed gain ramps over one grain.
	if (mastering.gain_enabled) {
		const auto& from = mastering.gain_rendered ? mastering.previous_gain : mastering.gain;
		for (uint32_t c = 0; c < inputs; ++c) {
			auto*        data = voice.samples.data() + size_t(c) * grain;
			const size_t k    = c == 3 ? 1 : 0;
			const float  step = (mastering.gain[k] - from[k]) / static_cast<float>(grain);
			for (uint32_t i = 0; i < grain; ++i) {
				data[i] *= from[k] + static_cast<float>(i) * step;
			}
		}
		mastering.previous_gain = mastering.gain;
	}
	mastering.gain_rendered = mastering.gain_enabled;

	if (metering) {
		Ngs2MeterPeaks(mastering, 1, voice.samples.data(), inputs, grain, blocks);
		mastering.meter_index = (mastering.meter_index + 1) % blocks;
		mastering.meter_count = std::min(mastering.meter_count + 1, blocks);
	}
	mastering.meter_channels = metering ? inputs : 0;

	Ngs2MixOutput(voice, voice.samples.data(), buffers, grain);
}

// Checks every buffer, in index order, before anything renders and returns the first failure.
static int Ngs2CheckRenderBuffers(const Ngs2RenderBufferInfo* buffers, uint32_t count,
                                  uint32_t grain) {
	if (buffers == nullptr && count != 0) {
		return NGS2_ERROR_INVALID_RENDER_BUFFER_ADDRESS;
	}
	if (count > 32) {
		return NGS2_ERROR_INVALID_NUM_BUFFERS;
	}
	for (uint32_t i = 0; i < count; ++i) {
		const auto& buffer = buffers[i];
		size_t      bytes  = 0;
		switch (buffer.waveform_type) {
			case 0x12:
			case 0x13: bytes = sizeof(int16_t); break;
			case 0x18:
			case 0x19: bytes = sizeof(float); break;
			default: return NGS2_ERROR_INVALID_WAVEFORM_TYPE;
		}
		if (buffer.num_channels == 0) {
			return NGS2_ERROR_INVALID_NUM_CHANNELS;
		}
		if (buffer.buffer == nullptr) {
			return NGS2_ERROR_INVALID_RENDER_BUFFER_ADDRESS;
		}
		if (buffer.buffer_size < bytes * buffer.num_channels * grain) {
			return NGS2_ERROR_INVALID_RENDER_BUFFER_SIZE;
		}
	}
	return OK;
}

// Publishes the voice's state flags after a render, which ends a pending PLAY.
static void Ngs2PublishVoiceState(Ngs2VoiceInternal& voice) {
	// No envelope is configured, so a release stop finishes in this render.
	if (voice.state == Ngs2VoicePlayState::Stopped) {
		voice.state = Ngs2VoicePlayState::Empty;
	}
	voice.state_flags  = static_cast<uint32_t>(voice.state);
	voice.play_pending = false;
}

int KYTY_SYSV_ABI Ngs2SystemRender(uintptr_t system_handle, const Ngs2RenderBufferInfo* buffer_info,
                                   uint32_t num_buffer_info) {
	auto* ngs = Ngs2FindSystem(system_handle);
	if (ngs == nullptr) {
		return NGS2_ERROR_INVALID_SYSTEM_HANDLE;
	}
	Common::LockGuard lock(ngs->mutex);
	const auto        grain  = ngs->option.num_grain_samples;
	const auto        result = Ngs2CheckRenderBuffers(buffer_info, num_buffer_info, grain);
	if (result != OK) {
		return result;
	}
	std::vector<Ngs2VoiceInternal*> voices;
	{
		Common::LockGuard racks_lock(g_racks_mutex);
		for (auto* rack = g_racks_list; rack != nullptr; rack = rack->next) {
			if (rack->ngs != ngs) {
				continue;
			}
			auto* items = reinterpret_cast<Ngs2VoiceInternal*>(rack + 1);
			for (uint32_t i = 0; i < rack->option.common.max_voices; ++i) {
				items[i].rendered = false;
				voices.push_back(items + i);
			}
		}
	}
	Ngs2RenderBuffers buffers {buffer_info, num_buffer_info};
	for (auto* voice: voices) {
		Ngs2RenderVoice(*voice, voices, grain);
		// A playing mastering voice renders its modules and writes its output even without input.
		if (voice->state != Ngs2VoicePlayState::Playing || voice->channels == 0) {
			continue;
		}
		if (voice->rack->type == Ngs2RackType::Mastering) {
			Ngs2RenderMastering(*voice, buffers, grain);
		} else if (voice->rack->type == Ngs2RackType::CustomMastering) {
			// A custom mastering voice writes through the first Output module of its chain.
			const auto* modules = voice->rack->option.custom_mastering.custom_rack_option.module;
			const auto* end     = modules + voice->modules.size();
			const auto* output =
			    std::ranges::find(modules, end, 0x1au, &Ngs2CustomRackModuleInfo::module_id);
			if (output != end) {
				Ngs2MixOutput(*voice, voice->Buffer(output->source_buffer_id, grain), buffers,
				              grain);
			}
		}
	}
	// A buffer no voice wrote is zeroed over its whole size.
	for (uint32_t i = 0; i < num_buffer_info; ++i) {
		if ((buffers.written >> i & 1u) == 0) {
			std::memset(buffer_info[i].buffer, 0, buffer_info[i].buffer_size);
		}
	}
	for (auto* voice: voices) {
		Ngs2PublishVoiceState(*voice);
	}
	++ngs->render_count;
	return OK;
}

static bool Ngs2FourCcEquals(const uint8_t* data, const char* four_cc) {
	return std::memcmp(data, four_cc, 4) == 0;
}

struct Ngs2Atrac9Frames {
	uint32_t size    = 0;
	uint32_t samples = 0;
	uint32_t count   = 0;
};

// The frame size, frame samples and frames per superframe of an ATRAC9 config of one stream (sync
// 0xfe) or of several (sync 0x30). Other sync bytes have no frames; only 12, 24 and 48 kHz have
// frame samples.
static Ngs2Atrac9Frames Ngs2Atrac9Geometry(uint32_t config) {
	constexpr std::array<uint32_t, 16> FRAME_SAMPLES = {0, 64, 0, 0, 128, 0, 0, 256};

	const uint32_t samples = FRAME_SAMPLES[(config >> 20u) & 0xfu];
	if (config >> 24u == 0xfe) {
		return {((config >> 5u) & 0x7ffu) + 1u, samples, 1u << ((config >> 3u) & 3u)};
	}
	if (config >> 24u == 0x30) {
		return {(((config >> 14u) & 0x3fu) + 1u) * (((config >> 2u) & 0x7ffu) + 1u), samples,
		        1u << (config & 3u)};
	}
	return {};
}

// Zeroes the block, then fills it with the data and skip of `num_samples` samples from
// `sample_pos`, in 32-bit math.
static int Ngs2CalcBlock(const Ngs2WaveformFormat* format, uint32_t sample_pos,
                         uint32_t num_samples, Ngs2WaveformBlock& block) {
	block = {};
	if (format == nullptr) {
		return NGS2_ERROR_INVALID_WAVEFORM_FORMAT;
	}
	if (format->waveform_type == NGS2_WAVEFORM_TYPE_ATRAC9) {
		const auto     frame              = Ngs2Atrac9Geometry(format->config_data);
		const uint32_t superframe_samples = frame.samples * frame.count;
		const uint32_t superframe_size    = frame.size * frame.count;
		if (superframe_samples == 0) {
			return NGS2_ERROR_UNKNOWN_WAVEFORM_FORMAT;
		}
		// Decoded samples lag the superframes by one frame.
		const uint32_t first = sample_pos / superframe_samples;
		const uint32_t last  = sample_pos + frame.samples + num_samples + superframe_samples - 1;

		block.data_offset      = first * superframe_size;
		block.data_size        = (last / superframe_samples - first) * superframe_size;
		block.num_skip_samples = sample_pos + frame.samples - first * superframe_samples;
	} else if (const uint32_t bytes = Ngs2PcmSampleBytes(format->waveform_type); bytes != 0) {
		// PCM has fixed-size frames, so the block starts exactly at the sample; the math wraps at
		// 32 bits.
		const uint32_t frame_size = format->num_channels * bytes;
		block.data_offset         = sample_pos * frame_size;
		block.data_size           = num_samples * frame_size;
	} else {
		return NGS2_ERROR_UNKNOWN_WAVEFORM_FORMAT;
	}
	block.num_samples = num_samples;
	return OK;
}

struct Ngs2RiffChunk {
	uint32_t body = 0;
	uint32_t size = 0;
};

// A waveform in guest memory, read at offsets from its start.
struct Ngs2WaveformReader {
	const uint8_t* data = nullptr;
	size_t         size = 0;

	// Copies `n` bytes at `offset` into `out`; false when they are not all inside the data.
	bool ReadAt(uint64_t offset, void* out, size_t n) const {
		if (data == nullptr || offset + n > size) {
			return false;
		}
		std::memcpy(out, data + offset, n);
		return true;
	}

	// Copies up to `cap` bytes of the first chunk `id` into `out`. Every search walks the RIFF list
	// from offset 12 and fails at a list size of 4 to 16, at a zero-size chunk before the match,
	// past the list size and on a failed read.
	std::optional<Ngs2RiffChunk> FindChunk(const char* id, void* out, size_t cap) const {
		uint8_t header[12];
		if (!ReadAt(0, header, sizeof(header))) {
			return {};
		}
		const uint32_t list = Ngs2Load<uint32_t>(header + 4);
		if (list - 4u <= 12u) {
			return {};
		}
		for (uint64_t position = 12;;) {
			if (!ReadAt(position, header, 8)) {
				return {};
			}
			const uint32_t chunk_size = Ngs2Load<uint32_t>(header + 4);
			const uint64_t body       = position + 8;
			if (Ngs2FourCcEquals(header, id)) {
				const size_t n = std::min<size_t>(chunk_size, cap);
				if (n != 0 && !ReadAt(body, out, n)) {
					return {};
				}
				return Ngs2RiffChunk {static_cast<uint32_t>(body), chunk_size};
			}
			// Odd sizes are padded; a walk past 4 GiB fails instead of wrapping.
			position = body + ((chunk_size + 1u) & ~1u);
			if (chunk_size == 0 || uint64_t(list) - 4u <= position) {
				return {};
			}
		}
	}
};

// Parses an ATRAC9 RIFF waveform from its first fmt, fact and data chunks; a failed parse writes
// nothing.
static int Ngs2ParseWaveform(const Ngs2WaveformReader& reader, Ngs2WaveformInfo* info) {
	static constexpr uint8_t PCM_GUID[16]    = {0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00,
	                                            0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71};
	static constexpr uint8_t ATRAC9_GUID[16] = {0xd2, 0x42, 0xe1, 0x47, 0xba, 0x36, 0x8d, 0x4d,
	                                            0x88, 0xfc, 0x61, 0x65, 0x4f, 0x8c, 0x83, 0x6c};
	if (info == nullptr) {
		return NGS2_ERROR_INVALID_OUT_ADDRESS;
	}
	uint8_t magic[4];
	if (!reader.ReadAt(0, magic, sizeof(magic)) || !Ngs2FourCcEquals(magic, "RIFF")) {
		return NGS2_ERROR_INVALID_WAVEFORM_DATA;
	}
	uint8_t    fmt[52]   = {};
	const auto fmt_chunk = reader.FindChunk("fmt ", fmt, sizeof(fmt));
	if (!fmt_chunk) {
		return NGS2_ERROR_INVALID_WAVEFORM_DATA;
	}
	// An extensible format with a subformat other than PCM is ATRAC9 at exactly 52 bytes or with
	// the ATRAC9 subformat.
	if (Ngs2Load<uint16_t>(fmt) != 0xfffe || fmt_chunk->size < 40 ||
	    std::memcmp(fmt + 24, PCM_GUID, sizeof(PCM_GUID)) == 0 ||
	    (fmt_chunk->size != sizeof(fmt) &&
	     std::memcmp(fmt + 24, ATRAC9_GUID, sizeof(ATRAC9_GUID)) != 0)) {
		return NGS2_ERROR_UNKNOWN_WAVEFORM_FORMAT;
	}
	uint8_t fact[12] = {};
	if (!reader.FindChunk("fact", fact, sizeof(fact))) {
		return NGS2_ERROR_INVALID_WAVEFORM_DATA;
	}
	const auto data = reader.FindChunk("data", nullptr, 0);
	if (!data) {
		return NGS2_ERROR_INVALID_WAVEFORM_DATA;
	}
	const uint32_t config = Ngs2LoadBigEndian<uint32_t>(fmt + 44);
	const auto     frame  = Ngs2Atrac9Geometry(config);

	// One block holds every sample.
	const Ngs2WaveformFormat format {NGS2_WAVEFORM_TYPE_ATRAC9, Ngs2Load<uint16_t>(fmt + 2),
	                                 Ngs2Load<uint32_t>(fmt + 4), config};
	Ngs2WaveformBlock        block;
	const int                result = Ngs2CalcBlock(&format, 0, Ngs2Load<uint32_t>(fact), block);
	if (result != OK) {
		return result;
	}
	info->format                   = format;
	info->data_offset              = data->body;
	info->data_size                = data->size;
	info->loop_begin_position      = 0;
	info->loop_end_position        = 0;
	info->num_samples              = block.num_samples;
	info->audio_unit_size          = frame.size;
	info->num_audio_unit_samples   = frame.samples;
	info->num_audio_unit_per_frame = frame.count;
	info->audio_frame_size         = frame.size * frame.count;
	info->num_audio_frame_samples  = frame.samples * frame.count;
	// The decoder delay is one frame, whatever the fact chunk holds.
	info->num_delay_samples = frame.samples;
	info->num_blocks        = 1;
	info->blocks[0]         = block;
	info->blocks[0].data_offset += data->body;
	return OK;
}

int KYTY_SYSV_ABI Ngs2ParseWaveformData(const void* data, size_t data_size,
                                        Ngs2WaveformInfo* info) {
	PRINT_NAME();
	LOGF("\t data = 0x%016" PRIx64 ", data_size = 0x%016" PRIx64 "\n",
	     reinterpret_cast<uint64_t>(data), static_cast<uint64_t>(data_size));

	// Only the waveform type and channel count are cleared before parsing.
	if (info != nullptr) {
		info->format.waveform_type = 0;
		info->format.num_channels  = 0;
	}
	return Ngs2ParseWaveform({static_cast<const uint8_t*>(data), data_size}, info);
}

int KYTY_SYSV_ABI Ngs2CalcWaveformBlock(const Ngs2WaveformFormat* format, uint32_t sample_pos,
                                        uint32_t num_samples, Ngs2WaveformBlock* block) {
	PRINT_NAME();
	LOGF("\t format = 0x%016" PRIx64 ", sample_pos = %" PRIu32 ", num_samples = %" PRIu32 "\n",
	     reinterpret_cast<uint64_t>(format), sample_pos, num_samples);

	if (block == nullptr) {
		return NGS2_ERROR_INVALID_OUT_ADDRESS;
	}
	return Ngs2CalcBlock(format, sample_pos, num_samples, *block);
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
		                          (180.0f / std::numbers::pi_v<float>) * 2.0f;
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

	LOGF("\t voice_id = %u\n", voice_id);

	auto* rack = Ngs2FindRack(rack_handle);
	if (rack == nullptr) {
		return NGS2_ERROR_INVALID_RACK_HANDLE;
	}
	if (handle == nullptr) {
		return NGS2_ERROR_INVALID_OUT_ADDRESS;
	}
	if (voice_id >= rack->option.common.max_voices) {
		return NGS2_ERROR_INVALID_VOICE_INDEX;
	}

	auto* voices = reinterpret_cast<Ngs2VoiceInternal*>(rack + 1);
	*handle      = reinterpret_cast<uintptr_t>(voices + voice_id);

	return OK;
}

// A patch to a rack handle connects to voice #0 of that rack.
static Ngs2VoiceInternal* Ngs2FindPatchDestination(uintptr_t handle) {
	auto* rack = Ngs2FindRack(handle);
	return rack != nullptr ? reinterpret_cast<Ngs2VoiceInternal*>(rack + 1)
	                       : reinterpret_cast<Ngs2VoiceInternal*>(handle);
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

		// Bit 15 of a custom rack id is bit 7 of its module id.
		EXIT_NOT_IMPLEMENTED(rack_id != 0x3000 && rack_id != 0x4000 &&
		                     ((param->id >> 15u) & 0x1u) != 0);

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
				EXIT_NOT_IMPLEMENTED(voice->rack->type != Ngs2RackType::Submixer);
				if ((param->id & 0xffffu) == 0) {
					EXIT_NOT_IMPLEMENTED(param->size != sizeof(Ngs2SubmixerVoiceSetupParam));
					const auto& setup =
					    *reinterpret_cast<const Ngs2SubmixerVoiceSetupParam*>(param);
					EXIT_NOT_IMPLEMENTED(
					    setup.num_io_channels == 0 ||
					    setup.num_io_channels > voice->rack->option.submixer.max_channels ||
					    setup.flags != 0);
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
				const auto  cid       = param->id & 0xffffu;
				const auto* values    = reinterpret_cast<const uint32_t*>(param + 1);
				auto&       mastering = *voice->mastering;
				if (cid > 6) {
					return NGS2_ERROR_INVALID_VOICE_CONTROL_ID;
				}
				if (param->size < (cid == 1 ? sizeof(Ngs2VoiceMatrixLevelsParam)
				                            : sizeof(Ngs2VoiceParamHeader) + 8)) {
					return NGS2_ERROR_INVALID_VOICE_CONTROL_SIZE;
				}
				switch (cid) {
					case 0:
						if (values[0] - 1 >= voice->rack->option.mastering.max_channels) {
							return NGS2_ERROR_INVALID_NUM_CHANNELS;
						}
						EXIT_NOT_IMPLEMENTED(values[0] > 8);
						// Setup restores the default levels, disables the LFE filter, the limiter,
						// the gain and the peak meters, and clears the limiter and meter states;
						// ramps, filter history, the limiter gain and the meter window go on.
						mastering.levels          = NGS2_MASTERING_DEFAULT_LEVELS;
						mastering.lfe_enabled     = false;
						mastering.limiter_enabled = false;
						mastering.limiter_state   = {};
						mastering.gain_enabled    = false;
						mastering.meters_enabled  = false;
						mastering.meter_channels  = 0;
						voice->channels           = values[0];
						voice->output_id          = 0;
						break;
					case 1: {
						const auto* matrix =
						    reinterpret_cast<const Ngs2VoiceMatrixLevelsParam*>(param);
						const auto type = matrix->matrix_id;
						if (type >= 16) {
							return NGS2_ERROR_INVALID_MATRIX_TYPE;
						}
						// Levels are copied unclamped from the type's field on, so extra levels
						// overwrite the following fields.
						// Type t converts channels[t / 4] into channels[t % 4] channels.
						constexpr std::array<uint32_t, 4> channels = {1, 2, 6, 8};
						const auto                        offset =
						    Ngs2MasteringLevelsOffset(channels[type / 4], channels[type % 4]);
						const size_t count = static_cast<uint16_t>(matrix->num_levels);
						std::copy_n(matrix->levels,
						            std::min(count, mastering.levels.size() - offset),
						            mastering.levels.begin() + offset);
						break;
					}
					case 2:
						// The enable flag is clamped to 1 and fc to 80..120 Hz; up to 80 Hz selects
						// the 80 Hz filter, above it the 120 Hz one.
						mastering.lfe_enable_flag = values[0] != 0;
						Ngs2SetMasteringLfe(mastering, values[1] <= 80 ? 80u : 120u,
						                    voice->rack->ngs->option.sample_rate);
						mastering.lfe_enabled = true;
						break;
					case 3: {
						const auto* levels = reinterpret_cast<const float*>(values);
						const auto  rate   = voice->rack->ngs->option.sample_rate;
						// The threshold is clamped to 0..1; a NaN threshold never limits.
						mastering.limiter_enable_flag = values[0] != 0;
						mastering.limiter_threshold   = std::clamp(levels[1], 0.0f, 1.0f);
						mastering.limiter_attack      = Ngs2LimiterCoefficient(rate, 0.01f);
						mastering.limiter_release     = Ngs2LimiterCoefficient(rate, 0.1f);
						mastering.limiter_enabled     = true;
						break;
					}
					case 4: {
						const auto* levels = reinterpret_cast<const float*>(values);
						mastering.gain         = {std::clamp(levels[0], -32.0f, 32.0f),
						                          std::clamp(levels[1], -32.0f, 32.0f)};
						mastering.gain_enabled = true;
						break;
					}
					case 5: voice->output_id = std::min(values[0], 32u); break;
					case 6:
						if (values[0] > 2) {
							return NGS2_ERROR_INVALID_PEAK_METER_DETECT_TYPE;
						}
						mastering.meter_detect_type = values[0];
						mastering.meters_enabled    = true;
						break;
				}
				break;
			}
			case 0x4000: {
				EXIT_NOT_IMPLEMENTED(!Ngs2RackIsCustom(voice->rack->type));
				// The id names module `index` and its module id; bits 5-7 are not read.
				const auto  index   = param->id & 0x1fu;
				const auto& modules = voice->rack->option.custom_sampler.custom_rack_option.module;
				if (index >= voice->modules.size() ||
				    (param->id >> 8u & 0xffu) != modules[index].module_id) {
					return NGS2_ERROR_INVALID_VOICE_PARAM_ID;
				}
				const auto module_id = modules[index].module_id;
				auto&      module    = voice->modules[index];
				if (module_id != 0x17 && module_id != 0x19 && module_id != 0x1f) {
					if (!module.control_warned) {
						Log::WriteToConsoleAndLog(fmt::sprintf(
						    "warning: NGS2 custom control 0x%08" PRIx32 " is ignored\n",
						    param->id));
						module.control_warned = true;
					}
					break;
				}
				if (module_id == 0x17) {
					EXIT_NOT_IMPLEMENTED(param->size != sizeof(Ngs2VoiceParamHeader) +
					                                       37 * 37 * sizeof(float));
					const auto* levels = reinterpret_cast<const float*>(param + 1);
					module.channel_matrix.resize(37 * 37);
					std::transform(levels, levels + module.channel_matrix.size(),
					               module.channel_matrix.begin(),
					               [](float level) { return std::clamp(level, -32.0f, 32.0f); });
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
						if (param->size < sizeof(BlocksParam)) {
							return NGS2_ERROR_INVALID_VOICE_CONTROL_SIZE;
						}
						const auto& blocks = *reinterpret_cast<const BlocksParam*>(param);
						if (blocks.count != 0 && blocks.blocks == nullptr) {
							return NGS2_ERROR_INVALID_WAVEFORM_BLOCK_ADDRESS;
						}
						const auto result = voice->AddWaveformBlocks(
						    blocks.data, blocks.flags, blocks.count, blocks.blocks);
						if (result != OK) {
							return result;
						}
						break;
					}
					case 4: {
						for (auto& block: voice->blocks) {
							if (Ngs2LoopsAgain(block)) {
								block.info.num_repeats = 0;
								break;
							}
						}
						break;
					}
					case 5: {
						voice->pitch_ratio =
						    std::clamp(*reinterpret_cast<const float*>(param + 1), 0.0f, 4.0f);
						break;
					}
					case 0xa: {
						EXIT_NOT_IMPLEMENTED(voice->rack->type != Ngs2RackType::Sampler);
						if (param->size <
						    sizeof(Ngs2VoiceParamHeader) + sizeof(Ngs2SamplerFilterParam)) {
							return NGS2_ERROR_INVALID_VOICE_CONTROL_SIZE;
						}
						const auto& filter =
						    *reinterpret_cast<const Ngs2SamplerFilterParam*>(param + 1);
						if (filter.index >= voice->rack->option.sampler.max_filters) {
							return NGS2_ERROR_INVALID_VOICE_PARAM_ID;
						}
						if (filter.filter.type > 11) {
							return NGS2_ERROR_INVALID_FILTER_TYPE;
						}
						if (filter.location >= voice->ports.size() + 2) {
							return NGS2_ERROR_INVALID_MODULE_LOCATION;
						}
						if (voice->filters.size() <= filter.index) {
							voice->filters.resize(filter.index + 1);
						}
						// A change keeps the history.
						auto& target    = voice->filters[filter.index];
						target.location = filter.location;
						target.history.resize(voice->channels);
						target.Design(filter.filter, voice->rack->ngs->option.sample_rate);
						break;
					}
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
			case 0x4003: {
				// SceNgs2CustomMasteringVoiceSetupParam {numInputChannels, flags} or
				// SceNgs2CustomMasteringVoiceOutputParam {outputId, reserved}.
				EXIT_NOT_IMPLEMENTED(voice->rack->type != Ngs2RackType::CustomMastering ||
				                     param->size != sizeof(Ngs2VoiceParamHeader) + 8u);
				const auto* values = reinterpret_cast<const uint32_t*>(param + 1);
				switch (param->id & 0xffffu) {
					case 0:
						EXIT_NOT_IMPLEMENTED(values[0] == 0 ||
						                     values[0] > voice->rack->option.common.max_voice_channels ||
						                     values[1] != 0);
						voice->channels = values[0];
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

// The player's part of a sampler voice state reads as zero between PLAY and the next render.
template <typename State>
static void Ngs2FillSamplerState(const Ngs2VoiceInternal& voice, State& state) {
	if (voice.play_pending) {
		return;
	}
	state.num_decoded_samples = voice.decoded_samples;
	state.decoded_data_size   = voice.decoded_bytes;
	state.waveform_data       = voice.WaveformData();
	state.user_data           = voice.blocks.empty() ? 0 : voice.blocks.front().info.user_data;
}

int KYTY_SYSV_ABI Ngs2VoiceGetState(uintptr_t voice_handle, Ngs2VoiceState* state,
                                    size_t state_size) {
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(voice_handle == 0);
	if (state_size < sizeof(Ngs2VoiceState)) {
		return NGS2_ERROR_INVALID_OUT_SIZE;
	}
	if (state == nullptr) {
		return NGS2_ERROR_INVALID_OUT_ADDRESS;
	}

	auto* voice = reinterpret_cast<Ngs2VoiceInternal*>(voice_handle);

	Common::LockGuard lock(voice->rack->ngs->mutex);

	// A voice not in use reads as zeros. Otherwise the header is written first and stays written
	// when the rack's part is too small.
	const Ngs2VoiceState header {voice->state_flags, 0};
	if (header.state_flags == 0) {
		std::memset(state, 0, state_size);
		return OK;
	}
	*state = header;
	if (state_size == sizeof(header)) {
		return OK;
	}

	switch (voice->rack->type) {
		case Ngs2RackType::Submixer:
			EXIT_NOT_IMPLEMENTED(state_size != sizeof(Ngs2SubmixerVoiceState));
			*reinterpret_cast<Ngs2SubmixerVoiceState*>(state) = {header};
			break;
		case Ngs2RackType::Mastering: {
			if (state_size < sizeof(Ngs2MasteringVoiceState)) {
				return NGS2_ERROR_INVALID_OUT_SIZE;
			}
			const auto& modules   = *voice->mastering;
			auto*       mastering = reinterpret_cast<Ngs2MasteringVoiceState*>(state);
			*mastering            = {header};
			// Between PLAY and the next render the limiter and the peak meters report nothing.
			if (voice->play_pending) {
				break;
			}
			mastering->limiter_peak_level  = modules.limiter_state[0];
			mastering->limiter_press_level = modules.limiter_state[1];
			std::copy_n(modules.meter_peaks[0].begin(), modules.meter_channels,
			            mastering->input_peak_height);
			std::copy_n(modules.meter_peaks[1].begin(), modules.meter_channels,
			            mastering->output_peak_height);
			break;
		}
		case Ngs2RackType::CustomMastering:
		case Ngs2RackType::CustomSampler: {
			// The fixed part is written before the configured size is checked; the custom module
			// states after it read as zero.
			const bool   sampler = voice->rack->type == Ngs2RackType::CustomSampler;
			const size_t base    = sampler ? sizeof(Ngs2CustomSamplerVoiceState)
			                               : sizeof(Ngs2CustomMasteringVoiceState);
			if (state_size < base) {
				return NGS2_ERROR_INVALID_OUT_SIZE;
			}
			std::memset(state + 1, 0, base - sizeof(header));
			if (sampler) {
				Ngs2FillSamplerState(*voice, *reinterpret_cast<Ngs2CustomSamplerVoiceState*>(state));
			}
			const size_t configured =
			    voice->rack->option.custom_sampler.custom_rack_option.state_size;
			if (state_size < configured) {
				return NGS2_ERROR_INVALID_OUT_SIZE;
			}
			if (configured > base) {
				std::memset(reinterpret_cast<uint8_t*>(state) + base, 0, configured - base);
			}
			break;
		}
		case Ngs2RackType::Sampler: {
			if (state_size < sizeof(Ngs2SamplerVoiceState)) {
				return NGS2_ERROR_INVALID_OUT_SIZE;
			}
			// The envelope and peak meter heights read 0.0: neither module is ever in use.
			auto& sampler = *reinterpret_cast<Ngs2SamplerVoiceState*>(state);
			sampler       = {header};
			Ngs2FillSamplerState(*voice, sampler);
			break;
		}
		default: EXIT("unknown type: %s\n", magic_enum::enum_name(voice->rack->type));
	}

	return OK;
}

int KYTY_SYSV_ABI Ngs2VoiceGetStateFlags(uintptr_t voice_handle, uint32_t* state_flags) {
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(voice_handle == 0);
	if (state_flags == nullptr) {
		return NGS2_ERROR_INVALID_OUT_ADDRESS;
	}

	auto* voice = reinterpret_cast<Ngs2VoiceInternal*>(voice_handle);

	Common::LockGuard lock(voice->rack->ngs->mutex);

	*state_flags = voice->state_flags;

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

	ngs->user_lock.Lock();

	return OK;
}

int KYTY_SYSV_ABI Ngs2SystemUnlock(uintptr_t system_handle) {
	PRINT_NAME();

	auto* ngs = Ngs2FindSystem(system_handle);
	if (ngs == nullptr) {
		return NGS2_ERROR_INVALID_SYSTEM_HANDLE;
	}

	ngs->user_lock.Unlock();

	return OK;
}

} // namespace Ngs2

} // namespace Libs::Audio
