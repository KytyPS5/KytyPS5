#ifndef EMULATOR_SRC_GRAPHICS_GUEST_GPU_FRAMEPACER_H_
#define EMULATOR_SRC_GRAPHICS_GUEST_GPU_FRAMEPACER_H_

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace Libs::Graphics {

// How far the guest records ahead of the host GPU, in frames (flips). The GPU-command thread
// counts a frame when it records a flip, the host GPU's completion of that flip retires it, and a
// guest frame boundary (AgcSuspendPoint) waits while the bound is reached. A game calls
// AgcSuspendPoint several times a frame: a wait for the host GPU at each one serializes the
// recording of a frame with its execution.
//
// Recording ahead is safe: guest command memory is read when the GPU thread processes it, and
// what draws read is copied into host caches as they are recorded. The bound keeps host memory
// (submissions, staging, deferred deletions) and input latency in check when the host GPU is
// the slower one.
class FramePacer final {
public:
	explicit FramePacer(uint32_t frames_in_flight): m_bound(std::max(frames_in_flight, 1u)) {}

	// The GPU-command thread recorded a flip.
	void FrameRecorded() {
		std::scoped_lock lock {m_mutex};
		m_in_flight++;
	}

	// The host GPU completed a recorded flip.
	void FrameCompleted() {
		{
			std::scoped_lock lock {m_mutex};
			if (m_in_flight == 0) {
				return;
			}
			m_in_flight--;
		}
		m_room.notify_all();
	}

	// A guest frame boundary: returns once fewer frames than the bound are in flight.
	void WaitForRoom() {
		std::unique_lock lock {m_mutex};
		m_room.wait(lock, [this] { return m_stopped || m_in_flight < m_bound; });
	}

	// Shutdown: the host GPU may complete no more frames, so no boundary waits from now on.
	void Stop() {
		{
			std::scoped_lock lock {m_mutex};
			m_stopped = true;
		}
		m_room.notify_all();
	}

	[[nodiscard]] uint32_t InFlight() {
		std::scoped_lock lock {m_mutex};
		return m_in_flight;
	}

private:
	const uint32_t          m_bound;
	std::mutex              m_mutex;
	std::condition_variable m_room;
	uint32_t                m_in_flight = 0;
	bool                    m_stopped   = false;
};

} // namespace Libs::Graphics

#endif /* EMULATOR_SRC_GRAPHICS_GUEST_GPU_FRAMEPACER_H_ */
