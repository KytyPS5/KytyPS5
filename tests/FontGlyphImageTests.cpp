#define STB_TRUETYPE_IMPLEMENTATION
#include "libs/fontGlyphImage.h"
#include "stb_truetype.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>

namespace {

int failures = 0;

#define CHECK(condition)                                                                           \
	do {                                                                                           \
		if (!(condition)) {                                                                        \
			std::fprintf(stderr, "FontGlyphImageTests:%d: %s\n", __LINE__, #condition);            \
			failures++;                                                                            \
		}                                                                                          \
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
	CHECK(Libs::Font::detail::rasterize_stb_glyph(font, 'A', 4.0f, 0.01f, &image, &width, &height));
	CHECK(width == Libs::Font::detail::FONT_GLYPH_MAX_DIM);
	CHECK(height > 0 && height < 128);
	CHECK(image.size() == static_cast<size_t>(width) * height);
}

void TestBitmapBoxEligibility(const stbtt_fontinfo* font) {
	int x0 = 0;
	int y0 = 0;
	int x1 = 0;
	int y1 = 0;
	CHECK(Libs::Font::detail::get_stb_glyph_bitmap_box(font, 'A', 1.0f, 1.0f, &x0, &y0, &x1, &y1));
	CHECK(x1 - x0 == 1024 && y1 - y0 == 1024);
	CHECK(Libs::Font::detail::get_stb_glyph_bitmap_box(font, 'A', 4.0f, 0.01f, &x0, &y0, &x1, &y1));
	CHECK(x1 - x0 == Libs::Font::detail::FONT_GLYPH_MAX_DIM);
}

void TestMetricsSource(const stbtt_fontinfo* font) {
	Libs::Font::detail::GlyphMetrics metrics {};
	CHECK(Libs::Font::detail::get_glyph_metrics(font, 'A', 1.0f, 1.0f, 64.0f, &metrics) ==
	      Libs::Font::detail::GlyphMetricsSource::TrueType);
	CHECK(metrics.width == 1024.0f && metrics.height == 1024.0f);
	CHECK(metrics.horizontal.advance == 1024.0f && metrics.vertical.advance == 64.0f);

	CHECK(Libs::Font::detail::get_glyph_metrics(font, 'A', 5.0f, 4.0f, 64.0f, &metrics) ==
	      Libs::Font::detail::GlyphMetricsSource::Bitmap);
	CHECK(metrics.width == 32.0f && metrics.height == 64.0f);
	CHECK(metrics.horizontal.bearing_y == 48.0f && metrics.horizontal.advance == 32.0f);
	CHECK(metrics.vertical.advance == 64.0f);
	CHECK(Libs::Font::detail::get_glyph_metrics(font, 'A', 4.0f, 5.0f, 64.0f, &metrics) ==
	      Libs::Font::detail::GlyphMetricsSource::Bitmap);
	CHECK(metrics.width == 32.0f && metrics.height == 64.0f);

	CHECK(Libs::Font::detail::get_glyph_metrics(font, 'A', 4.0f, 0.01f, 64.0f, &metrics) ==
	      Libs::Font::detail::GlyphMetricsSource::TrueType);
	CHECK(metrics.horizontal.advance == 4096.0f && metrics.vertical.advance == 64.0f);

	CHECK(Libs::Font::detail::get_glyph_metrics(font, ' ', 1.0f, 1.0f, 64.0f, &metrics) ==
	      Libs::Font::detail::GlyphMetricsSource::TrueType);
	CHECK(metrics.width == 0.0f && metrics.height == 0.0f);
	CHECK(metrics.horizontal.advance == 512.0f);

	CHECK(Libs::Font::detail::get_glyph_metrics(nullptr, 'A', 0.0f, 0.0f, 0.0f, &metrics) ==
	      Libs::Font::detail::GlyphMetricsSource::Bitmap);
	CHECK(metrics.width == 8.0f && metrics.height == 16.0f);
	CHECK(metrics.horizontal.bearing_y == 12.0f && metrics.vertical.advance == 16.0f);
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
	CHECK(!Libs::Font::detail::rasterize_stb_glyph(font, 'A', 5.0f, 4.0f, &image, &width, &height));
	CHECK(image == std::vector<uint8_t> {0x55});
	CHECK(width == 17 && height == 23);
	CHECK(!Libs::Font::detail::rasterize_stb_glyph(font, 'A', 4.0f, 5.0f, &image, &width, &height));
	CHECK(image == std::vector<uint8_t> {0x55});
	CHECK(width == 17 && height == 23);
	int x0 = 31;
	int y0 = 37;
	int x1 = 41;
	int y1 = 43;
	CHECK(!Libs::Font::detail::get_stb_glyph_bitmap_box(font, 'A', 5.0f, 4.0f, &x0, &y0, &x1, &y1));
	CHECK(x0 == 31 && y0 == 37 && x1 == 41 && y1 == 43);
	CHECK(!Libs::Font::detail::get_stb_glyph_bitmap_box(font, 'A', 4.0f, 5.0f, &x0, &y0, &x1, &y1));
	CHECK(x0 == 31 && y0 == 37 && x1 == 41 && y1 == 43);
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
	TestBitmapBoxEligibility(&font);
	TestMetricsSource(&font);
	TestBlankGlyph(&font);
	TestOversizedGlyphsAreRejected(&font);
	return failures == 0 ? 0 : 1;
}
