#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_VIEWS_TROPHY_UNLOCKED_TROPHY_UNLOCKED_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_VIEWS_TROPHY_UNLOCKED_TROPHY_UNLOCKED_H_

#include "graphics/presentation/system/core.h"
#include "graphics/presentation/system/core/encodedImage.h"
#include "graphics/presentation/system/element.h"

#include <cstdint>
#include <string>

namespace Libs::Graphics::SystemUi {

/// A trophy's grade, as its toast shows it. Unknown for a grade the game gave that isn't one of
/// these.
enum class TrophyGrade : uint8_t { Unknown, Platinum, Gold, Silver, Bronze };

/// The grade a toast shows for the game's `raw` grade, 1 platinum to 4 bronze, and Unknown for any
/// other.
[[nodiscard]] TrophyGrade GradeOf(int32_t raw);

/// The colour of a trophy of `grade`, the theme's trophy colour for it, and the success colour for
/// a grade it doesn't know.
[[nodiscard]] Color TrophyGradeColor(const ThemeContext& theme, TrophyGrade grade);

/// How a trophy toast differs from a notice. It is plain, with no band, as a cup in its grade's
/// colour marks it, and an icon beside a bold title over a quiet subtitle.
struct TrophyUnlockedTokens {
	// Wider than a notice at least, as the icon takes room. Its widest is the notice's, so the two
	// share a right edge in the stack.
	Dp min_width;
	// The icon's square side and corners, and the outline that parts it from the card.
	Dp    icon_size;
	Dp    icon_radius;
	Color icon_outline;
	Dp    icon_outline_width;
	// The glow around the icon, in the grade's colour, so the trophy's grade shows at a glance.
	Glow icon_glow;
	// The tile shown in the icon's place when there is none, with a larger cup on it.
	Color tile_fill;
	Dp    tile_cup_size;
	// The cup before the subtitle, about as tall as the subtitle's letters.
	Dp cup_size;
	// The space between the icon and the text.
	Dp        gap;
	TypeStyle title;
	Color     title_color;
	int       title_lines = 0;
	TypeStyle subtitle;
	Color     subtitle_color;
};

/// The theme's trophy toast, drawn on the theme's toast card.
[[nodiscard]] TrophyUnlockedTokens TrophyUnlockedDefaults(const ThemeContext& theme);

/// What a trophy toast says.
struct TrophyUnlockedProps {
	// The trophy's name, as the game gives it.
	std::string name;
	// The quieter line under the name, such as "Trophy earned".
	std::string subtitle;
	TrophyGrade grade = TrophyGrade::Unknown;
	// The trophy's icon, still encoded, or null when the game gave none.
	EncodedImage icon;
};

/// The toast for an unlocked trophy, saying what `props` say, built in the current frame. It has
/// the icon beside the name, over the subtitle after a cup in the grade's colour, or a tile with a
/// larger cup in the icon's place when it has none or it can't be shown, on a plain toast. The icon
/// is decoded by the ImageCache open where it is built. It reads the current theme, so it needs an
/// open FrameScope. If the frame can't build it, the result is a none handle.
[[nodiscard]] BoxElement TrophyUnlocked(const TrophyUnlockedProps& props);

} // namespace Libs::Graphics::SystemUi

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_VIEWS_TROPHY_UNLOCKED_TROPHY_UNLOCKED_H_
