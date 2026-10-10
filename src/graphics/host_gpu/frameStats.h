#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_FRAMESTATS_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_FRAMESTATS_H_

#include <atomic>
#include <cstdint>

namespace Libs::Graphics {

// Cheap always-on counters printed by the flip path every couple of seconds, so a plain console
// log is enough to tell shader-compilation stalls from steady-state GPU/CPU cost.
struct FrameStats {
	std::atomic<uint64_t> compiles {0};
	std::atomic<uint64_t> compile_us {0};
	std::atomic<uint64_t> compile_us_max {0};
	std::atomic<uint64_t> submits {0};
	std::atomic<uint64_t> submit_batches {0};
	std::atomic<uint64_t> submit_us {0};
	std::atomic<uint64_t> waits {0};
	std::atomic<uint64_t> wait_us {0};
};

inline FrameStats g_frame_stats;

} // namespace Libs::Graphics

#endif
