#include "common/gpuStall.h"

#include "common/debugCounters.h"
#include "common/logging/log.h"
#include "common/threads.h"

#include <atomic>
#include <cstdint>
#include <optional>
#include <utility>

namespace Common::GpuStall {

namespace {

struct State {
	Mutex           mutex;
	CondVar         released;
	StallTracker    tracker;
	Clock::duration threshold = GPU_STALL_AUDIO_HOLD_THRESHOLD;
	bool            enabled   = false;
	uint32_t        waiters   = 0;
	// When the first submit of the current stall was held, so a stall that held several audio
	// threads is counted once.
	std::optional<Clock::time_point> first_hold;
};

// Built on first use, so a Scope or audio push during static initialization finds it ready. It is
// never destroyed, so a thread still waiting at process exit cannot touch a dead mutex.
State& GetState() {
	static State& state = *new State;
	return state;
}

// Lock-free copies of the enabled flag and the Scope count, so an audio push with no compile in
// progress returns without touching the mutex.
std::atomic<bool>     g_enabled {false};
std::atomic<uint32_t> g_active_scopes {0};

// A thread that owns a Scope would wait on its own release, so it never waits.
thread_local uint32_t t_owned_scopes = 0;

// Whether this thread's most recent WaitWhileStalled call waited.
thread_local bool t_resumed_from_hold = false;

int64_t ElapsedMs(Clock::time_point since) {
	return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - since).count();
}

} // namespace

void StallTracker::Begin(Clock::time_point start) {
	m_starts.insert(start);
}

void StallTracker::End(Clock::time_point start) {
	const auto found = m_starts.find(start);
	if (found != m_starts.end()) {
		m_starts.erase(found);
	}
}

bool StallTracker::IsStalled(Clock::time_point now, Clock::duration threshold) const {
	return !m_starts.empty() && now - *m_starts.begin() >= threshold;
}

void Configure(bool enabled, Clock::duration threshold) {
	auto& state = GetState();
	{
		LockGuard lock(state.mutex);
		state.enabled   = enabled;
		state.threshold = threshold;
		g_enabled.store(enabled, std::memory_order_relaxed);
	}
	// Waiters recheck the new settings, and a disable lets them all go.
	state.released.SignalAll();
}

Scope::Scope(): m_start(Clock::now()) {
	auto& state = GetState();
	LockGuard lock(state.mutex);
	state.tracker.Begin(m_start);
	g_active_scopes.fetch_add(1, std::memory_order_relaxed);
	t_owned_scopes++;
}

Scope::~Scope() {
	auto&                            state = GetState();
	std::optional<Clock::time_point> stall_first_hold;
	{
		LockGuard lock(state.mutex);
		state.tracker.End(m_start);
		// Overlapping Scopes make one stall, which ends with the last of them.
		if (g_active_scopes.fetch_sub(1, std::memory_order_relaxed) == 1) {
			stall_first_hold = std::exchange(state.first_hold, std::nullopt);
		}
		t_owned_scopes--;
	}
	state.released.SignalAll();
	if (stall_first_hold.has_value()) {
		const auto held_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
		    Clock::now() - *stall_first_hold);
		DebugCounters::Add(DebugCounters::Counter::AudioHolds);
		DebugCounters::Add(DebugCounters::Counter::AudioHoldNs,
		                   static_cast<uint64_t>(held_ns.count()));
	}
}

namespace {

// Waits while a Scope is past the threshold and holding is on, and reports whether it waited.
bool HoldWhileStalled() {
	if (!g_enabled.load(std::memory_order_relaxed) ||
	    g_active_scopes.load(std::memory_order_relaxed) == 0 || t_owned_scopes != 0) {
		return false;
	}
	auto&             state      = GetState();
	bool              held       = false;
	Clock::time_point hold_start = {};
	{
		LockGuard lock(state.mutex);
		// Wakeups can be spurious or periodic, so the condition is checked again after each one.
		while (state.enabled && state.tracker.IsStalled(Clock::now(), state.threshold)) {
			if (!held) {
				held       = true;
				hold_start = Clock::now();
				state.waiters++;
				if (!state.first_hold.has_value()) {
					state.first_hold = hold_start;
				}
			}
			state.released.Wait(&state.mutex);
		}
		if (held) {
			state.waiters--;
		}
	}
	// Logged after unlocking, so slow log output never delays the GPU thread ending its Scope.
	if (held) {
		LOGF("GpuStall: held guest audio for %lld ms while a pipeline compile blocked the GPU\n",
		     static_cast<long long>(ElapsedMs(hold_start)));
	}
	return held;
}

} // namespace

bool WaitWhileStalled() {
	// Set on every call, so only the first submit after a hold is treated as resuming from it.
	t_resumed_from_hold = HoldWhileStalled();
	return t_resumed_from_hold;
}

bool ResumedFromHold() {
	return t_resumed_from_hold;
}

bool IsStalled() {
	if (!g_enabled.load(std::memory_order_relaxed) ||
	    g_active_scopes.load(std::memory_order_relaxed) == 0) {
		return false;
	}
	auto&     state = GetState();
	LockGuard lock(state.mutex);
	return state.enabled && state.tracker.IsStalled(Clock::now(), state.threshold);
}

uint32_t WaiterCount() {
	auto&     state = GetState();
	LockGuard lock(state.mutex);
	return state.waiters;
}

} // namespace Common::GpuStall
