#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_FRAMESTATS_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_FRAMESTATS_H_

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>

namespace Libs::Graphics {

// Where the CPU blocked waiting for the GPU. A thread tags the wait it is about to make with a
// WaitSiteScope; MasterSemaphore::Wait attributes the blocked time to that tag.
enum class WaitSite : uint32_t {
	Other,
	StreamBuffer,
	BufferDownloadSync,
	BufferRetire,
	FaultManager,
	SwapchainFrame,
	Finish,
	FlushAndWait,
	Count
};

inline const char* WaitSiteName(WaitSite site) {
	static const char* const names[] = {"other",      "stream_buffer", "buffer_download_sync",
	                                    "buffer_retire", "fault_manager", "swapchain_frame",
	                                    "finish",     "flush_and_wait"};
	const auto index = static_cast<size_t>(site);
	return index < (sizeof(names) / sizeof(names[0])) ? names[index] : "?";
}

inline thread_local WaitSite g_wait_site = WaitSite::Other;

class WaitSiteScope {
public:
	explicit WaitSiteScope(WaitSite site, bool only_if_unset = false) : m_previous(g_wait_site) {
		if (!only_if_unset || g_wait_site == WaitSite::Other) {
			g_wait_site = site;
		}
	}
	~WaitSiteScope() { g_wait_site = m_previous; }
	WaitSiteScope(const WaitSiteScope&)            = delete;
	WaitSiteScope& operator=(const WaitSiteScope&) = delete;

private:
	WaitSite m_previous;
};

// Cheap always-on counters printed every couple of seconds by NotePresentedFrame(), so a plain
// console log is enough to tell shader-compilation stalls from CPU<->GPU serialization.
struct FrameStats {
	std::atomic<uint64_t> compiles {0};
	std::atomic<uint64_t> compile_us {0};
	std::atomic<uint64_t> compile_us_max {0};
	std::atomic<uint64_t> draws {0};
	std::atomic<uint64_t> dispatches {0};
	std::atomic<uint64_t> eops {0};

	struct WaitStat {
		std::atomic<uint64_t> count {0};
		std::atomic<uint64_t> us {0};
		std::atomic<uint64_t> us_max {0};
	};
	std::array<WaitStat, static_cast<size_t>(WaitSite::Count)> waits;
};

inline FrameStats g_frame_stats;

inline void NoteGpuWait(uint64_t us) {
	auto& stat = g_frame_stats.waits[static_cast<size_t>(g_wait_site)];
	stat.count.fetch_add(1, std::memory_order_relaxed);
	stat.us.fetch_add(us, std::memory_order_relaxed);
	auto previous = stat.us_max.load(std::memory_order_relaxed);
	while (us > previous && !stat.us_max.compare_exchange_weak(previous, us)) {
	}
}

// Call once per frame that is actually presented (both CPU and GPU-EOP flips end up there).
inline void NotePresentedFrame() {
	static auto     window_start  = std::chrono::steady_clock::now();
	static uint64_t window_frames = 0;
	static uint64_t last_compiles = 0;
	static uint64_t last_us       = 0;
	static uint64_t last_draws    = 0;
	static uint64_t last_disp     = 0;
	static uint64_t last_eops     = 0;
	static std::array<uint64_t, static_cast<size_t>(WaitSite::Count)> last_wait_count {};
	static std::array<uint64_t, static_cast<size_t>(WaitSite::Count)> last_wait_us {};

	++window_frames;
	const auto now     = std::chrono::steady_clock::now();
	const auto elapsed = std::chrono::duration<double>(now - window_start).count();
	if (elapsed < 2.0) {
		return;
	}
	auto&      s        = g_frame_stats;
	const auto compiles = s.compiles.load(std::memory_order_relaxed);
	const auto us       = s.compile_us.load(std::memory_order_relaxed);
	const auto max_us   = s.compile_us_max.exchange(0, std::memory_order_relaxed);
	const auto draws    = s.draws.load(std::memory_order_relaxed);
	const auto disp     = s.dispatches.load(std::memory_order_relaxed);
	const auto eops     = s.eops.load(std::memory_order_relaxed);

	std::printf("PERF: %.1f fps | draws %llu, dispatches %llu, eops %llu | compiles %llu (%.0f ms, "
	            "max %.0f ms) | in last %.1f s\n",
	            static_cast<double>(window_frames) / elapsed,
	            static_cast<unsigned long long>(draws - last_draws),
	            static_cast<unsigned long long>(disp - last_disp),
	            static_cast<unsigned long long>(eops - last_eops),
	            static_cast<unsigned long long>(compiles - last_compiles),
	            static_cast<double>(us - last_us) / 1000.0, static_cast<double>(max_us) / 1000.0,
	            elapsed);

	uint64_t total_wait_us = 0;
	for (size_t i = 0; i < s.waits.size(); i++) {
		const auto count = s.waits[i].count.load(std::memory_order_relaxed);
		const auto wus   = s.waits[i].us.load(std::memory_order_relaxed);
		const auto max_w = s.waits[i].us_max.exchange(0, std::memory_order_relaxed);
		if (count != last_wait_count[i]) {
			total_wait_us += wus - last_wait_us[i];
			std::printf("PERF:   CPU waited for GPU at %-20s %6llu times, %8.1f ms total, max %.1f ms\n",
			            WaitSiteName(static_cast<WaitSite>(i)),
			            static_cast<unsigned long long>(count - last_wait_count[i]),
			            static_cast<double>(wus - last_wait_us[i]) / 1000.0,
			            static_cast<double>(max_w) / 1000.0);
		}
		last_wait_count[i] = count;
		last_wait_us[i]    = wus;
	}
	std::printf("PERF:   total blocked on GPU: %.0f ms of %.0f ms\n",
	            static_cast<double>(total_wait_us) / 1000.0, elapsed * 1000.0);
	std::fflush(stdout);

	window_start  = now;
	window_frames = 0;
	last_compiles = compiles;
	last_us       = us;
	last_draws    = draws;
	last_disp     = disp;
	last_eops     = eops;
}

} // namespace Libs::Graphics

#endif
