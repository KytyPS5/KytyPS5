#pragma once

#include <cstdint>
#include <vector>

struct stbtt_fontinfo;

namespace Libs::Font::detail {

// Keep oversized requests on the existing bitmap-font fallback path.
constexpr int FONT_GLYPH_MAX_DIM = 4096;

bool rasterize_stb_glyph(const stbtt_fontinfo* font, int codepoint, float scale_x, float scale_y,
                         std::vector<uint8_t>* image, uint32_t* width, uint32_t* height);

} // namespace Libs::Font::detail
