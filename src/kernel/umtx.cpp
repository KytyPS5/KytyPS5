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

// FreeBSD's kern_umtx.c for one guest process: a guest address is both queue key and word.

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

// Each type is its own queue: a WAIT and a MUTEX_LOCK sleeper on one word never wake each other.
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

// One key always maps to one bucket lock.
constexpr size_t BUCKET_COUNT = 128;

struct Bucket {
	std::mutex                                  m;
	std::unordered_map<Key, KeyState*, KeyHash> keys;
};

Bucket& GetBucket(const Key& key) {
	static Bucket buckets[BUCKET_COUNT];
	const auto    index = (KeyHash {}(key) >> 4u) % BUCKET_COUNT;
	return buckets[index];
}

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

tid_func_t           g_tid           = DefaultTid;
signal_poll_func_t   g_signal_poll   = nullptr;
clock_gettime_func_t g_clock_gettime = DefaultClockGettime;

void PollSignals() {
	if (g_signal_poll != nullptr) {
		g_signal_poll();
	}
}

// Guest words are dereferenced directly; an unmapped address faults the host instead of EFAULT.

template <typename T>
bool IsAccessible(const void* address) {
	return address != nullptr && (reinterpret_cast<uintptr_t>(address) & (alignof(T) - 1u)) == 0;
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

// Queue primitives. Every one of these runs with the key's bucket mutex held.

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
	auto&      q  = state->queues[waiter->queue];
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

// Dequeuing at signal time lets a sleeper tell a signal from a timeout (FreeBSD UQF_UMTXQ).
int SignalLocked(KeyState* state, int count, int queue) {
	int   woken = 0;
	auto& q     = state->queues[queue];
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

// A flag, not the bucket mutex: these guest-word sequences run with the bucket mutex dropped.
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

// A relative timeout becomes absolute on its own clock at once, so sliced sleeps cannot drift.
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
	const int64_t delta_ns = (deadline.sec - now_sec) * 1000000000LL + (deadline.nsec - now_nsec);
	if (delta_ns <= 0) {
		*expired = true;
		return 0;
	}
	*expired = false;
	return static_cast<uint64_t>(delta_ns) / 1000ull;
}

// The caller holds the bucket lock, has enqueued `waiter` and re-checked the word under it.
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

// Wait ops pass the timeout in uaddr2 and its size in uaddr1; a larger size is a _umtx_time.

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

// CV_WAIT takes a plain timespec in uaddr2; abs/clock come from the wflags in `val`.
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

template <typename T>
int DoWait(void* obj, T id, const Deadline& deadline, KeyType type) {
	if (!IsAccessible<T>(obj)) {
		return POSIX_EFAULT;
	}

	KeyHold hold({reinterpret_cast<uintptr_t>(obj), type});
	Waiter  waiter;

	// Enqueue under the bucket lock before reading the guest word; a waker needs the same lock.
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

	// A signal that raced with the timeout wins, or the wakeup is lost.
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

// PI and PP mutexes share the owner-word encoding; only the priority boost is lost.

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
			if (owner == UMUTEX_RB_OWNERDEAD) {
				uint32_t   observed = 0;
				const auto rv       = CompareAndSwap<uint32_t>(&mutex->m_owner, UMUTEX_RB_OWNERDEAD,
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

			// The guest may have released it between its own attempt and this syscall.
			uint32_t observed = 0;
			auto     rv = CompareAndSwap<uint32_t>(&mutex->m_owner, UMUTEX_UNOWNED, &observed, id);
			if (rv == CasResult::Fault) {
				return POSIX_EFAULT;
			}
			if (rv == CasResult::Success) {
				return OK;
			}
			owner = observed;

			// Keep the contested bit so our own unlock still wakes the queue.
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
				// libthr counts recursion in userland, so this is a genuine self-deadlock.
				return mode == MUTEX_MODE_TRY ? POSIX_EBUSY : POSIX_EDEADLK;
			}

			if (mode == MUTEX_MODE_TRY) {
				return POSIX_EBUSY;
			}
		}

		// Same ordering as DoWait: enqueue, publish the contested bit, re-test, then sleep.
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

	// The contested bit stays while several waiters are queued, or the last is never woken.
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

// The flags come from `val`: the guest may already have torn the mutex down.
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

// There is no priority protection here: swap the ceiling and report the old value.
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

	// Enqueue before publishing c_has_waiters and dropping the guest mutex: no lost wakeup.
	BusyLocked(hold.Lock(), hold.State());
	InsertLocked(hold.State(), &waiter, QUEUE_SHARED);

	uint32_t has_waiters = 0;
	if (LoadWord<uint32_t>(&cv->c_has_waiters, &has_waiters) && has_waiters == 0) {
		StoreWord<uint32_t>(&cv->c_has_waiters, 1);
	}
	UnbusyLocked(hold.State());

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
				// Hand the lock to the queued readers, or they sleep behind WRITE_WAITERS forever.
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

	// EINTR is never reported, so the unslept remainder is never written back.
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
				const auto rv       = CompareAndSwap<uint32_t>(&sem->count, value, &observed,
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

// Robust lists are recorded but never walked at thread exit; libthr treats a refusal as fatal.

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

void SetHooks(tid_func_t tid, signal_poll_func_t signal_poll, clock_gettime_func_t clock_gettime) {
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
	// A tid must never set UMUTEX_CONTESTED (the top bit), and zero is UMUTEX_UNOWNED.
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
		case UMTX_OP_SET_CEILING: return DoSetCeiling(obj, static_cast<uint32_t>(val), uaddr1);

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

		// There is no second process to share memory with.
		case UMTX_OP_SHM: return POSIX_EOPNOTSUPP;

		case UMTX_OP_RESERVED0:
		case UMTX_OP_RESERVED1: return POSIX_EOPNOTSUPP;

		default: return POSIX_EINVAL;
	}
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
