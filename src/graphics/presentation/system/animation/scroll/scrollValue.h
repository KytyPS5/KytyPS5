#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_ANIMATION_SCROLL_SCROLL_VALUE_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_ANIMATION_SCROLL_SCROLL_VALUE_H_

#include "graphics/presentation/system/animation/timeline.h"
#include "graphics/presentation/system/core/clock.h"

namespace Libs::Graphics::SystemUi {

/// A scroll position in pixels that eases toward a target and can be sent somewhere new at any
/// moment, such as a list scrolled by the wheel. Unlike a tween it is any length, not 0 to 1, and
/// a new target always takes the whole track from wherever the value is, so a notch of the wheel
/// mid-move never jumps. An ease that starts slowly starts slowly again at every new target, so
/// targets that change every frame are best followed with one that starts fast. Like a timeline,
/// it works from absolute times, so a stalled frame never freezes or slows it. Keeping it within
/// the content is the caller's part.
class ScrollValue {
public:
	/// A scroll position resting at `value`, or at 0 when it isn't a finite number.
	explicit ScrollValue(float value = 0.0f);

	/// Starts moving toward `target` at `now`, from the value there, over `track`. A track with no
	/// positive finite duration moves straight there, as reduced motion asks, and one longer than a
	/// minute takes a minute, so a broken token can't hold it moving. Asking again for the
	/// target it already has changes nothing, so a caller can ask every frame, and a target that
	/// isn't a finite number is ignored.
	void Retarget(float target, const Track& track, TimePoint now);

	/// Moves straight to `value` and rests there, as when the content shrinks under it. A value
	/// that isn't a finite number is ignored.
	void Snap(float value);

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

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_ANIMATION_SCROLL_SCROLL_VALUE_H_
