#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_READBACKPLAN_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_READBACKPLAN_H_

#include <cstdint>

// How the bytes the host GPU wrote into a guest buffer get back into guest memory when the guest
// touches their page.
//
// The general way is a copy command into a download buffer and a wait for everything recorded so
// far. That wait stops the GPU thread for as long as the host GPU needs for the whole frame up to
// that point, and a game can ask for it many times per frame.
//
// Where the CPU can read the buffer's memory, no copy is needed: the bytes are final once the
// last GPU-side write of the buffer has finished. So the read-back waits for that one submission
// only, and whoever asked does the waiting. A guest thread that faulted waits itself, and the GPU
// thread goes on recording. The GPU thread only does what must be done there: submit, when the
// write still sits in the open command buffer, and publish, when the write has finished.
//
// One exception: a buffer the GPU writes again in every submission would keep a waiting caller
// asking without end. After READBACK_MAX_CALLER_WAITS waits the GPU thread waits instead, which
// always ends (ReadbackCallerCanWait).

namespace Libs::Graphics {

struct ReadbackState {
	// The CPU can read the buffer's memory.
	bool cpu_readable = false;
	// The submission that holds the last GPU-side write of the buffer.
	uint64_t last_write_tick = 0;
	// The submission that is being recorded.
	uint64_t current_tick = 0;
	// The host GPU has finished `last_write_tick`. Never true for the open submission.
	bool last_write_done = false;
	// Whoever asked can wait outside the GPU thread. False for the GPU thread itself, and for a
	// caller that has already waited too often for a buffer that is written again each time.
	bool caller_can_wait = false;
};

enum class ReadbackStep : uint8_t {
	// Copy through the GPU and wait for it: the general way.
	CopyThroughGpu,
	// Copy the bytes from the buffer's memory to guest memory now.
	Publish,
	// Return `tick` to the caller. The caller waits for it and asks again.
	CallerWaits,
	// Wait for `tick` on the GPU thread, then publish.
	WaitThenPublish,
};

struct ReadbackPlan {
	ReadbackStep step = ReadbackStep::CopyThroughGpu;
	// The open submission holds the last write: submit it before the step.
	bool     submit_first = false;
	uint64_t tick         = 0;

	bool operator==(const ReadbackPlan&) const = default;
};

inline constexpr int READBACK_MAX_CALLER_WAITS = 3;

// Whether the caller of a read-back can wait for it (ReadbackState::caller_can_wait): only off the
// GPU thread, and only for its first READBACK_MAX_CALLER_WAITS waits.
[[nodiscard]] constexpr bool ReadbackCallerCanWait(bool off_gpu_thread, int waits) noexcept {
	return off_gpu_thread && waits < READBACK_MAX_CALLER_WAITS;
}

[[nodiscard]] constexpr ReadbackPlan PlanReadback(const ReadbackState& state) noexcept {
	if (!state.cpu_readable) {
		return {};
	}
	const bool in_open_submission = state.last_write_tick >= state.current_tick;
	if (!in_open_submission && state.last_write_done) {
		return {.step = ReadbackStep::Publish, .tick = state.last_write_tick};
	}
	return {.step = state.caller_can_wait ? ReadbackStep::CallerWaits
	                                      : ReadbackStep::WaitThenPublish,
	        .submit_first = in_open_submission,
	        .tick         = state.last_write_tick};
}

} // namespace Libs::Graphics

#endif /* EMULATOR_SRC_GRAPHICS_HOST_GPU_READBACKPLAN_H_ */
