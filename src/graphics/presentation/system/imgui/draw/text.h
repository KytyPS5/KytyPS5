#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_IMGUI_DRAW_TEXT_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_IMGUI_DRAW_TEXT_H_

#include "graphics/presentation/system/common/props/textStyles.h"
#include "graphics/presentation/system/core/geometry.h"

#include <string>
#include <string_view>
#include <vector>

struct ImDrawList;
struct ImFont;

namespace Libs::Graphics::SystemUi {

/// Text broken into the lines it will be drawn as. Every length in it is in output pixels, as it is
/// laid out for the screen it is drawn on.
struct TextLayout {
	/// One line of the text, as it will be drawn.
	struct Line {
		std::string text;
		float       width = 0.0f;
	};
	std::vector<Line> lines;
	ImFont*           font        = nullptr;
	float             font_size   = 0.0f;
	float             line_height = 0.0f;
	// The width of the widest line by the height of all lines.
	Vec2 size;
};

/// Everything LayoutText's result depends on apart from the text itself, as it resolves them, so
/// two calls with an equal key and equal text bytes give the same layout. Colour and alignment
/// aren't in it, as DrawText reads those from the props. Theme and scale reach it through what they
/// resolve to. Its sizes and width are in output pixels, after the scale.
struct TextLayoutKey {
	ImFont* font        = nullptr;
	float   font_size   = 0.0f;
	float   line_height = 0.0f;
	// The width laid out in, with every width that means the same thing made equal.
	float width = 0.0f;
	// The density the font bakes at, which decides the glyphs measured.
	float        density       = 0.0f;
	int          max_lines     = 0;
	TextOverflow overflow      = TextOverflow::Ellipsis;
	bool         shrink_to_fit = false;

	friend bool operator==(const TextLayoutKey&, const TextLayoutKey&) = default;
};

/// What to draw and how. The text is only viewed, so whoever builds the props must keep it alive
/// until the text is drawn. Keep owned strings in the view, not in props kept across frames.
struct DrawTextProps {
	std::string_view text;
	TextStyles       styles;
};

/// The key of laying `props` out in `max_width` pixels with the current fonts and scale. Call it on
/// the present thread inside a ContextScope, like LayoutText.
[[nodiscard]] TextLayoutKey MakeTextLayoutKey(const DrawTextProps& props, float max_width);

/// Lays `props` out in `max_width` pixels. It picks the font and its size from the style, with the
/// current fonts and scale, breaks wrapped text into lines, and shortens text that doesn't fit,
/// marking with an ellipsis any text it drops. Measure with this, place the result, then draw the
/// same layout. A slot with no width shows nothing unless the text is clipped. Call it on the
/// present thread only, since measuring can add glyphs to ImGui's shared font atlas, and inside a
/// ContextScope.
[[nodiscard]] TextLayout LayoutText(const DrawTextProps& props, float max_width);

/// How wide `props` is in output pixels as one line at its style's size, trailing spaces left off,
/// without laying it out, or 0 with no font. Call it on the present thread inside a ContextScope,
/// like LayoutText.
[[nodiscard]] float MeasureTextLine(const DrawTextProps& props);

/// How wide the mark that shows text was cut short is in `font` at `size` pixels, the same mark
/// LayoutText draws, or 0 with no font. A truncated text can't be narrower than this.
[[nodiscard]] float EllipsisWidth(ImFont* font, float size);

/// Draws `layout` in `rect`, each line aligned as `props` says and centred in its line height.
/// Clipped text is clipped to `rect`. `opacity` fades it with its element. Glyphs go left to right
/// in the order of the text, with no shaping and no reordering, so Arabic has to be shaped before
/// it gets here or its letters come out unjoined and in the wrong order. The overlay does that for
/// its trophy subtitle, in PrepareTrophySubtitle, and a trophy's name from a game gets no such
/// help.
void DrawText(ImDrawList& list, const TextLayout& layout, const DrawTextProps& props,
              const Rect& rect, float opacity = 1.0f);

} // namespace Libs::Graphics::SystemUi

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_IMGUI_DRAW_TEXT_H_
