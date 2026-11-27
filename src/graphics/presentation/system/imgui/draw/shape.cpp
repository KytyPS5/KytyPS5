#include "graphics/presentation/system/imgui/draw/shape.h"

#include "graphics/presentation/system/core/context.h"
#include "graphics/presentation/system/imgui/convert.h"
#include "imgui.h"
#include "imgui_internal.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <numbers>

namespace Libs::Graphics::SystemUi {

namespace {

// AddCircle pulls its path half a pixel inside the radius it is given, as AddRect does.
constexpr float ADD_CIRCLE_INSET = 0.5f;
// A quarter and a half turn, in radians.
constexpr float QUARTER_TURN = std::numbers::pi_v<float> / 2.0f;
constexpr float HALF_TURN    = std::numbers::pi_v<float>;
constexpr float FULL_TURN    = 2.0f * std::numbers::pi_v<float>;
// A triangle has three corners.
constexpr size_t TRIANGLE_CORNERS = 3;
// How far a circle's straight segments may stray from its curve, in pixels, a third of ImGui's
// default allowance. With ImGui's, a small ring's segments show as uneven beads along its edge.
constexpr float MAX_CIRCLE_ERROR = 0.1f;

// How many segments a circle of `radius` is drawn in, by ImGui's own rule with the finer allowance.
// The rule divides by the radius, so a circle of no radius gets the fewest instead. It is a macro
// from ImGui's internal header, which an ImGui upgrade may change.
int CircleSegments(float radius) {
	if (!(radius > 0.0f)) {
		return IM_DRAWLIST_CIRCLE_AUTO_SEGMENT_MIN;
	}
	return IM_DRAWLIST_CIRCLE_AUTO_SEGMENT_CALC(radius, MAX_CIRCLE_ERROR);
}

// The border's width in output pixels, or none when the style draws no border at `opacity`.
float BorderPixels(const PathShapeStyle& style, float opacity) {
	if (!(style.border_width > 0.0f) || style.border.ScaleAlpha(opacity).a == 0) {
		return 0.0f;
	}
	return CurrentScaleContext().Border(style.border_width);
}

// Which way the outside of the edge from `from` to `to` faces, as an angle, for a shape wound
// clockwise on screen, where y grows downwards.
float OutwardAngle(Vec2 from, Vec2 to) {
	return std::atan2(-(to.x - from.x), to.y - from.y);
}

// How many segments an arc of `radius` turning through `sweep` is drawn in, its share of what ImGui
// gives a whole circle of that radius. Left to choose, ImGui draws a short arc of a small radius in
// fewer, and a stroke round so coarse an arc turns sharply at each point, where ImGui stretches its
// smoothed edge past the shape.
// It reads the count from an ImGui helper marked internal, which an ImGui upgrade may change.
int ArcSegments(const ImDrawList& list, float radius, float sweep) {
	const float circle = static_cast<float>(list._CalcCircleAutoSegmentCount(radius));
	return static_cast<int>(std::ceil(circle * sweep / FULL_TURN));
}

// Adds a closed path to `list` around the triangle whose rounded corners are centred on `centres`,
// clockwise, each corner an arc of `radius` from the outward side of the edge before it to that of
// the edge after it, so the path is the corner triangle grown by `radius`.
void TrianglePath(ImDrawList& list, const std::array<Vec2, TRIANGLE_CORNERS>& centres,
                  float radius) {
	for (size_t index = 0; index < TRIANGLE_CORNERS; ++index) {
		const Vec2  before = centres[(index + TRIANGLE_CORNERS - 1) % TRIANGLE_CORNERS];
		const Vec2  corner = centres[index];
		const Vec2  after  = centres[(index + 1) % TRIANGLE_CORNERS];
		const float from   = OutwardAngle(before, corner);
		float       to     = OutwardAngle(corner, after);
		// Clockwise turns increase the angle, so the arc runs forwards the short way round.
		if (to < from) {
			to += FULL_TURN;
		}
		list.PathArcTo(ToImVec2(corner), radius, from, to, ArcSegments(list, radius, to - from));
	}
}

// The centres of the corners of a triangle pointing up that fills `rect`, rounded by `radius`.
// Each sits a radius in from the sides it touches, so the triangle they grow by the radius reaches
// the base, both bottom corners and the top of `rect`.
std::array<Vec2, TRIANGLE_CORNERS> CornerCentres(const Rect& rect, float radius) {
	return {{
	    {rect.x + rect.width / 2.0f, rect.y + radius},
	    {rect.Right() - radius, rect.Bottom() - radius},
	    {rect.x + radius, rect.Bottom() - radius},
	}};
}

// The largest circle inside a triangle `width` wide at its base and `height` tall, which no corner
// can be rounder than.
float InscribedRadius(float width, float height) {
	const float side = std::hypot(width / 2.0f, height);
	return width * height / (width + 2.0f * side);
}

} // namespace

void DrawCircle(ImDrawList& list, const Rect& rect, const PathShapeStyle& style, float opacity) {
	const ImVec2 centre = ToImVec2({rect.x + rect.width / 2.0f, rect.y + rect.height / 2.0f});
	const float  radius = std::min(rect.width, rect.height) / 2.0f;
	const Color  fill   = style.fill.ScaleAlpha(opacity);
	if (fill.a > 0) {
		list.AddCircleFilled(centre, radius, ToImU32(fill), CircleSegments(radius));
	}
	const float width = BorderPixels(style, opacity);
	if (width > 0.0f) {
		// ImGui centres a stroke on its path, so the path sits half the width inside the circle,
		// less the half pixel AddCircle takes off, which puts the stroke's outer edge on its edge.
		// Its segments are counted along that path.
		const float path = radius - width / 2.0f;
		list.AddCircle(centre, path + ADD_CIRCLE_INSET, ToImU32(style.border.ScaleAlpha(opacity)),
		               CircleSegments(path), width);
	}
}

void DrawTriangle(ImDrawList& list, const Rect& rect, const PathShapeStyle& style, float opacity) {
	const float most   = InscribedRadius(rect.width, rect.height);
	const float radius = std::min(CurrentScaleContext().Px(style.radius), most);
	const Color fill   = style.fill.ScaleAlpha(opacity);
	if (fill.a > 0) {
		TrianglePath(list, CornerCentres(rect, radius), radius);
		list.PathFillConvex(ToImU32(fill));
	}
	const float width = BorderPixels(style, opacity);
	if (width > 0.0f) {
		// The stroke is centred on its path, so the path runs half the width inside the edge. Its
		// corners are at least as round as it is thick, as a sharper corner would leave its path
		// a corner that ImGui mitres past the edge. A fill under it keeps its own corners, so with
		// a radius thinner than the stroke its sharper corners show just past the outline.
		const float round = std::min(std::max(radius, width), most);
		TrianglePath(list, CornerCentres(rect, round), std::max(0.0f, round - width / 2.0f));
		list.PathStroke(ToImU32(style.border.ScaleAlpha(opacity)), width, ImDrawFlags_Closed);
	}
}

void DrawRoundStroke(ImDrawList& list, std::span<const Vec2> points, float width_pixels,
                     Color color, float opacity) {
	const Color shown = color.ScaleAlpha(opacity);
	if (points.empty() || shown.a == 0 || !(width_pixels > 0.0f)) {
		return;
	}
	const ImU32 colour = ToImU32(shown);
	const float half   = width_pixels / 2.0f;
	if (points.size() == 1) {
		list.AddCircleFilled(ToImVec2(points[0]), half, colour);
		return;
	}
	for (size_t index = 1; index < points.size(); ++index) {
		const Vec2  from  = points[index - 1];
		const Vec2  to    = points[index];
		const float along = std::atan2(to.y - from.y, to.x - from.x);
		// A half circle round each end, clockwise, makes one rounded segment, and the next
		// segment's round end covers the join.
		list.PathArcTo(ToImVec2(to), half, along - QUARTER_TURN, along + QUARTER_TURN);
		list.PathArcTo(ToImVec2(from), half, along + QUARTER_TURN,
		               along + QUARTER_TURN + HALF_TURN);
		list.PathFillConvex(colour);
	}
}

} // namespace Libs::Graphics::SystemUi
