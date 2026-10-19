#ifndef KYTY_COMMON_TRACE_H_
#define KYTY_COMMON_TRACE_H_

#include "common/common.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

// A measured run (--trace-dir): what each guest frame cost, where the time went, and what went
// wrong, written to files a script reads after the run.
//
// In the trace folder:
//   frames.csv     one row per guest frame (flip): frame time, the perf counters of the frame,
//                  and the time spent in each of the frame columns (GPU waits, compiles, ...)
//   hotspots.csv   every timed site (each lib function, each wait): calls, total and longest time
//   anomalies.csv  everything that went wrong, once per kind and key, with a count, the frame of
//                  the first time and a host backtrace of it
//   threads.txt    what each thread is doing now: its innermost timed site and for how long
//   hang.txt       written when no frame ended for a while (and threads.txt with it)
//   crash.txt      a fatal error or signal, with a backtrace
//   shots/         a PNG of the presented frame every few seconds
// The files other than frames.csv are rewritten every two seconds, so a run that dies keeps them.
//
// Off (no --trace-dir), a timed site costs one relaxed load and a branch.

namespace Trace {

namespace Detail {
inline std::atomic<bool> g_enabled {false};
// Profiler blocks (profiler.h) are timed sites too, unless KYTY_TRACE_NO_PROF is set: there are
// many, and timing them all slows a run down by a quarter.
inline std::atomic<bool> g_prof_enabled {false};
} // namespace Detail

[[nodiscard]] inline bool Enabled() {
	return Detail::g_enabled.load(std::memory_order_relaxed);
}

[[nodiscard]] inline bool ProfEnabled() {
	return Detail::g_prof_enabled.load(std::memory_order_relaxed);
}

void Initialize();
void Shutdown();

struct Lifecycle {
	static constexpr const char* name               = "Trace";
	static constexpr auto        initialize         = Trace::Initialize;
	static constexpr auto        shutdown           = Trace::Shutdown;
	static constexpr auto        emergency_shutdown = Trace::Shutdown;
};

// A place whose calls a run times. Declared static where it is timed; it lives for the process.
struct Site {
	Site(const char* group, const char* name) noexcept;

	const char*           group;
	const char*           name;
	std::atomic<uint64_t> calls {0};
	std::atomic<uint64_t> nanoseconds {0};
	std::atomic<uint64_t> max_nanoseconds {0};
	Site*                 next = nullptr;
};

// The sites of a frame row of frames.csv: their time and calls of each frame.
enum class Column : uint8_t {
	GpuWait,         // a host thread waits for the GPU to retire work
	ShaderCompile,   // a guest shader translated to SPIR-V on a program cache miss
	PipelineCompile, // a host pipeline built (on a builder thread)
	PipelineWait,    // a draw waits for its pipeline to be built
	FrameAcquire,    // the presenter waits for a free frame
	GuestEqueueWait, // a guest thread waits on an event queue (flips, vblanks, GPU events)
	GpuIdle,         // the GPU-command thread waits for guest work (or for a blocked queue)
	GpuCommand,      // the GPU-command thread runs a command another thread sent it
	GpuGraphics,     // the GPU-command thread processes a graphics submission
	GpuCompute,      // the GPU-command thread processes a compute submission
	Count,
};

Site& ColumnSite(Column column);

// The thread that bounds the frame rate (the GPU-command thread) calls it once: frames.csv also
// has, for each column, the time of that thread alone (<column>_crit_ms).
void SetCriticalThread() noexcept;

void Enter(Site& site) noexcept;
void Leave(Site& site, uint64_t nanoseconds) noexcept;

[[nodiscard]] inline uint64_t NowNs() noexcept {
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
	                                 std::chrono::steady_clock::now().time_since_epoch())
	                                 .count());
}

