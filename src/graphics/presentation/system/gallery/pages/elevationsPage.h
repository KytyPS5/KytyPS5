#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_GALLERY_PAGES_ELEVATIONS_PAGE_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_GALLERY_PAGES_ELEVATIONS_PAGE_H_

#include "graphics/presentation/system/element.h"
#include "graphics/presentation/system/element/frame/frameBuilder.h"

namespace Libs::Graphics::SystemUi::Gallery {

/// Builds the Elevations page in the current frame and returns its root. It shows the theme's
/// low, medium and high shadows, each in a well beside its layers, a sample that lifts from the
/// low shadow to the medium one on hover, the same overlapping cards on one layer and then drawn by
/// layer, and a surface held in focus with its ring and glow. It reads the current theme, fonts and
/// clock, so it needs an open FrameScope.
[[nodiscard]] AnyElement BuildElevationsPage();

/// Keeps where the page's hover sample was placed in `frame`, which the next frame's build tests
/// the pointer against, as the sidebar keeps its rows. Call it after arranging.
void RememberElevationsHover(const FrameBuilder& frame);

} // namespace Libs::Graphics::SystemUi::Gallery

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_GALLERY_PAGES_ELEVATIONS_PAGE_H_
