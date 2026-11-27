#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_CORE_GEOMETRY_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_CORE_GEOMETRY_H_

#include <algorithm>
#include <cmath>

// Plain geometry for the system UI, in output pixels with y growing downward. It has no ImGui
// types, so layout code can be tested without a GPU.
namespace Libs::Graphics::SystemUi {

/// `value` rounded to the nearest whole pixel, with halves rounded up. Unlike std::round, which
/// rounds halves away from zero, this gives a span the same length wherever it sits, including
/// below zero while scrolling. It matches ImGui's own pixel rounding.
[[nodiscard]] inline float SnapPixel(float value) {
	constexpr float HALF = 0.5f;
	return std::floor(value + HALF);
}

/// A point or a size.
struct Vec2 {
	float x = 0.0f;
	float y = 0.0f;

	constexpr bool operator==(const Vec2&) const = default;
};

/// Space kept clear on each side of a rectangle, such as padding.
struct EdgeInsets {
	float left   = 0.0f;
	float top    = 0.0f;
	float right  = 0.0f;
	float bottom = 0.0f;

	/// No inset on any side.
	constexpr EdgeInsets() = default;
	/// The same inset on all four sides. Explicit, so a lone float never becomes insets by
	/// accident.
	explicit constexpr EdgeInsets(float inset): EdgeInsets(inset, inset) {}
	/// `vertical` at the top and bottom and `horizontal` on the left and right, in the order CSS
	/// takes them. Explicit, so the order is always read beside the type.
	explicit constexpr EdgeInsets(float vertical, float horizontal)
	    : EdgeInsets(vertical, horizontal, vertical, horizontal) {}
	/// A separate inset for each side, clockwise from the top as CSS takes them. Explicit, so the
	/// order is always read beside the type.
	explicit constexpr EdgeInsets(float top, float right, float bottom, float left)
	    : left(left), top(top), right(right), bottom(bottom) {}

	constexpr bool operator==(const EdgeInsets&) const = default;
};

namespace GeometryDetail {

/// Whether `insets` holds these values on these sides, given clockwise from the top. Read field by
/// field, so checking a constructor never goes through the constructors being checked.
[[nodiscard]] constexpr bool HasSides(const EdgeInsets& insets, float top, float right,
                                      float bottom, float left) {
	return insets.top == top && insets.right == right && insets.bottom == bottom &&
	       insets.left == left;
}

} // namespace GeometryDetail

// Each constructor puts its insets on the sides it names. Checked here, as a swapped pair would
// otherwise only show as space kept on the wrong sides.
static_assert(GeometryDetail::HasSides(EdgeInsets(), 0.0f, 0.0f, 0.0f, 0.0f));
static_assert(GeometryDetail::HasSides(EdgeInsets(1.0f), 1.0f, 1.0f, 1.0f, 1.0f));
static_assert(GeometryDetail::HasSides(EdgeInsets(1.0f, 2.0f), 1.0f, 2.0f, 1.0f, 2.0f));
static_assert(GeometryDetail::HasSides(EdgeInsets(1.0f, 2.0f, 3.0f, 4.0f), 1.0f, 2.0f, 3.0f, 4.0f));

/// A rectangle from its top-left corner and its size. Layout and drawing place rectangles in output
/// pixels, so every `Rect` the framework hands over is in output pixels.
struct Rect {
	float x      = 0.0f;
	float y      = 0.0f;
	float width  = 0.0f;
	float height = 0.0f;

	/// The x of the right edge.
	[[nodiscard]] constexpr float Right() const { return x + width; }
	/// The y of the bottom edge.
	[[nodiscard]] constexpr float Bottom() const { return y + height; }
	/// The top-left corner.
	[[nodiscard]] constexpr Vec2 Position() const { return {x, y}; }
	/// The width and height together.
	[[nodiscard]] constexpr Vec2 Size() const { return {width, height}; }
	/// The middle point.
	[[nodiscard]] constexpr Vec2 Center() const { return {x + width / 2.0f, y + height / 2.0f}; }

	/// True when `point` lies inside, counting the top and left edges but not the bottom and
	/// right ones, so neighbouring rectangles never both contain a point.
	[[nodiscard]] constexpr bool Contains(Vec2 point) const {
		return point.x >= x && point.y >= y && point.x < Right() && point.y < Bottom();
	}

	/// The rectangle shrunk by `insets`. Where the two insets on an axis add up to more than the
	/// rectangle, that axis is left empty, never negative, at the point where the insets meet.
	/// Equal insets meet at the centre, and the empty rectangle never lands outside this one, so
	/// anything placed against it later stays within its parent.
	[[nodiscard]] constexpr Rect Inset(const EdgeInsets& insets) const {
		return {InsetStart(x, width, insets.left, insets.right),
		        InsetStart(y, height, insets.top, insets.bottom),
		        std::max(0.0f, width - insets.left - insets.right),
		        std::max(0.0f, height - insets.top - insets.bottom)};
	}

	constexpr bool operator==(const Rect&) const = default;

private:
	/// Where a span from `start` of `length` begins once shrunk by `before` and `after`. When they
	/// don't fit, the length is shared in proportion to them, so the empty span sits where they
	/// meet, kept within the span. A share that can't be worked out, from a NaN or infinite inset,
	/// is the middle, as NaN fails both comparisons.
	static constexpr float InsetStart(float start, float length, float before, float after) {
		constexpr float MIDDLE = 0.5f;
		const float     total  = before + after;
		if (total <= length) {
			return start + before;
		}
		const float ratio = before / total;
		const float share = ratio >= 0.0f ? std::min(ratio, 1.0f) : (ratio < 0.0f ? 0.0f : MIDDLE);
		return start + std::max(0.0f, length) * share;
	}
};

/// The largest rectangle inside `rect` that stays clear of its corners rounded by `radius`. Each
/// side moves in by radius * (1 - 1/sqrt(2)), which puts the result's corners on the rounded
/// outline, so clipping to it keeps a drawing inside the rounded shape except for slivers along
/// the straight edges near each corner.
[[nodiscard]] inline Rect InsideCorners(const Rect& rect, float radius) {
	constexpr float CORNER_INSET = 0.29289322f; // 1 - 1/sqrt(2)
	return rect.Inset(EdgeInsets(std::max(0.0f, radius) * CORNER_INSET));
}

} // namespace Libs::Graphics::SystemUi

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_CORE_GEOMETRY_H_
