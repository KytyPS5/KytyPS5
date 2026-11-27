#include "graphics/presentation/system/animation/slide/slide.h"

#include <limits>

namespace Libs::Graphics::SystemUi {

Timeline ThemeSlide(const ThemeContext& theme) {
	return {.enter = {.duration_ms = theme.motion.base, .ease = Ease::Enter}};
}

Timeline TimelineOf(const Slide& slide, const ThemeContext& theme) {
	Timeline timeline = slide.timeline.value_or(ThemeSlide(theme));
	// It never leaves, so a hold of its own would only end a slide that has nothing to do.
	timeline.hold_ms = std::numeric_limits<float>::infinity();
	return timeline;
}

} // namespace Libs::Graphics::SystemUi
