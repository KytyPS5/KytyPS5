#include "graphics/host_gpu/renderer/pipeline/polygonMode.h"

#include <cstdio>
#include <cstdlib>

namespace {

using namespace Libs::Graphics;

void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "PolygonModeTests: failed: %s\n", message);
		std::abort();
	}
}

HW::ModeControl Mode(uint8_t front, uint8_t back) {
	HW::ModeControl mode {};
	mode.poly_mode           = 1;
	mode.polymode_front_ptype = front;
	mode.polymode_back_ptype  = back;
	return mode;
}

void TestNonFillModesWithFeature() {
	Check(ResolvePolygonMode(Mode(0, 0), false, false, true) == vk::PolygonMode::ePoint,
	      "point mode is not preserved when fillModeNonSolid is available");
	Check(ResolvePolygonMode(Mode(1, 1), false, false, true) == vk::PolygonMode::eLine,
	      "line mode is not preserved when fillModeNonSolid is available");
	Check(ResolvePolygonMode(Mode(2, 2), false, false, true) == vk::PolygonMode::eFill,
	      "fill mode changed");
}

void TestFallbackWithoutFeature() {
	// KosmicKrisp has no fillModeNonSolid: line and point modes must degrade to fill.
	Check(ResolvePolygonMode(Mode(0, 0), false, false, false) == vk::PolygonMode::eFill,
	      "point mode does not fall back to fill without fillModeNonSolid");
	Check(ResolvePolygonMode(Mode(1, 1), false, false, false) == vk::PolygonMode::eFill,
	      "line mode does not fall back to fill without fillModeNonSolid");
	Check(ResolvePolygonMode(Mode(1, 2), false, true, false) == vk::PolygonMode::eFill,
	      "culled-face line mode does not fall back to fill");
}

void TestDisabledAndCulledAreFill() {
	HW::ModeControl off {};
	Check(ResolvePolygonMode(off, false, false, true) == vk::PolygonMode::eFill,
	      "poly_mode == 0 is not fill");
	Check(ResolvePolygonMode(Mode(1, 1), true, true, true) == vk::PolygonMode::eFill,
	      "both faces culled is not fill");
}

} // namespace

int main() {
	TestNonFillModesWithFeature();
	TestFallbackWithoutFeature();
	TestDisabledAndCulledAreFill();
	std::puts("PolygonModeTests: ok");
	return 0;
}
