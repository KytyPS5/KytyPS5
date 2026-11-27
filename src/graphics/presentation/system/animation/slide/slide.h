#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_ANIMATION_SLIDE_SLIDE_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_ANIMATION_SLIDE_SLIDE_H_

#include "graphics/presentation/system/animation/timeline.h"
#include "graphics/presentation/system/core/dp.h"
#include "graphics/presentation/system/core/theme.h"

#include <optional>

namespace Libs::Graphics::SystemUi {

/// How an element slides into its place. It enters `x` and `y` away from its place the first time
/// it is built and eases in, then stays put, so a fade beside it takes it away when it leaves. Its
/// space never moves, so the content around it stays where it is while it slides.
struct Slide {
	// How far from its place it enters, in design units. A positive x enters from the right and a
	// positive y from below.
	Dp x;
	Dp y;
	// How it moves in, its delay and its entrance. Without one it takes the theme's, from
	// ThemeSlide. Its hold and exit are not used, as it never slides away.
	std::optional<Timeline> timeline;
};

/// The theme's slide, in at the base speed on the theme's enter curve, the same as its fade's
/// entrance, so a slide and a fade on one element arrive together.
[[nodiscard]] Timeline ThemeSlide(const ThemeContext& theme);

/// The timeline `slide` plays, its own or else the theme's, held until dismissed, as it never
/// leaves.
[[nodiscard]] Timeline TimelineOf(const Slide& slide, const ThemeContext& theme);

} // namespace Libs::Graphics::SystemUi

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_ANIMATION_SLIDE_SLIDE_H_
