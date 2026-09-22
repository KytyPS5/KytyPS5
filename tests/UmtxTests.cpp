#include "kernel/umtx.h"

#include "libs/errno.h"

#include <atomic>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

namespace {

using namespace Libs::LibKernel::Umtx; // NOLINT(google-build-using-namespace)
using namespace Libs::Posix;           // NOLINT(google-build-using-namespace)

int g_failures = 0;

void Check(bool value, const char* text) {
	if (!value) {
		std::fprintf(stderr, "UmtxTests: failed: %s\n", text);
		g_failures++;
	}
}

void CheckEq(int actual, int expected, const char* text) {
	if (actual != expected) {
		std::fprintf(stderr, "UmtxTests: failed: %s (got %d, expected %d)\n", text, actual,
		             expected);
		g_failures++;
	}
}

// A relative timeout in the shape _umtx_op takes it: uaddr1 is the struct's size, uaddr2 the
// struct itself. A size no larger than sizeof(timespec) selects the plain relative form.
struct RelTimeout {
	GuestTimespec ts {};

	explicit RelTimeout(int64_t micros) {
		ts.tv_sec  = micros / 1000000;
		ts.tv_nsec = (micros % 1000000) * 1000;
	}

	void* Size() { return reinterpret_cast<void*>(sizeof(GuestTimespec)); }
	void* Value() { return &ts; }
};

// The struct _umtx_time form, which carries its own clock id and an absolute flag.
struct UmtxTimeout {
	GuestUmtxTime ut {};

	UmtxTimeout(int64_t sec, int64_t nsec, uint32_t flags, uint32_t clockid) {
		ut.timeout.tv_sec  = sec;
		ut.timeout.tv_nsec = nsec;
		ut.flags           = flags;
		ut.clockid         = clockid;
	}

