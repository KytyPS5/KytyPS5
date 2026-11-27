#include "graphics/presentation/system/element/primitives/box/scroll.h"

#include "graphics/presentation/system/core/context.h"
#include "graphics/presentation/system/element/frame/frameBuilder.h"

#include <algorithm>
#include <cassert>
#include <climits>
#include <string_view>

namespace Libs::Graphics::SystemUi {

namespace {

// How far one notch of the wheel scrolls, in the theme's body lines, as desktop lists move about
// three lines a notch.
constexpr float LINES_PER_NOTCH = 3.0f;
// How many bits a box's place in its frame takes when ranked for the wheel, with its layer above
// them, so any layer outranks any place.
constexpr uint32_t ORDER_BITS = 32;
static_assert(sizeof(ScrollRecord::order) * CHAR_BIT <= ORDER_BITS);

// The part a box's key is extended by for its scroll position, so the key stays free for any
// other state a box keeps.
constexpr std::string_view SCROLL_PART = "scroll";
// The name unkeyed scrolling boxes are numbered under, kept apart from keyed ones as none is
// made by With.
constexpr std::string_view UNKEYED_SCROLL = "unkeyed scroll";

// The identity a scrolling box's position is kept by, its key extended for scrolling, or with no
// key, its place among the unkeyed scrolling boxes built so far this frame.
AnimationIdentity ScrollIdentity(ScrollFrame& scroll_frame, const Key& key) {
	if (!key.IsNone()) {
		return {key.With(SCROLL_PART).value};
	}
	return {Key(UNKEYED_SCROLL, scroll_frame.unkeyed++).value};
}

// The part of `rect` inside `clip`, empty where they don't meet.
Rect Intersect(const Rect& rect, const Rect& clip) {
	const float left   = std::max(rect.x, clip.x);
	const float top    = std::max(rect.y, clip.y);
	const float right  = std::min(rect.Right(), clip.Right());
	const float bottom = std::min(rect.Bottom(), clip.Bottom());
	return {left, top, std::max(0.0f, right - left), std::max(0.0f, bottom - top)};
}

// The notches of the wheel along a box's main axis, a column taking the vertical wheel and a row
// the horizontal one.
float WheelAlong(Vec2 wheel, FlexDirection direction) {
	return direction == FlexDirection::Column ? wheel.y : wheel.x;
}

// How a scroll eases, starting fast so it keeps up with a wheel that turns every frame, as each
// new notch starts the ease again.
Track ScrollTrack(const ThemeContext& theme) {
	return {theme.motion.base, Ease::Enter};
}

// Sends `value` to `target`, eased, or straight there with reduced motion.
void MoveTo(ScrollValue& value, float target, TimePoint now) {
	if (CurrentMotionContext().reduced) {
		value.Snap(target);
		return;
	}
	value.Retarget(target, ScrollTrack(CurrentThemeContext()), now);
}

// Keeps `value` within content that can scroll `max` pixels. A value already past the end moves
// straight back to it, as the content it showed is gone. A target past it is brought back, and
// rests there at once when the value is already at the end, so a notch at the end starts no move
// that needs frames. A notch past the end while still easing toward it starts the ease again from
// where the value is, which shows no jump.
void KeepWithin(ScrollValue& value, float max, TimePoint now) {
	if (value.Value(now) > max) {
		value.Snap(max);
	} else if (value.Target() > max) {
		if (value.Value(now) >= max) {
			value.Snap(max);
		} else {
			MoveTo(value, max, now);
		}
	}
}

// Keeps `metrics` for painting its box's indicator, replacing what an earlier arrange of the same
// box this frame left.
void RememberMetrics(ScrollFrame& scroll_frame, const ScrollMetrics& metrics) {
	for (ScrollMetrics& kept: scroll_frame.metrics) {
		if (kept.box == metrics.box) {
			kept = metrics;
			return;
		}
	}
	scroll_frame.metrics.push_back(metrics);
}

// The identity `box` was built with this frame, if it scrolls.
std::optional<AnimationIdentity> IdentityOf(const ScrollFrame& scroll_frame, AnyElement box) {
	for (const auto& [built, identity]: scroll_frame.boxes) {
		if (built == box) {
			return identity;
		}
	}
	return std::nullopt;
}

} // namespace

ScrollRecord& ScrollTracker::Visit(AnimationIdentity identity, uint32_t frame,
                                   const std::optional<Vec2>& pointer) {
	if (m_picked != frame) {
		PickWheelTargets(frame, pointer);
	}
	if (ScrollRecord* record = m_records.Visit(identity, frame)) {
		return *record;
	}
	return m_records.Add(identity, frame, ScrollRecord {});
}

bool ScrollTracker::TakesWheel(AnimationIdentity identity, FlexDirection direction) const {
	const std::optional<AnimationIdentity>& taker =
	    direction == FlexDirection::Column ? m_column : m_row;
	return taker.has_value() && *taker == identity;
}

void ScrollTracker::PickWheelTargets(uint32_t frame, const std::optional<Vec2>& pointer) {
	m_picked = frame;
	m_column.reset();
	m_row.reset();
	if (!pointer.has_value()) {
		return;
	}
	const uint32_t previous     = PreviousFrame(frame);
	uint64_t       column_order = 0;
	uint64_t       row_order    = 0;
	// Only a box drawn last frame where the pointer is, with content to scroll, is a candidate.
	// The one drawn on top wins, on the highest layer and then arranged last, so the innermost of
	// nested boxes wins, as it is arranged after the box around it on a layer no lower. The wheel
	// doesn't pass on to an outer box.
	m_records.ForEach([&](AnimationIdentity identity, const ScrollRecord& record) {
		if (record.arranged != previous || record.max <= 0.0f ||
		    !record.visible.Contains(*pointer)) {
			return;
		}
		const bool     column = record.direction == FlexDirection::Column;
		uint64_t&      best   = column ? column_order : row_order;
		const uint64_t rank   = (static_cast<uint64_t>(record.layer) << ORDER_BITS) | record.order;
		if (rank > best) {
			best                        = rank;
			(column ? m_column : m_row) = identity;
		}
	});
}

void UpdateScroll(FrameBuilder& frame, AnyElement box, const BoxProps& props) {
	ScrollFrame&            scroll_frame = frame.FrameWorkspace<ScrollFrame>();
	const AnimationIdentity identity     = ScrollIdentity(scroll_frame, props.key);
	const PointerContext&   pointer      = CurrentPointerContext();
	ScrollTracker&          tracker      = frame.Workspace<ScrollTracker>();
	ScrollRecord&           record = tracker.Visit(identity, frame.BeginCount(), pointer.position);
	scroll_frame.boxes.emplace_back(box, identity);
	const FlexDirection direction = props.layout.direction;
	const float         notches   = WheelAlong(pointer.wheel, direction);
	// The direction it is matched by is last frame's, which the pointer was over.
	const bool takes = notches != 0.0f && tracker.TakesWheel(identity, record.direction);
	record.direction = direction;
	if (!takes) {
		return;
	}
	const ThemeContext& theme = CurrentThemeContext();
	const float step = CurrentScaleContext().Px(LINES_PER_NOTCH * theme.type.body.line.value);
	// A notch toward the start of the content lowers the offset, never below the start. The end
	// is kept when the box is arranged, against this frame's content, so a notch counts in full
	// when the content grew this frame.
	const float target = std::max(0.0f, record.value.Target() - notches * step);
	MoveTo(record.value, target, CurrentClockContext().time);
}

float ArrangeScroll(FrameBuilder& frame, AnyElement box, const Rect& rect, float content,
                    float viewport) {
	ScrollFrame&                           scroll_frame = frame.FrameWorkspace<ScrollFrame>();
	const std::optional<AnimationIdentity> identity     = IdentityOf(scroll_frame, box);
	ScrollRecord* record = identity ? frame.Workspace<ScrollTracker>().Find(*identity) : nullptr;
	if (record == nullptr) {
		return 0.0f;
	}
	const auto  now = CurrentClockContext().time;
	const float max = std::max(0.0f, content - viewport);
	KeepWithin(record->value, max, now);
	record->max = max;
	record->visible =
	    scroll_frame.clips.empty() ? rect : Intersect(rect, scroll_frame.clips.back());
	record->arranged = frame.BeginCount();
	record->order    = ++scroll_frame.arranged;
	// The layer Arrange gave the box before calling this, so the wheel goes to what is drawn on
	// top.
	const FrameView::ElementLayout* const layout = frame.LayoutOf(box);
	record->layer                                = layout != nullptr ? layout->layer : 0;
	const float offset = SnapPixel(std::clamp(record->value.Value(now), 0.0f, max));
	RememberMetrics(scroll_frame, {box, record->direction, content, viewport, offset});
	return offset;
}

std::optional<Rect> ScrollIndicatorRect(const Rect& inside_border, const ScrollMetrics& metrics,
                                        const IndicatorSizes& sizes) {
	const bool  column = metrics.direction == FlexDirection::Column;
	const float max    = metrics.content - metrics.viewport;
	const float edge   = column ? inside_border.height : inside_border.width;
	// The track keeps the inset from both ends, and clear of rounded corners there as far as still
	// leaves a track of the least length. Corners bigger than that, as on a short box rounded
	// fully along its long side, are kept clear only that far, so the indicator pokes into their
	// curve as little as it can rather than not showing, and moves smoothly as the box resizes.
	const float corner = std::clamp((edge - 2.0f * sizes.inset - sizes.min_length) / 2.0f, 0.0f,
	                                std::max(0.0f, sizes.corner));
	const float track  = edge - 2.0f * (sizes.inset + corner);
	if (max <= 0.0f || metrics.content <= 0.0f || track <= 0.0f) {
		return std::nullopt;
	}
	// The share of the content in view, held to the minimum so a long list keeps a thumb that can
	// be seen, and never longer than the track.
	const float length =
	    std::min(track, std::max(sizes.min_length, track * metrics.viewport / metrics.content));
	// The offset can round a little past the end, as it is snapped to whole pixels, so its share
	// is held to the track.
	const float along =
	    sizes.inset + corner + (track - length) * std::clamp(metrics.offset / max, 0.0f, 1.0f);
	if (column) {
		return Rect {inside_border.Right() - sizes.inset - sizes.thickness, inside_border.y + along,
		             sizes.thickness, length};
	}
	return Rect {inside_border.x + along, inside_border.Bottom() - sizes.inset - sizes.thickness,
	             length, sizes.thickness};
}

const ScrollMetrics* FindScrollMetrics(const FrameBuilder& frame, AnyElement box) {
	const ScrollFrame* scroll_frame = frame.FindFrameWorkspace<ScrollFrame>();
	if (scroll_frame == nullptr) {
		return nullptr;
	}
	for (const ScrollMetrics& metrics: scroll_frame->metrics) {
		if (metrics.box == box) {
			return &metrics;
		}
	}
	return nullptr;
}

bool ScrollNeedsFrames(BoxElement box) {
	FrameBuilder&                          frame = CurrentBuilder();
	const std::optional<AnimationIdentity> identity =
	    IdentityOf(frame.FrameWorkspace<ScrollFrame>(), box);
	const ScrollRecord* record =
	    identity ? frame.Workspace<ScrollTracker>().Find(*identity) : nullptr;
	return record != nullptr && record->value.Moving(CurrentClockContext().time);
}

ArrangeClipScope::ArrangeClipScope(FrameBuilder& frame, const Rect& rect): m_frame(frame) {
	std::vector<Rect>& clips = frame.FrameWorkspace<ScrollFrame>().clips;
	clips.push_back(clips.empty() ? rect : Intersect(rect, clips.back()));
	m_depth = clips.size();
}

ArrangeClipScope::~ArrangeClipScope() {
	std::vector<Rect>& clips = m_frame.FrameWorkspace<ScrollFrame>().clips;
	assert(clips.size() == m_depth && "ArrangeClipScopes must end in reverse order");
	clips.pop_back();
}

} // namespace Libs::Graphics::SystemUi
