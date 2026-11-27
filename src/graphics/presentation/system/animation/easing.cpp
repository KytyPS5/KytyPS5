#include "graphics/presentation/system/animation/easing.h"

namespace Libs::Graphics::SystemUi {

namespace {

constexpr float HALF = 0.5f;

// Cubic ease in and out. Slow at both ends and symmetric about the middle.
float InOutCubic(float t) {
	constexpr float IN_SCALE = 4.0f;
	if (t < HALF) {
		return IN_SCALE * t * t * t;
	}
	const float from_end = 2.0f - 2.0f * t;
	return 1.0f - from_end * from_end * from_end * HALF;
}

// Ease out with a small overshoot past the end before settling, the standard "back" curve.
float OutBack(float t) {
	constexpr float OVERSHOOT = 1.70158f;
	const float     from_end  = t - 1.0f;
	return 1.0f + (OVERSHOOT + 1.0f) * from_end * from_end * from_end +
	       OVERSHOOT * from_end * from_end;
}

} // namespace

float ApplyEase(Ease ease, float t) {
	// Written so NaN fails the first test and lands on 0.
	const float progress = t > 0.0f ? (t < 1.0f ? t : 1.0f) : 0.0f;
	switch (ease) {
		case Ease::Enter: {
			const float remaining = 1.0f - progress;
			return 1.0f - remaining * remaining * remaining;
		}
		case Ease::Exit: return progress * progress * progress;
		case Ease::Standard: return InOutCubic(progress);
		case Ease::Emphasis: return OutBack(progress);
		case Ease::Linear: break;
	}
	return progress;
}

} // namespace Libs::Graphics::SystemUi
