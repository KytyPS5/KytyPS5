#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_ELEMENT_FRAME_FRAME_LAYOUT_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_ELEMENT_FRAME_FRAME_LAYOUT_H_

#include "graphics/presentation/system/core/geometry.h"
#include "graphics/presentation/system/element/element.h"

namespace Libs::Graphics::SystemUi {

/// Only named here, so code that just measures and arranges needn't see the hooks or the builder.
class FrameBuilder;

/// Measures `root` and its subtree in the current frame and returns the size it takes in output
/// pixels. `max` is the space on offer, and infinity on an axis means unbounded. Each element is
/// measured by its kind's frame hooks, and the result is not clamped to `max`. A stale or none
/// handle measures to nothing. Each call starts a new measure pass.
[[nodiscard]] Vec2 Measure(AnyElement root, Vec2 max);

/// Places `root` at `rect` and its subtree inside it, from the sizes Measure found. Returns false
/// for a stale or none handle anywhere in the subtree, and for an element Measure never reached.
/// The root is laid out whatever an earlier arrange of its parent decided, since only a parent
/// collapses a child.
bool Arrange(AnyElement root, const Rect& rect);

/// Measures `element` within the current pass, for a frame hook measuring its children. It
/// records what it found on the element, as Measure does, without starting a new pass.
[[nodiscard]] Vec2 MeasureChild(FrameBuilder& frame, AnyElement element, Vec2 max);

/// Places `element` at `rect`, for a frame hook placing its children. A child its parent
/// collapsed keeps its rectangle and places nothing below it.
bool ArrangeChild(FrameBuilder& frame, AnyElement element, const Rect& rect);

} // namespace Libs::Graphics::SystemUi

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_ELEMENT_FRAME_FRAME_LAYOUT_H_
