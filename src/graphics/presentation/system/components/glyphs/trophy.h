#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_COMPONENTS_GLYPHS_TROPHY_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_COMPONENTS_GLYPHS_TROPHY_H_

#include "graphics/presentation/system/core.h"
#include "graphics/presentation/system/element.h"

namespace Libs::Graphics::SystemUi {

/// How a trophy cup looks.
struct TrophyGlyphStyles {
	// The cup's colour, usually its grade's. It has no default, so a cup given none is clear.
	Color color;
};

/// How large a trophy cup is, what it asks of its container and how it looks.
struct TrophyGlyphProps {
	/// What the cup asks of its container and how it looks, following the element kinds' Self
	/// convention.
	struct Self {
		ItemLayout        layout;
		TrophyGlyphStyles styles;
	};

	// The cup's square side.
	Dp   size;
	Self self;
};

/// A trophy cup as `props` say, centred in its square. A bowl with square top corners and an
/// outlined handle each side over a stem and a base, the shape the overlay drew before, drawn as
/// one glyph so it needs no image or font. It needs an open FrameScope, and if the frame can't
/// build it, the result is a none handle.
[[nodiscard]] GlyphElement TrophyGlyph(const TrophyGlyphProps& props);

} // namespace Libs::Graphics::SystemUi

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_COMPONENTS_GLYPHS_TROPHY_H_