	void* Size() { return reinterpret_cast<void*>(sizeof(GuestUmtxTime)); }
	void* Value() { return &ut; }
};

void SpinUntilWaiting(const void* addr, int operation, int expected) {
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (WaiterCountForTests(addr, operation) < expected) {
		if (std::chrono::steady_clock::now() > deadline) {
			std::fprintf(stderr, "UmtxTests: failed: waiters never appeared on %p op %d\n", addr,
			             operation);
			g_failures++;
			return;
		}
		std::this_thread::yield();
	}
}

// -------------------------------------------------------------------------------------------
// UMTX_OP_WAIT / WAKE and the 32-bit private variants
// -------------------------------------------------------------------------------------------

void TestWaitValueMismatchReturnsImmediately() {
	uint64_t word = 7;
	CheckEq(Op(&word, UMTX_OP_WAIT, 9, nullptr, nullptr), OK,
	        "WAIT with a mismatched value returns 0 without sleeping");

	uint32_t word32 = 7;
	CheckEq(Op(&word32, UMTX_OP_WAIT_UINT, 9, nullptr, nullptr), OK,
	        "WAIT_UINT with a mismatched value returns 0 without sleeping");
	CheckEq(Op(&word32, UMTX_OP_WAIT_UINT_PRIVATE, 9, nullptr, nullptr), OK,
	        "WAIT_UINT_PRIVATE with a mismatched value returns 0 without sleeping");
}

void TestWaitTimesOut() {
	uint64_t   word = 3;
	RelTimeout timeout(30000); // 30 ms

	const auto start = std::chrono::steady_clock::now();
	CheckEq(Op(&word, UMTX_OP_WAIT, 3, timeout.Size(), timeout.Value()), POSIX_ETIMEDOUT,
	        "WAIT on a matching value times out");
	const auto elapsed = std::chrono::steady_clock::now() - start;
	Check(elapsed >= std::chrono::milliseconds(20), "WAIT actually waited for its timeout");
	CheckEq(WaiterCountForTests(&word, UMTX_OP_WAIT), 0, "a timed-out WAIT leaves no waiter");
}

void TestWaitAbsoluteTimeoutInThePastTimesOutImmediately() {
	uint32_t    word = 5;
	UmtxTimeout timeout(1, 0, UMTX_ABSTIME, 0 /* CLOCK_REALTIME: 1970, long past */);
	CheckEq(Op(&word, UMTX_OP_WAIT_UINT_PRIVATE, 5, timeout.Size(), timeout.Value()),
	        POSIX_ETIMEDOUT, "an absolute deadline already in the past times out at once");
}

void TestWaitRejectsBadTimespec() {
	uint64_t      word = 1;
	GuestTimespec bad {};
	bad.tv_nsec = 1000000000; // out of range
	CheckEq(Op(&word, UMTX_OP_WAIT, 1, reinterpret_cast<void*>(sizeof(bad)), &bad), POSIX_EINVAL,
	        "WAIT rejects tv_nsec >= 1e9");

	bad.tv_nsec = 0;
	bad.tv_sec  = -1;
	CheckEq(Op(&word, UMTX_OP_WAIT, 1, reinterpret_cast<void*>(sizeof(bad)), &bad), POSIX_EINVAL,
	        "WAIT rejects a negative relative timeout");

	UmtxTimeout unknown_clock(0, 1000, 0, 99);
	CheckEq(Op(&word, UMTX_OP_WAIT, 1, unknown_clock.Size(), unknown_clock.Value()), POSIX_EINVAL,
	        "WAIT rejects an unknown clock id");
}

void TestWaitRejectsBadAddress() {
	CheckEq(Op(nullptr, UMTX_OP_WAIT, 0, nullptr, nullptr), POSIX_EFAULT,
	        "WAIT rejects a null address");
	CheckEq(Op(nullptr, UMTX_OP_WAKE, 1, nullptr, nullptr), POSIX_EFAULT,
	        "WAKE rejects a null address");

	alignas(uint64_t) uint8_t bytes[16] = {};
	CheckEq(Op(bytes + 1, UMTX_OP_WAIT, 0, nullptr, nullptr), POSIX_EFAULT,
	        "WAIT rejects a misaligned address");

	uint64_t word = 0;
	CheckEq(Op(&word, UMTX_OP_WAKE, static_cast<uint64_t>(-1), nullptr, nullptr), POSIX_EINVAL,
	        "WAKE rejects a negative count");
}

void TestWakeReleasesWaiters() {
	uint32_t         word = 1;
	std::atomic<int> done {0};
	std::vector<std::thread> threads;
	int                      results[3] = {-1, -1, -1};

	for (int i = 0; i < 3; i++) {
		threads.emplace_back([&, i]() {
			results[i] = Op(&word, UMTX_OP_WAIT_UINT_PRIVATE, 1, nullptr, nullptr);
			done.fetch_add(1, std::memory_order_release);
		});
	}
	SpinUntilWaiting(&word, UMTX_OP_WAIT_UINT_PRIVATE, 3);

	CheckEq(Op(&word, UMTX_OP_WAKE_PRIVATE, 1, nullptr, nullptr), OK, "WAKE_PRIVATE(1) succeeds");
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (done.load(std::memory_order_acquire) < 1 &&
	       std::chrono::steady_clock::now() < deadline) {
		std::this_thread::yield();
	}
	CheckEq(done.load(std::memory_order_acquire), 1, "WAKE_PRIVATE(1) released exactly one waiter");
	CheckEq(WaiterCountForTests(&word, UMTX_OP_WAIT_UINT_PRIVATE), 2,
	        "two waiters are still parked");

	CheckEq(Op(&word, UMTX_OP_WAKE_PRIVATE, INT_MAX, nullptr, nullptr), OK,
	        "WAKE_PRIVATE(INT_MAX) succeeds");
	for (auto& t: threads) {
		t.join();
	}
	for (int result: results) {
		CheckEq(result, OK, "every woken WAIT returned 0");
	}
}

void TestQueueTypesDoNotCrossWake() {
	// A UMTX_OP_WAIT sleeper and a UMTX_OP_MUTEX_LOCK sleeper on the same word are on different
	// FreeBSD key types and must not wake each other.
	auto* storage = new GuestUmutex {};
	storage->m_owner = 0x11111111u | UMUTEX_CONTESTED;

	std::atomic<bool> woken {false};
	std::thread       waiter([&]() {
        RelTimeout timeout(400000);
        (void)Op(storage, UMTX_OP_MUTEX_WAIT, 0, timeout.Size(), timeout.Value());
        woken.store(true, std::memory_order_release);
	});
	SpinUntilWaiting(storage, UMTX_OP_MUTEX_LOCK, 1);

	CheckEq(Op(storage, UMTX_OP_WAKE, INT_MAX, nullptr, nullptr), OK, "plain WAKE succeeds");
	std::this_thread::sleep_for(std::chrono::milliseconds(40));
	Check(!woken.load(std::memory_order_acquire),
	      "a plain WAKE does not release a mutex-queue waiter");

	// Release it for real.
	__atomic_store_n(&storage->m_owner, UMUTEX_UNOWNED, __ATOMIC_RELEASE);
	CheckEq(Op(storage, UMTX_OP_MUTEX_WAKE, 0, nullptr, nullptr), OK, "MUTEX_WAKE succeeds");
	waiter.join();
	delete storage;
}

// The classic lost-wakeup shape: the waker stores the new value and wakes in a tight loop while
// the waiter compares and enqueues. If the compare and the enqueue were not under one lock, a
// waiter would eventually miss its wakeup and hang; the timeout below turns that hang into a
// reported failure.
void TestCompareThenWaitRace() {
	constexpr int ROUNDS = 400;

	for (int round = 0; round < ROUNDS; round++) {
		uint32_t         word = 0;
		std::atomic<int> result {-2};

		std::thread waiter([&]() {
			RelTimeout timeout(2000000); // 2 s - only reached if a wakeup was lost
			result.store(Op(&word, UMTX_OP_WAIT_UINT_PRIVATE, 0, timeout.Size(), timeout.Value()),
			             std::memory_order_release);
		});

		// No synchronization with the waiter at all: the store may land before the compare,
		// between the compare and the enqueue, or after the sleep has begun.
		std::this_thread::yield();
		__atomic_store_n(&word, 1u, __ATOMIC_RELEASE);
		(void)Op(&word, UMTX_OP_WAKE_PRIVATE, INT_MAX, nullptr, nullptr);

		waiter.join();
		const int value = result.load(std::memory_order_acquire);
		if (value != OK) {
			std::fprintf(stderr,
			             "UmtxTests: failed: compare-then-wait lost a wakeup in round %d (%d)\n",
			             round, value);
			g_failures++;
			return;
		}
	}
}

// A signal that lands at the same instant as the deadline must win: reporting ETIMEDOUT for a
// waiter that was already dequeued would consume the wakeup and lose it.
void TestSignalBeatsTimeout() {
	constexpr int ROUNDS = 200;

	for (int round = 0; round < ROUNDS; round++) {
		uint32_t         word = 0;
		std::atomic<int> result {-2};

		std::thread waiter([&]() {
			RelTimeout timeout(3000); // 3 ms
			result.store(Op(&word, UMTX_OP_WAIT_UINT_PRIVATE, 0, timeout.Size(), timeout.Value()),
			             std::memory_order_release);
		});

		std::this_thread::sleep_for(std::chrono::microseconds(2900));
		const bool had_waiter = WaiterCountForTests(&word, UMTX_OP_WAIT_UINT_PRIVATE) > 0;
		(void)Op(&word, UMTX_OP_WAKE_PRIVATE, INT_MAX, nullptr, nullptr);
		waiter.join();

		const int value = result.load(std::memory_order_acquire);
		if (value != OK && value != POSIX_ETIMEDOUT) {
			std::fprintf(stderr, "UmtxTests: failed: unexpected result %d in round %d\n", value,
			             round);
			g_failures++;
			return;
		}
		(void)had_waiter;
	}
}

void TestNWakePrivate() {
	uint32_t words[2] = {0, 0};
	uint64_t addresses[2] = {reinterpret_cast<uint64_t>(&words[0]),
	                         reinterpret_cast<uint64_t>(&words[1])};

	std::atomic<int>         done {0};
	std::vector<std::thread> threads;
	for (int i = 0; i < 2; i++) {
		threads.emplace_back([&, i]() {
			(void)Op(&words[i], UMTX_OP_WAIT_UINT_PRIVATE, 0, nullptr, nullptr);
			done.fetch_add(1, std::memory_order_release);
		});
	}
	SpinUntilWaiting(&words[0], UMTX_OP_WAIT_UINT_PRIVATE, 1);
	SpinUntilWaiting(&words[1], UMTX_OP_WAIT_UINT_PRIVATE, 1);

	CheckEq(Op(addresses, UMTX_OP_NWAKE_PRIVATE, 2, nullptr, nullptr), OK, "NWAKE_PRIVATE succeeds");
	for (auto& t: threads) {
		t.join();
	}
	CheckEq(done.load(std::memory_order_acquire), 2, "NWAKE_PRIVATE woke both addresses");

	CheckEq(Op(addresses, UMTX_OP_NWAKE_PRIVATE, 1000000, nullptr, nullptr), POSIX_EINVAL,
	        "NWAKE_PRIVATE rejects an absurd count");
}

// -------------------------------------------------------------------------------------------
// struct umutex
// -------------------------------------------------------------------------------------------

void TestMutexTrylockAndUnlock() {
	GuestUmutex m {};

	CheckEq(Op(&m, UMTX_OP_MUTEX_TRYLOCK, 0, nullptr, nullptr), OK, "TRYLOCK takes a free mutex");
	Check((m.m_owner & ~UMUTEX_CONTESTED) == CurrentTid(), "the owner word names this thread");

	CheckEq(Op(&m, UMTX_OP_MUTEX_TRYLOCK, 0, nullptr, nullptr), POSIX_EBUSY,
	        "TRYLOCK of an owned mutex returns EBUSY");
	CheckEq(Op(&m, UMTX_OP_MUTEX_LOCK, 0, nullptr, nullptr), POSIX_EDEADLK,
	        "a blocking self-lock returns EDEADLK");

	CheckEq(Op(&m, UMTX_OP_MUTEX_UNLOCK, 0, nullptr, nullptr), OK, "UNLOCK releases it");
	CheckEq(m.m_owner, UMUTEX_UNOWNED, "the owner word is cleared");
	CheckEq(Op(&m, UMTX_OP_MUTEX_UNLOCK, 0, nullptr, nullptr), POSIX_EPERM,
	        "UNLOCK of a free mutex returns EPERM");
}

void TestMutexUnlockByNonOwnerIsEPERM() {
	GuestUmutex m {};
	CheckEq(Op(&m, UMTX_OP_MUTEX_LOCK, 0, nullptr, nullptr), OK, "LOCK takes a free mutex");

	int result = OK;
	std::thread other([&]() { result = Op(&m, UMTX_OP_MUTEX_UNLOCK, 0, nullptr, nullptr); });
	other.join();
	CheckEq(result, POSIX_EPERM, "another thread cannot unlock it");
	CheckEq(Op(&m, UMTX_OP_MUTEX_UNLOCK, 0, nullptr, nullptr), OK, "the owner can");
}

void TestMutexHandoffUnderContention() {
	GuestUmutex      m {};
	std::atomic<int> counter {0};
	std::atomic<int> concurrent {0};
	std::atomic<int> max_concurrent {0};

	constexpr int THREADS    = 6;
	constexpr int ITERATIONS = 300;

	std::vector<std::thread> threads;
	for (int i = 0; i < THREADS; i++) {
		threads.emplace_back([&]() {
			for (int n = 0; n < ITERATIONS; n++) {
				if (Op(&m, UMTX_OP_MUTEX_LOCK, 0, nullptr, nullptr) != OK) {
					g_failures++;
					return;
				}
				const int now = concurrent.fetch_add(1, std::memory_order_acq_rel) + 1;
				int       observed = max_concurrent.load(std::memory_order_relaxed);
				while (now > observed &&
				       !max_concurrent.compare_exchange_weak(observed, now,
				                                             std::memory_order_relaxed)) {
				}
				counter.fetch_add(1, std::memory_order_relaxed);
				concurrent.fetch_sub(1, std::memory_order_acq_rel);
				if (Op(&m, UMTX_OP_MUTEX_UNLOCK, 0, nullptr, nullptr) != OK) {
					g_failures++;
					return;
				}
			}
		});
	}
	for (auto& t: threads) {
		t.join();
	}

	CheckEq(counter.load(), THREADS * ITERATIONS, "every critical section ran");
	CheckEq(max_concurrent.load(), 1, "the mutex never admitted two threads at once");
	CheckEq(m.m_owner, UMUTEX_UNOWNED, "the mutex ends up free");
}

void TestMutexLockTimesOut() {
	GuestUmutex m {};
	CheckEq(Op(&m, UMTX_OP_MUTEX_LOCK, 0, nullptr, nullptr), OK, "the main thread takes it");

	int         result = OK;
	std::thread other([&]() {
		RelTimeout timeout(40000);
		result = Op(&m, UMTX_OP_MUTEX_LOCK, 0, timeout.Size(), timeout.Value());
	});
	other.join();
	CheckEq(result, POSIX_ETIMEDOUT, "a contended timed lock times out");
	Check((m.m_owner & UMUTEX_CONTESTED) != 0,
	      "the timed-out waiter left the contested bit behind");
	CheckEq(Op(&m, UMTX_OP_MUTEX_UNLOCK, 0, nullptr, nullptr), OK, "the owner can still unlock");
}

void TestMutexWaitDoesNotAcquire() {
	GuestUmutex m {};
	CheckEq(Op(&m, UMTX_OP_MUTEX_WAIT, 0, nullptr, nullptr), OK,
	        "MUTEX_WAIT on a free mutex returns at once");
	CheckEq(m.m_owner, UMUTEX_UNOWNED, "MUTEX_WAIT did not take ownership");
}

void TestRobustMutexOwnerDead() {
	GuestUmutex m {};
	m.m_flags = UMUTEX_ROBUST;
	__atomic_store_n(&m.m_owner, UMUTEX_RB_OWNERDEAD, __ATOMIC_RELEASE);

	CheckEq(Op(&m, UMTX_OP_MUTEX_LOCK, 0, nullptr, nullptr), POSIX_EOWNERDEAD,
	        "locking a mutex whose owner died reports EOWNERDEAD");
	Check((m.m_owner & ~UMUTEX_CONTESTED) == CurrentTid(),
	      "the EOWNERDEAD acquire still transfers ownership");

	m.m_flags |= UMUTEX_NONCONSISTENT;
	CheckEq(Op(&m, UMTX_OP_MUTEX_UNLOCK, 0, nullptr, nullptr), OK, "unlock of a dead-owner mutex");
	CheckEq(m.m_owner, UMUTEX_RB_NOTRECOV, "a non-consistent unlock marks it unrecoverable");
	CheckEq(Op(&m, UMTX_OP_MUTEX_LOCK, 0, nullptr, nullptr), POSIX_ENOTRECOVERABLE,
	        "an unrecoverable mutex refuses every lock");
}

void TestSetCeiling() {
	GuestUmutex m {};
	CheckEq(Op(&m, UMTX_OP_SET_CEILING, 5, nullptr, nullptr), POSIX_EINVAL,
	        "SET_CEILING refuses a mutex that is not priority-protect");

	m.m_flags       = UMUTEX_PRIO_PROTECT;
	m.m_ceilings[0] = 3;
	uint32_t old    = 0;
	CheckEq(Op(&m, UMTX_OP_SET_CEILING, 5, &old, nullptr), OK, "SET_CEILING swaps the ceiling");
	CheckEq(static_cast<int>(old), 3, "SET_CEILING reports the old ceiling");
	CheckEq(static_cast<int>(m.m_ceilings[0]), 5, "SET_CEILING stored the new ceiling");
	CheckEq(Op(&m, UMTX_OP_SET_CEILING, 99, &old, nullptr), POSIX_EINVAL,
	        "SET_CEILING rejects an out-of-range ceiling");
}

// -------------------------------------------------------------------------------------------
// struct ucond
// -------------------------------------------------------------------------------------------

void TestCvSignalAndBroadcast() {
	GuestUcond  cv {};
	GuestUmutex m {};

	constexpr int            WAITERS = 4;
	std::atomic<int>         woken {0};
	std::vector<std::thread> threads;

	for (int i = 0; i < WAITERS; i++) {
		threads.emplace_back([&]() {
			if (Op(&m, UMTX_OP_MUTEX_LOCK, 0, nullptr, nullptr) != OK) {
				g_failures++;
				return;
			}
			// CV_WAIT drops the mutex for us; the caller retakes it afterwards.
			const int result = Op(&cv, UMTX_OP_CV_WAIT, 0, &m, nullptr);
			if (result != OK) {
				std::fprintf(stderr, "UmtxTests: CV_WAIT returned %d\n", result);
				g_failures++;
			}
			woken.fetch_add(1, std::memory_order_release);
		});
	}
	SpinUntilWaiting(&cv, UMTX_OP_CV_WAIT, WAITERS);
	Check(cv.c_has_waiters != 0, "CV_WAIT publishes c_has_waiters");

	CheckEq(Op(&cv, UMTX_OP_CV_SIGNAL, 0, nullptr, nullptr), OK, "CV_SIGNAL succeeds");
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (woken.load(std::memory_order_acquire) < 1 &&
	       std::chrono::steady_clock::now() < deadline) {
		std::this_thread::yield();
	}
	CheckEq(woken.load(std::memory_order_acquire), 1, "CV_SIGNAL woke exactly one waiter");

	CheckEq(Op(&cv, UMTX_OP_CV_BROADCAST, 0, nullptr, nullptr), OK, "CV_BROADCAST succeeds");
	for (auto& t: threads) {
		t.join();
	}
	CheckEq(woken.load(std::memory_order_acquire), WAITERS, "CV_BROADCAST woke the rest");
	CheckEq(static_cast<int>(cv.c_has_waiters), 0, "the last wakeup cleared c_has_waiters");
}

void TestCvWaitTimesOutAndClearsHasWaiters() {
	GuestUcond  cv {};
	GuestUmutex m {};
	CheckEq(Op(&m, UMTX_OP_MUTEX_LOCK, 0, nullptr, nullptr), OK, "take the mutex first");

	GuestTimespec relative {};
	relative.tv_nsec = 30000000; // 30 ms
	CheckEq(Op(&cv, UMTX_OP_CV_WAIT, 0, &m, &relative), POSIX_ETIMEDOUT, "CV_WAIT times out");
	CheckEq(static_cast<int>(cv.c_has_waiters), 0, "a timed-out CV_WAIT clears c_has_waiters");
	CheckEq(m.m_owner, UMUTEX_UNOWNED, "CV_WAIT released the mutex even though it timed out");
}

void TestCvWaitRejectsAnUnownedMutex() {
	GuestUcond  cv {};
	GuestUmutex m {};
	GuestTimespec relative {};
	relative.tv_nsec = 10000000;
	// The guest must hold the mutex; if it does not, the implicit unlock fails and CV_WAIT must
	// report that rather than parking forever.
	CheckEq(Op(&cv, UMTX_OP_CV_WAIT, 0, &m, &relative), POSIX_EPERM,
	        "CV_WAIT without the mutex held returns EPERM");
	CheckEq(WaiterCountForTests(&cv, UMTX_OP_CV_WAIT), 0, "the failed CV_WAIT left no waiter");
}

// -------------------------------------------------------------------------------------------
// struct urwlock
// -------------------------------------------------------------------------------------------

void TestRwlockReadersShare() {
	GuestUrwlock rw {};
	CheckEq(Op(&rw, UMTX_OP_RW_RDLOCK, 0, nullptr, nullptr), OK, "first read lock");
	CheckEq(Op(&rw, UMTX_OP_RW_RDLOCK, 0, nullptr, nullptr), OK, "second read lock");
	CheckEq(static_cast<int>(rw.rw_state & URWLOCK_MAX_READERS), 2, "two readers are counted");
	CheckEq(Op(&rw, UMTX_OP_RW_UNLOCK, 0, nullptr, nullptr), OK, "first read unlock");
	CheckEq(Op(&rw, UMTX_OP_RW_UNLOCK, 0, nullptr, nullptr), OK, "second read unlock");
	CheckEq(static_cast<int>(rw.rw_state), 0, "the lock ends up free");
	CheckEq(Op(&rw, UMTX_OP_RW_UNLOCK, 0, nullptr, nullptr), POSIX_EPERM,
	        "unlocking a free rwlock returns EPERM");
}

void TestRwlockWriterExcludes() {
	GuestUrwlock rw {};
	CheckEq(Op(&rw, UMTX_OP_RW_WRLOCK, 0, nullptr, nullptr), OK, "write lock");
	Check((rw.rw_state & URWLOCK_WRITE_OWNER) != 0, "the write-owner bit is set");

	int         read_result  = OK;
	int         write_result = OK;
	std::thread reader([&]() {
		RelTimeout timeout(40000);
		read_result = Op(&rw, UMTX_OP_RW_RDLOCK, 0, timeout.Size(), timeout.Value());
	});
	std::thread writer([&]() {
		RelTimeout timeout(40000);
		write_result = Op(&rw, UMTX_OP_RW_WRLOCK, 0, timeout.Size(), timeout.Value());
	});
	reader.join();
	writer.join();
	CheckEq(read_result, POSIX_ETIMEDOUT, "a reader cannot enter while a writer holds it");
	CheckEq(write_result, POSIX_ETIMEDOUT, "a second writer cannot enter either");

	CheckEq(Op(&rw, UMTX_OP_RW_UNLOCK, 0, nullptr, nullptr), OK, "write unlock");
	CheckEq(Op(&rw, UMTX_OP_RW_RDLOCK, 0, nullptr, nullptr), OK, "a reader can enter afterwards");
	CheckEq(Op(&rw, UMTX_OP_RW_UNLOCK, 0, nullptr, nullptr), OK, "and leave again");
}

void TestRwlockHandsOffToWaiters() {
	GuestUrwlock     rw {};
	std::atomic<int> value {0};
	std::atomic<int> readers_seen_final {0};

	CheckEq(Op(&rw, UMTX_OP_RW_WRLOCK, 0, nullptr, nullptr), OK, "writer takes it");

	constexpr int            READERS = 4;
	std::vector<std::thread> threads;
	for (int i = 0; i < READERS; i++) {
		threads.emplace_back([&]() {
			if (Op(&rw, UMTX_OP_RW_RDLOCK, 0, nullptr, nullptr) != OK) {
				g_failures++;
				return;
			}
			if (value.load(std::memory_order_acquire) == 42) {
				readers_seen_final.fetch_add(1, std::memory_order_relaxed);
			}
			(void)Op(&rw, UMTX_OP_RW_UNLOCK, 0, nullptr, nullptr);
		});
	}
	SpinUntilWaiting(&rw, UMTX_OP_RW_RDLOCK, READERS);

	value.store(42, std::memory_order_release);
	CheckEq(Op(&rw, UMTX_OP_RW_UNLOCK, 0, nullptr, nullptr), OK, "writer releases");
	for (auto& t: threads) {
		t.join();
	}
	CheckEq(readers_seen_final.load(), READERS, "every queued reader ran after the writer");
	CheckEq(static_cast<int>(rw.rw_state & URWLOCK_MAX_READERS), 0, "no reader count leaked");
}

void TestRwlockMutualExclusionUnderLoad() {
	GuestUrwlock     rw {};
	std::atomic<int> active_writers {0};
	std::atomic<int> active_readers {0};
	std::atomic<int> violations {0};

	constexpr int            THREADS    = 6;
	constexpr int            ITERATIONS = 200;
	std::vector<std::thread> threads;

	for (int i = 0; i < THREADS; i++) {
		const bool writer = (i % 2) == 0;
		threads.emplace_back([&, writer]() {
			for (int n = 0; n < ITERATIONS; n++) {
				const int op = writer ? UMTX_OP_RW_WRLOCK : UMTX_OP_RW_RDLOCK;
				if (Op(&rw, op, 0, nullptr, nullptr) != OK) {
					violations.fetch_add(1, std::memory_order_relaxed);
					return;
				}
				if (writer) {
					active_writers.fetch_add(1, std::memory_order_acq_rel);
					if (active_writers.load(std::memory_order_acquire) != 1 ||
					    active_readers.load(std::memory_order_acquire) != 0) {
						violations.fetch_add(1, std::memory_order_relaxed);
					}
					active_writers.fetch_sub(1, std::memory_order_acq_rel);
				} else {
					active_readers.fetch_add(1, std::memory_order_acq_rel);
					if (active_writers.load(std::memory_order_acquire) != 0) {
						violations.fetch_add(1, std::memory_order_relaxed);
					}
					active_readers.fetch_sub(1, std::memory_order_acq_rel);
				}
				if (Op(&rw, UMTX_OP_RW_UNLOCK, 0, nullptr, nullptr) != OK) {
					violations.fetch_add(1, std::memory_order_relaxed);
					return;
				}
			}
		});
	}
	for (auto& t: threads) {
		t.join();
	}
	CheckEq(violations.load(), 0, "the rwlock never let a writer overlap anyone");
	CheckEq(static_cast<int>(rw.rw_state & (URWLOCK_WRITE_OWNER | URWLOCK_MAX_READERS)), 0,
	        "the rwlock ends up with no owner and no readers");
}

// -------------------------------------------------------------------------------------------
// struct _usem2
// -------------------------------------------------------------------------------------------

void TestSem2CountedWaitDoesNotSleep() {
	GuestUsem2 sem {};
	sem.count = 2;

	CheckEq(Op(&sem, UMTX_OP_SEM2_WAIT, 0, nullptr, nullptr), OK, "first SEM2_WAIT consumes a unit");
	CheckEq(static_cast<int>(sem.count & USEM_MAX_COUNT), 1, "the count dropped to one");
	CheckEq(Op(&sem, UMTX_OP_SEM2_WAIT, 0, nullptr, nullptr), OK, "second SEM2_WAIT consumes it");
	CheckEq(static_cast<int>(sem.count & USEM_MAX_COUNT), 0, "the count dropped to zero");
}

void TestSem2WaitTimesOutAndWakeReleases() {
	GuestUsem2 sem {};

	RelTimeout timeout(30000);
	CheckEq(Op(&sem, UMTX_OP_SEM2_WAIT, 0, timeout.Size(), timeout.Value()), POSIX_ETIMEDOUT,
	        "SEM2_WAIT on an empty semaphore times out");
	Check((sem.count & USEM_HAS_WAITERS) != 0,
	      "SEM2_WAIT published the waiters flag before sleeping");

	std::atomic<int> result {-2};
	std::thread      waiter([&]() {
        result.store(Op(&sem, UMTX_OP_SEM2_WAIT, 0, nullptr, nullptr), std::memory_order_release);
	});
	SpinUntilWaiting(&sem, UMTX_OP_SEM2_WAIT, 1);

	// A post: bump the count, then hand the waiter over.
	__atomic_fetch_add(&sem.count, 1u, __ATOMIC_ACQ_REL);
	CheckEq(Op(&sem, UMTX_OP_SEM2_WAKE, 0, nullptr, nullptr), OK, "SEM2_WAKE succeeds");
	waiter.join();
	CheckEq(result.load(std::memory_order_acquire), OK, "the woken SEM2_WAIT returned 0");
}

// -------------------------------------------------------------------------------------------
// Operations that must report an error instead of aborting
// -------------------------------------------------------------------------------------------

void TestUnknownOperationsReportErrors() {
	uint64_t word = 0;
	CheckEq(Op(&word, 9999, 0, nullptr, nullptr), POSIX_EINVAL,
	        "an unknown operation returns EINVAL rather than aborting");
	CheckEq(Op(&word, UMTX_OP_RESERVED0, 0, nullptr, nullptr), POSIX_EOPNOTSUPP,
	        "a reserved operation returns EOPNOTSUPP");
	CheckEq(Op(&word, UMTX_OP_SHM, UMTX_SHM_CREAT, &word, nullptr), POSIX_EOPNOTSUPP,
	        "UMTX_OP_SHM reports EOPNOTSUPP rather than aborting");
}

void TestRobustListsIsAccepted() {
	GuestRobustListsParams params {};
	params.robust_list_offset = 0x10;
	CheckEq(Op(nullptr, UMTX_OP_ROBUST_LISTS, sizeof(params), &params, nullptr), OK,
	        "ROBUST_LISTS registration is accepted");
	CheckEq(Op(nullptr, UMTX_OP_ROBUST_LISTS, 7, &params, nullptr), POSIX_EINVAL,
	        "ROBUST_LISTS rejects a wrong size");
}

// A timed wait used to be a hard abort. The whole point of this work is that it is now an
// ordinary, reportable outcome for every operation that takes a timeout.
void TestEveryTimedWaitIsAnswerable() {
	RelTimeout timeout(5000);

	uint64_t word64 = 1;
	CheckEq(Op(&word64, UMTX_OP_WAIT, 1, timeout.Size(), timeout.Value()), POSIX_ETIMEDOUT,
	        "timed UMTX_OP_WAIT");

	uint32_t word32 = 1;
	CheckEq(Op(&word32, UMTX_OP_WAIT_UINT, 1, timeout.Size(), timeout.Value()), POSIX_ETIMEDOUT,
	        "timed UMTX_OP_WAIT_UINT");
	CheckEq(Op(&word32, UMTX_OP_WAIT_UINT_PRIVATE, 1, timeout.Size(), timeout.Value()),
	        POSIX_ETIMEDOUT, "timed UMTX_OP_WAIT_UINT_PRIVATE");

	GuestUmutex m {};
	m.m_owner = 0x22222222u;
	CheckEq(Op(&m, UMTX_OP_MUTEX_LOCK, 0, timeout.Size(), timeout.Value()), POSIX_ETIMEDOUT,
	        "timed UMTX_OP_MUTEX_LOCK");
	CheckEq(Op(&m, UMTX_OP_MUTEX_WAIT, 0, timeout.Size(), timeout.Value()), POSIX_ETIMEDOUT,
	        "timed UMTX_OP_MUTEX_WAIT");

	GuestUrwlock rw {};
	rw.rw_state = URWLOCK_WRITE_OWNER;
	CheckEq(Op(&rw, UMTX_OP_RW_RDLOCK, 0, timeout.Size(), timeout.Value()), POSIX_ETIMEDOUT,
	        "timed UMTX_OP_RW_RDLOCK");
	CheckEq(Op(&rw, UMTX_OP_RW_WRLOCK, 0, timeout.Size(), timeout.Value()), POSIX_ETIMEDOUT,
	        "timed UMTX_OP_RW_WRLOCK");

	GuestUsem2 sem2 {};
	CheckEq(Op(&sem2, UMTX_OP_SEM2_WAIT, 0, timeout.Size(), timeout.Value()), POSIX_ETIMEDOUT,
	        "timed UMTX_OP_SEM2_WAIT");

	GuestUsem sem {};
	CheckEq(Op(&sem, UMTX_OP_SEM_WAIT, 0, timeout.Size(), timeout.Value()), POSIX_ETIMEDOUT,
	        "timed UMTX_OP_SEM_WAIT");
}

} // namespace

