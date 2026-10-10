#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_FRAMESTATS_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_FRAMESTATS_H_

#include <atomic>
#include <chrono>
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

// Wall-clock time spent inside the main host-GPU entry points, so a log shows which one eats the
// frame (printed as PERF3 and replayed when the device is lost).
enum class StatSlotId : uint32_t { DrawIndex, DrawAuto, PrepareDraw, ExecuteDraw, Dispatch, Count };

struct StatSlot {
	std::atomic<uint64_t> calls {0};
	std::atomic<uint64_t> us {0};
};

inline StatSlot g_stat_slots[static_cast<uint32_t>(StatSlotId::Count)];

class ScopedStat {
public:
	explicit ScopedStat(StatSlotId id)
	    : m_slot(g_stat_slots[static_cast<uint32_t>(id)]), m_begin(std::chrono::steady_clock::now()) {}
	~ScopedStat() {
		m_slot.calls.fetch_add(1, std::memory_order_relaxed);
		m_slot.us.fetch_add(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
		                                              std::chrono::steady_clock::now() - m_begin)
		                                              .count()),
		                    std::memory_order_relaxed);
	}
	ScopedStat(const ScopedStat&)            = delete;
	ScopedStat& operator=(const ScopedStat&) = delete;

private:
	StatSlot&                             m_slot;
	std::chrono::steady_clock::time_point m_begin;
};

} // namespace Libs::Graphics

#endif
