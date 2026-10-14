// Tests for the bookkeeping that tells when a submission has retired. Fake fences stand in for
// the GPU, so no device is needed.

#include "graphics/host_gpu/renderer/fenceRetirement.h"

#include <cstdint>
#include <cstdio>
#include <functional>
#include <set>
#include <span>

namespace {

using namespace Libs::Graphics;

int g_failures = 0;

void Check(bool condition, const char* message) {
	if (!condition) {
		std::fprintf(stderr, "FAILED: %s\n", message);
		g_failures++;
	}
}

// What the fake GPU knows about its fences. A fence is an int.
struct FakeGpu {
	int                   next = 1;
	std::set<int>         signaled;
	std::set<int>         destroyed;
	std::set<int>         timed_out_once;
	int                   waiting_on = 0; // the fence a thread waits on, or 0
	bool                  reset_while_waited = false;
	int                   resets             = 0;
	std::function<void()> during_wait; // what another thread does while one waits
};

struct FakeOps {
	FakeGpu* gpu;

	[[nodiscard]] int  Create() const { return gpu->next++; }
	[[nodiscard]] bool IsSignaled(int fence) const { return gpu->signaled.contains(fence); }
	void               Reset(std::span<const int> fences) const {
        gpu->resets++;
        for (const auto fence: fences) {
            gpu->reset_while_waited |= fence == gpu->waiting_on;
            gpu->signaled.erase(fence);
        }
	}
	void Wait(int fence) const {
		gpu->waiting_on = fence;
		if (gpu->during_wait) {
			auto action = std::move(gpu->during_wait);
			gpu->during_wait = nullptr;
			action();
		}
		// The first wait on a fence returns early, as a bounded wait may; the next one sees the
		// GPU finish the submission.
		if (!gpu->timed_out_once.insert(fence).second) {
			gpu->signaled.insert(fence);
		}
		gpu->waiting_on = 0;
	}
	void Destroy(int fence) const { gpu->destroyed.insert(fence); }
};

using Retirement = FenceRetirement<int, FakeOps>;

void TestRetiresInOrder() {
	FakeGpu    gpu;
	Retirement retirement {FakeOps {&gpu}};
	const auto a = retirement.Acquire(1);
	const auto b = retirement.Acquire(2);
	Check(!retirement.IsRetired(1), "an unsignalled submission is not retired");
	gpu.signaled.insert(b);
	Check(!retirement.IsRetired(2), "a later signalled fence does not retire past an earlier one");
	gpu.signaled.insert(a);
	Check(retirement.IsRetired(2), "both retire once the earlier one is signalled");
	Check(retirement.IsRetired(1), "an earlier tick stays retired");
}

void TestFenceIsReusedAfterReset() {
	FakeGpu    gpu;
	Retirement retirement {FakeOps {&gpu}};
	const auto a = retirement.Acquire(1);
	gpu.signaled.insert(a);
	Check(retirement.IsRetired(1), "the submission retires");
	const auto b = retirement.Acquire(2);
	Check(b == a, "a retired fence is reused");
	Check(gpu.resets == 1 && !gpu.signaled.contains(b), "it was reset before reuse");
	Check(!retirement.IsRetired(2), "the reused fence starts unsignalled");
}

void TestNoResetWhileAThreadWaits() {
	FakeGpu    gpu;
	Retirement retirement {FakeOps {&gpu}};
	const auto a = retirement.Acquire(1);
	int        acquired_during_wait = 0;
	// While one thread waits on the oldest fence, another sees it signalled, retires it and
	// submits again.
	gpu.during_wait = [&] {
		gpu.signaled.insert(a);
		Check(retirement.IsRetired(1), "the other thread retires the waited fence");
		acquired_during_wait = retirement.Acquire(2);
	};
	retirement.WaitRetired(1);
	Check(!gpu.reset_while_waited, "the waited fence was not reset during the wait");
	Check(acquired_during_wait != a, "the waited fence was not handed to a new submission");
	Check(gpu.resets == 0, "no fence was reset while a thread waited");
	// With no waiter left, the next submission may recycle it.
	gpu.signaled.insert(acquired_during_wait);
	Check(retirement.IsRetired(2), "the second submission retires");
	const auto c = retirement.Acquire(3);
	Check(gpu.resets == 1, "retired fences are reset once nobody waits");
	Check(c == a || c == acquired_during_wait, "and reused");
}

void TestDestroysEveryFence() {
	FakeGpu gpu;
	{
		Retirement retirement {FakeOps {&gpu}};
		const auto a = retirement.Acquire(1);
		(void)retirement.Acquire(2);
		gpu.signaled.insert(a);
		(void)retirement.IsRetired(1);
	}
	Check(gpu.destroyed.size() == 2, "pending and retired fences are destroyed");
}

} // namespace

int main() {
	TestRetiresInOrder();
	TestFenceIsReusedAfterReset();
	TestNoResetWhileAThreadWaits();
	TestDestroysEveryFence();
	if (g_failures != 0) {
		std::fprintf(stderr, "%d check(s) failed\n", g_failures);
		return 1;
	}
	std::puts("fence retirement tests passed");
	return 0;
}