class Scope final {
public:
	explicit Scope(Site& site) noexcept: Scope(site, Enabled()) {}
	Scope(Site& site, bool on) noexcept: m_site(site), m_on(on) {
		if (m_on) {
			m_begin = NowNs();
			Enter(m_site);
		}
	}
	~Scope() {
		if (m_on) {
			Leave(m_site, NowNs() - m_begin);
		}
	}
	KYTY_CLASS_NO_COPY(Scope);

private:
	Site&    m_site;
	bool     m_on;
	uint64_t m_begin = 0;
};

// Something went wrong: counted once per kind and key. The first time keeps `detail` and a host
// backtrace.
void Note(std::string_view kind, std::string_view key, std::string_view detail = {});

// Notes `kind` for `key` (as hex) with the time taken when it took `nanoseconds` or more.
void NoteSlow(const char* kind, uint64_t key, uint64_t nanoseconds);

// Notes its scope when it takes `threshold_ms` or longer.
class SlowScope final {
public:
	SlowScope(const char* kind, uint64_t key, uint64_t threshold_ms) noexcept
	    : m_kind(kind), m_key(key), m_threshold_ns(threshold_ms * 1'000'000), m_on(Enabled()) {
		if (m_on) {
			m_begin = NowNs();
		}
	}
	~SlowScope() {
		if (m_on && NowNs() - m_begin >= m_threshold_ns) {
			NoteSlow(m_kind, m_key, NowNs() - m_begin);
		}
	}
	KYTY_CLASS_NO_COPY(SlowScope);

private:
	const char* m_kind;
	uint64_t    m_key;
	uint64_t    m_threshold_ns;
	bool        m_on;
	uint64_t    m_begin = 0;
};

// Adds `value` (and one call) to `key` of `table`: keyed.csv lists the largest keys of each table.
// `table` is a string literal.
void AddKeyed(const char* table, uint64_t key, uint64_t value);

// Times its scope and adds it to table "wait_stack" under a hash of the caller's stack; the first
// time a stack is seen, it is noted with its backtrace (kind "wait_stack").
class StackTimedScope final {
public:
	StackTimedScope() noexcept: m_on(Enabled()) {
		if (m_on) {
			m_begin = NowNs();
		}
	}
	~StackTimedScope();
	KYTY_CLASS_NO_COPY(StackTimedScope);

private:
	bool     m_on;
	uint64_t m_begin = 0;
};

// A guest frame ended (a flip). One thread calls it.
void EndFrame();

// The presenter asks at each present whether to save the frame; true at most every few seconds.
[[nodiscard]] bool ScreenshotDue();
// Writes an RGBA8 image as shots/<frame>.png, on a thread of its own.
void SaveScreenshot(const uint8_t* rgba, uint32_t width, uint32_t height);

// The process stops: writes crash.txt with `reason` and a backtrace, and the other reports.
void Fatal(std::string_view reason) noexcept;

} // namespace Trace

// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define KYTY_TRACE_CONCAT_IMPL(a, b) a##b
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define KYTY_TRACE_CONCAT(a, b) KYTY_TRACE_CONCAT_IMPL(a, b)

// Times the rest of the block as `name` of `group`.
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define KYTY_TRACE_SCOPE(group, name)                                                              \
	static ::Trace::Site KYTY_TRACE_CONCAT(kyty_trace_site_, __LINE__) {group, name};              \
	const ::Trace::Scope KYTY_TRACE_CONCAT(kyty_trace_scope_, __LINE__) {                          \
		KYTY_TRACE_CONCAT(kyty_trace_site_, __LINE__)                                              \
	}

// Times the rest of the block in a column of frames.csv.
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define KYTY_TRACE_COLUMN(column)                                                                  \
	const ::Trace::Scope KYTY_TRACE_CONCAT(kyty_trace_scope_, __LINE__) {                          \
		::Trace::ColumnSite(::Trace::Column::column)                                               \
	}

#endif /* KYTY_COMMON_TRACE_H_ */
