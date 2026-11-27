#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_GALLERY_PAGES_PARTS_CONTRAST_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_GALLERY_PAGES_PARTS_CONTRAST_H_

#include "graphics/presentation/system/core.h"

#include <string_view>
#include <vector>

// The WCAG contrast of the theme's colour pairs, which the Colours page tabulates. The surfaces
// are translucent over the game, so each pair is judged over a black and a white frame and the
// worse of the two decides.
namespace Libs::Graphics::SystemUi::Gallery {

/// The least contrast for text, and for large text and parts that aren't text.
inline constexpr float CONTRAST_TEXT  = 4.5f;
inline constexpr float CONTRAST_LARGE = 3.0f;

/// The frames a translucent pair is judged over, the darkest and the brightest a game can show.
inline constexpr Color CONTRAST_BLACK_FRAME = Rgb(0x000000);
inline constexpr Color CONTRAST_WHITE_FRAME = Rgb(0xFFFFFF);

/// One pair of the contrast table, a foreground on a background, judged against `target`.
struct ContrastRow {
	std::string_view name;
	Color            foreground;
	Color            background;
	float            target = CONTRAST_TEXT;
	// What lies under the background, for a tint on a surface. Transparent for nothing.
	Color under;
};

/// `top` drawn over `under`, which is taken as opaque, blended in sRGB as the renderer does. What
/// results is opaque.
[[nodiscard]] Color Composite(Color top, Color under);

/// The WCAG contrast ratio of two colours, from 1 to 21. Their alphas are ignored.
[[nodiscard]] float ContrastRatio(Color first, Color second);

/// The ratio `row` reaches on a frame of the opaque colour `frame`, with the background and what
/// lies under it laid over the frame first.
[[nodiscard]] float ContrastOver(const ContrastRow& row, Color frame);

/// Whether `row` reaches its target over both a black and a white frame.
[[nodiscard]] bool ContrastPasses(const ContrastRow& row);

/// The pairs the theme's contrast is judged on, the text levels on each surface, the status
/// colours on a raised surface and on their tints, and the parts that aren't text.
[[nodiscard]] std::vector<ContrastRow> ContrastRows(const ThemeContext& theme);

} // namespace Libs::Graphics::SystemUi::Gallery

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_GALLERY_PAGES_PARTS_CONTRAST_H_
