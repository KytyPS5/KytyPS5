#include "common/perfCounters.h"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

void Check(bool value, const char* text) {
	if (!value) {
		std::fprintf(stderr, "PerfCountersTests: failed: %s\n", text);
		std::abort();
	}
}

using PerfCounters::Counter;
using PerfCounters::FrameReporter;
using PerfCounters::Snapshot;

Snapshot With(Counter counter, uint64_t value, Snapshot snapshot = {}) {
	snapshot[static_cast<size_t>(counter)] = value;
	return snapshot;
}

// The report comes once every N frames, with what each frame of them did on average.
void TestReportEveryNFrames() {
	FrameReporter reporter(3);
	Check(!reporter.EndFrame({}, 0.0).has_value(), "the first frame end only starts the window");
	Check(!reporter.EndFrame(With(Counter::Draws, 100), 0.1).has_value(), "reported after one frame");
	Check(!reporter.EndFrame(With(Counter::Draws, 200), 0.2).has_value(), "reported after two frames");
	const auto report = reporter.EndFrame(With(Counter::Draws, 300), 0.3);
	Check(report.has_value(), "no report after three frames");
	Check(report->frames == 3, "the report does not cover three frames");
	Check(report->per_frame[static_cast<size_t>(Counter::Draws)] == 100.0,
	      "the draws of a frame are not the average over the window");
	Check(report->fps > 9.99 && report->fps < 10.01, "three frames in 0.3 s are not 10 fps");
}

// The next window starts where the last one ended: its counts are not added again.
void TestWindowsDoNotOverlap() {
	FrameReporter reporter(2);
	(void)reporter.EndFrame({}, 0.0);
	(void)reporter.EndFrame(With(Counter::GuestFaults, 10), 1.0);
	Check(reporter.EndFrame(With(Counter::GuestFaults, 20), 2.0).has_value(), "no first report");
	(void)reporter.EndFrame(With(Counter::GuestFaults, 60), 3.0);
	const auto report = reporter.EndFrame(With(Counter::GuestFaults, 100), 4.0);
	Check(report.has_value() && report->per_frame[static_cast<size_t>(Counter::GuestFaults)] == 40.0,
	      "the second window counted faults of the first");
	Check(report->fps > 0.99 && report->fps < 1.01, "two frames in 2 s are not 1 fps");
}

// The printed line names every counter once, in a form a script can read.
void TestFormat() {
	FrameReporter reporter(1);
	(void)reporter.EndFrame({}, 0.0);
	const auto report = reporter.EndFrame(With(Counter::RenderPasses, 190), 0.05);
	Check(report.has_value(), "no report");
	const auto line = PerfCounters::Format(*report);
	Check(line.starts_with("Perf: frames 1 fps 20.0 per frame:"), "the line does not start as expected");
	Check(line.find(" render_passes=190.0") != std::string::npos, "the render passes are not printed");
	for (size_t i = 0; i < PerfCounters::CounterCount; i++) {
		const std::string name = std::string(" ") + PerfCounters::NameOf(static_cast<Counter>(i)) + "=";
		Check(line.find(name) != std::string::npos, "a counter is missing from the line");
		Check(line.find(name, line.find(name) + 1) == std::string::npos, "a counter is printed twice");
	}
}

// Counting is off until it is turned on, so the emulator pays only a load when it is not asked for.
void TestOffUntilEnabled() {
	PerfCounters::SetEnabled(false);
	const auto before = PerfCounters::Read()[static_cast<size_t>(Counter::Draws)];
	PerfCounters::Add(Counter::Draws);
	Check(PerfCounters::Read()[static_cast<size_t>(Counter::Draws)] == before,
	      "a counter moved while counting was off");
	PerfCounters::SetEnabled(true);
	PerfCounters::Add(Counter::Draws, 5);
	Check(PerfCounters::Read()[static_cast<size_t>(Counter::Draws)] == before + 5,
	      "a counter did not move while counting was on");
	PerfCounters::SetEnabled(false);
}

} // namespace

int main() {
	TestReportEveryNFrames();
	TestWindowsDoNotOverlap();
	TestFormat();
	TestOffUntilEnabled();
	std::puts("PerfCountersTests: all cases passed");
	return 0;
}
