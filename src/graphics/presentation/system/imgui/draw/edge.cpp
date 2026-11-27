#include "graphics/presentation/system/imgui/draw/edge.h"

#include "graphics/presentation/system/core/context.h"
#include "graphics/presentation/system/imgui/convert.h"
#include "graphics/presentation/system/imgui/draw/box.h"
#include "imgui.h"

#include <algorithm>
#include <cmath>

namespace Libs::Graphics::SystemUi {

void DrawEdgeBand(ImDrawList& list, const Rect& rect, float radius, float width, Color color,
                  float opacity) {
	const Color fill = color.ScaleAlpha(opacity);
	if (!std::isfinite(width) || width <= 0.0f || fill.a == 0) {
		return;
	}
	const float band = std::min(CurrentScaleContext().Border(width), rect.height);
	if (band <= 0.0f) {
		return;
	}
	// The box's own rounded fill, cut to the band's strip, so its corners match the box's to the
	// vertex. A clip can only be a rectangle, which this strip is. Intersected with the clip
	// already set, so a clipping container still clips the band. Its bottom edge is on a whole
	// pixel, as the renderer cuts a clip to whole pixels and would otherwise drop up to one from a
	// box that starts part way through a pixel.
	const auto [rect_start, rect_end] = ToImRect(rect);
	list.PushClipRect(rect_start, {rect_end.x, std::round(rect.y + band)}, true);
	list.AddRectFilled(rect_start, rect_end, ToImU32(fill), BoxRounding(rect, radius));
	list.PopClipRect();
}

} // namespace Libs::Graphics::SystemUi
