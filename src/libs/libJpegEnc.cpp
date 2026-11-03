#include "common/abi.h"
#include "common/logging/log.h"
#include "libs/libs.h"
#include "loader/symbolDatabase.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <vector>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#define STBIWDEF static inline
#define STBI_WRITE_NO_STDIO
#include "stb_image_write.h"

namespace Libs {

LIB_VERSION("JpegEnc", 1, "JpegEnc", 1, 1);

namespace JpegEnc {

constexpr int32_t JPEG_ENC_ERROR_INVALID_ADDR   = static_cast<int32_t>(0x80650101u);
constexpr int32_t JPEG_ENC_ERROR_INVALID_SIZE   = static_cast<int32_t>(0x80650102u);
constexpr int32_t JPEG_ENC_ERROR_INVALID_PARAM  = static_cast<int32_t>(0x80650103u);
constexpr int32_t JPEG_ENC_ERROR_INVALID_HANDLE = static_cast<int32_t>(0x80650104u);
constexpr int32_t JPEG_ENC_ERROR_DATA_OVERFLOW  = static_cast<int32_t>(0x80650110u);

constexpr uint16_t JPEG_ENC_PIXEL_FORMAT_R8G8B8A8 = 0;
constexpr uint16_t JPEG_ENC_PIXEL_FORMAT_B8G8R8A8 = 1;
constexpr uint16_t JPEG_ENC_PIXEL_FORMAT_Y8U8Y8V8 = 10;
constexpr uint16_t JPEG_ENC_PIXEL_FORMAT_Y8       = 11;
constexpr uint16_t JPEG_ENC_ENCODE_MODE_MJPEG     = 1;
constexpr uint16_t JPEG_ENC_COLOR_SPACE_YCC       = 1;
constexpr uint16_t JPEG_ENC_COLOR_SPACE_GRAYSCALE = 2;
constexpr uint8_t  JPEG_ENC_SAMPLING_TYPE_FULL    = 0;
constexpr uint8_t  JPEG_ENC_SAMPLING_TYPE_422     = 1;
constexpr uint8_t  JPEG_ENC_SAMPLING_TYPE_420     = 2;

struct JpegEncCreateParam {
	uint32_t this_size;
	uint32_t attribute;
};

struct JpegEncEncodeParam {
	const uint8_t* image_mem_addr;
	uint8_t*       jpeg_mem_addr;
	uint32_t       image_mem_size;
	uint32_t       jpeg_mem_size;
	uint32_t       image_width;
	uint32_t       image_height;
	uint32_t       image_pitch;
	uint16_t       pixel_format;
	uint16_t       encode_mode;
	uint16_t       color_space;
	uint8_t        sampling_type;
	uint8_t        compression_ratio;
	int32_t        restart_interval;
};

struct JpegEncOutputInfo {
	uint32_t data_size;
	uint32_t processed_height;
};

static_assert(sizeof(JpegEncCreateParam) == 8 && sizeof(JpegEncEncodeParam) == 48);

struct JpegEncContext {
	uint64_t magic;
};

constexpr uint64_t JPEG_ENC_CONTEXT_MAGIC = 0x4b5954594a504745ull; // KYTYJPGE

struct JpegEncWriter {
	uint8_t* dst;
	uint32_t capacity;
	uint32_t size;
	bool     overflow;
};

static int32_t CheckCreateParam(const JpegEncCreateParam* param) {
	if (param == nullptr) {
		return JPEG_ENC_ERROR_INVALID_ADDR;
	}
	if (param->this_size != sizeof(JpegEncCreateParam)) {
		return JPEG_ENC_ERROR_INVALID_SIZE;
	}
	return param->attribute != 0 ? JPEG_ENC_ERROR_INVALID_PARAM : 0;
}

static void JpegEncWrite(void* context, void* data, int size) {
	auto* writer = static_cast<JpegEncWriter*>(context);
	if (writer->overflow || static_cast<uint32_t>(size) > writer->capacity - writer->size) {
		writer->overflow = true;
		return;
	}
	std::memcpy(writer->dst + writer->size, data, size);
	writer->size += size;
}

static int32_t KYTY_SYSV_ABI JpegEncQueryMemorySize(const JpegEncCreateParam* param) {
	PRINT_NAME();
	const auto result = CheckCreateParam(param);
	return result != 0 ? result : static_cast<int32_t>(sizeof(JpegEncContext));
}

static int32_t KYTY_SYSV_ABI JpegEncCreate(const JpegEncCreateParam* param, void* memory_address,
                                           uint32_t memory_size, void** handle) {
	PRINT_NAME();
	if (const auto result = CheckCreateParam(param); result != 0) {
		return result;
	}
	if (memory_address == nullptr || handle == nullptr) {
		return JPEG_ENC_ERROR_INVALID_ADDR;
	}
	if (memory_size < sizeof(JpegEncContext)) {
		return JPEG_ENC_ERROR_INVALID_SIZE;
	}
	static_cast<JpegEncContext*>(memory_address)->magic = JPEG_ENC_CONTEXT_MAGIC;
	*handle                                             = memory_address;
	return 0;
}

static int32_t KYTY_SYSV_ABI JpegEncDelete(void* handle) {
	PRINT_NAME();
	auto* ctx = static_cast<JpegEncContext*>(handle);
	if (ctx == nullptr || ctx->magic != JPEG_ENC_CONTEXT_MAGIC) {
		return JPEG_ENC_ERROR_INVALID_HANDLE;
	}
	ctx->magic = 0;
	return 0;
}

// stb_image_write picks the chroma sampling from the quality, always writes three components and
// full headers, and writes no restart markers; these only change the bytes of a valid JPEG.
static int32_t KYTY_SYSV_ABI JpegEncEncode(void* handle, const JpegEncEncodeParam* param,
                                           JpegEncOutputInfo* output_info) {
	PRINT_NAME();
	const auto* ctx = static_cast<const JpegEncContext*>(handle);
	if (ctx == nullptr || ctx->magic != JPEG_ENC_CONTEXT_MAGIC) {
		return JPEG_ENC_ERROR_INVALID_HANDLE;
	}
	if (param == nullptr || param->image_mem_addr == nullptr || param->jpeg_mem_addr == nullptr ||
	    (param->pixel_format != JPEG_ENC_PIXEL_FORMAT_Y8 &&
	     (reinterpret_cast<uintptr_t>(param->image_mem_addr) & 3u) != 0)) {
		return JPEG_ENC_ERROR_INVALID_ADDR;
	}
	const auto     format = param->pixel_format;
	const uint32_t width  = param->image_width;
	const uint32_t height = param->image_height;
	LOGF("\t %" PRIu32 "x%" PRIu32 ", pitch = %" PRIu32 ", pixel_format = %" PRIu16
	     ", mode = %" PRIu16 ", color_space = %" PRIu16 ", sampling = %" PRIu8 ", ratio = %" PRIu8
	     ", restart = %" PRId32 "\n",
	     width, height, param->image_pitch, format, param->encode_mode, param->color_space,
	     param->sampling_type, param->compression_ratio, param->restart_interval);
	const bool gray = param->color_space == JPEG_ENC_COLOR_SPACE_GRAYSCALE;
	uint32_t   row  = width * 4;
	if (format == JPEG_ENC_PIXEL_FORMAT_Y8U8Y8V8) {
		row = (width + 1) / 2 * 4;
	} else if (format == JPEG_ENC_PIXEL_FORMAT_Y8) {
		row = width;
	}
	// Grayscale takes full sampling; YCbCr takes 4:2:2 or 4:2:0 from a color format.
	const auto sampling = param->sampling_type;
	const bool ycc =
	    param->color_space == JPEG_ENC_COLOR_SPACE_YCC && format != JPEG_ENC_PIXEL_FORMAT_Y8 &&
	    (sampling == JPEG_ENC_SAMPLING_TYPE_422 || sampling == JPEG_ENC_SAMPLING_TYPE_420);
	if (width == 0 || width > 0xffff || height == 0 || height > 0xffff ||
	    param->image_pitch < row ||
	    (format != JPEG_ENC_PIXEL_FORMAT_Y8 && (param->image_pitch & 3u) != 0) ||
	    (format != JPEG_ENC_PIXEL_FORMAT_R8G8B8A8 && format != JPEG_ENC_PIXEL_FORMAT_B8G8R8A8 &&
	     format != JPEG_ENC_PIXEL_FORMAT_Y8U8Y8V8 && format != JPEG_ENC_PIXEL_FORMAT_Y8) ||
	    param->encode_mode > JPEG_ENC_ENCODE_MODE_MJPEG ||
	    !(ycc || (gray && sampling == JPEG_ENC_SAMPLING_TYPE_FULL)) ||
	    param->restart_interval < -1 || param->restart_interval > 0xffff) {
		return JPEG_ENC_ERROR_INVALID_PARAM;
	}
	if (param->jpeg_mem_size == 0 ||
	    static_cast<uint64_t>(param->image_pitch) * height > param->image_mem_size) {
		return JPEG_ENC_ERROR_INVALID_SIZE;
	}

	const uint32_t       components = gray ? 1 : 3;
	std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * components);
	auto*                dst = pixels.data();
	for (uint32_t y = 0; y < height; y++) {
		const auto* src = param->image_mem_addr + static_cast<size_t>(y) * param->image_pitch;
		for (uint32_t x = 0; x < width; x++) {
			int rgb[3] = {src[x], src[x], src[x]};
			if (format == JPEG_ENC_PIXEL_FORMAT_Y8U8Y8V8) {
				// JFIF YCbCr to RGB from the YUYV pair that holds this pixel; gray keeps the luma.
				const auto* pair = src + x / 2 * 4;
				const int   luma = pair[x % 2 * 2] * 256 + 128;
				const int   cb   = gray ? 0 : pair[1] - 128;
				const int   cr   = gray ? 0 : pair[3] - 128;
				rgb[0]           = std::clamp((luma + 359 * cr) >> 8, 0, 255);
				rgb[1]           = std::clamp((luma - 88 * cb - 183 * cr) >> 8, 0, 255);
				rgb[2]           = std::clamp((luma + 454 * cb) >> 8, 0, 255);
			} else if (format != JPEG_ENC_PIXEL_FORMAT_Y8) {
				const auto* p   = src + x * 4;
				const bool  bgr = format == JPEG_ENC_PIXEL_FORMAT_B8G8R8A8;
				rgb[0]          = p[bgr ? 2 : 0];
				rgb[1]          = p[1];
				rgb[2]          = p[bgr ? 0 : 2];
			}
			if (gray) {
				*dst++ =
				    static_cast<uint8_t>((77 * rgb[0] + 150 * rgb[1] + 29 * rgb[2] + 128) >> 8);
				continue;
			}
			for (const int c: rgb) {
				*dst++ = static_cast<uint8_t>(c);
			}
		}
	}

