#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_CORE_CLOCK_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_CORE_CLOCK_H_

#include <chrono>

namespace Libs::Graphics::SystemUi {

/// A moment the system UI animates and times interactions by, usually the start of a frame. Every
/// pose is worked out from absolute times rather than by adding up frame times, so a game that
/// stalls for seconds during a shader compile can't freeze or skip an animation. It resumes at the
/// right point on the next frame.
struct ClockContext {
	/// The clock underneath. It is monotonic, so a change to the system time can't make an
	/// animation jump.
	using Clock = std::chrono::steady_clock;

	// The moment this context stands for.
	Clock::time_point time {};

	/// A clock context at this instant. Read it once per frame, so everything drawn in the frame
	/// animates from the same moment.
	[[nodiscard]] static ClockContext Now() { return {Clock::now()}; }

	constexpr bool operator==(const ClockContext&) const = default;
};

/// A moment on the system UI's clock. Value types and services that are handed the time take one,
/// and it is the one name for it, declared here rather than in each class.
using TimePoint = ClockContext::Clock::time_point;

} // namespace Libs::Graphics::SystemUi

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_CORE_CLOCK_H_
