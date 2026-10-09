#include "libs/fontGlyphImage.h"

#include "stb_truetype.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace Libs::Font::detail {

bool rasterize_stb_glyph(const stbtt_fontinfo* font, int codepoint, float scale_x, float scale_y,
                         std::vector<uint8_t>* image, uint32_t* width, uint32_t* height) {
	if (font == nullptr || image == nullptr || width == nullptr || height == nullptr) {
		return false;
	}

	int x0 = 0;
	int y0 = 0;
	int x1 = 0;
	int y1 = 0;
	stbtt_GetCodepointBitmapBox(font, codepoint, scale_x, scale_y, &x0, &y0, &x1, &y1);
	const int64_t glyph_width  = static_cast<int64_t>(x1) - x0;
	const int64_t glyph_height = static_cast<int64_t>(y1) - y0;
	if (glyph_width > FONT_GLYPH_MAX_DIM || glyph_height > FONT_GLYPH_MAX_DIM) {
		return false;
	}

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
