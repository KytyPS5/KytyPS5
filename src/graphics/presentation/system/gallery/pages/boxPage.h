#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_GALLERY_PAGES_BOX_PAGE_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_GALLERY_PAGES_BOX_PAGE_H_

#include "graphics/presentation/system/element.h"

namespace Libs::Graphics::SystemUi::Gallery {

/// Builds the Box page in the current frame and returns its root, its head over sections of
/// captioned samples, fill, radius, border, stacking and motion, the trophy and emphasis glows,
/// gradients, states, a bright frame and boxes that scroll. It reads the current theme and fonts,
/// so it needs an open FrameScope. The caller measures, arranges and paints it.
[[nodiscard]] AnyElement BuildBoxPage();

} // namespace Libs::Graphics::SystemUi::Gallery

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_GALLERY_PAGES_BOX_PAGE_H_
