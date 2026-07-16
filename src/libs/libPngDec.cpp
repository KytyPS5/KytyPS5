#include "common/abi.h"
#include "common/logging/log.h"
#include "libs/libs.h"
#include "loader/symbolDatabase.h"
#include "stb_image.h"

#include <algorithm>
#include <cstddef>
#include <cstring>

namespace Libs {

LIB_VERSION("PngDec", 1, "PngDec", 1, 1);

namespace PngDec {

constexpr int32_t PNG_DEC_ERROR_INVALID_ADDR        = -2140602367; // 0x80690001
constexpr int32_t PNG_DEC_ERROR_INVALID_SIZE        = -2140602366; // 0x80690002
constexpr int32_t PNG_DEC_ERROR_INVALID_PARAM       = -2140602365; // 0x80690003
constexpr int32_t PNG_DEC_ERROR_INVALID_HANDLE      = -2140602364; // 0x80690004
constexpr int32_t PNG_DEC_ERROR_INVALID_WORK_MEMORY = -2140602363; // 0x80690005
constexpr int32_t PNG_DEC_ERROR_INVALID_DATA        = -2140602352; // 0x80690010
constexpr int32_t PNG_DEC_ERROR_DECODE_ERROR        = -2140602350; // 0x80690012

constexpr uint32_t PNG_DEC_ATTRIBUTE_BIT_DEPTH_16      = 1;
constexpr uint16_t PNG_DEC_COLOR_SPACE_GRAYSCALE       = 2;
constexpr uint16_t PNG_DEC_COLOR_SPACE_RGB             = 3;
constexpr uint16_t PNG_DEC_COLOR_SPACE_CLUT            = 4;
constexpr uint16_t PNG_DEC_COLOR_SPACE_GRAYSCALE_ALPHA = 18;
constexpr uint16_t PNG_DEC_COLOR_SPACE_RGBA            = 19;
constexpr uint16_t PNG_DEC_PIXEL_FORMAT_R8G8B8A8       = 0;
constexpr uint16_t PNG_DEC_PIXEL_FORMAT_B8G8R8A8       = 1;
constexpr uint32_t PNG_DEC_IMAGE_FLAG_ADAM7_INTERLACE  = 1;
constexpr uint32_t PNG_DEC_IMAGE_FLAG_TRNS_CHUNK_EXIST = 2;

struct PngDecCreateParam {
	uint32_t this_size;
	uint32_t attribute;
	uint32_t max_image_width;
};

struct PngDecDecodeParam {
	const void* png_mem_addr;
	void*       image_mem_addr;
	uint32_t    png_mem_size;
	uint32_t    image_mem_size;
	uint16_t    pixel_format;
	uint16_t    alpha_value;
	uint32_t    image_pitch;
};

struct PngDecParseParam {
	const void* png_mem_addr;
	uint32_t    png_mem_size;
	uint32_t    reserved0;
};

struct PngDecImageInfo {
	uint32_t image_width;
	uint32_t image_height;
	uint16_t color_space;
	uint16_t bit_depth;
	uint32_t image_flag;
};

struct PngDecContext {
	uint64_t magic;
};

struct PngHeaderInfo {
	uint32_t width;
	uint32_t height;
	uint16_t color_space;
	uint16_t bit_depth;
	uint32_t image_flag;
};

constexpr uint64_t PNG_DEC_CONTEXT_MAGIC = 0x4b595459504e4744ull; // KYTYPNGD

static uint32_t ReadBe32(const uint8_t* ptr) {
	return (static_cast<uint32_t>(ptr[0]) << 24u) | (static_cast<uint32_t>(ptr[1]) << 16u) |
	       (static_cast<uint32_t>(ptr[2]) << 8u) | static_cast<uint32_t>(ptr[3]);
}

static bool AddOverflow(uint32_t a, uint32_t b, uint32_t* out) {
	*out = a + b;
	return *out < a;
}

static bool ParsePngHeader(const void* data, uint32_t size, PngHeaderInfo* info) {
	if (data == nullptr || info == nullptr) {
		return false;
	}

	const auto*       bytes           = static_cast<const uint8_t*>(data);
	constexpr uint8_t png_signature[] = {0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a};

	if (size < 33u || std::memcmp(bytes, png_signature, sizeof(png_signature)) != 0) {
		return false;
	}

	if (ReadBe32(bytes + 8u) != 13u || std::memcmp(bytes + 12u, "IHDR", 4u) != 0) {
		return false;
	}

	const auto color_type = bytes[25u];

	switch (color_type) {
		case 0: info->color_space = PNG_DEC_COLOR_SPACE_GRAYSCALE; break;
		case 2: info->color_space = PNG_DEC_COLOR_SPACE_RGB; break;
		case 3: info->color_space = PNG_DEC_COLOR_SPACE_CLUT; break;
		case 4: info->color_space = PNG_DEC_COLOR_SPACE_GRAYSCALE_ALPHA; break;
		case 6: info->color_space = PNG_DEC_COLOR_SPACE_RGBA; break;
		default: return false;
	}

	info->width      = ReadBe32(bytes + 16u);
	info->height     = ReadBe32(bytes + 20u);
	info->bit_depth  = bytes[24u];
	info->image_flag = (bytes[28u] == 1u ? PNG_DEC_IMAGE_FLAG_ADAM7_INTERLACE : 0u);

	if (info->width == 0 || info->height == 0) {
		return false;
	}

	uint32_t offset = 33u;
	while (offset + 12u <= size) {
		const uint32_t length = ReadBe32(bytes + offset);
		const auto*    type   = bytes + offset + 4u;

		uint32_t chunk_end = 0;
		if (AddOverflow(offset, 12u, &chunk_end) || AddOverflow(chunk_end, length, &chunk_end) ||
		    chunk_end > size) {
			return false;
		}

		if (std::memcmp(type, "tRNS", 4u) == 0) {
			info->image_flag |= PNG_DEC_IMAGE_FLAG_TRNS_CHUNK_EXIST;
		}

		if (std::memcmp(type, "IDAT", 4u) == 0 || std::memcmp(type, "IEND", 4u) == 0) {
			break;
		}

		offset = chunk_end;
	}

	return true;
}

static void FillImageInfo(const PngHeaderInfo& header, PngDecImageInfo* image_info) {
	if (image_info != nullptr) {
		image_info->image_width  = header.width;
		image_info->image_height = header.height;
		image_info->color_space  = header.color_space;
		image_info->bit_depth    = header.bit_depth;
		image_info->image_flag   = header.image_flag;
	}
}

static bool PngHasSourceAlpha(const PngHeaderInfo& header) {
	return header.color_space == PNG_DEC_COLOR_SPACE_RGBA ||
	       header.color_space == PNG_DEC_COLOR_SPACE_GRAYSCALE_ALPHA ||
	       (header.image_flag & PNG_DEC_IMAGE_FLAG_TRNS_CHUNK_EXIST) != 0;
}

static int32_t KYTY_SYSV_ABI PngDecQueryMemorySize(const PngDecCreateParam* param) {
	PRINT_NAME();

	if (param == nullptr) {
		return PNG_DEC_ERROR_INVALID_PARAM;
	}

	LOGF("\t this_size       = %" PRIu32 "\n", param->this_size);
	LOGF("\t attribute       = %" PRIu32 "\n", param->attribute);
	LOGF("\t max_image_width = %" PRIu32 "\n", param->max_image_width);

	if (param->attribute > PNG_DEC_ATTRIBUTE_BIT_DEPTH_16) {
		return PNG_DEC_ERROR_INVALID_PARAM;
	}

	if (param->max_image_width == 0) {
		return PNG_DEC_ERROR_INVALID_SIZE;
	}

	return sizeof(PngDecContext);
}

static int32_t KYTY_SYSV_ABI PngDecCreate(const PngDecCreateParam* param, void* memory_address,
                                          uint32_t memory_size, void** handle) {
	PRINT_NAME();

	if (param == nullptr || handle == nullptr) {
		return PNG_DEC_ERROR_INVALID_PARAM;
	}

	LOGF("\t memory_address  = %p\n", memory_address);
	LOGF("\t memory_size     = %" PRIu32 "\n", memory_size);

	if (param->attribute > PNG_DEC_ATTRIBUTE_BIT_DEPTH_16) {
		return PNG_DEC_ERROR_INVALID_PARAM;
	}

	if (param->max_image_width == 0) {
		return PNG_DEC_ERROR_INVALID_SIZE;
	}

	if (memory_address == nullptr) {
		return PNG_DEC_ERROR_INVALID_ADDR;
	}

	if (memory_size < sizeof(PngDecContext)) {
		return PNG_DEC_ERROR_INVALID_WORK_MEMORY;
	}

	auto* ctx  = static_cast<PngDecContext*>(memory_address);
	ctx->magic = PNG_DEC_CONTEXT_MAGIC;
	*handle    = ctx;

	return 0;
}

static int32_t KYTY_SYSV_ABI PngDecDecode(void* handle, const PngDecDecodeParam* param,
                                          PngDecImageInfo* image_info) {
	PRINT_NAME();

	if (handle == nullptr) {
		return PNG_DEC_ERROR_INVALID_HANDLE;
	}

	const auto* ctx = static_cast<const PngDecContext*>(handle);
	if (ctx->magic != PNG_DEC_CONTEXT_MAGIC) {
		return PNG_DEC_ERROR_INVALID_HANDLE;
	}

	if (param == nullptr) {
		return PNG_DEC_ERROR_INVALID_PARAM;
	}

	if (param->png_mem_addr == nullptr || param->image_mem_addr == nullptr) {
		return PNG_DEC_ERROR_INVALID_ADDR;
	}

	if (param->pixel_format != PNG_DEC_PIXEL_FORMAT_R8G8B8A8 &&
	    param->pixel_format != PNG_DEC_PIXEL_FORMAT_B8G8R8A8) {
		return PNG_DEC_ERROR_INVALID_PARAM;
	}

	PngHeaderInfo header {};
	if (!ParsePngHeader(param->png_mem_addr, param->png_mem_size, &header)) {
		return PNG_DEC_ERROR_INVALID_DATA;
	}

	FillImageInfo(header, image_info);

	const uint32_t min_pitch = header.width * 4u;
	const uint32_t pitch     = (param->image_pitch == 0 ? min_pitch : param->image_pitch);
	const uint64_t min_size  = static_cast<uint64_t>(pitch) * (header.height - 1u) + min_pitch;

	if (pitch < min_pitch || min_size > param->image_mem_size) {
		return PNG_DEC_ERROR_INVALID_SIZE;
	}

	LOGF("\t png_mem_addr   = %p\n", param->png_mem_addr);
	LOGF("\t image_mem_addr = %p\n", param->image_mem_addr);
	LOGF("\t png_mem_size   = %" PRIu32 "\n", param->png_mem_size);
	LOGF("\t image_mem_size = %" PRIu32 "\n", param->image_mem_size);
	LOGF("\t pixel_format   = %" PRIu16 "\n", param->pixel_format);
	LOGF("\t alpha_value    = %" PRIu16 "\n", param->alpha_value);
	LOGF("\t image_pitch    = %" PRIu32 "\n", param->image_pitch);

	int   width      = 0;
	int   height     = 0;
	int   components = 0;
	auto* rgba       = stbi_load_from_memory(static_cast<const stbi_uc*>(param->png_mem_addr),
	                                         static_cast<int>(param->png_mem_size), &width, &height,
	                                         &components, 4);

	if (rgba == nullptr || width <= 0 || height <= 0 ||
	    static_cast<uint32_t>(width) != header.width ||
	    static_cast<uint32_t>(height) != header.height) {
		if (rgba != nullptr) {
			stbi_image_free(rgba);
		}
		return PNG_DEC_ERROR_DECODE_ERROR;
	}

	const bool apply_alpha_value = !PngHasSourceAlpha(header);
	auto*      dst_row           = static_cast<uint8_t*>(param->image_mem_addr);

	for (uint32_t y = 0; y < header.height; y++) {
		const auto* src = rgba + static_cast<size_t>(y) * min_pitch;
		auto*       dst = dst_row;

		for (uint32_t x = 0; x < header.width; x++) {
			dst[0] = src[0];
			dst[1] = src[1];
			dst[2] = src[2];
			dst[3] = (apply_alpha_value
			              ? static_cast<uint8_t>(std::min<uint16_t>(param->alpha_value, 255u))
			              : src[3]);

			if (param->pixel_format == PNG_DEC_PIXEL_FORMAT_B8G8R8A8) {
				std::swap(dst[0], dst[2]);
			}

			src += 4;
			dst += 4;
		}

		dst_row += pitch;
	}

	stbi_image_free(rgba);

	return (header.width > 32767u || header.height > 32767u
	            ? 0
	            : static_cast<int32_t>((header.width << 16u) | header.height));
}

static int32_t KYTY_SYSV_ABI PngDecDelete(void* handle) {
	PRINT_NAME();

	if (handle == nullptr) {
		return PNG_DEC_ERROR_INVALID_HANDLE;
	}

	auto* ctx = static_cast<PngDecContext*>(handle);
	if (ctx->magic != PNG_DEC_CONTEXT_MAGIC) {
		return PNG_DEC_ERROR_INVALID_HANDLE;
	}

	ctx->magic = 0;
	return 0;
}

static int32_t KYTY_SYSV_ABI PngDecParseHeader(const PngDecParseParam* param,
                                               PngDecImageInfo*        image_info) {
	PRINT_NAME();

	if (param == nullptr || image_info == nullptr) {
		return PNG_DEC_ERROR_INVALID_PARAM;
	}

	if (param->png_mem_addr == nullptr) {
		return PNG_DEC_ERROR_INVALID_ADDR;
	}

	if (param->reserved0 != 0) {
		return PNG_DEC_ERROR_INVALID_PARAM;
	}

	PngHeaderInfo header {};
	if (!ParsePngHeader(param->png_mem_addr, param->png_mem_size, &header)) {
		return PNG_DEC_ERROR_INVALID_DATA;
	}

	FillImageInfo(header, image_info);

	LOGF("\t image_width  = %" PRIu32 "\n", image_info->image_width);
	LOGF("\t image_height = %" PRIu32 "\n", image_info->image_height);
	LOGF("\t color_space  = %" PRIu16 "\n", image_info->color_space);
	LOGF("\t bit_depth    = %" PRIu16 "\n", image_info->bit_depth);
	LOGF("\t image_flag   = 0x%08" PRIx32 "\n", image_info->image_flag);

	return 0;
}

} // namespace PngDec

LIB_DEFINE(InitPngDec_1) {
	PRINT_NAME_ENABLE(true);

	LIB_FUNC("-6srIGbLTIU", PngDec::PngDecQueryMemorySize);
	LIB_FUNC("m0uW+8pFyaw", PngDec::PngDecCreate);
	LIB_FUNC("WC216DD3El4", PngDec::PngDecDecode);
	LIB_FUNC("QbD+eENEwo8", PngDec::PngDecDelete);
	LIB_FUNC("U6h4e5JRPaQ", PngDec::PngDecParseHeader);
}

} // namespace Libs
