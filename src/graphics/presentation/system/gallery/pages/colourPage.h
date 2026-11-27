#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_GALLERY_PAGES_COLOUR_PAGE_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_GALLERY_PAGES_COLOUR_PAGE_H_

#include "graphics/presentation/system/element.h"

namespace Libs::Graphics::SystemUi::Gallery {

/// Builds the Colours page in the current frame and returns its root. It shows the palette's
/// ramps, the opacities colours take their alpha from, every semantic colour by the name it has in
/// the code, each over a strip half black and half white so its translucency shows, and a table of
/// the theme's contrast, each pair judged over a black and a white frame with the worse deciding.
/// It reads the current theme and fonts, so it needs an open FrameScope.
[[nodiscard]] AnyElement BuildColourPage();

} // namespace Libs::Graphics::SystemUi::Gallery

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_GALLERY_PAGES_COLOUR_PAGE_H_
