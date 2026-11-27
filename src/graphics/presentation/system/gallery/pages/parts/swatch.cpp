#include "graphics/presentation/system/gallery/pages/parts/swatch.h"

#include "graphics/presentation/system/element.h"
#include "graphics/presentation/system/gallery/components/page.h"
#include "graphics/presentation/system/gallery/pages/parts/format.h"

#include <array>
#include <vector>

namespace Libs::Graphics::SystemUi::Gallery {

namespace {

// A swatch chip's height in design units.
constexpr Dp CHIP_HEIGHT = Dp(46.0f);

// The strip behind a chip, black and white halves, so a translucent colour shows both.
constexpr Color STRIP_DARK  = Rgb(0x000000);
constexpr Color STRIP_LIGHT = Rgb(0xFFFFFF);

// The chip, the strip with the colour laid over it and a faint edge, so a colour close to the
// card still shows where it ends.
AnyElement Chip(const ThemeContext& theme, Color color) {
	const std::array<AnyElement, 2> halves {
	    Box({.self = {.layout = {.grow = 1.0f, .align_self = Align::Stretch},
	                  .styles = {.fill = STRIP_DARK}}}),
	    Box({.self = {.layout = {.grow = 1.0f, .align_self = Align::Stretch},
	                  .styles = {.fill = STRIP_LIGHT}}}),
	};
	const std::array<AnyElement, 2> layers {
	    Box({}, halves),
	    Box({.self = {.styles = {.fill         = color,
	                             .border       = theme.colors.border.subtle,
	                             .border_width = theme.border.hair}}}),
	};
	return Box({.layout = {.stacked = true},
	            .self   = {.layout = {.width = Dp(SWATCH_WIDTH), .height = CHIP_HEIGHT}}},
	           layers);
}

} // namespace

AnyElement Swatch(const ThemeContext& theme, const NamedColor& named) {
	const std::array<AnyElement, 2> parts {
	    Chip(theme, named.color),
	    NameAndValue(theme, named.name, HexText(named.color), SWATCH_WIDTH),
	};
	return Box({.layout = {.direction = FlexDirection::Column}}, parts);
}

AnyElement SwatchGrid(const ThemeContext& theme, std::span<const NamedColor> colors) {
	std::vector<AnyElement> swatches;
	swatches.reserve(colors.size());
	for (const NamedColor& named: colors) {
		swatches.push_back(Swatch(theme, named));
	}
	return Grid(theme, swatches, SWATCH_COLUMNS);
}

} // namespace Libs::Graphics::SystemUi::Gallery
