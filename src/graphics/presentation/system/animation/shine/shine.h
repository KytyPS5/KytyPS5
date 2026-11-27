#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_ANIMATION_SHINE_SHINE_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_ANIMATION_SHINE_SHINE_H_

#include "graphics/presentation/system/animation/timeline.h"
#include "graphics/presentation/system/core/theme.h"

#include <optional>

namespace Libs::Graphics::SystemUi {

/// A soft band of light across an element, drawn over it in the theme's shine. It sweeps across
/// once, from just off the element's left edge to just off its right, the first time it is built,
/// and then shows nothing, as a passing moment rather than a look. With reduced motion it doesn't
/// sweep. A held position draws the band standing still instead, also with reduced motion.
struct Shine {
	// Where the band stands, from 0, wholly off the left edge, to 1, wholly off the right, for
	// showing a shine without its sweep. Without one it sweeps.
	std::optional<float> position;
	// How it sweeps, its delay and its pass across. Without one it takes the theme's, from
	// ThemeShine. Its hold and exit are not used, as it never sweeps back.
	std::optional<Timeline> timeline;
};

/// The theme's sweep, across in the theme's shine time on its standard curve, so it eases in and
/// out of view.
[[nodiscard]] Timeline ThemeShine(const ThemeContext& theme);

/// The timeline `shine` sweeps on, its own or else the theme's, held once across, as it never
/// sweeps back.
[[nodiscard]] Timeline TimelineOf(const Shine& shine, const ThemeContext& theme);

} // namespace Libs::Graphics::SystemUi

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_ANIMATION_SHINE_SHINE_H_
