#ifndef KYTY_COMMON_DEBUG_COUNTERS_H_
#define KYTY_COMMON_DEBUG_COUNTERS_H_

#include <atomic>
#include <cstdint>

namespace Common::DebugCounters {

// Monotonic event counters and gauges for the performance panel. Updates are relaxed atomics,
// cheap enough for hot paths and safe in signal handlers. Rates come from the reader comparing
// two snapshots.
enum class Counter {
	// GPU work.
	Draws,
	DrawInstances,
	IndirectDraws,
	Dispatches,
	QueueSubmits,
	GpuWaits,     // CPU blocked on a GPU timeline semaphore.
	GpuWaitNs,
	GpuFaults,    // Guest writes to GPU-tracked memory.
	ShadersCompiled,
	ShaderCompileNs,
	PipelinesCreated,
	PipelineCreateNs,
	// Caches.
	TextureUploadBytes,
	TextureDownloadBytes,
	TextureEvictions,
	BufferUploadBytes,
	BufferDownloadBytes,
	BufferEvictions,
	// Presentation.
	FlipsRequested,
	FlipsRejected, // Guest flip queue was full.
	LateVblanks,   // The present thread fell behind the vblank rate.
	SwapchainRecreations,
	DroppedPresents,
	// Emulator.
	GuestThreadsCreated,
	GuestThreadsExited,
	EmulatedInstructions, // Host CPU lacked an instruction the guest used.
	UnresolvedImportCalls,
	GuestFileReadBytes,
	GuestFileWriteBytes,
	AudioUnderruns,
	Count
};

enum class Gauge {
	TextureCacheBytes,
	TextureImages,
	BufferCacheBytes,
	Count
};

namespace Detail {
inline std::atomic<uint64_t> g_counters[static_cast<int>(Counter::Count)] {};
inline std::atomic<uint64_t> g_gauges[static_cast<int>(Gauge::Count)] {};
} // namespace Detail

inline void Add(Counter counter, uint64_t value = 1) noexcept {
	Detail::g_counters[static_cast<int>(counter)].fetch_add(value, std::memory_order_relaxed);
}

inline void Adjust(Gauge gauge, int64_t delta) noexcept {
	Detail::g_gauges[static_cast<int>(gauge)].fetch_add(static_cast<uint64_t>(delta),
	                                                    std::memory_order_relaxed);
}

[[nodiscard]] inline uint64_t Get(Counter counter) noexcept {
	return Detail::g_counters[static_cast<int>(counter)].load(std::memory_order_relaxed);
}

[[nodiscard]] inline uint64_t Get(Gauge gauge) noexcept {
	return Detail::g_gauges[static_cast<int>(gauge)].load(std::memory_order_relaxed);
}

} // namespace Common::DebugCounters

#endif /* KYTY_COMMON_DEBUG_COUNTERS_H_ */
