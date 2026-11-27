#include "graphics/presentation/system/gallery/pages/parts/motionTrack.h"

#include "graphics/presentation/system/core.h"
#include "graphics/presentation/system/core/motion.h"
#include "graphics/presentation/system/gallery/components/page.h"

#include <chrono>
#include <cmath>

namespace Libs::Graphics::SystemUi::Gallery {

namespace {

// The motion track and the dot moving along it, in design units.
constexpr Dp TRACK_WIDTH = Dp(400.0f);
constexpr Dp DOT_SIZE    = Dp(16.0f);
// The share of the span the dot can move along kept clear at each end of the track, so an ease that
// overshoots, as Emphasis does by about a tenth, stays on the track both ways.
constexpr float OVERSHOOT_ROOM = 0.12f;

// The current clock in milliseconds since its epoch, in double, as that is too large for a float to
// keep milliseconds.
double NowMs() {
	using Milliseconds = std::chrono::duration<double, std::milli>;
	return std::chrono::duration_cast<Milliseconds>(CurrentClockContext().time.time_since_epoch())
	    .count();
}

// How far into its loop of out, rest, back and rest a specimen moving over `duration_ms` is now,
// in milliseconds.
float LoopElapsed(float duration_ms) {
	const double leg = static_cast<double>(duration_ms + MOTION_HOLD_MS);
	return static_cast<float>(std::fmod(NowMs(), 2.0 * leg));
}

} // namespace

uint64_t LoopFlips(float duration_ms) {
	return static_cast<uint64_t>(NowMs() / static_cast<double>(duration_ms + MOTION_HOLD_MS));
}

bool LoopIsOut(float duration_ms) {
	return LoopElapsed(duration_ms) < duration_ms + MOTION_HOLD_MS;
}

float DotPosition(float duration_ms, Ease ease) {
	const float leg     = duration_ms + MOTION_HOLD_MS;
	const float elapsed = LoopElapsed(duration_ms);
	const bool  out     = elapsed < leg;
	const float moving  = out ? elapsed : elapsed - leg;
	if (CurrentMotionContext().reduced || moving >= duration_ms || duration_ms <= 0.0f) {
		return out ? 1.0f : 0.0f;
	}
	const float eased = ApplyEase(ease, moving / duration_ms);
	return out ? eased : 1.0f - eased;
}

AnyElement TrackRow(const ThemeContext& theme, std::string_view label, float position) {
	const float      span   = (TRACK_WIDTH - DOT_SIZE).value;
	const float      room   = span * OVERSHOOT_ROOM;
	const float      travel = span - 2.0f * room;
	const AnyElement dot =
	    Box({.self = {.layout = {.width = DOT_SIZE, .height = DOT_SIZE},
	                  .styles = {.fill = theme.colors.accent.line, .radius = RADIUS_FULL}}});
	const AnyElement track =
	    Box({.layout = {.align   = Align::Center,
	                    .padding = DpInsets(Dp(), Dp(), Dp(), Dp(room + travel * position))},
	         .self   = {.layout = {.width = TRACK_WIDTH, .height = DOT_SIZE},
	                    .styles = {.fill = theme.colors.surface.inset, .radius = RADIUS_FULL}}},
	        {dot});
	return Box({.layout = {.align = Align::Center}},
	           {CaptionCell(theme, label, TOKEN_NAME_WIDTH, theme.colors.text.secondary), track});
}

} // namespace Libs::Graphics::SystemUi::Gallery
