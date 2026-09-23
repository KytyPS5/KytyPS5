#include "common/abi.h"
#include "libs/errno.h"
#include "libs/libs.h"
#include "libs/videoDec2Decoder.h"
#include "loader/symbolDatabase.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Libs {

LIB_VERSION("Videodec2", 1, "Videodec2", 1, 1);

namespace VideoDec2 {

constexpr int32_t VIDEODEC2_ERROR_API_FAIL             = -2128805632; // 0x811d0100
constexpr int32_t VIDEODEC2_ERROR_STRUCT_SIZE          = -2128805631; // 0x811d0101
constexpr int32_t VIDEODEC2_ERROR_ARGUMENT_POINTER     = -2128805630; // 0x811d0102
constexpr int32_t VIDEODEC2_ERROR_DECODER_INSTANCE     = -2128805629; // 0x811d0103
constexpr int32_t VIDEODEC2_ERROR_MEMORY_SIZE          = -2128805628; // 0x811d0104
constexpr int32_t VIDEODEC2_ERROR_MEMORY_POINTER       = -2128805627; // 0x811d0105
constexpr int32_t VIDEODEC2_ERROR_FRAME_BUFFER_SIZE    = -2128805626; // 0x811d0106
constexpr int32_t VIDEODEC2_ERROR_FRAME_BUFFER_POINTER = -2128805625; // 0x811d0107
constexpr int32_t VIDEODEC2_ERROR_ACCESS_UNIT_SIZE     = -2128805619; // 0x811d010d
constexpr int32_t VIDEODEC2_ERROR_ACCESS_UNIT_POINTER  = -2128805618; // 0x811d010e
constexpr int32_t VIDEODEC2_ERROR_OUTPUT_INFO          = -2128805617; // 0x811d010f
constexpr int32_t VIDEODEC2_ERROR_COMPUTE_QUEUE        = -2128805616; // 0x811d0110
constexpr int32_t VIDEODEC2_ERROR_CONFIG_INFO          = -2128805376; // 0x811d0200
constexpr int32_t VIDEODEC2_ERROR_COMPUTE_PIPE_ID      = -2128805375; // 0x811d0201
constexpr int32_t VIDEODEC2_ERROR_COMPUTE_QUEUE_ID     = -2128805374; // 0x811d0202
constexpr int32_t VIDEODEC2_ERROR_RESOURCE_TYPE        = -2128805373; // 0x811d0203
constexpr int32_t VIDEODEC2_ERROR_CODEC_TYPE           = -2128805372; // 0x811d0204
constexpr int32_t VIDEODEC2_ERROR_INPUT_QUEUE_DEPTH    = -2128805370; // 0x811d0206
constexpr int32_t VIDEODEC2_ERROR_DPB_FRAME_COUNT      = -2128805367; // 0x811d0209
constexpr int32_t VIDEODEC2_ERROR_FRAME_WIDTH_HEIGHT   = -2128805366; // 0x811d020a
constexpr int32_t VIDEODEC2_ERROR_ACCESS_UNIT          = -2128805119; // 0x811d0301
constexpr int32_t VIDEODEC2_ERROR_OVERSIZE_DECODE      = -2128805118; // 0x811d0302

constexpr uint32_t VIDEODEC2_RESOURCE_TYPE_COMPUTE = 1;
constexpr size_t   VIDEODEC2_MIN_MEMORY_SIZE       = 16ull * 1024ull * 1024ull;
constexpr uint32_t VIDEODEC2_FRAME_FORMAT_DEFAULT  = 0;

using Videodec2Decoder      = void*;
using Videodec2ComputeQueue = void*;

struct Videodec2DecoderConfigInfo {
	size_t                this_size;
	uint32_t              resource_type;
	uint32_t              codec_type;
	uint32_t              profile;
	uint32_t              max_level;
	int32_t               max_frame_width;
	int32_t               max_frame_height;
	int32_t               max_dpb_frame_count;
	uint32_t              decode_input_queue_depth;
	Videodec2ComputeQueue compute_queue;
	uint64_t              cpu_affinity_mask;
	int32_t               cpu_thread_priority;
	bool                  optimize_progressive_video;
	bool                  check_memory_type;
	uint8_t               reserved0;
	uint8_t               reserved1;
	void*                 extra_config_info;
};

struct Videodec2DecoderMemoryInfo {
	size_t   this_size;
	size_t   cpu_memory_size;
	void*    cpu_memory;
	size_t   gpu_memory_size;
	void*    gpu_memory;
	size_t   cpu_gpu_memory_size;
	void*    cpu_gpu_memory;
	size_t   max_frame_buffer_size;
	uint32_t frame_buffer_alignment;
	uint32_t reserved0;
};

struct Videodec2InputData {
	size_t   this_size;
	void*    au_data;
	size_t   au_size;
	uint64_t pts_data;
	uint64_t dts_data;
	uint64_t attached_data;
};

struct Videodec2OutputInfo {
	size_t   this_size;
	bool     is_valid;
	bool     is_error_frame;
	uint8_t  picture_count;
	bool     is_discarded_frame;
	uint32_t codec_type;
	uint32_t frame_width;
	uint32_t frame_pitch;
	uint32_t frame_height;
	void*    frame_buffer;
	size_t   frame_buffer_size;
	uint32_t frame_format;
	uint32_t frame_pitch_in_bytes;
};

struct Videodec2FrameBuffer {
	size_t this_size;
	void*  frame_buffer;
	size_t frame_buffer_size;
	bool   is_accepted;
};

struct Videodec2AvcPictureInfo {
	size_t this_size;
	bool   is_valid;

	uint64_t pts_data;
	uint64_t dts_data;
	uint64_t attached_data;

	uint8_t  idr_picture_flag;
	uint8_t  profile_idc;
	uint8_t  level_idc;
	uint32_t pic_width_in_mbs_minus1;
	uint32_t pic_height_in_map_units_minus1;
	uint8_t  frame_mbs_only_flag;

