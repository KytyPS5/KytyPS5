#include "graphics/presentation/system/animation/fade/fade.h"

namespace Libs::Graphics::SystemUi {

Timeline ThemeFade(const ThemeContext& theme) {
	return {.enter = {.duration_ms = theme.motion.base, .ease = Ease::Enter},
	        .exit  = {.duration_ms = theme.motion.exit_fade, .ease = Ease::Exit}};
}

Timeline TimelineOf(const Fade& fade, const ThemeContext& theme) {
	return fade.timeline.value_or(ThemeFade(theme));
}

} // namespace Libs::Graphics::SystemUi
