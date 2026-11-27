#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_ANIMATION_TWEEN_TWEEN_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_ANIMATION_TWEEN_TWEEN_H_

#include "graphics/presentation/system/animation/tween/tweenValue.h"
#include "graphics/presentation/system/core/theme.h"

#include <optional>

namespace Libs::Graphics::SystemUi {

/// How a tween should play. It eases toward its target and turns around from wherever it is when
/// the target changes. It starts resting at its target, so only a change of target animates.
struct Tween {
	// Where the value should go, within 0 to 1.
	float target = 1.0f;
	// How it moves. Without them it takes the theme's, from ThemeTween.
	std::optional<TweenTracks> tracks;
	// Every move takes the track's whole duration however short, as a tab indicator sliding
	// between neighbouring tabs does.
	bool whole_track = false;
};

/// The theme's tween, rising at the base speed on the theme's enter curve and falling at the exit
/// fade's speed on its exit curve, as the theme's fade does.
[[nodiscard]] TweenTracks ThemeTween(const ThemeContext& theme);

/// The tracks `tween` moves on, its own or else the theme's.
[[nodiscard]] TweenTracks TracksOf(const Tween& tween, const ThemeContext& theme);

} // namespace Libs::Graphics::SystemUi

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_ANIMATION_TWEEN_TWEEN_H_