	uint8_t  frame_cropping_flag;
	uint32_t frame_crop_left_offset;
	uint32_t frame_crop_right_offset;
	uint32_t frame_crop_top_offset;
	uint32_t frame_crop_bottom_offset;

	uint8_t  aspect_ratio_info_present_flag;
	uint8_t  aspect_ratio_idc;
	uint16_t sar_width;
	uint16_t sar_height;

	uint8_t video_signal_type_present_flag;
	uint8_t video_format;
	uint8_t video_full_range_flag;
	uint8_t colour_description_present_flag;
	uint8_t colour_primaries;
	uint8_t transfer_characteristics;
	uint8_t matrix_coefficients;

	uint8_t  timing_info_present_flag;
	uint32_t num_units_in_tick;
	uint32_t time_scale;
	uint8_t  fixed_frame_rate_flag;

	uint8_t bitstream_restriction_flag;
	uint8_t max_dec_frame_buffering;

	uint8_t pic_struct_present_flag;
	uint8_t pic_struct;
	uint8_t field_pic_flag;
	uint8_t bottom_field_flag;

	uint8_t sequence_parameter_set_present_flag;
	uint8_t picture_parameter_set_present_flag;
	uint8_t au_delimiter_present_flag;
	uint8_t end_of_sequence_present_flag;
	uint8_t end_of_stream_present_flag;
	uint8_t filler_data_present_flag;
	uint8_t picture_timing_sei_present_flag;
	uint8_t buffering_period_sei_present_flag;

