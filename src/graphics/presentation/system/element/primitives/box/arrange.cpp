#include "graphics/presentation/system/core/units.h"
#include "graphics/presentation/system/element/frame/frameBuilder.h"
#include "graphics/presentation/system/element/frame/frameLayout.h"
#include "graphics/presentation/system/element/primitives/box/layout.h"
#include "graphics/presentation/system/element/primitives/box/scroll.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace Libs::Graphics::SystemUi {

namespace {

// The flex style a box's layout asks for, with lengths in output pixels.
FlexStyle ToFlexStyle(const BoxLayout& layout) {
	return {.direction = layout.direction,
	        .justify   = layout.justify,
	        .align     = layout.align,
	        .gap       = ToPixels(layout.gap),
	        .padding   = PaddingOf(layout)};
}

// Arranges a stacked box's children, each filling the space inside the padding. A stack never
// collapses a child, so any flag an earlier flex arrange left is cleared.
bool ArrangeStack(FrameBuilder& frame, AnyElement box, const BoxLayout& layout, const Rect& rect) {
	const Rect inner    = rect.Inset(PaddingOf(layout));
	bool       arranged = true;
	for (size_t index = 0; index < frame.ChildCount(box); ++index) {
		const AnyElement child = frame.Child(box, index);
		if (FrameView::ElementLayout* below = frame.LayoutOf(child)) {
			below->collapsed = false;
		}
		arranged = ArrangeChild(frame, child, inner) && arranged;
	}
	return arranged;
}

// Moves the children placed in `rects` back along `box`'s main axis by how far it has scrolled.
// The content runs from the start inside the padding to the end of the furthest child, and the
// viewport is the space inside the padding.
void ScrollChildren(FrameBuilder& frame, AnyElement box, const BoxLayout& layout, const Rect& rect,
                    std::span<Rect> rects) {
	const FlexDirection direction = layout.direction;
	const Rect          inner     = rect.Inset(PaddingOf(layout));
	const float         start     = MainOf(inner.Position(), direction);
	float               end       = start;
	for (const Rect& placed: rects) {
		end = std::max(end, MainOf({placed.Right(), placed.Bottom()}, direction));
	}
	const float offset =
	    ArrangeScroll(frame, box, rect, end - start, MainOf(inner.Size(), direction));
	for (Rect& placed: rects) {
		(direction == FlexDirection::Row ? placed.x : placed.y) -= offset;
	}
}

} // namespace

bool ArrangeBox(FrameBuilder& frame, AnyElement box, const Rect& rect) {
	const BoxProps* props = frame.FindAs<BoxProps>(box);
	if (props == nullptr) {
		return false;
	}
	// Copies, as the frame's tables are only promised to stay put while nothing is added.
	const BoxLayout layout  = props->layout;
	const bool      scrolls = Scrolls(*props);
	// A box that clips hides what is arranged inside it beyond its edge, so the wheel only finds a
	// scrolling box inside it where it shows.
	std::optional<ArrangeClipScope> clip;
	if (props->self.styles.overflow != Overflow::Visible) {
		clip.emplace(frame, rect);
	}
	if (layout.stacked) {
		return ArrangeStack(frame, box, layout, rect);
	}
	const size_t count = frame.ChildCount(box);
	BuildFlexItems(frame, box, layout, true);

	// The rectangles and flags are stacks, so a nested solve lands after this box's and the two
	// never overlap. Children are read by index because the nested solve may grow the storage.
	BoxLayoutWorkspace& work      = frame.Workspace<BoxLayoutWorkspace>();
	const size_t        base      = work.rects.size();
	const size_t        flag_base = work.collapsed.size();
	work.rects.resize(base + count);
	work.collapsed.resize(flag_base + count);
	bool arranged = SolveFlex(
	    rect, ToFlexStyle(layout), work.items, std::span<Rect>(work.rects).subspan(base, count),
	    std::span<uint8_t>(work.collapsed).subspan(flag_base, count), work.scratch);
	if (scrolls) {
		ScrollChildren(
		    frame, box, layout, rect,
		    std::span<Rect>(frame.Workspace<BoxLayoutWorkspace>().rects).subspan(base, count));
	}
	for (size_t index = 0; index < count; ++index) {
		const AnyElement child  = frame.Child(box, index);
		const Rect       placed = frame.Workspace<BoxLayoutWorkspace>().rects[base + index];
		if (FrameView::ElementLayout* below = frame.LayoutOf(child)) {
			below->collapsed =
			    frame.Workspace<BoxLayoutWorkspace>().collapsed[flag_base + index] != 0;
		}
		arranged = ArrangeChild(frame, child, placed) && arranged;
	}
	frame.Workspace<BoxLayoutWorkspace>().rects.resize(base);
	frame.Workspace<BoxLayoutWorkspace>().collapsed.resize(flag_base);
	return arranged;
}

uint8_t LayerBox(const FrameBuilder& frame, AnyElement box) {
	const BoxProps* props = frame.FindAs<BoxProps>(box);
	return props != nullptr ? props->self.styles.layer : 0;
}

} // namespace Libs::Graphics::SystemUi
