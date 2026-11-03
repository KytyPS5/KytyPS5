#include "common/timer.h"

#include "common/platform/sysTimer.h"

namespace Common {

Timer::Timer() noexcept {
	SysQueryPerformanceFrequency(&m_Frequency);
}

void Timer::Start() {
	SysQueryPerformanceCounter(&m_StartTime);
}

// return time in milliseconds
double Timer::GetTimeMs() const {
	return GetTimeS() * 1000.0;
}

// return time in seconds
double Timer::GetTimeS() const {
	uint64_t current_time = 0;
	SysQueryPerformanceCounter(&current_time);

	return (static_cast<double>(current_time - m_StartTime)) / static_cast<double>(m_Frequency);
}

uint64_t Timer::QueryPerformanceFrequency() {
	uint64_t ret = 0;
	SysQueryPerformanceFrequency(&ret);
	return ret;
}

uint64_t Timer::QueryPerformanceCounter() {
	uint64_t ret = 0;
	SysQueryPerformanceCounter(&ret);
	return ret;
}

} // namespace Common
