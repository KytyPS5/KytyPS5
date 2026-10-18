#include "libs/libPngEnc.cpp"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "stb_image.h"

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
using namespace Libs::PngEnc;

#define CHECK(condition)                                                                           \
	do {                                                                                           \
		if (!(condition)) {                                                                        \
			std::fprintf(stderr, "PngEncTests:%d: %s\n", __LINE__, #condition);                    \
			std::abort();                                                                          \
		}                                                                                          \
	} while (false)

struct Encoder {
	PngEncContext context {};
	void*         handle = nullptr;

	explicit Encoder(uint32_t max_filters = 4) {
		const PngEncCreateParam create {sizeof(PngEncCreateParam), 0, PNG_ENC_MAX_IMAGE_WIDTH,
		                                max_filters};
		CHECK(PngEncQueryMemorySize(&create) == sizeof(context));
		CHECK(PngEncCreate(&create, &context, sizeof(context), &handle) == 0);
	}

	~Encoder() {
		CHECK(PngEncDelete(handle) == 0);
		CHECK(PngEncDelete(handle) == PNG_ENC_ERROR_INVALID_HANDLE);
	}
};

PngEncEncodeParam EncodeParam(const uint8_t* source, uint32_t source_size, uint8_t* destination,
                              uint32_t destination_size) {
	PngEncEncodeParam param {};
	param.image_mem_addr    = source;
	param.png_mem_addr      = destination;
	param.image_mem_size    = source_size;
	param.png_mem_size      = destination_size;
	param.image_width       = 2;
	param.image_height      = 2;
	param.image_pitch       = 8;
	param.color_space       = PNG_ENC_COLOR_SPACE_RGBA;
	param.bit_depth         = 8;
	param.compression_level = 6;
	return param;
}

uint32_t ReadBe32(const uint8_t* bytes) {
	return (uint32_t {bytes[0]} << 24u) | (uint32_t {bytes[1]} << 16u) |
	       (uint32_t {bytes[2]} << 8u) | bytes[3];
}

void TestPixels() {
	Encoder                           encoder;
	constexpr std::array<uint8_t, 16> rgba {255, 0, 13,  0,  4,   210, 6,   255,
	                                        7,   8, 220, 64, 111, 12,  113, 128};
	for (const bool bgr: {false, true}) {
		for (const bool alpha: {false, true}) {
			for (const bool padded: {false, true}) {
				const uint32_t       pitch = padded ? 12 : 8;
				std::vector<uint8_t> source(pitch * 2, 0xee);
				std::vector<uint8_t> expected;
				for (uint32_t y = 0; y < 2; ++y) {
					for (uint32_t x = 0; x < 2; ++x) {
						const auto* pixel = rgba.data() + (y * 2 + x) * 4;
						auto*       input = source.data() + y * pitch + x * 4;
						input[0]          = pixel[bgr ? 2 : 0];
						input[1]          = pixel[1];
						input[2]          = pixel[bgr ? 0 : 2];
						input[3]          = pixel[3];
						expected.insert(expected.end(), pixel, pixel + (alpha ? 4 : 3));
					}
				}
				std::array<uint8_t, 1024> png {};
				auto param = EncodeParam(source.data(), source.size(), png.data(), png.size());
				param.pixel_format =
				    bgr ? PNG_ENC_PIXEL_FORMAT_B8G8R8A8 : PNG_ENC_PIXEL_FORMAT_R8G8B8A8;
				param.color_space = alpha ? PNG_ENC_COLOR_SPACE_RGBA : PNG_ENC_COLOR_SPACE_RGB;
				param.image_pitch = pitch;
				PngEncOutputInfo info {};
				const auto       size = PngEncEncode(encoder.handle, &param, &info);
				CHECK(size > 0 && info.data_size == static_cast<uint32_t>(size));
				CHECK(info.processed_height == 2);
				int   width = 0, height = 0, components = 0;
				auto* decoded =
				    stbi_load_from_memory(png.data(), size, &width, &height, &components, 0);
				CHECK(decoded != nullptr);
				CHECK(width == 2 && height == 2 && components == (alpha ? 4 : 3));
				CHECK(std::memcmp(decoded, expected.data(), expected.size()) == 0);
				stbi_image_free(decoded);
			}
		}
	}
}

void TestFilterMasks() {
	Encoder                            encoder;
	alignas(4) std::array<uint8_t, 16> pixels {};
	std::array<uint8_t, 1024>          png {};
	auto param = EncodeParam(pixels.data(), pixels.size(), png.data(), png.size());
	for (uint16_t mask = 0; mask <= 15; ++mask) {
		param.filter_type = mask;
		const auto size   = PngEncEncode(encoder.handle, &param, nullptr);
		CHECK(size > 0);
		CHECK(std::memcmp(png.data() + 37, "IDAT", 4) == 0);
		std::array<char, 18> filtered {};
		CHECK(stbi_zlib_decode_buffer(filtered.data(), filtered.size(),
		                              reinterpret_cast<const char*>(png.data() + 41),
		                              ReadBe32(png.data() + 33)) == filtered.size());
		for (size_t row = 0; row < 2; ++row) {
			const auto filter = static_cast<uint8_t>(filtered[row * 9]);
			CHECK(filter <= 4);
			CHECK(filter == 0 ? (mask == 0 || mask == 15) : (mask & (1u << (filter - 1u))) != 0);
		}
	}
	param.filter_type = 16;
	CHECK(PngEncEncode(encoder.handle, &param, nullptr) == PNG_ENC_ERROR_INVALID_PARAM);
	for (uint32_t count = 0; count <= 4; ++count) {
		Encoder limited(count);
		for (uint16_t mask = 0; mask <= 15; ++mask) {
			param.filter_type = mask;
			const auto result = PngEncEncode(limited.handle, &param, nullptr);
			CHECK(std::popcount(mask) <= count ? result > 0
			                                   : result == PNG_ENC_ERROR_INVALID_PARAM);
		}
	}
}

void TestOutputCapacity() {
	Encoder                            encoder;
	alignas(4) std::array<uint8_t, 16> pixels {};
	std::array<uint8_t, 1024>          png {};
	auto       param = EncodeParam(pixels.data(), pixels.size(), png.data(), png.size());
	const auto size  = PngEncEncode(encoder.handle, &param, nullptr);
	CHECK(size > 0);
	std::vector<uint8_t> guarded(size + 2, 0xa5);
	param.png_mem_addr = guarded.data() + 1;
	param.png_mem_size = size;
	PngEncOutputInfo info {};
	CHECK(PngEncEncode(encoder.handle, &param, &info) == size);
	CHECK(guarded.front() == 0xa5 && guarded.back() == 0xa5);
	CHECK(std::memcmp(guarded.data() + 1, png.data(), size) == 0);
	std::fill(guarded.begin(), guarded.end(), 0xa5);
	param.png_mem_size = size - 1;
	CHECK(PngEncEncode(encoder.handle, &param, &info) == PNG_ENC_ERROR_DATA_OVERFLOW);
	CHECK(info.data_size == 0 && info.processed_height == 0);
	CHECK(std::all_of(guarded.begin(), guarded.end(), [](uint8_t byte) { return byte == 0xa5; }));
}

void TestValidation() {
	Encoder           encoder;
	PngEncCreateParam create {sizeof(PngEncCreateParam), 0, 2, 4};
	CHECK(PngEncQueryMemorySize(nullptr) == PNG_ENC_ERROR_INVALID_ADDR);
	CHECK(PngEncCreate(&create, nullptr, sizeof(PngEncContext), &encoder.handle) ==
	      PNG_ENC_ERROR_INVALID_ADDR);
	CHECK(PngEncCreate(&create, &encoder.context, 0, &encoder.handle) ==
	      PNG_ENC_ERROR_INVALID_SIZE);
	create.max_image_width = 0;
	CHECK(PngEncQueryMemorySize(&create) == PNG_ENC_ERROR_INVALID_SIZE);
	create.max_image_width = PNG_ENC_MAX_IMAGE_WIDTH + 1;
	CHECK(PngEncQueryMemorySize(&create) == PNG_ENC_ERROR_INVALID_SIZE);
	create.max_image_width = 2;
	create.this_size       = 0;
	CHECK(PngEncQueryMemorySize(&create) == PNG_ENC_ERROR_INVALID_PARAM);
	create.this_size         = sizeof(create);
	create.max_filter_number = 5;
	CHECK(PngEncQueryMemorySize(&create) == PNG_ENC_ERROR_INVALID_PARAM);
	alignas(4) std::array<uint8_t, 16> pixels {};
	std::array<uint8_t, 1024>          png {};
	const auto valid = EncodeParam(pixels.data(), pixels.size(), png.data(), png.size());
	CHECK(PngEncEncode(nullptr, &valid, nullptr) == PNG_ENC_ERROR_INVALID_HANDLE);
	CHECK(PngEncEncode(encoder.handle, nullptr, nullptr) == PNG_ENC_ERROR_INVALID_PARAM);
	for (const auto width: {0u, PNG_ENC_MAX_IMAGE_WIDTH + 1, 0x40000000u, 0xffffffffu}) {
		auto param        = valid;
		param.image_width = width;
		CHECK(PngEncEncode(encoder.handle, &param, nullptr) == PNG_ENC_ERROR_INVALID_SIZE);
	}
	for (const auto height: {0u, PNG_ENC_MAX_IMAGE_HEIGHT + 1, 0xffffffffu}) {
		auto param           = valid;
		param.image_width    = 1;
		param.image_height   = height;
		param.image_mem_size = 0xffffffffu;
		CHECK(PngEncEncode(encoder.handle, &param, nullptr) == PNG_ENC_ERROR_INVALID_SIZE);
	}
	for (const auto height: {262144u, 400000u, 0x7ffffu}) {
		auto param           = valid;
		param.image_width    = 1024;
		param.image_height   = height;
		param.image_pitch    = 4096;
		param.image_mem_size = 0xffffffffu;
		CHECK(PngEncEncode(encoder.handle, &param, nullptr) == PNG_ENC_ERROR_INVALID_SIZE);
	}
	for (const auto pitch: {0u, 7u, 9u, 0xfffffffcu}) {
		auto param        = valid;
		param.image_pitch = pitch;
		CHECK(PngEncEncode(encoder.handle, &param, nullptr) == PNG_ENC_ERROR_INVALID_SIZE);
	}
	auto param           = valid;
	param.image_pitch    = 12;
	param.image_mem_size = 20;
	CHECK(PngEncEncode(encoder.handle, &param, nullptr) == PNG_ENC_ERROR_INVALID_SIZE);
	param = valid;
	param.image_mem_size--;
	CHECK(PngEncEncode(encoder.handle, &param, nullptr) == PNG_ENC_ERROR_INVALID_SIZE);
	param                = valid;
	param.image_mem_addr = nullptr;
	CHECK(PngEncEncode(encoder.handle, &param, nullptr) == PNG_ENC_ERROR_INVALID_ADDR);
	param.image_mem_addr = pixels.data() + 1;
	CHECK(PngEncEncode(encoder.handle, &param, nullptr) == PNG_ENC_ERROR_INVALID_ADDR);
	param           = valid;
	param.bit_depth = 16;
	CHECK(PngEncEncode(encoder.handle, &param, nullptr) == PNG_ENC_ERROR_INVALID_PARAM);
	param             = valid;
	param.clut_number = 1;
	CHECK(PngEncEncode(encoder.handle, &param, nullptr) == PNG_ENC_ERROR_INVALID_PARAM);
	param                   = valid;
	param.compression_level = 10;
	CHECK(PngEncEncode(encoder.handle, &param, nullptr) == PNG_ENC_ERROR_INVALID_PARAM);
}
} // namespace

int main() {
	TestPixels();
	TestFilterMasks();
	TestOutputCapacity();
	TestValidation();
	std::puts("PNG encoder tests passed");
}
