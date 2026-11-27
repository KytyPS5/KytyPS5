#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_GALLERY_PAGES_GLYPH_PAGE_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_GALLERY_PAGES_GLYPH_PAGE_H_

#include "graphics/presentation/system/element.h"

namespace Libs::Graphics::SystemUi::Gallery {

/// Builds the Glyphs page in the current frame and returns its root. It shows the trophy cup in
/// each grade's colour at the two sizes the trophy toast draws it at and one larger, beside the
/// grade colours by their names in the code, every status glyph outlined and filled, and the cup
/// paired with a word as it must be in use. It reads the current theme and fonts, so it needs an
/// open FrameScope.
[[nodiscard]] AnyElement BuildGlyphPage();

} // namespace Libs::Graphics::SystemUi::Gallery

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_GALLERY_PAGES_GLYPH_PAGE_H_
