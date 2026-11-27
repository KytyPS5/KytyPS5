#include "graphics/presentation/system/animation/tween/tween.h"

namespace Libs::Graphics::SystemUi {

TweenTracks ThemeTween(const ThemeContext& theme) {
	return {.rise = {.duration_ms = theme.motion.base, .ease = Ease::Enter},
	        .fall = {.duration_ms = theme.motion.exit_fade, .ease = Ease::Exit}};
}

TweenTracks TracksOf(const Tween& tween, const ThemeContext& theme) {
	return tween.tracks.value_or(ThemeTween(theme));
}

} // namespace Libs::Graphics::SystemUi
