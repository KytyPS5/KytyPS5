#ifndef KYTY_COMMON_PERF_REPORT_H_
#define KYTY_COMMON_PERF_REPORT_H_

// Performance report: a plain-text summary of where the emulator spends time outside the GPU,
// written every few seconds to a text file (_kyty_perf.txt by default). It answers questions that
// a sampling profiler answers poorly, such as which code path made the CPU wait for the GPU,
// which code path submitted work, and which guest memory causes write-tracking faults.
//
// Enabled with --perf-report and --perf-report-file (launcher: "Performance report"). When
// disabled, every entry point
// is a relaxed atomic load and a branch; Now() returns 0 and the Note* calls return at once.
// See docs/performance-report.md for how to read the report.

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <source_location>

namespace Common::PerfReport {

namespace Detail {
inline std::atomic<bool>                  g_enabled {false};
// Outermost instrumented scheduler call on this thread (see SiteScope).
inline thread_local const std::source_location* g_site = nullptr;
// Buffer downloads started on this thread; lets a fault handler tell whether it downloaded.
inline thread_local uint64_t g_downloads = 0;
uint64_t NowNs() noexcept;
} // namespace Detail

[[nodiscard]] inline bool Enabled() noexcept {
	return Detail::g_enabled.load(std::memory_order_relaxed);
}

// Starts or stops collection, writing the report to `file` (replaced if it exists). Stopping
// writes the time since the last block as a final, partial block. Called from Config::Load,
// Config::Shutdown and Config::EmergencyShutdown (the normal exit path uses quick_exit, which runs
// only emergency shutdown).
void Configure(bool enabled, const std::filesystem::path& file);

// Timestamp for the Note* calls that take a start time, or 0 when the report is disabled.
[[nodiscard]] inline uint64_t Now() noexcept {
	return Enabled() ? Detail::NowNs() : 0;
}

// Attributes GPU waits and queue submits to the outermost instrumented call site on this thread.
// The scheduler entry points take a defaulted std::source_location, so the site is the caller of
// Flush(), Finish(), Wait() and so on, e.g. "bufferCache.cpp:236 DownloadBufferMemory".
class SiteScope {
public:
	explicit SiteScope(const std::source_location& site) noexcept
	    : m_owner(Enabled() && Detail::g_site == nullptr) {
		if (m_owner) {
			Detail::g_site = &site;
		}
	}
	~SiteScope() {
		if (m_owner) {
			Detail::g_site = nullptr;
		}
	}
	SiteScope(const SiteScope&)            = delete;
	SiteScope& operator=(const SiteScope&) = delete;

private:
	bool m_owner;
};

// The CPU waited for the GPU (timeline semaphore) since `start`.
void NoteGpuWait(bool gpu_thread, uint64_t start);
// A queue submission that began at `start`.
void NoteQueueSubmit(bool gpu_thread, uint64_t start);
// The GPU command thread had no work to process since `start`.
void NoteGpuIdle(uint64_t start);
// The GPU command thread had work, but every queue was blocked on a guest wait (for example
// WAIT_REG_MEM on a label the game has not written yet), since `start`.
void NoteGpuBlocked(uint64_t start);
// A flip completed (one presented frame).
void NoteFlip();

// A buffer download from GPU memory started on this thread.
inline void NoteDownload() noexcept {
	++Detail::g_downloads;
}
[[nodiscard]] inline uint64_t DownloadCount() noexcept {
	return Detail::g_downloads;
}

// A write-tracking fault handled since `start`. For write faults, `buffer_done` is when the
// buffer cache finished (the texture cache ran after it); `downloaded` says the handler had to
// download GPU data first.
void NoteFault(uint64_t address, bool write, uint64_t start, uint64_t buffer_done, bool downloaded);
// A shader binding that the buffer cache will mark GPU-written. Lets download faults name the
// shader that wrote the page.
void NoteGpuWrite(uint64_t address, uint64_t size, uint64_t shader_hash);
// A tracking protection change: `lock_start` before taking the address-space lock, `call_start`
// after it.
void NoteProtect(uint64_t lock_start, uint64_t call_start);
// Bytes copied from guest memory into GPU buffers.
void NoteBufferUpload(uint64_t bytes);
// A page range queued for buffer-device-address synchronization.
void NoteBdaQueue(uint64_t bytes);
// Buffer cache size, sampled at each garbage-collector tick.
void NoteBufferCache(uint64_t buffers, uint64_t bytes);

// Writes an interval to the report if enough time has passed. Called by the GPU command thread
// before each command or submission; cheap when nothing is due. An interval stretches while the
// thread is busy with one long command, and the report prints each interval's real length.
void MaybeWrite();

} // namespace Common::PerfReport

#endif /* KYTY_COMMON_PERF_REPORT_H_ */
