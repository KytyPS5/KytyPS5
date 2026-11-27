#include "graphics/presentation/system/element/primitives/glyph/glyph.h"

#include "graphics/presentation/system/common/props/itemLayout.h"
#include "graphics/presentation/system/core/context.h"
#include "graphics/presentation/system/core/geometry.h"
#include "graphics/presentation/system/core/units.h"
#include "graphics/presentation/system/element/frame/frameBuilder.h"
#include "graphics/presentation/system/element/frame/framePaint.h"
#include "graphics/presentation/system/imgui/draw/box.h"
#include "graphics/presentation/system/imgui/draw/shape.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <span>

namespace Libs::Graphics::SystemUi {

namespace {

// A glyph is its square whatever room it is given, and can't shrink below it either.
Measured MeasureGlyph(FrameBuilder& frame, AnyElement glyph, Vec2 /*max*/) {
	const GlyphProps* const props = frame.FindAs<GlyphProps>(glyph);
	if (props == nullptr) {
		return {};
	}
	const float side = ToPixels(props->size);
	return {.size = {side, side}, .min_content = {side, side}};
}

// Where `shape` lands in a glyph arranged at `glyph`, at `unit` design units to the grid unit. A
// box's child sits inside its parent's padding with its edges snapped to whole pixels, so each edge
// here is worked out the same way and in the same order, and lands on the same pixel.
Rect PlaceShape(const Rect& glyph, float unit, const GlyphShape& shape) {
	const float left   = glyph.x + ToPixels(Dp(shape.x * unit));
	const float top    = glyph.y + ToPixels(Dp(shape.y * unit));
	const float right  = left + ToPixels(Dp(shape.width * unit));
	const float bottom = top + ToPixels(Dp(shape.height * unit));
	const float x      = SnapPixel(left);
	const float y      = SnapPixel(top);
	return {x, y, SnapPixel(right) - x, SnapPixel(bottom) - y};
}

// How `shape` looks in `color`, at `unit` design units to the grid unit. DrawBox takes design units
// and scales them itself, as it does a box's.
DrawBoxStyle ShapeStyle(const GlyphShape& shape, float unit, Color color) {
	return {.fill         = shape.fill ? color : Color {},
	        .radius       = shape.radius * unit,
	        .border       = shape.border_width > 0.0f ? color : Color {},
	        .border_width = shape.border_width * unit};
}

// Where something `width` pixels across centres on the grid line `point` of a glyph whose grid
// starts at `origin`, at `unit` design units to the grid unit. Its edge is snapped to a whole pixel
// rather than its middle, so anything a whole number of pixels across has both edges on whole
// pixels and stays sharp, and two things whose widths are both odd or both even centre on the same
// point exactly.
float CentreOnGrid(float origin, float point, float unit, float width) {
	const float half = width / 2.0f;
	return SnapPixel(origin + point * unit * CurrentScaleContext().Scale() - half) + half;
}

// How thick a stroke is in pixels, or nothing for a shape that isn't a stroke or has no thickness.
float StrokePixels(const GlyphShape& shape, float unit) {
	if (shape.kind != GlyphShapeKind::Stroke || !(shape.border_width > 0.0f)) {
		return 0.0f;
	}
	return CurrentScaleContext().Border(shape.border_width * unit);
}

// How thick the glyph's thickest stroke is in pixels, or nothing when it has none, which its
// circles and triangles match so its marks centre in them.
float WidestStroke(std::span<const GlyphShape> shapes, float unit) {
	float widest = 0.0f;
	for (const GlyphShape& shape: shapes) {
		widest = std::max(widest, StrokePixels(shape, unit));
	}
	return widest;
}

// `length` design units in whole pixels, a pixel less when it is odd and `stroke` is even, or the
// other way round, so a stroke centres in it exactly. A stroke of nothing asks for neither.
float MatchedPixels(float length, float stroke) {
	constexpr float PAIR   = 2.0f;
	const float     pixels = ToPixels(Dp(length));
	const bool      differ = stroke > 0.0f && std::fmod(std::fabs(pixels - stroke), PAIR) != 0.0f;
	return differ && pixels > 1.0f ? pixels - 1.0f : pixels;
}

// Where a circle or triangle `shape` lands in a glyph arranged at `glyph`. It centres where its
// grid rectangle centres, snapped as strokes are, with sides both odd or both even as `stroke` is,
// so the glyph's marks centre in it however the glyph is scaled or placed.
Rect PlaceCentred(const Rect& glyph, float unit, const GlyphShape& shape, float stroke) {
	const float width    = MatchedPixels(shape.width * unit, stroke);
	const float height   = MatchedPixels(shape.height * unit, stroke);
	const float middle_x = CentreOnGrid(glyph.x, shape.x + shape.width / 2.0f, unit, width);
	const float middle_y = CentreOnGrid(glyph.y, shape.y + shape.height / 2.0f, unit, height);
	return {middle_x - width / 2.0f, middle_y - height / 2.0f, width, height};
}

// Draws `shape`'s stroke in a glyph arranged at `glyph`, as thick as its border, through its first
// points up to the most a stroke draws. A stroke of no thickness draws nothing.
void PaintStroke(ImDrawList& list, const Rect& glyph, float unit, const GlyphShape& shape,
                 Color color, float opacity) {
	const float width = StrokePixels(shape, unit);
	if (!(width > 0.0f)) {
		return;
	}
	const std::span<const GlyphPoint> points = shape.points.Points();
	const size_t                      count  = std::min(points.size(), MAX_GLYPH_STROKE_POINTS);
	std::array<Vec2, MAX_GLYPH_STROKE_POINTS> placed {};
	for (size_t index = 0; index < count; ++index) {
		placed[index] = {CentreOnGrid(glyph.x, points[index].x, unit, width),
		                 CentreOnGrid(glyph.y, points[index].y, unit, width)};
	}
	DrawRoundStroke(list, std::span<const Vec2>(placed).first(count), width, color, opacity);
}

// Draws `shape` in a glyph arranged at `glyph` in `color`, as its kind says. A circle or triangle
// matches the glyph's widest stroke, `stroke` pixels thick.
void PaintShape(ImDrawList& list, const Rect& glyph, float unit, const GlyphShape& shape,
                Color color, float opacity, float stroke) {
	const DrawBoxStyle style = ShapeStyle(shape, unit, color);
	if (shape.kind == GlyphShapeKind::Rect) {
		DrawBox(list, PlaceShape(glyph, unit, shape), style, opacity);
		return;
	}
	if (shape.kind == GlyphShapeKind::Stroke) {
		PaintStroke(list, glyph, unit, shape, color, opacity);
		return;
	}
	const Rect           placed = PlaceCentred(glyph, unit, shape, stroke);
	const PathShapeStyle path {.fill         = style.fill,
	                           .border       = style.border,
	                           .border_width = style.border_width,
	                           .radius       = style.radius};
	if (shape.kind == GlyphShapeKind::Circle) {
		DrawCircle(list, placed, path, opacity);
	} else {
		DrawTriangle(list, placed, path, opacity);
	}
}

// Draws every shape where it lands, in order, each in its ink and faded with the glyph as a box
// would be.
void PaintGlyph(const FrameBuilder& frame, AnyElement glyph, ImDrawList& list,
                const PaintState& state) {
	const GlyphProps* const               props  = frame.FindAs<GlyphProps>(glyph);
	const FrameView::ElementLayout* const layout = frame.LayoutOf(glyph);
	if (props == nullptr || layout == nullptr || props->grid <= 0.0f) {
		return;
	}
	const float unit   = props->size.value / props->grid;
	const float stroke = WidestStroke(props->shapes.Shapes(), unit);
	for (const GlyphShape& shape: props->shapes.Shapes()) {
		const Color color =
		    shape.ink == GlyphInk::Mark ? props->self.styles.mark : props->self.styles.color;
		PaintShape(list, layout->arranged, unit, shape, color, state.opacity, stroke);
	}
}

// How glyphs are measured and painted. A glyph has nothing below it to place, so no arrange hook.
constexpr FrameHooks GLYPH_FRAME_HOOKS {.measure = &MeasureGlyph, .paint = &PaintGlyph};

} // namespace

GlyphElement Glyph(const GlyphProps& props) {
	FrameBuilder& frame = CurrentBuilder();
	// Set on every call, as Box and Image do, so any frame a glyph is built in can lay it out.
	frame.SetFrameHooks<GlyphProps>(GLYPH_FRAME_HOOKS);
	// The caller's layout, with its size forced to the glyph's square and nothing left that could
	// change it, no main-axis limits and no collapsing, as its shapes are drawn for that square
	// alone.
	ItemLayout item = props.self.layout;
	item.width      = props.size;
	item.height     = props.size;
	item.min_main.reset();
	item.max_main.reset();
	item.collapses = false;
	return frame.AddNode(props, {}, item);
}

} // namespace Libs::Graphics::SystemUi
