#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_GALLERY_PAGES_PARTS_SWATCH_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_GALLERY_PAGES_PARTS_SWATCH_H_

#include "graphics/presentation/system/core.h"
#include "graphics/presentation/system/element.h"

#include <cstddef>
#include <span>
#include <string_view>

namespace Libs::Graphics::SystemUi::Gallery {

/// A colour and the name it goes by in the code, such as "surface.raised".
struct NamedColor {
	std::string_view name;
	Color            color;
};

/// How many swatches a row of the Colours page holds, as many as fit beside the sidebar at the
/// reference size.
inline constexpr size_t SWATCH_COLUMNS = 12;

/// A swatch's width in design units, so SWATCH_COLUMNS of them and their gaps fit the page. A token
/// name too long for it is clipped.
inline constexpr float SWATCH_WIDTH = 108.0f;

/// A swatch of `named`, a chip of its colour over a strip that is half black and half white, so
/// its translucency shows, with its name and its value as #RRGGBB and opacity below.
[[nodiscard]] AnyElement Swatch(const ThemeContext& theme, const NamedColor& named);

/// Swatches of `colors` in rows of SWATCH_COLUMNS.
[[nodiscard]] AnyElement SwatchGrid(const ThemeContext& theme, std::span<const NamedColor> colors);

} // namespace Libs::Graphics::SystemUi::Gallery

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_GALLERY_PAGES_PARTS_SWATCH_H_
