#include "graphics/presentation/system/animation/scroll/scrollValue.h"

#include "graphics/presentation/system/animation/easing.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace Libs::Graphics::SystemUi {

namespace {

using Milliseconds = std::chrono::duration<float, std::milli>;

// Longer than any scroll takes, so a broken token can't make one move for ever.
constexpr float LONGEST_SCROLL_MS = 60000.0f;

// `track`'s duration when it is a finite number above zero, capped, otherwise zero.
float Duration(const Track& track) {
	return std::isfinite(track.duration_ms) && track.duration_ms > 0.0f
	           ? std::min(track.duration_ms, LONGEST_SCROLL_MS)
	           : 0.0f;
}

} // namespace

ScrollValue::ScrollValue(float value): m_from(std::isfinite(value) ? value : 0.0f), m_to(m_from) {}

void ScrollValue::Retarget(float target, const Track& track, TimePoint now) {
	if (!std::isfinite(target) || target == m_to) {
		return;
	}
	// From where the value is now, so a new notch mid-move carries on from there without a jump,
	// and over the whole track, so every notch moves at the same pace whatever is left.
	m_from    = Value(now);
	m_to      = target;
	m_track   = {Duration(track), track.ease};
	m_changed = now;
}

void ScrollValue::Snap(float value) {
	if (!std::isfinite(value)) {
		return;
	}
	m_from  = value;
	m_to    = value;
	m_track = {};
}

float ScrollValue::Value(TimePoint now) const {
	if (m_track.duration_ms <= 0.0f) {
		return m_to;
	}
	const float elapsed = std::chrono::duration_cast<Milliseconds>(now - m_changed).count();
	// Exactly the target once there, so a resting scroll lands on whole pixels it was sent to.
	if (elapsed >= m_track.duration_ms) {
		return m_to;
	}
	return m_from + (m_to - m_from) * ApplyEase(m_track.ease, elapsed / m_track.duration_ms);
}

bool ScrollValue::Moving(TimePoint now) const {
	return m_track.duration_ms > 0.0f &&
	       std::chrono::duration_cast<Milliseconds>(now - m_changed).count() < m_track.duration_ms;
}

} // namespace Libs::Graphics::SystemUi
