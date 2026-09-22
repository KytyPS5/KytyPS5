#include "kernel/umtx.h"

#include "libs/errno.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

// A faithful-enough reimplementation of FreeBSD's sys/kern/kern_umtx.c for a single guest
// process. The guest address space is mapped into this process, so a guest virtual address is
// directly usable as both the wait-queue key and the pointer to the word being compared; that
// collapses FreeBSD's AUTO_SHARE / THREAD_SHARE key distinction, which only exists to make a
// shared-memory mapping hash to the same key in two different address spaces.

namespace Libs::LibKernel::Umtx {

namespace {

using namespace Libs::Posix; // NOLINT(google-build-using-namespace) - POSIX_E* constants

constexpr uint32_t SIGNAL_POLL_MICROS = 10000;

// FreeBSD clock ids, matching the KERNEL_CLOCK_* table in pthread.cpp.
constexpr int CLOCK_ID_REALTIME          = 0;
constexpr int CLOCK_ID_VIRTUAL           = 1;
constexpr int CLOCK_ID_PROF              = 2;
constexpr int CLOCK_ID_MONOTONIC         = 4;
constexpr int CLOCK_ID_UPTIME            = 5;
constexpr int CLOCK_ID_UPTIME_PRECISE    = 7;
constexpr int CLOCK_ID_UPTIME_FAST       = 8;
constexpr int CLOCK_ID_REALTIME_PRECISE  = 9;
constexpr int CLOCK_ID_REALTIME_FAST     = 10;
constexpr int CLOCK_ID_MONOTONIC_PRECISE = 11;
constexpr int CLOCK_ID_MONOTONIC_FAST    = 12;
constexpr int CLOCK_ID_SECOND            = 13;
constexpr int CLOCK_ID_THREAD_CPUTIME_ID = 14;

// Wait-queue types. Two different types on one address are two different queues, exactly as
// FreeBSD's umtx_key.type does: a UMTX_OP_WAIT sleeper and a UMTX_OP_MUTEX_LOCK sleeper on the
// same word must never wake each other.
enum class KeyType : uint32_t {
	SimpleWait = 0, // UMTX_OP_WAIT / WAKE, 64-bit compare
	SimpleWaitU32,  // UMTX_OP_WAIT_UINT / WAIT_UINT_PRIVATE / WAKE_PRIVATE, 32-bit compare
	Mutex,          // struct umutex
	Cv,             // struct ucond
	Rwlock,         // struct urwlock
	Sem,            // struct _usem / _usem2
};

constexpr int QUEUE_SHARED    = 0;
constexpr int QUEUE_EXCLUSIVE = 1;
constexpr int QUEUE_COUNT     = 2;

struct Waiter {
	std::condition_variable cv;
	bool                    queued = false;
	bool                    woken  = false;
	int                     queue  = QUEUE_SHARED;
};

struct KeyState {
	std::vector<Waiter*>    queues[QUEUE_COUNT];
	bool                    busy = false;
	std::condition_variable busy_cv;
	int                     refs = 0;
};

struct Key {
	uintptr_t addr = 0;
	KeyType   type = KeyType::SimpleWait;

