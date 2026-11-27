#include "graphics/presentation/system/animation/animate.h"

#include "graphics/presentation/system/animation/fade/fadeTracker.h"
#include "graphics/presentation/system/animation/identity.h"
#include "graphics/presentation/system/core/color.h"
#include "graphics/presentation/system/core/context.h"
#include "graphics/presentation/system/element/frame/frameBuilder.h"
#include "graphics/presentation/system/element/frame/frameLayout.h"
#include "graphics/presentation/system/element/frame/framePaint.h"
#include "graphics/presentation/system/element/primitives/box/box.h"
#include "graphics/presentation/system/imgui/draw/shine.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string_view>

namespace Libs::Graphics::SystemUi {

namespace {

// The part an Animate's key is extended by for its animations, so the key stays free for any
// other state kept for it.
constexpr std::string_view ANIMATE_PART = "animate";
// The name unkeyed Animates are numbered under, kept apart from keyed ones as none is made by With.
constexpr std::string_view UNKEYED_ANIMATE = "unkeyed animate";

// How many unkeyed Animates the current frame has built, which numbers the next. It is remade at
// zero every frame.
struct AnimateFrame {
	uint32_t unkeyed = 0;
};

// The identity an Animate's animations are kept by, its key extended for animating, or with no
// key, its place among the unkeyed Animates built so far this frame.
AnimationIdentity AnimateIdentity(FrameBuilder& frame, const Key& key) {
	if (!key.IsNone()) {
		return {key.With(ANIMATE_PART).value};
	}
	return {Key(UNKEYED_ANIMATE, frame.FrameWorkspace<AnimateFrame>().unkeyed++).value};
}

// Measures an Animate as its only child, so wrapping content in one changes nothing about its size.
Measured MeasureAnimate(FrameBuilder& frame, AnyElement animate, Vec2 max) {
	if (frame.ChildCount(animate) == 0) {
		return {};
	}
	const AnyElement child = frame.Child(animate, 0);
	const Vec2       size  = MeasureChild(frame, child, max);
	const auto*      below = frame.LayoutOf(child);
	return {size, below != nullptr ? below->min_content : size};
}

// Gives an Animate's child the whole rectangle the Animate was given, moved by its slide, so the
// child slides while the Animate keeps its place and the content around it stays put.
bool ArrangeAnimate(FrameBuilder& frame, AnyElement animate, const Rect& rect) {
	if (frame.ChildCount(animate) == 0) {
		return true;
	}
	Rect placed = rect;
	if (const AnimateData* data = frame.FindAs<AnimateData>(animate)) {
		placed.x += data->slide_offset.x;
		placed.y += data->slide_offset.y;
	}
	return ArrangeChild(frame, frame.Child(animate, 0), placed);
}

// The animations' opacity multiplied into what the element and its child paint with, as its
// inherit hook. The poses were worked out when it was built, so painting reads no clock.
PaintState InheritAnimate(const FrameBuilder& frame, AnyElement animate, const PaintState& parent) {
	PaintState own = parent;
	if (const AnimateData* data = frame.FindAs<AnimateData>(animate)) {
		// Each clamped on its own, as an overshooting tween must not lift a fading element.
		own.opacity *= ClampUnit(data->fade_pose.alpha) * ClampUnit(data->tween_pose.value);
	}
	return own;
}

// The theme's shine as the drawing layer takes it.
ShineStyle ShineOf(const ThemeContext& theme) {
	return {.color            = theme.colors.effect.shine,
	        .angle_degrees    = theme.shine.angle_degrees,
	        .width_fraction   = theme.shine.width_fraction,
	        .min_width        = theme.shine.min_width.value,
	        .peak_alpha       = theme.shine.peak_alpha,
	        .plateau_fraction = theme.shine.plateau_fraction};
}

// Draws an Animate's shine over its child, once the child has painted, so the band lies over the
// child's fill and content alike. It follows the child's own shape, its corners rounded as the
// child box's are, and square for a child that isn't a box. A state of the box that changes its
// radius isn't followed, as no shining box changes shape on hover.
void PaintAnimateOver(const FrameBuilder& frame, AnyElement animate, ImDrawList& list,
                      const PaintState& state) {
	const AnimateData* data = frame.FindAs<AnimateData>(animate);
	if (data == nullptr || !data->shine_position.has_value() || frame.ChildCount(animate) == 0) {
		return;
	}
	const AnyElement                      child  = frame.Child(animate, 0);
	const FrameView::ElementLayout* const layout = frame.LayoutOf(child);
	if (layout == nullptr) {
		return;
	}
	const BoxProps* box    = frame.FindAs<BoxProps>(child);
	const float     radius = box != nullptr ? box->self.styles.radius.value : 0.0f;
	DrawShine(list, layout->arranged, radius, ShineOf(CurrentThemeContext()), *data->shine_position,
	          state.opacity);
}

// The layer an Animate asks for, its child's, so wrapping a box in an animation doesn't change the
// layer it is drawn on, and the shine drawn over the child is drawn on the child's layer.
uint8_t LayerAnimate(const FrameBuilder& frame, AnyElement animate) {
	if (frame.ChildCount(animate) == 0) {
		return 0;
	}
	const AnyElement        child = frame.Child(animate, 0);
	const FrameHooks* const hooks = frame.FrameHooksOf(child);
	return hooks != nullptr && hooks->layer != nullptr ? hooks->layer(frame, child) : 0;
}

// How Animates are measured, placed, faded and shone on, set on every frame one is built in. An
// Animate draws nothing of its own apart from its shine, which it draws over its child.
constexpr FrameHooks ANIMATE_FRAME_HOOKS {.measure    = &MeasureAnimate,
                                          .arrange    = &ArrangeAnimate,
                                          .inherit    = &InheritAnimate,
                                          .paint_over = &PaintAnimateOver,
                                          .layer      = &LayerAnimate};

// The slides' progress from frame to frame. A slide's entrance is tracked as a fade's is, in a
// tracker of its own, so a slide and a fade on one Animate are each tracked once.
struct SlideTracker {
	FadeTracker entrances;
};

// How far from its place a slide entering from `from` pixels away is at `pose`. Its alpha is how
// far along its eased entrance it is, 0 until it starts to enter and 1 once it is in, and a slide
// never exits, so what remains of the way is one minus that.
Vec2 SlideOffset(Vec2 from, const Pose& pose) {
	const float remaining = 1.0f - ClampUnit(pose.alpha);
	return {from.x * remaining, from.y * remaining};
}

// Works out how far into its place `slide` has moved `data`'s child at `now`, for an Animate whose
// slide moves.
void UpdateSlide(FrameBuilder& frame, AnimateData& data, const Slide& slide,
                 AnimationIdentity identity, TimePoint now) {
	data.slide_pose = frame.Workspace<SlideTracker>().entrances.PoseAt(identity, *data.slide, true,
	                                                                   now, frame.BeginCount());
	const ScaleContext& scale = CurrentScaleContext();
	data.slide_offset =
	    SlideOffset({scale.Px(slide.x.value), scale.Px(slide.y.value)}, data.slide_pose);
}

// The shines' sweeps from frame to frame, tracked as a slide's entrance is, in a tracker of their
// own, so a sweep starts with the fade and slide of the same Animate.
struct ShineTracker {
	FadeTracker entrances;
};

// Works out where `shine` stands across `data`'s child at `now`. A held position stands still. A
// sweep shows its band only while it enters, its eased progress carrying the band across, and
// nothing before it starts or once it is over.
void UpdateShine(FrameBuilder& frame, AnimateData& data, const Shine& shine,
                 AnimationIdentity identity, TimePoint now) {
	if (shine.position.has_value()) {
		data.shine_position = shine.position;
		return;
	}
	if (!data.shine.has_value()) {
		return;
	}
	data.shine_pose = frame.Workspace<ShineTracker>().entrances.PoseAt(identity, *data.shine, true,
	                                                                   now, frame.BeginCount());
	if (data.shine_pose.phase == Phase::Entering) {
		data.shine_position = data.shine_pose.alpha;
	}
}

// Works out the poses of `data`'s animations at the frame's clock from the trackers the frame
// keeps, so each carries on from where it was last frame.
void UpdatePoses(FrameBuilder& frame, AnimateData& data, const AnimateProps& props,
                 AnimationIdentity identity) {
	const TimePoint now = CurrentClockContext().time;
	if (props.fade.has_value()) {
		data.fade_pose = frame.Workspace<FadeTracker>().PoseAt(
		    identity, *data.fade, props.fade->shown, now, frame.BeginCount());
	}
	if (props.slide.has_value() && data.slide.has_value()) {
		UpdateSlide(frame, data, *props.slide, identity, now);
	}
	if (props.shine.has_value()) {
		UpdateShine(frame, data, *props.shine, identity, now);
	}
	if (props.tween.has_value()) {
		const TweenTracks tracks = TracksOf(*props.tween, CurrentThemeContext());
		data.tween_pose = frame.Workspace<TweenTracker>().PoseAt(identity, *props.tween, tracks,
		                                                         now, frame.BeginCount());
	}
}

// The data of `animate`, or null for a none or stale handle.
const AnimateData* DataOf(AnimateElement animate) {
	return CurrentBuilder().Find(animate);
}

// `timeline` with its entrance and exit no longer than the theme's reduced_fade_max, as a fade
// plays with reduced motion.
Timeline Reduced(Timeline timeline, const ThemeContext& theme) {
	const float longest        = theme.motion.reduced_fade_max;
	timeline.enter.duration_ms = std::min(timeline.enter.duration_ms, longest);
	timeline.exit.duration_ms  = std::min(timeline.exit.duration_ms, longest);
	return timeline;
}

// The timelines `props` play in the current context. With reduced motion the fade is shortened
// and the slide and the sweep don't move, so they have none. A held shine has none either, as it
// stands still.
AnimateData TimelinesOf(const AnimateProps& props) {
	const ThemeContext& theme   = CurrentThemeContext();
	const bool          reduced = CurrentMotionContext().reduced;
	AnimateData         data {};
	if (props.fade.has_value()) {
		const Timeline fade = TimelineOf(*props.fade, theme);
		data.fade           = reduced ? Reduced(fade, theme) : fade;
	}
	if (props.slide.has_value() && !reduced) {
		data.slide = TimelineOf(*props.slide, theme);
	}
	if (props.shine.has_value() && !props.shine->position.has_value() && !reduced) {
		data.shine = TimelineOf(*props.shine, theme);
	}
	return data;
}

} // namespace

AnimateElement Animate(const AnimateProps& props, AnyElement child) {
	FrameBuilder& frame = CurrentBuilder();
	frame.SetFrameHooks<AnimateData>(ANIMATE_FRAME_HOOKS);
	const AnimateData data = TimelinesOf(props);
	// It asks its container for what its child asks, so it lays out as the child would.
	const FrameView::ElementMetadata* const below = frame.Find(child);
	const ItemLayout                        item  = below != nullptr ? below->item : ItemLayout {};
	const std::array<AnyElement, 1>         children {child};
	const AnimateElement                    animate = frame.AddNode(data, children, item);
	// Only one really built moves on, so a refused one is not built in this frame and starts again
	// when it next is, as one that wasn't built does.
	if (AnimateData* built = frame.Find(animate)) {
		UpdatePoses(frame, *built, props, AnimateIdentity(frame, props.key));
	}
	return animate;
}

Pose FadePose(AnimateElement animate) {
	const AnimateData* data = DataOf(animate);
	return data != nullptr ? data->fade_pose : Pose {Phase::Gone, 0.0f};
}

float TweenAmount(AnimateElement animate) {
	const AnimateData* data = DataOf(animate);
	return data != nullptr ? data->tween_pose.value : 0.0f;
}

std::optional<float> ShinePosition(AnimateElement animate) {
	const AnimateData* data = DataOf(animate);
	return data != nullptr ? data->shine_position : std::nullopt;
}

bool AnimateNeedsFrames(AnimateElement animate) {
	const AnimateData* data = DataOf(animate);
	if (data == nullptr) {
		return false;
	}
	const bool fading  = data->fade.has_value() && data->fade_pose.NeedsFrames(*data->fade);
	const bool sliding = data->slide.has_value() && data->slide_pose.NeedsFrames(*data->slide);
	const bool shining = data->shine.has_value() && data->shine_pose.NeedsFrames(*data->shine);
	return fading || sliding || shining || data->tween_pose.moving;
}

} // namespace Libs::Graphics::SystemUi
