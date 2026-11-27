#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_IMGUI_DRAW_SHAPE_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_IMGUI_DRAW_SHAPE_H_

#include "graphics/presentation/system/core/color.h"
#include "graphics/presentation/system/core/geometry.h"

#include <span>

struct ImDrawList;

namespace Libs::Graphics::SystemUi {

/// How a circle or a triangle looks. Lengths are design units, as a box's are. A transparent
/// colour or a zero border width draws no border, and a transparent fill no fill.
struct PathShapeStyle {
	Color fill;
	Color border;
	float border_width = 0.0f;
	// How round a triangle's corners are. A circle ignores it.
	float radius = 0.0f;
};

/// Draws the circle filling the square middle of `rect` at the current scale, its fill and then its
/// border just inside its edge, as a box's border is. ImGui's own circle, as a fully rounded box
/// keeps a pixel of flat side. It is drawn in finer segments than ImGui would choose, as small
/// rings otherwise look uneven. `opacity` fades it with its element. It needs an open ContextScope.
void DrawCircle(ImDrawList& list, const Rect& rect, const PathShapeStyle& style,
                float opacity = 1.0f);

/// Draws a triangle pointing up that fills `rect`, its base along the bottom and its tip at the
/// middle of the top, at the current scale. Its corners are rounded by the style's radius, and it
/// still reaches every side of `rect`, so the rectangle is the shape's visible bounds. Its border
/// is drawn just inside its edge. An outline's corners are at least as round as it is thick, so
/// a sharp corner never pushes its stroke past the edge. `opacity` fades it with its element. It
/// needs an open ContextScope.
void DrawTriangle(ImDrawList& list, const Rect& rect, const PathShapeStyle& style,
                  float opacity = 1.0f);

/// Draws a line through `points`, in output pixels, `width_pixels` thick, with round ends and round
/// joins, one rounded segment per pair of points, or a dot for a single point. Segments overlap at
/// their joins, so a translucent line is a little darker there. `opacity` fades it with its
/// element.
void DrawRoundStroke(ImDrawList& list, std::span<const Vec2> points, float width_pixels,
                     Color color, float opacity = 1.0f);

} // namespace Libs::Graphics::SystemUi

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_IMGUI_DRAW_SHAPE_H_
