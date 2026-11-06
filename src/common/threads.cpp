#include "common/threads.h"

#include <atomic>
#include <cerrno>
#include <chrono> // IWYU pragma: keep
#include <ctime>
#include <memory>
#include <thread>

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#include <windows.h> // IWYU pragma: keep
// IWYU pragma: no_include <winbase.h>
constexpr DWORD KYTY_CS_SPIN_COUNT = 4000;

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

// High-resolution waitable timers need Windows 10 1803, the floor MapViewOfFile3 already sets.
static void SleepHighResolution100ns(uint64_t units_100ns) {
	if (units_100ns == 0) {
		return;
	}
	// Closed when the thread exits.
	thread_local const std::unique_ptr<void, decltype(&CloseHandle)> timer {
	    CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
		                       TIMER_ALL_ACCESS),
	    &CloseHandle};
	LARGE_INTEGER due_time {};
	due_time.QuadPart = -static_cast<LONGLONG>(units_100ns);
	if (SetWaitableTimerEx(timer.get(), &due_time, 0, nullptr, nullptr, nullptr, 0) != 0) {
		WaitForSingleObject(timer.get(), INFINITE);
	}
}

// IWYU pragma: no_include <minwindef.h>
// IWYU pragma: no_include <synchapi.h>
// IWYU pragma: no_include <minwinbase.h>
// IWYU pragma: no_include <errhandlingapi.h>
// IWYU pragma: no_include <winerror.h>

static CRITICAL_SECTION* ToCs(unsigned char* storage) {
	return reinterpret_cast<CRITICAL_SECTION*>(storage);
}

static CONDITION_VARIABLE* ToCv(void** storage) {
	return reinterpret_cast<CONDITION_VARIABLE*>(storage);
}

#endif

namespace Common {

static wait_poll_func_t g_cond_wait_poll_callback = nullptr;

static std::thread::id g_main_thread;

void InitializeThreads() {
	g_main_thread = std::this_thread::get_id();
}

void Thread::SleepMicro(uint32_t micros) {
	SleepNano(uint64_t {micros} * 1000);
}

void Thread::SleepNano(uint64_t nanos) {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	SleepHighResolution100ns((nanos + 99) / 100);
#elif defined(__APPLE__) // no clock_nanosleep
	std::this_thread::sleep_for(std::chrono::nanoseconds(nanos));
#else
	if (nanos == 0) {
		return;
	}
	constexpr uint64_t NANOS_PER_SEC = 1000000000;
	// An absolute deadline preserves the requested sleep across signal interruptions.
	timespec deadline {};
	clock_gettime(CLOCK_MONOTONIC, &deadline);
	auto target_nsec = static_cast<uint64_t>(deadline.tv_nsec) + nanos;
	deadline.tv_sec += static_cast<time_t>(target_nsec / NANOS_PER_SEC);
	deadline.tv_nsec = static_cast<long>(target_nsec % NANOS_PER_SEC);
	while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, nullptr) == EINTR) {
	}
#endif
}

bool Thread::IsMainThread() {
	return g_main_thread == std::this_thread::get_id();
}

int Thread::GetThreadIdUnique() {
	static std::atomic<int> counter = 0;
	static thread_local int tid     = ++counter;
	return tid;
}

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
Mutex::Mutex() {
	static_assert(sizeof(CRITICAL_SECTION) == sizeof(m_cs) &&
	              alignof(CRITICAL_SECTION) <= alignof(Mutex));
	InitializeCriticalSectionAndSpinCount(ToCs(m_cs), KYTY_CS_SPIN_COUNT);
}

Mutex::~Mutex() {
	DeleteCriticalSection(ToCs(m_cs));
}

void Mutex::Lock() {
	EnterCriticalSection(ToCs(m_cs));
}

void Mutex::Unlock() {
	LeaveCriticalSection(ToCs(m_cs));
}

void CondVar::Signal() {
	WakeConditionVariable(ToCv(&m_cv));
}

void CondVar::SignalAll() {
	WakeAllConditionVariable(ToCv(&m_cv));
}
#endif

void CondVar::Wait(Mutex* mutex) {
	auto* callback = g_cond_wait_poll_callback;
	if (callback == nullptr) {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		SleepConditionVariableCS(ToCv(&m_cv), ToCs(mutex->m_cs), INFINITE);
#else
		m_cv.wait(mutex->m_mutex);
#endif
	} else if (!WaitFor(mutex, 10000)) {
		mutex->Unlock();
		callback();
		mutex->Lock();
	}
}

void CondVar::SetWaitPollCallback(wait_poll_func_t callback) {
	g_cond_wait_poll_callback = callback;
}

bool CondVar::WaitFor(Mutex* mutex, uint32_t micros) {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	return SleepConditionVariableCS(ToCv(&m_cv), ToCs(mutex->m_cs),
	                                micros < 1000 ? 1 : micros / 1000) != 0 ||
	       GetLastError() != ERROR_TIMEOUT;
#else
	return m_cv.wait_for(mutex->m_mutex, std::chrono::microseconds(micros)) ==
	       std::cv_status::no_timeout;
#endif
}

} // namespace Common
