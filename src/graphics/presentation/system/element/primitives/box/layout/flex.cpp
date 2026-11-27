#include "graphics/presentation/system/element/primitives/box/layout/flex.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

namespace Libs::Graphics::SystemUi {

namespace {

constexpr float HALF = 0.5f;

// A length along the main axis for a direction.
float Main(Vec2 size, FlexDirection direction) {
	return direction == FlexDirection::Row ? size.x : size.y;
}

// A length along the cross axis for a direction.
float Cross(Vec2 size, FlexDirection direction) {
	return direction == FlexDirection::Row ? size.y : size.x;
}

// `value` when it is finite and not negative, otherwise zero.
float NonNegative(float value) {
	return std::isfinite(value) && value > 0.0f ? value : 0.0f;
}

// An item with every number made safe to lay out. Sizes, minimums and grow factors that are
// negative or not finite become zero. A NaN maximum means no maximum, and one below the minimum
// becomes the minimum, so the minimum always wins.
FlexItem Sanitized(const FlexItem& item) {
	FlexItem safe = item;
	safe.size     = {NonNegative(item.size.x), NonNegative(item.size.y)};
	safe.grow     = NonNegative(item.grow);
	safe.shrink   = NonNegative(item.shrink);
	safe.min_main = NonNegative(item.min_main);
	const float max =
	    std::isnan(item.max_main) ? std::numeric_limits<float>::infinity() : item.max_main;
	safe.max_main = std::max(max, safe.min_main);
	return safe;
}

// An item's preferred main-axis length within its limits.
float BaseLength(const FlexItem& item, FlexDirection direction) {
	return std::clamp(Main(item.size, direction), item.min_main, item.max_main);
}

// The free space left after the frozen items' lengths and the other items' base lengths.
float FreeSpace(std::span<const FlexItem> items, FlexDirection direction, float available,
                std::span<const float> lengths, std::span<const uint8_t> frozen) {
	float free = available;
	for (size_t i = 0; i < items.size(); i++) {
		free -= frozen[i] != 0 ? lengths[i] : BaseLength(items[i], direction);
	}
	return free;
}

// Takes the overflow back from the items that can shrink, in proportion to their length times
// their shrink factor. An item that would go below its minimum stops at it and drops out, and the
// rest give up the remainder, so a minimum never leaves the row overflowing when others could
// have shrunk. `lengths` holds the result, and `frozen` is working space.
void ShrinkLengths(std::span<const FlexItem> items, FlexDirection direction, float available,
                   std::span<float> lengths, std::span<uint8_t> frozen) {
	for (size_t i = 0; i < items.size(); i++) {
		frozen[i] = items[i].shrink > 0.0f ? 0 : 1;
	}
	// Each pass freezes an item or finishes, so one pass per item is enough.
	for (size_t pass = 0; pass <= items.size(); pass++) {
		float overflow = -available;
		float scaled   = 0.0f;
		// Unfrozen items restart from their base each pass, since a freeze changes everyone's
		// share.
		for (size_t i = 0; i < items.size(); i++) {
			const float base = BaseLength(items[i], direction);
			overflow += frozen[i] != 0 ? lengths[i] : base;
			scaled += frozen[i] != 0 ? 0.0f : items[i].shrink * base;
			lengths[i] = frozen[i] != 0 ? lengths[i] : base;
		}
		if (overflow <= 0.0f || scaled <= 0.0f) {
			break;
		}
		bool floored = false;
		for (size_t i = 0; i < items.size(); i++) {
			if (frozen[i] != 0) {
				continue;
			}
			const float base = lengths[i];
			lengths[i]       = base - overflow * items[i].shrink * base / scaled;
			if (lengths[i] < items[i].min_main) {
				lengths[i] = items[i].min_main;
				frozen[i]  = 1;
				floored    = true;
			}
		}
		if (!floored) {
			break;
		}
	}
}

// Empties the items that collapse when the room is short, in place, sets one flag per item to say
// which they were, and returns how many. A collapsing item leaves when what `available` has left
// after it is less than the other items' minimums together, so the others have all of it.
// `available` is what is left after every gap, which is the conservative reading, since a collapsed
// item gives its gap back only afterwards. An unlimited `available` never collapses anything.
size_t CollapseShort(std::span<FlexItem> items, FlexDirection direction, float available,
                     std::span<uint8_t> collapsed) {
	std::fill(collapsed.begin(), collapsed.end(), uint8_t {0});
	if (!std::isfinite(available) ||
	    std::none_of(items.begin(), items.end(),
	                 [](const FlexItem& item) { return item.collapses; })) {
		return 0;
	}
	float collapsing = 0.0f;
	float others_min = 0.0f;
	for (const FlexItem& item: items) {
		if (item.collapses) {
			collapsing += BaseLength(item, direction);
		} else {
			others_min += item.min_main;
		}
	}
	if (collapsing <= 0.0f || available - collapsing >= others_min) {
		return 0;
	}
	size_t count = 0;
	for (size_t i = 0; i < items.size(); i++) {
		if (items[i].collapses && BaseLength(items[i], direction) > 0.0f) {
			collapsed[i]      = 1;
			items[i].min_main = 0.0f;
			items[i].max_main = 0.0f;
			items[i].grow     = 0.0f;
			items[i].shrink   = 0.0f;
			count++;
		}
	}
	return count;
}

// How many gaps separate the visible items, one between each two and none when there is at most
// one. A collapsed item has no gap beside it.
size_t VisibleGaps(size_t hidden, size_t count) {
	const size_t visible = count - hidden;
	return visible > 0 ? visible - 1 : 0;
}

// Shares `available` main-axis space between the items, writing their lengths. Growing items split
// the free space by their grow factors. One that reaches its maximum keeps it and drops out, and
// the rest is shared again among the others, so a capped item never wastes space another could use.
// `frozen` is working space.
void MainLengths(std::span<const FlexItem> items, FlexDirection direction, float available,
                 std::span<float> lengths, std::span<uint8_t> frozen) {
	for (size_t i = 0; i < items.size(); i++) {
		lengths[i] = BaseLength(items[i], direction);
		frozen[i]  = items[i].grow > 0.0f ? 0 : 1;
	}
	// With no limit there is nothing to share or to take back.
	if (!std::isfinite(available)) {
		return;
	}
	if (std::accumulate(lengths.begin(), lengths.end(), 0.0f) > available) {
		ShrinkLengths(items, direction, available, lengths, frozen);
		return;
	}
	// Each pass freezes at least one item or finishes, so this ends within one pass per item. A
	// pass that caps an item recomputes every other grower on the next pass, and the free space
	// stays positive, up to float rounding, because a capped item takes less than its share, so no
	// stale length survives.
	for (size_t pass = 0; pass <= items.size(); pass++) {
		const float free = FreeSpace(items, direction, available, lengths, frozen);
		float       grow = 0.0f;
		for (size_t i = 0; i < items.size(); i++) {
			grow += frozen[i] != 0 ? 0.0f : items[i].grow;
		}
		if (grow <= 0.0f || free <= 0.0f) {
			break;
		}
		bool capped = false;
		for (size_t i = 0; i < items.size(); i++) {
			if (frozen[i] != 0) {
				continue;
			}
			lengths[i] = BaseLength(items[i], direction) + free * items[i].grow / grow;
			if (lengths[i] > items[i].max_main) {
				lengths[i] = items[i].max_main;
				frozen[i]  = 1;
				capped     = true;
			}
		}
		if (!capped) {
			break;
		}
	}
}

// Where the first item starts on the main axis and the extra space added to each gap, for
// `leftover` space the items didn't take. Nothing moves when the items overflow.
Vec2 JustifyOffsets(Justify justify, float leftover, size_t count) {
	if (leftover <= 0.0f) {
		return {};
	}
	switch (justify) {
		case Justify::Center: return {leftover * HALF, 0.0f};
		case Justify::End: return {leftover, 0.0f};
		case Justify::SpaceBetween:
			return count > 1 ? Vec2 {0.0f, leftover / static_cast<float>(count - 1)} : Vec2 {};
		case Justify::Start: break;
	}
	return {};
}

// An item's cross-axis offset and length in a cross axis `available` long. Like the main axis,
// an item longer than the cross axis starts at its start whatever the alignment.
Vec2 CrossPlacement(Align align, float length, float available) {
	const float spare = std::max(0.0f, available - length);
	switch (align) {
		case Align::Center: return {spare * HALF, length};
		case Align::End: return {spare, length};
		case Align::Stretch: return {0.0f, available};
		case Align::Start: break;
	}
	return {0.0f, length};
}

// A rectangle from absolute main and cross-axis edges, each rounded to a whole pixel. Neighbours
// pass the same float for a shared edge, so it rounds the same way for both.
Rect SnappedRect(FlexDirection direction, Vec2 main_edges, Vec2 cross_edges) {
	const float main_start  = SnapPixel(main_edges.x);
	const float main_end    = SnapPixel(main_edges.y);
	const float cross_start = SnapPixel(cross_edges.x);
	const float cross_end   = SnapPixel(cross_edges.y);
	if (direction == FlexDirection::Row) {
		return {main_start, cross_start, main_end - main_start, cross_end - cross_start};
	}
	return {cross_start, main_start, cross_end - cross_start, main_end - main_start};
}

// The main-axis space left for the items after `count` of them are separated by `gap`.
float MainSpace(size_t count, float gap, float inner_main) {
	return inner_main - (count == 0 ? 0.0f : gap * static_cast<float>(count - 1));
}

// Copies `items` into the scratch with every number made safe. The caller's items are left alone.
void PrepareItems(std::span<const FlexItem> items, FlexScratch& scratch) {
	scratch.safe.assign(items.begin(), items.end());
	std::transform(scratch.safe.begin(), scratch.safe.end(), scratch.safe.begin(), Sanitized);
	scratch.frozen.resize(items.size());
}

// Sets `collapsed_out`, when given, from one flag per item, and leaves it empty when none is set.
void ReportCollapsed(std::span<const uint8_t> flags, std::vector<bool>* collapsed_out) {
	if (collapsed_out == nullptr) {
		return;
	}
	collapsed_out->clear();
	if (std::find(flags.begin(), flags.end(), uint8_t {1}) != flags.end()) {
		collapsed_out->assign(flags.begin(), flags.end());
	}
}

} // namespace

bool FlexMainLengths(float inner_main, FlexDirection direction, float gap,
                     std::span<const FlexItem> items, std::span<float> out,
                     std::span<uint8_t> collapsed, FlexScratch& scratch) {
	if (out.size() < items.size() || collapsed.size() < items.size()) {
		return false;
	}
	PrepareItems(items, scratch);
	const std::span<FlexItem> safe   = scratch.safe;
	const float               space  = NonNegative(gap);
	const float               spaced = MainSpace(safe.size(), space, inner_main);
	const size_t hidden = CollapseShort(safe, direction, spaced, collapsed.first(safe.size()));
	const float  available =
        hidden == 0 ? spaced
	                 : inner_main - space * static_cast<float>(VisibleGaps(hidden, safe.size()));
	MainLengths(safe, direction, available, out.first(safe.size()), scratch.frozen);
	return true;
}

bool FlexMainLengths(float inner_main, FlexDirection direction, float gap,
                     std::span<const FlexItem> items, std::span<float> out,
                     std::vector<bool>* collapsed_out) {
	FlexScratch          scratch;
	std::vector<uint8_t> flags(items.size());
	if (!FlexMainLengths(inner_main, direction, gap, items, out, flags, scratch)) {
		return false;
	}
	ReportCollapsed(flags, collapsed_out);
	return true;
}

std::vector<Rect> SolveFlex(const Rect& container, const FlexStyle& style,
                            std::span<const FlexItem> items) {
	std::vector<Rect> rects(items.size());
	// The output is sized to the items, so the span overload can't refuse it.
	(void)SolveFlex(container, style, items, rects);
	return rects;
}

bool SolveFlex(const Rect& container, const FlexStyle& style, std::span<const FlexItem> items,
               std::span<Rect> out, std::span<uint8_t> collapsed, FlexScratch& scratch) {
	if (out.size() < items.size() || collapsed.size() < items.size()) {
		return false;
	}
	PrepareItems(items, scratch);
	scratch.lengths.resize(items.size());
	const std::span<FlexItem> safe      = scratch.safe;
	const std::span<float>    lengths   = scratch.lengths;
	const auto                inner     = container.Inset(style.padding);
	const auto                direction = style.direction;
	const float               gap       = NonNegative(style.gap);
	const float               spaced = MainSpace(safe.size(), gap, Main(inner.Size(), direction));
	const size_t hidden = CollapseShort(safe, direction, spaced, collapsed.first(safe.size()));
	// The gaps are between the visible items only, and a collapsed item takes no share of the
	// space that justify spreads.
	const float available = hidden == 0
	                            ? spaced
	                            : Main(inner.Size(), direction) -
	                                  gap * static_cast<float>(VisibleGaps(hidden, safe.size()));
	MainLengths(safe, direction, available, lengths, scratch.frozen);
	const float used     = std::accumulate(lengths.begin(), lengths.end(), 0.0f);
	const auto  offsets  = JustifyOffsets(style.justify, available - used, safe.size() - hidden);
	const float cross    = Cross(inner.Size(), direction);
	const float cross_at = Cross(inner.Position(), direction);

	// One running edge, so each item starts on exactly the float the previous one ended on.
	float edge = Main(inner.Position(), direction) + offsets.x;
	for (size_t i = 0; i < safe.size(); i++) {
		const float end         = edge + lengths[i];
		const auto  placement   = CrossPlacement(safe[i].align_self.value_or(style.align),
		                                         Cross(safe[i].size, direction), cross);
		const float cross_start = cross_at + placement.x;
		out[i] = SnappedRect(direction, {edge, end}, {cross_start, cross_start + placement.y});
		// A collapsed item sits where the next visible one starts and leaves no gap of its own, so
		// a gap is added only after a visible item.
		if (collapsed[i] == 0) {
			edge = end + gap + offsets.y;
		}
	}
	return true;
}

bool SolveFlex(const Rect& container, const FlexStyle& style, std::span<const FlexItem> items,
               std::span<Rect> out, std::vector<bool>* collapsed_out) {
	FlexScratch          scratch;
	std::vector<uint8_t> flags(items.size());
	if (!SolveFlex(container, style, items, out, flags, scratch)) {
		return false;
	}
	ReportCollapsed(flags, collapsed_out);
	return true;
}

} // namespace Libs::Graphics::SystemUi
