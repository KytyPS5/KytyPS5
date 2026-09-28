#include "graphics/presentation/videoOutFlipDue.h"

#include <cstdio>
#include <cstdlib>

namespace {

using Libs::VideoOut::IsFlipDueAtVblank;
using Libs::VideoOut::kNoPresentedVblank;

void Check(bool value, const char* text) {
	if (!value) {
		std::fprintf(stderr, "VideoOutFlipDueTests: failed: %s\n", text);
		std::abort();
	}
}

// Absolute modulus pacing used before the relative fix. A Ready frame that
// becomes ready after the interval elapsed can still be "not due" on an odd
// vblank with flip_rate=1.
[[nodiscard]] bool AbsoluteFlipDue(uint64_t vblank_count, int flip_rate) {
	const int interval = flip_rate + 1;
	return interval <= 1 || (vblank_count % static_cast<uint64_t>(interval)) == 0;
}

void TestRateZeroAlwaysDue() {
	Check(IsFlipDueAtVblank(0, 0, kNoPresentedVblank), "rate 0 first present");
	Check(IsFlipDueAtVblank(1, 0, 0), "rate 0 at vblank 1");
	Check(IsFlipDueAtVblank(99, 0, 50), "rate 0 ignores last present");
}

void TestFirstPresentImmediate() {
	Check(IsFlipDueAtVblank(0, 1, kNoPresentedVblank), "first present rate 1");
	Check(IsFlipDueAtVblank(1, 2, kNoPresentedVblank), "first present rate 2");
}

void TestRateOneRequiresIntervalSinceLastPresent() {
	Check(!IsFlipDueAtVblank(1, 1, 0), "rate 1 not due one vblank after present");
	Check(IsFlipDueAtVblank(2, 1, 0), "rate 1 due two vblanks after present");
	Check(!IsFlipDueAtVblank(3, 1, 2), "rate 1 not due one vblank after second present");
	Check(IsFlipDueAtVblank(4, 1, 2), "rate 1 due two vblanks after second present");
}

void TestRelativeDoesNotStarveOddPhase() {
	// Soft-stall shape: last present landed on an even vblank; Flip later samples
	// an odd count after the interval has already elapsed. Absolute modulus still
	// says not due and would defer the Ready frame another tick (or more under
	// load); relative pacing must present.
	constexpr uint64_t last = 15252;
	constexpr uint64_t odd  = 15255;
	Check(!AbsoluteFlipDue(odd, 1), "precondition: absolute modulus not due on odd");
	Check((odd - last) >= 2, "precondition: interval since last present elapsed");
	Check(IsFlipDueAtVblank(odd, 1, last),
	      "Ready frame starved by absolute odd-phase flip_rate check");
}

void TestRateTwoSpacing() {
	Check(!IsFlipDueAtVblank(2, 2, 0), "rate 2 early at vblank 2");
	Check(IsFlipDueAtVblank(3, 2, 0), "rate 2 due at vblank 3");
	Check(!IsFlipDueAtVblank(5, 2, 3), "rate 2 not due before next interval");
	Check(IsFlipDueAtVblank(6, 2, 3), "rate 2 due after interval");
}

void TestPresentPacingCreditIsBounded() {
	using Libs::VideoOut::ClampPresentPacingWait;
	// Absolute sleep used to clamp only to UINT32_MAX us (~4295s). Accumulated
	// credit from fast overlays must not exceed one refresh period.
	constexpr uint64_t period = 16'666;
	Check(ClampPresentPacingWait(0, period) == 0, "zero wait unchanged");
	Check(ClampPresentPacingWait(period / 2, period) == static_cast<int64_t>(period / 2),
	      "sub-period credit unchanged");
	Check(ClampPresentPacingWait(static_cast<int64_t>(period) * 1000, period) ==
	          static_cast<int64_t>(period),
	      "huge credit must clamp to one period");
	Check(ClampPresentPacingWait(-50, period) == -50, "debt unchanged");
}

} // namespace

int main() {
	TestRateZeroAlwaysDue();
	TestFirstPresentImmediate();
	TestRateOneRequiresIntervalSinceLastPresent();
	TestRelativeDoesNotStarveOddPhase();
	TestRateTwoSpacing();
	TestPresentPacingCreditIsBounded();
	std::printf("VideoOutFlipDueTests: all checks passed\n");
	return 0;
}
