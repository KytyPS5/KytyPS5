#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_SPLASHANIMATION_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_SPLASHANIMATION_H_

#include <algorithm>
#include <array>
#include <cmath>

namespace Libs::Graphics {

// One layer of the startup image: the UV span it shows about the center, the same across and
// down, and its alpha. A span above 1 repeats the edge texels.
struct SplashLayer {
	float span;
	float alpha;
};

// Growth of the startup image `age` seconds after it appears, a steep ease-out from 0 to 1 over
// 1.4 s.
constexpr float GrowSplash(float age) {
	const float rest = 1.0f - std::clamp(age / 1.4f, 0.0f, 1.0f);
	const float sq   = rest * rest;
	return 1.0f - sq * (0.2f + sq * sq * (0.65f + 0.15f * sq));
}

// The background and logo layers `age` seconds after the startup image appears. The background
// grows from 95% to full size, then zooms in by 10% over 12 s while the logo fades in over 2.2 s.
// With `logo_first` (param attribute3 bit 12) the logo shows at once and grows with the background.
constexpr std::array<SplashLayer, 2> AnimateSplash(float age, bool logo_first) {
	const auto ease = [](float x) {
		x = std::clamp(x, 0.0f, 1.0f);
		return x < 0.5f ? 2.0f * x * x : 1.0f - 2.0f * (1.0f - x) * (1.0f - x);
	};
	const float span = (1.0f - 0.05f * GrowSplash(age)) / 0.95f;
	const float zoom = 1.0f + 0.1f * ease((age - 1.4f) / 12.0f);
	return {{{span / zoom, 1.0f}, {span, logo_first ? 1.0f : ease((age - 1.4f) / 2.2f)}}};
}

// How much of the startup image shows at its point (x, y), in 0..1 of its size, `age` seconds
// after it appears: a soft circle about the center that widens until the whole image shows.
inline float RevealSplash(float age, float x, float y) {
	const auto smooth = [](float t) {
		t = std::clamp(t, 0.0f, 1.0f);
		return t * t * (3.0f - 2.0f * t);
	};
	const float dx = x - 0.5f;
	const float dy = (y - 0.5f) * (9.0f / 16.0f);
	const float edge =
	    1.0f + 0.5f * (std::sqrt(2.0f * (dx * dx + dy * dy)) - 3.0f * GrowSplash(age));
	return smooth(1.05f * (1.0f - smooth(edge)));
}

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_SPLASHANIMATION_H_
