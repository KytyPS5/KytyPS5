// Tests for the rule that keeps pages the guest writes again and again open.

#include "graphics/host_gpu/writeHeat.h"

#include <iostream>

namespace {

using Heat = Libs::Graphics::WriteHeat<64>;

int g_failures = 0;

void Check(bool condition, const char* message) {
	if (!condition) {
		std::cerr << "FAILED: " << message << '\n';
		g_failures++;
	}
}

void TestAPageIsHotAfterItsSecondWrite() {
	Heat heat;
	heat.NoteWrite(3, 5, 0);
	Check(!heat.IsHot(3) && !heat.IsHot(4), "one write must not make a page hot");
	heat.NoteWrite(4, 6, 0);
	Check(!heat.IsHot(3) && heat.IsHot(4) && !heat.IsHot(5), "only the page written twice is hot");
	heat.NoteWrite(4, 5, 0);
	Check(heat.IsHot(4), "a hot page stays hot");
}

void TestHotPagesStayDirtyWhenTheRangeIsCleared() {
	Heat heat;
	heat.NoteWrite(8, 10, 0);
	heat.NoteWrite(8, 9, 0);
	Heat::Bits dirty;
	dirty.SetRange(8, 12);
	dirty.UnsetRange(8, 12); // the renderer uploaded the range
	heat.KeepHotDirty(dirty, 8, 12, 0);
	Check(dirty.Get(8), "a hot page must stay dirty, so that it is uploaded again");
	Check(!dirty.Get(9) && !dirty.Get(10) && !dirty.Get(11), "a page that is not hot is clean");
}

void TestCoolingEndsHeat() {
	Heat heat;
	heat.NoteWrite(1, 3, 0);
	heat.NoteWrite(1, 3, 0);
	Check(heat.Cool(2, 4), "cooling a range with a hot page must say so");
	Check(heat.IsHot(1) && !heat.IsHot(2), "only the cooled page lost its heat");
	Check(!heat.Cool(2, 4), "a range with no hot page reports none");
	heat.NoteWrite(2, 3, 0);
	Check(!heat.IsHot(2), "a cooled page needs two writes again");
	heat.CoolAll();
	Check(!heat.IsHot(1), "cooling everything ends all heat");
	heat.NoteWrite(1, 2, 0);
	Check(!heat.IsHot(1), "after cooling everything a page needs two writes again");
}

void TestTheLeaseEndsAfterItsFrames() {
	Heat       heat;
	Heat::Bits dirty;
	heat.NoteWrite(5, 6, 10);
	heat.NoteWrite(5, 6, 10);
	Check(heat.IsHot(5), "the second write makes the page hot");
	for (const uint32_t frame: {10u, 10u, 11u, 10u + Heat::LeaseFrames - 1}) {
		dirty.UnsetRange(0, 64);
		heat.KeepHotDirty(dirty, 0, 64, frame);
		Check(dirty.Get(5) && heat.IsHot(5), "the page stays open within its lease, at every use");
	}
	dirty.UnsetRange(0, 64);
	heat.KeepHotDirty(dirty, 0, 64, 10 + Heat::LeaseFrames);
	Check(!dirty.Get(5) && !heat.IsHot(5),
	      "at its last upload the page is clean again, so it is protected before the copy");
}

// A page the game writes every frame comes back right after its lease: one write heats it again,
// so it costs one fault per lease, not two.
void TestAPageThatComesBackAfterItsLeaseIsHotAgainAtOnce() {
	Heat       heat;
	Heat::Bits dirty;
	heat.NoteWrite(5, 6, 0);
	heat.NoteWrite(5, 6, 0);
	heat.KeepHotDirty(dirty, 0, 64, Heat::LeaseFrames);
	Check(!heat.IsHot(5), "the lease ended");
	heat.NoteWrite(5, 6, Heat::LeaseFrames + 1);
	Check(heat.IsHot(5), "one write after the lease did not heat the page again");
	dirty.UnsetRange(0, 64);
	heat.KeepHotDirty(dirty, 0, 64, 2 * Heat::LeaseFrames);
	Check(dirty.Get(5), "the new lease does not run from the write that started it");
}

void TestTheLeaseSurvivesFrameCounterWrap() {
	Heat       heat;
	Heat::Bits dirty;
	heat.NoteWrite(1, 2, 0xffffffffu);
	heat.NoteWrite(1, 2, 0xffffffffu);
	heat.KeepHotDirty(dirty, 0, 64, 0);
	Check(dirty.Get(1), "a lease that crosses the counter wrap is still open");
	dirty.UnsetRange(0, 64);
	heat.KeepHotDirty(dirty, 0, 64, 0xffffffffu + Heat::LeaseFrames);
	Check(!dirty.Get(1), "and it ends after its frames");
}

} // namespace

int main() {
	TestAPageIsHotAfterItsSecondWrite();
	TestHotPagesStayDirtyWhenTheRangeIsCleared();
	TestCoolingEndsHeat();
	TestTheLeaseEndsAfterItsFrames();
	TestAPageThatComesBackAfterItsLeaseIsHotAgainAtOnce();
	TestTheLeaseSurvivesFrameCounterWrap();
	if (g_failures != 0) {
		std::cerr << "WriteHeatTests: " << g_failures << " check(s) failed\n";
		return 1;
	}
	std::cout << "WriteHeatTests: all cases passed\n";
	return 0;
}
