#include "graphics/presentation/system/core/gradient.h"

#include <cmath>
#include <numbers>

namespace Libs::Graphics::SystemUi {

namespace {

constexpr float DEGREES_PER_HALF_TURN = 180.0f;

} // namespace

GradientLine MakeGradientLine(const Rect& rect, float angle_degrees) {
	const float  radians = angle_degrees * std::numbers::pi_v<float> / DEGREES_PER_HALF_TURN;
	GradientLine line;
	// The direction the gradient runs in, with y growing downward, so 0 points up and 90 right.
	line.direction_x = std::sin(radians);
	line.direction_y = -std::cos(radians);
	line.centre_x    = rect.x + rect.width / 2.0f;
	line.centre_y    = rect.y + rect.height / 2.0f;
	// CSS's gradient length, the rect's extent along that direction, which makes the line end at
	// the corners furthest along it either way. Zero, negative or NaN leaves the inverse at zero.
	const float length =
	    std::abs(rect.width * line.direction_x) + std::abs(rect.height * line.direction_y);
	if (length > 0.0f) {
		line.inverse_length = 1.0f / length;
	}
	return line;
}

float GradientPosition(const Rect& rect, float angle_degrees, Vec2 point) {
	return MakeGradientLine(rect, angle_degrees).Position(point);
}

} // namespace Libs::Graphics::SystemUi