	bool operator==(const Key& other) const { return addr == other.addr && type == other.type; }
};

struct KeyHash {
	size_t operator()(const Key& key) const {
		return std::hash<uintptr_t> {}(key.addr) ^ (static_cast<size_t>(key.type) * 0x9e3779b9u);
	}
};

// FreeBSD hashes keys into umtxq_chains so that unrelated addresses do not serialize. The
// important property for correctness is only that one key always maps to one lock.
constexpr size_t BUCKET_COUNT = 128;

struct Bucket {
	std::mutex                                    m;
	std::unordered_map<Key, KeyState*, KeyHash> keys;
};

Bucket& GetBucket(const Key& key) {
	static Bucket buckets[BUCKET_COUNT];
	const auto    index = (KeyHash {}(key) >> 4u) % BUCKET_COUNT;
	return buckets[index];
}

// ---------------------------------------------------------------------------------------------
// Hooks
// ---------------------------------------------------------------------------------------------

uint32_t DefaultTid() {
	static std::atomic<uint32_t> next {100000};
	thread_local uint32_t        id = next.fetch_add(1, std::memory_order_relaxed);
	return id;
}

bool DefaultClockGettime(int clock_id, int64_t* sec, int64_t* nsec) {
	switch (clock_id) {
		case CLOCK_ID_REALTIME:
		case CLOCK_ID_REALTIME_PRECISE:
		case CLOCK_ID_REALTIME_FAST:
		case CLOCK_ID_SECOND: {
			const auto now = std::chrono::system_clock::now().time_since_epoch();
			const auto ns  = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
			*sec           = ns / 1000000000LL;
			*nsec          = ns % 1000000000LL;
			if (clock_id == CLOCK_ID_SECOND) {
				*nsec = 0;
			}
			return true;
		}
		case CLOCK_ID_MONOTONIC:
		case CLOCK_ID_MONOTONIC_PRECISE:
		case CLOCK_ID_MONOTONIC_FAST:
		case CLOCK_ID_UPTIME:
		case CLOCK_ID_UPTIME_PRECISE:
		case CLOCK_ID_UPTIME_FAST: {
			const auto now = std::chrono::steady_clock::now().time_since_epoch();
			const auto ns  = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
			*sec           = ns / 1000000000LL;
			*nsec          = ns % 1000000000LL;
			return true;
		}
		default: return false;
	}
}

tid_func_t           g_tid            = DefaultTid;
signal_poll_func_t   g_signal_poll    = nullptr;
clock_gettime_func_t g_clock_gettime  = DefaultClockGettime;

void PollSignals() {
	if (g_signal_poll != nullptr) {
		g_signal_poll();
	}
}

// ---------------------------------------------------------------------------------------------
// Guest word access
//
// The guest address space lives inside this process, so a guest pointer is directly
// dereferenceable. FreeBSD's fueword/casueword return -1 on a fault; we can only cheaply check
// for null and misalignment, so a genuinely unmapped guest address will fault the host rather
// than produce EFAULT. That is a known divergence, documented in the findings note.
// ---------------------------------------------------------------------------------------------

template <typename T>
bool IsAccessible(const void* address) {
	return address != nullptr &&
	       (reinterpret_cast<uintptr_t>(address) & (alignof(T) - 1u)) == 0;
}

template <typename T>
bool LoadWord(const void* address, T* out) {
	if (!IsAccessible<T>(address)) {
		return false;
	}
	*out = __atomic_load_n(static_cast<const T*>(address), __ATOMIC_ACQUIRE);
	return true;
}

template <typename T>
bool StoreWord(void* address, T value) {
	if (!IsAccessible<T>(address)) {
		return false;
	}
	__atomic_store_n(static_cast<T*>(address), value, __ATOMIC_RELEASE);
	return true;
}

enum class CasResult { Fault, Success, Mismatch };

// Mirrors casueword32(): on Mismatch, *observed holds the value that was actually there.
template <typename T>
CasResult CompareAndSwap(void* address, T expected, T* observed, T desired) {
	if (!IsAccessible<T>(address)) {
		return CasResult::Fault;
	}
	T actual = expected;
	if (__atomic_compare_exchange_n(static_cast<T*>(address), &actual, desired, false,
	                                __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
		*observed = expected;
		return CasResult::Success;
	}
	*observed = actual;
	return CasResult::Mismatch;
}

// ---------------------------------------------------------------------------------------------
// Queue primitives. Every one of these runs with the key's bucket mutex held.
// ---------------------------------------------------------------------------------------------

KeyState* AcquireKeyLocked(Bucket& bucket, const Key& key) {
	auto it = bucket.keys.find(key);
	if (it == bucket.keys.end()) {
		it = bucket.keys.emplace(key, new KeyState).first;
	}
	it->second->refs++;
	return it->second;
}

void ReleaseKeyLocked(Bucket& bucket, const Key& key, KeyState* state) {
	state->refs--;
	if (state->refs != 0 || state->busy || !state->queues[QUEUE_SHARED].empty() ||
	    !state->queues[QUEUE_EXCLUSIVE].empty()) {
		return;
	}
	bucket.keys.erase(key);
	delete state;
}

void InsertLocked(KeyState* state, Waiter* waiter, int queue) {
	waiter->queue  = queue;
	waiter->queued = true;
	waiter->woken  = false;
	state->queues[queue].push_back(waiter);
}

void RemoveLocked(KeyState* state, Waiter* waiter) {
	if (!waiter->queued) {
		return;
	}
	auto& q  = state->queues[waiter->queue];
	const auto it = std::find(q.begin(), q.end(), waiter);
	if (it != q.end()) {
		q.erase(it);
	}
	waiter->queued = false;
}

int CountLocked(const KeyState* state, int queue) {
	return static_cast<int>(state->queues[queue].size());
}

int CountAllLocked(const KeyState* state) {
	return CountLocked(state, QUEUE_SHARED) + CountLocked(state, QUEUE_EXCLUSIVE);
}

// umtxq_signal_queue(): dequeue up to `count` waiters and mark them woken. Dequeuing at signal
// time - not at wake-up time - is what lets a sleeper distinguish "I was signalled" from "I timed
// out", exactly as FreeBSD's UQF_UMTXQ flag does.
int SignalLocked(KeyState* state, int count, int queue) {
	int woken = 0;
	auto& q   = state->queues[queue];
	while (!q.empty() && woken < count) {
		Waiter* waiter = q.front();
		q.erase(q.begin());
		waiter->queued = false;
		waiter->woken  = true;
		waiter->cv.notify_one();
		woken++;
	}
	return woken;
}

// umtxq_busy(): serializes the read-modify-write sequences that different threads perform on the
// same guest word while holding no other lock (setting the contested bit, clearing a waiters
// flag). It is a flag rather than the bucket mutex because those sequences must be able to touch
// guest memory with the bucket mutex dropped.
void BusyLocked(std::unique_lock<std::mutex>& lock, KeyState* state) {
	while (state->busy) {
		state->busy_cv.wait(lock);
	}
	state->busy = true;
}

void UnbusyLocked(KeyState* state) {
	state->busy = false;
	state->busy_cv.notify_all();
}

// ---------------------------------------------------------------------------------------------
// Timeouts
// ---------------------------------------------------------------------------------------------

struct Deadline {
	bool    present  = false;
	bool    absolute = false;
	int     clock_id = CLOCK_ID_REALTIME;
	int64_t sec      = 0;
	int64_t nsec     = 0;
};

bool IsValidInterval(const GuestTimespec& ts) {
	return ts.tv_sec >= 0 && ts.tv_nsec >= 0 && ts.tv_nsec < 1000000000LL;
}

bool IsKnownClock(int clock_id) {
	int64_t sec  = 0;
	int64_t nsec = 0;
	return g_clock_gettime(clock_id, &sec, &nsec);
}

void AddTimespec(int64_t a_sec, int64_t a_nsec, int64_t b_sec, int64_t b_nsec, int64_t* out_sec,
                 int64_t* out_nsec) {
	int64_t sec  = a_sec + b_sec;
	int64_t nsec = a_nsec + b_nsec;
	if (nsec >= 1000000000LL) {
		nsec -= 1000000000LL;
		sec++;
	}
	*out_sec  = sec;
	*out_nsec = nsec;
}

// umtx_abs_timeout_init(): a relative timeout is turned into an absolute one on its own clock
// immediately, so that a slice-by-slice sleep cannot drift.
int MakeDeadline(const GuestTimespec& ts, bool absolute, int clock_id, Deadline* out) {
	if (!IsKnownClock(clock_id)) {
		return POSIX_EINVAL;
	}
	out->present  = true;
	out->absolute = true;
	out->clock_id = clock_id;

	if (absolute) {
		if (ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000LL) {
			return POSIX_EINVAL;
		}
		out->sec  = ts.tv_sec;
		out->nsec = ts.tv_nsec;
		return OK;
	}

	if (!IsValidInterval(ts)) {
		return POSIX_EINVAL;
	}
	int64_t now_sec  = 0;
	int64_t now_nsec = 0;
	if (!g_clock_gettime(clock_id, &now_sec, &now_nsec)) {
		return POSIX_EINVAL;
	}
	AddTimespec(now_sec, now_nsec, ts.tv_sec, ts.tv_nsec, &out->sec, &out->nsec);
	return OK;
}

// Microseconds left before the deadline, or 0 when it has already passed.
uint64_t RemainingMicros(const Deadline& deadline, bool* expired) {
	int64_t now_sec  = 0;
	int64_t now_nsec = 0;
	if (!g_clock_gettime(deadline.clock_id, &now_sec, &now_nsec)) {
		*expired = true;
		return 0;
	}
	const int64_t delta_ns =
	    (deadline.sec - now_sec) * 1000000000LL + (deadline.nsec - now_nsec);
	if (delta_ns <= 0) {
		*expired = true;
		return 0;
	}
	*expired = false;
	return static_cast<uint64_t>(delta_ns) / 1000ull;
}

// umtxq_sleep(): park until signalled or until the deadline. The caller must already hold
// `lock` (the key's bucket mutex) and must already have inserted `waiter` into the queue *and*
// re-checked whatever guest word it is waiting on, under that same lock.
//
// Returns OK when signalled, POSIX_ETIMEDOUT when the deadline expired.
int SleepLocked(std::unique_lock<std::mutex>& lock, Waiter* waiter, const Deadline& deadline) {
	for (;;) {
		if (waiter->woken) {
			return OK;
		}

		uint64_t slice_micros = SIGNAL_POLL_MICROS;
		if (deadline.present) {
			bool       expired   = false;
			const auto remaining = RemainingMicros(deadline, &expired);
			if (expired) {
				return POSIX_ETIMEDOUT;
			}
			slice_micros = std::min<uint64_t>(remaining, SIGNAL_POLL_MICROS);
			if (slice_micros == 0) {
				slice_micros = 1;
			}
		}

		waiter->cv.wait_for(lock, std::chrono::microseconds(slice_micros));

		if (waiter->woken) {
			return OK;
		}
		if (g_signal_poll != nullptr) {
			lock.unlock();
			PollSignals();
			lock.lock();
		}
	}
}

// ---------------------------------------------------------------------------------------------
// A small RAII helper that holds a key reference and its bucket lock.
// ---------------------------------------------------------------------------------------------

class KeyHold {
public:
	explicit KeyHold(const Key& key): m_key(key), m_bucket(GetBucket(key)), m_lock(m_bucket.m) {
		m_state = AcquireKeyLocked(m_bucket, m_key);
	}

	~KeyHold() {
		if (!m_lock.owns_lock()) {
			m_lock.lock();
		}
		ReleaseKeyLocked(m_bucket, m_key, m_state);
	}

	KeyState*                     State() { return m_state; }
	std::unique_lock<std::mutex>& Lock() { return m_lock; }

	KYTY_CLASS_NO_COPY(KeyHold);

private:
	Key                          m_key;
	Bucket&                      m_bucket;
	std::unique_lock<std::mutex> m_lock;
	KeyState*                    m_state = nullptr;
};

// ---------------------------------------------------------------------------------------------
// Timeout argument decoding
//
// For the wait-style operations FreeBSD passes the timeout in uaddr2 and its *size* in uaddr1:
// a size no larger than sizeof(struct timespec) means a plain relative timespec on
// CLOCK_REALTIME, anything larger means a struct _umtx_time carrying its own clock id and an
// UMTX_ABSTIME flag. (umtx_copyin_umtx_time().)
// ---------------------------------------------------------------------------------------------

int DecodeUmtxTime(const void* uaddr1, const void* uaddr2, Deadline* out) {
	if (uaddr2 == nullptr) {
		out->present = false;
		return OK;
	}

	const auto size = reinterpret_cast<size_t>(uaddr1);
	if (size <= sizeof(GuestTimespec)) {
		GuestTimespec ts {};
		if (!IsAccessible<int64_t>(uaddr2)) {
			return POSIX_EFAULT;
		}
		std::memcpy(&ts, uaddr2, sizeof(ts));
		return MakeDeadline(ts, false, CLOCK_ID_REALTIME, out);
	}

	GuestUmtxTime ut {};
	if (!IsAccessible<int64_t>(uaddr2)) {
		return POSIX_EFAULT;
	}
	std::memcpy(&ut, uaddr2, sizeof(ut));
	return MakeDeadline(ut.timeout, (ut.flags & UMTX_ABSTIME) != 0, static_cast<int>(ut.clockid),
	                    out);
}

// UMTX_OP_CV_WAIT takes a plain struct timespec in uaddr2 and takes its abs/clock selection from
// the `val` wflags and the ucond instead. (__umtx_op_cv_wait().)
int DecodeCvTime(const void* uaddr2, GuestTimespec* out, bool* present) {
	if (uaddr2 == nullptr) {
		*present = false;
		return OK;
	}
	if (!IsAccessible<int64_t>(uaddr2)) {
		return POSIX_EFAULT;
	}
	std::memcpy(out, uaddr2, sizeof(*out));
	if (out->tv_nsec < 0 || out->tv_nsec >= 1000000000LL) {
		return POSIX_EINVAL;
	}
	*present = true;
	return OK;
}

// ---------------------------------------------------------------------------------------------
// UMTX_OP_WAIT / WAIT_UINT / WAIT_UINT_PRIVATE / WAKE / WAKE_PRIVATE  (do_wait, kern_umtx_wake)
// ---------------------------------------------------------------------------------------------

template <typename T>
int DoWait(void* obj, T id, const Deadline& deadline, KeyType type) {
	if (!IsAccessible<T>(obj)) {
		return POSIX_EFAULT;
	}

	KeyHold hold({reinterpret_cast<uintptr_t>(obj), type});
	Waiter  waiter;

	// THE ORDERING THAT MAKES THIS RACE-FREE, and it is FreeBSD's:
	//
	//   1. take the key's bucket lock,
	//   2. enqueue this waiter,
	//   3. only *then* read the guest word,
	//   4. compare and sleep, still under the same lock.
	//
	// A waker must take the same bucket lock to find this waiter. So if the waker's store to the
	// guest word happened before our read at step 3, we observe the new value and never sleep;
	// and if it happened after, the waker is necessarily still blocked on the bucket lock we
	// hold, so its signal lands on a waiter that is already queued. There is no window in which
	// the store is visible but the waiter is not.
	InsertLocked(hold.State(), &waiter, QUEUE_SHARED);

	T current {};
	if (!LoadWord<T>(obj, &current)) {
		RemoveLocked(hold.State(), &waiter);
		return POSIX_EFAULT;
	}

	int error = OK;
	if (current == id) {
		error = SleepLocked(hold.Lock(), &waiter, deadline);
	}

	// A signal dequeues us; anything else leaves us queued. Matching FreeBSD, a signal that
	// raced with the timeout wins - a woken waiter must not report ETIMEDOUT, or the wakeup is
	// lost.
	if (!waiter.queued) {
		error = OK;
	} else {
		RemoveLocked(hold.State(), &waiter);
	}
	return error;
}

int DoWake(void* obj, int32_t count, KeyType type) {
	if (obj == nullptr) {
		return POSIX_EFAULT;
	}
	if (count < 0) {
		return POSIX_EINVAL;
	}
	KeyHold hold({reinterpret_cast<uintptr_t>(obj), type});
	SignalLocked(hold.State(), count, QUEUE_SHARED);
	return OK;
}

// ---------------------------------------------------------------------------------------------
// struct umutex  (do_lock_normal / do_unlock_normal / do_wake_umutex / do_wake2_umutex)
//
// Priority-inherit and priority-protect mutexes are routed through the same code. They share the
// owner-word encoding exactly, so mutual exclusion and every guest-visible state transition are
// correct; what is lost is the priority boost, which this emulator has no scheduler to apply.
// ---------------------------------------------------------------------------------------------

constexpr int MUTEX_MODE_LOCK = 0;
constexpr int MUTEX_MODE_TRY  = 1;
constexpr int MUTEX_MODE_WAIT = 2;

uint32_t UnlockValue(uint32_t flags, bool robust_dead) {
	if (robust_dead) {
		return UMUTEX_RB_OWNERDEAD;
	}
	if ((flags & UMUTEX_NONCONSISTENT) != 0) {
		return UMUTEX_RB_NOTRECOV;
	}
	return UMUTEX_UNOWNED;
}

int DoLockMutex(void* obj, const Deadline& deadline, int mode) {
	auto* mutex = static_cast<GuestUmutex*>(obj);
	if (!IsAccessible<uint32_t>(mutex)) {
		return POSIX_EFAULT;
	}

	uint32_t flags = 0;
	if (!LoadWord<uint32_t>(&mutex->m_flags, &flags)) {
		return POSIX_EFAULT;
	}

	const uint32_t id = CurrentTid();

	for (;;) {
		uint32_t owner = 0;
		if (!LoadWord<uint32_t>(&mutex->m_owner, &owner)) {
			return POSIX_EFAULT;
		}

		if (mode == MUTEX_MODE_WAIT) {
			if (owner == UMUTEX_UNOWNED || owner == UMUTEX_CONTESTED ||
			    owner == UMUTEX_RB_OWNERDEAD || owner == UMUTEX_RB_NOTRECOV) {
				return OK;
			}
		} else {
			// A robust mutex whose owner died: hand it over and tell the guest, which is then
			// responsible for marking it consistent or non-recoverable.
			if (owner == UMUTEX_RB_OWNERDEAD) {
				uint32_t observed = 0;
				const auto rv = CompareAndSwap<uint32_t>(&mutex->m_owner, UMUTEX_RB_OWNERDEAD,
				                                         &observed, id | UMUTEX_CONTESTED);
				if (rv == CasResult::Fault) {
					return POSIX_EFAULT;
				}
				if (rv == CasResult::Success) {
					return POSIX_EOWNERDEAD;
				}
				continue;
			}
			if (owner == UMUTEX_RB_NOTRECOV) {
				return POSIX_ENOTRECOVERABLE;
			}

			// The uncontested acquire. Userland normally does this itself; we repeat it because
			// the guest may have raced and released between its own attempt and this syscall.
			uint32_t observed = 0;
			auto     rv = CompareAndSwap<uint32_t>(&mutex->m_owner, UMUTEX_UNOWNED, &observed, id);
			if (rv == CasResult::Fault) {
				return POSIX_EFAULT;
			}
			if (rv == CasResult::Success) {
				return OK;
			}
			owner = observed;

			// Unowned but marked contested: take it and keep the contested bit, so that our own
			// unlock still goes through the slow path and wakes whoever is queued.
			if (owner == UMUTEX_CONTESTED) {
				rv = CompareAndSwap<uint32_t>(&mutex->m_owner, UMUTEX_CONTESTED, &observed,
				                              id | UMUTEX_CONTESTED);
				if (rv == CasResult::Fault) {
					return POSIX_EFAULT;
				}
				if (rv == CasResult::Success) {
					return OK;
				}
				continue;
			}

			if ((owner & ~UMUTEX_CONTESTED) == id) {
				// Re-locking a umutex this thread already owns. libthr counts recursion in
				// userland and never reaches the kernel for a recursive mutex, so this is a
				// genuine self-deadlock.
				return mode == MUTEX_MODE_TRY ? POSIX_EBUSY : POSIX_EDEADLK;
			}

			if (mode == MUTEX_MODE_TRY) {
				return POSIX_EBUSY;
			}
		}

		// Slow path. Same ordering rule as DoWait: enqueue under the bucket lock first, then
		// publish the contested bit, then re-test it under the same lock before sleeping.
		{
			KeyHold hold({reinterpret_cast<uintptr_t>(mutex), KeyType::Mutex});
			Waiter  waiter;
			InsertLocked(hold.State(), &waiter, QUEUE_SHARED);

			uint32_t   observed = 0;
			const auto rv       = CompareAndSwap<uint32_t>(&mutex->m_owner, owner, &observed,
			                                               owner | UMUTEX_CONTESTED);
			if (rv == CasResult::Fault) {
				RemoveLocked(hold.State(), &waiter);
				return POSIX_EFAULT;
			}

			int error = OK;
			if (rv == CasResult::Success || observed == (owner | UMUTEX_CONTESTED)) {
				// The owner word still says what we expected, so an unlock from here on is
				// guaranteed to go through the contested path and find us queued.
				error = SleepLocked(hold.Lock(), &waiter, deadline);
			}

			if (!waiter.queued) {
				error = OK;
			} else {
				RemoveLocked(hold.State(), &waiter);
			}
			if (error != OK) {
				return error;
			}
		}
		// Woken (or the owner word moved under us): retry the acquire.
	}
}

int DoUnlockMutex(void* obj, bool robust_dead) {
	auto* mutex = static_cast<GuestUmutex*>(obj);
	if (!IsAccessible<uint32_t>(mutex)) {
		return POSIX_EFAULT;
	}

	uint32_t flags = 0;
	uint32_t owner = 0;
	if (!LoadWord<uint32_t>(&mutex->m_flags, &flags) ||
	    !LoadWord<uint32_t>(&mutex->m_owner, &owner)) {
		return POSIX_EFAULT;
	}

	const uint32_t id = CurrentTid();
	if ((owner & ~UMUTEX_CONTESTED) != id) {
		return POSIX_EPERM;
	}

	const uint32_t new_value = UnlockValue(flags, robust_dead);

	if ((owner & UMUTEX_CONTESTED) == 0) {
		uint32_t   observed = 0;
		const auto rv       = CompareAndSwap<uint32_t>(&mutex->m_owner, id, &observed, new_value);
		if (rv == CasResult::Fault) {
			return POSIX_EFAULT;
		}
		if (rv == CasResult::Success) {
			return OK;
		}
		// Contention appeared between the load and the store; fall through to the slow path.
		owner = observed;
		if ((owner & ~UMUTEX_CONTESTED) != id) {
			return POSIX_EPERM;
		}
	}

	KeyHold hold({reinterpret_cast<uintptr_t>(mutex), KeyType::Mutex});
	BusyLocked(hold.Lock(), hold.State());
	const int count = CountLocked(hold.State(), QUEUE_SHARED);

	// The contested bit must survive the unlock whenever more than one thread is still queued,
	// or the second waiter's own unlock would take the fast path and never wake the third.
	uint32_t store_value = new_value;
	if (count > 1) {
		store_value |= UMUTEX_CONTESTED;
	}

	uint32_t   observed = 0;
	const auto rv       = CompareAndSwap<uint32_t>(&mutex->m_owner, owner, &observed, store_value);
	SignalLocked(hold.State(), 1, QUEUE_SHARED);
	UnbusyLocked(hold.State());

	if (rv == CasResult::Fault) {
		return POSIX_EFAULT;
	}
	if (rv == CasResult::Mismatch && observed != owner) {
		return POSIX_EINVAL;
	}
	return OK;
}

// do_wake_umutex(): wake one waiter, and drop the contested bit if this was the last one.
int DoWakeMutex(void* obj) {
	auto* mutex = static_cast<GuestUmutex*>(obj);
	if (!IsAccessible<uint32_t>(mutex)) {
		return POSIX_EFAULT;
	}

	uint32_t owner = 0;
	if (!LoadWord<uint32_t>(&mutex->m_owner, &owner)) {
		return POSIX_EFAULT;
	}
	if ((owner & ~UMUTEX_CONTESTED) != 0) {
		return OK;
	}

	KeyHold hold({reinterpret_cast<uintptr_t>(mutex), KeyType::Mutex});
	BusyLocked(hold.Lock(), hold.State());
	const int count = CountLocked(hold.State(), QUEUE_SHARED);

	int error = OK;
	if (count <= 1) {
		uint32_t   observed = 0;
		const auto rv =
		    CompareAndSwap<uint32_t>(&mutex->m_owner, UMUTEX_CONTESTED, &observed, UMUTEX_UNOWNED);
		if (rv == CasResult::Fault) {
			error = POSIX_EFAULT;
		} else {
			owner = (rv == CasResult::Success) ? UMUTEX_CONTESTED : observed;
		}
	}
	if (error == OK && count != 0 && (owner & ~UMUTEX_CONTESTED) == 0) {
		SignalLocked(hold.State(), 1, QUEUE_SHARED);
	}
	UnbusyLocked(hold.State());
	return error;
}

// do_wake2_umutex(): the flags come from `val` rather than the mutex, because the guest may be
// waking a mutex whose memory it has already torn down.
int DoWakeMutex2(void* obj, uint32_t flags) {
	auto* mutex = static_cast<GuestUmutex*>(obj);
	if (!IsAccessible<uint32_t>(mutex)) {
		return POSIX_EFAULT;
	}
	(void)flags;

	KeyHold hold({reinterpret_cast<uintptr_t>(mutex), KeyType::Mutex});
	BusyLocked(hold.Lock(), hold.State());
	const int count = CountLocked(hold.State(), QUEUE_SHARED);

	int error = OK;
	if (count > 1) {
		// More than one waiter: the contested bit must stay set.
		uint32_t owner = 0;
		if (!LoadWord<uint32_t>(&mutex->m_owner, &owner)) {
			error = POSIX_EFAULT;
		} else if (owner == UMUTEX_UNOWNED) {
			uint32_t observed = 0;
			if (CompareAndSwap<uint32_t>(&mutex->m_owner, UMUTEX_UNOWNED, &observed,
			                             UMUTEX_CONTESTED) == CasResult::Fault) {
				error = POSIX_EFAULT;
			}
		}
	} else if (count == 1) {
		uint32_t owner = 0;
		if (!LoadWord<uint32_t>(&mutex->m_owner, &owner)) {
			error = POSIX_EFAULT;
		} else if (owner == (UMUTEX_CONTESTED | CurrentTid()) || owner == UMUTEX_CONTESTED) {
			uint32_t observed = 0;
			if (CompareAndSwap<uint32_t>(&mutex->m_owner, owner, &observed,
			                             owner & ~UMUTEX_CONTESTED) == CasResult::Fault) {
				error = POSIX_EFAULT;
			}
		}
	}

	if (error == OK && count != 0) {
		SignalLocked(hold.State(), 1, QUEUE_SHARED);
	}
	UnbusyLocked(hold.State());
	return error;
}

// do_set_ceiling(). FreeBSD locks the priority-protect mutex before swapping the ceiling so the
// change cannot land while another thread holds it at the old ceiling. There is no priority
// protection here to make inconsistent, so this swaps the word and reports the old value.
int DoSetCeiling(void* obj, uint32_t ceiling, void* old_ceiling_out) {
	auto* mutex = static_cast<GuestUmutex*>(obj);
	if (!IsAccessible<uint32_t>(mutex)) {
		return POSIX_EFAULT;
	}
	uint32_t flags = 0;
	if (!LoadWord<uint32_t>(&mutex->m_flags, &flags)) {
		return POSIX_EFAULT;
	}
	if ((flags & UMUTEX_PRIO_PROTECT) == 0) {
		return POSIX_EINVAL;
	}
	// FreeBSD bounds the ceiling by RTP_PRIO_MAX (31).
	if (ceiling > 31) {
		return POSIX_EINVAL;
	}

	uint32_t old = 0;
	if (!LoadWord<uint32_t>(&mutex->m_ceilings[0], &old)) {
		return POSIX_EFAULT;
	}
	if (!StoreWord<uint32_t>(&mutex->m_ceilings[0], ceiling)) {
		return POSIX_EFAULT;
	}
	if (old_ceiling_out != nullptr && !StoreWord<uint32_t>(old_ceiling_out, old)) {
		return POSIX_EFAULT;
	}
	return OK;
}

// ---------------------------------------------------------------------------------------------
// struct ucond  (do_cv_wait / do_cv_signal / do_cv_broadcast)
// ---------------------------------------------------------------------------------------------

int DoCvWait(void* obj, void* mutex_obj, const GuestTimespec* timeout, uint64_t wflags) {
	auto* cv = static_cast<GuestUcond*>(obj);
	if (!IsAccessible<uint32_t>(cv)) {
		return POSIX_EFAULT;
	}

	uint32_t flags = 0;
	if (!LoadWord<uint32_t>(&cv->c_flags, &flags)) {
		return POSIX_EFAULT;
	}

	int clock_id = CLOCK_ID_REALTIME;
	if ((wflags & CVWAIT_CLOCKID) != 0) {
		uint32_t guest_clock = 0;
		if (!LoadWord<uint32_t>(&cv->c_clockid, &guest_clock)) {
			return POSIX_EFAULT;
		}
		if (guest_clock > static_cast<uint32_t>(CLOCK_ID_THREAD_CPUTIME_ID)) {
			return POSIX_EINVAL;
		}
		clock_id = static_cast<int>(guest_clock);
	}

	Deadline deadline;
	if (timeout != nullptr) {
		const int error =
		    MakeDeadline(*timeout, (wflags & CVWAIT_ABSTIME) != 0, clock_id, &deadline);
		if (error != OK) {
			return error;
		}
	}

	KeyHold hold({reinterpret_cast<uintptr_t>(cv), KeyType::Cv});
	Waiter  waiter;

	// Enqueue, then publish c_has_waiters, and only then drop the guest mutex. A signaller that
	// observes c_has_waiters can only be running after our enqueue, and must take this bucket
	// lock to signal - so the unlock below cannot lose a wakeup.
	BusyLocked(hold.Lock(), hold.State());
	InsertLocked(hold.State(), &waiter, QUEUE_SHARED);

	uint32_t has_waiters = 0;
	if (LoadWord<uint32_t>(&cv->c_has_waiters, &has_waiters) && has_waiters == 0) {
		StoreWord<uint32_t>(&cv->c_has_waiters, 1);
	}
	UnbusyLocked(hold.State());

	// The kernel releases the guest mutex on the caller's behalf; the caller re-acquires it
	// itself after this call returns.
	hold.Lock().unlock();
	int error = DoUnlockMutex(mutex_obj, false);
	hold.Lock().lock();

	if (error == OK) {
		error = SleepLocked(hold.Lock(), &waiter, deadline);
	}

	if (!waiter.queued) {
		// Signalled: the signaller owns c_has_waiters.
		error = OK;
	} else {
		BusyLocked(hold.Lock(), hold.State());
		const bool last = CountLocked(hold.State(), QUEUE_SHARED) == 1;
		RemoveLocked(hold.State(), &waiter);
		if (last) {
			StoreWord<uint32_t>(&cv->c_has_waiters, 0);
		}
		UnbusyLocked(hold.State());
	}
	return error;
}

int DoCvSignal(void* obj, bool broadcast) {
	auto* cv = static_cast<GuestUcond*>(obj);
	if (!IsAccessible<uint32_t>(cv)) {
		return POSIX_EFAULT;
	}
	uint32_t flags = 0;
	if (!LoadWord<uint32_t>(&cv->c_flags, &flags)) {
		return POSIX_EFAULT;
	}

	KeyHold hold({reinterpret_cast<uintptr_t>(cv), KeyType::Cv});
	BusyLocked(hold.Lock(), hold.State());

	const int count = CountLocked(hold.State(), QUEUE_SHARED);
	const int woken = SignalLocked(hold.State(), broadcast ? INT_MAX : 1, QUEUE_SHARED);

	int error = OK;
	if (count <= woken) {
		if (!StoreWord<uint32_t>(&cv->c_has_waiters, 0)) {
			error = POSIX_EFAULT;
		}
	}
	UnbusyLocked(hold.State());
	return error;
}

// ---------------------------------------------------------------------------------------------
// struct urwlock  (do_rw_rdlock / do_rw_wrlock / do_rw_unlock)
// ---------------------------------------------------------------------------------------------

int DoRwRdlock(void* obj, uint64_t fflag, const Deadline& deadline) {
	auto* rw = static_cast<GuestUrwlock*>(obj);
	if (!IsAccessible<uint32_t>(rw)) {
		return POSIX_EFAULT;
	}
	uint32_t flags = 0;
	if (!LoadWord<uint32_t>(&rw->rw_flags, &flags)) {
		return POSIX_EFAULT;
	}

	uint32_t wrflags = URWLOCK_WRITE_OWNER;
	if ((fflag & URWLOCK_PREFER_READER) == 0 && (flags & URWLOCK_PREFER_READER) == 0) {
		wrflags |= URWLOCK_WRITE_WAITERS;
	}

	for (;;) {
		uint32_t state = 0;
		if (!LoadWord<uint32_t>(&rw->rw_state, &state)) {
			return POSIX_EFAULT;
		}

		// Fast path: no writer in the way, bump the reader count.
		while ((state & wrflags) == 0) {
			if ((state & URWLOCK_MAX_READERS) == URWLOCK_MAX_READERS) {
				return POSIX_EAGAIN;
			}
			uint32_t   observed = 0;
			const auto rv = CompareAndSwap<uint32_t>(&rw->rw_state, state, &observed, state + 1);
			if (rv == CasResult::Fault) {
				return POSIX_EFAULT;
			}
			if (rv == CasResult::Success) {
				return OK;
			}
			state = observed;
		}

		KeyHold hold({reinterpret_cast<uintptr_t>(rw), KeyType::Rwlock});
		BusyLocked(hold.Lock(), hold.State());

		if (!LoadWord<uint32_t>(&rw->rw_state, &state)) {
			UnbusyLocked(hold.State());
			return POSIX_EFAULT;
		}

		// Publish "a reader is waiting" so an unlocking writer knows to signal us.
		while ((state & wrflags) != 0 && (state & URWLOCK_READ_WAITERS) == 0) {
			uint32_t   observed = 0;
			const auto rv       = CompareAndSwap<uint32_t>(&rw->rw_state, state, &observed,
			                                               state | URWLOCK_READ_WAITERS);
			if (rv == CasResult::Fault) {
				UnbusyLocked(hold.State());
				return POSIX_EFAULT;
			}
			if (rv == CasResult::Success) {
				state |= URWLOCK_READ_WAITERS;
				break;
			}
			state = observed;
		}

		if ((state & wrflags) == 0) {
			// The writer left while we were publishing; retry the fast path.
			UnbusyLocked(hold.State());
			continue;
		}

		uint32_t blocked = 0;
		if (LoadWord<uint32_t>(&rw->rw_blocked_readers, &blocked)) {
			StoreWord<uint32_t>(&rw->rw_blocked_readers, blocked + 1);
		}

		Waiter waiter;
		InsertLocked(hold.State(), &waiter, QUEUE_SHARED);
		UnbusyLocked(hold.State());

		int error = SleepLocked(hold.Lock(), &waiter, deadline);
		if (!waiter.queued) {
			error = OK;
		} else {
			RemoveLocked(hold.State(), &waiter);
		}

		BusyLocked(hold.Lock(), hold.State());
		if (LoadWord<uint32_t>(&rw->rw_blocked_readers, &blocked) && blocked > 0) {
			StoreWord<uint32_t>(&rw->rw_blocked_readers, blocked - 1);
			if (blocked == 1 && LoadWord<uint32_t>(&rw->rw_state, &state)) {
				uint32_t observed = 0;
				(void)CompareAndSwap<uint32_t>(&rw->rw_state, state, &observed,
				                               state & ~URWLOCK_READ_WAITERS);
			}
		}
		UnbusyLocked(hold.State());

		if (error != OK) {
			return error;
		}
	}
}

int DoRwWrlock(void* obj, const Deadline& deadline) {
	auto* rw = static_cast<GuestUrwlock*>(obj);
	if (!IsAccessible<uint32_t>(rw)) {
		return POSIX_EFAULT;
	}
	uint32_t flags = 0;
	if (!LoadWord<uint32_t>(&rw->rw_flags, &flags)) {
		return POSIX_EFAULT;
	}

	constexpr uint32_t BLOCKERS = URWLOCK_WRITE_OWNER | URWLOCK_READ_OWNER | URWLOCK_MAX_READERS;

	for (;;) {
		uint32_t state = 0;
		if (!LoadWord<uint32_t>(&rw->rw_state, &state)) {
			return POSIX_EFAULT;
		}

		while ((state & BLOCKERS) == 0) {
			uint32_t   observed = 0;
			const auto rv       = CompareAndSwap<uint32_t>(&rw->rw_state, state, &observed,
			                                               state | URWLOCK_WRITE_OWNER);
			if (rv == CasResult::Fault) {
				return POSIX_EFAULT;
			}
			if (rv == CasResult::Success) {
				return OK;
			}
			state = observed;
		}

		KeyHold hold({reinterpret_cast<uintptr_t>(rw), KeyType::Rwlock});
		BusyLocked(hold.Lock(), hold.State());

		if (!LoadWord<uint32_t>(&rw->rw_state, &state)) {
			UnbusyLocked(hold.State());
			return POSIX_EFAULT;
		}

		while ((state & BLOCKERS) != 0 && (state & URWLOCK_WRITE_WAITERS) == 0) {
			uint32_t   observed = 0;
			const auto rv       = CompareAndSwap<uint32_t>(&rw->rw_state, state, &observed,
			                                               state | URWLOCK_WRITE_WAITERS);
			if (rv == CasResult::Fault) {
				UnbusyLocked(hold.State());
				return POSIX_EFAULT;
			}
			if (rv == CasResult::Success) {
				state |= URWLOCK_WRITE_WAITERS;
				break;
			}
			state = observed;
		}

		if ((state & BLOCKERS) == 0) {
			UnbusyLocked(hold.State());
			continue;
		}

		uint32_t blocked = 0;
		if (LoadWord<uint32_t>(&rw->rw_blocked_writers, &blocked)) {
			StoreWord<uint32_t>(&rw->rw_blocked_writers, blocked + 1);
		}

		Waiter waiter;
		InsertLocked(hold.State(), &waiter, QUEUE_EXCLUSIVE);
		UnbusyLocked(hold.State());

		int error = SleepLocked(hold.Lock(), &waiter, deadline);
		if (!waiter.queued) {
			error = OK;
		} else {
			RemoveLocked(hold.State(), &waiter);
		}

		BusyLocked(hold.Lock(), hold.State());
		if (LoadWord<uint32_t>(&rw->rw_blocked_writers, &blocked) && blocked > 0) {
			StoreWord<uint32_t>(&rw->rw_blocked_writers, blocked - 1);
			if (blocked == 1 && LoadWord<uint32_t>(&rw->rw_state, &state)) {
				uint32_t observed = 0;
				(void)CompareAndSwap<uint32_t>(&rw->rw_state, state, &observed,
				                               state & ~URWLOCK_WRITE_WAITERS);
				// A writer giving up must hand the lock to the queued readers, or they sleep
				// forever behind a WRITE_WAITERS bit nobody will clear again.
				if (CountLocked(hold.State(), QUEUE_SHARED) > 0) {
					SignalLocked(hold.State(), INT_MAX, QUEUE_SHARED);
				}
			}
		}
		UnbusyLocked(hold.State());

		if (error != OK) {
			return error;
		}
	}
}

int DoRwUnlock(void* obj) {
	auto* rw = static_cast<GuestUrwlock*>(obj);
	if (!IsAccessible<uint32_t>(rw)) {
		return POSIX_EFAULT;
	}
	uint32_t flags = 0;
	uint32_t state = 0;
	if (!LoadWord<uint32_t>(&rw->rw_flags, &flags) || !LoadWord<uint32_t>(&rw->rw_state, &state)) {
		return POSIX_EFAULT;
	}

	if ((state & URWLOCK_WRITE_OWNER) != 0) {
		for (;;) {
			uint32_t   observed = 0;
			const auto rv       = CompareAndSwap<uint32_t>(&rw->rw_state, state, &observed,
			                                               state & ~URWLOCK_WRITE_OWNER);
			if (rv == CasResult::Fault) {
				return POSIX_EFAULT;
			}
			if (rv == CasResult::Success) {
				break;
			}
			state = observed;
			if ((state & URWLOCK_WRITE_OWNER) == 0) {
				return POSIX_EPERM;
			}
		}
	} else if ((state & URWLOCK_MAX_READERS) != 0) {
		for (;;) {
			uint32_t   observed = 0;
			const auto rv = CompareAndSwap<uint32_t>(&rw->rw_state, state, &observed, state - 1);
			if (rv == CasResult::Fault) {
				return POSIX_EFAULT;
			}
			if (rv == CasResult::Success) {
				break;
			}
			state = observed;
			if ((state & URWLOCK_MAX_READERS) == 0) {
				return POSIX_EPERM;
			}
		}
	} else {
		return POSIX_EPERM;
	}

	int count = 0;
	int queue = QUEUE_EXCLUSIVE;
	if ((flags & URWLOCK_PREFER_READER) == 0) {
		if ((state & URWLOCK_WRITE_WAITERS) != 0) {
			count = 1;
			queue = QUEUE_EXCLUSIVE;
		} else if ((state & URWLOCK_READ_WAITERS) != 0) {
			count = INT_MAX;
			queue = QUEUE_SHARED;
		}
	} else {
		if ((state & URWLOCK_READ_WAITERS) != 0) {
			count = INT_MAX;
			queue = QUEUE_SHARED;
		} else if ((state & URWLOCK_WRITE_WAITERS) != 0) {
			count = 1;
			queue = QUEUE_EXCLUSIVE;
		}
	}

	if (count != 0) {
		KeyHold hold({reinterpret_cast<uintptr_t>(rw), KeyType::Rwlock});
		BusyLocked(hold.Lock(), hold.State());
		SignalLocked(hold.State(), count, queue);
		UnbusyLocked(hold.State());
	}
	return OK;
}

// ---------------------------------------------------------------------------------------------
// struct _usem2 / struct _usem  (do_sem2_wait / do_sem2_wake / do_sem_wait / do_sem_wake)
// ---------------------------------------------------------------------------------------------

int DoSem2Wait(void* obj, const Deadline& deadline) {
	auto* sem = static_cast<GuestUsem2*>(obj);
	if (!IsAccessible<uint32_t>(sem)) {
		return POSIX_EFAULT;
	}
	uint32_t flags = 0;
	if (!LoadWord<uint32_t>(&sem->flags, &flags)) {
		return POSIX_EFAULT;
	}

	KeyHold hold({reinterpret_cast<uintptr_t>(sem), KeyType::Sem});
	Waiter  waiter;

	BusyLocked(hold.Lock(), hold.State());
	InsertLocked(hold.State(), &waiter, QUEUE_SHARED);

	uint32_t count = 0;
	if (!LoadWord<uint32_t>(&sem->count, &count)) {
		RemoveLocked(hold.State(), &waiter);
		UnbusyLocked(hold.State());
		return POSIX_EFAULT;
	}

	int error = OK;
	for (;;) {
		if ((count & USEM_MAX_COUNT) != 0) {
			// A post beat us here: consume a unit and do not sleep at all.
			uint32_t   observed = 0;
			const auto rv = CompareAndSwap<uint32_t>(&sem->count, count, &observed, count - 1);
			if (rv == CasResult::Fault) {
				error = POSIX_EFAULT;
				break;
			}
			if (rv == CasResult::Success) {
				RemoveLocked(hold.State(), &waiter);
				UnbusyLocked(hold.State());
				return OK;
			}
			count = observed;
			continue;
		}
		if (count == USEM_HAS_WAITERS) {
			break;
		}
		uint32_t   observed = 0;
		const auto rv = CompareAndSwap<uint32_t>(&sem->count, 0u, &observed, USEM_HAS_WAITERS);
		if (rv == CasResult::Fault) {
			error = POSIX_EFAULT;
			break;
		}
		if (rv == CasResult::Success) {
			break;
		}
		count = observed;
	}

	UnbusyLocked(hold.State());
	if (error == OK) {
		error = SleepLocked(hold.Lock(), &waiter, deadline);
	}

	if (!waiter.queued) {
		error = OK;
	} else {
		RemoveLocked(hold.State(), &waiter);
	}

	// FreeBSD writes the unslept remainder back just past the caller's _umtx_time, but only when
	// the wait was interrupted by a signal. This layer never reports EINTR, so nothing is written
	// and the caller's timeout struct is left untouched.
	return error;
}

int DoSem2Wake(void* obj) {
	auto* sem = static_cast<GuestUsem2*>(obj);
	if (!IsAccessible<uint32_t>(sem)) {
		return POSIX_EFAULT;
	}
	uint32_t flags = 0;
	if (!LoadWord<uint32_t>(&sem->flags, &flags)) {
		return POSIX_EFAULT;
	}

	KeyHold hold({reinterpret_cast<uintptr_t>(sem), KeyType::Sem});
	BusyLocked(hold.Lock(), hold.State());

	int       error = OK;
	const int count = CountLocked(hold.State(), QUEUE_SHARED);
	if (count > 0) {
		if (count == 1) {
			uint32_t value = 0;
			if (!LoadWord<uint32_t>(&sem->count, &value)) {
				error = POSIX_EFAULT;
			}
			while (error == OK && (value & USEM_HAS_WAITERS) != 0) {
				uint32_t   observed = 0;
				const auto rv = CompareAndSwap<uint32_t>(&sem->count, value, &observed,
				                                         value & ~USEM_HAS_WAITERS);
				if (rv == CasResult::Fault) {
					error = POSIX_EFAULT;
					break;
				}
				if (rv == CasResult::Success) {
					break;
				}
				value = observed;
			}
		}
		if (error == OK) {
			SignalLocked(hold.State(), 1, QUEUE_SHARED);
		}
	}
	UnbusyLocked(hold.State());
	return error;
}

// The FreeBSD 10 semaphore, kept because a FreeBSD 9-derived guest libthr may still use it.
int DoSemWait(void* obj, const Deadline& deadline) {
	auto* sem = static_cast<GuestUsem*>(obj);
	if (!IsAccessible<uint32_t>(sem)) {
		return POSIX_EFAULT;
	}

	KeyHold hold({reinterpret_cast<uintptr_t>(sem), KeyType::Sem});
	Waiter  waiter;

	BusyLocked(hold.Lock(), hold.State());
	InsertLocked(hold.State(), &waiter, QUEUE_SHARED);
	if (!StoreWord<uint32_t>(&sem->has_waiters, 1)) {
		RemoveLocked(hold.State(), &waiter);
		UnbusyLocked(hold.State());
		return POSIX_EFAULT;
	}
	UnbusyLocked(hold.State());

	uint32_t count = 0;
	if (!LoadWord<uint32_t>(&sem->count, &count)) {
		RemoveLocked(hold.State(), &waiter);
		return POSIX_EFAULT;
	}

	int error = OK;
	if (count == 0) {
		error = SleepLocked(hold.Lock(), &waiter, deadline);
	}

	if (!waiter.queued) {
		error = OK;
	} else {
		RemoveLocked(hold.State(), &waiter);
	}
	return error;
}

int DoSemWake(void* obj) {
	auto* sem = static_cast<GuestUsem*>(obj);
	if (!IsAccessible<uint32_t>(sem)) {
		return POSIX_EFAULT;
	}
	KeyHold hold({reinterpret_cast<uintptr_t>(sem), KeyType::Sem});
	SignalLocked(hold.State(), 1, QUEUE_SHARED);
	return OK;
}

// ---------------------------------------------------------------------------------------------
// UMTX_OP_NWAKE_PRIVATE  (__umtx_op_nwake_private)
// ---------------------------------------------------------------------------------------------

constexpr uint64_t NWAKE_MAX = 65536;

int DoNWakePrivate(void* obj, uint64_t count) {
	if (count == 0) {
		return OK;
	}
	if (count > NWAKE_MAX) {
		return POSIX_EINVAL;
	}
	if (!IsAccessible<uint64_t>(obj)) {
		return POSIX_EFAULT;
	}
	const auto* addresses = static_cast<const uint64_t*>(obj);
	for (uint64_t i = 0; i < count; i++) {
		auto* target = reinterpret_cast<void*>(static_cast<uintptr_t>(addresses[i]));
		if (target == nullptr) {
			continue;
		}
		(void)DoWake(target, INT_MAX, KeyType::SimpleWaitU32);
	}
	return OK;
}

// ---------------------------------------------------------------------------------------------
// UMTX_OP_ROBUST_LISTS  (__umtx_op_robust_lists)
//
// Accepted and recorded, but nothing walks the list at thread exit, so a thread that dies while
// holding a robust mutex will not have its mutex stamped UMUTEX_RB_OWNERDEAD. Refusing the call
// would be worse: libthr treats the failure as fatal at startup.
// ---------------------------------------------------------------------------------------------

thread_local GuestRobustListsParams g_robust_lists {};
thread_local bool                   g_robust_lists_set = false;

int DoRobustLists(uint64_t size, const void* params) {
	if (size != sizeof(GuestRobustListsParams) || params == nullptr) {
		return POSIX_EINVAL;
	}
	std::memcpy(&g_robust_lists, params, sizeof(g_robust_lists));
	g_robust_lists_set = true;
	return OK;
}

KeyType KeyTypeForOperation(int operation) {
	switch (operation) {
		case UMTX_OP_WAIT:
		case UMTX_OP_WAKE: return KeyType::SimpleWait;
		case UMTX_OP_WAIT_UINT:
		case UMTX_OP_WAIT_UINT_PRIVATE:
		case UMTX_OP_WAKE_PRIVATE:
		case UMTX_OP_NWAKE_PRIVATE: return KeyType::SimpleWaitU32;
		case UMTX_OP_MUTEX_TRYLOCK:
		case UMTX_OP_MUTEX_LOCK:
		case UMTX_OP_MUTEX_UNLOCK:
		case UMTX_OP_MUTEX_WAIT:
		case UMTX_OP_MUTEX_WAKE:
		case UMTX_OP_MUTEX_WAKE2:
		case UMTX_OP_SET_CEILING: return KeyType::Mutex;
		case UMTX_OP_CV_WAIT:
		case UMTX_OP_CV_SIGNAL:
		case UMTX_OP_CV_BROADCAST: return KeyType::Cv;
		case UMTX_OP_RW_RDLOCK:
		case UMTX_OP_RW_WRLOCK:
		case UMTX_OP_RW_UNLOCK: return KeyType::Rwlock;
		case UMTX_OP_SEM_WAIT:
		case UMTX_OP_SEM_WAKE:
		case UMTX_OP_SEM2_WAIT:
		case UMTX_OP_SEM2_WAKE: return KeyType::Sem;
		default: return KeyType::SimpleWait;
	}
}

} // namespace

void SetHooks(tid_func_t tid, signal_poll_func_t signal_poll,
              clock_gettime_func_t clock_gettime) {
	if (tid != nullptr) {
		g_tid = tid;
	}
	if (signal_poll != nullptr) {
		g_signal_poll = signal_poll;
	}
	if (clock_gettime != nullptr) {
		g_clock_gettime = clock_gettime;
	}
}

uint32_t CurrentTid() {
	const uint32_t id = g_tid();
	// The top bit of a umutex owner word is UMUTEX_CONTESTED, so a tid must never set it, and
	// zero is UMUTEX_UNOWNED.
	const uint32_t masked = id & ~UMUTEX_CONTESTED;
	return masked == 0 ? 1u : masked;
}

int Op(void* obj, int operation, uint64_t val, void* uaddr1, void* uaddr2) {
	Deadline deadline;

	switch (operation) {
		case UMTX_OP_WAIT: {
			const int error = DecodeUmtxTime(uaddr1, uaddr2, &deadline);
			if (error != OK) {
				return error;
			}
			return DoWait<uint64_t>(obj, val, deadline, KeyType::SimpleWait);
		}
		case UMTX_OP_WAIT_UINT:
		case UMTX_OP_WAIT_UINT_PRIVATE: {
			const int error = DecodeUmtxTime(uaddr1, uaddr2, &deadline);
			if (error != OK) {
				return error;
			}
			return DoWait<uint32_t>(obj, static_cast<uint32_t>(val), deadline,
			                        KeyType::SimpleWaitU32);
		}
		case UMTX_OP_WAKE: return DoWake(obj, static_cast<int32_t>(val), KeyType::SimpleWait);
		case UMTX_OP_WAKE_PRIVATE:
			return DoWake(obj, static_cast<int32_t>(val), KeyType::SimpleWaitU32);
		case UMTX_OP_NWAKE_PRIVATE: return DoNWakePrivate(obj, val);

		case UMTX_OP_MUTEX_TRYLOCK: return DoLockMutex(obj, deadline, MUTEX_MODE_TRY);
		case UMTX_OP_MUTEX_LOCK: {
			const int error = DecodeUmtxTime(uaddr1, uaddr2, &deadline);
			if (error != OK) {
				return error;
			}
			return DoLockMutex(obj, deadline, MUTEX_MODE_LOCK);
		}
		case UMTX_OP_MUTEX_WAIT: {
			const int error = DecodeUmtxTime(uaddr1, uaddr2, &deadline);
			if (error != OK) {
				return error;
			}
			return DoLockMutex(obj, deadline, MUTEX_MODE_WAIT);
		}
		case UMTX_OP_MUTEX_UNLOCK: return DoUnlockMutex(obj, false);
		case UMTX_OP_MUTEX_WAKE: return DoWakeMutex(obj);
		case UMTX_OP_MUTEX_WAKE2: return DoWakeMutex2(obj, static_cast<uint32_t>(val));
		case UMTX_OP_SET_CEILING:
			return DoSetCeiling(obj, static_cast<uint32_t>(val), uaddr1);

		case UMTX_OP_CV_WAIT: {
			GuestTimespec ts {};
			bool          present = false;
			const int     error   = DecodeCvTime(uaddr2, &ts, &present);
			if (error != OK) {
				return error;
			}
			return DoCvWait(obj, uaddr1, present ? &ts : nullptr, val);
		}
		case UMTX_OP_CV_SIGNAL: return DoCvSignal(obj, false);
		case UMTX_OP_CV_BROADCAST: return DoCvSignal(obj, true);

		case UMTX_OP_RW_RDLOCK: {
			const int error = DecodeUmtxTime(uaddr1, uaddr2, &deadline);
			if (error != OK) {
				return error;
			}
			return DoRwRdlock(obj, val, deadline);
		}
		case UMTX_OP_RW_WRLOCK: {
			const int error = DecodeUmtxTime(uaddr1, uaddr2, &deadline);
			if (error != OK) {
				return error;
			}
			return DoRwWrlock(obj, deadline);
		}
		case UMTX_OP_RW_UNLOCK: return DoRwUnlock(obj);

		case UMTX_OP_SEM_WAIT: {
			const int error = DecodeUmtxTime(uaddr1, uaddr2, &deadline);
			if (error != OK) {
				return error;
			}
			return DoSemWait(obj, deadline);
		}
		case UMTX_OP_SEM_WAKE: return DoSemWake(obj);
		case UMTX_OP_SEM2_WAIT: {
			const int error = DecodeUmtxTime(uaddr1, uaddr2, &deadline);
			if (error != OK) {
				return error;
			}
			return DoSem2Wait(obj, deadline);
		}
		case UMTX_OP_SEM2_WAKE: return DoSem2Wake(obj);

		case UMTX_OP_ROBUST_LISTS: return DoRobustLists(val, uaddr1);

		// UMTX_OP_SHM hands back a shared-memory file descriptor keyed by a guest address. There
		// is no second process to share with here, so refusing it is both honest and harmless:
		// libthr only uses it for process-shared robust mutex bookkeeping.
		case UMTX_OP_SHM: return POSIX_EOPNOTSUPP;

		case UMTX_OP_RESERVED0:
		case UMTX_OP_RESERVED1: return POSIX_EOPNOTSUPP;

		default: return POSIX_EINVAL;
	}
}

void ResetForTests() {
	for (size_t i = 0; i < BUCKET_COUNT; i++) {
		Key dummy {i, KeyType::SimpleWait};
		(void)dummy;
	}
	// Buckets are function-local statics keyed by address; an explicit reset would have to walk
	// them all. Waiters clean their own key up on the way out, so a test that leaves no waiter
	// behind already leaves an empty registry.
}

int WaiterCountForTests(const void* addr, int operation) {
	const Key key {reinterpret_cast<uintptr_t>(addr), KeyTypeForOperation(operation)};
	Bucket&   bucket = GetBucket(key);

	std::lock_guard lock(bucket.m);
	const auto      it = bucket.keys.find(key);
	if (it == bucket.keys.end()) {
		return 0;
	}
	return CountAllLocked(it->second);
}

} // namespace Libs::LibKernel::Umtx
