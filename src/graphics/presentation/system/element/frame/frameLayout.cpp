#include "graphics/presentation/system/element/frame/frameLayout.h"

#include "graphics/presentation/system/element/frame/frameBuilder.h"

#include <algorithm>
#include <cassert>
#include <cstdint>

namespace Libs::Graphics::SystemUi {

Vec2 MeasureChild(FrameBuilder& frame, AnyElement element, Vec2 max) {
	FrameView::ElementLayout* layout = frame.LayoutOf(element);
	if (layout == nullptr) {
		return {};
	}
	layout->is_measured = true;
	layout->last_limit  = max;
	Measured found;
	if (const FrameHooks* hooks = frame.FrameHooksOf(element);
	    hooks != nullptr && hooks->measure != nullptr) {
		found = hooks->measure(frame, element, max);
		// Measuring adds no elements, so the layouts don't move, but reading it again keeps this
		// correct should a hook ever build.
		layout = frame.LayoutOf(element);
		assert(layout != nullptr && "a measure hook must not begin the frame or build");
		if (layout == nullptr) {
			return found.size;
		}
	}
	layout->measured    = found.size;
	layout->min_content = found.min_content;
	return found.size;
}

Vec2 Measure(AnyElement root, Vec2 max) {
	FrameBuilder& frame = CurrentBuilder();
	(void)frame.BeginMeasurePass();
	return MeasureChild(frame, root, max);
}

namespace {

// The layer of the element being arranged, which its children are raised to, so a child is never
// drawn below the element holding it. It is remade at 0 every frame and set again by each Arrange.
struct ArrangeLayer {
	uint8_t current = 0;
};

// The layer `element` asks for through its hook, 0 without one.
uint8_t AskedLayer(const FrameBuilder& frame, AnyElement element, const FrameHooks* hooks) {
	return hooks != nullptr && hooks->layer != nullptr ? hooks->layer(frame, element) : 0;
}

} // namespace

bool ArrangeChild(FrameBuilder& frame, AnyElement element, const Rect& rect) {
	FrameView::ElementLayout* const layout = frame.LayoutOf(element);
	if (layout == nullptr || !layout->is_measured) {
		return false;
	}
	layout->arranged = rect;
	if (layout->collapsed) {
		// Absent, so the subtree keeps what it was measured as and is neither painted nor walked.
		return true;
	}
	const FrameHooks* const hooks  = frame.FrameHooksOf(element);
	uint8_t&                parent = frame.FrameWorkspace<ArrangeLayer>().current;
	layout->layer                  = std::max(parent, AskedLayer(frame, element, hooks));
	if (hooks == nullptr || hooks->arrange == nullptr) {
		return true;
	}
	// The children are arranged on this element's layer, then the parent's is put back for the
	// element's later siblings. The frame keeps each workspace in place, so the reference still
	// holds after the hook.
	const uint8_t outer = parent;
	parent              = layout->layer;
	const bool placed   = hooks->arrange(frame, element, rect);
	parent              = outer;
	return placed;
}

bool Arrange(AnyElement root, const Rect& rect) {
	FrameBuilder& frame = CurrentBuilder();
	if (FrameView::ElementLayout* const layout = frame.LayoutOf(root)) {
		layout->collapsed = false;
	}
	// A root sits on the layer it asks for, whatever is being arranged around the call, and what
	// was there is put back after, so an Arrange made inside a hook leaves that hook's layer as it
	// found it.
	uint8_t&      current = frame.FrameWorkspace<ArrangeLayer>().current;
	const uint8_t outer   = current;
	current               = 0;
	const bool placed     = ArrangeChild(frame, root, rect);
	current               = outer;
	return placed;
}

} // namespace Libs::Graphics::SystemUi
