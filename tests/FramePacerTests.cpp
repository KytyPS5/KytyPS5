#include "graphics/presentation/framePacer.h"

#include <cstdio>
#include <cstdlib>

using Libs::Graphics::FramePacer;
namespace {
void Check(bool condition, const char* message) {
	if (!condition) {
		std::fprintf(stderr, "FramePacerTests: %s\n", message);
		std::exit(1);
	}
}
}
int main() {
	constexpr uint64_t frequency = 1000000;
	FramePacer clock(frequency, 0);
	uint64_t now = 0;
	for (int frame = 0; frame < 360; ++frame) {
		now += clock.Remaining(now);
		clock.Advance(now + 100, 360);
	}
	Check(now + clock.Remaining(now) == frequency, "360 Hz accumulates rounding drift");

	FramePacer stalled(frequency, 0);
	stalled.Advance(100000, 60); // A 100 ms shader/UI stall at the real loop boundary.
	now = 100000;
	int burst_frames = 0;
	for (int frame = 0; frame < 10; ++frame) {
		if (stalled.Remaining(now) == 0) ++burst_frames;
		now += stalled.Remaining(now) + 200;
		stalled.Advance(now, 60);
	}
	Check(burst_frames <= 1, "a stall is followed by a burst of accelerated vblanks");

	FramePacer oversleep(frequency, 0);
	oversleep.Advance(200, 60);
	Check(oversleep.Remaining(500) == 16166, "normal frame work is not deducted");
	oversleep.Advance(17166, 60); // Scheduler wakes 500 us late.
	Check(oversleep.Remaining(17166) == 16167, "small oversleep loses cadence");
	oversleep.Advance(17366, 120);
	Check(oversleep.Remaining(17366) == 8333, "refresh changes retain an old deadline");
	std::puts("Frame pacer tests passed");
}
