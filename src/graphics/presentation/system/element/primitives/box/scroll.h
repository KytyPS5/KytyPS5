#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_ELEMENT_PRIMITIVES_BOX_SCROLL_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_ELEMENT_PRIMITIVES_BOX_SCROLL_H_

#include "graphics/presentation/system/animation/identity.h"
#include "graphics/presentation/system/animation/records.h"
#include "graphics/presentation/system/animation/scroll/scrollValue.h"
#include "graphics/presentation/system/core/geometry.h"
#include "graphics/presentation/system/element/element.h"
#include "graphics/presentation/system/element/primitives/box/box.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

// How boxes that scroll keep their place from frame to frame and take the wheel. The tree is built
// again every frame, so the frame keeps a tracker that finds each scrolling box's position by its
// key, or without one, by its build order among the unkeyed scrolling boxes.
// Internal to the box primitive, so components don't include it.
namespace Libs::Graphics::SystemUi {

class FrameBuilder;

/// What the tracker keeps of one scrolling box from frame to frame. The layout fields are from the
/// last frame the box was arranged in, which is what the wheel is matched against, as the pointer
/// moves over what was last drawn.
struct ScrollRecord {
	ScrollValue   value {};
	FlexDirection direction = FlexDirection::Column;
	// How far the box could scroll when last arranged, 0 when its content fit.
	float max = 0.0f;
	// The part of the box that could be seen when last arranged, its rectangle within the clips
	// around it, so a box scrolled out of view of another doesn't take the wheel.
	Rect visible;
	// The frame it was last arranged in, and its place among the scrolling boxes arranged in that
	// frame. A box inside another is arranged after it, so the innermost comes last.
	uint32_t arranged = 0;
	uint32_t order    = 0;
	// The layer it was drawn on when last arranged, so a box drawn over another on a higher layer
	// takes the wheel from it, whatever order the two were built in.
	uint8_t layer = 0;
};

/// Every scrolling box built recently, found by its identity, kept in the frame's workspace.
class ScrollTracker {
public:
	/// The record of the box with `identity`, visited in `frame`, added at rest at the start of its
	/// content when it is new. The first visit of a frame works out which box each axis of the
	/// wheel goes to from the boxes arranged in the frame before, the one drawn on top under
	/// `pointer` that could scroll on that axis. Only scrolling boxes are matched, so one drawn
	/// over by a box that doesn't scroll still takes the wheel. Among overlapping boxes the one on
	/// the highest layer counts as drawn on top, and on one layer the one arranged last, so the
	/// innermost of nested boxes. The layer outranks the arrange order across separate roots too,
	/// though separate Paint calls draw in call order, so give overlapping roots one parent when
	/// the wheel must follow what is drawn on top. The reference is good until the next Visit.
	ScrollRecord& Visit(AnimationIdentity identity, uint32_t frame,
	                    const std::optional<Vec2>& pointer);

	/// The record of the box with `identity` without visiting it, or null when there is none. The
	/// pointer is good until the next Visit.
	[[nodiscard]] ScrollRecord* Find(AnimationIdentity identity) {
		return m_records.Find(identity);
	}

	/// Whether the box with `identity` takes the wheel along `direction` this frame.
	[[nodiscard]] bool TakesWheel(AnimationIdentity identity, FlexDirection direction) const;

private:
	/// Picks the box each axis of the wheel goes to in `frame`.
	void PickWheelTargets(uint32_t frame, const std::optional<Vec2>& pointer);

