#ifndef KYTY_COMMON_GPU_STALL_H_
#define KYTY_COMMON_GPU_STALL_H_

#include <chrono>
#include <cstdint>
#include <set>

// Holds guest audio output while the GPU command thread is stuck in a long pipeline compile.
// Games that pace cutscenes by their audio clock otherwise skip ahead while no frames are drawn.
namespace Common::GpuStall {

using Clock = std::chrono::steady_clock;

// Compiles shorter than this let audio run on. The host queue hides a hold of about 50 ms at
// most, and most cold compiles in measured games finish under 250 ms, so holding for those would
// add audible dropouts to stalls nobody notices.
inline constexpr std::chrono::milliseconds GPU_STALL_AUDIO_HOLD_THRESHOLD {250};

// Bookkeeping of the stalls in progress, driven by explicit time points so tests can use a fake
// clock. Not thread safe on its own.
class StallTracker {
public:
	/// Records a stall that began at start.
	void Begin(Clock::time_point start);
	/// Removes one stall that began at start.
	void End(Clock::time_point start);
	/// True when the oldest stall in progress has lasted at least threshold.
	[[nodiscard]] bool IsStalled(Clock::time_point now, Clock::duration threshold) const;

private:
	// Ordered so the oldest start is first. Equal starts are interchangeable.
	std::multiset<Clock::time_point> m_starts;
};

/// Turns holding on or off and sets the threshold. Turning it off releases every held thread.
void Configure(bool enabled, Clock::duration threshold = GPU_STALL_AUDIO_HOLD_THRESHOLD);

// Marks the GPU command thread as blocked for its lifetime. Construct one only where that thread
// itself waits, never around work it hands to another thread, or audio would stop while the game
// keeps drawing. A thread that owns a Scope never waits in WaitWhileStalled, so a Scope must be
// destroyed on the thread that created it.
class Scope {
public:
	Scope();
	~Scope();
	Scope(const Scope&)            = delete;
	Scope& operator=(const Scope&) = delete;

private:
	Clock::time_point m_start;
};

/// Blocks the caller while holding is on and a Scope has lasted at least the threshold.
/// Returns true when it waited. Guest audio submit entry points call this before anything else,
/// and must not hold a lock the GPU command thread could need, or the compile could never end.
/// There is no time limit. A compile that never ends has already frozen the game, and stalls
/// of over 30 s are real, so giving up early would bring back the skipped scene. Each call also
/// records whether it waited, which ResumedFromHold reports.
bool WaitWhileStalled();

/// True when this thread's most recent WaitWhileStalled call waited. The first audio submit after a
/// hold finds the host queue drained on purpose, and uses this to keep that out of underrun counts.
[[nodiscard]] bool ResumedFromHold();

/// True when holding is on and a Scope has lasted at least the threshold. Unlike
/// WaitWhileStalled it does not exempt the thread that owns the Scope.
[[nodiscard]] bool IsStalled();

/// Number of threads waiting in WaitWhileStalled, so tests can tell when a waiter is parked.
[[nodiscard]] uint32_t WaiterCount();

} // namespace Common::GpuStall

#endif /* KYTY_COMMON_GPU_STALL_H_ */
