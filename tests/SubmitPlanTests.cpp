// Tests for the rule that decides when the open host command buffer is submitted.

#include "graphics/host_gpu/submitPlan.h"

#include <iostream>

namespace {

using Libs::Graphics::PlanSubmit;
using Libs::Graphics::SUBMIT_AGE_LIMIT_US;
using Libs::Graphics::SUBMIT_OPERATION_LIMIT;
using Libs::Graphics::SUBMIT_WORK_LIMIT;
using Libs::Graphics::SubmitReason;
using Libs::Graphics::SubmitState;

int g_failures = 0;

void Check(bool condition, const char* message) {
	if (!condition) {
		std::cerr << "FAILED: " << message << '\n';
		g_failures++;
	}
}

void TestAnEmptyCommandBufferIsNeverSubmitted() {
	Check(PlanSubmit({}) == SubmitReason::None, "nothing recorded, nothing to submit");
	Check(PlanSubmit({.about_to_wait = true}) == SubmitReason::None,
	      "an empty command buffer stays open while the GPU thread waits");
	Check(PlanSubmit({.open_age_us = SUBMIT_AGE_LIMIT_US * 10}) == SubmitReason::None,
	      "age counts from the first command, an empty buffer has none");
}

void TestAFewDrawsAreHeldBack() {
	const SubmitState state {.open_work = 1, .open_used = true, .open_age_us = 100};
	Check(PlanSubmit(state) == SubmitReason::None, "one fresh draw waits for more");
	Check(PlanSubmit({.open_work = SUBMIT_WORK_LIMIT - 1, .open_used = true}) == SubmitReason::None,
	      "below the work limit the buffer stays open");
	Check(PlanSubmit({.open_used = true, .open_age_us = SUBMIT_AGE_LIMIT_US - 1}) ==
	          SubmitReason::None,
	      "below the age limit the buffer stays open");
}

void TestEnoughWorkOrAgeSubmits() {
	Check(PlanSubmit({.open_work = SUBMIT_WORK_LIMIT, .open_used = true}) == SubmitReason::Work,
	      "the work limit submits");
	Check(PlanSubmit({.open_used = true, .open_age_us = SUBMIT_AGE_LIMIT_US}) == SubmitReason::Age,
	      "the age limit submits, also with transfers only");
}

void TestACallbackSubmitsAtOnce() {
	Check(PlanSubmit({.open_work = 1, .open_callbacks = 1, .open_used = true}) ==
	          SubmitReason::Callbacks,
	      "an end-of-pipe interrupt or flip on the open buffer submits it");
	Check(PlanSubmit({.open_callbacks = 1}) == SubmitReason::Callbacks,
	      "a callback without recorded commands still needs the submission to complete");
}

void TestTheGpuThreadSubmitsBeforeItWaits() {
	Check(PlanSubmit({.open_work = 1, .open_used = true, .about_to_wait = true}) ==
	          SubmitReason::Idle,
	      "what is recorded goes to the GPU before the thread sleeps");
	Check(PlanSubmit({.open_used = true, .about_to_wait = true}) == SubmitReason::Idle,
	      "transfers alone also go before the thread sleeps");
}

void TestReasonsInOrder() {
	// A callback outranks everything, then the thread's wait, then the work and age limits.
	const SubmitState all {.open_work      = SUBMIT_WORK_LIMIT,
	                       .open_callbacks = 1,
	                       .open_used      = true,
	                       .open_age_us    = SUBMIT_AGE_LIMIT_US,
	                       .about_to_wait  = true};
	Check(PlanSubmit(all) == SubmitReason::Callbacks, "callbacks come first");
	SubmitState state = all;
	state.open_callbacks = 0;
	Check(PlanSubmit(state) == SubmitReason::Idle, "then the thread's wait");
	state.about_to_wait = false;
	Check(PlanSubmit(state) == SubmitReason::Work, "then the work limit");
	state.open_work = 0;
	Check(PlanSubmit(state) == SubmitReason::Age, "then the age limit");
	Check(PlanSubmit({.open_work = SUBMIT_WORK_LIMIT * 4, .open_used = true}) == SubmitReason::Work,
	      "far above the work limit still submits for work");
	Check(PlanSubmit({.open_work = SUBMIT_WORK_LIMIT, .open_used = false}) == SubmitReason::None,
	      "work without a recorded command is a contradiction and submits nothing");
}

void TestManyDeferredOperationsSubmit() {
	Check(PlanSubmit({.open_operations = SUBMIT_OPERATION_LIMIT - 1, .open_used = true}) ==
	          SubmitReason::None,
	      "below the operation limit the buffer stays open");
	Check(PlanSubmit({.open_operations = SUBMIT_OPERATION_LIMIT, .open_used = true}) ==
	          SubmitReason::Operations,
	      "the operation limit submits: the operations wait for this submission");
	Check(PlanSubmit({.open_operations = SUBMIT_OPERATION_LIMIT}) == SubmitReason::Operations,
	      "an empty command buffer also goes, so that its operations can run");
	Check(PlanSubmit({.open_work      = SUBMIT_WORK_LIMIT,
	                  .open_operations = SUBMIT_OPERATION_LIMIT,
	                  .open_used      = true}) == SubmitReason::Operations,
	      "the operation limit comes before the work limit");
	Check(PlanSubmit({.open_operations = SUBMIT_OPERATION_LIMIT, .about_to_wait = true}) ==
	          SubmitReason::Idle,
	      "the thread's wait comes before the operation limit");
}

} // namespace

int main() {
	TestManyDeferredOperationsSubmit();
	TestReasonsInOrder();
	TestAnEmptyCommandBufferIsNeverSubmitted();
	TestAFewDrawsAreHeldBack();
	TestEnoughWorkOrAgeSubmits();
	TestACallbackSubmitsAtOnce();
	TestTheGpuThreadSubmitsBeforeItWaits();
	if (g_failures != 0) {
		std::cerr << g_failures << " check(s) failed\n";
		return 1;
	}
	std::cout << "submit plan tests passed\n";
	return 0;
}
