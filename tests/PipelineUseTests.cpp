// Tests for the rule that decides what a draw does when its pipeline is not built yet.

#include "graphics/host_gpu/pipelineUse.h"

#include <iostream>

namespace {

using Libs::Graphics::AheadBuildProgress;
using Libs::Graphics::DrawEffects;
using Libs::Graphics::FrameWaitBudget;
using Libs::Graphics::GRACE_WAIT_BUDGET_US;
using Libs::Graphics::GRACE_WAIT_US;
using Libs::Graphics::GraceWaitUs;
using Libs::Graphics::RESET_TARGET_WAIT_BUDGET_US;
using Libs::Graphics::PipelineBuilderCount;
using Libs::Graphics::PipelineUse;
using Libs::Graphics::PlanPipelineUse;
using Libs::Graphics::TargetHistory;

int g_failures = 0;

void Check(bool condition, const char* message) {
	if (!condition) {
		std::cerr << "FAILED: " << message << '\n';
		g_failures++;
	}
}

void TestAReadyPipelineDraws() {
	for (const bool always_wait: {false, true}) {
		Check(PlanPipelineUse({.writes_memory = true, .clears_depth = true, .writes_fresh_target = true},
		                      true, always_wait) == PipelineUse::Draw,
		      "a built pipeline is used at once, whatever the draw does");
	}
}

void TestAVisualDrawIsLeftOutWhileItsPipelineBuilds() {
	Check(PlanPipelineUse({}, false, false) == PipelineUse::LeaveOut,
	      "a draw that only changes the picture is left out while its pipeline builds");
}

void TestADrawWithEffectsBeyondThePictureWaits() {
	Check(PlanPipelineUse({.writes_memory = true}, false, false) == PipelineUse::Wait,
	      "a draw whose shaders write buffers or images waits for its pipeline");
	Check(PlanPipelineUse({.clears_depth = true}, false, false) == PipelineUse::Wait,
	      "a draw that clears its depth or stencil target waits for its pipeline");
	Check(PlanPipelineUse({.writes_fresh_target = true}, false, false) == PipelineUse::Wait,
	      "a draw into a target the game did not draw last frame waits: it may never be drawn "
	      "again");
}

void TestADrawIntoATargetResetThisFrameWaitsWithinTheBudget() {
	Check(PlanPipelineUse({.writes_reset_target = true}, false, false) ==
	          PipelineUse::WaitWithinBudget,
	      "a draw into a target cleared or overwritten this frame waits while the frame's budget "
	      "lasts: left out, the clear colour would show in its place");
	Check(PlanPipelineUse({.writes_fresh_target = true, .writes_reset_target = true}, false,
	                      false) == PipelineUse::Wait,
	      "a reset target that is also fresh waits without a limit: it may never be drawn again");
	Check(PlanPipelineUse({.writes_memory = true, .writes_reset_target = true}, false, false) ==
	          PipelineUse::Wait,
	      "a draw that writes memory waits without a limit, whatever its targets");
}

void TestTheWaitBudgetIsPerFrame() {
	FrameWaitBudget budget;
	Check(budget.Left(5) == RESET_TARGET_WAIT_BUDGET_US, "a frame starts with the whole budget");
	budget.Spend(5, 10'000);
	Check(budget.Left(5) == RESET_TARGET_WAIT_BUDGET_US - 10'000, "a wait spends from the budget");
	budget.Spend(5, RESET_TARGET_WAIT_BUDGET_US);
	Check(budget.Left(5) == 0, "a wait longer than what is left empties the budget");
	Check(budget.Left(6) == RESET_TARGET_WAIT_BUDGET_US, "the next frame has the whole budget again");
	budget.Spend(6, 1'000);
	Check(budget.Left(6) == RESET_TARGET_WAIT_BUDGET_US - 1'000,
	      "spending in the new frame does not bring back the old frame's spending");
}

void TestTargetHistoryTellsResetsApart() {
	TargetHistory history;
	Check(history.Observe(10, 5) == TargetHistory::Seen::Fresh, "a target never drawn is fresh");
	history.Drawn(5, 10);
	Check(history.Observe(10, 5) == TargetHistory::Seen::Drawn,
	      "drawn this frame, nothing else wrote it: it may be left out");
	Check(history.Observe(10, 6) == TargetHistory::Seen::Drawn,
	      "drawn last frame, nothing else wrote it: last frame's picture stays in place");
	Check(history.Observe(10, 7) == TargetHistory::Seen::Fresh,
	      "not drawn for two frames: fresh again");

	TargetHistory cleared;
	cleared.Drawn(5, 10);
	Check(cleared.Observe(11, 6) == TargetHistory::Seen::Reset,
	      "a clear or copy since the last draw: the target was reset");
	cleared.Drawn(6, 11);
	Check(cleared.Observe(11, 6) == TargetHistory::Seen::Reset,
	      "the rest of that frame's draws into it wait as well: the clear shows under each one");
	Check(cleared.Observe(11, 7) == TargetHistory::Seen::Drawn,
	      "the next frame without a clear keeps the last picture: draws may be left out again");
}

void TestAResetAcrossAFrameGapIsKept() {
	TargetHistory history;
	history.Drawn(5, 10);
	Check(history.Observe(11, 7) == TargetHistory::Seen::Fresh,
	      "not drawn for two frames and cleared since: the draw waits as a fresh target");
	history.Drawn(7, 11);
	Check(history.Observe(11, 7) == TargetHistory::Seen::Reset,
	      "the clear seen at that first draw still holds for the rest of the frame");
}

void TestAPendingUploadCountsAsAReset() {
	TargetHistory history;
	history.Drawn(5, 10);
	history.MarkReset(6);
	Check(history.Observe(10, 6) == TargetHistory::Seen::Reset,
	      "CPU writes not yet uploaded to the target: the upload resets it in this frame");
	history.Drawn(6, 11);
	Check(history.Observe(11, 6) == TargetHistory::Seen::Reset,
	      "the upload done inside a drawn draw still holds for the rest of the frame");
	Check(history.Observe(11, 7) == TargetHistory::Seen::Drawn, "the next frame is clean again");
}

void TestEveryDrawWaitsWhenAsked() {
	Check(PlanPipelineUse({}, false, true) == PipelineUse::Wait,
	      "with --pipeline-wait a visual draw waits too");
}

// Many draws of one frame can share a pipeline that is still building: only the first waits, so
// a frame does not stop for 2 ms per draw.
void TestAPipelineWaitsOnceAFrame() {
	Check(GraceWaitUs(UINT64_MAX, 7, GRACE_WAIT_BUDGET_US) == GRACE_WAIT_US,
	      "the first draw of a building pipeline did not wait");
	Check(GraceWaitUs(7, 7, GRACE_WAIT_BUDGET_US) == 0,
	      "a second draw of the same pipeline in the frame waited again");
	Check(GraceWaitUs(7, 8, GRACE_WAIT_BUDGET_US) == GRACE_WAIT_US,
	      "the pipeline did not wait again in the next frame");
}

// At a level start a frame can ask for hundreds of new pipelines: the waits of a frame have a cap.
void TestShortWaitsHaveABudgetPerFrame() {
	FrameWaitBudget budget(GRACE_WAIT_BUDGET_US);
	Check(budget.Left(3) == GRACE_WAIT_BUDGET_US, "a frame starts with the whole short-wait budget");
	budget.Spend(3, GRACE_WAIT_BUDGET_US - 500);
	Check(GraceWaitUs(UINT64_MAX, 3, budget.Left(3)) == 500,
	      "a wait was longer than what is left of the frame's budget");
	budget.Spend(3, 500);
	Check(GraceWaitUs(UINT64_MAX, 3, budget.Left(3)) == 0,
	      "a draw waited after the frame's budget was spent");
	Check(budget.Left(4) == GRACE_WAIT_BUDGET_US, "the next frame has the whole budget again");
}

void TestBuildersLeaveCoresForTheGame() {
	Check(PipelineBuilderCount(14) == 10, "a 14-core host builds on 10 threads, 4 cores stay free");
	Check(PipelineBuilderCount(8) == 4, "an 8-core host builds on 4 threads");
	Check(PipelineBuilderCount(32) == 10, "no more than 10 build threads: nothing past 10 was measured");
	Check(PipelineBuilderCount(4) == 1 && PipelineBuilderCount(2) == 1,
	      "a small host still builds on one thread");
	Check(PipelineBuilderCount(0) == 1, "an unknown core count builds on one thread");
}

void TestTheTitleCountsPipelinesBuiltAhead() {
	AheadBuildProgress progress;
	Check(!progress.Building(), "a start without a list shows no count");
	progress.Listed(3);
	const auto started = progress.Building();
	Check(started && started->built == 0 && started->total == 3,
	      "a list of 3 pipelines shows 0 of 3 built");
	progress.Built();
	progress.Built();
	const auto two = progress.Building();
	Check(two && two->built == 2 && two->total == 3, "two built pipelines show 2 of 3");
	progress.Built();
	Check(!progress.Building(), "the count goes away once the list is built");
	progress.Listed(0);
	Check(!progress.Building(), "an empty list shows no count");
}

} // namespace

int main() {
	TestAReadyPipelineDraws();
	TestAVisualDrawIsLeftOutWhileItsPipelineBuilds();
	TestADrawWithEffectsBeyondThePictureWaits();
	TestADrawIntoATargetResetThisFrameWaitsWithinTheBudget();
	TestTheWaitBudgetIsPerFrame();
	TestTargetHistoryTellsResetsApart();
	TestAResetAcrossAFrameGapIsKept();
	TestAPendingUploadCountsAsAReset();
	TestEveryDrawWaitsWhenAsked();
	TestAPipelineWaitsOnceAFrame();
	TestShortWaitsHaveABudgetPerFrame();
	TestBuildersLeaveCoresForTheGame();
	TestTheTitleCountsPipelinesBuiltAhead();
	if (g_failures != 0) {
		std::cerr << g_failures << " check(s) failed\n";
		return 1;
	}
	std::cout << "pipeline use: all checks passed\n";
	return 0;
}
