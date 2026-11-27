#include "graphics/presentation/system/imgui/draw/box.h"

#include "graphics/presentation/system/core/context.h"
#include "graphics/presentation/system/imgui/convert.h"
#include "imgui.h"

#include <algorithm>

namespace Libs::Graphics::SystemUi {

namespace {

// AddRect pulls its path half a pixel inside the box it is given, to keep thin lines crisp.
constexpr float ADD_RECT_INSET = 0.5f;

// A gradient's shape is drawn in this, then recoloured vertex by vertex.
constexpr ImU32 SHAPE_COLOR   = IM_COL32_WHITE;
constexpr ImU32 ALPHA_MASK    = 0xFFu;
constexpr float CHANNEL_SCALE = 255.0f;

// Recolours the vertices `list` added from `first` on with `gradient` across `rect`, each by where
// it lies. The shape was drawn opaque, so a vertex's own alpha is how much of it ImGui's smoothed
// edge kept, and it scales the gradient's alpha there, keeping the edge smooth.
void ShadeLinear(ImDrawList& list, int first, const Rect& rect, const LinearGradient& gradient) {
	const GradientLine line = MakeGradientLine(rect, gradient.angle_degrees);
	for (int index = first; index < list.VtxBuffer.Size; ++index) {
		ImDrawVert& vertex = list.VtxBuffer[index];
		const Color color =
		    Lerp(gradient.from, gradient.to, line.Position({vertex.pos.x, vertex.pos.y}));
		const ImU32 alpha    = (vertex.col >> IM_COL32_A_SHIFT) & ALPHA_MASK;
		const float coverage = static_cast<float>(alpha) / CHANNEL_SCALE;
		vertex.col           = ToImU32(color.ScaleAlpha(coverage));
	}
}

// The box's fill, its gradient when it has one and its colour otherwise.
void DrawFill(ImDrawList& list, const Rect& rect, const DrawBoxStyle& style, float radius,
              float opacity) {
	const auto [rect_start, rect_end] = ToImRect(rect);
	if (style.fill_gradient) {
		const LinearGradient gradient = style.fill_gradient->ScaleAlpha(opacity);
		if (gradient.Visible()) {
			const int first = list.VtxBuffer.Size;
			list.AddRectFilled(rect_start, rect_end, SHAPE_COLOR, radius);
			ShadeLinear(list, first, rect, gradient);
		}
		return;
	}
	const Color fill = style.fill.ScaleAlpha(opacity);
	if (fill.a > 0) {
		list.AddRectFilled(rect_start, rect_end, ToImU32(fill), radius);
	}
}

// The box's border, its gradient when it has one and its colour otherwise.
void DrawBorder(ImDrawList& list, const Rect& rect, const DrawBoxStyle& style, float radius,
                float opacity) {
	const std::optional<LinearGradient> gradient =
	    style.border_gradient ? std::optional(style.border_gradient->ScaleAlpha(opacity))
	                          : std::nullopt;
	const Color border  = style.border.ScaleAlpha(opacity);
	const bool  visible = gradient ? gradient->Visible() : border.a > 0;
	if (!(style.border_width > 0.0f) || !visible) {
		return;
	}
	// ImGui centres a stroke on its path, so the path sits half the width inside the box, less
	// the half pixel AddRect adds, which puts the stroke's outer edge on the box's edge.
	const float width = CurrentScaleContext().Border(style.border_width);
	const float half  = width / 2.0f;
	const Rect  path  = rect.Inset(EdgeInsets(half - ADD_RECT_INSET));

	const auto [path_start, path_end] = ToImRect(path);
	const int first                   = list.VtxBuffer.Size;
	list.AddRect(path_start, path_end, gradient ? SHAPE_COLOR : ToImU32(border), radius - half,
	             width);
	if (gradient) {
		ShadeLinear(list, first, rect, *gradient);
	}
}

} // namespace

void DrawBox(ImDrawList& list, const Rect& rect, const DrawBoxStyle& style, float opacity) {
	const float radius = BoxRounding(rect, style.radius);
	DrawFill(list, rect, style, radius, opacity);
	DrawBorder(list, rect, style, radius, opacity);
}

float BoxRounding(const Rect& rect, float radius) {
	return std::min(CurrentScaleContext().Px(radius), std::min(rect.width, rect.height) / 2.0f);
}

} // namespace Libs::Graphics::SystemUi
