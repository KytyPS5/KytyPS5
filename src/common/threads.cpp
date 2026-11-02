#include "common/threads.h"

#include <algorithm>
#include <bit>
#include <atomic>
#include <cerrno>
#include <chrono> // IWYU pragma: keep
#include <ctime>
#include <memory>
#include <thread>
#include <vector>

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#include <windows.h> // IWYU pragma: keep
#include <tlhelp32.h>
// IWYU pragma: no_include <winbase.h>
constexpr DWORD KYTY_CS_SPIN_COUNT = 4000;

// The logical processors other than those of the cores Thread::ReserveCores() keeps for pinned
// threads (0 while none are reserved), and the next ideal processor to hand out among them.
static std::atomic<uint64_t> g_unreserved_processors {0};
static std::atomic<uint32_t> g_next_ideal_processor {0};

// A default CPU set alone does not keep a thread off the reserved cores: Windows still gives the
// thread an ideal processor there, and a thread made ready while every allowed processor is busy
// waits in that processor's queue, which the thread pinned there never yields to. Astro Bot's
// game threads waited there 2-6 s at a time. A hard affinity, with the ideal processor inside it,
// keeps such threads among the processors they may use. Threads that chose their own processors
// keep them.
static void KeepThreadOffReservedCores(HANDLE thread) noexcept {
	const auto unreserved = g_unreserved_processors.load(std::memory_order_acquire);
	if (unreserved == 0) {
		return;
	}
	DWORD_PTR process_mask = 0;
	DWORD_PTR system_mask  = 0;
	if (!::GetProcessAffinityMask(::GetCurrentProcess(), &process_mask, &system_mask)) {
		return;
	}
	const auto previous = ::SetThreadAffinityMask(thread, static_cast<DWORD_PTR>(unreserved));
	if (previous == 0) {
		return;
	}
	if (previous != process_mask) {
		(void)::SetThreadAffinityMask(thread, previous);
		return;
	}
	auto       allowed = unreserved;
	const auto count   = static_cast<uint32_t>(std::popcount(allowed));
	for (auto skip = g_next_ideal_processor.fetch_add(1, std::memory_order_relaxed) % count;
	     skip != 0; skip--) {
		allowed &= allowed - 1;
	}
	(void)::SetThreadIdealProcessor(thread, static_cast<DWORD>(std::countr_zero(allowed)));
}

// Runs in every thread of the process when it starts, also in those that other libraries and the
// graphics driver create.
static void NTAPI KeepNewThreadOffReservedCores(PVOID /*module*/, DWORD reason, PVOID /*reserved*/) {
	if (reason == DLL_THREAD_ATTACH) {
		KeepThreadOffReservedCores(::GetCurrentThread());
	}
}

#pragma section(".CRT$XLK", read)
extern "C" __declspec(allocate(".CRT$XLK")) const PIMAGE_TLS_CALLBACK kyty_keep_new_thread_off_reserved_cores =
    KeepNewThreadOffReservedCores;
#pragma comment(linker, "/INCLUDE:_tls_used")
#pragma comment(linker, "/INCLUDE:kyty_keep_new_thread_off_reserved_cores")

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

void Thread::RaiseCurrentPriority() {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
#endif
}

void Thread::LowerCurrentPriority() {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
}

void Thread::PinCurrent(int logical_cpu) {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	if (logical_cpu >= 0 && logical_cpu < 64) {
		const auto mask = DWORD_PTR {1} << static_cast<uint32_t>(logical_cpu);
		::SetThreadAffinityMask(::GetCurrentThread(), mask);
		::SetThreadIdealProcessor(::GetCurrentThread(), static_cast<DWORD>(logical_cpu));
	}
#else
	(void)logical_cpu;
#endif
}

void Thread::ReserveCores(std::span<const int> logical_cpus) {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	ULONG length = 0;
	(void)::GetSystemCpuSetInformation(nullptr, 0, &length, ::GetCurrentProcess(), 0);
	if (length == 0) {
		return;
	}
	std::vector<uint8_t> buffer(length);
	auto* const          first = reinterpret_cast<SYSTEM_CPU_SET_INFORMATION*>(buffer.data());
	if (!::GetSystemCpuSetInformation(first, length, &length, ::GetCurrentProcess(), 0)) {
		return;
	}
	const auto for_each_set = [&](auto&& func) {
		for (ULONG offset = 0; offset + sizeof(SYSTEM_CPU_SET_INFORMATION) <= length;) {
			const auto* info =
			    reinterpret_cast<const SYSTEM_CPU_SET_INFORMATION*>(buffer.data() + offset);
			if (info->Type == CpuSetInformation) {
				func(info->CpuSet);
			}
			offset += info->Size;
		}
	};
	// The physical cores (group, core index) of the reserved logical processors.
	std::vector<std::pair<WORD, BYTE>> reserved;
	for_each_set([&](const auto& set) {
		for (const auto cpu: logical_cpus) {
			if (set.Group == 0 && cpu >= 0 && set.LogicalProcessorIndex == cpu) {
				reserved.emplace_back(set.Group, set.CoreIndex);
			}
		}
	});
	std::vector<ULONG>                 others;
	std::vector<std::pair<WORD, BYTE>> other_cores;
	for_each_set([&](const auto& set) {
		const std::pair core {set.Group, set.CoreIndex};
		if (std::ranges::find(reserved, core) == reserved.end()) {
			others.push_back(set.Id);
			if (std::ranges::find(other_cores, core) == other_cores.end()) {
				other_cores.push_back(core);
			}
		}
	});
	// Smaller processors keep every core for the guest, which expects about seven.
	constexpr size_t MinOtherCores = 6;
	if (reserved.empty() || other_cores.size() < MinOtherCores) {
		return;
	}
	(void)::SetProcessDefaultCpuSets(::GetCurrentProcess(), others.data(),
	                                 static_cast<ULONG>(others.size()));
	// The default CPU sets stay for threads that start without the callback above; every other
	// thread, those already running included, gets a hard affinity off the reserved cores. The
	// calling thread pins itself next.
	uint64_t unreserved = 0;
	for_each_set([&](const auto& set) {
		if (set.Group == 0 && set.LogicalProcessorIndex < 64 &&
		    std::ranges::find(reserved, std::pair {set.Group, set.CoreIndex}) == reserved.end()) {
			unreserved |= uint64_t {1} << set.LogicalProcessorIndex;
		}
	});
	g_unreserved_processors.store(unreserved, std::memory_order_release);
	if (HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
	    snapshot != INVALID_HANDLE_VALUE) {
		THREADENTRY32 entry {};
		entry.dwSize = sizeof(entry);
		for (BOOL ok = ::Thread32First(snapshot, &entry); ok; ok = ::Thread32Next(snapshot, &entry)) {
			if (entry.th32OwnerProcessID != ::GetCurrentProcessId() ||
			    entry.th32ThreadID == ::GetCurrentThreadId()) {
				continue;
			}
			if (HANDLE thread = ::OpenThread(THREAD_SET_INFORMATION | THREAD_QUERY_INFORMATION,
			                                 FALSE, entry.th32ThreadID);
			    thread != nullptr) {
				KeepThreadOffReservedCores(thread);
				::CloseHandle(thread);
			}
		}
		::CloseHandle(snapshot);
	}
#else
	(void)logical_cpus;
#endif
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
