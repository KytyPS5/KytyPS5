#include "graphics/presentation/system/animation/shine/shine.h"

#include <limits>

namespace Libs::Graphics::SystemUi {

Timeline ThemeShine(const ThemeContext& theme) {
	return {.enter = {.duration_ms = theme.motion.shine, .ease = Ease::Standard}};
}

Timeline TimelineOf(const Shine& shine, const ThemeContext& theme) {
	Timeline timeline = shine.timeline.value_or(ThemeShine(theme));
	// It never sweeps back, so a hold of its own would only end a sweep that is already over.
	timeline.hold_ms = std::numeric_limits<float>::infinity();
	return timeline;
}

} // namespace Libs::Graphics::SystemUi
