// Regression tests for the condition-variable layer that guest signal delivery runs through.
//
// Two defects are covered, both of which shipped:
//
//   1. CondVar::SignalThread woke its targets while holding the waiter-registry mutex. Waking is
//      not guaranteed non-blocking, and the waiters it retires leave through
//      UnregisterCondWaiter, which needs that same mutex, so the waker deadlocked against every
//      waiter it was trying to wake.
//   2. Fixing that by collecting the targets and waking with the lock released introduced a
//      use-after-free: between the two, a waiter can return, unregister, and have its owning
//      CondVar destroyed. The registry now holds a shared_ptr. Run under ASan to catch a
//      regression here -- without it the stress cases below are only a no-crash smoke test.

#include "common/threads.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>
#include <vector>

namespace {

using Common::CondVar;
using Common::HleCriticalSection;
using Common::Mutex;

void Check(bool value, const char* text) {
	if (!value) {
		std::fprintf(stderr, "SignalSafetyTests: failed: %s\n", text);
		std::abort();
	}
}

// Defect 2. A waiter repeatedly creates a CondVar, parks on it briefly, then destroys it, while a
// second thread hammers SignalThread against that waiter's id. The destruction races the wake; if
// SignalThread does not hold the private object alive across the gap, the wake touches freed
// memory.
void TestSignalThreadDoesNotOutliveCondVar() {
	std::atomic_bool  stop {false};
	std::atomic<int>  waiter_tid {0};
	std::atomic<bool> waiter_ready {false};

	std::thread waiter([&] {
		waiter_tid.store(Common::Thread::GetThreadIdUnique(), std::memory_order_release);
		waiter_ready.store(true, std::memory_order_release);
		while (!stop.load(std::memory_order_acquire)) {
			// Fresh objects each round so the destroy lands in the signaller's wake window.
			auto mutex = std::make_unique<Mutex>();
			auto cond  = std::make_unique<CondVar>();
			mutex->Lock();
			(void)cond->WaitFor(mutex.get(), 200);
			mutex->Unlock();
			cond.reset();
			mutex.reset();
		}
	});

	while (!waiter_ready.load(std::memory_order_acquire)) {
		std::this_thread::yield();
	}
	const int target = waiter_tid.load(std::memory_order_acquire);
	Check(target != 0, "waiter reported a thread id");

	std::thread signaller([&] {
		while (!stop.load(std::memory_order_acquire)) {
			CondVar::SignalThread(target);
		}
	});

	std::this_thread::sleep_for(std::chrono::milliseconds(750));
	stop.store(true, std::memory_order_release);
	signaller.join();
	waiter.join();
}

// Same defect, wider window: many waiters on distinct condvars, all torn down while a signaller
// walks the registry.
void TestSignalThreadWithConcurrentTeardown() {
	constexpr int WAITER_COUNT = 8;

	std::atomic_bool         stop {false};
	std::vector<std::thread> waiters;
	std::atomic<int>         ids[WAITER_COUNT];
	for (auto& id: ids) {
		id.store(0, std::memory_order_relaxed);
	}

	waiters.reserve(WAITER_COUNT);
	for (int i = 0; i < WAITER_COUNT; i++) {
		waiters.emplace_back([&, i] {
			ids[i].store(Common::Thread::GetThreadIdUnique(), std::memory_order_release);
			while (!stop.load(std::memory_order_acquire)) {
				Mutex   mutex;
				CondVar cond;
				mutex.Lock();
				(void)cond.WaitFor(&mutex, 100);
				mutex.Unlock();
			}
		});
	}

	std::thread signaller([&] {
		while (!stop.load(std::memory_order_acquire)) {
			for (auto& id: ids) {
				if (const int target = id.load(std::memory_order_acquire); target != 0) {
					CondVar::SignalThread(target);
				}
			}
		}
	});

	std::this_thread::sleep_for(std::chrono::milliseconds(750));
	stop.store(true, std::memory_order_release);
	signaller.join();
	for (auto& thread: waiters) {
		thread.join();
	}
}

// Defect 1, end to end: SignalThread must wake a waiter rather than deadlock against it, and must
// cut a long wait short instead of letting it time out. A regression here hangs this test rather
// than failing it, which is the honest shape for a deadlock.
void TestSignalThreadWakesAWaiter() {
	Mutex   mutex;
	CondVar cond;

	std::atomic<int>  waiter_tid {0};
	std::atomic_bool  waiting {false};
	std::atomic<bool> returned {false};

	std::thread waiter([&] {
		waiter_tid.store(Common::Thread::GetThreadIdUnique(), std::memory_order_release);
		mutex.Lock();
		waiting.store(true, std::memory_order_release);
		(void)cond.WaitFor(&mutex, 3000000); // 3 s
		mutex.Unlock();
		returned.store(true, std::memory_order_release);
	});

	while (!waiting.load(std::memory_order_acquire)) {
		std::this_thread::yield();
	}
	std::this_thread::sleep_for(std::chrono::milliseconds(50));

	const auto start = std::chrono::steady_clock::now();
	CondVar::SignalThread(waiter_tid.load(std::memory_order_acquire));
	waiter.join();
	const auto elapsed = std::chrono::steady_clock::now() - start;

	Check(returned.load(std::memory_order_acquire), "the waiter returned");
	Check(elapsed < std::chrono::milliseconds(1500),
	      "SignalThread cuts the wait short rather than letting it time out");
}

// An unsignalled WaitFor still honours its timeout.
void TestWaitForTimesOut() {
	Mutex   mutex;
	CondVar cond;

	const auto start = std::chrono::steady_clock::now();
	mutex.Lock();
	const bool signalled = cond.WaitFor(&mutex, 100000); // 100 ms
	mutex.Unlock();
	const auto elapsed = std::chrono::steady_clock::now() - start;

	Check(!signalled, "an unsignalled WaitFor reports a timeout");
	Check(elapsed >= std::chrono::milliseconds(80), "WaitFor honours its timeout");
}

// HleCriticalSection is what pending-signal dispatch consults before running a guest handler, so
// its depth accounting has to survive nesting.
void TestCriticalSectionNesting() {
	Check(!Common::InHleCriticalSection(), "not in a critical section to begin with");
	{
		HleCriticalSection outer;
		Check(Common::InHleCriticalSection(), "inside the outer section");
		{
			HleCriticalSection inner;
			Check(Common::InHleCriticalSection(), "inside the nested section");
		}
		Check(Common::InHleCriticalSection(), "still inside after the nested section ends");
	}
	Check(!Common::InHleCriticalSection(), "outside once the outer section ends");
}

} // namespace

int main() {
	TestCriticalSectionNesting();
	TestWaitForTimesOut();
	TestSignalThreadWakesAWaiter();
	TestSignalThreadDoesNotOutliveCondVar();
	TestSignalThreadWithConcurrentTeardown();

	std::printf("SignalSafetyTests: all passed\n");
	return 0;
}
