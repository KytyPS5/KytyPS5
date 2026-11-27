#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_GALLERY_PAGES_SPACING_PAGE_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_GALLERY_PAGES_SPACING_PAGE_H_

#include "graphics/presentation/system/element.h"

namespace Libs::Graphics::SystemUi::Gallery {

/// Builds the Spacing page in the current frame and returns its root. It shows the spacing scale
/// as bars of each length, each radius on a square, each border width on a box, and the rules that
/// scale design units to the output, each token by its name in the code with its value. It reads
/// the current theme and fonts, so it needs an open FrameScope.
[[nodiscard]] AnyElement BuildSpacingPage();

} // namespace Libs::Graphics::SystemUi::Gallery

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_GALLERY_PAGES_SPACING_PAGE_H_
