#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_COMMON_PROPS_ITEM_LAYOUT_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_COMMON_PROPS_ITEM_LAYOUT_H_

#include "graphics/presentation/system/common/props/align.h"
#include "graphics/presentation/system/core/units.h"

#include <optional>

namespace Libs::Graphics::SystemUi {

/// What an element asks of the container it sits in. Every kind of element takes one, so Box, Text
/// and Image share the same fields, and the frame keeps it with the element. Lengths are in design
/// units. Min and max limit the length along the container's main axis, whichever way that runs.
struct ItemLayout {
	// An explicit size replaces the element's own measurement on that axis, border and padding
	// included. An element with an explicit main-axis size, width in a row or height in a column,
	// doesn't grow, though min_main and max_main still limit it.
	std::optional<Dp> width;
	std::optional<Dp> height;
	// The element's share of the container's free main-axis space, relative to its siblings. Zero,
	// the default, keeps it at its own size. Elements don't shrink when their siblings overflow the
	// container.
	float grow = 0.0f;
	// Limits on the main-axis length. Since elements never shrink, max_main caps how far an element
	// grows and min_main is a floor on its own size.
	std::optional<Dp> min_main;
	std::optional<Dp> max_main;
	// Overrides the container's cross-axis alignment for this element.
	std::optional<Align> align_self;
	// Gives up all its length along the main axis when the container is too short to leave the
	// other items their minimums, as a value label does when it would leave the track too short.
	// The container's length must be known, so it never collapses while measured with no limit.
	// An interactive element that has focus when it collapses loses it, since a collapsed element
	// is not registered.
	bool collapses = false;
};

} // namespace Libs::Graphics::SystemUi

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_COMMON_PROPS_ITEM_LAYOUT_H_
