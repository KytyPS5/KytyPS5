#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_ELEMENT_PRIMITIVES_IMAGE_LAYOUT_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_ELEMENT_PRIMITIVES_IMAGE_LAYOUT_H_

#include "graphics/presentation/system/common/props/itemLayout.h"
#include "graphics/presentation/system/core/geometry.h"
#include "graphics/presentation/system/element/element.h"
#include "graphics/presentation/system/element/frame/frameHooks.h"

// The Image element's measuring. Internal to the image primitive, so components don't include it.
namespace Libs::Graphics::SystemUi {

/// The size in output pixels an image of `size` pixels takes when it asks for `item`. Both explicit
/// lengths are used as they are. One alone sets that axis, and the other follows the image's
/// proportions, snapped to a whole pixel, or is 0 for an image with no proportions. With neither,
/// the image is its own size at the reference resolution, at the current scale, so it keeps its
/// place beside text and boxes but is magnified, and so softened, on larger screens.
[[nodiscard]] Vec2 ImageSize(const ItemLayout& item, Vec2 size);

/// Measures an image to ImageSize whatever the space on offer, so an image wider than its room
/// overflows it, and its least size is the same, as an image never shrinks.
[[nodiscard]] Measured MeasureImage(FrameBuilder& frame, AnyElement image, Vec2 max);

} // namespace Libs::Graphics::SystemUi

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_ELEMENT_PRIMITIVES_IMAGE_LAYOUT_H_
