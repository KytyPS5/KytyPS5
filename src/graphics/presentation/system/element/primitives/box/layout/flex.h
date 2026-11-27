#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_ELEMENT_PRIMITIVES_BOX_LAYOUT_FLEX_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_ELEMENT_PRIMITIVES_BOX_LAYOUT_FLEX_H_

#include "graphics/presentation/system/common/props/align.h"
#include "graphics/presentation/system/common/props/direction.h"
#include "graphics/presentation/system/common/props/justify.h"
#include "graphics/presentation/system/core/geometry.h"

#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <vector>

// A single-line flex layout. Items are laid out along a main axis, a row or a column, and
// aligned on the cross axis. The box measures its items first, then place them in the
// rectangles this returns.
// Internal to the box primitive, so components don't include it.
namespace Libs::Graphics::SystemUi {

/// How a container lays out its items.
struct FlexStyle {
	FlexDirection direction = FlexDirection::Row;
	Justify       justify   = Justify::Start;
	Align         align     = Align::Start;
	// Space between neighbouring items on the main axis.
	float      gap = 0.0f;
	EdgeInsets padding;
};

/// One item to lay out. Sizes, minimums and grow factors that are negative or not finite count as
/// zero. A NaN maximum means no maximum, and one below the minimum counts as the minimum.
struct FlexItem {
	// The item's preferred width and height.
	Vec2 size;
	// The item's share of free main-axis space, relative to the other items. Zero keeps it at
	// its preferred size.
	float grow = 0.0f;
	// How readily the item gives up length when the items overflow, weighted by its length, so
	// equal factors shrink items in proportion to their sizes, down to their minimums. Zero, the
	// default, keeps an item at its size and lets it run past the container's end.
	float shrink = 0.0f;
	// Limits on the item's main-axis length, applied to its preferred size and when it grows.
	float min_main = 0.0f;
	float max_main = std::numeric_limits<float>::infinity();
	// Overrides the style's cross-axis alignment for this item.
	std::optional<Align> align_self;
	// Gives up all its length along the main axis, and only along it, when the room is short. If
	// what is left for the other items, after the collapsing items take their preferred lengths, is
	// less than the sum of the other items' minimums, every collapsing item takes none and the
	// others have the room. It only applies when the container's length is known. A collapsed item
	// has no gap beside it and takes no share of the space justify spreads, so the gaps lie only
	// between the items that remain, and its empty rectangle sits where the next remaining item
	// starts. One after the last remaining item has no next item, so it sits one gap, plus any
	// space justify puts between items, past that item's end, and can lie outside the container.
	// Across the axis it keeps the cross length it would have had, so a container's minimum across
	// still counts it, but a container that measures its cross size leaves it out. In the element
	// tree a focused interactive child that collapses loses focus, since it is no longer walked or
	// registered.
	bool collapses = false;
};

/// Working space for a solve, which keeps its capacity from one to the next so that a solve of no
/// more items than before allocates nothing. Nothing in it means anything between solves, and a
/// solve never starts another, so one is enough however deeply boxes nest. Items passed to a solve
/// must not be the scratch's own.
struct FlexScratch {
	std::vector<FlexItem> safe;
	std::vector<float>    lengths;
	std::vector<uint8_t>  frozen;
};

/// FlexMainLengths with the caller's storage, which allocates nothing once `scratch` has grown.
/// `collapsed` gets one flag per item, 1 for an item that collapsed and 0 for the rest. Returns
/// false and writes nothing when `out` or `collapsed` is shorter than `items`.
[[nodiscard]] bool FlexMainLengths(float inner_main, FlexDirection direction, float gap,
                                   std::span<const FlexItem> items, std::span<float> out,
                                   std::span<uint8_t> collapsed, FlexScratch& scratch);

/// SolveFlex with the caller's storage, which allocates nothing once `scratch` has grown.
/// `collapsed` gets one flag per item, 1 for an item that collapsed and 0 for the rest. Returns
/// false and writes nothing when `out` or `collapsed` is shorter than `items`.
[[nodiscard]] bool SolveFlex(const Rect& container, const FlexStyle& style,
                             std::span<const FlexItem> items, std::span<Rect> out,
                             std::span<uint8_t> collapsed, FlexScratch& scratch);

/// The main-axis length of each item in a container whose inner main axis is `inner_main` long,
/// written into `out`. These are the lengths SolveFlex gives the same items, since it uses the
/// same code, so a caller can learn what an item will get before it places anything. An
/// `inner_main` that is not finite leaves every item at its preferred length. Returns false and
/// writes nothing when `out` is shorter than `items`. When `collapsed` is given, it is set to which
/// items collapsed, one flag each, or left empty when none did. SolveFlex collapses the same items,
/// and a collapsed item has no length and no gap beside it, so a caller adding up lengths and gaps
/// should use these flags and not work it out from the lengths, which also come out zero for an
/// item that is merely limited to nothing.
[[nodiscard]] bool FlexMainLengths(float inner_main, FlexDirection direction, float gap,
                                   std::span<const FlexItem> items, std::span<float> out,
                                   std::vector<bool>* collapsed = nullptr);

/// Lays `items` out in `container` and returns one rectangle per item, in order, snapped to
/// whole pixels at their edges so neighbours never overlap or leave hairline gaps. Free space
/// goes to growing items first and is placed by `justify` only when none can take it. Items
/// that don't fit shrink by their shrink factors down to their minimums, and whatever still
/// doesn't fit runs past the container's end, as items with no shrink do. Wrapping, reversed
/// order, space-around, margins and a separate flex basis are not supported yet.
[[nodiscard]] std::vector<Rect> SolveFlex(const Rect& container, const FlexStyle& style,
                                          std::span<const FlexItem> items);

/// The same layout written into `out`, one rectangle per item, for callers that keep their own
/// storage. Returns false and writes nothing when `out` is shorter than `items`. A longer `out`
/// keeps its extra elements as they were. When `collapsed` is given it is set to which items
/// collapsed, as FlexMainLengths reports them, or left empty when none did.
[[nodiscard]] bool SolveFlex(const Rect& container, const FlexStyle& style,
                             std::span<const FlexItem> items, std::span<Rect> out,
                             std::vector<bool>* collapsed = nullptr);

} // namespace Libs::Graphics::SystemUi

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_ELEMENT_PRIMITIVES_BOX_LAYOUT_FLEX_H_
