#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_ELEMENT_FRAME_FRAME_PAINT_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_ELEMENT_FRAME_FRAME_PAINT_H_

#include "graphics/presentation/system/element/element.h"

/// Only named, so code that paints needn't pull in ImGui to declare a call.
struct ImDrawList;

namespace Libs::Graphics::SystemUi {

/// What a paint pass carries down the tree. A parent's opacity reaches its children, so a fading
/// parent fades everything below it. An element's own opacity, poses and clips join it as they
/// come over.
struct PaintState {
	float opacity = 1.0f;
};

/// Draws `root` and its subtree from the current frame into `list`, which must have been arranged.
/// Each element draws itself through its kind's paint hook and then its children are drawn over
/// it, in order. Elements on a higher layer, as Arrange set them, are drawn over everything on
/// lower layers in the same call, keeping the clip and opacity they would have had in tree order.
/// Layers order one call only, so a later Paint into the same list draws over all of an earlier
/// one. `list` may already be split into channels by the caller, and a paint hook must never leave
/// it split or change its channel, as Paint switches channels around each hook. An element its
/// parent collapsed draws nothing, and neither does its subtree. A stale or none handle draws
/// nothing.
void Paint(AnyElement root, ImDrawList& list, const PaintState& state = {});

} // namespace Libs::Graphics::SystemUi

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_ELEMENT_FRAME_FRAME_PAINT_H_
