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
};

struct Config {
	uint32_t codec_type = 0;
	int32_t  max_width  = -1;
	int32_t  max_height = -1;
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

class Instance;

[[nodiscard]] bool      IsCodecSupported(uint32_t codec_type);
[[nodiscard]] Instance* Create(const Config& config);
void                    Destroy(Instance* instance);
[[nodiscard]] uint32_t  GetCodecType(const Instance* instance);
[[nodiscard]] Result Decode(Instance* instance, const Input& input, const FrameBuffer& frame_buffer,
                            Output* output);
[[nodiscard]] Result Flush(Instance* instance, const FrameBuffer& frame_buffer, Output* output);
void                 Reset(Instance* instance);
[[nodiscard]] bool   GetPictureInfo(void* frame_buffer, PictureInfo* picture_info);

} // namespace Libs::VideoDec2::Decoder

#endif // EMULATOR_INCLUDE_EMULATOR_LIBS_VIDEODEC2DECODER_H_
