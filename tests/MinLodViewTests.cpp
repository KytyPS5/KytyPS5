#include "graphics/host_gpu/renderer/image/minLodView.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace {

using namespace Libs::Graphics;

void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "MinLodViewTests: failed: %s\n", message);
		std::exit(1);
	}
}

bool Is(const MinLodViewRange& r, uint32_t base, uint32_t count, uint32_t min_lod) {
	return r.base_level == base && r.level_count == count && r.min_lod == min_lod;
}

// MinLod (U4.8, relative to the view base) -> view base level.
void TestMapping() {
	Check(Is(MapMinLodToBaseLevel(0, 10, 0), 0, 10, 0), "MinLod 0 must leave the view alone");
	Check(Is(MapMinLodToBaseLevel(3, 5, 0), 3, 5, 0), "MinLod 0 with a base must leave the view alone");
	Check(Is(MapMinLodToBaseLevel(0, 10, 256), 1, 9, 0), "MinLod 256 = one level up");
	Check(Is(MapMinLodToBaseLevel(0, 10, 1024), 4, 6, 0), "MinLod 1024 = four levels up");
	Check(Is(MapMinLodToBaseLevel(0, 10, 1100), 5, 5, 0), "MinLod 1100 (4.29) rounds up to 5");
	Check(Is(MapMinLodToBaseLevel(0, 10, 1), 1, 9, 0), "any fraction rounds up");
	Check(Is(MapMinLodToBaseLevel(2, 6, 512), 4, 4, 0), "shift is added to the view base");
	Check(Is(MapMinLodToBaseLevel(0, 4, 256 * 9), 3, 1, 0), "shift stops at the last level");
	Check(Is(MapMinLodToBaseLevel(0, 1, 256), 0, 1, 0), "single-level view keeps its level");
	Check(Is(MapMinLodToBaseLevel(0, 0, 256), 0, 0, 256), "empty view is left for the caller to reject");
}

// one shift formula for the host view and the recompiler (table 0/256/1024/1100/cap).
void TestShiftTable() {
	Check(MinLodShift(12, 0) == 0, "MinLod 0: no shift");
	Check(MinLodShift(12, 256) == 1, "MinLod 256: 1");
	Check(MinLodShift(12, 1024) == 4, "MinLod 1024: 4");
	Check(MinLodShift(12, 1100) == 5, "MinLod 1100 rounds up to 5");
	Check(MinLodShift(3, 256 * 9) == 2, "cap at level_count - 1");
	Check(MinLodShift(0, 256) == 0, "empty view: no shift");
	Check(MinLodShift(10, 1024) == MapMinLodToBaseLevel(0, 10, 1024).base_level, "host mapping uses the same shift");
	Check(MinLodIsFractional(1100) && MinLodIsFractional(1) && !MinLodIsFractional(1024) && !MinLodIsFractional(0),
	      "fraction detection");
}

// The view the host builds after the shift reports this size for lod 0 and this many levels.
uint32_t ViewExtent(uint32_t extent0, uint32_t base_level, uint32_t shift) {
	return std::max(extent0 >> (base_level + shift), 1u);
}

// RESINFO answers from the T#, not from the shifted view.
void TestResinfo() {
	// base 0, last 11, MinLod 1024 on a 2048x2048 texture: the view would say 128x128 / 8 levels.
	const auto r = MakeMinLodResinfo(true, 0, 11, 1024, 2048, 2048, 1);
	Check(r.Active() && r.shift == 4, "MinLod 1024 shifts by 4");
	Check(r.extent[0] == 2048 && r.extent[1] == 2048 && r.levels == 12, "resinfo reports 2048x2048 / 12 levels");
	Check(ViewExtent(2048, 0, r.shift) == 128 && r.levels != 12 - r.shift, "the shifted view reports something else");
	Check(MinLodResinfoExtent(r.extent[0], 0) == 2048 && MinLodResinfoExtent(r.extent[0], 3) == 256 &&
	          MinLodResinfoExtent(r.extent[0], 11) == 1 && MinLodResinfoExtent(r.extent[0], 20) == 1,
	      "extent of mip lod");
	// A T# whose base level is above 0: the extent is that of the base level.
	const auto b = MakeMinLodResinfo(true, 2, 9, 2 * 256 + 512, 1024, 512, 1);
	Check(b.shift == 2 && b.extent[0] == 256 && b.extent[1] == 128 && b.levels == 8, "base level 2, MinLod 2.0 above it");
	// Volume: depth is shifted too.
	const auto v = MakeMinLodResinfo(true, 0, 6, 512, 64, 64, 64);
	Check(v.extent[2] == 64 && v.levels == 7, "3D resinfo keeps depth");
	// No correction needed.
	Check(!MakeMinLodResinfo(true, 0, 11, 0, 2048, 2048, 1).Active(), "MinLod 0: view size is right");
	Check(!MakeMinLodResinfo(false, 0, 11, 1024, 2048, 2048, 1).Active(), "native clamp: view size is right");
	Check(!MakeMinLodResinfo(true, 3, 11, 3 * 256, 2048, 2048, 1).Active(), "MinLod at the base level: no shift");
	Check(!MakeMinLodResinfo(true, 5, 4, 1024, 2048, 2048, 1).Active(), "invalid mip range is left alone");
	Check(MakeMinLodResinfo(true, 0, 3, 256 * 9, 64, 64, 1).shift == 3, "shift capped at the last level");
}

} // namespace

int main() {
	TestMapping();
	TestShiftTable();
	TestResinfo();
	std::puts("MinLodViewTests: ok");
	return 0;
}
