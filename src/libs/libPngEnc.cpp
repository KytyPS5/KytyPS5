#include "common/abi.h"
#include "common/logging/log.h"
#include "libs/libs.h"
#include "loader/symbolDatabase.h"

#include <bit>
#include <cstddef>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <new>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#define STBIWDEF static inline
#define STBI_WRITE_NO_STDIO
#include "stb_image_write.h"

namespace Libs {

LIB_VERSION("PngEnc", 1, "PngEnc", 1, 1);

namespace PngEnc {

constexpr int32_t PNG_ENC_ERROR_INVALID_ADDR   = -2140602111; // 0x80690101
constexpr int32_t PNG_ENC_ERROR_INVALID_SIZE   = -2140602110; // 0x80690102
constexpr int32_t PNG_ENC_ERROR_INVALID_PARAM  = -2140602109; // 0x80690103
constexpr int32_t PNG_ENC_ERROR_INVALID_HANDLE = -2140602108; // 0x80690104
constexpr int32_t PNG_ENC_ERROR_DATA_OVERFLOW  = -2140602096; // 0x80690110
constexpr int32_t PNG_ENC_ERROR_FATAL          = -2140602080; // 0x80690120

constexpr uint32_t PNG_ENC_MAX_IMAGE_WIDTH       = 1000000;
constexpr uint32_t PNG_ENC_MAX_IMAGE_HEIGHT      = 1000000;
constexpr uint32_t PNG_ENC_MAX_FILTER_NUMBER     = 4;
constexpr uint16_t PNG_ENC_COLOR_SPACE_RGB       = 3;
constexpr uint16_t PNG_ENC_COLOR_SPACE_RGBA      = 19;
constexpr uint16_t PNG_ENC_PIXEL_FORMAT_R8G8B8A8 = 0;
constexpr uint16_t PNG_ENC_PIXEL_FORMAT_B8G8R8A8 = 1;
constexpr uint16_t PNG_ENC_FILTER_TYPE_SUB       = 1;
constexpr uint16_t PNG_ENC_FILTER_TYPE_UP        = 2;
constexpr uint16_t PNG_ENC_FILTER_TYPE_AVERAGE   = 4;
constexpr uint16_t PNG_ENC_FILTER_TYPE_PAETH     = 8;

struct PngEncCreateParam {
	uint32_t this_size;
	uint32_t attribute;
	uint32_t max_image_width;
	uint32_t max_filter_number;
};

struct PngEncEncodeParam {
	const uint8_t* image_mem_addr;
	uint8_t*       png_mem_addr;
	uint32_t       image_mem_size;
	uint32_t       png_mem_size;
	uint32_t       image_width;
	uint32_t       image_height;
	uint32_t       image_pitch;
	uint16_t       pixel_format;
	uint16_t       color_space;
	uint16_t       bit_depth;
	uint16_t       clut_number;
	uint16_t       filter_type;
	uint16_t       compression_level;
};

struct PngEncOutputInfo {
	uint32_t data_size;
	uint32_t processed_height;
};

static_assert(sizeof(PngEncCreateParam) == 16);
static_assert(sizeof(PngEncEncodeParam) == 48);

struct PngEncContext {
	uint64_t magic;
	uint32_t max_image_width;
	uint32_t max_filter_number;
};

constexpr uint64_t PNG_ENC_CONTEXT_MAGIC = 0x4b595459504e4745ull; // KYTYPNGE

struct PngEncWriter {
	uint8_t* dst;
	uint32_t capacity;
	uint32_t size;
	bool     overflow;
};

// stb_image_write keeps its compression level and filter choice in globals
static std::mutex g_png_enc_mutex;

static void PngEncWrite(void* context, void* data, int size) {
	auto* writer = static_cast<PngEncWriter*>(context);

	if (writer->overflow || size < 0 ||
	    static_cast<uint64_t>(writer->size) + static_cast<uint32_t>(size) > writer->capacity) {
		writer->overflow = true;
		return;
	}

	std::memcpy(writer->dst + writer->size, data, static_cast<size_t>(size));
	writer->size += static_cast<uint32_t>(size);
}

static int PngEncForcedFilter(uint16_t filter_type) {
	if (filter_type == (PNG_ENC_FILTER_TYPE_SUB | PNG_ENC_FILTER_TYPE_UP |
	                    PNG_ENC_FILTER_TYPE_AVERAGE | PNG_ENC_FILTER_TYPE_PAETH)) {
		return -1;
	}
	// stb's adaptive mode cannot restrict itself to the guest's filter mask.
	if ((filter_type & PNG_ENC_FILTER_TYPE_SUB) != 0) {
		return 1;
	}
	if ((filter_type & PNG_ENC_FILTER_TYPE_UP) != 0) {
		return 2;
	}
	if ((filter_type & PNG_ENC_FILTER_TYPE_AVERAGE) != 0) {
		return 3;
	}
	if ((filter_type & PNG_ENC_FILTER_TYPE_PAETH) != 0) {
		return 4;
	}
	return 0;
}

static int32_t ValidateCreateParam(const PngEncCreateParam* param) {
	if (param == nullptr) {
		return PNG_ENC_ERROR_INVALID_ADDR;
	}

	LOGF("\t this_size         = %" PRIu32 "\n", param->this_size);
	LOGF("\t attribute         = %" PRIu32 "\n", param->attribute);
	LOGF("\t max_image_width   = %" PRIu32 "\n", param->max_image_width);
	LOGF("\t max_filter_number = %" PRIu32 "\n", param->max_filter_number);

	if (param->this_size != sizeof(PngEncCreateParam) || param->attribute != 0 ||
	    param->max_filter_number > PNG_ENC_MAX_FILTER_NUMBER) {
		return PNG_ENC_ERROR_INVALID_PARAM;
	}

	if (param->max_image_width == 0 || param->max_image_width > PNG_ENC_MAX_IMAGE_WIDTH) {
		return PNG_ENC_ERROR_INVALID_SIZE;
	}

	return 0;
}

static int32_t KYTY_SYSV_ABI PngEncQueryMemorySize(const PngEncCreateParam* param) {
	PRINT_NAME();

	if (int32_t result = ValidateCreateParam(param); result != 0) {
		return result;
	}

	return sizeof(PngEncContext);
}

static int32_t KYTY_SYSV_ABI PngEncCreate(const PngEncCreateParam* param, void* memory_address,
                                          uint32_t memory_size, void** handle) {
	PRINT_NAME();

	LOGF("\t memory_address    = %p\n", memory_address);
	LOGF("\t memory_size       = %" PRIu32 "\n", memory_size);

	if (int32_t result = ValidateCreateParam(param); result != 0) {
		return result;
	}

	if (memory_address == nullptr || handle == nullptr) {
		return PNG_ENC_ERROR_INVALID_ADDR;
	}

	if (memory_size < sizeof(PngEncContext)) {
		return PNG_ENC_ERROR_INVALID_SIZE;
	}

	auto* ctx              = static_cast<PngEncContext*>(memory_address);
	ctx->magic             = PNG_ENC_CONTEXT_MAGIC;
	ctx->max_image_width   = param->max_image_width;
	ctx->max_filter_number = param->max_filter_number;
	*handle                = ctx;

	return 0;
}

static int32_t KYTY_SYSV_ABI PngEncEncode(void* handle, const PngEncEncodeParam* param,
                                          PngEncOutputInfo* output_info) {
	PRINT_NAME();

	if (handle == nullptr) {
		return PNG_ENC_ERROR_INVALID_HANDLE;
	}

	const auto* ctx = static_cast<const PngEncContext*>(handle);
	if (ctx->magic != PNG_ENC_CONTEXT_MAGIC) {
		return PNG_ENC_ERROR_INVALID_HANDLE;
	}

	if (param == nullptr) {
		return PNG_ENC_ERROR_INVALID_PARAM;
	}

	LOGF("\t image_mem_addr    = %p\n", static_cast<const void*>(param->image_mem_addr));
	LOGF("\t png_mem_addr      = %p\n", static_cast<const void*>(param->png_mem_addr));
	LOGF("\t image_mem_size    = %" PRIu32 "\n", param->image_mem_size);
	LOGF("\t png_mem_size      = %" PRIu32 "\n", param->png_mem_size);
	LOGF("\t image_width       = %" PRIu32 "\n", param->image_width);
	LOGF("\t image_height      = %" PRIu32 "\n", param->image_height);
	LOGF("\t image_pitch       = %" PRIu32 "\n", param->image_pitch);
	LOGF("\t pixel_format      = %" PRIu16 "\n", param->pixel_format);
	LOGF("\t color_space       = %" PRIu16 "\n", param->color_space);
	LOGF("\t bit_depth         = %" PRIu16 "\n", param->bit_depth);
	LOGF("\t filter_type       = %" PRIu16 "\n", param->filter_type);
	LOGF("\t compression_level = %" PRIu16 "\n", param->compression_level);

	if (param->image_mem_addr == nullptr || param->png_mem_addr == nullptr ||
	    (reinterpret_cast<uintptr_t>(param->image_mem_addr) & 3u) != 0) {
		return PNG_ENC_ERROR_INVALID_ADDR;
	}

	if (param->pixel_format != PNG_ENC_PIXEL_FORMAT_R8G8B8A8 &&
	    param->pixel_format != PNG_ENC_PIXEL_FORMAT_B8G8R8A8) {
		return PNG_ENC_ERROR_INVALID_PARAM;
	}

	if (param->color_space != PNG_ENC_COLOR_SPACE_RGB &&
	    param->color_space != PNG_ENC_COLOR_SPACE_RGBA) {
		return PNG_ENC_ERROR_INVALID_PARAM;
	}

	if (param->bit_depth != 8 || param->clut_number != 0 || param->compression_level > 9) {
		return PNG_ENC_ERROR_INVALID_PARAM;
	}

	constexpr uint16_t filter_mask = PNG_ENC_FILTER_TYPE_SUB | PNG_ENC_FILTER_TYPE_UP |
	                                 PNG_ENC_FILTER_TYPE_AVERAGE | PNG_ENC_FILTER_TYPE_PAETH;
	if ((param->filter_type & ~filter_mask) != 0 ||
	    static_cast<uint32_t>(std::popcount(param->filter_type)) > ctx->max_filter_number) {
		return PNG_ENC_ERROR_INVALID_PARAM;
	}

	const uint32_t width  = param->image_width;
	const uint32_t height = param->image_height;
	if (width == 0 || height == 0 || width > ctx->max_image_width ||
	    width > PNG_ENC_MAX_IMAGE_WIDTH || height > PNG_ENC_MAX_IMAGE_HEIGHT) {
		return PNG_ENC_ERROR_INVALID_SIZE;
	}

	const int      components = (param->color_space == PNG_ENC_COLOR_SPACE_RGBA ? 4 : 3);
	const uint32_t row_size   = width * static_cast<uint32_t>(components);
	const uint32_t min_pitch  = width * 4u;
	const uint32_t pitch      = param->image_pitch;

	// Leave room for deflate expansion and stb's signed-int buffer-capacity doubling.
	constexpr uint64_t max_filtered_size = std::numeric_limits<int>::max() / 2u;
	if (static_cast<uint64_t>(row_size + 1u) * height > max_filtered_size ||
	    param->png_mem_size == 0 || pitch < min_pitch || (pitch & 3u) != 0 ||
	    static_cast<uint64_t>(pitch) * height > param->image_mem_size) {
		return PNG_ENC_ERROR_INVALID_SIZE;
	}

	const bool                 bgr = (param->pixel_format == PNG_ENC_PIXEL_FORMAT_B8G8R8A8);
	std::unique_ptr<uint8_t[]> pixels(new (std::nothrow)
	                                      uint8_t[static_cast<size_t>(row_size) * height]);
	if (pixels == nullptr) {
		return PNG_ENC_ERROR_FATAL;
	}

	for (uint32_t y = 0; y < height; y++) {
		const auto* src = param->image_mem_addr + static_cast<size_t>(y) * pitch;
		auto*       dst = pixels.get() + static_cast<size_t>(y) * row_size;

		for (uint32_t x = 0; x < width; x++) {
			dst[0] = src[bgr ? 2 : 0];
			dst[1] = src[1];
			dst[2] = src[bgr ? 0 : 2];
			if (components == 4) {
				dst[3] = src[3];
			}

			src += 4;
			dst += components;
		}
	}

	PngEncWriter writer {param->png_mem_addr, param->png_mem_size, 0, false};

	int ok = 0;
	{
		std::scoped_lock lock(g_png_enc_mutex);

		stbi_write_png_compression_level = param->compression_level;
		stbi_write_force_png_filter      = PngEncForcedFilter(param->filter_type);

		ok = stbi_write_png_to_func(PngEncWrite, &writer, static_cast<int>(width),
		                            static_cast<int>(height), components, pixels.get(),
		                            static_cast<int>(row_size));
	}

	if (output_info != nullptr) {
		output_info->data_size        = (writer.overflow ? 0 : writer.size);
		output_info->processed_height = (ok != 0 && !writer.overflow ? height : 0);
	}

	if (ok == 0) {
		LOGF("\t encode failed\n");
		return PNG_ENC_ERROR_FATAL;
	}

	if (writer.overflow) {
		LOGF("\t output buffer too small\n");
		return PNG_ENC_ERROR_DATA_OVERFLOW;
	}

	LOGF("\t data_size = %" PRIu32 "\n", writer.size);

	return static_cast<int32_t>(writer.size);
}

static int32_t KYTY_SYSV_ABI PngEncDelete(void* handle) {
	PRINT_NAME();

	if (handle == nullptr) {
		return PNG_ENC_ERROR_INVALID_HANDLE;
	}

	auto* ctx = static_cast<PngEncContext*>(handle);
	if (ctx->magic != PNG_ENC_CONTEXT_MAGIC) {
		return PNG_ENC_ERROR_INVALID_HANDLE;
	}

	ctx->magic = 0;
	return 0;
}

} // namespace PngEnc

LIB_DEFINE(InitPngEnc_1) {
	PRINT_NAME_ENABLE(true);

	LIB_FUNC("9030RnBDoh4", PngEnc::PngEncQueryMemorySize);
	LIB_FUNC("7aGTPfrqT9s", PngEnc::PngEncCreate);
	LIB_FUNC("xgDjJKpcyHo", PngEnc::PngEncEncode);
	LIB_FUNC("RUrWdwTWZy8", PngEnc::PngEncDelete);
}

} // namespace Libs
