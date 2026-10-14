#ifndef EMULATOR_INCLUDE_EMULATOR_LIBS_VIDEODEC2DECODER_H_
#define EMULATOR_INCLUDE_EMULATOR_LIBS_VIDEODEC2DECODER_H_

#include <cstddef>
#include <cstdint>

namespace Libs::VideoDec2::Decoder {

constexpr uint64_t TIMESTAMP_INVALID = UINT64_MAX;

enum class Result {
	Ok,
	ApiFail,
	AccessUnit,
	FrameBufferSize,
	OversizeDecode,
	InputFull,
	NoFrame,
};

struct Config {
	uint32_t codec_type         = 0;
	int32_t  max_width          = -1;
	int32_t  max_height         = -1;
	uint32_t max_pending_frames = 2;
};

struct Input {
	const void* data          = nullptr;
	size_t      size          = 0;
	uint64_t    pts           = TIMESTAMP_INVALID;
	uint64_t    dts           = TIMESTAMP_INVALID;
	uint64_t    attached_data = 0;
};

struct FrameBuffer {
	void*  data = nullptr;
	size_t size = 0;
};

struct Output {
	bool     valid           = false;
	bool     last_frame      = false;
	bool     error_frame     = false;
	bool     buffer_accepted = false;
	uint32_t codec_type      = 0;
	uint32_t width           = 0;
	uint32_t pitch           = 0;
	uint32_t height          = 0;
	void*    buffer          = nullptr;
	size_t   buffer_size     = 0;
};

struct PictureInfo {
	uint64_t pts             = TIMESTAMP_INVALID;
	uint64_t dts             = TIMESTAMP_INVALID;
	uint64_t attached_data   = 0;
	uint32_t codec_type      = 0;
	uint32_t width           = 0;
	uint32_t height          = 0;
	uint32_t crop_left       = 0;
	uint32_t crop_right      = 0;
	uint32_t crop_top        = 0;
	uint32_t crop_bottom     = 0;
	uint32_t profile         = 0;
	uint32_t level           = 0;
	uint16_t sar_width       = 0;
	uint16_t sar_height      = 0;
	uint8_t  color_range     = 0;
	uint8_t  color_primaries = 0;
	uint8_t  color_trc       = 0;
	uint8_t  color_space     = 0;
	bool     key_frame       = false;
};

struct AvcPictureInfo {
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

static_assert(sizeof(AvcPictureInfo) == 120);

[[nodiscard]] AvcPictureInfo MakeAvcPictureInfo(const PictureInfo& decoded, size_t size);

class Instance;

[[nodiscard]] bool      IsCodecSupported(uint32_t codec_type);
[[nodiscard]] Instance* Create(const Config& config);
void                    Destroy(Instance* instance);
[[nodiscard]] uint32_t  GetCodecType(const Instance* instance);
[[nodiscard]] Result   Submit(Instance* instance, const Input& input, uint32_t* produced);
[[nodiscard]] Result   Receive(Instance* instance, const FrameBuffer& frame_buffer, Output* output);
[[nodiscard]] Result   Finalize(Instance* instance);
[[nodiscard]] Result Decode(Instance* instance, const Input& input, const FrameBuffer& frame_buffer,
                            Output* output);
[[nodiscard]] Result Flush(Instance* instance, const FrameBuffer& frame_buffer, Output* output);
void                 Reset(Instance* instance);
[[nodiscard]] bool   GetPictureInfo(void* frame_buffer, PictureInfo* picture_info);

} // namespace Libs::VideoDec2::Decoder

#endif // EMULATOR_INCLUDE_EMULATOR_LIBS_VIDEODEC2DECODER_H_
