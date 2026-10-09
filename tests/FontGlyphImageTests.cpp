#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#include "libs/fontGlyphImage.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>

namespace {

int failures = 0;

#define CHECK(condition)                                                       \
	do {                                                                         \
		if (!(condition)) {                                                        \
			std::fprintf(stderr, "FontGlyphImageTests:%d: %s\n", __LINE__, #condition); \
			failures++;                                                             \
		}                                                                         \
	} while (false)

std::vector<uint8_t> ReadFont(const char* path) {
	std::ifstream file(path, std::ios::binary);
	return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

void TestRasterization(const stbtt_fontinfo* font) {
	std::vector<uint8_t> image;
	uint32_t             width  = 0;
	uint32_t             height = 0;
	CHECK(Libs::Font::detail::rasterize_stb_glyph(font, 'A', 1.0f, 1.0f, &image, &width, &height));
	CHECK(width == 1024 && height == 1024);
	CHECK(image.size() == static_cast<size_t>(width) * height);
	CHECK(std::any_of(image.begin(), image.end(), [](uint8_t value) { return value != 0; }));
}

void TestMaximumWidth(const stbtt_fontinfo* font) {
	std::vector<uint8_t> image;
	uint32_t             width  = 0;
	uint32_t             height = 0;
	CHECK(Libs::Font::detail::rasterize_stb_glyph(font, 'A', 4.0f, 0.01f, &image, &width,
	                                               &height));
	CHECK(width == Libs::Font::detail::FONT_GLYPH_MAX_DIM);
	CHECK(height > 0 && height < 128);
	CHECK(image.size() == static_cast<size_t>(width) * height);
}

void TestBlankGlyph(const stbtt_fontinfo* font) {
	std::vector<uint8_t> image;
	uint32_t             width  = 1;
	uint32_t             height = 1;
	CHECK(Libs::Font::detail::rasterize_stb_glyph(font, ' ', 1.0f, 1.0f, &image, &width, &height));
	CHECK(width == 0 && height == 0);
	CHECK(image.size() == 1 && image.front() == 0);
}

void TestOversizedGlyphsAreRejected(const stbtt_fontinfo* font) {
	std::vector<uint8_t> image {0x55};
	uint32_t             width  = 17;
	uint32_t             height = 23;
	CHECK(!Libs::Font::detail::rasterize_stb_glyph(font, 'A', 5.0f, 4.0f, &image, &width,
	                                               &height));
	CHECK(image == std::vector<uint8_t> {0x55});
	CHECK(width == 17 && height == 23);
	CHECK(!Libs::Font::detail::rasterize_stb_glyph(font, 'A', 4.0f, 5.0f, &image, &width,
	                                               &height));
	CHECK(image == std::vector<uint8_t> {0x55});
	CHECK(width == 17 && height == 23);
}

} // namespace

int main(int argc, char** argv) {
	if (argc != 2) {
		std::fprintf(stderr, "usage: FontGlyphImageTests <font-fixture>\n");
		return 2;
	}

	auto font_data = ReadFont(argv[1]);
	CHECK(!font_data.empty());
	if (font_data.empty()) {
		return 1;
	}
	stbtt_fontinfo font {};
	CHECK(stbtt_InitFont(&font, font_data.data(), 0) != 0);
	if (font.numGlyphs == 0) {
		return 1;
	}
	TestRasterization(&font);
	TestMaximumWidth(&font);
	TestBlankGlyph(&font);
	TestOversizedGlyphsAreRejected(&font);
	return failures == 0 ? 0 : 1;
}
