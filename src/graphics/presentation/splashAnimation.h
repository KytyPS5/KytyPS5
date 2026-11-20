#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_SPLASHANIMATION_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_SPLASHANIMATION_H_

#include <algorithm>
#include <array>

namespace Libs::Graphics {

// One layer of the startup image: its UV rectangle and alpha. UVs outside 0..1 repeat the edge
// texels.
struct SplashLayer {
	float u0;
	float v0;
	float u1;
	float v1;
	float alpha;
};

// The background and logo layers `age` seconds after the startup image appears. The background
// fades in over 0.4 s while it grows from 96.5% to full size over 1.4 s about (0.13, 0.2) of the
// image, where the home screen shows the selected title. The logo then fades in over 2.2 s and the
// background zooms in by 10% about the center over 12 s. With `logo_first` (param attribute3
// bit 12) the logo fades and grows in with the background instead.
constexpr std::array<SplashLayer, 2> AnimateSplash(float age, bool logo_first) {
	const auto ease = [](float x) {
		x = std::clamp(x, 0.0f, 1.0f);
		return x < 0.5f ? 2.0f * x * x : 1.0f - 2.0f * (1.0f - x) * (1.0f - x);
	};
	const float rest   = std::max(1.0f - age / 1.4f, 0.0f);
	const float grow   = 1.0f / (1.0f - 0.035f * rest * rest * rest * rest);
	const float zoom   = 1.0f + 0.1f * ease((age - 1.4f) / 12.0f);
	const float shown  = ease(age / 0.4f);
	const float u0     = 0.13f * (1.0f - grow);
	const float v0     = 0.2f * (1.0f - grow);
	const float inset  = 0.5f - 0.5f / zoom;
	const auto  zoomed = [zoom, inset](float uv) { return uv / zoom + inset; };
	return {{{zoomed(u0), zoomed(v0), zoomed(u0 + grow), zoomed(v0 + grow), shown},
	         {u0, v0, u0 + grow, v0 + grow, logo_first ? shown : ease((age - 1.4f) / 2.2f)}}};
}

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_SPLASHANIMATION_H_
