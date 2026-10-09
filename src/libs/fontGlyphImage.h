#pragma once

#include <cstdint>
#include <vector>

struct stbtt_fontinfo;

namespace Libs::Font::detail {

// Keep oversized requests on the existing bitmap-font fallback path.
constexpr int FONT_GLYPH_MAX_DIM  = 4096;
constexpr int FONT_BITMAP_MAX_DIM = 128;

struct GlyphMetrics {
	float width;
	float height;
	struct {
		float bearing_x;
		float bearing_y;
		float advance;
	} horizontal;
	struct {
		float bearing_x;
		float bearing_y;
		float advance;
	} vertical;
};

enum class GlyphMetricsSource { Bitmap, TrueType };

bool get_stb_glyph_bitmap_box(const stbtt_fontinfo* font, int codepoint, float scale_x,
                              float scale_y, int* x0, int* y0, int* x1, int* y1);

int                bitmap_glyph_height(float requested_height);
int                bitmap_glyph_width(float requested_height);
GlyphMetricsSource get_glyph_metrics(const stbtt_fontinfo* font, int codepoint, float scale_x,
                                     float scale_y, float requested_height, GlyphMetrics* metrics);

bool rasterize_stb_glyph(const stbtt_fontinfo* font, int codepoint, float scale_x, float scale_y,
                         std::vector<uint8_t>* image, uint32_t* width, uint32_t* height);

} // namespace Libs::Font::detail
