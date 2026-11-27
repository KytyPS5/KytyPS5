#include "graphics/presentation/system/imgui/draw/highlight.h"

#include "graphics/presentation/system/core/context.h"
#include "graphics/presentation/system/imgui/convert.h"
#include "graphics/presentation/system/imgui/draw/box.h"
#include "imgui.h"

#include <algorithm>
#include <cmath>

namespace Libs::Graphics::SystemUi {

namespace {

// Rings of an outside glow are this thin, so they blend into a halo rather than stripes.
constexpr float RING_WIDTH     = 1.5f;
constexpr int   MAX_RING_COUNT = 32;

// `rect` grown by `amount` pixels on every side.
Rect Expand(const Rect& rect, float amount) {
	return rect.Inset(EdgeInsets(-amount));
}

// A rounded outline `thickness` pixels wide whose inner edge lies `inner` pixels outside `rect`,
// whose corners have `rounding` pixels.
void StrokeOutside(ImDrawList& list, const Rect& rect, float rounding, float inner, float thickness,
                   Color color) {
	// ImGui centres strokes on their path, so the path sits half the thickness further out.
	// AddRect also pulls its path half a pixel inside the box it is given, to keep thin lines
	// crisp, so the box is grown by that half pixel too.
	constexpr float ADD_RECT_INSET = 0.5f;
	const float     half           = thickness / 2.0f;
	const Rect      bounds         = Expand(rect, inner + half + ADD_RECT_INSET);
	const auto [start, end]        = ToImRect(bounds);
	list.AddRect(start, end, ToImU32(color), rounding + inner + half, thickness);
}

// A halo of `color` that never covers `rect`, `spread` pixels wide, made of thin rings from its
// edge outward, fading quadratically from `peak` to nothing.
void GlowOutside(ImDrawList& list, const Rect& rect, float rounding, float spread, float peak,
                 Color color) {
	if (!(spread > 0.0f) || !(peak > 0.0f) || color.a == 0) {
		return;
	}
	// The clamp comes before the conversion, as a huge spread would not fit an integer.
	const int count = static_cast<int>(
	    std::ceil(std::clamp(spread / RING_WIDTH, 1.0f, static_cast<float>(MAX_RING_COUNT))));
	const float steps = static_cast<float>(count);
	const float ring  = spread / steps;
	for (int layer = 0; layer < count; layer++) {
		// Bright at the element's edge and soft at the outside.
		const float remaining = 1.0f - static_cast<float>(layer) / steps;
		StrokeOutside(list, rect, rounding, ring * static_cast<float>(layer), ring,
		              color.ScaleAlpha(peak * remaining * remaining));
	}
}

} // namespace

void DrawFocusRing(ImDrawList& list, const Rect& rect, float radius, const FocusRingStyle& style,
                   float opacity, float cover) {
	const ScaleContext& scale    = CurrentScaleContext();
	const float         rounding = BoxRounding(rect, radius);
	const float         ring     = scale.Border(style.ring_width);
	const float         keyline  = scale.Border(style.keyline_width);
	// Converted as DrawBox converts a border, so the ring starts exactly at the border's inner
	// edge. Written so a NaN or a negative cover gives none.
	const float start = cover > 0.0f ? -scale.Border(cover) : 0.0f;
	// Outward from the start, the keyline, then the light ring, then the glow touching the ring. A
	// dark band outside the ring read as a second border, so the keyline sits inside it, where it
	// still parts the ring from a light element.
	const float outer = start + keyline + ring;
	GlowOutside(list, Expand(rect, outer), rounding + outer, scale.Px(style.glow_radius),
	            ClampUnit(style.glow_peak) * ClampUnit(opacity), style.glow);
	StrokeOutside(list, rect, rounding, start, keyline, style.keyline.ScaleAlpha(opacity));
	StrokeOutside(list, rect, rounding, start + keyline, ring, style.ring.ScaleAlpha(opacity));
}

void DrawSelection(ImDrawList& list, const Rect& rect, float radius, const SelectionStyle& style,
                   float opacity) {
	const float rounding    = BoxRounding(rect, radius);
	const auto [start, end] = ToImRect(rect);
	list.AddRectFilled(start, end, ToImU32(style.tint.ScaleAlpha(opacity)), rounding);
	// The bar stops short of the rounded corners, so it never pokes past the element's outline.
	// ImGui caps its rounding a pixel short of half its width, which still reads as rounded ends.
	const float bar_width = std::min(CurrentScaleContext().Border(style.bar_width), rect.width);
	// BoxRounding already keeps the rounding within half the height.
	const float bar_inset = std::max(rounding, 0.0f);
	const Rect  bar {rect.x, rect.y + bar_inset, bar_width, rect.height - 2.0f * bar_inset};
	// A fully rounded element leaves no straight edge for the bar. ImGui would still draw a sliver
	// of a zero height rectangle, which shows as a dot, so nothing is drawn.
	if (!(bar.height > 0.0f)) {
		return;
	}
	const auto [bar_start, bar_end] = ToImRect(bar);
	list.AddRectFilled(bar_start, bar_end, ToImU32(style.bar.ScaleAlpha(opacity)),
	                   bar_width / 2.0f);
}

} // namespace Libs::Graphics::SystemUi
