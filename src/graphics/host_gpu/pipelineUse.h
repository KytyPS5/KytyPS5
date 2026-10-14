#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_PIPELINEUSE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_PIPELINEUSE_H_

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <optional>

// What a draw does when its graphics pipeline is not built yet.
//
// A pipeline is built on a worker thread. With the driver's caches warm that takes about a
// millisecond; on a machine that sees the game for the first time the host has to compile the
// shaders, which takes 100 ms to 2 s per pipeline, and a level asks for several new pipelines
// per second. A draw that waited would stop the picture for each of them.
//
// So a draw that only changes the picture is left out until its pipeline is built: the object or
// effect appears a few frames late, once per pipeline on this machine. A draw that changes more
// than the picture waits: what its shaders write to memory, and the depth clear it carries, are
// read by later work, and leaving them out would change what the game computes.
//
// A picture the game draws once and reads for the rest of the level (a colour table, a light
// probe, an atlas) is the same: left out once, it stays missing. The game's own frames tell the
// two apart: a target drawn in the previous frame as well is drawn again anyway, so a draw into
// it may be left out; a draw into any other target waits.
//
// Leaving a draw out keeps what the target held before it: last frame's picture, when the game
// draws over it. A target the game clears or overwrites (a copy, an upload, a compute write) in
// this frame holds the clear colour instead, and every object left out of it shows as a black
// hole until its pipeline is built. So in a frame in which a target was reset, a draw into it
// waits (TargetHistory), but only within a budget per frame: games clear most targets every
// frame, and at a level start on a machine that sees the game for the first time about 200 new
// pipelines of 30 to 450 ms each are needed one after another. Waiting for all of them stopped
// the game for 10 to 40 s (measured). Past the budget such a draw is left out, and the black hole
// shows until its pipeline is built.
//
// What this cannot see: a draw into one region of a target the game otherwise redraws (that
// region stays missing), and a target the game later copies into memory it reads (the bytes
// miss the draw). Both need knowledge of what the game does next, which the draw does not have.

namespace Libs::Graphics {

struct DrawEffects {
	// A shader of the draw writes buffers or images, or uses atomics on them.
	bool writes_memory = false;
	// The draw clears its depth or stencil target.
	bool clears_depth = false;
	// A target of the draw was not drawn in the previous frame.
	bool writes_fresh_target = false;
	// A target of the draw was cleared or overwritten, other than by a draw, in this frame.
	bool writes_reset_target = false;
};

enum class PipelineUse : uint8_t {
	// The pipeline is built: draw.
	Draw,
	// Wait for the build, then draw.
	Wait,
	// Wait for the build while the frame's budget lasts (FrameWaitBudget), then draw; past the
	// budget, leave the draw out.
	WaitWithinBudget,
	// Leave the draw out; it is drawn again by the game once the pipeline is built.
	LeaveOut,
};

// `always_wait`: every draw waits (--pipeline-wait), so the picture is exact from the first frame.
[[nodiscard]] constexpr PipelineUse PlanPipelineUse(const DrawEffects& effects, bool ready,
                                                    bool always_wait) noexcept {
	if (ready) {
		return PipelineUse::Draw;
	}
	if (always_wait || effects.writes_memory || effects.clears_depth ||
	    effects.writes_fresh_target) {
		return PipelineUse::Wait;
	}
	if (effects.writes_reset_target) {
		return PipelineUse::WaitWithinBudget;
	}
	return PipelineUse::LeaveOut;
}

// The threads that build pipelines on a host with `host_threads` hardware threads. A build is
// mostly the host's shader compiler, which runs builds in parallel; at a level start a game asks
// for hundreds at once. Four threads stay free for the game's threads and the GPU thread, except
// while the program list is translated at a start: its threads come on top (pipelineCache.cpp). Measured
// on an M4 Max (14 threads), building a list of 544 pipelines: 3 threads 8.4 s, 6 threads 6.7 s,
// 10 threads 6.2 s with the compiler's cache warm; past 10 threads nothing was measured.
[[nodiscard]] constexpr uint32_t PipelineBuilderCount(uint32_t host_threads) noexcept {
	constexpr uint32_t Reserved = 4;
	constexpr uint32_t Most     = 10;
	return std::clamp(host_threads > Reserved ? host_threads - Reserved : 1u, 1u, Most);
}

// The pipelines of the game's list built ahead at this start. The window title shows the count
// while they build: the player sees that the start is doing something.
class AheadBuildProgress {
public:
	struct Count {
		uint32_t built = 0;
		uint32_t total = 0;
	};

