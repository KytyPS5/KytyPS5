#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_IMGUI_DRAW_SHINE_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_IMGUI_DRAW_SHINE_H_

#include "graphics/presentation/system/core/color.h"
#include "graphics/presentation/system/core/geometry.h"

struct ImDrawList;

namespace Libs::Graphics::SystemUi {

/// How a shine looks, a soft band of light leaning right. Its minimum width is design units,
/// converted when it is drawn, and the rest have no unit.
struct ShineStyle {
	Color color;
	// How far the band leans from vertical, its top to the right.
	float angle_degrees = 0.0f;
	// How wide the band is, as a share of the element's width, and the least it may be.
	float width_fraction = 0.0f;
	float min_width      = 0.0f;
	// How strong the band is at its brightest, as a fraction of the colour's own alpha.
	float peak_alpha = 0.0f;
	// How much of the band is at full brightness, in its middle. The rest fades smoothly to each
	// side.
	float plateau_fraction = 0.0f;
};

/// A shine across `rect`, whose corners are rounded by `radius` design units as a box's are, kept
/// inside that rounded shape. `position` runs from 0, the band wholly off the element's left edge,
/// to 1, wholly off its right, so a sweep from 0 to 1 starts and ends showing nothing. Positions
/// outside that or not finite, no strength, a clear colour or a width that isn't finite draw
/// nothing, and `opacity` fades all of it. It needs an open ContextScope.
void DrawShine(ImDrawList& list, const Rect& rect, float radius, const ShineStyle& style,
               float position, float opacity = 1.0f);

} // namespace Libs::Graphics::SystemUi

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_IMGUI_DRAW_SHINE_H_
