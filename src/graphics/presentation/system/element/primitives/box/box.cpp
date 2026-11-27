#include "graphics/presentation/system/element/primitives/box/box.h"

#include "graphics/presentation/system/element/frame/frameBuilder.h"
#include "graphics/presentation/system/element/primitives/box/layout.h"
#include "graphics/presentation/system/element/primitives/box/paint.h"
#include "graphics/presentation/system/element/primitives/box/scroll.h"

namespace Libs::Graphics::SystemUi {

namespace {

// How boxes are measured, placed and painted, set on every frame a box is built in.
constexpr FrameHooks BOX_FRAME_HOOKS {.measure    = &MeasureBox,
                                      .arrange    = &ArrangeBox,
                                      .paint      = &PaintBox,
                                      .inherit    = &InheritBox,
                                      .clip       = &ClipBox,
                                      .paint_over = &PaintBoxOver,
                                      .layer      = &LayerBox};

} // namespace

BoxElement Box(const BoxProps& props, const Children& children) {
	FrameBuilder& frame = CurrentBuilder();
	// Set on every call, which is one store, so any frame a box is built in can lay it out and
	// paint it without a separate step to remember.
	frame.SetFrameHooks<BoxProps>(BOX_FRAME_HOOKS);
	// What the box asks of its container is generic to every element, so the frame keeps it with
	// the element, and the box's table keeps the props whole for layout and paint.
	const BoxElement box = frame.AddNode(props, children, props.self.layout);
	// Only a box really built scrolls, so a refused one keeps no position, as a refused Animate
	// moves on no animation.
	if (Scrolls(props) && frame.Find(box) != nullptr) {
		UpdateScroll(frame, box, props);
	}
	return box;
}

} // namespace Libs::Graphics::SystemUi
