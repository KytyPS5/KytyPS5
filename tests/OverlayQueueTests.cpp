// When the system overlay drains the queue before it draws.
#include "graphics/presentation/overlayQueue.h"

#include <cstdio>
#include <cstdlib>

namespace {

using Libs::Graphics::OverlayBuffers;
using Libs::Graphics::OverlayDraw;
using Libs::Graphics::OverlayDrawNeedsIdleQueue;

void Check(bool condition, const char* text) {
	if (!condition) {
		std::fprintf(stderr, "OverlayQueueTests: %s\n", text);
		std::abort();
	}
}

void TestOutgrownBuffersNeedAnIdleQueue() {
	Check(OverlayDrawNeedsIdleQueue({.vertices = 101, .indices = 10},
	                                {.vertices = 100, .indices = 100}),
	      "a draw that outgrows the vertex buffer did not drain the queue");
	Check(OverlayDrawNeedsIdleQueue({.vertices = 10, .indices = 101},
	                                {.vertices = 100, .indices = 100}),
	      "a draw that outgrows the index buffer did not drain the queue");
}

// The trophy toast retires its picture: the backend destroys the texture during the draw.
void TestADestroyedTextureNeedsAnIdleQueue() {
	Check(OverlayDrawNeedsIdleQueue({.vertices = 10, .indices = 10, .destroys_texture = true},
	                                {.vertices = 100, .indices = 100}),
	      "a draw that destroys a texture did not drain the queue");
}

void TestADrawThatFitsDoesNotWait() {
	Check(!OverlayDrawNeedsIdleQueue({.vertices = 100, .indices = 100},
	                                 {.vertices = 100, .indices = 100}),
	      "a draw that fits its buffers drained the queue");
	Check(!OverlayDrawNeedsIdleQueue({}, {}), "an empty draw drained the queue");
}

} // namespace

int main() {
	TestOutgrownBuffersNeedAnIdleQueue();
	TestADestroyedTextureNeedsAnIdleQueue();
	TestADrawThatFitsDoesNotWait();
	std::puts("OverlayQueueTests: all cases passed");
	return 0;
}
