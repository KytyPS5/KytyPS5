#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_CORE_DP_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_CORE_DP_H_

// Only the length types, without context.h, so the theme can hold its sizes as Dp. Converting them
// to pixels needs the current scale, which is in units.h.

#include <compare>

namespace Libs::Graphics::SystemUi {

/// A length in design units, the theme's reference pixels at 1080p. The element API takes these
/// everywhere, so components never handle output pixels and can't mix the two. Each length is
/// converted on its own and rounded to a whole pixel, as the theme's tokens always have been, so
/// sums of converted lengths match what the components drew before.
struct Dp {
	float value = 0.0f;

	/// No length.
	constexpr Dp() = default;
	/// `length` design units.
	explicit constexpr Dp(float length): value(length) {}

	constexpr auto operator<=>(const Dp&) const = default;

	/// Lengths add and scale as plain numbers do, staying in design units.
	constexpr Dp& operator+=(Dp other) {
		value += other.value;
		return *this;
	}
	constexpr Dp& operator-=(Dp other) {
		value -= other.value;
		return *this;
	}
	friend constexpr Dp operator+(Dp a, Dp b) { return Dp(a.value + b.value); }
	friend constexpr Dp operator-(Dp a, Dp b) { return Dp(a.value - b.value); }
	friend constexpr Dp operator-(Dp length) { return Dp(-length.value); }
	friend constexpr Dp operator*(Dp length, float factor) { return Dp(length.value * factor); }
	friend constexpr Dp operator*(float factor, Dp length) { return Dp(length.value * factor); }
	friend constexpr Dp operator/(Dp length, float divisor) { return Dp(length.value / divisor); }
};

/// Insets on each side, in design units, such as a box's padding.
struct DpInsets {
	Dp left;
	Dp top;
	Dp right;
	Dp bottom;

	/// No inset on any side.
	constexpr DpInsets() = default;
	/// The same inset on all four sides. Explicit, so a lone Dp never becomes insets by accident.
	explicit constexpr DpInsets(Dp inset): DpInsets(inset, inset) {}
	/// `vertical` at the top and bottom and `horizontal` on the left and right, in the order CSS
	/// takes them. Explicit, so the order is always read beside the type.
	explicit constexpr DpInsets(Dp vertical, Dp horizontal)
	    : DpInsets(vertical, horizontal, vertical, horizontal) {}
	/// A separate inset for each side, clockwise from the top as CSS takes them. Explicit, so the
	/// order is always read beside the type.
	explicit constexpr DpInsets(Dp top, Dp right, Dp bottom, Dp left)
	    : left(left), top(top), right(right), bottom(bottom) {}

	constexpr bool operator==(const DpInsets&) const = default;
};

namespace DpDetail {

/// Whether `insets` holds these lengths on these sides, given clockwise from the top. Read field by
/// field, so checking a constructor never goes through the constructors being checked.
[[nodiscard]] constexpr bool HasSides(const DpInsets& insets, float top, float right, float bottom,
                                      float left) {
	return insets.top == Dp(top) && insets.right == Dp(right) && insets.bottom == Dp(bottom) &&
	       insets.left == Dp(left);
}

} // namespace DpDetail

// Each constructor puts its insets on the sides it names. Checked here, as a swapped pair would
// otherwise only show as padding on the wrong sides.
static_assert(DpDetail::HasSides(DpInsets(), 0.0f, 0.0f, 0.0f, 0.0f));
static_assert(DpDetail::HasSides(DpInsets(Dp(1.0f)), 1.0f, 1.0f, 1.0f, 1.0f));
static_assert(DpDetail::HasSides(DpInsets(Dp(1.0f), Dp(2.0f)), 1.0f, 2.0f, 1.0f, 2.0f));
static_assert(DpDetail::HasSides(DpInsets(Dp(1.0f), Dp(2.0f), Dp(3.0f), Dp(4.0f)), 1.0f, 2.0f, 3.0f,
                                 4.0f));

} // namespace Libs::Graphics::SystemUi

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_CORE_DP_H_
