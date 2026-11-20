#include "common/threads.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono> // IWYU pragma: keep
#include <ctime>
#include <memory>
#include <thread>
#include <tuple>

#ifdef __linux__
#include <fstream>
#include <map>
#include <sched.h>
#include <string>

struct CoreReservation {
	cpu_set_t allowed {};
	cpu_set_t cpus {};
	int       first = -1;
};

// Threads inherit affinity, so the calling thread and every thread it starts later leave the core.
static const CoreReservation& GetCoreReservation() {
	static const auto reservation = []() -> CoreReservation {
		cpu_set_t allowed {};
		if (sched_getaffinity(0, sizeof(allowed), &allowed) != 0) {
			return {};
		}
		struct Core {
			cpu_set_t cpus {};
			int       lowest      = CPU_SETSIZE;
			unsigned  performance = 0;
			unsigned  frequency   = 0;
		};
		// Keyed by the first CPU of the core's sibling list, which can lie outside the affinity.
		std::map<int, Core> cores;
		for (int cpu = 0; cpu < CPU_SETSIZE; cpu++) {
			if (!CPU_ISSET(cpu, &allowed)) {
				continue;
			}
			const auto path  = "/sys/devices/system/cpu/cpu" + std::to_string(cpu) + "/";
			int        first = 0;
			if (!(std::ifstream(path + "topology/thread_siblings_list") >> first)) {
				return {};
			}
			auto& core = cores[first];
			CPU_SET(cpu, &core.cpus);
			core.lowest = std::min(core.lowest, cpu);
			std::ifstream(path + "acpi_cppc/highest_perf") >> core.performance;
			std::ifstream(path + "cpufreq/cpuinfo_max_freq") >> core.frequency;
		}
		// Leave the guest at least the seven cores a PS5 game expects for its own threads.
		if (cores.size() < 8) {
			return {};
		}
		// The highest performance, then frequency, then the last core; core 0 serves interrupts.
		const auto best = std::ranges::max_element(cores, {}, [](const auto& entry) {
			return std::tuple(entry.second.performance, entry.second.frequency, entry.first);
		});
		// Without either value the fastest core is unknown.
		if (best->second.performance == 0 && best->second.frequency == 0) {
			return {};
		}
		cpu_set_t others {};
		CPU_XOR(&others, &allowed, &best->second.cpus);
		if (sched_setaffinity(0, sizeof(others), &others) != 0) {
			return {};
		}
		return {allowed, best->second.cpus, best->second.lowest};
	}();
	return reservation;
}
#endif

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#include <windows.h> // IWYU pragma: keep
// IWYU pragma: no_include <winbase.h>

#include <bitset>
#include <vector>

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

static int ReserveCore() {
	// Leave explicit affinity/CPU-set policies and multi-group systems alone.
	DWORD_PTR      process_mask = 0;
	DWORD_PTR      system_mask  = 0;
	GROUP_AFFINITY thread_affinity {};
	ULONG          count = 0;
	if (GetActiveProcessorGroupCount() != 1 ||
	    GetProcessAffinityMask(GetCurrentProcess(), &process_mask, &system_mask) == 0 ||
	    GetThreadGroupAffinity(GetCurrentThread(), &thread_affinity) == 0 ||
	    process_mask != system_mask || thread_affinity.Mask != process_mask ||
	    GetProcessDefaultCpuSets(GetCurrentProcess(), nullptr, 0, &count) == 0 || count != 0 ||
	    GetThreadSelectedCpuSets(GetCurrentThread(), nullptr, 0, &count) == 0 || count != 0) {
		return -1;
	}
	ULONG size = 0;
	GetSystemCpuSetInformation(nullptr, 0, &size, GetCurrentProcess(), 0);
	std::vector<uint8_t> buffer(size);
	if (GetSystemCpuSetInformation(reinterpret_cast<SYSTEM_CPU_SET_INFORMATION*>(buffer.data()),
	                               size, &size, GetCurrentProcess(), 0) == 0) {
		return -1;
	}
	using CpuSet = decltype(SYSTEM_CPU_SET_INFORMATION::CpuSet);
	std::vector<const CpuSet*> sets;
	std::bitset<256>           cores;
	for (ULONG offset = 0; offset < size;) {
		const auto& entry = *reinterpret_cast<const SYSTEM_CPU_SET_INFORMATION*>(&buffer[offset]);
		offset += entry.Size;
		if (entry.Type != CpuSetInformation ||
		    (entry.CpuSet.Allocated != 0 && entry.CpuSet.AllocatedToTargetProcess == 0)) {
			continue;
		}
		sets.push_back(&entry.CpuSet);
		cores.set(entry.CpuSet.CoreIndex);
	}
	// Leave the guest at least the seven cores a PS5 game expects for its own threads.
	if (cores.count() < 8) {
		return -1;
	}
	// Highest efficiency, then scheduling class, then the last core; core 0 serves interrupts.
	const auto* best = *std::ranges::max_element(sets, {}, [](const CpuSet* set) {
		return std::tuple(set->EfficiencyClass, set->SchedulingClass, set->CoreIndex);
	});

	std::vector<ULONG> reserved;
	std::vector<ULONG> others;
	int                first = best->LogicalProcessorIndex;
	for (const auto* set: sets) {
		if (set->CoreIndex == best->CoreIndex) {
			reserved.push_back(set->Id);
			first = std::min<int>(first, set->LogicalProcessorIndex);
		} else {
			others.push_back(set->Id);
		}
	}
	if (SetProcessDefaultCpuSets(GetCurrentProcess(), others.data(),
	                             static_cast<ULONG>(others.size())) == 0) {
		return -1;
	}
	if (SetThreadSelectedCpuSets(GetCurrentThread(), reserved.data(),
	                             static_cast<ULONG>(reserved.size())) == 0) {
		SetProcessDefaultCpuSets(GetCurrentProcess(), nullptr, 0);
		return -1;
	}
	return first;
}

#endif

namespace Common {

static wait_poll_func_t g_cond_wait_poll_callback = nullptr;

static std::thread::id g_main_thread;

void InitializeThreads() {
	g_main_thread = std::this_thread::get_id();
#ifdef __linux__
	GetCoreReservation();
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

int Thread::ReserveCoreForCurrentThread() {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	return ReserveCore();
#elif defined(__linux__)
	const auto& core = GetCoreReservation();
	return core.first >= 0 && sched_setaffinity(0, sizeof(core.cpus), &core.cpus) == 0 ? core.first
	                                                                                   : -1;
#else
	return -1;
#endif
}

#ifdef __linux__
void Thread::RestoreProcessAffinity() {
	if (const auto& core = GetCoreReservation(); core.first >= 0) {
		sched_setaffinity(0, sizeof(core.allowed), &core.allowed);
	}
}
#endif

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
