#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_GALLERY_PAGES_SPEED_PAGE_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_GALLERY_PAGES_SPEED_PAGE_H_

#include "graphics/presentation/system/element.h"

namespace Libs::Graphics::SystemUi::Gallery {

/// Builds the Speed page in the current frame and returns its root. It shows each duration a move
/// takes as a dot moving with the Standard ease, then every duration of the theme by its name in
/// the code. It reads the current theme, fonts and clock, so it needs an open FrameScope.
[[nodiscard]] AnyElement BuildSpeedPage();

} // namespace Libs::Graphics::SystemUi::Gallery

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_GALLERY_PAGES_SPEED_PAGE_H_
