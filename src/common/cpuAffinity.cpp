#include "common/cpuAffinity.h"

#include "common/common.h"
#include "common/logging/log.h"

#include <algorithm>
#include <cstdint>
#include <vector>

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h> // IWYU pragma: keep
#elif !defined(__APPLE__)
#include <fstream>
#include <pthread.h>
#include <sched.h>
#include <string>
#include <unistd.h>
#endif

namespace Common {

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS

void PreferPerformanceCores(const char* name) {
	ULONG length = 0;
	GetSystemCpuSetInformation(nullptr, 0, &length, GetCurrentProcess(), 0);
	if (length == 0) {
		return;
	}
	std::vector<uint8_t> buffer(length);
	auto* first = reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(buffer.data());
	if (!GetSystemCpuSetInformation(first, length, &length, GetCurrentProcess(), 0)) {
		return;
	}
	struct Set {
		ULONG id;
		BYTE  efficiency;
	};
	std::vector<Set> sets;
	BYTE             lowest = 0xff, highest = 0;
	for (ULONG offset = 0; offset < length;) {
		const auto* info = reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(buffer.data() + offset);
		if (info->Size == 0) {
			break;
		}
		if (info->Type == CpuSetInformation) {
			sets.push_back({info->CpuSet.Id, info->CpuSet.EfficiencyClass});
			lowest  = std::min(lowest, info->CpuSet.EfficiencyClass);
			highest = std::max(highest, info->CpuSet.EfficiencyClass);
		}
		offset += info->Size;
	}
	if (sets.empty() || lowest == highest) {
		return; // All cores alike.
	}
	std::vector<ULONG> fast;
	for (const auto& set: sets) {
		if (set.efficiency == highest) {
			fast.push_back(set.id);
		}
	}
	const bool ok =
	    SetThreadSelectedCpuSets(GetCurrentThread(), fast.data(), static_cast<ULONG>(fast.size())) != 0;
	LOGF("CPU: %s prefers the %zu fastest of %zu logical processors (efficiency class %u)%s\n", name,
	     fast.size(), sets.size(), static_cast<unsigned>(highest), ok ? "" : " - failed");
}

#elif !defined(__APPLE__)

void PreferPerformanceCores(const char* name) {
	const long count = sysconf(_SC_NPROCESSORS_CONF);
	if (count <= 0) {
		return;
	}
	std::vector<unsigned long> max_khz(static_cast<size_t>(count), 0);
	unsigned long              lowest = ~0ul, highest = 0;
	for (long cpu = 0; cpu < count; cpu++) {
		std::ifstream file("/sys/devices/system/cpu/cpu" + std::to_string(cpu) +
		                   "/cpufreq/cpuinfo_max_freq");
		unsigned long khz = 0;
		if (!(file >> khz) || khz == 0) {
			return; // Unknown: leave the scheduler alone.
		}
		max_khz[static_cast<size_t>(cpu)] = khz;
		lowest                            = std::min(lowest, khz);
		highest                           = std::max(highest, khz);
	}
	// Hybrid only: efficiency cores top out well below the performance cores.
	if (lowest * 100ul >= highest * 85ul) {
		return;
	}
	cpu_set_t set;
	CPU_ZERO(&set);
	size_t fast = 0;
	for (long cpu = 0; cpu < count; cpu++) {
		if (max_khz[static_cast<size_t>(cpu)] * 100ul >= highest * 85ul) {
			CPU_SET(static_cast<int>(cpu), &set);
			fast++;
		}
	}
	const bool ok = pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0;
	LOGF("CPU: %s runs on the %zu fastest of %ld logical processors (up to %lu MHz)%s\n", name, fast,
	     count, highest / 1000ul, ok ? "" : " - failed");
}

#else

void PreferPerformanceCores(const char* /*name*/) {}

#endif

} // namespace Common
