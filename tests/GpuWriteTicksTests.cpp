// Tests for the submissions that last wrote ranges of guest memory on the GPU.

#include "graphics/host_gpu/gpuWriteTicks.h"

#include <iostream>

namespace {

using Libs::Graphics::GpuWriteTicks;

int g_failures = 0;

void Check(bool condition, const char* message) {
	if (!condition) {
		std::cerr << "FAILED: " << message << '\n';
		g_failures++;
	}
}

// A range nobody wrote has no tick; a written one has its submission's.
void TestWrittenRanges() {
	GpuWriteTicks ticks;
	ticks.Add(0x1000, 0x100, 5);
	Check(ticks.LastWrite(0x1000, 4) == 5, "a written range");
	Check(ticks.LastWrite(0x10fc, 4) == 5, "the end of a written range");
	Check(ticks.LastWrite(0x1100, 4) == 0, "just past a written range");
	Check(ticks.LastWrite(0xffc, 4) == 0, "just before a written range");
	Check(ticks.LastWrite(0xffc, 8) == 5, "a range that reaches into a written one");
}

// A later write of part of a range gives that part its tick, and the rest keeps the old one.
void TestOverlappingWrites() {
	GpuWriteTicks ticks;
	ticks.Add(0x1000, 0x1000, 3);
	ticks.Add(0x1400, 0x100, 9);
	Check(ticks.LastWrite(0x1000, 4) == 3, "the part before the newer write");
	Check(ticks.LastWrite(0x1400, 4) == 9, "the newer write");
	Check(ticks.LastWrite(0x1500, 4) == 3, "the part after the newer write");
	Check(ticks.LastWrite(0x1000, 0x1000) == 9, "the whole range has the newest tick");
	ticks.Add(0x0800, 0x2000, 4);
	Check(ticks.LastWrite(0x1400, 4) == 4, "a write over everything replaces older ticks");
}

// Writes the host GPU has completed are forgotten.
void TestForgetCompleted() {
	GpuWriteTicks ticks;
	ticks.Add(0x1000, 0x100, 3);
	ticks.Add(0x2000, 0x100, 7);
	ticks.ForgetCompleted(5);
	Check(ticks.LastWrite(0x1000, 4) == 0, "a completed write was kept");
	Check(ticks.LastWrite(0x2000, 4) == 7, "a pending write was forgotten");
	Check(ticks.Size() == 1, "completed entries remain");
}

} // namespace

int main() {
	TestWrittenRanges();
	TestOverlappingWrites();
	TestForgetCompleted();
	if (g_failures != 0) {
		std::cerr << g_failures << " GPU write tick check(s) failed\n";
		return 1;
	}
	std::cout << "GPU write tick tests passed\n";
	return 0;
}