	void Listed(uint32_t total) noexcept {
		m_built.store(0, std::memory_order_relaxed);
		m_total.store(total, std::memory_order_relaxed);
	}
	// A listed pipeline is built, dropped, or was built already.
	void Built() noexcept { m_built.fetch_add(1, std::memory_order_relaxed); }
	// The count while pipelines of the list are left to build; nothing once all are built.
	[[nodiscard]] std::optional<Count> Building() const noexcept {
		const Count count {.built = m_built.load(std::memory_order_relaxed),
		                   .total = m_total.load(std::memory_order_relaxed)};
		return count.built < count.total ? std::optional(count) : std::nullopt;
	}

private:
	std::atomic<uint32_t> m_built {0};
	std::atomic<uint32_t> m_total {0};
};

// The time draws into reset targets may wait for their pipelines in one frame: one refresh at
// 60 Hz, so such a frame takes at most that much longer.
inline constexpr uint64_t RESET_TARGET_WAIT_BUDGET_US = 16'000;

// A draw that is left out first waits this long for its pipeline: a build the driver has cached
// takes about a millisecond, so on a host that builds fast the picture stays exact.
inline constexpr uint64_t GRACE_WAIT_US = 2'000;
// All such short waits of one frame together.
inline constexpr uint64_t GRACE_WAIT_BUDGET_US = 4'000;

class FrameWaitBudget {
public:
	explicit FrameWaitBudget(uint64_t per_frame_us = RESET_TARGET_WAIT_BUDGET_US) noexcept
	    : m_per_frame(per_frame_us), m_left(per_frame_us) {}

	// Microseconds left to wait in `frame`.
	[[nodiscard]] uint64_t Left(uint64_t frame) noexcept {
		if (frame != m_frame) {
			m_frame = frame;
			m_left  = m_per_frame;
		}
		return m_left;
	}

	// A draw in `frame` waited `us` microseconds.
	void Spend(uint64_t frame, uint64_t us) noexcept {
		const auto left = Left(frame);
		m_left          = left > us ? left - us : 0;
	}

private:
	uint64_t m_per_frame;
	uint64_t m_frame = UINT64_MAX;
	uint64_t m_left;
};

// Microseconds a draw that is left out waits for its pipeline in `frame`. `last_wait_frame`: the
// frame of the pipeline's last such wait. `budget_left`: what is left of GRACE_WAIT_BUDGET_US.
[[nodiscard]] constexpr uint64_t GraceWaitUs(uint64_t last_wait_frame, uint64_t frame,
                                             uint64_t budget_left) noexcept {
	if (last_wait_frame == frame) {
		return 0;
	}
	return std::min(GRACE_WAIT_US, budget_left);
}

// What the renderer remembers of one draw target (an image, mip and layer): the frame of its last
// draw, the image's content generation after that draw, and the frame in which a write other
// than a draw was last seen. The content generation counts every write to the image's contents
// (uploads, copies, clears, compute writes, draws that write depth); a draw into a colour target
// does not change it.
class TargetHistory {
public:
	enum class Seen : uint8_t {
		// Not drawn in this or the previous frame.
		Fresh,
		// Drawn recently, and holds the picture of those draws.
		Drawn,
		// Cleared or overwritten in this frame.
		Reset,
	};

	// The target is about to be drawn into in `frame`; its image's content generation is
	// `generation`.
	[[nodiscard]] Seen Observe(uint64_t generation, uint64_t frame) noexcept {
		// First: a write since the last draw holds for the whole frame, also when this draw
		// waits as a fresh target and its Drawn takes the new generation.
		if (m_drawn && generation != m_generation) {
			m_reset_frame = frame;
		}
		if (!m_drawn || m_drawn_frame + 1 < frame) {
			return Seen::Fresh;
		}
		return m_reset_frame == frame ? Seen::Reset : Seen::Drawn;
	}

	// The target is reset in `frame` by something its content generation does not show yet: CPU
	// writes to its memory, uploaded when the draw acquires the target.
	void MarkReset(uint64_t frame) noexcept { m_reset_frame = frame; }

	// A draw into the target was recorded in `frame`; the content generation after it.
	void Drawn(uint64_t frame, uint64_t generation) noexcept {
		m_drawn       = true;
		m_drawn_frame = frame;
		m_generation  = generation;
	}

private:
	bool     m_drawn       = false;
	uint64_t m_drawn_frame = 0;
	uint64_t m_generation  = 0;
	uint64_t m_reset_frame = UINT64_MAX;
};

} // namespace Libs::Graphics

#endif /* EMULATOR_SRC_GRAPHICS_HOST_GPU_PIPELINEUSE_H_ */
