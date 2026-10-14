// Which GPU-written images are copied back to guest memory for the CPU.
#include "graphics/host_gpu/imageReadback.h"

#include <cstdio>
#include <cstdlib>

namespace {

using Libs::Graphics::ReadsBackToGuest;

void Check(bool condition, const char* text) {
	if (!condition) {
		std::fprintf(stderr, "ImageReadbackTests: %s\n", text);
		std::abort();
	}
}

void TestTinyImagesAreReadBack() {
	// The 1x1 target Unreal Engine renders a sky light's average brightness into is tiled.
	Check(ReadsBackToGuest({.width = 1, .height = 1, .tiled = true}, false),
	      "a tiled 1x1 target was not read back");
	Check(ReadsBackToGuest({.width = 4, .height = 4, .tiled = true}, false),
	      "a tiled 4x4 target was not read back");
	Check(ReadsBackToGuest({.width = 1, .height = 1}, false),
	      "a linear 1x1 target was not read back");
	Check(!ReadsBackToGuest({.width = 5, .height = 1, .tiled = true}, false),
	      "a 5x1 target counted as tiny");
	Check(!ReadsBackToGuest({.width = 1, .height = 5, .tiled = true}, false),
	      "a 1x5 target counted as tiny");
	Check(!ReadsBackToGuest({.width = 4, .height = 4, .tiled = true, .block_compressed = true}, false),
	      "a block-compressed image counted as a readback target");
}

// A tiny array or volume image is not a few bytes: each of its layers or slices is copied.
void TestTinyArraysAndVolumesAreNotReadBack() {
	Check(!ReadsBackToGuest({.width = 4, .height = 4, .layers = 64, .tiled = true}, false),
	      "a 4x4 array of 64 layers counted as tiny");
	Check(!ReadsBackToGuest({.width = 4, .height = 4, .depth = 64, .tiled = true}, false),
	      "a 4x4x64 volume counted as tiny");
}

void TestLargerImagesFollowTheSetting() {
	Check(!ReadsBackToGuest({.width = 64, .height = 64}, false),
	      "a linear 64x64 image was read back with the setting off");
	Check(ReadsBackToGuest({.width = 64, .height = 64}, true),
	      "a linear 64x64 image was not read back with the setting on");
	Check(!ReadsBackToGuest({.width = 64, .height = 64, .tiled = true}, true),
	      "a tiled 64x64 image was read back by the linear setting");
}

} // namespace

int main() {
	TestTinyImagesAreReadBack();
	TestTinyArraysAndVolumesAreNotReadBack();
	TestLargerImagesFollowTheSetting();
	std::puts("ImageReadbackTests: passed");
	return 0;
}
