#ifndef KYTY_COMMON_THREADS_H_
#define KYTY_COMMON_THREADS_H_

#include "common/common.h"

#include <span>

#if KYTY_PLATFORM != KYTY_PLATFORM_WINDOWS
#include <condition_variable>
#include <mutex>
#endif

namespace Common {

void InitializeThreads();

using wait_poll_func_t = void (*)();

namespace Thread {

void SleepMicro(uint32_t micros);
void SleepNano(uint64_t nanos);
bool IsMainThread();
// Unique per thread; never reused by another thread.
int GetThreadIdUnique();

// Current thread: raises its priority to "highest", and restricts it to one logical
// processor (when `logical_cpu` is not negative). No-ops where unsupported.
void RaiseCurrentPriority();
// Current thread: lowers its priority below normal, for background work that must not take
// processors from the emulated machine. No-op where unsupported.
void LowerCurrentPriority();
void PinCurrent(int logical_cpu);

// Keeps the threads of the process that pick no processors themselves off the physical cores
// of `logical_cpus` (and their SMT siblings), for threads pinned there, when at least six
// other physical cores remain: those running now and those that start later get a hard
// affinity to the other processors.
void ReserveCores(std::span<const int> logical_cpus);

} // namespace Thread

// Recursive.
class Mutex {
public:
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	Mutex();
	~Mutex();

	void Lock();
	void Unlock();

	KYTY_CLASS_NO_COPY(Mutex);

private:
	// A CRITICAL_SECTION.
	alignas(8) unsigned char m_cs[40];
#else
	void Lock() { m_mutex.lock(); }
	void Unlock() { m_mutex.unlock(); }

private:
	std::recursive_mutex m_mutex;
#endif

	friend class CondVar;
};

class CondVar {
public:
	CondVar() = default;

	void Wait(Mutex* mutex);
	bool WaitFor(Mutex* mutex, uint32_t micros);
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	void Signal();
	void SignalAll();
#else
	void Signal() { m_cv.notify_one(); }
	void SignalAll() { m_cv.notify_all(); }
#endif

	static void SetWaitPollCallback(wait_poll_func_t callback);

	KYTY_CLASS_NO_COPY(CondVar);

private:
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	// A CONDITION_VARIABLE, whose initial state is zero.
	void* m_cv = nullptr;
#else
	std::condition_variable_any m_cv;
#endif
};

class LockGuard {
public:
	using mutex_type = Mutex;

	// NOLINTNEXTLINE(google-runtime-references)
	explicit LockGuard(mutex_type& m): m_mutex(m) { m_mutex.Lock(); }

	~LockGuard() { m_mutex.Unlock(); }

	KYTY_CLASS_NO_COPY(LockGuard);

private:
	mutex_type& m_mutex;
};

} // namespace Common

#endif /* KYTY_COMMON_THREADS_H_ */
