#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_CORE_GRADIENT_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_CORE_GRADIENT_H_

#include "graphics/presentation/system/core/color.h"
#include "graphics/presentation/system/core/geometry.h"

namespace Libs::Graphics::SystemUi {

/// The angle a gradient runs at by default, top to bottom.
inline constexpr float GRADIENT_TOP_TO_BOTTOM = 180.0f;

/// A colour that changes evenly from `from` to `to` across a box, as CSS's linear-gradient does.
/// The angle follows CSS too, so the gradient reads as anyone who knows CSS expects. At 0 it runs
/// from the bottom up, and it turns clockwise, so 90 runs to the right and 180 from top to bottom.
struct LinearGradient {
	Color from;
	Color to;
	float angle_degrees = GRADIENT_TOP_TO_BOTTOM;

	/// The same gradient with both colours' alpha scaled by `factor`, as Color::ScaleAlpha does.
	[[nodiscard]] constexpr LinearGradient ScaleAlpha(float factor) const {
		return {from.ScaleAlpha(factor), to.ScaleAlpha(factor), angle_degrees};
	}

	/// Whether any of it can be seen, so a gradient clear at both ends draws nothing.
	[[nodiscard]] constexpr bool Visible() const { return from.a > 0 || to.a > 0; }

	constexpr bool operator==(const LinearGradient&) const = default;
};

/// A gradient's line across one rect, worked out once, so placing each of a shape's many points
/// along it costs a few multiplies. As in CSS, the line passes through the rect's centre and is
/// just long enough that the corners it starts and ends at get exactly the end colours.
struct GradientLine {
	float centre_x    = 0.0f;
	float centre_y    = 0.0f;
	float direction_x = 0.0f;
	float direction_y = 0.0f;
	// One over the line's length, or zero for an empty rect, which puts every point at the middle.
	float inverse_length = 0.0f;

	/// How far along the line `point` lies, 0 at its start and 1 at its end, held to that range.
	[[nodiscard]] float Position(Vec2 point) const {
		constexpr float MIDDLE = 0.5f;
		const float along = (point.x - centre_x) * direction_x + (point.y - centre_y) * direction_y;
		return ClampUnit(along * inverse_length + MIDDLE);
	}
};

/// The line of a gradient at `angle_degrees` across `rect`.
[[nodiscard]] GradientLine MakeGradientLine(const Rect& rect, float angle_degrees);

/// How far along a gradient at `angle_degrees` across `rect` the point `point` lies, 0 at its start
/// and 1 at its end, held to that range, for a single point.
[[nodiscard]] float GradientPosition(const Rect& rect, float angle_degrees, Vec2 point);

} // namespace Libs::Graphics::SystemUi

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_CORE_GRADIENT_H_
