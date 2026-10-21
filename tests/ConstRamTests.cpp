// Which constant RAM ranges a guest packet may write or dump.
#include "graphics/guest_gpu/command_processor/constRam.h"

#include <cstdio>
#include <cstdlib>

namespace {

using Libs::Graphics::CONST_RAM_DWORDS;
using Libs::Graphics::ConstRamRangeFits;

void Check(bool condition, const char* text) {
	if (!condition) {
		std::fprintf(stderr, "ConstRamTests: %s\n", text);
		std::abort();
	}
}

void TestRangesInsideTheRamFit() {
	Check(ConstRamRangeFits(0, CONST_RAM_DWORDS - 1), "a range from the start did not fit");
	Check(ConstRamRangeFits(0xbffc, 1), "the last dword did not fit");
	Check(ConstRamRangeFits(0x100, 0), "an empty range did not fit");
}

// An offset and a length that fit apart can still run past the end together.
void TestARangePastTheEndIsRefused() {
	Check(!ConstRamRangeFits(0xbffc, 2), "a range one dword past the end was accepted");
	Check(!ConstRamRangeFits(0x4000, CONST_RAM_DWORDS - 1), "a long range past the end was accepted");
	Check(!ConstRamRangeFits(0xc000, 0), "an offset past the end was accepted");
	Check(!ConstRamRangeFits(0, CONST_RAM_DWORDS), "a range as long as the RAM was accepted");
}

void TestAnUnalignedOffsetIsRefused() {
	Check(!ConstRamRangeFits(2, 1), "an unaligned offset was accepted");
}

} // namespace

int main() {
	TestRangesInsideTheRamFit();
	TestARangePastTheEndIsRefused();
	TestAnUnalignedOffsetIsRefused();
	std::puts("ConstRamTests: all cases passed");
	return 0;
}
