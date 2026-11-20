#ifndef KYTY_COMMON_THREADS_H_
#define KYTY_COMMON_THREADS_H_

#include "common/common.h"

#if KYTY_PLATFORM != KYTY_PLATFORM_WINDOWS
#include <condition_variable>
#include <mutex>
#include <thread>
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
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
// Windows only. Sets the calling thread to THREAD_PRIORITY_HIGHEST so busy guest threads do not
// take its core. Returns false when the host refuses.
bool SetCurrentPriorityHighest();
#endif
// Lowers the calling thread's priority so the game's own threads run first (the shader workers).
void LowerPriority();

} // namespace Thread

// Recursive.
class Mutex {
public:
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	Mutex();
	~Mutex();

	void Lock();
	void Unlock();
	bool TryLock();
	bool IsHeldByCurrentThread() const;

	KYTY_CLASS_NO_COPY(Mutex);

private:
	// A CRITICAL_SECTION.
	alignas(8) unsigned char m_cs[40];
#else
	void Lock() {
		m_mutex.lock();
		m_owner.store(std::this_thread::get_id(), std::memory_order_relaxed);
	}
	void Unlock() {
		m_owner.store({}, std::memory_order_relaxed);
		m_mutex.unlock();
	}
	bool TryLock() {
		if (m_mutex.try_lock()) {
			m_owner.store(std::this_thread::get_id(), std::memory_order_relaxed);
			return true;
		}
		return false;
	}
	bool IsHeldByCurrentThread() const {
		return m_owner.load(std::memory_order_relaxed) == std::this_thread::get_id();
	}

private:
	std::recursive_mutex         m_mutex;
	std::atomic<std::thread::id> m_owner {};
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
