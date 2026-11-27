#include "graphics/presentation/system/components/glyphs/status.h"

#include "graphics/presentation/system/core.h"

#include <array>

namespace Libs::Graphics::SystemUi {

namespace {

// The glyphs share a grid of 20 units, so a unit is 0.8 pixels at 16 and 1.6 at 32, and quarter
// units land on fifths of a pixel. The outline is 1.25 units, a whole pixel at 16 and two at 32.
// The marks are 2 units thick, a little heavier than the outline so they read at 16.
constexpr float GRID          = 20.0f;
constexpr float OUTLINE       = 1.25f;
constexpr float MARK          = 2.0f;
constexpr float MIDDLE        = GRID / 2.0f;
constexpr float CIRCLE_INSET  = 1.0f;
constexpr float CIRCLE_SIDE   = GRID - 2.0f * CIRCLE_INSET;
constexpr float TRIANGLE_TOP  = 1.0f;
constexpr float TRIANGLE_SIDE = GRID - 2.0f * TRIANGLE_TOP;
// The triangle's corners, rounded so its tip isn't sharp at small sizes.
constexpr float TRIANGLE_CORNER = 2.0f;

// The marks' centrelines. Each bar and dot is a round stroke, as ImGui keeps a pixel of flat side
// on a fully rounded box, which leaves a two pixel dot square. The circles' marks centre on the
// grid's middle, and the warning's sits lower, at the triangle's centre of mass.
constexpr std::array<GlyphPoint, 1> INFO_DOT {{{MIDDLE, 6.25f}}};
constexpr std::array<GlyphPoint, 2> INFO_BAR {{{MIDDLE, 9.75f}, {MIDDLE, 13.75f}}};
constexpr std::array<GlyphPoint, 2> ERROR_BAR {{{MIDDLE, 6.0f}, {MIDDLE, 10.0f}}};
constexpr std::array<GlyphPoint, 1> ERROR_DOT {{{MIDDLE, 14.0f}}};
constexpr std::array<GlyphPoint, 2> WARNING_BAR {{{MIDDLE, 9.0f}, {MIDDLE, 11.5f}}};
constexpr std::array<GlyphPoint, 1> WARNING_DOT {{{MIDDLE, 15.25f}}};
constexpr std::array<GlyphPoint, 3> CHECK {{{5.75f, 10.0f}, {8.75f, 13.0f}, {14.25f, 7.0f}}};

// A circle or triangle body, outlined or filled.
constexpr GlyphShape Body(GlyphShapeKind kind, bool filled) {
	const bool  circle = kind == GlyphShapeKind::Circle;
	const float inset  = circle ? CIRCLE_INSET : 0.0f;
	return {.x            = inset,
	        .y            = circle ? CIRCLE_INSET : TRIANGLE_TOP,
	        .width        = GRID - 2.0f * inset,
	        .height       = circle ? CIRCLE_SIDE : TRIANGLE_SIDE,
	        .fill         = filled,
	        .radius       = circle ? 0.0f : TRIANGLE_CORNER,
	        .border_width = filled ? 0.0f : OUTLINE,
	        .kind         = kind};
}

// A part of a mark through `points`, in the glyph's colour on an outline and in its mark colour on
// a filled body.
constexpr GlyphShape Mark(GlyphPoints points, bool filled) {
	return {.border_width = MARK,
	        .ink          = filled ? GlyphInk::Mark : GlyphInk::Color,
	        .kind         = GlyphShapeKind::Stroke,
	        .points       = points};
}

// The body first, so the mark is drawn over it.
constexpr std::array<GlyphShape, 3> INFO_OUTLINE {
    {Body(GlyphShapeKind::Circle, false), Mark(INFO_DOT, false), Mark(INFO_BAR, false)}};
constexpr std::array<GlyphShape, 3> INFO_FILLED {
    {Body(GlyphShapeKind::Circle, true), Mark(INFO_DOT, true), Mark(INFO_BAR, true)}};
constexpr std::array<GlyphShape, 3> WARNING_OUTLINE {
    {Body(GlyphShapeKind::Triangle, false), Mark(WARNING_BAR, false), Mark(WARNING_DOT, false)}};
constexpr std::array<GlyphShape, 3> WARNING_FILLED {
    {Body(GlyphShapeKind::Triangle, true), Mark(WARNING_BAR, true), Mark(WARNING_DOT, true)}};
constexpr std::array<GlyphShape, 3> ERROR_OUTLINE {
    {Body(GlyphShapeKind::Circle, false), Mark(ERROR_BAR, false), Mark(ERROR_DOT, false)}};
constexpr std::array<GlyphShape, 3> ERROR_FILLED {
    {Body(GlyphShapeKind::Circle, true), Mark(ERROR_BAR, true), Mark(ERROR_DOT, true)}};
constexpr std::array<GlyphShape, 2> SUCCESS_OUTLINE {
    {Body(GlyphShapeKind::Circle, false), Mark(CHECK, false)}};
constexpr std::array<GlyphShape, 2> SUCCESS_FILLED {
    {Body(GlyphShapeKind::Circle, true), Mark(CHECK, true)}};

// The outlined and filled shapes of `status`'s glyph.
struct StatusShapes {
	GlyphShapes outline;
	GlyphShapes filled;
};

// The shapes of the glyph for `status`.
StatusShapes ShapesOf(Status status) {
	switch (status) {
		case Status::Warning: return {WARNING_OUTLINE, WARNING_FILLED};
		case Status::Error: return {ERROR_OUTLINE, ERROR_FILLED};
		case Status::Success: return {SUCCESS_OUTLINE, SUCCESS_FILLED};
		case Status::Info: break;
	}
	return {INFO_OUTLINE, INFO_FILLED};
}

} // namespace

GlyphElement StatusGlyph(const StatusGlyphProps& props) {
	const ThemeContext& theme  = CurrentThemeContext();
	const StatusShapes  shapes = ShapesOf(props.status);
	// The filled shapes when filled and the outline otherwise, in the status colour unless the
	// caller gives one, with the mark in the theme's colour for content on a coloured fill.
	return Glyph({.shapes = props.fill ? shapes.filled : shapes.outline,
	              .grid   = GRID,
	              .size   = props.size,
	              .self   = {.layout = props.self.layout,
	                         .styles = {.color = props.self.styles.color.value_or(
                                          StatusColor(theme, props.status)),
	                                    .mark = theme.colors.text.on_accent}}});
}

} // namespace Libs::Graphics::SystemUi
