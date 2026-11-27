#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_ELEMENT_PRIMITIVES_GLYPH_GLYPH_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_ELEMENT_PRIMITIVES_GLYPH_GLYPH_H_

#include "graphics/presentation/system/common/props/itemLayout.h"
#include "graphics/presentation/system/core/color.h"
#include "graphics/presentation/system/core/dp.h"
#include "graphics/presentation/system/element/element.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace Libs::Graphics::SystemUi {

struct GlyphProps;

/// A glyph's element, what Glyph returns.
using GlyphElement = Element<GlyphProps>;

/// Which of a glyph's two colours a shape is drawn in, the glyph's own colour or its mark colour,
/// which draws a mark over a filled shape where a hole would show what's behind.
enum class GlyphInk : uint8_t { Color, Mark };

/// What a glyph shape draws. A rectangle may be rounded, but ImGui keeps a pixel of flat side on a
/// fully rounded one, so a circle is its own kind. A triangle points up, its corners rounded. A
/// stroke is a line through its points with round ends and joins, a dot when it has one point.
enum class GlyphShapeKind : uint8_t { Rect, Circle, Triangle, Stroke };

/// A point on a glyph's grid, in grid units from its top left corner.
struct GlyphPoint {
	float x = 0.0f;
	float y = 0.0f;
};

/// How many points a stroke draws at most. The rest are left out.
inline constexpr std::size_t MAX_GLYPH_STROKE_POINTS = 8;

/// The points of a stroke, made only from an array with static storage, as GlyphShapes are, since
/// the frame keeps the shape but not what it points at. None by default.
class GlyphPoints {
public:
	constexpr GlyphPoints() = default;

	/// The points in `points`, which must outlive every frame that draws them. More than a stroke
	/// draws doesn't compile, so no table loses points without anyone knowing.
	template <std::size_t N>
	consteval GlyphPoints(const std::array<GlyphPoint, N>& points): m_points(points) { // NOLINT
		static_assert(N <= MAX_GLYPH_STROKE_POINTS, "more points than a stroke draws");
	}

	/// The points, in the order the stroke passes through them.
	[[nodiscard]] constexpr std::span<const GlyphPoint> Points() const noexcept { return m_points; }

private:
	std::span<const GlyphPoint> m_points;
};

/// One shape of a glyph on the glyph's grid, filled or outlined in one of the glyph's colours, or
/// both. Lengths are grid units, so a glyph scales as a whole with its size. A rectangle, circle or
/// triangle fills the rectangle at x, y, width and height. A stroke ignores it and runs through its
/// points instead, as thick as its border width, and draws nothing at a border width of none.
struct GlyphShape {
	float x      = 0.0f;
	float y      = 0.0f;
	float width  = 0.0f;
	float height = 0.0f;
	// Whether the inside is filled.
	bool fill = false;
	// How round every corner is.
	float radius = 0.0f;
	// How thick the outline is, drawn inside the shape. None at zero. A stroke's thickness.
	float          border_width = 0.0f;
	GlyphInk       ink          = GlyphInk::Color;
	GlyphShapeKind kind         = GlyphShapeKind::Rect;
	GlyphPoints    points;
};

/// The shapes of a glyph, drawn in order, so later shapes go over earlier ones. The frame keeps a
/// glyph's props until it is drawn but not the shapes they point at, so these can only be made from
/// an array with static storage, such as a constexpr table at namespace scope. Making them from an
/// array on the stack doesn't compile.
class GlyphShapes {
public:
	/// The shapes in `shapes`, which must outlive every frame that draws them.
	template <std::size_t N>
	consteval GlyphShapes(const std::array<GlyphShape, N>& shapes): m_shapes(shapes) {} // NOLINT

	/// The shapes, in drawing order.
	[[nodiscard]] constexpr std::span<const GlyphShape> Shapes() const noexcept { return m_shapes; }

private:
	std::span<const GlyphShape> m_shapes;
};

/// How a glyph looks, the colours its shapes are drawn in.
struct GlyphStyles {
	// The colour of shapes drawn in colour ink.
	Color color;
	// The colour of shapes drawn in mark ink.
	Color mark;
};

/// A small drawing, such as an icon, made of shapes on a square grid and drawn as one element. It
/// has no children or state of its own, so it costs one element however many shapes it has.
/// A rectangle lands on whole pixels exactly where a box's child would, so rectangles draw the same
/// pixels as the same shapes laid out as boxes, for shapes that start inside the grid. A box keeps
/// a child it pads past its own edge inside itself, which a glyph doesn't. A stroke's edges land on
/// whole pixels too, so thin marks stay sharp. A circle or triangle centres where its grid
/// rectangle centres, snapped as strokes are, and is a pixel smaller when a side is odd and the
/// glyph's thickest stroke is even, or the other way round, so a mark centres in it exactly. It has
/// no default, as its shapes must always name a static table.
struct GlyphProps {
	GlyphShapes shapes;
	// How many grid units the glyph is on a side.
	float grid = 1.0f;
	/// What the glyph asks of its container and how it looks. Its width and height are always its
	/// size, and no main-axis limit or collapsing changes it, whatever the layout says.
	struct Self {
		ItemLayout  layout;
		GlyphStyles styles;
	};

	// The glyph's square side, which also sets how large a grid unit is.
	Dp   size;
	Self self;
};

/// A glyph as `props` say, built in the current frame. It is always its square, whatever room its
/// container gives it, and it fades with its parent. It needs an open FrameScope. A refused glyph
/// is a none handle, and a RefusalScope open around the call says why.
[[nodiscard]] GlyphElement Glyph(const GlyphProps& props);

} // namespace Libs::Graphics::SystemUi

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_ELEMENT_PRIMITIVES_GLYPH_GLYPH_H_
