// Tests for the rule that a resource is destroyed only after every command buffer of the queue.

#include "graphics/host_gpu/queueCommits.h"

#include <iostream>

namespace {

using Libs::Graphics::QueueCommits;

int g_failures = 0;

void Check(bool condition, const char* message) {
	if (!condition) {
		std::cerr << "FAILED: " << message << '\n';
		g_failures++;
	}
}

void TestEveryTimelineMustBeDone() {
	uint64_t     render_done  = 0;
	uint64_t     present_done = 0;
	QueueCommits queue;
	const auto   render  = queue.AddTimeline([&](uint64_t tick) { return tick <= render_done; });
	const auto   present = queue.AddTimeline([&](uint64_t tick) { return tick <= present_done; });
	Check(queue.AllDone(), "an empty queue is done");
	queue.NoteSubmit(render, 5);
	queue.NoteSubmit(present, 2);
	render_done = 5;
	Check(!queue.AllDone(), "the other submitter's command buffer is still running");
	present_done = 2;
	Check(queue.AllDone(), "both timelines reached their last submission");
	queue.RemoveTimeline(present);
	queue.NoteSubmit(render, 6);
	render_done = 6;
	Check(queue.AllDone(), "a removed timeline holds nothing");
}

void TestAPresentNeedsALaterSubmission() {
	uint64_t     done = 0;
	QueueCommits queue;
	const auto   render = queue.AddTimeline([&](uint64_t tick) { return tick <= done; });
	queue.NoteSubmit(render, 1);
	done = 1;
	queue.NotePresent();
	Check(!queue.AllDone(), "a present signals nothing, so its completion is unknown");
	queue.NoteSubmit(render, 2);
	Check(!queue.AllDone(), "the submission after the present is still running");
	done = 2;
	Check(queue.AllDone(), "the queue runs in order: the present completed before it");
}

// A wait for the idle queue completes the present too: no later submission is needed.
void TestAnIdleQueueCompletesAPresent() {
	uint64_t     done = 0;
	QueueCommits queue;
	const auto   render = queue.AddTimeline([&](uint64_t tick) { return tick <= done; });
	queue.NoteSubmit(render, 1);
	done = 1;
	queue.NotePresent();
	Check(!queue.AllDone(), "a present signals nothing, so its completion is unknown");
	queue.NoteIdle();
	Check(queue.AllDone(), "the idle queue still held the present");
}

} // namespace

int main() {
	TestEveryTimelineMustBeDone();
	TestAPresentNeedsALaterSubmission();
	TestAnIdleQueueCompletesAPresent();
	if (g_failures != 0) {
		std::cerr << "QueueCommitsTests: " << g_failures << " check(s) failed\n";
		return 1;
	}
	std::cout << "QueueCommitsTests: all cases passed\n";
	return 0;
}
