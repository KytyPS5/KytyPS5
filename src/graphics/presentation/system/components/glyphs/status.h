#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_COMPONENTS_GLYPHS_STATUS_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_COMPONENTS_GLYPHS_STATUS_H_

#include "graphics/presentation/system/core.h"
#include "graphics/presentation/system/element.h"

#include <optional>

namespace Libs::Graphics::SystemUi {

/// How a status glyph looks.
struct StatusGlyphStyles {
	// The glyph's colour, or its status's colour from the theme when unset.
	std::optional<Color> color;
};

/// How a status glyph is drawn, which status it shows, its square side, whether its circle or
/// triangle is filled, what it asks of its container and how it looks. Filled, its mark is drawn in
/// the theme's colour for content on a coloured fill, as the mark can't be cut out to show what's
/// behind.
struct StatusGlyphProps {
	/// What the glyph asks of its container and how it looks, following the element kinds' Self
	/// convention.
	struct Self {
		ItemLayout        layout;
		StatusGlyphStyles styles;
	};

	// Which status the glyph shows, which picks its shape and, unless its styles set one, its
	// colour.
	Status status = Status::Info;
	Dp     size;
	bool   fill = false;
	Self   self;
};

/// The glyph for the status `props` name, as they say. An i in a circle for Info, an exclamation
/// mark in a triangle for Warning, an exclamation mark in a circle for Error and a check mark in a
/// circle for Success. It reads the current theme, so it needs an open FrameScope, and if the frame
/// can't build it, the result is a none handle.
[[nodiscard]] GlyphElement StatusGlyph(const StatusGlyphProps& props);

} // namespace Libs::Graphics::SystemUi

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_COMPONENTS_GLYPHS_STATUS_H_
