#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_SUBMITCADENCE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_SUBMITCADENCE_H_

#include <chrono>
#include <cstdint>

namespace Libs::Graphics {

// A long command stream recorded into one command buffer leaves the GPU idle until the whole
// stream is recorded. Submitting about this often lets the GPU start on earlier work, while a
// submit costs only tens of microseconds. Measured on LEGO Batman, where recording took 7 to 14 ms.
inline constexpr std::chrono::microseconds SubmitCadenceInterval {1000};
// When this many submits are still running, the GPU already has work queued, so an extra submit
// only adds command buffers in flight without letting the GPU start any sooner.
inline constexpr uint64_t SubmitCadenceMaxGpuLag = 2;

/// Decides when the GPU thread should submit the command buffer it is recording. It holds no
/// Vulkan state, so the policy can be tested without a device. Used on the GPU thread only.
class SubmitCadence {
public:
	using Clock   = std::chrono::steady_clock;
	using NowFunc = Clock::time_point (*)();

	/// Creates a policy that never asks for a submit unless enabled. The interval and the clock
	/// can be replaced by tests.
	explicit SubmitCadence(bool enabled, Clock::duration interval = SubmitCadenceInterval,
	                       NowFunc now = &Clock::now)
	    : m_enabled(enabled), m_interval(interval), m_now(now) {}

	/// Records that a draw or dispatch went into the current command buffer. The clock is read
	/// only while the flag is on and no submit is due yet, so the off path costs one branch. The
	/// interval starts at the first piece of work, so idle time before it does not count. Only
	/// this sets a submit due, so a command buffer with no draw or dispatch never submits early.
	void NoteWork() {
		if (!m_enabled || m_due) {
			return;
		}
		const auto now = m_now();
		if (m_work == 0) {
			m_start = now;
		}
		m_work++;
		m_due = now - m_start >= m_interval;
	}

	/// Returns whether to submit now, given how many submitted command buffers the GPU has not
	/// finished. A true result stays true until Reset, so the caller must submit, which begins a
	/// new command buffer. A due submit is held back while the GPU lags. Holding it clears due but
	/// keeps the start time, so the next draw or dispatch re-arms it at once. The lag is then
	/// checked once per draw rather than at every packet boundary.
	[[nodiscard]] bool ShouldSubmit(uint64_t gpu_lag) {
		if (!m_due) {
			return false;
		}
		if (gpu_lag >= SubmitCadenceMaxGpuLag) {
			m_held_back++;
			m_due = false;
			return false;
		}
		return true;
	}

	/// Returns whether a submit is due, before the GPU lag is checked. A caller uses it to skip
	/// reading the GPU progress when nothing is due.
	[[nodiscard]] bool Due() const { return m_due; }

	/// Starts counting again for a new command buffer.
	void Reset() {
		m_work = 0;
		m_due  = false;
	}

	/// Returns and clears how many times a due submit was held back by GPU lag, for the debug
	/// counters. A long hold counts once per draw or dispatch that re-armed it.
	[[nodiscard]] uint64_t TakeHeldBack() {
		const auto held_back = m_held_back;
		m_held_back          = 0;
		return held_back;
	}

	/// Returns whether the cadence flag was on when the policy was built.
	[[nodiscard]] bool Enabled() const { return m_enabled; }

private:
	bool              m_enabled;
	Clock::duration   m_interval;
	NowFunc           m_now;
	Clock::time_point m_start {};
	uint32_t          m_work      = 0;
	bool              m_due       = false;
	uint64_t          m_held_back = 0;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_SUBMITCADENCE_H_
