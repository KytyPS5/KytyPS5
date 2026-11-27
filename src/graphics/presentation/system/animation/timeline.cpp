#include "graphics/presentation/system/animation/timeline.h"

#include "graphics/presentation/system/animation/easing.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace Libs::Graphics::SystemUi {

namespace {

using Milliseconds = std::chrono::duration<float, std::milli>;

// Longer than any element stays on screen, and short enough that adding it to a clock time never
// overflows. A longer hold counts as shown until dismissed.
constexpr float LONGEST_TIME_MS = 1.0e9f; // about eleven days

// `value` when it is a finite number above zero, otherwise zero. Delays and durations from a
// theme or a caller can't make an element wait, enter or leave forever.
float NonNegativeFinite(float value) {
	return std::isfinite(value) && value > 0.0f ? value : 0.0f;
}

// A clock duration of `ms` milliseconds, which must be within LONGEST_TIME_MS.
ClockContext::Clock::duration ToDuration(float ms) {
	return std::chrono::duration_cast<ClockContext::Clock::duration>(Milliseconds(ms));
}

// Milliseconds from `from` to `to`. The difference is taken in whole clock ticks first, so it
// stays exact however long ago `from` was.
float Elapsed(TimePoint from, TimePoint to) {
	return std::chrono::duration_cast<Milliseconds>(to - from).count();
}

// How long a track runs, with a duration that isn't a positive number counting as none.
float Length(const Track& track) {
	return std::min(NonNegativeFinite(track.duration_ms), LONGEST_TIME_MS);
}

// A track's eased progress `elapsed` milliseconds in. A track without a positive duration has
// already finished.
float Progress(const Track& track, float elapsed) {
	const float duration = Length(track);
	return duration > 0.0f ? ApplyEase(track.ease, elapsed / duration) : 1.0f;
}

// How opaque the element is `elapsed` milliseconds into the entrance. It stays within 0..1 even
// when an overshooting curve drives the fade.
float EnteringAlpha(const Track& enter, float elapsed) {
	return std::clamp(Progress(enter, elapsed), 0.0f, 1.0f);
}

// The hold, with NaN and negative holds counting as no hold.
float HoldMs(const Timeline& timeline) {
	return std::isnan(timeline.hold_ms) ? 0.0f : std::max(0.0f, timeline.hold_ms);
}

// When the exit begins, the earlier of the end of the hold and the dismissal, or never.
std::optional<TimePoint> ExitStart(const Timeline& timeline, TimePoint enter_start,
                                   std::optional<TimePoint> dismissed) {
	std::optional<TimePoint> exit_start = dismissed;
	if (!HoldsUntilDismissed(timeline)) {
		const auto automatic = enter_start + ToDuration(Length(timeline.enter) + HoldMs(timeline));
		exit_start           = dismissed.has_value() ? std::min(*dismissed, automatic) : automatic;
	}
	return exit_start;
}

} // namespace

bool HoldsUntilDismissed(const Timeline& timeline) {
	// Written so a NaN sum also counts as never ending.
	return !(Length(timeline.enter) + HoldMs(timeline) <= LONGEST_TIME_MS);
}

Pose Evaluate(const Timeline& timeline, TimePoint start, std::optional<TimePoint> dismissed,
              TimePoint now) {
	const float delay       = std::min(NonNegativeFinite(timeline.delay_ms), LONGEST_TIME_MS);
	const auto  enter_start = start + ToDuration(delay);
	const auto  exit_start  = ExitStart(timeline, enter_start, dismissed);
	// Dismissed before it ever appeared, or as it was about to, so it never will. A dismissal at
	// the very moment the entrance begins counts, so an element hidden from the start is gone
	// rather than fading out from nothing. Only a dismissal, as an instant entrance with no hold
	// ends its hold at that moment too and should still flash and fade out.
	if (dismissed.has_value() && *dismissed <= enter_start && now >= *dismissed) {
		return {Phase::Gone, 0.0f};
	}
	if (now < enter_start) {
		// Spelled out, since an instant entrance would otherwise count as already shown.
		return {Phase::Waiting, 0.0f};
	}
	if (!exit_start.has_value() || now < *exit_start) {
		const float elapsed = Elapsed(enter_start, now);
		const auto  phase   = elapsed < Length(timeline.enter) ? Phase::Entering : Phase::Shown;
		return {phase, EnteringAlpha(timeline.enter, elapsed)};
	}
	const float into_exit = Elapsed(*exit_start, now);
	if (into_exit >= Length(timeline.exit)) {
		return {Phase::Gone, 0.0f};
	}
	// Fades out from however opaque the element was when its exit began.
	const float from = EnteringAlpha(timeline.enter, Elapsed(enter_start, *exit_start));
	const float fade = Progress(timeline.exit, into_exit);
	return {Phase::Exiting, std::clamp(from * (1.0f - fade), 0.0f, 1.0f)};
}

TimePoint EntranceStartFor(const Timeline& timeline, float alpha, TimePoint now) {
	// Enough halvings to land within a millionth of the entrance, finer than a frame.
	constexpr int STEPS  = 24;
	const float   delay  = std::min(NonNegativeFinite(timeline.delay_ms), LONGEST_TIME_MS);
	const float   wanted = ClampUnit(alpha);
	const float   length = Length(timeline.enter);
	// An instant entrance, or nothing to reach, starts the entrance now.
	if (!(length > 0.0f) || wanted <= 0.0f) {
		return now - ToDuration(delay);
	}
	// Held to 0..1, the theme's curves never fall back on the way in, even the overshooting one,
	// so halving the span finds the earliest time the entrance is at least that opaque. A curve
	// that wobbled would need another search.
	float low  = 0.0f;
	float high = length;
	for (int step = 0; step < STEPS; ++step) {
		const float middle                                             = (low + high) / 2.0f;
		(EnteringAlpha(timeline.enter, middle) >= wanted ? high : low) = middle;
	}
	return now - ToDuration(delay + high);
}

} // namespace Libs::Graphics::SystemUi
