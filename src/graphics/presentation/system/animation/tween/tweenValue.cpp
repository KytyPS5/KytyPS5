#include "graphics/presentation/system/animation/tween/tweenValue.h"

#include "graphics/presentation/system/animation/easing.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace Libs::Graphics::SystemUi {

namespace {

using Milliseconds = std::chrono::duration<float, std::milli>;

// Longer than any control takes to move, so a broken token can't make one move for ever.
constexpr float LONGEST_TWEEN_MS = 60000.0f;

// `value` clamped to 0 to 1, with NaN counting as 0.
float Unit(float value) {
	return std::isnan(value) ? 0.0f : std::clamp(value, 0.0f, 1.0f);
}

// `track`'s duration when it is a finite number above zero, capped, otherwise zero.
float Duration(const Track& track) {
	return std::isfinite(track.duration_ms) && track.duration_ms > 0.0f
	           ? std::min(track.duration_ms, LONGEST_TWEEN_MS)
	           : 0.0f;
}

} // namespace

TweenValue::TweenValue(float value): m_from(Unit(value)), m_to(Unit(value)) {}

void TweenValue::Retarget(float target, const TweenTracks& tracks, TimePoint now,
                          bool whole_track) {
	const float to = Unit(target);
	if (to == m_to) {
		return;
	}
	// Rising means the target went up, such as a control gaining focus, even when an overshoot
	// has carried the value past the new target.
	const Track& track = to > m_to ? tracks.rise : tracks.fall;
	m_from             = Value(now);
	m_to               = to;
	// Only the distance left is covered, in the same share of the track, so turning around half
	// way takes half as long and moves at the track's own pace. An overshoot can leave more than
	// the whole distance, which still takes no longer than the track.
	const float share = whole_track ? 1.0f : std::min(std::fabs(to - m_from), 1.0f);
	m_track           = {Duration(track) * share, track.ease};
	m_changed         = now;
}

float TweenValue::Value(TimePoint now) const {
	if (m_track.duration_ms <= 0.0f) {
		return m_to;
	}
	const float elapsed = std::chrono::duration_cast<Milliseconds>(now - m_changed).count();
	// Exactly the target once there, so a resting tween reports it without rounding.
	if (elapsed >= m_track.duration_ms) {
		return m_to;
	}
	return m_from + (m_to - m_from) * ApplyEase(m_track.ease, elapsed / m_track.duration_ms);
}

bool TweenValue::Moving(TimePoint now) const {
	return m_track.duration_ms > 0.0f &&
	       std::chrono::duration_cast<Milliseconds>(now - m_changed).count() < m_track.duration_ms;
}

} // namespace Libs::Graphics::SystemUi
