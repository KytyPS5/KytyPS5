#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_ANIMATION_TWEEN_TWEEN_VALUE_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_ANIMATION_TWEEN_TWEEN_VALUE_H_

#include "graphics/presentation/system/animation/timeline.h"
#include "graphics/presentation/system/core/clock.h"

namespace Libs::Graphics::SystemUi {

/// How a tween moves up toward 1 and down toward 0. A focus ring, for one, fades in over 100 ms
/// and out over 60 ms.
struct TweenTracks {
	Track rise;
	Track fall;
};

/// A value that eases toward a target from 0 to 1 and can turn around at any moment, such as a
/// control's focus highlight, its press or a toggle's knob. Turning around starts from wherever
/// the value is, so it never jumps, and covers the rest of the way in the same share of its track
/// as the distance left, never more than the whole track. Like a timeline, it works from absolute
/// times, so a stalled frame never freezes or skips it. The target is always within 0 to 1, but
/// an overshooting ease such as Emphasis takes the value briefly past it, so clamp the value
/// before using it as an opacity.
class TweenValue {
public:
	/// A tween resting at `value`, clamped to 0 to 1.
	explicit TweenValue(float value = 0.0f);

	/// Starts moving toward `target`, clamped to 0 to 1, at `now`, using the rise track when the
	/// target is above the last one and the fall track when it is below. Asking again for the
	/// target it already has changes nothing, so a caller can ask every frame. With `whole_track`,
	/// every move takes the track's whole duration however short, as a tab indicator sliding
	/// between neighbouring tabs does.
	void Retarget(float target, const TweenTracks& tracks, TimePoint now, bool whole_track = false);

	/// The value at `now`.
	[[nodiscard]] float Value(TimePoint now) const;
	/// True while the value is still moving at `now`, so the UI must keep drawing frames.
	[[nodiscard]] bool Moving(TimePoint now) const;
	/// Where the value is heading, or resting.
	[[nodiscard]] float Target() const { return m_to; }

private:
	float     m_from;
	float     m_to;
	Track     m_track;
	TimePoint m_changed;
};

} // namespace Libs::Graphics::SystemUi

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_ANIMATION_TWEEN_TWEEN_VALUE_H_
