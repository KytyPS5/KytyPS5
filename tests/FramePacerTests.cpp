// Tests for the bound on guest frames recorded ahead of the host GPU.

#include "graphics/guest_gpu/framePacer.h"

#include <chrono>
#include <iostream>
#include <semaphore>
#include <thread>

namespace {

using Libs::Graphics::FramePacer;

int g_failures = 0;

void Check(bool condition, const char* message) {
	if (!condition) {
		std::cerr << "FAILED: " << message << '\n';
		g_failures++;
	}
}

// True when WaitForRoom returns within `timeout` on another thread. The thread is left to finish
// once the caller makes room.
bool ReturnsWithin(FramePacer& pacer, std::chrono::milliseconds timeout, std::jthread& waiter,
                   std::binary_semaphore& returned) {
	waiter = std::jthread([&pacer, &returned] {
		pacer.WaitForRoom();
		returned.release();
	});
	return returned.try_acquire_for(timeout);
}

// Under the bound, a frame boundary does not wait.
void TestUnderTheBoundDoesNotWait() {
	FramePacer pacer(2);
	pacer.FrameRecorded();
	std::binary_semaphore returned {0};
	std::jthread          waiter;
	Check(ReturnsWithin(pacer, std::chrono::seconds(2), waiter, returned),
	      "one frame in flight of two made the guest wait");
	Check(pacer.InFlight() == 1, "in-flight count after one recorded frame");
}

// At the bound, it waits until the host GPU completes the oldest frame.
void TestAtTheBoundWaitsForACompletion() {
	FramePacer pacer(2);
	pacer.FrameRecorded();
	pacer.FrameRecorded();
	std::binary_semaphore returned {0};
	std::jthread          waiter;
	Check(!ReturnsWithin(pacer, std::chrono::milliseconds(100), waiter, returned),
	      "two frames in flight of two did not make the guest wait");
	pacer.FrameCompleted();
	Check(returned.try_acquire_for(std::chrono::seconds(2)),
	      "a completed frame did not release the waiting guest");
	Check(pacer.InFlight() == 1, "in-flight count after a completion");
}

// No frame recorded yet (no flips: a loading screen that only submits work) never waits.
void TestNoFramesNeverWait() {
	FramePacer pacer(1);
	for (int i = 0; i < 100; i++) {
		pacer.WaitForRoom();
	}
	Check(pacer.InFlight() == 0, "waits recorded frames");
}

// One frame of bound: the guest records the next frame only after the last one completed.
void TestBoundOfOne() {
	FramePacer pacer(1);
	pacer.FrameRecorded();
	std::binary_semaphore returned {0};
	std::jthread          waiter;
	Check(!ReturnsWithin(pacer, std::chrono::milliseconds(100), waiter, returned),
	      "a bound of one let a second frame start");
	pacer.FrameCompleted();
	Check(returned.try_acquire_for(std::chrono::seconds(2)), "completion did not release a bound of one");
}

// Stop releases a waiting guest, and no later boundary waits: shutdown must not hang on a GPU
// that no longer completes frames.
void TestStopReleasesWaiters() {
	FramePacer pacer(1);
	pacer.FrameRecorded();
	std::binary_semaphore returned {0};
	std::jthread          waiter;
	Check(!ReturnsWithin(pacer, std::chrono::milliseconds(100), waiter, returned),
	      "the bound did not hold before stop");
	pacer.Stop();
	Check(returned.try_acquire_for(std::chrono::seconds(2)), "stop did not release the waiting guest");
	pacer.FrameRecorded();
	pacer.FrameRecorded();
	pacer.WaitForRoom();
}

// A completion with no frame in flight (callbacks run during teardown) does not wrap the count.
void TestStrayCompletionDoesNotUnderflow() {
	FramePacer pacer(2);
	pacer.FrameCompleted();
	Check(pacer.InFlight() == 0, "a stray completion changed the in-flight count");
	pacer.FrameRecorded();
	pacer.FrameRecorded();
	std::binary_semaphore returned {0};
	std::jthread          waiter;
	Check(!ReturnsWithin(pacer, std::chrono::milliseconds(100), waiter, returned),
	      "a stray completion raised the bound");
	pacer.FrameCompleted();
	Check(returned.try_acquire_for(std::chrono::seconds(2)), "completion did not release the guest");
}

} // namespace

int main() {
	TestUnderTheBoundDoesNotWait();
	TestAtTheBoundWaitsForACompletion();
	TestNoFramesNeverWait();
	TestBoundOfOne();
	TestStopReleasesWaiters();
	TestStrayCompletionDoesNotUnderflow();
	if (g_failures != 0) {
		std::cerr << g_failures << " frame pacer check(s) failed\n";
		return 1;
	}
	std::cout << "frame pacer tests passed\n";
	return 0;
}
