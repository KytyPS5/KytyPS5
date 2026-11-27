#include "graphics/presentation/system/imgui/draw/text.h"

#include "graphics/presentation/system/core/context.h"
#include "graphics/presentation/system/imgui/convert.h"

#include <algorithm>
#include <cfloat>
#include <cmath>

namespace Libs::Graphics::SystemUi {

namespace {

// Text that doesn't fit shrinks to no less than this share of its size before it is cut short.
constexpr float SHRINK_LIMIT = 0.85f;
// Text this close to its space still fits. Measuring at a fractional size rounds slightly, and
// shrunk text must not be cut short by a rounding error.
constexpr float            FIT_EPSILON   = 0.5f;
constexpr ImWchar          ELLIPSIS      = 0x2026;
constexpr std::string_view ELLIPSIS_UTF8 = "\xE2\x80\xA6";
constexpr std::string_view DOTS          = "...";

// The current font for `weight`, or ImGui's current font when the context's fonts are missing.
ImFont* ResolveFont(FontWeight weight) {
	ImFont* font = CurrentFontsContext().For(weight);
	return font != nullptr ? font : ImGui::GetFont();
}

// How wide `text` is in `font` at `size` pixels, on one line.
float Width(ImFont* font, float size, std::string_view text) {
	return font->CalcTextSizeA(size, FLT_MAX, 0.0f, text.data(), text.data() + text.size()).x;
}

// The mark `font` shows where text was cut short, its own ellipsis character when it has one, or
// three dots.
std::string_view EllipsisMark(ImFont* font) {
	return font->IsGlyphInFont(ELLIPSIS) ? ELLIPSIS_UTF8 : DOTS;
}

// `text` without trailing spaces.
std::string_view TrimEnd(std::string_view text) {
	while (!text.empty() && text.back() == ' ') {
		text.remove_suffix(1);
	}
	return text;
}

// `text` cut short with an ellipsis so it fits in `max_width`. It is left whole when it fits,
// unless `dropped` says text after it was dropped, which the ellipsis then shows. The ellipsis is
// the font's own character when it has one, or three dots. ImGui stops measuring at the first
// whole character that doesn't fit, so a multi-byte character is never split.
std::string Ellipsize(ImFont* font, float size, std::string_view text, float max_width,
                      bool dropped) {
	if (!dropped && Width(font, size, text) <= max_width + FIT_EPSILON) {
		return std::string(text);
	}
	const std::string_view mark  = EllipsisMark(font);
	const float            space = std::max(0.0f, max_width - Width(font, size, mark));
	const char*            fits  = text.data();
	font->CalcTextSizeA(size, space, 0.0f, text.data(), text.data() + text.size(), &fits);
	return std::string(TrimEnd({text.data(), static_cast<size_t>(fits - text.data())}))
	    .append(mark);
}

// Breaks `paragraph`, which has no line breaks, into lines no wider than `max_width`, at spaces
// where it can, and stops once there are more than `limit` lines. A word too long for a line is
// broken where it reaches the edge. Every line, even an empty one, points into `paragraph`, so
// its position in the whole text can be found.
void AppendWrapped(ImFont* font, float size, std::string_view paragraph, float max_width,
                   size_t limit, std::vector<std::string_view>& lines) {
	const char* begin = paragraph.data();
	const char* end   = begin + paragraph.size();
	if (begin == end) {
		lines.emplace_back(begin, 0);
	}
	while (begin < end && lines.size() <= limit) {
		// ImGui always moves on by at least one whole character, so this loop ends.
		const char* cut = font->CalcWordWrapPosition(size, begin, end, max_width);
		lines.push_back(TrimEnd({begin, static_cast<size_t>(cut - begin)}));
		begin = cut;
		while (begin < end && *begin == ' ') {
			begin++;
		}
	}
}

// Fills `layout` with one line of `props`, shrunk and cut short as its props ask. Text after a
// line break doesn't fit on one line, so ellipsis text shows that it was dropped.
void LayoutSingleLine(TextLayout& layout, const DrawTextProps& props, float max_width) {
	const size_t newline = props.text.find('\n');
	const auto   text    = props.text.substr(0, newline);
	const bool   dropped = newline != std::string_view::npos;
	float        width   = Width(layout.font, layout.font_size, text);
	if (props.styles.shrink_to_fit && width > max_width) {
		layout.font_size *= std::max(max_width / width, SHRINK_LIMIT);
		width = Width(layout.font, layout.font_size, text);
	}
	std::string shown(text);
	if (props.styles.overflow == TextOverflow::Ellipsis &&
	    (dropped || width > max_width + FIT_EPSILON)) {
		shown = Ellipsize(layout.font, layout.font_size, text, max_width, dropped);
		width = Width(layout.font, layout.font_size, shown);
	}
	layout.lines.push_back({std::move(shown), width});
}

// Fills `layout` with `props` wrapped to `max_width`. When there are more lines than allowed,
// the last one shown holds as much of its own paragraph as fits, followed by an ellipsis, since
// text after it was dropped.
void LayoutWrapped(TextLayout& layout, const DrawTextProps& props, float max_width) {
	const size_t limit =
	    props.styles.max_lines > 0 ? static_cast<size_t>(props.styles.max_lines) : SIZE_MAX - 1;
	std::vector<std::string_view> lines;
	for (size_t start = 0; lines.size() <= limit;) {
		const size_t newline = props.text.find('\n', start);
		AppendWrapped(layout.font, layout.font_size, props.text.substr(start, newline - start),
		              max_width, limit, lines);
		if (newline == std::string_view::npos) {
			break;
		}
		start = newline + 1;
	}
	const bool   dropped = lines.size() > limit;
	const size_t shown   = std::min(lines.size(), limit);
	for (size_t index = 0; index < shown; index++) {
		std::string text(lines[index]);
		if (dropped && index + 1 == shown) {
			const auto from = static_cast<size_t>(lines[index].data() - props.text.data());
			const auto tail = props.text.substr(from, props.text.find('\n', from) - from);
			text            = Ellipsize(layout.font, layout.font_size, tail, max_width, true);
		}
		const float width = Width(layout.font, layout.font_size, text);
		layout.lines.push_back({std::move(text), width});
	}
}

// The width `props` lays out in for `max_width`. A slot with no width shows nothing, unless the
// text is clipped, where drawing clips it anyway, and gives 0. A width that isn't a number doesn't
// constrain the text.
float LayoutWidth(const DrawTextProps& props, float max_width) {
	if (max_width <= 0.0f && props.styles.overflow != TextOverflow::Clip) {
		return 0.0f;
	}
	return max_width > 0.0f && std::isfinite(max_width) ? max_width : FLT_MAX;
}

} // namespace

TextLayoutKey MakeTextLayoutKey(const DrawTextProps& props, float max_width) {
	const ScaleContext& scale = CurrentScaleContext();
	TextLayoutKey       key;
	key.font          = ResolveFont(props.styles.type.weight);
	key.font_size     = scale.FontSize(props.styles.type);
	key.line_height   = std::max(scale.Px(props.styles.type.line.value), key.font_size);
	key.width         = LayoutWidth(props, max_width);
	key.density       = key.font != nullptr ? key.font->CurrentRasterizerDensity : 0.0f;
	key.max_lines     = props.styles.max_lines;
	key.overflow      = props.styles.overflow;
	key.shrink_to_fit = props.styles.shrink_to_fit;
	return key;
}

TextLayout LayoutText(const DrawTextProps& props, float max_width) {
	// From the key, so a kept layout can't depend on anything the key leaves out.
	const TextLayoutKey key = MakeTextLayoutKey(props, max_width);
	TextLayout          layout;
	layout.font        = key.font;
	layout.font_size   = key.font_size;
	layout.line_height = key.line_height;
	const float width  = key.width;
	if (props.text.empty() || layout.font == nullptr || width <= 0.0f) {
		return layout;
	}
	if (props.styles.overflow == TextOverflow::Wrap) {
		LayoutWrapped(layout, props, width);
	} else {
		LayoutSingleLine(layout, props, width);
	}
	for (const auto& line: layout.lines) {
		layout.size.x = std::max(layout.size.x, line.width);
	}
	layout.size.y = layout.line_height * static_cast<float>(layout.lines.size());
	return layout;
}

float MeasureTextLine(const DrawTextProps& props) {
	const std::string_view text = TrimEnd(props.text);
	ImFont*                font = ResolveFont(props.styles.type.weight);
	// Empty text may have no data, and ImGui would measure that up to a terminator it doesn't have.
	if (text.empty() || font == nullptr) {
		return 0.0f;
	}
	return Width(font, CurrentScaleContext().FontSize(props.styles.type), text);
}

float EllipsisWidth(ImFont* font, float size) {
	return font != nullptr ? Width(font, size, EllipsisMark(font)) : 0.0f;
}

void DrawText(ImDrawList& list, const TextLayout& layout, const DrawTextProps& props,
              const Rect& rect, float opacity) {
	const Color color = props.styles.color.ScaleAlpha(opacity);
	if (layout.font == nullptr || layout.lines.empty() || color.a == 0) {
		return;
	}

	const bool clip = props.styles.overflow == TextOverflow::Clip;
	if (clip) {
		const auto [rect_start, rect_end] = ToImRect(rect);
		list.PushClipRect(rect_start, rect_end, true);
	}

	const float glyph_top = (layout.line_height - layout.font_size) / 2.0f;
	for (size_t index = 0; index < layout.lines.size(); index++) {
		const auto&  line  = layout.lines[index];
		const float  spare = rect.width - line.width;
		const float  shift = props.styles.align == TextAlign::Center ? spare / 2.0f
		                     : props.styles.align == TextAlign::End  ? spare
		                                                             : 0.0f;
		const ImVec2 position {
		    SnapPixel(rect.x + shift),
		    SnapPixel(rect.y + layout.line_height * static_cast<float>(index) + glyph_top)};
		list.AddText(layout.font, layout.font_size, position, ToImU32(color), line.text.data(),
		             line.text.data() + line.text.size());
	}

	if (clip) {
		list.PopClipRect();
	}
}

} // namespace Libs::Graphics::SystemUi
