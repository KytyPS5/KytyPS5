#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_SPLASHANIMATION_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_SPLASHANIMATION_H_

#include <algorithm>

namespace Libs::Graphics {

// One frame of the startup image. Each UV applies to both axes; past 1 it repeats the right and
// bottom edge texels.
struct SplashFrame {
	float background_uv0;
	float background_uv1;
	float logo_uv1;
	float background_alpha;
	float logo_alpha;
};

// `age` seconds after the startup image appears, it fades in over 0.4 s while the background grows
// from 96.5% to full size about the top left over 1.4 s. The logo then fades in over 2.2 s and the
// background zooms in by 10% about the center over 12 s. With `logo_first` (param attribute3
// bit 12) the logo grows in with the background instead.
constexpr SplashFrame AnimateSplash(float age, bool logo_first) {
	const auto ease = [](float x) {
		x = std::clamp(x, 0.0f, 1.0f);
		return x < 0.5f ? 2.0f * x * x : 1.0f - 2.0f * (1.0f - x) * (1.0f - x);
	};
	const float rest  = std::max(1.0f - age / 1.4f, 0.0f);
	const float grow  = 1.0f / (1.0f - 0.035f * rest * rest * rest * rest);
	const float zoom  = 1.0f + 0.1f * ease((age - 1.4f) / 12.0f);
	const float shown = ease(age / 0.4f);
	return {0.5f - 0.5f / zoom, 0.5f + (grow - 0.5f) / zoom, grow, shown,
	        logo_first ? shown : ease((age - 1.4f) / 2.2f)};
}

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_SPLASHANIMATION_H_
