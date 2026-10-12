#ifndef EMULATOR_INCLUDE_EMULATOR_LIBS_VIDEODEC2_H_
#define EMULATOR_INCLUDE_EMULATOR_LIBS_VIDEODEC2_H_

#include "libs/videoDec2Decoder.h"

#include <cstddef>
#include <cstdint>

// Guest ABI shared by the Videodec2 and Vdecsw libraries, which decode through the same backend.
namespace Libs::VideoDec2 {

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

static_assert(sizeof(Videodec2ComputeMemoryInfo) == 24);
static_assert(sizeof(Videodec2ComputeConfigInfo) == 16);
static_assert(sizeof(Videodec2DecoderConfigInfo) == 72);
static_assert(sizeof(Videodec2DecoderMemoryInfo) == 72);
static_assert(sizeof(Videodec2InputData) == 48);

[[nodiscard]] int32_t ValidateDecoderConfig(const Videodec2DecoderConfigInfo* config,
                                            bool require_compute_queue);
[[nodiscard]] int32_t ValidateComputeQueueConfig(const Videodec2ComputeConfigInfo* config);
[[nodiscard]] int32_t MapDecoderResult(Decoder::Result result);
// Fills the codec's guest picture-info structures for a picture decoded into frame_buffer.
[[nodiscard]] int32_t WritePictureInfo(void* frame_buffer, uint32_t codec_type,
                                       void* first_picture_info, void* second_picture_info);

} // namespace Libs::VideoDec2

#endif // EMULATOR_INCLUDE_EMULATOR_LIBS_VIDEODEC2_H_
