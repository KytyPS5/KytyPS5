#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_GALLERY_PAGES_ANIMATIONS_PAGE_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_GALLERY_PAGES_ANIMATIONS_PAGE_H_

#include "graphics/presentation/system/element.h"

namespace Libs::Graphics::SystemUi::Gallery {

/// Builds the Animations page in the current frame and returns its root. It shows Animate's fade,
/// slide, tween and shine and a fade with a slide, each looping as set beside the same under
/// reduced motion, with the props and tokens that make it, the shine held still at a few places,
/// then the interaction tokens no component reads yet. It reads the current theme, fonts and clock,
/// so it needs an open FrameScope.
[[nodiscard]] AnyElement BuildAnimationsPage();

} // namespace Libs::Graphics::SystemUi::Gallery

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_GALLERY_PAGES_ANIMATIONS_PAGE_H_
