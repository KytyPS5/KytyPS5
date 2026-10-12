#include "common/perfCounters.h"

#include "common/profiler.h"

#include <chrono>

namespace PerfCounters {

void EndFrame() {
	if (!Enabled()) {
		return;
	}
	static FrameReporter reporter(30);
	static const auto    start    = std::chrono::steady_clock::now();
	static Snapshot      previous = Read();

	const auto now = Read();
	if (tracy::ProfilerAvailable()) {
		for (size_t i = 0; i < CounterCount; i++) {
			TracyPlot(NameOf(static_cast<Counter>(i)), static_cast<int64_t>(now[i] - previous[i]));
		}
	}
	previous = now;

	const auto seconds =
	    std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
	if (const auto report = reporter.EndFrame(now, seconds)) {
		std::printf("%s\n", Format(*report).c_str());
	}
}

} // namespace PerfCounters
