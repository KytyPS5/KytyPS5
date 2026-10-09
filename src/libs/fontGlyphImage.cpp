#include "libs/fontGlyphImage.h"

#include "stb_truetype.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace Libs::Font::detail {

bool get_stb_glyph_bitmap_box(const stbtt_fontinfo* font, int codepoint, float scale_x,
                              float scale_y, int* x0, int* y0, int* x1, int* y1) {
	if (font == nullptr || x0 == nullptr || y0 == nullptr || x1 == nullptr || y1 == nullptr) {
		return false;
	}

	int box_x0 = 0;
	int box_y0 = 0;
	int box_x1 = 0;
	int box_y1 = 0;
	stbtt_GetCodepointBitmapBox(font, codepoint, scale_x, scale_y, &box_x0, &box_y0, &box_x1,
	                            &box_y1);
	const int64_t glyph_width  = static_cast<int64_t>(box_x1) - box_x0;
	const int64_t glyph_height = static_cast<int64_t>(box_y1) - box_y0;
	if (glyph_width > FONT_GLYPH_MAX_DIM || glyph_height > FONT_GLYPH_MAX_DIM) {
		return false;
	}

	*x0 = box_x0;
	*y0 = box_y0;
	*x1 = box_x1;
	*y1 = box_y1;
	return true;
}

int bitmap_glyph_height(float requested_height) {
	const float scale = (requested_height > 1.0f ? requested_height : 16.0f);
	return std::clamp(static_cast<int>(scale + 0.5f), 8, FONT_BITMAP_MAX_DIM);
}

int bitmap_glyph_width(float requested_height) {
	return std::clamp((bitmap_glyph_height(requested_height) + 1) / 2, 4, FONT_BITMAP_MAX_DIM);
}

GlyphMetricsSource get_glyph_metrics(const stbtt_fontinfo* font, int codepoint, float scale_x,
                                     float scale_y, float requested_height, GlyphMetrics* metrics) {
	if (metrics == nullptr) {
		return GlyphMetricsSource::Bitmap;
	}

	int bitmap_x0 = 0;
	int bitmap_y0 = 0;
	int bitmap_x1 = 0;
	int bitmap_y1 = 0;
	if (get_stb_glyph_bitmap_box(font, codepoint, scale_x, scale_y, &bitmap_x0, &bitmap_y0,
	                             &bitmap_x1, &bitmap_y1)) {
		int advance = 0;
		int x0      = 0;
		int y0      = 0;
		int x1      = 0;
		int y1      = 0;
		stbtt_GetCodepointHMetrics(font, codepoint, &advance, nullptr);
		stbtt_GetCodepointBox(font, codepoint, &x0, &y0, &x1, &y1);

		metrics->width                = static_cast<float>(x1 - x0) * scale_x;
		metrics->height               = static_cast<float>(y1 - y0) * scale_y;
		metrics->horizontal.bearing_x = static_cast<float>(x0) * scale_x;
		metrics->horizontal.bearing_y = static_cast<float>(y1) * scale_y;
		metrics->horizontal.advance   = static_cast<float>(advance) * scale_x;
		metrics->vertical.bearing_x   = 0.0f;
		metrics->vertical.bearing_y   = 0.0f;
		metrics->vertical.advance     = requested_height;
		return GlyphMetricsSource::TrueType;
	}

	const float width             = static_cast<float>(bitmap_glyph_width(requested_height));
	const float height            = static_cast<float>(bitmap_glyph_height(requested_height));
	const float ascent            = height * 0.75f;
	metrics->width                = width;
	metrics->height               = height;
	metrics->horizontal.bearing_x = 0.0f;
	metrics->horizontal.bearing_y = ascent;
	metrics->horizontal.advance   = width;
	metrics->vertical.bearing_x   = 0.0f;
	metrics->vertical.bearing_y   = 0.0f;
	metrics->vertical.advance     = height;
	return GlyphMetricsSource::Bitmap;
}

bool rasterize_stb_glyph(const stbtt_fontinfo* font, int codepoint, float scale_x, float scale_y,
                         std::vector<uint8_t>* image, uint32_t* width, uint32_t* height) {
	if (font == nullptr || image == nullptr || width == nullptr || height == nullptr) {
		return false;
	}

	int x0 = 0;
	int y0 = 0;
	int x1 = 0;
	int y1 = 0;
	if (!get_stb_glyph_bitmap_box(font, codepoint, scale_x, scale_y, &x0, &y0, &x1, &y1)) {
		return false;
	}
	const int64_t glyph_width  = static_cast<int64_t>(x1) - x0;
	const int64_t glyph_height = static_cast<int64_t>(y1) - y0;

	*width  = static_cast<uint32_t>(std::max<int64_t>(glyph_width, 0));
	*height = static_cast<uint32_t>(std::max<int64_t>(glyph_height, 0));
	image->assign(std::max<size_t>(static_cast<size_t>(*width) * *height, 1), 0);
	if (*width == 0 || *height == 0) {
		return true;
	}

	stbtt_MakeCodepointBitmap(font, image->data(), static_cast<int>(*width),
	                          static_cast<int>(*height), static_cast<int>(*width), scale_x, scale_y,
	                          codepoint);
	return true;
}

} // namespace Libs::Font::detail
