#include "graphics/presentation/system/gallery/pages/timingPage.h"

#include "graphics/presentation/system/core.h"
#include "graphics/presentation/system/gallery/components/page.h"
#include "graphics/presentation/system/gallery/pages/parts/motionTrack.h"

#include <array>
#include <string_view>
#include <vector>

namespace Libs::Graphics::SystemUi::Gallery {

namespace {

constexpr std::string_view RULE =
    "An ease gives a move its feel. Arrivals slow into place, exits speed away, moves within the "
    "screen ease at both ends, and only a moment worth celebrating overshoots.";

// Each ease and what it is for, shown moving over motion.slower so their shapes tell apart.
struct NamedEase {
	std::string_view name;
	Ease             ease;
};

constexpr std::array<NamedEase, 5> EASES {NamedEase {"Ease::Linear, spinners", Ease::Linear},
                                          NamedEase {"Ease::Enter, arriving", Ease::Enter},
                                          NamedEase {"Ease::Exit, leaving", Ease::Exit},
                                          NamedEase {"Ease::Standard, moving", Ease::Standard},
                                          NamedEase {"Ease::Emphasis, a pop", Ease::Emphasis}};

// Each ease moving over motion.slower.
AnyElement Eases(const ThemeContext& theme) {
	std::vector<AnyElement> rows;
	for (const NamedEase& named: EASES) {
		rows.push_back(TrackRow(theme, named.name, DotPosition(theme.motion.slower, named.ease)));
	}
	return Lines(theme, rows);
}

} // namespace

AnyElement BuildTimingPage() {
	const ThemeContext& theme = CurrentThemeContext();
	return Page({.title = "Timing", .subtitle = RULE},
	            {Section(theme, "Eases, over motion.slower", Eases(theme))});
}

} // namespace Libs::Graphics::SystemUi::Gallery
