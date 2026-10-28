#include "libs/libJpegDec.cpp"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include "stb_image_write.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace Loader {
void SymbolDatabase::Add(const SymbolResolve&, uint64_t, const std::string&) {}
namespace Timer {
double GetTimeMs() {
	return 0.0;
}
} // namespace Timer
} // namespace Loader

namespace {
using namespace Libs::JpegDec;

#define CHECK(condition)                                                                           \
	do {                                                                                           \
		if (!(condition)) {                                                                        \
			std::fprintf(stderr, "JpegDecTests:%d: %s\n", __LINE__, #condition);                   \
			std::abort();                                                                          \
		}                                                                                          \
	} while (false)

constexpr uint32_t WIDTH  = 16;
constexpr uint32_t HEIGHT = 8;

// cjpeg -grayscale -progressive -optimize -quality 100 of an 8x8 image with pixel x*16 + y*8,
// without the JFIF APP0 segment.
constexpr uint8_t PROGRESSIVE[] = {
    0xff, 0xd8, 0xff, 0xdb, 0x00, 0x43, 0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0xff, 0xc2, 0x00, 0x0b, 0x08, 0x00, 0x08, 0x00, 0x08,
    0x01, 0x01, 0x11, 0x00, 0xff, 0xc4, 0x00, 0x14, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0xff, 0xda, 0x00, 0x08, 0x01, 0x01,
    0x00, 0x00, 0x00, 0x01, 0x27, 0xff, 0x00, 0xff, 0xc4, 0x00, 0x16, 0x10, 0x00, 0x03, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x06, 0x07, 0xff,
    0xda, 0x00, 0x08, 0x01, 0x01, 0x00, 0x01, 0x05, 0x02, 0x9b, 0x2d, 0x9f, 0xff, 0xc4, 0x00, 0x18,
    0x10, 0x00, 0x02, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x03, 0x22, 0x41, 0x52, 0xff, 0xda, 0x00, 0x08, 0x01, 0x01, 0x00, 0x06, 0x3f, 0x02,
    0x44, 0x33, 0x47, 0xff, 0xc4, 0x00, 0x16, 0x10, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x51, 0xf0, 0xff, 0xda, 0x00, 0x08, 0x01,
    0x01, 0x00, 0x01, 0x3f, 0x21, 0x9c, 0x47, 0xff, 0xda, 0x00, 0x08, 0x01, 0x01, 0x00, 0x00, 0x00,
    0x10, 0x7f, 0xff, 0xc4, 0x00, 0x16, 0x10, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xb1, 0xf0, 0xff, 0xda, 0x00, 0x08, 0x01, 0x01,
    0x00, 0x01, 0x3f, 0x10, 0x8f, 0x43, 0xff, 0xd9};

// Red 128, green by row, blue by column.
int Expected(uint32_t x, uint32_t y, uint32_t c) {
	const int rgb[3] = {128, static_cast<int>(y * 32), static_cast<int>(x * 16)};
	return rgb[c];
}

int Luma(uint32_t x, uint32_t y) {
	return (77 * 128 + 150 * Expected(x, y, 1) + 29 * Expected(x, y, 2) + 128) >> 8;
}

void CheckNear(int value, int expected) {
	CHECK(std::abs(value - expected) <= 8);
}

// Quality 100 keeps 4:4:4 sampling; 50 subsamples to 4:2:0.
std::vector<uint8_t> Encode(int quality, uint32_t width = WIDTH) {
	std::vector<uint8_t> rgb(width * HEIGHT * 3);
	for (uint32_t i = 0; i < rgb.size(); i++) {
		rgb[i] = static_cast<uint8_t>(Expected(i / 3 % width, i / 3 / width, i % 3));
	}
	std::vector<uint8_t> jpeg;
	stbi_write_jpg_to_func(
	    [](void* context, void* data, int size) {
		    auto*       out   = static_cast<std::vector<uint8_t>*>(context);
		    const auto* bytes = static_cast<const uint8_t*>(data);
		    out->insert(out->end(), bytes, bytes + size);
	    },
	    &jpeg, static_cast<int>(width), HEIGHT, 3, rgb.data(), quality);
	return jpeg;
}

// The first marker of this type in an stb_image_write output.
auto Marker(std::vector<uint8_t>& jpeg, uint8_t type) {
	const uint8_t marker[2] = {0xff, type};
	const auto    it = std::search(jpeg.begin(), jpeg.end(), std::begin(marker), std::end(marker));
	CHECK(it != jpeg.end());
	return it;
}

struct Decoder {
	JpegDecContext context {};
	void*          handle = nullptr;

	explicit Decoder(uint32_t max_width) {
		const JpegDecCreateParam create {sizeof(JpegDecCreateParam),
		                                 JPEG_DEC_ATTRIBUTE_CSC_STD_SAMPLING, max_width};
		CHECK(JpegDecQueryMemorySize(&create) == sizeof(context));
		CHECK(JpegDecCreate(&create, &context, sizeof(context), &handle) == 0);
	}

	~Decoder() {
		CHECK(JpegDecDelete(handle) == 0);
		CHECK(JpegDecDelete(handle) == JPEG_DEC_ERROR_INVALID_HANDLE);
	}
};

void TestParseHeader() {
	auto              jpeg = Encode(100);
	JpegDecParseParam parse {jpeg.data(), static_cast<uint32_t>(jpeg.size()), 0, 2};
	JpegDecImageInfo  info {};
	CHECK(JpegDecParseHeader(&parse, &info) == 0);
	CHECK(info.image_width == WIDTH && info.image_height == HEIGHT);
	CHECK(info.color_space == JPEG_DEC_COLOR_SPACE_YCC && info.component_number == 3);
	CHECK(info.component_id[0] == 1 && info.component_id[1] == 2 && info.component_id[2] == 3);
	CHECK(info.sampling_factor[0] == 0x11 && info.sampling_factor[2] == 0x11);
	CHECK(info.coefficient_mem_size == 0);
	CHECK(info.suitable_csc_attribute == JPEG_DEC_ATTRIBUTE_CSC_STD_SAMPLING);
	CHECK(info.output_image_width == WIDTH / 2 && info.output_image_height == HEIGHT / 2);

	auto subsampled = Encode(50);
	parse           = {subsampled.data(), static_cast<uint32_t>(subsampled.size()), 1, 8};
	CHECK(JpegDecParseHeader(&parse, &info) == 0);
	CHECK(info.sampling_factor[0] == 0x22 && info.sampling_factor[1] == 0x11);
	CHECK(info.output_image_width == 2 && info.output_image_height == 1);

	// 4x1 luma needs full sampling.
	Marker(subsampled, 0xc0)[11] = 0x41;
	CHECK(JpegDecParseHeader(&parse, &info) == 0);
	CHECK(info.suitable_csc_attribute == JPEG_DEC_ATTRIBUTE_CSC_FULL_SAMPLING);

	// A progressive frame keeps 16-bit coefficients for 2x1 blocks of each of the 3 components.
	const auto sof = Marker(jpeg, 0xc0);
	sof[1]         = 0xc2;
	parse          = {jpeg.data(), static_cast<uint32_t>(jpeg.size()), 0, 1};
	CHECK(JpegDecParseHeader(&parse, &info) == 0 && info.coefficient_mem_size == 3 * 2 * 128);
	sof[1] = 0xc3;
	CHECK(JpegDecParseHeader(&parse, &info) == JPEG_DEC_ERROR_UNSUPPORT_DATA);

	parse.down_scale = 3;
	CHECK(JpegDecParseHeader(&parse, &info) == JPEG_DEC_ERROR_INVALID_PARAM);
	parse = {jpeg.data(), 0, 0, 1};
	CHECK(JpegDecParseHeader(&parse, &info) == JPEG_DEC_ERROR_INVALID_SIZE);
	CHECK(JpegDecParseHeader(&parse, nullptr) == JPEG_DEC_ERROR_INVALID_ADDR);
	jpeg[1] = 0;
	parse   = {jpeg.data(), static_cast<uint32_t>(jpeg.size()), 0, 1};
	CHECK(JpegDecParseHeader(&parse, &info) == JPEG_DEC_ERROR_INVALID_DATA);
}

void TestDecode() {
	const auto                                               jpeg = Encode(100);
	Decoder                                                  decoder(WIDTH);
	alignas(4) std::array<uint8_t, (WIDTH * 4 + 8) * HEIGHT> image {};
	// GT7 passes a non-NULL coefficient address with size 0 for baseline images.
	JpegDecDecodeParam param {.jpeg_mem_addr        = jpeg.data(),
	                          .image_mem_addr       = image.data(),
	                          .coefficient_mem_addr = image.data(),
	                          .jpeg_mem_size        = static_cast<uint32_t>(jpeg.size()),
	                          .image_mem_size       = image.size(),
	                          .down_scale           = 1,
	                          .pixel_format         = JPEG_DEC_PIXEL_FORMAT_R8G8B8A8,
	                          .alpha_value          = 200,
	                          .image_pitch          = WIDTH * 4 + 8};
	image.fill(0xee);
	CHECK(JpegDecDecode(decoder.handle, &param, nullptr) ==
	      static_cast<int32_t>(WIDTH << 16 | HEIGHT));
	for (uint32_t y = 0; y < HEIGHT; y++) {
		const auto* row = &image[y * param.image_pitch];
		for (uint32_t x = 0; x < WIDTH; x++) {
			for (uint32_t c = 0; c < 3; c++) {
				CheckNear(row[x * 4 + c], Expected(x, y, c));
			}
			CHECK(row[x * 4 + 3] == 200);
		}
		CHECK(row[WIDTH * 4] == 0xee && row[WIDTH * 4 + 7] == 0xee);
	}

	param.pixel_format = JPEG_DEC_PIXEL_FORMAT_B8G8R8A8;
	param.image_pitch  = 0;
	CHECK(JpegDecDecode(decoder.handle, &param, nullptr) > 0);
	CheckNear(image[(3 * WIDTH + 5) * 4], Expected(5, 3, 2));
	CheckNear(image[(3 * WIDTH + 5) * 4 + 2], 128);

	param.pixel_format = JPEG_DEC_PIXEL_FORMAT_Y8;
	CHECK(JpegDecDecode(decoder.handle, &param, nullptr) > 0);
	CheckNear(image[3 * WIDTH + 5], Luma(5, 3));

	// YUYV holds both lumas and the pair's average chroma; saturated pairs stay at 255.
	param.pixel_format = JPEG_DEC_PIXEL_FORMAT_Y8U8Y8V8;
	CHECK(JpegDecDecode(decoder.handle, &param, nullptr) > 0);
	const auto* pair = &image[(3 * WIDTH + 4) * 2];
	CheckNear(pair[0], Luma(4, 3));
	CheckNear(pair[2], Luma(5, 3));
	const int blue = (Expected(4, 3, 2) + Expected(5, 3, 2)) / 2;
	CheckNear(pair[1], (-43 * 128 - 85 * Expected(4, 3, 1) + 128 * blue + 32896) >> 8);
	CheckNear(pair[3], (128 * 128 - 107 * Expected(4, 3, 1) - 21 * blue + 32896) >> 8);
	constexpr uint8_t red[3]       = {255, 0, 0};
	constexpr uint8_t pure_blue[3] = {0, 0, 255};
	CHECK(Chroma(pure_blue, pure_blue, -43, -85, 128) == 255);
	CHECK(Chroma(red, red, 128, -107, -21) == 255);

	// Each downscaled pixel averages its 4x4 block: (1, 1) covers columns and rows 4-7.
	param.pixel_format = JPEG_DEC_PIXEL_FORMAT_R8G8B8A8;
	param.down_scale   = 4;
	CHECK(JpegDecDecode(decoder.handle, &param, nullptr) ==
	      static_cast<int32_t>(WIDTH / 4 << 16 | HEIGHT / 4));
	CheckNear(image[(WIDTH / 4 + 1) * 4 + 1], (4 + 5 + 6 + 7) * 32 / 4);
	CheckNear(image[(WIDTH / 4 + 1) * 4 + 2], (4 + 5 + 6 + 7) * 16 / 4);

	// An odd YUYV width repeats the last pixel.
	const auto odd      = Encode(100, WIDTH - 1);
	param.jpeg_mem_addr = odd.data();
	param.jpeg_mem_size = static_cast<uint32_t>(odd.size());
	param.pixel_format  = JPEG_DEC_PIXEL_FORMAT_Y8U8Y8V8;
	param.down_scale    = 1;
	CHECK(JpegDecDecode(decoder.handle, &param, nullptr) > 0);
	const auto* last = &image[(3 * WIDTH + WIDTH - 2) * 2];
	CheckNear(last[0], Luma(WIDTH - 2, 3));
	CHECK(last[2] == last[0]);
}

void TestProgressive() {
	Decoder                                   decoder(8);
	std::array<uint8_t, 128>                  coefficients {};
	alignas(4) std::array<uint8_t, 8 * 8 * 4> image {};
	JpegDecDecodeParam                        param {.jpeg_mem_addr        = PROGRESSIVE,
	                                                 .image_mem_addr       = image.data(),
	                                                 .coefficient_mem_addr = coefficients.data(),
	                                                 .jpeg_mem_size        = sizeof(PROGRESSIVE),
	                                                 .image_mem_size       = image.size(),
	                                                 .coefficient_mem_size = coefficients.size(),
	                                                 .down_scale           = 1,
	                                                 .pixel_format = JPEG_DEC_PIXEL_FORMAT_R8G8B8A8,
	                                                 .alpha_value  = 255};
	JpegDecImageInfo                          info {};
	CHECK(JpegDecDecode(decoder.handle, &param, &info) == (8 << 16 | 8));
	CHECK(info.color_space == JPEG_DEC_COLOR_SPACE_GRAYSCALE && info.component_number == 1);
	for (uint32_t i = 0; i < 64; i++) {
		CheckNear(image[i * 4], i % 8 * 16 + i / 8 * 8);
	}
	param.coefficient_mem_size--;
	CHECK(JpegDecDecode(decoder.handle, &param, nullptr) == JPEG_DEC_ERROR_INVALID_COEF_MEMORY);
	param.coefficient_mem_addr = nullptr;
	param.coefficient_mem_size = coefficients.size();
	CHECK(JpegDecDecode(decoder.handle, &param, nullptr) == JPEG_DEC_ERROR_INVALID_COEF_MEMORY);
}

// A scan whose Huffman tables no DHT defined gets zeroed tables, not the previous decode's: they
// consume no bits, so stb fails at the scan's first stuffed 0xff.
void TestMissingTables() {
	auto                                               jpeg = Encode(100);
	Decoder                                            decoder(WIDTH);
	alignas(4) std::array<uint8_t, WIDTH * HEIGHT * 4> image {};
	JpegDecDecodeParam param {.jpeg_mem_addr  = jpeg.data(),
	                          .image_mem_addr = image.data(),
	                          .jpeg_mem_size  = static_cast<uint32_t>(jpeg.size()),
	                          .image_mem_size = image.size(),
	                          .down_scale     = 1,
	                          .pixel_format   = JPEG_DEC_PIXEL_FORMAT_R8G8B8A8,
	                          .alpha_value    = 255};
	CHECK(JpegDecDecode(decoder.handle, &param, nullptr) > 0);
	const auto dht = Marker(jpeg, 0xc4);
	jpeg.erase(dht, dht + 2 + (dht[2] << 8 | dht[3]));
	param.jpeg_mem_size = static_cast<uint32_t>(jpeg.size());
	CHECK(JpegDecDecode(decoder.handle, &param, nullptr) == JPEG_DEC_ERROR_DECODE_ERROR);
}

void TestDecodeValidation() {
	auto                                               jpeg = Encode(100);
	Decoder                                            decoder(WIDTH);
	alignas(4) std::array<uint8_t, WIDTH * HEIGHT * 4> image {};
	const JpegDecDecodeParam valid {.jpeg_mem_addr  = jpeg.data(),
	                                .image_mem_addr = image.data(),
	                                .jpeg_mem_size  = static_cast<uint32_t>(jpeg.size()),
	                                .image_mem_size = image.size(),
	                                .down_scale     = 1,
	                                .pixel_format   = JPEG_DEC_PIXEL_FORMAT_R8G8B8A8,
	                                .alpha_value    = 255};
	CHECK(JpegDecDecode(decoder.handle, &valid, nullptr) > 0);
	CHECK(JpegDecDecode(nullptr, &valid, nullptr) == JPEG_DEC_ERROR_INVALID_HANDLE);
	CHECK(JpegDecDecode(decoder.handle, nullptr, nullptr) == JPEG_DEC_ERROR_INVALID_ADDR);

	auto param           = valid;
	param.image_mem_addr = image.data() + 2;
	CHECK(JpegDecDecode(decoder.handle, &param, nullptr) == JPEG_DEC_ERROR_INVALID_ADDR);
	param.pixel_format = JPEG_DEC_PIXEL_FORMAT_Y8;
	CHECK(JpegDecDecode(decoder.handle, &param, nullptr) > 0);
	param = valid;
	param.image_mem_size--;
	CHECK(JpegDecDecode(decoder.handle, &param, nullptr) == JPEG_DEC_ERROR_INVALID_SIZE);
	param             = valid;
	param.image_pitch = WIDTH * 4 - 4;
	CHECK(JpegDecDecode(decoder.handle, &param, nullptr) == JPEG_DEC_ERROR_INVALID_PARAM);
	param              = valid;
	param.pixel_format = 2;
	CHECK(JpegDecDecode(decoder.handle, &param, nullptr) == JPEG_DEC_ERROR_INVALID_PARAM);
	param             = valid;
	param.alpha_value = 256;
	CHECK(JpegDecDecode(decoder.handle, &param, nullptr) == JPEG_DEC_ERROR_INVALID_PARAM);

	// The width limit applies after downscaling.
	Decoder narrow(WIDTH / 2);
	CHECK(JpegDecDecode(narrow.handle, &valid, nullptr) == JPEG_DEC_ERROR_INVALID_WORK_MEMORY);
	param            = valid;
	param.down_scale = 2;
	CHECK(JpegDecDecode(narrow.handle, &param, nullptr) > 0);

	// Full sampling needs a decoder created for it.
	const auto sof = Marker(jpeg, 0xc0);
	sof[11]        = 0x41;
	CHECK(JpegDecDecode(decoder.handle, &valid, nullptr) == JPEG_DEC_ERROR_INVALID_WORK_MEMORY);
	// An output above 32767 pixels needs image information.
	sof[11] = 0x11;
	sof[7]  = 40000 >> 8;
	sof[8]  = 40000 & 0xff;
	Decoder wide(0xffff);
	CHECK(JpegDecDecode(wide.handle, &valid, nullptr) == JPEG_DEC_ERROR_INVALID_ADDR);

	JpegDecCreateParam create {sizeof(JpegDecCreateParam), 3, WIDTH};
	CHECK(JpegDecQueryMemorySize(&create) == JPEG_DEC_ERROR_INVALID_PARAM);
	create = {sizeof(JpegDecCreateParam), JPEG_DEC_ATTRIBUTE_CSC_FULL_SAMPLING, 0};
	CHECK(JpegDecQueryMemorySize(&create) == JPEG_DEC_ERROR_INVALID_PARAM);
	create.this_size = 8;
	CHECK(JpegDecQueryMemorySize(&create) == JPEG_DEC_ERROR_INVALID_SIZE);
	CHECK(JpegDecQueryMemorySize(nullptr) == JPEG_DEC_ERROR_INVALID_ADDR);
}
} // namespace

int main() {
	TestParseHeader();
	TestDecode();
	TestProgressive();
	TestMissingTables();
	TestDecodeValidation();
	std::puts("JPEG decoder tests passed");
}