	uint8_t constraint_set0_flag;
	uint8_t constraint_set1_flag;
	uint8_t constraint_set2_flag;
	uint8_t constraint_set3_flag;
	uint8_t constraint_set4_flag;
	uint8_t constraint_set5_flag;
};

struct Videodec2ComputeMemoryInfo {
	size_t this_size;
	size_t cpu_gpu_memory_size;
	void*  cpu_gpu_memory;
};

struct Videodec2ComputeConfigInfo {
	size_t   this_size;
	uint16_t compute_pipe_id;
	uint16_t compute_queue_id;
	bool     check_memory_type;
	uint8_t  reserved0;
	uint16_t reserved1;
};

using DecoderState = Decoder::Instance;

static_assert(sizeof(Videodec2ComputeMemoryInfo) == 24);
static_assert(sizeof(Videodec2ComputeConfigInfo) == 16);
static_assert(sizeof(Videodec2DecoderConfigInfo) == 72);
static_assert(sizeof(Videodec2DecoderMemoryInfo) == 72);
static_assert(sizeof(Videodec2InputData) == 48);
static_assert(sizeof(Videodec2OutputInfo) == 56);
static_assert(sizeof(Videodec2FrameBuffer) == 32);
static_assert(sizeof(Videodec2AvcPictureInfo) == 120);

static std::mutex                g_decoder_mutex;
static std::unordered_set<void*> g_decoders;

static bool IsOutputInfoSizeValid(size_t this_size) {
	return this_size == sizeof(Videodec2OutputInfo) ||
	       (this_size | 8u) == sizeof(Videodec2OutputInfo);
}

static DecoderState* GetDecoder(Videodec2Decoder decoder) {
	std::scoped_lock lock(g_decoder_mutex);
	return g_decoders.contains(decoder) ? static_cast<DecoderState*>(decoder) : nullptr;
}

static void FillNoPictureOutput(const Videodec2FrameBuffer* frame_buffer,
                                Videodec2OutputInfo* output_info, uint32_t codec_type) {
	output_info->is_valid           = false;
	output_info->is_error_frame     = false;
	output_info->picture_count      = 0;
	output_info->is_discarded_frame = false;
	output_info->codec_type         = codec_type;
	output_info->frame_width        = 0;
	output_info->frame_pitch        = 0;
	output_info->frame_height       = 0;
	output_info->frame_buffer      = frame_buffer != nullptr ? frame_buffer->frame_buffer : nullptr;
	output_info->frame_buffer_size = frame_buffer != nullptr ? frame_buffer->frame_buffer_size : 0;
	if (output_info->this_size == sizeof(Videodec2OutputInfo)) {
		output_info->frame_format         = VIDEODEC2_FRAME_FORMAT_DEFAULT;
		output_info->frame_pitch_in_bytes = 0;
	}
}

static int32_t MapDecoderResult(Decoder::Result result) {
	switch (result) {
		case Decoder::Result::Ok: return OK;
		case Decoder::Result::ApiFail: return VIDEODEC2_ERROR_API_FAIL;
		case Decoder::Result::AccessUnit: return VIDEODEC2_ERROR_ACCESS_UNIT;
		case Decoder::Result::FrameBufferSize: return VIDEODEC2_ERROR_FRAME_BUFFER_SIZE;
		case Decoder::Result::OversizeDecode: return VIDEODEC2_ERROR_OVERSIZE_DECODE;
	}
	return VIDEODEC2_ERROR_API_FAIL;
}

static void ApplyDecodedOutput(const Decoder::Output& decoded, Videodec2FrameBuffer* frame_buffer,
                               Videodec2OutputInfo* output_info) {
	frame_buffer->is_accepted = decoded.buffer_accepted;
	if (!decoded.valid) {
		return;
	}
	output_info->is_valid          = true;
	output_info->is_error_frame    = decoded.error_frame;
	output_info->picture_count     = 1;
	output_info->codec_type        = decoded.codec_type;
	output_info->frame_width       = decoded.width;
	output_info->frame_pitch       = decoded.pitch;
	output_info->frame_height      = decoded.height;
	output_info->frame_buffer      = decoded.buffer;
	output_info->frame_buffer_size = decoded.buffer_size;
	if (output_info->this_size == sizeof(Videodec2OutputInfo)) {
		output_info->frame_format         = VIDEODEC2_FRAME_FORMAT_DEFAULT;
		output_info->frame_pitch_in_bytes = decoded.pitch;
	}
}

static int32_t ValidateDecoderConfig(const Videodec2DecoderConfigInfo* config,
                                     bool                              require_compute_queue) {
	if (config->resource_type != VIDEODEC2_RESOURCE_TYPE_COMPUTE) {
		return VIDEODEC2_ERROR_RESOURCE_TYPE;
	}

	if (!Decoder::IsCodecSupported(config->codec_type)) {
		return VIDEODEC2_ERROR_CODEC_TYPE;
	}

	if (config->reserved0 != 0 || config->reserved1 != 0) {
		return VIDEODEC2_ERROR_CONFIG_INFO;
	}

	if (config->decode_input_queue_depth == 0) {
		return VIDEODEC2_ERROR_INPUT_QUEUE_DEPTH;
	}

	if (config->max_dpb_frame_count < -1 || config->max_dpb_frame_count == 0) {
		return VIDEODEC2_ERROR_DPB_FRAME_COUNT;
	}

	if (config->max_frame_width < -1 || config->max_frame_height < -1 ||
	    config->max_frame_width == 0 || config->max_frame_height == 0) {
		return VIDEODEC2_ERROR_FRAME_WIDTH_HEIGHT;
	}

	if (require_compute_queue && config->compute_queue == nullptr) {
		return VIDEODEC2_ERROR_COMPUTE_QUEUE;
	}

	return OK;
}

static int32_t KYTY_SYSV_ABI
QueryComputeMemoryInfo(Videodec2ComputeMemoryInfo* compute_memory_info) {
	PRINT_NAME();

	if (compute_memory_info == nullptr) {
		return VIDEODEC2_ERROR_ARGUMENT_POINTER;
	}

	if (compute_memory_info->this_size != sizeof(Videodec2ComputeMemoryInfo)) {
		return VIDEODEC2_ERROR_STRUCT_SIZE;
	}

	compute_memory_info->cpu_gpu_memory_size = VIDEODEC2_MIN_MEMORY_SIZE;
	compute_memory_info->cpu_gpu_memory      = nullptr;

	return OK;
}

static int32_t KYTY_SYSV_ABI AllocateComputeQueue(
    const Videodec2ComputeConfigInfo* compute_config_info,
    const Videodec2ComputeMemoryInfo* compute_memory_info, Videodec2ComputeQueue* compute_queue) {
	PRINT_NAME();

	if (compute_config_info == nullptr || compute_memory_info == nullptr ||
	    compute_queue == nullptr) {
		return VIDEODEC2_ERROR_ARGUMENT_POINTER;
	}

	if (compute_config_info->this_size != sizeof(Videodec2ComputeConfigInfo) ||
	    compute_memory_info->this_size != sizeof(Videodec2ComputeMemoryInfo)) {
		return VIDEODEC2_ERROR_STRUCT_SIZE;
	}

	if (compute_config_info->reserved0 != 0 || compute_config_info->reserved1 != 0) {
		return VIDEODEC2_ERROR_CONFIG_INFO;
	}

	if (compute_config_info->compute_pipe_id > 4) {
		return VIDEODEC2_ERROR_COMPUTE_PIPE_ID;
	}

	if (compute_config_info->compute_queue_id > 7) {
		return VIDEODEC2_ERROR_COMPUTE_QUEUE_ID;
	}

	if (compute_memory_info->cpu_gpu_memory_size < VIDEODEC2_MIN_MEMORY_SIZE) {
		return VIDEODEC2_ERROR_MEMORY_SIZE;
	}

	if (compute_memory_info->cpu_gpu_memory == nullptr) {
		return VIDEODEC2_ERROR_MEMORY_POINTER;
	}

	*compute_queue = compute_memory_info->cpu_gpu_memory;
	return OK;
}

static int32_t KYTY_SYSV_ABI ReleaseComputeQueue(Videodec2ComputeQueue compute_queue) {
	PRINT_NAME();

	return compute_queue != nullptr ? OK : VIDEODEC2_ERROR_COMPUTE_QUEUE_ID;
}

static int32_t KYTY_SYSV_ABI QueryDecoderMemoryInfo(const Videodec2DecoderConfigInfo* config,
                                                    Videodec2DecoderMemoryInfo*       memory_info) {
	PRINT_NAME();

	if (config == nullptr || memory_info == nullptr) {
		return VIDEODEC2_ERROR_ARGUMENT_POINTER;
	}

	if (config->this_size != sizeof(Videodec2DecoderConfigInfo) ||
	    memory_info->this_size != sizeof(Videodec2DecoderMemoryInfo)) {
		return VIDEODEC2_ERROR_STRUCT_SIZE;
	}

	const auto validation_result = ValidateDecoderConfig(config, false);
	if (validation_result != OK) {
		return validation_result;
	}

	memory_info->cpu_memory_size        = VIDEODEC2_MIN_MEMORY_SIZE;
	memory_info->cpu_memory             = nullptr;
	memory_info->gpu_memory_size        = VIDEODEC2_MIN_MEMORY_SIZE;
	memory_info->gpu_memory             = nullptr;
	memory_info->cpu_gpu_memory_size    = VIDEODEC2_MIN_MEMORY_SIZE;
	memory_info->cpu_gpu_memory         = nullptr;
	memory_info->max_frame_buffer_size  = VIDEODEC2_MIN_MEMORY_SIZE;
	memory_info->frame_buffer_alignment = 0x100;
	memory_info->reserved0              = 0;

	return OK;
}

static int32_t KYTY_SYSV_ABI CreateDecoder(const Videodec2DecoderConfigInfo* config,
                                           const Videodec2DecoderMemoryInfo* memory_info,
                                           Videodec2Decoder*                 decoder) {
	PRINT_NAME();

	if (config == nullptr || memory_info == nullptr || decoder == nullptr) {
		return VIDEODEC2_ERROR_ARGUMENT_POINTER;
	}

	if (config->this_size != sizeof(Videodec2DecoderConfigInfo) ||
	    memory_info->this_size != sizeof(Videodec2DecoderMemoryInfo)) {
		return VIDEODEC2_ERROR_STRUCT_SIZE;
	}

	const auto validation_result = ValidateDecoderConfig(config, true);
	if (validation_result != OK) {
		return validation_result;
	}

	if (memory_info->cpu_memory_size < VIDEODEC2_MIN_MEMORY_SIZE ||
	    memory_info->gpu_memory_size < VIDEODEC2_MIN_MEMORY_SIZE ||
	    memory_info->cpu_gpu_memory_size < VIDEODEC2_MIN_MEMORY_SIZE ||
	    memory_info->max_frame_buffer_size < VIDEODEC2_MIN_MEMORY_SIZE) {
		return VIDEODEC2_ERROR_MEMORY_SIZE;
	}

	if (memory_info->cpu_memory == nullptr || memory_info->gpu_memory == nullptr ||
	    memory_info->cpu_gpu_memory == nullptr) {
		return VIDEODEC2_ERROR_MEMORY_POINTER;
	}

	auto* state =
	    Decoder::Create({config->codec_type, config->max_frame_width, config->max_frame_height});
	if (state == nullptr) {
		return VIDEODEC2_ERROR_API_FAIL;
	}

	{
		std::scoped_lock lock(g_decoder_mutex);
		g_decoders.insert(state);
	}

	*decoder = state;
	return OK;
}

static int32_t KYTY_SYSV_ABI DeleteDecoder(Videodec2Decoder decoder) {
	PRINT_NAME();

	DecoderState* state = nullptr;
	{
		std::scoped_lock lock(g_decoder_mutex);
		auto             it = g_decoders.find(decoder);
		if (it == g_decoders.end()) {
			return VIDEODEC2_ERROR_DECODER_INSTANCE;
		}
		state = static_cast<DecoderState*>(*it);
		g_decoders.erase(it);
	}

	Decoder::Destroy(state);

	return OK;
}

static int32_t KYTY_SYSV_ABI Decode(Videodec2Decoder decoder, const Videodec2InputData* input_data,
                                    Videodec2FrameBuffer* frame_buffer,
                                    Videodec2OutputInfo*  output_info) {
	PRINT_NAME();

	auto* state = GetDecoder(decoder);
	if (state == nullptr) {
		return VIDEODEC2_ERROR_DECODER_INSTANCE;
	}

	if (input_data == nullptr || frame_buffer == nullptr || output_info == nullptr) {
		return VIDEODEC2_ERROR_ARGUMENT_POINTER;
	}

	if (input_data->this_size != sizeof(Videodec2InputData) ||
	    frame_buffer->this_size != sizeof(Videodec2FrameBuffer) ||
	    !IsOutputInfoSizeValid(output_info->this_size)) {
		return VIDEODEC2_ERROR_STRUCT_SIZE;
	}

	if (input_data->au_size == 0) {
		return VIDEODEC2_ERROR_ACCESS_UNIT_SIZE;
	}

	if (input_data->au_data == nullptr) {
		return VIDEODEC2_ERROR_ACCESS_UNIT_POINTER;
	}

	if (frame_buffer->frame_buffer_size == 0) {
		return VIDEODEC2_ERROR_FRAME_BUFFER_SIZE;
	}

	if (frame_buffer->frame_buffer == nullptr) {
		return VIDEODEC2_ERROR_FRAME_BUFFER_POINTER;
	}

	frame_buffer->is_accepted = false;
	FillNoPictureOutput(frame_buffer, output_info, Decoder::GetCodecType(state));

	Decoder::Output decoded {};
	const auto      result =
	    Decoder::Decode(state,
	                    {input_data->au_data, input_data->au_size, input_data->pts_data,
	                     input_data->dts_data, input_data->attached_data},
	                    {frame_buffer->frame_buffer, frame_buffer->frame_buffer_size}, &decoded);
	ApplyDecodedOutput(decoded, frame_buffer, output_info);
	return MapDecoderResult(result);
}

static int32_t KYTY_SYSV_ABI Flush(Videodec2Decoder decoder, Videodec2FrameBuffer* frame_buffer,
                                   Videodec2OutputInfo* output_info) {
	PRINT_NAME();

	auto* state = GetDecoder(decoder);
	if (state == nullptr) {
		return VIDEODEC2_ERROR_DECODER_INSTANCE;
	}

	if (frame_buffer == nullptr || output_info == nullptr) {
		return VIDEODEC2_ERROR_ARGUMENT_POINTER;
	}

	if (frame_buffer->this_size != sizeof(Videodec2FrameBuffer) ||
	    !IsOutputInfoSizeValid(output_info->this_size)) {
		return VIDEODEC2_ERROR_STRUCT_SIZE;
	}

	if (frame_buffer->frame_buffer_size == 0) {
		return VIDEODEC2_ERROR_FRAME_BUFFER_SIZE;
	}

	if (frame_buffer->frame_buffer == nullptr) {
		return VIDEODEC2_ERROR_FRAME_BUFFER_POINTER;
	}

	frame_buffer->is_accepted = false;
	FillNoPictureOutput(frame_buffer, output_info, Decoder::GetCodecType(state));

	Decoder::Output decoded {};
	const auto      result = Decoder::Flush(
	    state, {frame_buffer->frame_buffer, frame_buffer->frame_buffer_size}, &decoded);
	ApplyDecodedOutput(decoded, frame_buffer, output_info);
	return MapDecoderResult(result);
}

static int32_t KYTY_SYSV_ABI Reset(Videodec2Decoder decoder) {
	PRINT_NAME();

	auto* state = GetDecoder(decoder);
	if (state == nullptr) {
		return VIDEODEC2_ERROR_DECODER_INSTANCE;
	}
	Decoder::Reset(state);
	return OK;
}

static int32_t KYTY_SYSV_ABI GetPictureInfo(const Videodec2OutputInfo* output_info,
                                            void* first_picture_info, void* second_picture_info) {
	PRINT_NAME();

	if (output_info == nullptr || first_picture_info == nullptr) {
		return VIDEODEC2_ERROR_ARGUMENT_POINTER;
	}

	if (!IsOutputInfoSizeValid(output_info->this_size)) {
		return VIDEODEC2_ERROR_STRUCT_SIZE;
	}

	if (!output_info->is_valid || output_info->picture_count == 0 ||
	    output_info->frame_buffer == nullptr) {
		return VIDEODEC2_ERROR_OUTPUT_INFO;
	}

	Decoder::PictureInfo decoded {};
	if (!Decoder::GetPictureInfo(output_info->frame_buffer, &decoded) ||
	    decoded.codec_type != output_info->codec_type) {
		return VIDEODEC2_ERROR_OUTPUT_INFO;
	}

	auto fill_common = [&decoded](void* destination, bool valid) -> int32_t {
		auto*      bytes = static_cast<uint8_t*>(destination);
		const auto size  = *static_cast<const size_t*>(destination);
		if (size < 40 || size > 256) {
			return VIDEODEC2_ERROR_STRUCT_SIZE;
		}
		std::memset(bytes + sizeof(size_t), 0, size - sizeof(size_t));
		bytes[8] = valid ? 1 : 0;
		if (valid) {
			std::memcpy(bytes + 16, &decoded.pts, sizeof(decoded.pts));
			std::memcpy(bytes + 24, &decoded.dts, sizeof(decoded.dts));
			std::memcpy(bytes + 32, &decoded.attached_data, sizeof(decoded.attached_data));
		}
		return OK;
	};

	if (output_info->codec_type == 1) {
		const auto requested_size = *static_cast<const size_t*>(first_picture_info);
		if (requested_size != sizeof(Videodec2AvcPictureInfo) &&
		    (requested_size | 16u) != sizeof(Videodec2AvcPictureInfo)) {
			return VIDEODEC2_ERROR_STRUCT_SIZE;
		}

		Videodec2AvcPictureInfo picture {};
		picture.this_size                      = requested_size;
		picture.is_valid                       = true;
		picture.pts_data                       = decoded.pts;
		picture.dts_data                       = decoded.dts;
		picture.attached_data                  = decoded.attached_data;
		picture.idr_picture_flag               = decoded.key_frame ? 1 : 0;
		picture.profile_idc                    = static_cast<uint8_t>(decoded.profile);
		picture.level_idc                      = static_cast<uint8_t>(decoded.level);
		picture.pic_width_in_mbs_minus1        = (decoded.width + 15u) / 16u - 1u;
		picture.pic_height_in_map_units_minus1 = (decoded.height + 15u) / 16u - 1u;
		picture.frame_mbs_only_flag            = 1;
		picture.frame_cropping_flag      = decoded.crop_left != 0 || decoded.crop_right != 0 ||
		                                           decoded.crop_top != 0 || decoded.crop_bottom != 0
		                                       ? 1
		                                       : 0;
		picture.frame_crop_left_offset   = decoded.crop_left;
		picture.frame_crop_right_offset  = decoded.crop_right;
		picture.frame_crop_top_offset    = decoded.crop_top;
		picture.frame_crop_bottom_offset = decoded.crop_bottom;
		picture.aspect_ratio_info_present_flag =
		    decoded.sar_width != 0 && decoded.sar_height != 0 ? 1 : 0;
		picture.aspect_ratio_idc               = picture.aspect_ratio_info_present_flag ? 255 : 0;
		picture.sar_width                      = decoded.sar_width;
		picture.sar_height                     = decoded.sar_height;
		picture.video_signal_type_present_flag = 1;
		picture.video_format                   = 5;
		picture.video_full_range_flag          = decoded.color_range == 2 ? 1 : 0;
		picture.colour_description_present_flag =
		    decoded.color_primaries != 0 || decoded.color_trc != 0 || decoded.color_space != 0 ? 1
		                                                                                       : 0;
		picture.colour_primaries         = decoded.color_primaries;
		picture.transfer_characteristics = decoded.color_trc;
		picture.matrix_coefficients      = decoded.color_space;
		std::memcpy(first_picture_info, &picture, requested_size);
	} else {
		const auto result = fill_common(first_picture_info, true);
		if (result != OK) {
			return result;
		}
	}

	if (second_picture_info != nullptr) {
		const auto result = fill_common(second_picture_info, false);
		if (result != OK) {
			return result;
		}
	}
	return OK;
}

LIB_DEFINE(InitVideoDec2_1) {
	PRINT_NAME_ENABLE(true);

	LIB_FUNC("RnDibcGCPKw", QueryComputeMemoryInfo);
	LIB_FUNC("eD+X2SmxUt4", AllocateComputeQueue);
	LIB_FUNC("UvtA3FAiF4Y", ReleaseComputeQueue);
	LIB_FUNC("qqMCwlULR+E", QueryDecoderMemoryInfo);
	LIB_FUNC("CNNRoRYd8XI", CreateDecoder);
	LIB_FUNC("jwImxXRGSKA", DeleteDecoder);
	LIB_FUNC("852F5+q6+iM", Decode);
	LIB_FUNC("l1hXwscLuCY", Flush);
	LIB_FUNC("wJXikG6QFN8", Reset);
	LIB_FUNC("NtXRa3dRzU0", GetPictureInfo);
	LIB_FUNC("kjrLbcyhEiw", GetPictureInfo);
}

} // namespace VideoDec2

