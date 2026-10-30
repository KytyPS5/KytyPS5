#include "libs/libJpegEnc.cpp"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#include "stb_image.h"

#include <array>
#include <cstdio>
#include <cstdlib>

namespace Loader {
void SymbolDatabase::Add(const SymbolResolve&, uint64_t, const std::string&) {}
namespace Timer {
double GetTimeMs() {
	return 0.0;
}
} // namespace Timer
} // namespace Loader

namespace {
using namespace Libs::JpegEnc;

#define CHECK(condition)                                                                           \
	do {                                                                                           \
		if (!(condition)) {                                                                        \
			std::fprintf(stderr, "JpegEncTests:%d: %s\n", __LINE__, #condition);                   \
			std::abort();                                                                          \
		}                                                                                          \
	} while (false)

constexpr uint32_t WIDTH  = 16;
constexpr uint32_t HEIGHT = 8;

using Jpeg    = std::array<uint8_t, 4096>;
using Pattern = int (*)(uint32_t x, uint32_t y, uint32_t c);

struct Encoder {
	JpegEncContext context {};
	void*          handle = nullptr;

	Encoder() {
		const JpegEncCreateParam create {sizeof(JpegEncCreateParam), 0};
		CHECK(JpegEncQueryMemorySize(&create) == sizeof(context));
		CHECK(JpegEncCreate(&create, &context, sizeof(context), &handle) == 0);
	}

	~Encoder() {
		CHECK(JpegEncDelete(handle) == 0);
		CHECK(JpegEncDelete(handle) == JPEG_ENC_ERROR_INVALID_HANDLE);
	}
};

// Red 128, green by row, blue by column.
int Expected(uint32_t x, uint32_t y, uint32_t c) {
	const int rgb[3] = {128, static_cast<int>(y * 32), static_cast<int>(x * 16)};
	return rgb[c];
}

int Luma(uint32_t x, uint32_t y, uint32_t /*c*/) {
	return (77 * 128 + 150 * Expected(x, y, 1) + 29 * Expected(x, y, 2) + 128) >> 8;
}

// Decodes the output and checks every channel against the pattern.
void CheckDecoded(const Jpeg& jpeg, int32_t size, Pattern pattern) {
	CHECK(size > 0);
	int   width  = 0;
	int   height = 0;
	auto* rgb    = stbi_load_from_memory(jpeg.data(), size, &width, &height, nullptr, 3);
	CHECK(rgb != nullptr && width == WIDTH && height == HEIGHT);
	for (uint32_t i = 0; i < WIDTH * HEIGHT * 3; i++) {
		CHECK(std::abs(rgb[i] - pattern(i / 3 % WIDTH, i / 3 / WIDTH, i % 3)) <= 8);
	}
	stbi_image_free(rgb);
}

void TestEncode() {
	Encoder                                            encoder;
	alignas(4) std::array<uint8_t, WIDTH * HEIGHT * 4> bgra {};
	std::array<uint8_t, WIDTH * HEIGHT>                y8 {};
	for (uint32_t i = 0; i < WIDTH * HEIGHT; i++) {
		const uint32_t x = i % WIDTH;
		const uint32_t y = i / WIDTH;
		bgra[i * 4]      = Expected(x, y, 2);
		bgra[i * 4 + 1]  = Expected(x, y, 1);
		bgra[i * 4 + 2]  = Expected(x, y, 0);
		y8[i]            = Luma(x, y, 0);
	}
	Jpeg               jpeg {};
	JpegEncEncodeParam param {.image_mem_addr = bgra.data(),
	                          .jpeg_mem_addr  = jpeg.data(),
	                          .image_mem_size = bgra.size(),
	                          .jpeg_mem_size  = jpeg.size(),
	                          .image_width    = WIDTH,
	                          .image_height   = HEIGHT,
	                          .image_pitch    = WIDTH * 4,
	                          .pixel_format   = JPEG_ENC_PIXEL_FORMAT_B8G8R8A8,
	                          .color_space    = JPEG_ENC_COLOR_SPACE_YCC,
	                          .sampling_type  = JPEG_ENC_SAMPLING_TYPE_420};
	JpegEncOutputInfo  info {};
	const auto         size = JpegEncEncode(encoder.handle, &param, &info);
	CHECK(info.data_size == static_cast<uint32_t>(size) && info.processed_height == HEIGHT);
	CheckDecoded(jpeg, size, Expected);
	const auto color = param;

	// The same pixels read as RGBA swap red and blue.
	param.pixel_format = JPEG_ENC_PIXEL_FORMAT_R8G8B8A8;
	CheckDecoded(jpeg, JpegEncEncode(encoder.handle, &param, nullptr),
	             [](uint32_t x, uint32_t y, uint32_t c) { return Expected(x, y, 2 - c); });

	param               = color;
	param.color_space   = JPEG_ENC_COLOR_SPACE_GRAYSCALE;
	param.sampling_type = JPEG_ENC_SAMPLING_TYPE_FULL;
	CheckDecoded(jpeg, JpegEncEncode(encoder.handle, &param, nullptr), Luma);

	param.image_mem_addr = y8.data();
	param.image_mem_size = y8.size();
	param.image_pitch    = WIDTH;
	param.pixel_format   = JPEG_ENC_PIXEL_FORMAT_Y8;
	CheckDecoded(jpeg, JpegEncEncode(encoder.handle, &param, nullptr), Luma);

	// GT7's YUYV rows hold 2 bytes per pixel at a pitch of width * 4.
	constexpr uint8_t                                  pair[4] = {64, 160, 192, 96};
	alignas(4) std::array<uint8_t, WIDTH * HEIGHT * 4> yuyv {};
	for (uint32_t i = 0; i < yuyv.size(); i++) {
		yuyv[i] = i % (WIDTH * 4) < WIDTH * 2 ? pair[i % 4] : 0xff;
	}
	param                = color;
	param.image_mem_addr = yuyv.data();
	param.pixel_format   = JPEG_ENC_PIXEL_FORMAT_Y8U8Y8V8;
	param.sampling_type  = JPEG_ENC_SAMPLING_TYPE_422;
	CheckDecoded(jpeg, JpegEncEncode(encoder.handle, &param, nullptr),
	             [](uint32_t x, uint32_t, uint32_t c) {
		             constexpr int rgb[2][3] = {{19, 76, 121}, {147, 204, 249}};
		             return rgb[x % 2][c];
	             });
	param.color_space   = JPEG_ENC_COLOR_SPACE_GRAYSCALE;
	param.sampling_type = JPEG_ENC_SAMPLING_TYPE_FULL;
	CheckDecoded(jpeg, JpegEncEncode(encoder.handle, &param, nullptr),
	             [](uint32_t x, uint32_t, uint32_t) { return x % 2 == 0 ? 64 : 192; });

	param               = color;
	param.jpeg_mem_size = 100;
	CHECK(JpegEncEncode(encoder.handle, &param, &info) == JPEG_ENC_ERROR_DATA_OVERFLOW);
	CHECK(info.data_size <= 100 && info.processed_height == 0);
}

void TestValidation() {
	JpegEncCreateParam create {sizeof(JpegEncCreateParam), 1};
	CHECK(JpegEncQueryMemorySize(nullptr) == JPEG_ENC_ERROR_INVALID_ADDR);
	CHECK(JpegEncQueryMemorySize(&create) == JPEG_ENC_ERROR_INVALID_PARAM);
	create = {4, 0};
	CHECK(JpegEncQueryMemorySize(&create) == JPEG_ENC_ERROR_INVALID_SIZE);

	Encoder                                            encoder;
	alignas(4) std::array<uint8_t, WIDTH * HEIGHT * 4> rgba {};
	Jpeg                                               jpeg {};
	const JpegEncEncodeParam valid {.image_mem_addr    = rgba.data(),
	                                .jpeg_mem_addr     = jpeg.data(),
	                                .image_mem_size    = rgba.size(),
	                                .jpeg_mem_size     = jpeg.size(),
	                                .image_width       = WIDTH,
	                                .image_height      = HEIGHT,
	                                .image_pitch       = WIDTH * 4,
	                                .pixel_format      = JPEG_ENC_PIXEL_FORMAT_R8G8B8A8,
	                                .color_space       = JPEG_ENC_COLOR_SPACE_YCC,
	                                .sampling_type     = JPEG_ENC_SAMPLING_TYPE_422,
	                                .compression_ratio = 50,
	                                .restart_interval  = -1};
	CHECK(JpegEncEncode(encoder.handle, &valid, nullptr) > 0);
	CHECK(JpegEncEncode(nullptr, &valid, nullptr) == JPEG_ENC_ERROR_INVALID_HANDLE);
	CHECK(JpegEncEncode(encoder.handle, nullptr, nullptr) == JPEG_ENC_ERROR_INVALID_ADDR);

	auto param           = valid;
	param.image_mem_addr = rgba.data() + 1;
	CHECK(JpegEncEncode(encoder.handle, &param, nullptr) == JPEG_ENC_ERROR_INVALID_ADDR);
	param = valid;
	param.image_mem_size--;
	CHECK(JpegEncEncode(encoder.handle, &param, nullptr) == JPEG_ENC_ERROR_INVALID_SIZE);
	param             = valid;
	param.image_pitch = WIDTH * 4 - 4;
	CHECK(JpegEncEncode(encoder.handle, &param, nullptr) == JPEG_ENC_ERROR_INVALID_PARAM);
	param               = valid;
	param.sampling_type = JPEG_ENC_SAMPLING_TYPE_FULL;
	CHECK(JpegEncEncode(encoder.handle, &param, nullptr) == JPEG_ENC_ERROR_INVALID_PARAM);
	param              = valid;
	param.pixel_format = JPEG_ENC_PIXEL_FORMAT_Y8;
	CHECK(JpegEncEncode(encoder.handle, &param, nullptr) == JPEG_ENC_ERROR_INVALID_PARAM);
	param                  = valid;
	param.restart_interval = -2;
	CHECK(JpegEncEncode(encoder.handle, &param, nullptr) == JPEG_ENC_ERROR_INVALID_PARAM);
	param             = valid;
	param.encode_mode = 2;
	CHECK(JpegEncEncode(encoder.handle, &param, nullptr) == JPEG_ENC_ERROR_INVALID_PARAM);
	param              = valid;
	param.pixel_format = 2;
	CHECK(JpegEncEncode(encoder.handle, &param, nullptr) == JPEG_ENC_ERROR_INVALID_PARAM);
}
} // namespace

int main() {
	TestEncode();
	TestValidation();
	std::puts("JPEG encoder tests passed");
}
