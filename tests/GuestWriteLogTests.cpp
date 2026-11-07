// Tests for the log of guest memory the CPU wrote, which upload passes synchronize.

#include "graphics/host_gpu/guestWriteLog.h"

#include <iostream>
#include <vector>

namespace {

using Libs::Graphics::GuestWriteLog;
using Libs::Graphics::GuestRange;

int g_failures = 0;

void Check(bool condition, const char* message) {
	if (!condition) {
		std::cerr << "FAILED: " << message << '\n';
		g_failures++;
	}
}

bool Same(const std::vector<GuestRange>& got, const std::vector<GuestRange>& want) {
	if (got.size() != want.size()) {
		return false;
	}
	for (size_t i = 0; i < got.size(); i++) {
		if (got[i].address != want[i].address || got[i].size != want[i].size) {
			return false;
		}
	}
	return true;
}

// A new log has no history, so the first pass synchronizes everything.
void TestTheFirstPassIsFull() {
	GuestWriteLog             log(16);
	std::vector<GuestRange> out;
	Check(!log.Take(out), "the first pass was not a full one");
	Check(log.Take(out) && out.empty(), "a second pass with nothing written was not empty");
}

// A pass gets each written run once: sorted, with overlapping and adjacent ranges merged.
void TestWritesAreSortedAndMerged() {
	GuestWriteLog             log(16);
	std::vector<GuestRange> out;
	(void)log.Take(out);
	log.Record({0x3000, 0x100});
	log.Record({0x1000, 0x100});
	log.Record({0x1080, 0x100});
	log.Record({0x1180, 0x80});
	log.Record({0x2000, 0x10});
	Check(log.Take(out), "a pass with a few writes was full");
	Check(Same(out, {{0x1000, 0x200}, {0x2000, 0x10}, {0x3000, 0x100}}),
	      "the written ranges were not sorted and merged");
	Check(log.Take(out) && out.empty(), "a range was taken twice");
}

// When the log fills up, the next pass synchronizes everything instead.
void TestOverflowAsksForAFullPass() {
	GuestWriteLog             log(2);
	std::vector<GuestRange> out;
	(void)log.Take(out);
	log.Record({0x1000, 1});
	log.Record({0x3000, 1});
	log.Record({0x5000, 1});
	Check(!log.Take(out), "an overflowed log did not ask for a full pass");
	Check(log.Take(out) && out.empty(), "the full pass did not start a new log");
	log.RequestFullPass();
	log.Record({0x1000, 1});
	Check(!log.Take(out), "a requested full pass was not full");
}

// A hot range stays open after an upload, so the CPU's next writes to it do not fault. It is
// uploaded again once the next guest submission starts, not at every pass of the same one.
void TestHotRangesWaitForTheNextSubmission() {
	GuestWriteLog             log(16);
	std::vector<GuestRange> out;
	(void)log.Take(out);
	log.RecordHot({0x8000, 0x4000});
	Check(log.Take(out) && out.empty(), "a hot range was due within its own submission");
	log.BeginSubmission();
	Check(log.Take(out) && Same(out, {{0x8000, 0x4000}}),
	      "a hot range was not due after the next submission started");
	Check(log.Take(out) && out.empty(), "a hot range was due twice");
}

// Hot ranges count against the capacity as written ranges do: past it a full pass is cheaper,
// and the log does not grow without a limit.
void TestHotRangesOverflowToAFullPass() {
	GuestWriteLog             log(2);
	std::vector<GuestRange> out;
	(void)log.Take(out);
	log.RecordHot({0x1000, 1});
	log.RecordHot({0x3000, 1});
	log.RecordHot({0x5000, 1});
	log.BeginSubmission();
	Check(!log.Take(out), "more hot ranges than the capacity did not ask for a full pass");
	Check(log.Take(out) && out.empty(), "the full pass did not start a new log");
}

// A hot range uploaded in many submissions before a pass is due once.
void TestHotRangesAreDueOnce() {
	GuestWriteLog             log(4);
	std::vector<GuestRange> out;
	(void)log.Take(out);
	for (int submission = 0; submission < 10; submission++) {
		log.RecordHot({0x8000, 0x4000});
		log.RecordHot({0x8000, 0x4000});
		log.BeginSubmission();
	}
	Check(log.Take(out), "a hot range bound in many submissions filled the log");
	Check(Same(out, {{0x8000, 0x4000}}), "a hot range was due more than once");
}

// Hot ranges and written ranges that touch are merged into one run.
void TestHotAndWrittenRangesMerge() {
	GuestWriteLog             log(16);
	std::vector<GuestRange> out;
	(void)log.Take(out);
	log.RecordHot({0x1000, 0x1000});
	log.BeginSubmission();
	log.Record({0x2000, 0x100});
	Check(log.Take(out) && Same(out, {{0x1000, 0x1100}}),
	      "a hot range and the write after it were not merged");
}

} // namespace

int main() {
	TestTheFirstPassIsFull();
	TestWritesAreSortedAndMerged();
	TestOverflowAsksForAFullPass();
	TestHotRangesWaitForTheNextSubmission();
	TestHotRangesOverflowToAFullPass();
	TestHotRangesAreDueOnce();
	TestHotAndWrittenRangesMerge();
	if (g_failures != 0) {
		return 1;
	}
	std::cout << "GuestWriteLogTests: all cases passed\n";
	return 0;
}
