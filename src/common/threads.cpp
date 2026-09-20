#include "common/threads.h"

#include "common/assert.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>             // IWYU pragma: keep
#include <condition_variable> // IWYU pragma: keep
#include <mutex>

#include <ctime>

#include <sstream>
#include <string>
#include <thread>

// Spin for very short waits; use an absolute deadline for longer waits.
static void SleepHighResolutionNanos(uint64_t nanos) {
	if (nanos == 0) {
		return;
	}

	constexpr uint64_t NANOS_PER_SEC = 1000000000;
	constexpr uint64_t SPIN_LIMIT_NS = 50000; // below this a context switch dominates

	timespec deadline {};
	if (clock_gettime(CLOCK_MONOTONIC, &deadline) != 0) {
		std::this_thread::sleep_for(std::chrono::nanoseconds(nanos));
		return;
	}

	auto target_nsec = static_cast<uint64_t>(deadline.tv_nsec) + nanos;
	deadline.tv_sec += static_cast<time_t>(target_nsec / NANOS_PER_SEC);
	deadline.tv_nsec = static_cast<long>(target_nsec % NANOS_PER_SEC);

	if (nanos <= SPIN_LIMIT_NS) {
		timespec now {};
		do {
			if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
				return;
			}
		} while (now.tv_sec < deadline.tv_sec ||
		         (now.tv_sec == deadline.tv_sec && now.tv_nsec < deadline.tv_nsec));
		return;
	}

	while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, nullptr) == EINTR) {
	}
}

namespace Common {

using thread_id_t = std::thread::id;

struct MutexPrivate {
	std::recursive_mutex m_mutex;
};

struct CondVarPrivate {
	std::condition_variable_any m_cv;
};

static wait_poll_func_t g_cond_wait_poll_callback = nullptr;

struct ThreadPrivate {
	ThreadPrivate(thread_func_t f, void* a): func(f), arg(a), m_thread(&Run, this) {}

	static void Run(ThreadPrivate* t) {
		t->unique_id = Thread::GetThreadIdUnique();
		t->started   = true;
		t->func(t->arg);
	}

	thread_func_t    func;
	void*            arg;
	std::atomic_bool finished    = false;
	std::atomic_bool auto_delete = false;
	std::atomic_bool started     = false;
	int              unique_id   = 0;
	std::thread      m_thread;
};

static thread_id_t      g_main_thread;
static int              g_main_thread_int;
static std::atomic<int> g_thread_counter = 0;

void InitializeThreads() {
	g_main_thread     = std::this_thread::get_id();
	g_main_thread_int = Thread::GetThreadIdUnique();
}

Thread::Thread(thread_func_t func, void* arg)
    : m_thread(std::make_unique<ThreadPrivate>(func, arg)) {
	while (!m_thread->started) {
		Common::Thread::SleepMicro(1000);
	}
}

Thread::~Thread() {
	EXIT_IF(!m_thread->finished && !m_thread->auto_delete);

	m_thread.reset();
}

void Thread::Join() {
	EXIT_IF(m_thread->finished || m_thread->auto_delete);

	m_thread->m_thread.join();

	m_thread->finished = true;
}

void Thread::Detach() {
	EXIT_IF(m_thread->finished || m_thread->auto_delete);

	m_thread->auto_delete = true;
	m_thread->m_thread.detach();
}

void Thread::SleepMicro(uint32_t micros) {
	SleepHighResolutionNanos(static_cast<uint64_t>(micros) * 1000);
}

void Thread::SleepNano(uint64_t nanos) {
	SleepHighResolutionNanos(nanos);
}

bool Thread::IsMainThread() {
	return g_main_thread == std::this_thread::get_id();
}

std::string Thread::GetId() const {
	std::stringstream ss;
	ss << m_thread->m_thread.get_id();
	return ss.str();
}

int Thread::GetUniqueId() const {
	return m_thread->unique_id;
}

std::string Thread::GetThreadId() {
	std::stringstream ss;
	ss << std::this_thread::get_id();
	return ss.str();
}

Mutex::Mutex(): m_mutex(std::make_unique<MutexPrivate>()) {}

Mutex::~Mutex() {
	m_mutex.reset();
}

void Mutex::Lock() {
	m_mutex->m_mutex.lock();
}

void Mutex::Unlock() {
	m_mutex->m_mutex.unlock();
}

bool Mutex::TryLock() {
	return m_mutex->m_mutex.try_lock();
}

CondVar::CondVar(): m_cond_var(std::make_unique<CondVarPrivate>()) {}

CondVar::~CondVar() {
	m_cond_var.reset();
}

void CondVar::Wait(Mutex* mutex) {
	std::unique_lock<std::recursive_mutex> cpp_lock(mutex->m_mutex->m_mutex, std::adopt_lock_t());
	
	auto poll_callback = [&] {
		auto* callback = g_cond_wait_poll_callback;
		if (callback == nullptr) {
			return;
		}
		cpp_lock.unlock();
		callback();
		cpp_lock.lock();
	};
	
	if (g_cond_wait_poll_callback == nullptr) {
		m_cond_var->m_cv.wait(cpp_lock);
	} else {
		if (m_cond_var->m_cv.wait_for(cpp_lock, std::chrono::microseconds(10000)) ==
		    std::cv_status::timeout) {
			poll_callback();
		}
	}
	cpp_lock.release();
}

void CondVar::SetWaitPollCallback(wait_poll_func_t callback) {
	g_cond_wait_poll_callback = callback;
}

bool CondVar::WaitFor(Mutex* mutex, uint32_t micros) {
	bool ok = false;
	std::unique_lock<std::recursive_mutex> cpp_lock(mutex->m_mutex->m_mutex, std::adopt_lock_t());

	ok = (m_cond_var->m_cv.wait_for(cpp_lock, std::chrono::microseconds(micros)) ==
	      std::cv_status::no_timeout);
	cpp_lock.release();
	return ok;
}

void CondVar::Signal() {
	m_cond_var->m_cv.notify_one();
}

void CondVar::SignalAll() {
	m_cond_var->m_cv.notify_all();
}

int Thread::GetThreadIdUnique() {
	static thread_local int tid = ++g_thread_counter;
	return tid;
}

} // namespace Common