	// A lower compression ratio means higher quality: 0 maps to stb quality 100 and 255 to 1.
	JpegEncWriter writer {param->jpeg_mem_addr, param->jpeg_mem_size, 0, false};
	stbi_write_jpg_to_func(JpegEncWrite, &writer, static_cast<int>(width), static_cast<int>(height),
	                       static_cast<int>(components), pixels.data(),
	                       100 - param->compression_ratio * 99 / 255);
	if (output_info != nullptr) {
		output_info->data_size        = writer.size;
		output_info->processed_height = writer.overflow ? 0 : height;
	}
	return writer.overflow ? JPEG_ENC_ERROR_DATA_OVERFLOW : static_cast<int32_t>(writer.size);
}

} // namespace JpegEnc

LIB_DEFINE(InitJpegEnc_1) {
	PRINT_NAME_ENABLE(true);

	LIB_FUNC("o6ZgXfFdWXQ", JpegEnc::JpegEncQueryMemorySize);
	LIB_FUNC("K+rocojkr-I", JpegEnc::JpegEncCreate);
	LIB_FUNC("QbrU0cUghEM", JpegEnc::JpegEncEncode);
	LIB_FUNC("j1LyMdaM+C0", JpegEnc::JpegEncDelete);
}

} // namespace Libs
