#include "common/abi.h"
#include "common/logging/log.h"
#include "libs/libs.h"
#include "loader/symbolDatabase.h"

#include <algorithm>
#include <cstddef>
#include <cstdlib>

// The emulator's stb_image implementation. Guest JPEG data reaches it here, and zeroed allocations
// keep a scan that uses a Huffman table no DHT defined (an MJPEG frame) from reading uninitialised
// tables.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_SIMD
#define STBI_MALLOC(size)           calloc(1, size)
#define STBI_REALLOC(pointer, size) realloc(pointer, size)
#define STBI_FREE(pointer)          free(pointer)
#include "stb_image.h"

namespace Libs {

LIB_VERSION("JpegDec", 1, "JpegDec", 1, 1);

namespace JpegDec {

constexpr int32_t JPEG_DEC_ERROR_INVALID_ADDR        = static_cast<int32_t>(0x80650001u);
constexpr int32_t JPEG_DEC_ERROR_INVALID_SIZE        = static_cast<int32_t>(0x80650002u);
constexpr int32_t JPEG_DEC_ERROR_INVALID_PARAM       = static_cast<int32_t>(0x80650003u);
constexpr int32_t JPEG_DEC_ERROR_INVALID_HANDLE      = static_cast<int32_t>(0x80650004u);
constexpr int32_t JPEG_DEC_ERROR_INVALID_COEF_MEMORY = static_cast<int32_t>(0x80650005u);
constexpr int32_t JPEG_DEC_ERROR_INVALID_WORK_MEMORY = static_cast<int32_t>(0x80650006u);
constexpr int32_t JPEG_DEC_ERROR_INVALID_DATA        = static_cast<int32_t>(0x80650010u);
constexpr int32_t JPEG_DEC_ERROR_UNSUPPORT_DATA      = static_cast<int32_t>(0x80650011u);
constexpr int32_t JPEG_DEC_ERROR_DECODE_ERROR        = static_cast<int32_t>(0x80650012u);

constexpr uint32_t JPEG_DEC_ATTRIBUTE_CSC_STD_SAMPLING  = 1;
constexpr uint32_t JPEG_DEC_ATTRIBUTE_CSC_FULL_SAMPLING = 2;
constexpr uint16_t JPEG_DEC_DECODE_MODE_MJPEG           = 1;
constexpr uint16_t JPEG_DEC_PIXEL_FORMAT_R8G8B8A8       = 0;
constexpr uint16_t JPEG_DEC_PIXEL_FORMAT_B8G8R8A8       = 1;
constexpr uint16_t JPEG_DEC_PIXEL_FORMAT_Y8U8Y8V8       = 10;
constexpr uint16_t JPEG_DEC_PIXEL_FORMAT_Y8             = 11;
constexpr uint16_t JPEG_DEC_COLOR_SPACE_UNKNOWN         = 0;
constexpr uint16_t JPEG_DEC_COLOR_SPACE_YCC             = 1;
constexpr uint16_t JPEG_DEC_COLOR_SPACE_GRAYSCALE       = 2;

struct JpegDecCreateParam {
	uint32_t this_size;
	uint32_t attribute;
	uint32_t max_image_width;
};

struct JpegDecDecodeParam {
	const void* jpeg_mem_addr;
	void*       image_mem_addr;
	void*       coefficient_mem_addr;
	uint32_t    jpeg_mem_size;
	uint32_t    image_mem_size;
	uint32_t    coefficient_mem_size;
	uint16_t    decode_mode;
	uint16_t    down_scale;
	uint16_t    pixel_format;
	uint16_t    alpha_value;
	uint32_t    image_pitch;
};

struct JpegDecParseParam {
	const void* jpeg_mem_addr;
	uint32_t    jpeg_mem_size;
	uint16_t    decode_mode;
	uint16_t    down_scale;
};

struct JpegDecImageInfo {
	uint32_t image_width;
	uint32_t image_height;
	uint16_t color_space;
	uint16_t component_number;
	uint8_t  component_id[4];
	uint8_t  sampling_factor[4];
	uint32_t coefficient_mem_size;
	uint32_t suitable_csc_attribute;
	uint32_t output_image_width;
	uint32_t output_image_height;
};

static_assert(sizeof(JpegDecCreateParam) == 12 && sizeof(JpegDecDecodeParam) == 48);
static_assert(sizeof(JpegDecParseParam) == 16 && sizeof(JpegDecImageInfo) == 36);

struct JpegDecContext {
	uint64_t magic;
	uint32_t attribute;
	uint32_t max_image_width;
};

constexpr uint64_t JPEG_DEC_CONTEXT_MAGIC = 0x4b5954594a504744ull; // KYTYJPGD

static int32_t CheckCreateParam(const JpegDecCreateParam* param) {
	if (param == nullptr) {
		return JPEG_DEC_ERROR_INVALID_ADDR;
	}
	if (param->this_size != sizeof(JpegDecCreateParam)) {
		return JPEG_DEC_ERROR_INVALID_SIZE;
	}
	if ((param->attribute != JPEG_DEC_ATTRIBUTE_CSC_STD_SAMPLING &&
	     param->attribute != JPEG_DEC_ATTRIBUTE_CSC_FULL_SAMPLING) ||
	    param->max_image_width == 0 || param->max_image_width > 0xffffu) {
		return JPEG_DEC_ERROR_INVALID_PARAM;
	}
	return 0;
}

static bool CheckScale(uint16_t decode_mode, uint16_t down_scale) {
	return decode_mode <= JPEG_DEC_DECODE_MODE_MJPEG &&
	       (down_scale == 1 || down_scale == 2 || down_scale == 4 || down_scale == 8);
}

// Fills the image information from the frame header (T.81 B.2.2) that precedes the first scan.
static int32_t ParseHeader(const void* jpeg, uint32_t size, uint16_t down_scale,
                           JpegDecImageInfo* info) {
	const auto* data = static_cast<const uint8_t*>(jpeg);
	if (size < 4 || data[0] != 0xff || data[1] != 0xd8) {
		return JPEG_DEC_ERROR_INVALID_DATA;
	}
	for (uint32_t offset = 2; offset + 4 <= size;) {
		const uint8_t marker = data[offset + 1];
		if (data[offset] != 0xff || marker == 0xda || marker == 0xd9) {
			return JPEG_DEC_ERROR_INVALID_DATA;
		}
		if (marker == 0xff) {
			offset++;
			continue;
		}
		const uint32_t length  = (data[offset + 2] << 8u) | data[offset + 3];
		const auto*    segment = data + offset + 4;
		if (length < 2 || offset + 2 + length > size) {
			return JPEG_DEC_ERROR_INVALID_DATA;
		}
		offset += 2 + length;
		// SOF0-SOF15 except DHT, JPG and DAC.
		if (marker < 0xc0 || marker > 0xcf || marker == 0xc4 || marker == 0xc8 || marker == 0xcc) {
			continue;
		}
		const uint32_t components = length < 8 ? 0 : segment[5];
		if (components == 0 || length < 8 + components * 3 || (segment[3] | segment[4]) == 0) {
			return JPEG_DEC_ERROR_INVALID_DATA;
		}
		*info              = {};
		info->image_height = (segment[1] << 8u) | segment[2];
		info->image_width  = (segment[3] << 8u) | segment[4];
		// Baseline, extended and progressive Huffman 8-bit frames with a known height.
		if (marker > 0xc2 || segment[0] != 8 || components > 4 || info->image_height == 0) {
			return JPEG_DEC_ERROR_UNSUPPORT_DATA;
		}
		uint32_t h_max = 1;
		uint32_t v_max = 1;
		for (uint32_t i = 0; i < components; i++) {
			const uint8_t factor = segment[7 + i * 3];
			if ((factor >> 4u) - 1u > 3u || (factor & 0xfu) - 1u > 3u) {
				return JPEG_DEC_ERROR_INVALID_DATA;
			}
			info->component_id[i]    = segment[6 + i * 3];
			info->sampling_factor[i] = factor;
			h_max                    = std::max<uint32_t>(h_max, factor >> 4u);
			v_max                    = std::max<uint32_t>(v_max, factor & 0xfu);
		}
		info->component_number = components;
		if (marker == 0xc2) {
			// Progressive scans keep every 16-bit DCT coefficient of the MCU-padded image.
			const uint32_t mcu_x = (info->image_width + h_max * 8 - 1) / (h_max * 8);
			const uint32_t mcu_y = (info->image_height + v_max * 8 - 1) / (v_max * 8);
			for (uint32_t i = 0; i < components; i++) {
				info->coefficient_mem_size += mcu_x * (info->sampling_factor[i] >> 4u) * mcu_y *
				                              (info->sampling_factor[i] & 0xfu) * 128u;
			}
		}
		switch (components) {
			case 1: info->color_space = JPEG_DEC_COLOR_SPACE_GRAYSCALE; break;
			case 3: info->color_space = JPEG_DEC_COLOR_SPACE_YCC; break;
			default: info->color_space = JPEG_DEC_COLOR_SPACE_UNKNOWN; break;
		}
		// Standard sampling: grayscale, or 1x1 chroma with luma at most 2x2 (4:4:4/4:2:2/4:2:0).
		const bool standard =
		    components == 1 || (info->sampling_factor[1] == 0x11 &&
			                    info->sampling_factor[2] == 0x11 && h_max <= 2 && v_max <= 2);
		info->suitable_csc_attribute =
		    standard ? JPEG_DEC_ATTRIBUTE_CSC_STD_SAMPLING : JPEG_DEC_ATTRIBUTE_CSC_FULL_SAMPLING;
		info->output_image_width  = (info->image_width + down_scale - 1) / down_scale;
		info->output_image_height = (info->image_height + down_scale - 1) / down_scale;
		return 0;
	}
	return JPEG_DEC_ERROR_INVALID_DATA;
}

static uint8_t Luma(const uint8_t* p) {
	return static_cast<uint8_t>((77 * p[0] + 150 * p[1] + 29 * p[2] + 128) >> 8);
}

// JFIF chroma of the average of two pixels, rounding half down so a saturated pair stays 255.
static uint8_t Chroma(const uint8_t* p, const uint8_t* q, int r, int g, int b) {
	return static_cast<uint8_t>(
	    (r * (p[0] + q[0]) + g * (p[1] + q[1]) + b * (p[2] + q[2]) + 65791) >> 9);
}

static int32_t KYTY_SYSV_ABI JpegDecQueryMemorySize(const JpegDecCreateParam* param) {
	PRINT_NAME();
	const auto result = CheckCreateParam(param);
	return result != 0 ? result : static_cast<int32_t>(sizeof(JpegDecContext));
}

static int32_t KYTY_SYSV_ABI JpegDecCreate(const JpegDecCreateParam* param, void* memory_address,
                                           uint32_t memory_size, void** handle) {
	PRINT_NAME();
	if (const auto result = CheckCreateParam(param); result != 0) {
		return result;
	}
	if (memory_address == nullptr || handle == nullptr) {
		return JPEG_DEC_ERROR_INVALID_ADDR;
	}
	if (memory_size < sizeof(JpegDecContext)) {
		return JPEG_DEC_ERROR_INVALID_SIZE;
	}
	*static_cast<JpegDecContext*>(memory_address) = {JPEG_DEC_CONTEXT_MAGIC, param->attribute,
	                                                 param->max_image_width};
	*handle                                       = memory_address;
	return 0;
}

static int32_t KYTY_SYSV_ABI JpegDecDelete(void* handle) {
	PRINT_NAME();
	auto* ctx = static_cast<JpegDecContext*>(handle);
	if (ctx == nullptr || ctx->magic != JPEG_DEC_CONTEXT_MAGIC) {
		return JPEG_DEC_ERROR_INVALID_HANDLE;
	}
	ctx->magic = 0;
	return 0;
}

static int32_t KYTY_SYSV_ABI JpegDecParseHeader(const JpegDecParseParam* param,
                                                JpegDecImageInfo*        image_info) {
	PRINT_NAME();
	if (param == nullptr || image_info == nullptr || param->jpeg_mem_addr == nullptr) {
		return JPEG_DEC_ERROR_INVALID_ADDR;
	}
	if (param->jpeg_mem_size == 0) {
		return JPEG_DEC_ERROR_INVALID_SIZE;
	}
	if (!CheckScale(param->decode_mode, param->down_scale)) {
		return JPEG_DEC_ERROR_INVALID_PARAM;
	}
	return ParseHeader(param->jpeg_mem_addr, param->jpeg_mem_size, param->down_scale, image_info);
}

static int32_t KYTY_SYSV_ABI JpegDecDecode(void* handle, const JpegDecDecodeParam* param,
                                           JpegDecImageInfo* image_info) {
	PRINT_NAME();
	const auto* ctx = static_cast<const JpegDecContext*>(handle);
	if (ctx == nullptr || ctx->magic != JPEG_DEC_CONTEXT_MAGIC) {
		return JPEG_DEC_ERROR_INVALID_HANDLE;
	}
	if (param == nullptr || param->jpeg_mem_addr == nullptr || param->image_mem_addr == nullptr ||
	    (param->pixel_format != JPEG_DEC_PIXEL_FORMAT_Y8 &&
	     (reinterpret_cast<uintptr_t>(param->image_mem_addr) & 3u) != 0)) {
		return JPEG_DEC_ERROR_INVALID_ADDR;
	}
	const auto format = param->pixel_format;
	LOGF("\t jpeg_mem_size = %" PRIu32 ", image_mem_size = %" PRIu32 ", mode = %" PRIu16
	     ", down_scale = %" PRIu16 ", pixel_format = %" PRIu16 ", image_pitch = %" PRIu32 "\n",
	     param->jpeg_mem_size, param->image_mem_size, param->decode_mode, param->down_scale, format,
	     param->image_pitch);
	if (param->jpeg_mem_size == 0 || param->image_mem_size == 0) {
		return JPEG_DEC_ERROR_INVALID_SIZE;
	}
	if (!CheckScale(param->decode_mode, param->down_scale) || param->alpha_value > 255 ||
	    (format != JPEG_DEC_PIXEL_FORMAT_R8G8B8A8 && format != JPEG_DEC_PIXEL_FORMAT_B8G8R8A8 &&
	     format != JPEG_DEC_PIXEL_FORMAT_Y8U8Y8V8 && format != JPEG_DEC_PIXEL_FORMAT_Y8) ||
	    (format != JPEG_DEC_PIXEL_FORMAT_Y8 && (param->image_pitch & 3u) != 0)) {
		return JPEG_DEC_ERROR_INVALID_PARAM;
	}

	JpegDecImageInfo info {};
	if (const auto result =
	        ParseHeader(param->jpeg_mem_addr, param->jpeg_mem_size, param->down_scale, &info);
	    result != 0) {
		return result;
	}
	if (info.color_space == JPEG_DEC_COLOR_SPACE_UNKNOWN) {
		return JPEG_DEC_ERROR_UNSUPPORT_DATA;
	}
	if (info.output_image_width > ctx->max_image_width ||
	    info.suitable_csc_attribute > ctx->attribute) {
		return JPEG_DEC_ERROR_INVALID_WORK_MEMORY;
	}
	if (info.coefficient_mem_size > param->coefficient_mem_size ||
	    (info.coefficient_mem_size != 0 && param->coefficient_mem_addr == nullptr)) {
		return JPEG_DEC_ERROR_INVALID_COEF_MEMORY;
	}
	const uint32_t width  = info.output_image_width;
	const uint32_t height = info.output_image_height;
	const bool     large  = width > 32767 || height > 32767;
	if (large && image_info == nullptr) {
		return JPEG_DEC_ERROR_INVALID_ADDR;
	}
	uint32_t row = width * 4;
	if (format == JPEG_DEC_PIXEL_FORMAT_Y8U8Y8V8) {
		row = (width + 1) / 2 * 4;
	} else if (format == JPEG_DEC_PIXEL_FORMAT_Y8) {
		row = width;
	}
	const uint32_t pitch = param->image_pitch == 0 ? row : param->image_pitch;
	if (pitch < row) {
		return JPEG_DEC_ERROR_INVALID_PARAM;
	}
	if (static_cast<uint64_t>(pitch) * height > param->image_mem_size) {
		return JPEG_DEC_ERROR_INVALID_SIZE;
	}

	int   src_width  = 0;
	int   src_height = 0;
	auto* rgb        = stbi_load_from_memory(static_cast<const stbi_uc*>(param->jpeg_mem_addr),
	                                         static_cast<int>(param->jpeg_mem_size), &src_width,
	                                         &src_height, nullptr, 3);
	if (rgb == nullptr || static_cast<uint32_t>(src_width) != info.image_width ||
	    static_cast<uint32_t>(src_height) != info.image_height) {
		stbi_image_free(rgb);
		return JPEG_DEC_ERROR_DECODE_ERROR;
	}
	// Each output pixel averages its down_scale x down_scale source block, packed in place: an
	// output pixel never lands past a source pixel still to be read.
	const uint32_t scale = param->down_scale;
	if (scale > 1) {
		auto* out = rgb;
		for (uint32_t y = 0; y < height; y++) {
			for (uint32_t x = 0; x < width; x++) {
				const uint32_t x_end  = std::min((x + 1) * scale, info.image_width);
				const uint32_t y_end  = std::min((y + 1) * scale, info.image_height);
				const uint32_t count  = (x_end - x * scale) * (y_end - y * scale);
				uint32_t       sum[3] = {};
				for (uint32_t sy = y * scale; sy < y_end; sy++) {
					for (uint32_t sx = x * scale; sx < x_end; sx++) {
						const auto* p = rgb + (static_cast<size_t>(sy) * info.image_width + sx) * 3;
						sum[0] += p[0];
						sum[1] += p[1];
						sum[2] += p[2];
					}
				}
				for (const uint32_t c: sum) {
					*out++ = static_cast<uint8_t>((c + count / 2) / count);
				}
			}
		}
	}
	const auto  alpha = static_cast<uint8_t>(param->alpha_value);
	const auto* p     = rgb;
	for (uint32_t y = 0; y < height; y++) {
		auto* dst = static_cast<uint8_t*>(param->image_mem_addr) + static_cast<size_t>(y) * pitch;
		for (uint32_t x = 0; x < width; x++, p += 3) {
			switch (format) {
				case JPEG_DEC_PIXEL_FORMAT_R8G8B8A8:
					dst    = std::copy_n(p, 3, dst);
					*dst++ = alpha;
					break;
				case JPEG_DEC_PIXEL_FORMAT_B8G8R8A8:
					*dst++ = p[2];
					*dst++ = p[1];
					*dst++ = p[0];
					*dst++ = alpha;
					break;
				case JPEG_DEC_PIXEL_FORMAT_Y8: *dst++ = Luma(p); break;
				default:
					if (x % 2 == 0) {
						// An odd width repeats the last pixel.
						const auto* q = x + 1 < width ? p + 3 : p;
						*dst++        = Luma(p);
						*dst++        = Chroma(p, q, -43, -85, 128);
						*dst++        = Luma(q);
						*dst++        = Chroma(p, q, 128, -107, -21);
					}
					break;
			}
		}
	}
	stbi_image_free(rgb);
	if (image_info != nullptr) {
		*image_info = info;
	}
	return large ? 0 : static_cast<int32_t>((width << 16u) | height);
}

} // namespace JpegDec

LIB_DEFINE(InitJpegDec_1) {
	PRINT_NAME_ENABLE(true);

	LIB_FUNC("uNAUmANZMEw", JpegDec::JpegDecQueryMemorySize);
	LIB_FUNC("JPh3Zgg0Zwc", JpegDec::JpegDecCreate);
	LIB_FUNC("1kzQRoWEgSA", JpegDec::JpegDecDecode);
	LIB_FUNC("Hwh11+m5KoI", JpegDec::JpegDecDelete);
	LIB_FUNC("LSinoSQH790", JpegDec::JpegDecParseHeader);
}

} // namespace Libs
