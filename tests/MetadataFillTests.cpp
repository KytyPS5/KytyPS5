// Tests for the pieces that let a colour fast clear be resolved from the guest's own metadata fill
// instead of a GPU read-back: the fill code rule and the write watch set. No GPU is needed.

#include "graphics/host_gpu/renderer/image/colorMetadataFill.h"
#include "graphics/host_gpu/writeWatchSet.h"

#include <cstdint>
#include <cstdio>

namespace {

using namespace Libs::Graphics;

int g_failures = 0;

void Check(bool condition, const char* message) {
	if (!condition) {
		std::fprintf(stderr, "FAILED: %s\n", message);
		g_failures++;
	}
}

void TestFillCode() {
	Check(ColorMetadataFillCode(0x00000000u) == 0x00, "zero fill is code 0x00");
	Check(ColorMetadataFillCode(0x40404040u) == 0x40, "repeated 0x40 is code 0x40");
	Check(ColorMetadataFillCode(0xc0c0c0c0u) == 0xc0, "repeated 0xc0 is code 0xc0");
	Check(ColorMetadataFillCode(0xffffffffu) == 0xff, "repeated 0xff is code 0xff");
	Check(!ColorMetadataFillCode(0x40404041u), "a value whose bytes differ is not a code");
	Check(!ColorMetadataFillCode(0x3f800000u), "a float constant is not a code");
}

void TestOverlappingWriteBumpsGeneration() {
	WriteWatchSet set;
	const auto    a      = set.Watch(0x1000, 0x1000);
	const auto    before = set.Generation(a);
	set.NotifyWrite(0x1800, 8);
	Check(set.Generation(a) != before, "a write inside the range changes the generation");
	const auto after = set.Generation(a);
	set.NotifyWrite(0x2000, 8);
	set.NotifyWrite(0x0ff8, 8);
	Check(set.Generation(a) == after, "writes next to the range do not change the generation");
	set.NotifyWrite(0x0ff8, 16);
	Check(set.Generation(a) != after, "a write that straddles the start changes the generation");
	const auto g = set.Generation(a);
	set.NotifyWrite(0x1ffc, 8);
	Check(set.Generation(a) != g, "a write that straddles the end changes the generation");
}

void TestSameRangeSharesId() {
	WriteWatchSet set;
	const auto    a = set.Watch(0x4000, 0x2000);
	Check(a == set.Watch(0x4000, 0x2000), "watching the same range twice returns the same id");
	Check(set.Watch(0x4000, 0x1000) != a, "a different range is a different watch");
}

void TestOverlappingWatchesBothChange() {
	WriteWatchSet set;
	const auto    big   = set.Watch(0x10000, 0x40000);
	const auto    small = set.Watch(0x20000, 0x1000);
	const auto    gb    = set.Generation(big);
	const auto    gs    = set.Generation(small);
	set.NotifyWrite(0x20010, 4);
	Check(set.Generation(big) != gb && set.Generation(small) != gs,
	      "a write inside two overlapping watches changes both");
	const auto gs2 = set.Generation(small);
	set.NotifyWrite(0x30000, 4);
	Check(set.Generation(small) == gs2, "a write outside the small watch leaves it alone");
}

void TestGenerationNeverRepeats() {
	WriteWatchSet set;
	const auto    a     = set.Watch(0x1000, 0x1000);
	const auto    first = set.Generation(a);
	set.NotifyWrite(0x1000, 4);
	const auto second = set.Generation(a);
	set.NotifyWrite(0x1000, 4);
	const auto third = set.Generation(a);
	Check(first != second && second != third && first != third,
	      "every write produces a generation not seen before");
}

void TestLongWatchFoundFromFarWrite() {
	WriteWatchSet set;
	const auto    a = set.Watch(0x100000, 0x80000);
	const auto    g = set.Generation(a);
	(void)set.Watch(0x180000, 0x1000);
	set.NotifyWrite(0x17fff0, 8);
	Check(set.Generation(a) != g, "a write near the end of a long watch is found");
}

void TestUnwatchEndsTheWatchAndReusesTheId() {
	WriteWatchSet set;
	const auto    a      = set.Watch(0x1000, 0x1000);
	const auto    stored = set.Generation(a);
	set.Unwatch(a);
	// The next watch may get the same id. A reader that still holds the old id and generation
	// must not see its record as valid.
	const auto b = set.Watch(0x9000, 0x1000);
	Check(b == a, "an unwatched id is reused");
	Check(set.Generation(b) != stored, "a reused id starts with a generation nobody has seen");
	const auto fresh = set.Generation(b);
	set.NotifyWrite(0x1800, 8);
	Check(set.Generation(b) == fresh, "a write to the old range does not reach the new watch");
	set.NotifyWrite(0x9800, 8);
	Check(set.Generation(b) != fresh, "a write to the new range does");
}

} // namespace

int main() {
	TestUnwatchEndsTheWatchAndReusesTheId();
	TestFillCode();
	TestOverlappingWriteBumpsGeneration();
	TestSameRangeSharesId();
	TestOverlappingWatchesBothChange();
	TestGenerationNeverRepeats();
	TestLongWatchFoundFromFarWrite();
	if (g_failures != 0) {
		std::fprintf(stderr, "%d check(s) failed\n", g_failures);
		return 1;
	}
	std::puts("metadata fill tests passed");
	return 0;
}
