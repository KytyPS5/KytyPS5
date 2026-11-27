#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_GALLERY_PAGES_TYPOGRAPHY_PAGE_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_GALLERY_PAGES_TYPOGRAPHY_PAGE_H_

#include "graphics/presentation/system/element.h"

namespace Libs::Graphics::SystemUi::Gallery {

/// Builds the Typography page in the current frame and returns its root. It shows the type ramp,
/// each style by its name in the code with a sample in its line box and its metrics, the text
/// colours, how text gives way in a slot too short for it, longer languages, digits aligned in a
/// column and the font files. It reads the current theme and fonts, so it needs an open
/// FrameScope.
[[nodiscard]] AnyElement BuildTypographyPage();

} // namespace Libs::Graphics::SystemUi::Gallery

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_GALLERY_PAGES_TYPOGRAPHY_PAGE_H_
