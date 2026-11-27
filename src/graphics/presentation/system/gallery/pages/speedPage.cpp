#include "graphics/presentation/system/gallery/pages/speedPage.h"

#include "graphics/presentation/system/core.h"
#include "graphics/presentation/system/gallery/components/page.h"
#include "graphics/presentation/system/gallery/pages/parts/format.h"
#include "graphics/presentation/system/gallery/pages/parts/motionTrack.h"

#include <array>
#include <string_view>
#include <vector>

namespace Libs::Graphics::SystemUi::Gallery {

namespace {

constexpr std::string_view RULE =
    "Small changes move fast and large ones take longer, so the eye can follow them. Keep a move "
    "under half a second, and hold back the longer durations for an arrival worth noticing.";

// A duration of the theme and its name in the code.
struct NamedDuration {
	std::string_view name;
	float MotionDurations::* ms;
};

constexpr std::array<NamedDuration, 11> DURATIONS {
    NamedDuration {"motion.instant", &MotionDurations::instant},
    NamedDuration {"motion.fast", &MotionDurations::fast},
    NamedDuration {"motion.base", &MotionDurations::base},
    NamedDuration {"motion.slow", &MotionDurations::slow},
    NamedDuration {"motion.slower", &MotionDurations::slower},
    NamedDuration {"motion.emphasis", &MotionDurations::emphasis},
    NamedDuration {"motion.shine", &MotionDurations::shine},
    NamedDuration {"motion.shine_delay", &MotionDurations::shine_delay},
    NamedDuration {"motion.exit_fade", &MotionDurations::exit_fade},
    NamedDuration {"motion.pulse", &MotionDurations::pulse},
    NamedDuration {"motion.reduced_fade_max", &MotionDurations::reduced_fade_max}};
// The durations are all floats, so one added to the theme and not here fails to build, though a
// table naming one twice could still hide another.
static_assert(sizeof(MotionDurations) == DURATIONS.size() * sizeof(float));

// The durations a move takes, shown moving with the Standard ease.
constexpr std::array<NamedDuration, 5> MOVING_DURATIONS {
    NamedDuration {"motion.fast", &MotionDurations::fast},
    NamedDuration {"motion.base", &MotionDurations::base},
    NamedDuration {"motion.slow", &MotionDurations::slow},
    NamedDuration {"motion.slower", &MotionDurations::slower},
    NamedDuration {"motion.emphasis", &MotionDurations::emphasis}};

// Each duration a move takes, as a dot moving with the Standard ease.
AnyElement Moving(const ThemeContext& theme) {
	std::vector<AnyElement> rows;
	for (const NamedDuration& named: MOVING_DURATIONS) {
		rows.push_back(
		    TrackRow(theme, named.name, DotPosition(theme.motion.*named.ms, Ease::Standard)));
	}
	return Lines(theme, rows);
}

// Every duration of the theme and its value.
AnyElement Durations(const ThemeContext& theme) {
	std::vector<AnyElement> rows;
	for (const NamedDuration& named: DURATIONS) {
		rows.push_back(TokenLine(theme, named.name, MsText(theme.motion.*named.ms)));
	}
	return Lines(theme, rows);
}

} // namespace

AnyElement BuildSpeedPage() {
	const ThemeContext& theme = CurrentThemeContext();
	return Page({.title = "Speed", .subtitle = RULE},
	            {Section(theme, "Moving, with Ease::Standard", Moving(theme)),
	             Section(theme, "Durations", Durations(theme))});
}

} // namespace Libs::Graphics::SystemUi::Gallery
