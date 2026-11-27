#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_ANIMATION_ANIMATE_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_ANIMATION_ANIMATE_H_

#include "graphics/presentation/system/animation/fade/fade.h"
#include "graphics/presentation/system/animation/shine/shine.h"
#include "graphics/presentation/system/animation/slide/slide.h"
#include "graphics/presentation/system/animation/timeline.h"
#include "graphics/presentation/system/animation/tween/tween.h"
#include "graphics/presentation/system/animation/tween/tweenTracker.h"
#include "graphics/presentation/system/core/geometry.h"
#include "graphics/presentation/system/core/key.h"
#include "graphics/presentation/system/element/element.h"

#include <cstdint>
#include <optional>

namespace Libs::Graphics::SystemUi {

struct AnimateData;

/// An animation's element, what Animate returns.
using AnimateElement = Element<AnimateData>;

/// The animations to apply to an element. Each is optional, so one Animate can carry any mix.
struct AnimateProps {
	// Fades the element in and out.
	std::optional<Fade> fade;
	// Eases the element's opacity toward a target, multiplied with the fade's.
	std::optional<Tween> tween;
	// Slides the element into its place the first time it is built.
	std::optional<Slide> slide;
	// Sweeps a band of light across the element the first time it is built, drawn over it.
	std::optional<Shine> shine;
	// Tells the animation apart from every other in the frame, so its progress carries on from
	// frame to frame. Keys are frame wide, so the rows of a loop take Key("row", index). Without
	// one an Animate is told apart by how many unkeyed ones were built before it in the frame,
	// which holds while that number and order don't change, so give one to an animation that
	// comes and goes.
	Key key;
};

/// What a built Animate is, the timelines it plays and the poses worked out from the clock when it
/// was built. Without any animation it is fully shown.
struct AnimateData {
	std::optional<Timeline> fade;
	// Fully shown until a fade says otherwise, so one without a fade paints at full opacity and
	// never needs frames.
	Pose fade_pose {Phase::Shown, 1.0f};
	// At 1 and resting until a tween says otherwise, for the same reason.
	TweenPose tween_pose;
	// The slide's timeline when it slides and moves, and how far along its entrance it is, in
	// place until a slide says otherwise. Absent with reduced motion, as it then never moves.
	std::optional<Timeline> slide;
	Pose                    slide_pose {Phase::Shown, 1.0f};
	// How far from its place the child is laid out, in output pixels, worked out when it was built.
	Vec2 slide_offset;
	// The shine's timeline when it sweeps, and how far along its pass it is, done until a shine
	// says otherwise. Absent with reduced motion or a held position, as it then never moves.
	std::optional<Timeline> shine;
	Pose                    shine_pose {Phase::Shown, 1.0f};
	// Where the band stands, from 0 to 1 across the child, worked out when it was built. None
	// draws no band, before the sweep starts, once it is over or with no shine.
	std::optional<float> shine_position;
};

/// `child` with the animations of `props` applied, built in the current frame. It lays out exactly
/// as its child would, asking its container for what the child asks, and keeps that space even
/// when it has faded to nothing or while its child slides in, so the content around it doesn't
/// move. With the player's reduced motion a slide doesn't move, a shine doesn't sweep and a fade
/// is no longer than the theme's reduced_fade_max. It works out how far along each animation is
/// from the frame's clock, and the frame keeps their progress from frame to frame by its key, so
/// the caller keeps nothing. An animation not built in a frame starts again when next built, and
/// the caller stops building a fading one once it is gone, which FadePose tells. It needs an open
/// FrameScope. A refused Animate is a none handle, counts as not built, and a RefusalScope open
/// around the call says why.
[[nodiscard]] AnimateElement Animate(const AnimateProps& props, AnyElement child);

/// Where the fade of `animate` was in its life when it was built, and how opaque, so the caller
/// knows when it has faded out and can stop building it. One without a fade is fully shown, and a
/// none or stale handle is gone. It reads the current frame, so it needs the FrameScope the
/// element was built in.
[[nodiscard]] Pose FadePose(AnimateElement animate);

/// How far the tween of `animate` was toward 1 when it was built, before clamping, so an
/// overshooting ease shows. One without a tween is at 1, and a none or stale handle at 0. It needs
/// the FrameScope the element was built in.
[[nodiscard]] float TweenAmount(AnimateElement animate);

/// Where the shine of `animate` stood across its child when it was built, from 0 to 1, or none
/// when it draws no band, as before its sweep starts, once the sweep is over, with reduced motion
/// and no held position, or with no shine. A none or stale handle has none. It needs the
/// FrameScope the element was built in.
[[nodiscard]] std::optional<float> ShinePosition(AnimateElement animate);

/// Whether any animation of `animate` will change without new input, so the UI must keep drawing
/// frames while it does. A none or stale handle needs none. It needs the FrameScope the element
/// was built in.
[[nodiscard]] bool AnimateNeedsFrames(AnimateElement animate);

} // namespace Libs::Graphics::SystemUi

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_ANIMATION_ANIMATE_H_