int main() {
	TestWaitValueMismatchReturnsImmediately();
	TestWaitTimesOut();
	TestWaitAbsoluteTimeoutInThePastTimesOutImmediately();
	TestWaitRejectsBadTimespec();
	TestWaitRejectsBadAddress();
	TestWakeReleasesWaiters();
	TestQueueTypesDoNotCrossWake();
	TestCompareThenWaitRace();
	TestSignalBeatsTimeout();
	TestNWakePrivate();

	TestMutexTrylockAndUnlock();
	TestMutexUnlockByNonOwnerIsEPERM();
	TestMutexHandoffUnderContention();
	TestMutexLockTimesOut();
	TestMutexWaitDoesNotAcquire();
	TestRobustMutexOwnerDead();
	TestSetCeiling();

	TestCvSignalAndBroadcast();
	TestCvWaitTimesOutAndClearsHasWaiters();
	TestCvWaitRejectsAnUnownedMutex();

	TestRwlockReadersShare();
	TestRwlockWriterExcludes();
	TestRwlockHandsOffToWaiters();
	TestRwlockMutualExclusionUnderLoad();

	TestSem2CountedWaitDoesNotSleep();
	TestSem2WaitTimesOutAndWakeReleases();

	TestUnknownOperationsReportErrors();
	TestRobustListsIsAccepted();
	TestEveryTimedWaitIsAnswerable();

	if (g_failures != 0) {
		std::fprintf(stderr, "UmtxTests: %d failure(s)\n", g_failures);
		return 1;
	}
	std::printf("UmtxTests: all passed\n");
	return 0;
}
