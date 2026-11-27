#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_GALLERY_PAGES_THEME_PAGE_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_GALLERY_PAGES_THEME_PAGE_H_

#include "graphics/presentation/system/element.h"

namespace Libs::Graphics::SystemUi::Gallery {

/// Builds the Theme page in the current frame and returns its root. It shows how the theme's three
/// tiers lead from a palette colour to a component's look, a card patched beside the theme's own,
/// and how one length scales with the screen. It reads the current theme and fonts, so it needs an
/// open FrameScope.
[[nodiscard]] AnyElement BuildThemePage();

} // namespace Libs::Graphics::SystemUi::Gallery

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_GALLERY_PAGES_THEME_PAGE_H_
