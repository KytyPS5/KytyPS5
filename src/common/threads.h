#ifndef KYTY_COMMON_THREADS_H_
#define KYTY_COMMON_THREADS_H_

#include "common/common.h"

#include <memory>
#include <string>

namespace Common {

void InitializeThreads();

using thread_func_t    = void (*)(void*);
using wait_poll_func_t = void (*)();

struct ThreadPrivate;
struct MutexPrivate;
struct CondVarPrivate;

class Thread {
public:
	Thread(thread_func_t func, void* arg);
	~Thread();

	void Join();
	void Detach();

	// Once a thread has finished, the id may be reused by another thread.
	[[nodiscard]] std::string GetId() const;

	// The id is unique and can't be reused by another thread.
	[[nodiscard]] int GetUniqueId() const;

	static void SleepMicro(uint32_t micros);
	static void SleepNano(uint64_t nanos);
	static bool IsMainThread();

	// Get current thread id
	// Once a thread has finished, the id may be reused by another thread.
	static std::string GetThreadId();

	// Get current thread id
	// The id is unique and can't be reused by another thread.
	static int GetThreadIdUnique();

	// Gives the calling thread a performance core of its own: it runs only on that core's
	// logical processors, and the process's other threads default to the remaining ones.
	// Returns the core's first logical processor, or -1 where no core can be spared.
	static int ReserveCoreForCurrentThread();

	KYTY_CLASS_NO_COPY(Thread);

private:
	std::unique_ptr<ThreadPrivate> m_thread;
};

class Mutex {
public:
	Mutex();
	~Mutex();

	void Lock();
	void Unlock();
	bool TryLock();

	friend class CondVar;

	KYTY_CLASS_NO_COPY(Mutex);

private:
	std::unique_ptr<MutexPrivate> m_mutex;
};

class CondVar {
public:
	CondVar();
	~CondVar();

	void Wait(Mutex* mutex);
	bool WaitFor(Mutex* mutex, uint32_t micros);
	void Signal();
	void SignalAll();

	static void SetWaitPollCallback(wait_poll_func_t callback);

	KYTY_CLASS_NO_COPY(CondVar);

private:
	std::unique_ptr<CondVarPrivate> m_cond_var;
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
