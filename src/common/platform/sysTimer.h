#ifndef KYTY_COMMON_PLATFORM_SYSTIMER_H_
#define KYTY_COMMON_PLATFORM_SYSTIMER_H_

#include "common/common.h"

#include <ctime>

struct SysTimeStruct {
	uint16_t Year;         // NOLINT(readability-identifier-naming)
	uint16_t Month;        // NOLINT(readability-identifier-naming)
	uint16_t Day;          // NOLINT(readability-identifier-naming)
	uint16_t Hour;         // NOLINT(readability-identifier-naming)
	uint16_t Minute;       // NOLINT(readability-identifier-naming)
	uint16_t Second;       // NOLINT(readability-identifier-naming)
	uint16_t Milliseconds; // NOLINT(readability-identifier-naming)
	bool     is_invalid;   // NOLINT(readability-identifier-naming)
};

struct SysFileTimeStruct {
	// Nanoseconds preserve sub-second file timestamps.
	time_t time;
	long   nanos;
	bool is_invalid;
};

inline void SysFileToSystemTimeUtc(const SysFileTimeStruct& f, SysTimeStruct& t) {
	struct tm i {};

	if (f.is_invalid || gmtime_r(&f.time, &i) == nullptr) {
		t.is_invalid = true;
		return;
	}

	t.is_invalid   = false;
	t.Year         = i.tm_year + 1900;
	t.Month        = i.tm_mon + 1;
	t.Day          = i.tm_mday;
	t.Hour         = i.tm_hour;
	t.Minute       = i.tm_min;
	t.Second       = (i.tm_sec == 60 ? 59 : i.tm_sec);
	t.Milliseconds = static_cast<uint16_t>((f.nanos / 1000000) % 1000);
}

inline void SysTimeTToSystem(time_t t, SysTimeStruct& s) {
	SysFileTimeStruct ft {};
	ft.time       = t;
	ft.is_invalid = false;
	SysFileToSystemTimeUtc(ft, s);
}

inline void SysSystemToFileTimeUtc(const SysTimeStruct& f, SysFileTimeStruct& t) {
	struct tm i {};

	i.tm_year = f.Year - 1900;
	i.tm_mon  = f.Month - 1;
	i.tm_mday = f.Day;
	i.tm_hour = f.Hour;
	i.tm_min  = f.Minute;
	i.tm_sec  = f.Second;

	t.is_invalid = (f.is_invalid || (t.time = ::timegm(&i)) == static_cast<time_t>(-1));
}

// Retrieves the current local date and time.
inline void SysGetSystemTime(SysTimeStruct& t) {
	// Preserve millisecond precision.
	timespec  now {};
	struct tm i {};

	if (clock_gettime(CLOCK_REALTIME, &now) != 0 || localtime_r(&now.tv_sec, &i) == nullptr) {
		t.is_invalid = true;
		return;
	}

	t.is_invalid   = false;
	t.Year         = i.tm_year + 1900;
	t.Month        = i.tm_mon + 1;
	t.Day          = i.tm_mday;
	t.Hour         = i.tm_hour;
	t.Minute       = i.tm_min;
	t.Second       = (i.tm_sec == 60 ? 59 : i.tm_sec);
	t.Milliseconds = static_cast<uint16_t>((now.tv_nsec / 1000000) % 1000);
}

// Retrieves the current system date and time in Coordinated Universal Time (UTC).
inline void SysGetSystemTimeUtc(SysTimeStruct& t) {
	// Preserve millisecond precision.
	timespec  now {};
	struct tm i {};

	if (clock_gettime(CLOCK_REALTIME, &now) != 0 || gmtime_r(&now.tv_sec, &i) == nullptr) {
		t.is_invalid = true;
		return;
	}

	t.is_invalid   = false;
	t.Year         = i.tm_year + 1900;
	t.Month        = i.tm_mon + 1;
	t.Day          = i.tm_mday;
	t.Hour         = i.tm_hour;
	t.Minute       = i.tm_min;
	t.Second       = (i.tm_sec == 60 ? 59 : i.tm_sec);
	t.Milliseconds = static_cast<uint16_t>((now.tv_nsec / 1000000) % 1000);
}

inline void SysQueryPerformanceFrequency(uint64_t* freq) {
	*freq = 1000000000LL;
}

inline void SysQueryPerformanceCounter(uint64_t* counter) {
	struct timespec now {};
	clock_gettime(CLOCK_MONOTONIC, &now);
	*counter = now.tv_sec * 1000000000LL + now.tv_nsec;
}

#endif /* KYTY_COMMON_PLATFORM_SYSTIMER_H_ */
