#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_ELEMENT_PRIMITIVES_IMAGE_PAINT_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_ELEMENT_PRIMITIVES_IMAGE_PAINT_H_

#include "graphics/presentation/system/element/element.h"
#include "graphics/presentation/system/element/frame/frameHooks.h"

// The Image element's painting. Internal to the image primitive, so components don't include it.
namespace Libs::Graphics::SystemUi {

/// Draws an image where it was arranged, fitted and rounded as its styles say and faded by
/// `state`, as the image's paint hook. A null texture draws nothing.
void PaintImage(const FrameBuilder& frame, AnyElement image, ImDrawList& list,
                const PaintState& state);

} // namespace Libs::Graphics::SystemUi

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_ELEMENT_PRIMITIVES_IMAGE_PAINT_H_
