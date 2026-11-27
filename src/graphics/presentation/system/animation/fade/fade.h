#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_ANIMATION_FADE_FADE_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_ANIMATION_FADE_FADE_H_

#include "graphics/presentation/system/animation/timeline.h"
#include "graphics/presentation/system/core/theme.h"

#include <optional>

namespace Libs::Graphics::SystemUi {

/// How a fade should play. It is shown or hidden, and fades between the two along its timeline.
struct Fade {
	// Whether the content should be shown. Turning it off fades the content out from wherever it
	// is, and turning it back on fades it in again from there.
	bool shown = true;
	// How it fades. Without one it takes the theme's, from ThemeFade.
	std::optional<Timeline> timeline;
};

/// The theme's fade, in at the base speed on the theme's enter curve, out at the exit fade's speed
/// on its exit curve, and shown until hidden.
[[nodiscard]] Timeline ThemeFade(const ThemeContext& theme);

/// The timeline `fade` plays, its own or else the theme's.
[[nodiscard]] Timeline TimelineOf(const Fade& fade, const ThemeContext& theme);

} // namespace Libs::Graphics::SystemUi

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_ANIMATION_FADE_FADE_H_
