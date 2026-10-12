#ifndef EMULATOR_INCLUDE_EMULATOR_COMMON_PERFCOUNTERS_H_
#define EMULATOR_INCLUDE_EMULATOR_COMMON_PERFCOUNTERS_H_

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>

// Counts of events that cost frame time, reported per guest frame (--perf-counters).
//
// A counter costs one relaxed load and a branch while counting is off, so the calls stay in the
// emulator. With counting on, every 30 flips the emulator prints one line with what a frame did
// on average, and with --profile each counter is also a Tracy plot.

namespace PerfCounters {

enum class Counter : uint8_t {
	GuestFaults,     // write faults on pages the emulator watches
	PageProtections, // protection changes of watched pages
	UploadPasses,    // draws and dispatches that read memory through device addresses
	UploadBytes,     // bytes the buffer cache copied from guest memory to GPU buffers
	UploadCopies,    // copy regions of those uploads
	Draws,
	Dispatches,
	RenderPasses,    // render passes begun on the host
	Submissions,     // host queue submissions
	ZeroSleeps,      // guest sleeps of no time (a guest thread spinning on a wait)
	Count,
};

inline constexpr size_t CounterCount = static_cast<size_t>(Counter::Count);

using Snapshot = std::array<uint64_t, CounterCount>;

[[nodiscard]] constexpr const char* NameOf(Counter counter) {
	switch (counter) {
		case Counter::GuestFaults: return "guest_faults";
		case Counter::PageProtections: return "page_protections";
		case Counter::UploadPasses: return "upload_passes";
		case Counter::UploadBytes: return "upload_bytes";
		case Counter::UploadCopies: return "upload_copies";
		case Counter::Draws: return "draws";
		case Counter::Dispatches: return "dispatches";
		case Counter::RenderPasses: return "render_passes";
		case Counter::Submissions: return "submissions";
		case Counter::ZeroSleeps: return "zero_sleeps";
		case Counter::Count: break;
	}
	return "?";
}

namespace Detail {
inline std::atomic<bool>                                 g_enabled {false};
inline std::array<std::atomic<uint64_t>, CounterCount> g_values {};
} // namespace Detail

inline void SetEnabled(bool enabled) {
	Detail::g_enabled.store(enabled, std::memory_order_relaxed);
}

[[nodiscard]] inline bool Enabled() {
	return Detail::g_enabled.load(std::memory_order_relaxed);
}

inline void Add(Counter counter, uint64_t count = 1) {
	if (Enabled()) {
		Detail::g_values[static_cast<size_t>(counter)].fetch_add(count, std::memory_order_relaxed);
	}
}

[[nodiscard]] inline Snapshot Read() {
	Snapshot snapshot {};
	for (size_t i = 0; i < CounterCount; i++) {
		snapshot[i] = Detail::g_values[i].load(std::memory_order_relaxed);
	}
	return snapshot;
}

struct Report {
	uint32_t                          frames = 0;
	double                            fps    = 0.0;
	std::array<double, CounterCount> per_frame {};
};

// Turns the counters' running totals at each frame end into per-frame averages over a window of
// frames. The first call only starts the first window.
class FrameReporter final {
public:
	explicit FrameReporter(uint32_t frames_per_report): m_frames_per_report(frames_per_report) {}

	// `now`: the counters at the end of a frame. `seconds`: a clock that only moves forward.
	std::optional<Report> EndFrame(const Snapshot& now, double seconds) {
		if (!m_started) {
			m_started = true;
			m_start   = now;
			m_seconds = seconds;
			return std::nullopt;
		}
		if (++m_frames < m_frames_per_report) {
			return std::nullopt;
		}
		Report report;
		report.frames = m_frames;
		report.fps    = seconds > m_seconds ? m_frames / (seconds - m_seconds) : 0.0;
		for (size_t i = 0; i < CounterCount; i++) {
			report.per_frame[i] = static_cast<double>(now[i] - m_start[i]) / m_frames;
		}
		m_start   = now;
		m_seconds = seconds;
		m_frames  = 0;
		return report;
	}

private:
	uint32_t m_frames_per_report;
	uint32_t m_frames  = 0;
	bool     m_started = false;
	Snapshot m_start {};
	double   m_seconds = 0.0;
};

// One line a script can read: "Perf: frames N fps F per frame: name=value ...".
[[nodiscard]] inline std::string Format(const Report& report) {
	char buffer[64];
	std::snprintf(buffer, sizeof(buffer), "Perf: frames %u fps %.1f per frame:", report.frames,
	              report.fps);
	std::string line = buffer;
	for (size_t i = 0; i < CounterCount; i++) {
		std::snprintf(buffer, sizeof(buffer), " %s=%.1f", NameOf(static_cast<Counter>(i)),
		              report.per_frame[i]);
		line += buffer;
	}
	return line;
}

// The guest frame ends (at every flip): prints a report when one is due, and plots the counters
// for Tracy. Call from one thread.
void EndFrame();

} // namespace PerfCounters

#endif /* EMULATOR_INCLUDE_EMULATOR_COMMON_PERFCOUNTERS_H_ */
