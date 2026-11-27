#include "graphics/presentation/system/core/units.h"
#include "graphics/presentation/system/element/frame/frameBuilder.h"
#include "graphics/presentation/system/element/frame/frameLayout.h"
#include "graphics/presentation/system/element/primitives/box/layout.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

namespace Libs::Graphics::SystemUi {

namespace {

constexpr float UNBOUNDED = std::numeric_limits<float>::infinity();

// Every child without a fixed main-axis size gives up length in proportion to its size when its
// row overflows, down to its own minimum, so content never runs past its box.
constexpr float CHILD_SHRINK = 1.0f;

// The space inside a box's padding that its children may use, given the space `max` on offer to
// the box. An explicit size in the box's own `item` replaces the offer on its axis. Infinity stays
// infinity.
Vec2 InnerLimit(const ItemLayout& item, const EdgeInsets& padding, Vec2 max) {
	if (item.width) {
		max.x = ToPixels(*item.width);
	}
	if (item.height) {
		max.y = ToPixels(*item.height);
	}
	return {std::max(0.0f, max.x - padding.left - padding.right),
	        std::max(0.0f, max.y - padding.top - padding.bottom)};
}

// The least main-axis length a child can take. Its own minimum when it sets one, and otherwise
// its min-content, as CSS does with min-width auto, so its content never spills. A maximum below
// the min-content still wins. Flex items and the parent's min-content both use this, so the two
// never disagree about what a child needs.
float EffectiveMinMain(const ItemLayout& item, float min_content_main) {
	if (item.min_main) {
		return ToPixels(*item.min_main);
	}
	const float max_main = item.max_main ? ToPixels(*item.max_main) : UNBOUNDED;
	return std::min(min_content_main, max_main);
}

// The flex item for a child of a container laid out by `container`. Its size is the one given.
// A child that sets its own cross size is not stretched, as in CSS, so a small icon in a tall row
// keeps its size instead of filling the row. A child of a box that `scrolls` neither shrinks nor
// collapses, as the box shows what doesn't fit by scrolling, and it still grows when it fits.
FlexItem ToFlexItem(const BoxLayout& container, const ItemLayout& item, Vec2 size,
                    float min_content_main, bool scrolls) {
	FlexItem flex;
	flex.size       = size;
	flex.grow       = item.grow;
	flex.max_main   = item.max_main ? ToPixels(*item.max_main) : UNBOUNDED;
	flex.min_main   = EffectiveMinMain(item, min_content_main);
	flex.align_self = item.align_self;
	flex.collapses  = item.collapses;
	// A fixed main-axis size is a promise, so such a child keeps it and its siblings shrink
	// instead.
	const bool row            = container.direction == FlexDirection::Row;
	const bool explicit_main  = row ? item.width.has_value() : item.height.has_value();
	flex.shrink               = explicit_main ? 0.0f : CHILD_SHRINK;
	const bool explicit_cross = row ? item.height.has_value() : item.width.has_value();
	if (explicit_cross && flex.align_self.value_or(container.align) == Align::Stretch) {
		flex.align_self = Align::Start;
	}
	if (scrolls) {
		flex.shrink    = 0.0f;
		flex.collapses = false;
	}
	return flex;
}

// What a box's children add up to along the main axis, and the longest of them across it.
struct ChildrenSize {
	float main  = 0.0f;
	float cross = 0.0f;
};

// Measures `child` with no limit on either axis, once per pass, and keeps the result as its
// unbounded size. Keyed by the room across, the memo missed whenever a row and a column
// alternated, and the work doubled at each level of depth, so the room is left out of the key. A
// later pass may offer different space, so a result from an earlier pass is never reused.
void MeasureUnbounded(FrameBuilder& frame, AnyElement child) {
	const uint32_t pass = frame.MeasurePass();
	if (const FrameView::ElementLayout* layout = frame.LayoutOf(child);
	    layout->has_natural && layout->natural_generation == pass) {
		return;
	}
	(void)MeasureChild(frame, child, {UNBOUNDED, UNBOUNDED});
	FrameView::ElementLayout* layout = frame.LayoutOf(child);
	layout->unbounded                = layout->measured;
	layout->has_natural              = true;
	layout->natural_generation       = pass;
}

// Gives every child its natural size, the size it wants with no limit along the main axis. A child
// wider across than the box has room for is measured again at that room, and that is its natural
// size, so wrapped text in a column is as tall as its lines there. The room across a child can
// narrow and widen within a pass, since a box that is measured more than once may be assigned a
// longer main length the second time, which a child inside it takes as its room across. A child
// therefore records the limit it was last measured at, and AssignLengths checks it.
void MeasureNatural(FrameBuilder& frame, AnyElement box, FlexDirection direction, float cross_cap) {
	for (size_t index = 0; index < frame.ChildCount(box); ++index) {
		const AnyElement child = frame.Child(box, index);
		if (frame.LayoutOf(child) == nullptr) {
			continue;
		}
		MeasureUnbounded(frame, child);
		FrameView::ElementLayout* layout = frame.LayoutOf(child);
		layout->natural                  = layout->unbounded;
		if (CrossOf(layout->unbounded, direction) > cross_cap) {
			(void)MeasureChild(frame, child, FromAxes(UNBOUNDED, cross_cap, direction));
			layout          = frame.LayoutOf(child);
			layout->natural = layout->measured;
		}
	}
}

// Which layout an element's subtree already holds, if it is the one for the limit about to be
// assigned. The two differ in where the element's size comes from, since one kept for its exact
// limit has the size of that measure, which can differ from its natural size when its length was
// clamped.
enum class Held : uint8_t {
	No,
	Exact,
	Natural,
};

// Whether an element's subtree already holds the layout for the limit it is about to be assigned.
// That is so when it was last measured at exactly that limit, or when its length is unchanged and
// it was last measured as MeasureNatural measures it, with no limit along the main axis and the
// room across only when it didn't fit. A subtree last measured at some other room across holds
// sizes for that room, even when the element's own natural size looks right.
Held HoldsLayout(const FrameView::ElementLayout& layout, FlexDirection direction, float final_main,
                 float cross_cap) {
	const Vec2 assigned = FromAxes(final_main, cross_cap, direction);
	if (layout.last_limit.x == assigned.x && layout.last_limit.y == assigned.y) {
		return Held::Exact;
	}
	if (final_main != MainOf(layout.natural, direction)) {
		return Held::No;
	}
	const float natural_cross =
	    CrossOf(layout.unbounded, direction) > cross_cap ? cross_cap : UNBOUNDED;
	const Vec2 natural_limit = FromAxes(UNBOUNDED, natural_cross, direction);
	const bool natural =
	    layout.last_limit.x == natural_limit.x && layout.last_limit.y == natural_limit.y;
	return natural ? Held::Natural : Held::No;
}

// Measures one child again at the main length the solver gave it, unless its subtree already holds
// that layout, and returns the cross size it takes there. The child then takes exactly the length
// it was assigned, whatever it would have chosen.
float AssignOne(FrameBuilder& frame, AnyElement child, FlexDirection direction, float final_main,
                float cross_cap) {
	const FrameView::ElementLayout& held_layout = *frame.LayoutOf(child);
	Vec2                            assigned    = held_layout.natural;
	const Held held = HoldsLayout(held_layout, direction, final_main, cross_cap);
	if (held == Held::No) {
		assigned = MeasureChild(frame, child, FromAxes(final_main, cross_cap, direction));
	} else if (held == Held::Exact) {
		// The result of that exact measure, since a clamped length can wrap the element taller
		// than its natural size.
		assigned = held_layout.measured;
	}
	frame.LayoutOf(child)->measured = FromAxes(final_main, CrossOf(assigned, direction), direction);
	return CrossOf(assigned, direction);
}

// Gives each child its final main length from the flex solver and measures it again at that
// length, so a child that grew or shrank can lay out its own content in the new space. A child
// that already holds the layout for its limit keeps it, which spares nested boxes a second pass.
// The result is the children's lengths added up, with the longest cross size.
ChildrenSize AssignLengths(FrameBuilder& frame, AnyElement box, const BoxLayout& layout,
                           float inner_main, float cross_cap) {
	const FlexDirection direction = layout.direction;
	const float         gap       = ToPixels(layout.gap);
	const size_t        count     = frame.ChildCount(box);
	BuildFlexItems(frame, box, layout, false);
	BoxLayoutWorkspace& work      = frame.Workspace<BoxLayoutWorkspace>();
	const size_t        base      = work.lengths.size();
	const size_t        flag_base = work.collapsed.size();
	work.lengths.resize(base + count);
	work.collapsed.resize(flag_base + count);
	(void)FlexMainLengths(
	    inner_main, direction, gap, work.items, std::span<float>(work.lengths).subspan(base, count),
	    std::span<uint8_t>(work.collapsed).subspan(flag_base, count), work.scratch);
	// The gaps are between the children the solver left visible, and it says which it collapsed.
	// Every child it didn't collapse counts, as the solver places a gap beside each of them.
	ChildrenSize size;
	size_t       hidden = 0;
	for (size_t index = 0; index < count; ++index) {
		// Read by index, as the nested measures below may grow the stacks.
		if (frame.Workspace<BoxLayoutWorkspace>().collapsed[flag_base + index] != 0) {
			++hidden;
		}
		const AnyElement child = frame.Child(box, index);
		if (frame.LayoutOf(child) == nullptr) {
			continue;
		}
		const float final_main = frame.Workspace<BoxLayoutWorkspace>().lengths[base + index];
		if (frame.Workspace<BoxLayoutWorkspace>().collapsed[flag_base + index] != 0) {
			// A collapsed child is absent. It keeps the cross size it naturally has, for its
			// rectangle, but it isn't measured again at no length, and it doesn't count toward
			// the box's cross size.
			FrameView::ElementLayout* collapsed = frame.LayoutOf(child);
			collapsed->measured = FromAxes(0.0f, CrossOf(collapsed->natural, direction), direction);
			continue;
		}
		size.cross =
		    std::max(size.cross, AssignOne(frame, child, direction, final_main, cross_cap));
		size.main += final_main;
	}
	const size_t visible = count - hidden;
	size.main += visible > 1 ? gap * static_cast<float>(visible - 1) : 0.0f;
	frame.Workspace<BoxLayoutWorkspace>().lengths.resize(base);
	frame.Workspace<BoxLayoutWorkspace>().collapsed.resize(flag_base);
	return size;
}

// What a box's children need at least, along its main axis and across it. A child contributes
// its own minimum when it sets one, and its min-content otherwise. A child that collapses is
// counted as collapsed, since the box can always be as short as that, so it adds no length along
// the main axis and no gap beside it. It still counts across, since a collapsed child is as wide or
// tall as it was in the other direction.
Vec2 ChildrenMinContent(FrameBuilder& frame, AnyElement box, const BoxLayout& layout) {
	const FlexDirection direction = layout.direction;
	float               main      = 0.0f;
	float               cross     = 0.0f;
	size_t              counted   = 0;
	for (size_t index = 0; index < frame.ChildCount(box); ++index) {
		const AnyElement                  child    = frame.Child(box, index);
		const FrameView::ElementLayout*   measured = frame.LayoutOf(child);
		const FrameView::ElementMetadata* metadata = frame.Find(child);
		if (measured == nullptr || metadata == nullptr) {
			continue;
		}
		cross = std::max(cross, CrossOf(measured->min_content, direction));
		if (metadata->item.collapses) {
			continue;
		}
		main += EffectiveMinMain(metadata->item, MainOf(measured->min_content, direction));
		++counted;
	}
	if (counted > 1) {
		main += ToPixels(layout.gap) * static_cast<float>(counted - 1);
	}
	return FromAxes(main, cross, direction);
}

// Adds a box's padding to a size, and replaces an axis with the explicit size the box's own `item`
// gives it.
Vec2 OuterSize(const ItemLayout& item, const EdgeInsets& padding, Vec2 inner) {
	Vec2 outer {inner.x + padding.left + padding.right, inner.y + padding.top + padding.bottom};
	if (item.width) {
		outer.x = ToPixels(*item.width);
	}
	if (item.height) {
		outer.y = ToPixels(*item.height);
	}
	return outer;
}

// Measures a stacked box, whose children share its inner rectangle. Each child is measured in the
// space inside the padding, and the box is the largest of them plus the padding, as is its least
// size. An explicit size on an axis replaces either, as for any box.
Measured MeasureStack(FrameBuilder& frame, AnyElement box, const ItemLayout& item,
                      const EdgeInsets& padding, Vec2 inner) {
	Vec2 size;
	Vec2 min;
	for (size_t index = 0; index < frame.ChildCount(box); ++index) {
		const AnyElement child = frame.Child(box, index);
		const Vec2       found = MeasureChild(frame, child, inner);
		size                   = {std::max(size.x, found.x), std::max(size.y, found.y)};
		if (const FrameView::ElementLayout* measured = frame.LayoutOf(child)) {
			min = {std::max(min.x, measured->min_content.x),
			       std::max(min.y, measured->min_content.y)};
		}
	}
	return {OuterSize(item, padding, size), OuterSize(item, padding, min)};
}

} // namespace

void BuildFlexItems(FrameBuilder& frame, AnyElement box, const BoxLayout& layout,
                    bool use_measured_cross) {
	const BoxProps*        props   = frame.FindAs<BoxProps>(box);
	const bool             scrolls = props != nullptr && Scrolls(*props);
	std::vector<FlexItem>& items   = frame.Workspace<BoxLayoutWorkspace>().items;
	items.clear();
	for (size_t index = 0; index < frame.ChildCount(box); ++index) {
		const AnyElement                  child    = frame.Child(box, index);
		const FrameView::ElementLayout*   measured = frame.LayoutOf(child);
		const FrameView::ElementMetadata* metadata = frame.Find(child);
		if (measured == nullptr || metadata == nullptr) {
			items.emplace_back();
			continue;
		}
		const Vec2 cross_source = use_measured_cross ? measured->measured : measured->natural;
		const Vec2 size         = FromAxes(MainOf(measured->natural, layout.direction),
		                                   CrossOf(cross_source, layout.direction), layout.direction);
		items.push_back(ToFlexItem(layout, metadata->item, size,
		                           MainOf(measured->min_content, layout.direction), scrolls));
	}
}

Measured MeasureBox(FrameBuilder& frame, AnyElement box, Vec2 max) {
	const BoxProps* props = frame.FindAs<BoxProps>(box);
	if (props == nullptr) {
		return {};
	}
	// Copies, as the frame's tables are only promised to stay put while nothing is added.
	const BoxLayout  layout  = props->layout;
	const ItemLayout item    = props->self.layout;
	const bool       scrolls = Scrolls(*props);
	const EdgeInsets padding = PaddingOf(layout);
	const Vec2       inner   = InnerLimit(item, padding, max);
	if (layout.stacked) {
		return MeasureStack(frame, box, item, padding, inner);
	}
	const float cross_cap  = CrossOf(inner, layout.direction);
	const float inner_main = MainOf(inner, layout.direction);
	MeasureNatural(frame, box, layout.direction, cross_cap);
	const ChildrenSize content      = AssignLengths(frame, box, layout, inner_main, cross_cap);
	const Vec2         children_min = ChildrenMinContent(frame, box, layout);
	// A scroller is only as long as the room it has, an unlimited room giving its whole content,
	// and needs no length of its own beyond its padding, as what doesn't fit scrolls.
	const float main = scrolls ? std::min(content.main, inner_main) : content.main;
	const Vec2  min  = scrolls
	                       ? FromAxes(0.0f, CrossOf(children_min, layout.direction), layout.direction)
	                       : children_min;
	const Vec2  size = FromAxes(main, content.cross, layout.direction);
	return {OuterSize(item, padding, size), OuterSize(item, padding, min)};
}

} // namespace Libs::Graphics::SystemUi
