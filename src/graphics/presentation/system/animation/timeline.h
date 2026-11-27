#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_ANIMATION_TIMELINE_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_ANIMATION_TIMELINE_H_

#include "graphics/presentation/system/core/clock.h"
#include "graphics/presentation/system/core/theme.h"

#include <cstdint>
#include <limits>
#include <optional>

// How an element enters, stays and leaves. Every pose is computed from absolute times, never by
// adding up frame times, so a game that stalls for seconds during a shader compile can't freeze
// or skip an animation. It resumes at the right point on the next frame. A fade plays its opacity
// along one and a slide its entrance, while scaling waits for paint transforms.
namespace Libs::Graphics::SystemUi {

/// One animated value going from hidden to shown or back. A zero duration is instant.
struct Track {
	float duration_ms = 0.0f;
	Ease  ease        = Ease::Linear;
};

/// The whole life of an element, a fade in and a fade out with an optional wait before and a hold
/// between them.
struct Timeline {
	// A wait before entering, such as a list row's stagger.
	float delay_ms = 0.0f;
	Track enter;
	// How long the element stays fully shown before leaving on its own. Infinite stays until it is
	// dismissed.
	float hold_ms = std::numeric_limits<float>::infinity();
	Track exit;
};

/// Where an element is in its life, in the order it passes through them.
enum class Phase : uint8_t { Waiting, Entering, Shown, Exiting, Gone };

/// True when `timeline` never leaves on its own, because its hold is infinite or too long to
/// represent. Evaluate and Pose::NeedsFrames both use it, so they always agree.
[[nodiscard]] bool HoldsUntilDismissed(const Timeline& timeline);

/// What to draw at one moment, the phase and how opaque the element is, from 0 to 1.
struct Pose {
	Phase phase = Phase::Waiting;
	float alpha = 0.0f;

	/// True while the pose will change without new input, so the UI must keep drawing frames.
	/// Only an element shown until dismissed, or one that is gone, can rest.
	[[nodiscard]] bool NeedsFrames(const Timeline& timeline) const {
		return phase != Phase::Gone && !(phase == Phase::Shown && HoldsUntilDismissed(timeline));
	}
};

/// The pose at `now` of an element that started at `start` and was dismissed at `dismissed`, if it
/// was. A dismissal fades out from wherever the element is, so an interrupted entrance never jumps.
/// A dismissal at or before the moment the entrance begins means it never appears, and one after
/// the element has started leaving on its own changes nothing. Delays and
/// durations that are negative or not finite count as zero, and a hold too long to represent
/// counts as shown until dismissed.
[[nodiscard]] Pose Evaluate(const Timeline& timeline, TimePoint start,
                            std::optional<TimePoint> dismissed, TimePoint now);

/// When `timeline` must have started for its entrance to reach `alpha` at `now`, its delay
/// included, so an entrance can start again from an element's current opacity instead of from
/// nothing. Alpha is held to 0..1, with NaN as 0, and an instant entrance reaches everything at
/// once.
[[nodiscard]] TimePoint EntranceStartFor(const Timeline& timeline, float alpha, TimePoint now);

} // namespace Libs::Graphics::SystemUi

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_ANIMATION_TIMELINE_H_