	AnimationRecords<ScrollRecord> m_records;
	// The frame the boxes taking the wheel were last picked for. Zero is never a frame.
	uint32_t                         m_picked = 0;
	std::optional<AnimationIdentity> m_column;
	std::optional<AnimationIdentity> m_row;
};

/// How a scrolling box was arranged this frame, which its indicator is drawn from. Lengths are
/// pixels along its main axis.
struct ScrollMetrics {
	AnyElement    box;
	FlexDirection direction = FlexDirection::Column;
	float         content   = 0.0f;
	float         viewport  = 0.0f;
	float         offset    = 0.0f;
};

/// What the current frame knows of its scrolling boxes, emptied by every Begin. The boxes built
/// with their identities, so arranging one finds its record, how many unkeyed scrolling boxes have
/// been built, which numbers the next, the clips around the box being arranged, innermost last,
/// how many scrolling boxes have been arranged, and how each was arranged, for painting its
/// indicator.
struct ScrollFrame {
	std::vector<std::pair<AnyElement, AnimationIdentity>> boxes;
	uint32_t                                              unkeyed = 0;
	std::vector<Rect>                                     clips;
	uint32_t                                              arranged = 0;
	std::vector<ScrollMetrics>                            metrics;

	/// Empties it for a new frame, keeping its capacity.
	void clear() {
		boxes.clear();
		unkeyed = 0;
		clips.clear();
		arranged = 0;
		metrics.clear();
	}
};

/// How a scroll indicator is sized, in pixels. The inset is its gap from the edge inside the box's
/// border, and the corner is how far each end of its track keeps clear of a rounded corner.
struct IndicatorSizes {
	float thickness  = 0.0f;
	float inset      = 0.0f;
	float min_length = 0.0f;
	float corner     = 0.0f;
};

/// Where the indicator of a box arranged as `metrics` goes, inside `inside_border`, the box's
/// rectangle within its border, or none when its content fits or there is no room for a track. It
/// runs along the right edge of a column and the bottom edge of a row. Its length is the share of
/// the content in view, never shorter than the minimum, and it moves from the start of its track to
/// the end as the box scrolls from the start of its content to the end. Corners are kept clear only
/// as far as still leaves a track of the least length, so the indicator always shows.
[[nodiscard]] std::optional<Rect> ScrollIndicatorRect(const Rect&           inside_border,
                                                      const ScrollMetrics&  metrics,
                                                      const IndicatorSizes& sizes);

/// How `box` was arranged this frame, if it scrolls and was arranged, for painting it.
[[nodiscard]] const ScrollMetrics* FindScrollMetrics(const FrameBuilder& frame, AnyElement box);

/// Visits the record of `box`, a box that scrolls built in `frame`, found by its key or, without
/// one, by how many unkeyed scrolling boxes were built before it this frame, and moves it by the
/// wheel when it takes the wheel, three of the theme's body lines a notch, eased over the theme's
/// base duration, or straight there with reduced motion. Called by Box.
void UpdateScroll(FrameBuilder& frame, AnyElement box, const BoxProps& props);

/// The offset in whole pixels `box`'s children are moved back by along its main axis, for content
/// `content` long in a viewport `viewport` long, kept within the content. Records where the box
/// was arranged in `rect` for the next frame's wheel. A box that doesn't scroll has none. Called
/// by ArrangeBox. Keeping the position within the content lasts, as a position past the end moves
/// straight back, so a root is arranged once a frame at its final rectangle, never first at a
/// smaller one.
[[nodiscard]] float ArrangeScroll(FrameBuilder& frame, AnyElement box, const Rect& rect,
                                  float content, float viewport);

/// Clips the boxes arranged inside a box that clips to `rect`, within whatever clips it already,
/// while it lives, so the wheel only finds a scrolling box where it can be seen.
class ArrangeClipScope {
public:
	/// Pushes `rect`, within the clip around it, onto `frame`'s arrange clips.
	ArrangeClipScope(FrameBuilder& frame, const Rect& rect);
	/// Pops what the constructor pushed.
	~ArrangeClipScope();

	ArrangeClipScope(const ArrangeClipScope&)            = delete;
	ArrangeClipScope& operator=(const ArrangeClipScope&) = delete;
	ArrangeClipScope(ArrangeClipScope&&)                 = delete;
	ArrangeClipScope& operator=(ArrangeClipScope&&)      = delete;

private:
	FrameBuilder& m_frame;
	// How deep the clip stack was with this scope's clip on it, so ending out of order is caught
	// rather than popping another scope's clip.
	size_t m_depth;
};

} // namespace Libs::Graphics::SystemUi

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_ELEMENT_PRIMITIVES_BOX_SCROLL_H_
