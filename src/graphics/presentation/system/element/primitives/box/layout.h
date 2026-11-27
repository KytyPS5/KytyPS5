#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_ELEMENT_PRIMITIVES_BOX_LAYOUT_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_ELEMENT_PRIMITIVES_BOX_LAYOUT_H_

#include "graphics/presentation/system/core/geometry.h"
#include "graphics/presentation/system/element/element.h"
#include "graphics/presentation/system/element/frame/frameHooks.h"
#include "graphics/presentation/system/element/primitives/box/box.h"
#include "graphics/presentation/system/element/primitives/box/layout/flex.h"

#include <cstdint>
#include <vector>

// The Box element's measuring and arranging. Internal to the box primitive, so components don't
// include it.
namespace Libs::Graphics::SystemUi {

class FrameBuilder;

/// Measures a box and its subtree in the space `max`, as the box's measure hook. Its children are
/// laid out along its main axis by the flex solver, inside its padding and with its gap between
/// them. A stacked box instead measures as its largest child, inside its padding. A box that
/// scrolls keeps its children at their own main-axis lengths, is no longer on that axis than the
/// room in `max`, and has a main-axis min-content of its padding alone. A box with an explicit
/// size on an axis is that size on it, and its min-content is too.
[[nodiscard]] Measured MeasureBox(FrameBuilder& frame, AnyElement box, Vec2 max);

/// Places a box's children inside `rect`, from the sizes MeasureBox found, as the box's arrange
/// hook. A child the solver collapsed is marked so, keeps its rectangle and places nothing below
/// it. A stacked box gives every child the whole space inside its padding and collapses none. A box
/// that scrolls places children that don't fit past its end, from the start of its content.
/// Returns false when a child can't be placed.
bool ArrangeBox(FrameBuilder& frame, AnyElement box, const Rect& rect);

/// The layer `box` asks for, its styles' layer, as its layer hook. Arrange raises it to its
/// parent's.
[[nodiscard]] uint8_t LayerBox(const FrameBuilder& frame, AnyElement box);

/// The box layout's working storage, kept in the frame as a workspace so a steady frame allocates
/// nothing. The lengths, rectangles and flags are stacks, as a box's children are laid out while
/// its own solve is still in use, so a nested box's entries land after its parent's and the two
/// never overlap. The items are only read by the solve that builds them, before any child is laid
/// out, so one set is enough at any depth.
struct BoxLayoutWorkspace {
	std::vector<FlexItem> items;
	std::vector<float>    lengths;
	std::vector<Rect>     rects;
	std::vector<uint8_t>  collapsed;
	FlexScratch           scratch;
};

/// A box's padding in output pixels at the current scale. A box with none set has none.
[[nodiscard]] inline EdgeInsets PaddingOf(const BoxLayout& layout) {
	return ToPixels(layout.padding.value_or(DpInsets {}));
}

/// Whether a box lays its children out to scroll, at their own lengths along its main axis. A
/// stacked box is laid out as a stack before this is asked.
[[nodiscard]] inline bool Scrolls(const BoxProps& props) {
	return props.self.styles.overflow == Overflow::Scroll;
}

/// A length along the main axis for a direction.
[[nodiscard]] inline float MainOf(Vec2 size, FlexDirection direction) {
	return direction == FlexDirection::Row ? size.x : size.y;
}

/// A length along the cross axis for a direction.
[[nodiscard]] inline float CrossOf(Vec2 size, FlexDirection direction) {
	return direction == FlexDirection::Row ? size.y : size.x;
}

/// A size from its main and cross lengths.
[[nodiscard]] inline Vec2 FromAxes(float main, float cross, FlexDirection direction) {
	return direction == FlexDirection::Row ? Vec2 {main, cross} : Vec2 {cross, main};
}

/// Fills the workspace's items with one flex item per child of `box`. `use_measured_cross` takes
/// each child's cross size from its assigned measurement, as arranging does, and otherwise from its
/// natural one, as measuring does. Shared by MeasureBox and ArrangeBox so the two build the same
/// items.
void BuildFlexItems(FrameBuilder& frame, AnyElement box, const BoxLayout& layout,
                    bool use_measured_cross);

} // namespace Libs::Graphics::SystemUi

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_ELEMENT_PRIMITIVES_BOX_LAYOUT_H_
