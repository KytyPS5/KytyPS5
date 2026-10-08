#include "graphics/presentation/preparedFrameSelection.h"

#include <array>
#include <cstdio>
#include <cstdlib>

namespace {
struct Frame {
	bool compatible, ready;
};
void Check(bool condition, const char* message) {
	if (!condition) {
		std::fprintf(stderr, "PreparedFrameSelectionTests: %s\n", message);
		std::exit(1);
	}
}
size_t Select(std::span<Frame* const> frames) {
	return Libs::Graphics::SelectPreparedFrame(
	    frames, [](const Frame& frame) { return frame.compatible; },
	    [](const Frame& frame) { return frame.ready; });
}
} // namespace

int main() {
	Frame                       pending_match {true, false}, ready_match {true, true};
	Frame                       pending_other {false, false}, ready_other {false, true};
	const std::array<Frame*, 2> matches {&pending_match, &ready_match};
	Check(Select(matches) == 1, "a pending compatible frame hid a ready compatible frame");
	const std::array<Frame*, 2> mixed {&pending_match, &ready_other};
	Check(Select(mixed) == 1, "descriptor reuse waited despite another ready frame");
	const std::array<Frame*, 2> ordered {&ready_other, &ready_match};
	Check(Select(ordered) == 1, "a ready compatible allocation was not preferred");
	const std::array<Frame*, 2> pending {&pending_other, &pending_match};
	Check(Select(pending) == 1, "all-pending fallback lost its compatible allocation");
	const std::array<Frame*, 2> unmatched {&pending_other, &ready_other};
	Check(Select(unmatched) == 1, "an unmatched ready frame was not selected");
	const std::array<Frame*, 2> no_ready {&pending_other, &pending_other};
	Check(Select(no_ready) == 1, "all-pending unmatched fallback changed");
	Check(Select({}) == 0, "empty free list must return its end index");
	std::puts("Prepared frame selection tests passed");
}
