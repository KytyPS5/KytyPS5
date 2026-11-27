#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_GALLERY_PAGES_TIMING_PAGE_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_GALLERY_PAGES_TIMING_PAGE_H_

#include "graphics/presentation/system/element.h"

namespace Libs::Graphics::SystemUi::Gallery {

/// Builds the Timing page in the current frame and returns its root. It shows each ease as a dot
/// moving over motion.slower, so their shapes tell apart, with what each is for. It reads the
/// current theme, fonts and clock, so it needs an open FrameScope.
[[nodiscard]] AnyElement BuildTimingPage();

} // namespace Libs::Graphics::SystemUi::Gallery

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_GALLERY_PAGES_TIMING_PAGE_H_