// The software video API shares the decoder configuration prefix and memory layout
// with Videodec2, but extends its configuration to 80 bytes on this platform.
namespace Vdecsw {

LIB_VERSION("Vdecsw", 1, "Vdecsw", 1, 1);
using namespace VideoDec2;
constexpr size_t WorkspaceBytes = 256;
constexpr size_t ConfigSize     = 80;

constexpr int32_t OutputPending = static_cast<int32_t>(0x81510115u);
// The input caller retries 0x114 after yielding to the output consumer.
constexpr int32_t InputQueueFull = static_cast<int32_t>(0x81510114u);
constexpr int32_t InputClosed    = static_cast<int32_t>(0x81510113u);
constexpr int32_t InputDrained   = static_cast<int32_t>(0x81510116u);
constexpr int32_t NoCompletion   = static_cast<int32_t>(0x81510117u);

static int32_t SoftwareError(int32_t result) {
	return result == OK
	           ? OK
	           : static_cast<int32_t>(0x81510000u | (static_cast<uint32_t>(result) & 0xffffu));
}

struct SoftwareFrameBuffer {
	size_t this_size;
	void*  frame_buffer;
	size_t frame_buffer_size;
};

struct SoftwareOutput {
	size_t   this_size;
	bool     is_valid;
	bool     is_end_of_stream;
	bool     is_error_frame;
	uint8_t  picture_count;
	uint32_t codec_type;
	uint32_t frame_width;
	uint32_t frame_pitch;
	uint32_t frame_height;
	void*    frame_buffer;
	size_t   frame_buffer_size;
	uint32_t frame_format;
	uint32_t frame_pitch_in_bytes;
};

struct SoftwareInputCompletion {
	size_t   this_size;
	uint64_t reserved;
	uint32_t consumed_count;
	uint32_t padding;
};

static_assert(sizeof(SoftwareFrameBuffer) == 24);
static_assert(sizeof(SoftwareOutput) == 56 && offsetof(SoftwareOutput, picture_count) == 11);
static_assert(sizeof(SoftwareInputCompletion) == 24 &&
              offsetof(SoftwareInputCompletion, consumed_count) == 16);

struct SoftwareInput {
	Videodec2InputData   info;
	std::vector<uint8_t> bytes;
};

struct SoftwareQueue {
	uint32_t                        depth              = 1;
	uint32_t                        consumed           = 0;
	size_t                          queued_input_bytes = 0;
	bool                            finishing          = false;
	bool                            drained            = false;
	int32_t                         error              = OK;
	std::deque<SoftwareInput>       inputs;
	std::deque<SoftwareFrameBuffer> buffers;
	std::deque<Decoder::Output>     outputs;
};

static std::mutex                                          g_software_mutex;
static std::unordered_map<Videodec2Decoder, SoftwareQueue> g_software_queues;

// Run only with g_software_mutex held. An output reservation survives input AUs
// that produce no frame (including decoder reordering delay).
static void Pump(Decoder::Instance* decoder, SoftwareQueue& queue) {
	while (queue.error == OK && !queue.drained && !queue.buffers.empty()) {
		const auto&     buffer = queue.buffers.front();
		Decoder::Output decoded {};
		Decoder::Result result;
		if (!queue.inputs.empty()) {
			const auto& input = queue.inputs.front();
			result = Decoder::Decode(decoder,
			                         {input.bytes.data(), input.bytes.size(), input.info.pts_data,
			                          input.info.dts_data, input.info.attached_data},
			                         {buffer.frame_buffer, buffer.frame_buffer_size}, &decoded);
			queue.queued_input_bytes -= input.bytes.size();
			queue.inputs.pop_front();
		} else if (queue.finishing) {
			result =
			    Decoder::Drain(decoder, {buffer.frame_buffer, buffer.frame_buffer_size}, &decoded);
			if (result == Decoder::Result::Ok && !decoded.valid) queue.drained = true;
		} else {
			break;
		}
		queue.error = SoftwareError(MapDecoderResult(result));
		if (decoded.valid) {
			queue.drained |= decoded.end_of_stream;
			queue.outputs.push_back(decoded);
			queue.buffers.pop_front();
		}
	}
}

static int32_t KYTY_SYSV_ABI SetDecodeInput(Videodec2Decoder          decoder,
                                            const Videodec2InputData* input) {
	if (input == nullptr) return SoftwareError(VIDEODEC2_ERROR_ARGUMENT_POINTER);
	if (input->this_size != sizeof(*input)) return SoftwareError(VIDEODEC2_ERROR_STRUCT_SIZE);
	if (input->au_data == nullptr) return SoftwareError(VIDEODEC2_ERROR_ACCESS_UNIT_POINTER);
	if (input->au_size == 0 || input->au_size > INT32_MAX)
		return SoftwareError(VIDEODEC2_ERROR_ACCESS_UNIT_SIZE);
	std::scoped_lock lock(g_software_mutex);
	const auto       found = g_software_queues.find(decoder);
	if (found == g_software_queues.end()) return SoftwareError(VIDEODEC2_ERROR_DECODER_INSTANCE);
	auto& queue = found->second;
	if (queue.finishing) return InputClosed;
	// Queue depth limits guest inputs awaiting completion. Copied packets no
	// longer borrow those inputs, and must not occupy their submission slots.
	// Bound the private compressed-data backlog separately so paused output
	// can exert backpressure without an unbounded allocation.
	constexpr size_t MaxQueuedInputBytes = 64u * 1024u * 1024u;
	if (queue.consumed >= queue.depth ||
	    (!queue.inputs.empty() &&
	     (queue.queued_input_bytes >= MaxQueuedInputBytes ||
	      input->au_size > MaxQueuedInputBytes - queue.queued_input_bytes))) {
		return InputQueueFull;
	}
	const auto* bytes = static_cast<const uint8_t*>(input->au_data);
	queue.inputs.push_back({*input, std::vector<uint8_t>(bytes, bytes + input->au_size)});
	queue.queued_input_bytes += input->au_size;
	// The queue owns a copy, so the caller can reuse this input buffer immediately.
	// Input completion must not wait for a frame buffer or decoder finalization.
	++queue.consumed;
	Pump(static_cast<Decoder::Instance*>(decoder), queue);
	return queue.error;
}

static int32_t KYTY_SYSV_ABI SetDecodeOutput(Videodec2Decoder           decoder,
                                             const SoftwareFrameBuffer* buffer) {
	if (buffer == nullptr) return SoftwareError(VIDEODEC2_ERROR_ARGUMENT_POINTER);
	if (buffer->this_size != sizeof(*buffer)) return SoftwareError(VIDEODEC2_ERROR_STRUCT_SIZE);
	if (buffer->frame_buffer == nullptr) return SoftwareError(VIDEODEC2_ERROR_FRAME_BUFFER_POINTER);
	if (buffer->frame_buffer_size == 0) return SoftwareError(VIDEODEC2_ERROR_FRAME_BUFFER_SIZE);
	std::scoped_lock lock(g_software_mutex);
	const auto       found = g_software_queues.find(decoder);
	if (found == g_software_queues.end()) return SoftwareError(VIDEODEC2_ERROR_DECODER_INSTANCE);
	auto& queue = found->second;
	queue.buffers.push_back(*buffer);
	Pump(static_cast<Decoder::Instance*>(decoder), queue);
	return queue.error;
}

static int32_t KYTY_SYSV_ABI TrySyncDecodeOutput(Videodec2Decoder decoder, SoftwareOutput* output) {
	if (output == nullptr) return SoftwareError(VIDEODEC2_ERROR_ARGUMENT_POINTER);
	if (output->this_size != sizeof(*output)) return SoftwareError(VIDEODEC2_ERROR_STRUCT_SIZE);
	std::scoped_lock lock(g_software_mutex);
	const auto       found = g_software_queues.find(decoder);
	if (found == g_software_queues.end()) return SoftwareError(VIDEODEC2_ERROR_DECODER_INSTANCE);
	auto& queue = found->second;
	*output     = {.this_size = sizeof(*output)};
	Pump(static_cast<Decoder::Instance*>(decoder), queue);
	if (queue.error != OK) return queue.error;
	if (queue.outputs.empty()) return queue.drained ? NoCompletion : OutputPending;
	const auto decoded = queue.outputs.front();
	queue.outputs.pop_front();
	*output = {.this_size            = sizeof(*output),
	           .is_valid             = true,
	           .is_end_of_stream     = decoded.end_of_stream,
	           .is_error_frame       = decoded.error_frame,
	           .picture_count        = 1,
	           .codec_type           = decoded.codec_type,
	           .frame_width          = decoded.width,
	           .frame_pitch          = decoded.pitch,
	           .frame_height         = decoded.height,
	           .frame_buffer         = decoded.buffer,
	           .frame_buffer_size    = decoded.buffer_size,
	           .frame_pitch_in_bytes = decoded.pitch};
	return OK;
}

static int32_t KYTY_SYSV_ABI TrySyncDecodeInput(Videodec2Decoder         decoder,
                                                SoftwareInputCompletion* completion) {
	if (completion == nullptr) return SoftwareError(VIDEODEC2_ERROR_ARGUMENT_POINTER);
	if (completion->this_size != sizeof(*completion))
		return SoftwareError(VIDEODEC2_ERROR_STRUCT_SIZE);
	std::scoped_lock lock(g_software_mutex);
	const auto       found = g_software_queues.find(decoder);
	if (found == g_software_queues.end()) return SoftwareError(VIDEODEC2_ERROR_DECODER_INSTANCE);
	auto& queue = found->second;
	Pump(static_cast<Decoder::Instance*>(decoder), queue);
	*completion = {.this_size      = sizeof(*completion),
	               .consumed_count = std::exchange(queue.consumed, 0u)};
	if (queue.error != OK) return queue.error;
	if (completion->consumed_count != 0) return OK;
	// No guest input buffers remain borrowed, even when copied AUs still await
	// decoding. Clients use this result to decide when to call FinalizeDecode.
	return InputDrained;
}

static int32_t KYTY_SYSV_ABI FinalizeDecode(Videodec2Decoder decoder) {
	std::scoped_lock lock(g_software_mutex);
	const auto       found = g_software_queues.find(decoder);
	if (found == g_software_queues.end()) return SoftwareError(VIDEODEC2_ERROR_DECODER_INSTANCE);
	auto& queue     = found->second;
	queue.finishing = true;
	Pump(static_cast<Decoder::Instance*>(decoder), queue);
	return queue.error;
}

static int32_t KYTY_SYSV_ABI GetAvcPictureInfo(const SoftwareOutput* output, void* first,
                                               void* second) {
	if (output == nullptr) return SoftwareError(VIDEODEC2_ERROR_ARGUMENT_POINTER);
	if (output->this_size != sizeof(*output)) return SoftwareError(VIDEODEC2_ERROR_STRUCT_SIZE);
	// Vdecsw adds stream completion before the error flag in its output prefix.
	Videodec2OutputInfo common {};
	std::memcpy(&common, output, sizeof(common));
	common.picture_count      = output->picture_count;
	common.is_error_frame     = output->is_error_frame;
	common.is_discarded_frame = false;
	return SoftwareError(VideoDec2::GetPictureInfo(&common, first, second));
}

static int32_t KYTY_SYSV_ABI DeleteSoftwareDecoder(Videodec2Decoder decoder) {
	std::scoped_lock lock(g_software_mutex);
	if (g_software_queues.erase(decoder) == 0)
		return SoftwareError(VIDEODEC2_ERROR_DECODER_INSTANCE);
	return SoftwareError(VideoDec2::DeleteDecoder(decoder));
}

static int32_t KYTY_SYSV_ABI ResetSoftwareDecoder(Videodec2Decoder decoder) {
	std::scoped_lock lock(g_software_mutex);
	const auto       found = g_software_queues.find(decoder);
	if (found == g_software_queues.end()) return SoftwareError(VIDEODEC2_ERROR_DECODER_INSTANCE);
	const auto depth = found->second.depth;
	found->second    = {.depth = depth};
	return SoftwareError(VideoDec2::Reset(decoder));
}

static int32_t ValidateConfig(const Videodec2DecoderConfigInfo* config, bool require_queue) {
	// Vdecsw assigns flags to the two bytes that Videodec2 reserves at offsets 62/63.
	// Validate the shared fields using a copy; do not reject those Vdecsw flags or
	// modify the caller's larger configuration object.
	auto common      = *config;
	common.this_size = sizeof(common);
	common.reserved0 = common.reserved1 = 0;
	return ValidateDecoderConfig(&common, require_queue);
}

static int32_t KYTY_SYSV_ABI QueryComputeMemoryInfo(Videodec2ComputeMemoryInfo* info) {
	if (info == nullptr) return VIDEODEC2_ERROR_ARGUMENT_POINTER;
	if (info->this_size != sizeof(*info)) return VIDEODEC2_ERROR_STRUCT_SIZE;
	// FFmpeg owns its decoding allocations. This guest reservation identifies the
	// compute queue; no GPU compute work area is needed by the software backend.
	info->cpu_gpu_memory_size = WorkspaceBytes;
	info->cpu_gpu_memory      = nullptr;
	return OK;
}

static int32_t KYTY_SYSV_ABI AllocateComputeQueue(const Videodec2ComputeConfigInfo* config,
                                                  const Videodec2ComputeMemoryInfo* memory,
                                                  Videodec2ComputeQueue*            queue) {
	if (config == nullptr || memory == nullptr || queue == nullptr)
		return VIDEODEC2_ERROR_ARGUMENT_POINTER;
	if (config->this_size != sizeof(*config) || memory->this_size != sizeof(*memory))
		return VIDEODEC2_ERROR_STRUCT_SIZE;
	if (config->reserved0 != 0 || config->reserved1 != 0) return VIDEODEC2_ERROR_CONFIG_INFO;
	if (config->compute_pipe_id > 4) return VIDEODEC2_ERROR_COMPUTE_PIPE_ID;
	if (config->compute_queue_id > 7) return VIDEODEC2_ERROR_COMPUTE_QUEUE_ID;
	if (memory->cpu_gpu_memory_size < WorkspaceBytes) return VIDEODEC2_ERROR_MEMORY_SIZE;
	if (memory->cpu_gpu_memory == nullptr) return VIDEODEC2_ERROR_MEMORY_POINTER;
	*queue = memory->cpu_gpu_memory;
	return OK;
}

static int32_t KYTY_SYSV_ABI QueryDecoderMemoryInfo(const Videodec2DecoderConfigInfo* config,
                                                    Videodec2DecoderMemoryInfo*       memory) {
	if (config == nullptr || memory == nullptr) return VIDEODEC2_ERROR_ARGUMENT_POINTER;
	if (config->this_size != ConfigSize || memory->this_size != sizeof(*memory))
		return VIDEODEC2_ERROR_STRUCT_SIZE;
	const auto validation = ValidateConfig(config, false);
	if (validation != OK) return validation;
	const uint64_t width  = config->max_frame_width > 0 ? config->max_frame_width : 4096u;
	const uint64_t height = config->max_frame_height > 0 ? config->max_frame_height : 2160u;
	const auto     pitch  = (width + 255u) & ~uint64_t {255};
	*memory               = {.this_size              = sizeof(*memory),
	                         .cpu_memory_size        = WorkspaceBytes,
	                         .max_frame_buffer_size  = pitch * (height + (height + 1u) / 2u),
	                         .frame_buffer_alignment = 256};
	return OK;
}

static int32_t KYTY_SYSV_ABI CreateDecoder(const Videodec2DecoderConfigInfo* config,
                                           const Videodec2DecoderMemoryInfo* memory,
                                           Videodec2Decoder*                 decoder) {
	if (config == nullptr || memory == nullptr || decoder == nullptr)
		return VIDEODEC2_ERROR_ARGUMENT_POINTER;
	if (config->this_size != ConfigSize || memory->this_size != sizeof(*memory))
		return VIDEODEC2_ERROR_STRUCT_SIZE;
	const auto validation = ValidateConfig(config, true);
	if (validation != OK) return validation;
	if (memory->cpu_memory_size < WorkspaceBytes) return VIDEODEC2_ERROR_MEMORY_SIZE;
	if (memory->cpu_memory == nullptr) return VIDEODEC2_ERROR_MEMORY_POINTER;
	auto* state =
	    Decoder::Create({config->codec_type, config->max_frame_width, config->max_frame_height});
	if (state == nullptr) return VIDEODEC2_ERROR_API_FAIL;
	{
		std::scoped_lock lock(g_decoder_mutex);
		g_decoders.insert(state);
	}
	{
		std::scoped_lock lock(g_software_mutex);
		SoftwareQueue    queue;
		queue.depth = config->decode_input_queue_depth;
		g_software_queues.emplace(state, std::move(queue));
	}
	*decoder = state;
	return OK;
}

LIB_DEFINE(InitVdecsw_1) {
	LIB_FUNC("0moTubWCsTM", Vdecsw::QueryComputeMemoryInfo);
	LIB_FUNC("hIgrg5h4V6s", Vdecsw::AllocateComputeQueue);
	LIB_FUNC("A+2M7EivuOU", Vdecsw::QueryDecoderMemoryInfo);
	LIB_FUNC("+L5ArV1tPGA", Vdecsw::CreateDecoder);
	LIB_FUNC("ecUtPX+dBYk", Vdecsw::DeleteSoftwareDecoder);
	LIB_FUNC("fX-zOOefbbs", VideoDec2::ReleaseComputeQueue);
	LIB_FUNC("veb-YBrOqo0", Vdecsw::ResetSoftwareDecoder);
	LIB_FUNC("aqMiF0AgUYI", Vdecsw::SetDecodeInput);
	LIB_FUNC("rgtMCOpyBSc", Vdecsw::SetDecodeOutput);
	LIB_FUNC("kMBw37oH8nI", Vdecsw::TrySyncDecodeOutput);
	LIB_FUNC("l4sQYy5wPkc", Vdecsw::TrySyncDecodeInput);
	LIB_FUNC("ihNT-uuEAr4", Vdecsw::GetAvcPictureInfo);
	LIB_FUNC("5Y6nZqIZvBg", Vdecsw::FinalizeDecode);
}

} // namespace Vdecsw

} // namespace Libs
