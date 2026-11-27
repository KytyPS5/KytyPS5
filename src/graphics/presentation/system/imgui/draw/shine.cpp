#include "graphics/presentation/system/imgui/draw/shine.h"

#include "graphics/presentation/system/core/context.h"
#include "graphics/presentation/system/imgui/convert.h"
#include "graphics/presentation/system/imgui/draw/box.h"
#include "imgui.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <numbers>

namespace Libs::Graphics::SystemUi {

namespace {

// Each side of the band fades over this many strips. Brightness is straight across a strip, so the
// smooth fade is followed closely, and the band never shows the line a single straight fade does.
constexpr int RAMP_SLABS = 4;
// The edges between strips, from the band's left edge to its right, the plateau one strip between
// the two fades.
constexpr int BAND_EDGES = 2 * RAMP_SLABS + 2;
// The most points the rounded outline may have. ImGui cuts each corner into at most a quarter of
// its finest circle, so a rounder shape than this allows is left without a shine rather than read
// past the end.
constexpr int MAX_SHINE_POINTS = 256;
// A strip is the outline clipped against two straight lines, and each clip adds at most one point.
constexpr int CLIPS_PER_STRIP = 2;
constexpr int MAX_CLIPPED     = MAX_SHINE_POINTS + CLIPS_PER_STRIP;
// One edge of the shape adds at most its own start and a crossing to a clipped shape.
constexpr int POINTS_PER_EDGE = 2;
// The corners of a rectangle, which bound how far along the band's direction it reaches.
constexpr size_t RECT_CORNERS = 4;
// Each strip is drawn in this, then recoloured vertex by vertex, as gradients are.
constexpr ImU32 SHAPE_COLOR   = IM_COL32_WHITE;
constexpr ImU32 ALPHA_MASK    = 0xFFu;
constexpr float CHANNEL_SCALE = 255.0f;
constexpr float HALF_TURN     = 180.0f;
// Points closer than this in pixels are one point, far under what shows.
constexpr float SAME_POINT = 1.0e-3f;

// A convex outline as points in clockwise order, which ImGui's smoothed fill needs.
struct Outline {
	std::array<ImVec2, MAX_CLIPPED> points;
	int                             count = 0;
};

// Where along the band's direction `point` lies, as a distance from the origin.
float Along(ImVec2 point, ImVec2 normal) {
	return point.x * normal.x + point.y * normal.y;
}

// The outline ImGui fills `rect` with when its corners are rounded by `rounding` pixels, the same
// points as the box's own fill, so the shine stops exactly at the box's edge. Empty when the shape
// has more points than the outline holds.
Outline ShapeOutline(ImDrawList& list, const Rect& rect, float rounding) {
	// The list's path is borrowed and cleared, so one the caller had open would be lost.
	assert(list._Path.Size == 0 && "a shine is drawn with no path open");
	const auto [start, end] = ToImRect(rect);
	list.PathRect(start, end, rounding);
	Outline   outline;
	const int count = list._Path.Size;
	assert(count <= MAX_SHINE_POINTS && "a shine's outline has more points than it holds");
	if (count <= MAX_SHINE_POINTS) {
		std::copy(list._Path.begin(), list._Path.end(), outline.points.begin());
		outline.count = count;
	}
	list.PathClear();
	return outline;
}

// Whether `a` and `b` are the same point to well under a pixel.
bool SamePoint(ImVec2 a, ImVec2 b) {
	return std::abs(a.x - b.x) < SAME_POINT && std::abs(a.y - b.y) < SAME_POINT;
}

// The part of convex `shape` on the side of the line `Along == limit` that `keep_above` names,
// kept convex and clockwise. Each edge that crosses the line is cut where it crosses, which is the
// Sutherland-Hodgman clip against one straight edge. A point that repeats the one before it is
// left out, as happens when the line passes through a point of the shape or lies along one of
// its sides. ImGui's smoothed fill finds each edge's direction to place its soft edge, and an
// edge of no length has none, which throws vertices far outside the shape.
Outline ClipToSide(const Outline& shape, ImVec2 normal, float limit, bool keep_above) {
	Outline    clipped;
	const int  count  = shape.count;
	const auto inside = [&](ImVec2 point) {
		const float side = Along(point, normal) - limit;
		return keep_above ? side >= 0.0f : side <= 0.0f;
	};
	const auto keep = [&clipped](ImVec2 point) {
		if (clipped.count == 0 || !SamePoint(point, clipped.points[clipped.count - 1])) {
			clipped.points[clipped.count++] = point;
		}
	};
	for (int index = 0; index < count && clipped.count + POINTS_PER_EDGE <= MAX_CLIPPED; ++index) {
		const ImVec2 from = shape.points[index];
		const ImVec2 to   = shape.points[(index + 1) % count];
		if (inside(from)) {
			keep(from);
		}
		if (inside(from) != inside(to)) {
			// The crossing is the share of the edge where Along reaches the limit.
			const float t =
			    (limit - Along(from, normal)) / (Along(to, normal) - Along(from, normal));
			keep({from.x + (to.x - from.x) * t, from.y + (to.y - from.y) * t});
		}
	}
	// The last point can repeat the first, closing the shape on itself.
	if (clipped.count > 1 && SamePoint(clipped.points[clipped.count - 1], clipped.points[0])) {
		--clipped.count;
	}
	return clipped;
}

// How bright the band is a fraction `t` of the way through a fade, a smoothstep, so the fade meets
// the surface flat rather than at an angle the eye picks out as a line.
float Smooth(float t) {
	return t * t * (3.0f - 2.0f * t);
}

// The band's strip edges, where each lies along its direction and how bright it is there, from
// its left edge, dark, up a smooth fade to the plateau, across it, and down the matching fade.
struct BandEdges {
	std::array<float, BAND_EDGES> along;
	std::array<float, BAND_EDGES> brightness;
};

BandEdges MakeBandEdges(float start, float width, float plateau_fraction) {
	const float plateau = std::clamp(plateau_fraction, 0.0f, 1.0f) * width;
	const float ramp    = (width - plateau) / 2.0f;
	BandEdges   edges;
	for (int step = 0; step <= RAMP_SLABS; ++step) {
		const float t                           = static_cast<float>(step) / RAMP_SLABS;
		edges.along[step]                       = start + ramp * t;
		edges.brightness[step]                  = Smooth(t);
		edges.along[BAND_EDGES - 1 - step]      = start + width - ramp * t;
		edges.brightness[BAND_EDGES - 1 - step] = Smooth(t);
	}
	return edges;
}

// Draws the part of `shape` between `from` and `to` along `normal` in `color`, its brightness
// running straight from `from_bright` to `to_bright` across it and scaled by `peak`.
void DrawStrip(ImDrawList& list, const Outline& shape, ImVec2 normal, float from, float to,
               float from_bright, float to_bright, Color color, float peak) {
	if (!(to > from)) {
		return;
	}
	const Outline strip = ClipToSide(ClipToSide(shape, normal, from, true), normal, to, false);
	if (strip.count < 3) {
		return;
	}
	const int first = list.VtxBuffer.Size;
	list.AddConvexPolyFilled(strip.points.data(), strip.count, SHAPE_COLOR);
	for (int index = first; index < list.VtxBuffer.Size; ++index) {
		ImDrawVert& vertex = list.VtxBuffer[index];
		// The smoothed edge reaches a little past the strip, so the share is held to the strip.
		const float t = std::clamp((Along(vertex.pos, normal) - from) / (to - from), 0.0f, 1.0f);
		const float light    = from_bright + (to_bright - from_bright) * t;
		const ImU32 alpha    = (vertex.col >> IM_COL32_A_SHIFT) & ALPHA_MASK;
		const float coverage = static_cast<float>(alpha) / CHANNEL_SCALE;
		vertex.col           = ToImU32(color.ScaleAlpha(light * peak * coverage));
	}
}

} // namespace

void DrawShine(ImDrawList& list, const Rect& rect, float radius, const ShineStyle& style,
               float position, float opacity) {
	const float peak = ClampUnit(style.peak_alpha) * ClampUnit(opacity);
	// A band at 0 or 1 or past them is wholly off the element, so the outline isn't worked out.
	// Written so a NaN position fails too.
	if (!(position > 0.0f && position < 1.0f) || !(peak > 0.0f) || style.color.a == 0) {
		return;
	}
	// A rectangle that isn't a number would put NaN points in the outline, which a clip keeps, so
	// nothing is drawn for one, or for an empty one.
	const bool finite = std::isfinite(rect.x) && std::isfinite(rect.y) &&
	                    std::isfinite(rect.width) && std::isfinite(rect.height);
	if (!finite || !(rect.width > 0.0f) || !(rect.height > 0.0f)) {
		return;
	}
	const float width =
	    std::max(style.width_fraction * rect.width, CurrentScaleContext().Px(style.min_width));
	if (!(width > 0.0f) || !std::isfinite(width)) {
		return;
	}
	// The band's lines run across this direction. It points right and a little down, so a line
	// across it leans right at the top by the style's angle.
	const float  angle  = style.angle_degrees * std::numbers::pi_v<float> / HALF_TURN;
	const ImVec2 normal = {std::cos(angle), std::sin(angle)};
	// The band travels from just off the element's first corner along its direction to just off
	// its last, so position 0 and 1 both leave the element unlit.
	const std::array<float, RECT_CORNERS> corners {
	    Along({rect.x, rect.y}, normal), Along({rect.Right(), rect.y}, normal),
	    Along({rect.x, rect.Bottom()}, normal), Along({rect.Right(), rect.Bottom()}, normal)};
	const float     first    = *std::min_element(corners.begin(), corners.end());
	const float     last     = *std::max_element(corners.begin(), corners.end());
	const float     start    = first - width + (last - first + width) * position;
	const float     rounding = BoxRounding(rect, radius);
	const Outline   shape    = ShapeOutline(list, rect, std::isfinite(rounding) ? rounding : 0.0f);
	const BandEdges edges    = MakeBandEdges(start, width, style.plateau_fraction);
	for (int edge = 0; edge + 1 < BAND_EDGES; ++edge) {
		DrawStrip(list, shape, normal, edges.along[edge], edges.along[edge + 1],
		          edges.brightness[edge], edges.brightness[edge + 1], style.color, peak);
	}
}

} // namespace Libs::Graphics::SystemUi
