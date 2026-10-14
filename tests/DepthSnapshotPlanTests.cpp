// Tests for the rule that decides whether a depth-bounds draw reads the previous depth snapshot.

#include "graphics/host_gpu/depthSnapshotPlan.h"

#include <iostream>

namespace {

using Libs::Graphics::DepthSnapshotState;
using Libs::Graphics::DepthSnapshotStep;
using Libs::Graphics::PlanDepthSnapshot;

int g_failures = 0;

void Check(bool condition, const char* message) {
	if (!condition) {
		std::cerr << "FAILED: " << message << '\n';
		g_failures++;
	}
}

DepthSnapshotState Held() {
	return {.target_image       = 7,
	        .content_generation = 3,
	        .load_clear         = false,
	        .cached             = true,
	        .cached_target_image = 7,
	        .cached_generation  = 3,
	        .cached_load_clear  = false};
}

void TestWithoutASnapshotTheTargetIsCopied() {
	Check(PlanDepthSnapshot({.target_image = 7}) == DepthSnapshotStep::Copy,
	      "the first draw copies the depth target");
	Check(PlanDepthSnapshot({.target_image = 7, .load_clear = true, .clear_value = 1.0f}) ==
	          DepthSnapshotStep::Fill,
	      "the first draw of a clearing pass fills the clear value");
}

void TestAnUnchangedTargetIsReadAgain() {
	Check(PlanDepthSnapshot(Held()) == DepthSnapshotStep::Reuse,
	      "same target, same generation, same pass: reuse");
}

// The renderer counts every draw of a clearing pass as a write of the depth image (a new
// content generation), and each draw restarts the pass with the clear. The fill does not depend
// on what the image held: the next draw of the pass reads it again, whatever the generation.
void TestAClearingPassReusesTheFillAcrossItsDraws() {
	auto clearing               = Held();
	clearing.load_clear         = true;
	clearing.clear_value        = 0.5f;
	clearing.cached_load_clear  = true;
	clearing.cached_clear_value = 0.5f;
	for (const uint64_t writes : {uint64_t {0}, uint64_t {1}, uint64_t {5}}) {
		clearing.content_generation = clearing.cached_generation + writes;
		Check(PlanDepthSnapshot(clearing) == DepthSnapshotStep::Reuse,
		      "a draw of a clearing pass with the same clear value filled the snapshot again");
	}
	auto other_layer         = clearing;
	other_layer.target_layer = 1;
	Check(PlanDepthSnapshot(other_layer) == DepthSnapshotStep::Fill,
	      "a clearing draw into another layer reused the fill");
}

void TestAWriteToTheTargetEndsTheReuse() {
	auto written = Held();
	written.content_generation++;
	Check(PlanDepthSnapshot(written) == DepthSnapshotStep::Copy,
	      "a write since the snapshot copies again");
}

void TestAnotherTargetEndsTheReuse() {
	auto other         = Held();
	other.target_image = 8;
	Check(PlanDepthSnapshot(other) == DepthSnapshotStep::Copy, "another image copies");
	auto other_layer         = Held();
	other_layer.target_layer = 1;
	Check(PlanDepthSnapshot(other_layer) == DepthSnapshotStep::Copy, "another layer copies");
}

void TestAChangedClearStateEndsTheReuse() {
	auto now_clearing        = Held();
	now_clearing.load_clear  = true;
	now_clearing.clear_value = 1.0f;
	Check(PlanDepthSnapshot(now_clearing) == DepthSnapshotStep::Fill,
	      "a pass that now clears fills instead of reading the copy");
	auto other_value               = now_clearing;
	other_value.cached_load_clear  = true;
	other_value.cached_clear_value = 0.0f;
	Check(PlanDepthSnapshot(other_value) == DepthSnapshotStep::Fill,
	      "another clear value fills again");
	auto no_longer_clearing              = Held();
	no_longer_clearing.cached_load_clear = true;
	Check(PlanDepthSnapshot(no_longer_clearing) == DepthSnapshotStep::Copy,
	      "a held fill does not stand for a pass that does not clear");
}

} // namespace

int main() {
	TestWithoutASnapshotTheTargetIsCopied();
	TestAnUnchangedTargetIsReadAgain();
	TestAClearingPassReusesTheFillAcrossItsDraws();
	TestAWriteToTheTargetEndsTheReuse();
	TestAnotherTargetEndsTheReuse();
	TestAChangedClearStateEndsTheReuse();
	if (g_failures != 0) {
		std::cerr << g_failures << " check(s) failed\n";
		return 1;
	}
	std::cout << "depth snapshot plan tests passed\n";
	return 0;
}
