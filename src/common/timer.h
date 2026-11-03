#ifndef KYTY_COMMON_TIMER_H_
#define KYTY_COMMON_TIMER_H_

#include "common/common.h"

namespace Common {

class Timer final {
public:
	Timer() noexcept;
	~Timer() = default;

	void Start();

	// return time in milliseconds
	[[nodiscard]] double GetTimeMs() const;

	// return time in seconds
	[[nodiscard]] double GetTimeS() const;

	KYTY_CLASS_NO_COPY(Timer);

	[[nodiscard]] static uint64_t QueryPerformanceFrequency();
	[[nodiscard]] static uint64_t QueryPerformanceCounter();

private:
	uint64_t m_Frequency = 0;
	uint64_t m_StartTime = 0;
};

} // namespace Common

#endif /* KYTY_COMMON_TIMER_H_ */
