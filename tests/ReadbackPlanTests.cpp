// Tests for the rule that decides how GPU-written bytes get back into guest memory.

#include "graphics/host_gpu/readbackPlan.h"

#include <iostream>

namespace {

using Libs::Graphics::PlanReadback;
using Libs::Graphics::READBACK_MAX_CALLER_WAITS;
using Libs::Graphics::ReadbackCallerCanWait;
using Libs::Graphics::ReadbackPlan;
using Libs::Graphics::ReadbackState;
using Libs::Graphics::ReadbackStep;

int g_failures = 0;

void Check(bool condition, const char* message) {
	if (!condition) {
		std::cerr << "FAILED: " << message << '\n';
		g_failures++;
	}
}

void TestMemoryTheCpuCannotReadIsCopiedThroughTheGpu() {
	const auto plan = PlanReadback({.cpu_readable    = false,
	                                .last_write_tick = 3,
	                                .current_tick    = 9,
	                                .last_write_done = true,
	                                .caller_can_wait = true});
	Check(plan == ReadbackPlan {}, "without CPU-readable memory the copy path must stay");
}

void TestAFinishedWriteIsPublishedAtOnce() {
	for (const bool can_wait: {false, true}) {
		const auto plan = PlanReadback({.cpu_readable    = true,
		                                .last_write_tick = 3,
		                                .current_tick    = 9,
		                                .last_write_done = true,
		                                .caller_can_wait = can_wait});
		Check(plan.step == ReadbackStep::Publish && !plan.submit_first && plan.tick == 3,
		      "a finished write needs no wait and no submit");
	}
}

void TestAWriteInFlightIsWaitedForByTheCaller() {
	const auto plan = PlanReadback({.cpu_readable    = true,
	                                .last_write_tick = 7,
	                                .current_tick    = 9,
	                                .last_write_done = false,
	                                .caller_can_wait = true});
	Check(plan.step == ReadbackStep::CallerWaits && !plan.submit_first && plan.tick == 7,
	      "the caller must wait for the submission of the write, and only for it");
}

void TestAWriteInFlightIsWaitedForOnTheGpuThreadWhenTheCallerCannot() {
	const auto plan = PlanReadback({.cpu_readable    = true,
	                                .last_write_tick = 7,
	                                .current_tick    = 9,
	                                .last_write_done = false,
	                                .caller_can_wait = false});
	Check(plan.step == ReadbackStep::WaitThenPublish && !plan.submit_first && plan.tick == 7,
	      "the GPU thread must wait for the submission of the write, and only for it");
}

void TestAWriteInTheOpenSubmissionIsSubmittedFirst() {
	for (const bool can_wait: {false, true}) {
		// `last_write_done` is never true for the open submission; the rule must not trust it.
		for (const bool done: {false, true}) {
			const auto plan = PlanReadback({.cpu_readable    = true,
			                                .last_write_tick = 9,
			                                .current_tick    = 9,
			                                .last_write_done = done,
			                                .caller_can_wait = can_wait});
			Check(plan.submit_first && plan.tick == 9,
			      "a write in the open submission must be submitted");
			Check(plan.step ==
			          (can_wait ? ReadbackStep::CallerWaits : ReadbackStep::WaitThenPublish),
			      "a write in the open submission is never published without a wait");
		}
	}
}

void TestABufferTheGpuNeverWroteIsPublishedAtOnce() {
	const auto plan = PlanReadback({.cpu_readable    = true,
	                                .last_write_tick = 0,
	                                .current_tick    = 1,
	                                .last_write_done = true,
	                                .caller_can_wait = true});
	Check(plan.step == ReadbackStep::Publish, "tick 0 is before every submission");
}

} // namespace

// A buffer the GPU writes in every submission: the caller waits a few times, then the GPU thread
// waits, so the read-back ends.
void TestACallerThatWaitedTooOftenLeavesTheWaitToTheGpuThread() {
	Check(ReadbackCallerCanWait(true, 0), "a guest thread could not wait for its first read-back");
	Check(ReadbackCallerCanWait(true, READBACK_MAX_CALLER_WAITS - 1),
	      "a guest thread could not wait before its limit");
	Check(!ReadbackCallerCanWait(true, READBACK_MAX_CALLER_WAITS),
	      "a guest thread kept waiting past its limit");
	Check(!ReadbackCallerCanWait(false, 0), "the GPU thread was taken as a caller that can wait");
}

int main() {
	TestMemoryTheCpuCannotReadIsCopiedThroughTheGpu();
	TestAFinishedWriteIsPublishedAtOnce();
	TestAWriteInFlightIsWaitedForByTheCaller();
	TestAWriteInFlightIsWaitedForOnTheGpuThreadWhenTheCallerCannot();
	TestAWriteInTheOpenSubmissionIsSubmittedFirst();
	TestABufferTheGpuNeverWroteIsPublishedAtOnce();
	TestACallerThatWaitedTooOftenLeavesTheWaitToTheGpuThread();
	if (g_failures != 0) {
		std::cerr << g_failures << " failure(s)\n";
		return 1;
	}
	std::cout << "readback plan tests passed\n";
	return 0;
}
