#ifndef KYTY_COMMON_PLATFORM_SYSTIMER_H_
#define KYTY_COMMON_PLATFORM_SYSTIMER_H_

#include "common/common.h"

#include <algorithm>
#include <cstdint>
#include <ctime>

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

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
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	FILETIME time;
#elif KYTY_PLATFORM == KYTY_PLATFORM_LINUX
	// Nanoseconds preserve sub-second file timestamps.
	time_t time;
	long   nanos;
#endif
	bool is_invalid;
};

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS

// FILETIME counts 100 ns units from 1601-01-01.
inline int64_t SysFileTimeToNs(const SysFileTimeStruct& f) {
	const auto units =
	    static_cast<int64_t>((uint64_t {f.time.dwHighDateTime} << 32u) | f.time.dwLowDateTime);
	// Clamp times outside 1677-2262 instead of overflowing.
	return std::clamp<int64_t>(units - 116444736000000000, INT64_MIN / 100, INT64_MAX / 100) * 100;
}

// Retrieves the current local date and time.
// NOLINTNEXTLINE(google-runtime-references)
inline void SysGetSystemTime(SysTimeStruct& t) {
	SYSTEMTIME s;
	GetLocalTime(&s);

	t.is_invalid   = false;
	t.Year         = s.wYear;
	t.Month        = s.wMonth;
	t.Day          = s.wDay;
	t.Hour         = s.wHour;
	t.Minute       = s.wMinute;
	t.Second       = (s.wSecond == 60 ? 59 : s.wSecond);
	t.Milliseconds = s.wMilliseconds;
}

// Retrieves the current system date and time in Coordinated Universal Time (UTC).
// NOLINTNEXTLINE(google-runtime-references)
inline void SysGetSystemTimeUtc(SysTimeStruct& t) {
	SYSTEMTIME s;
	GetSystemTime(&s);

	t.is_invalid   = false;
	t.Year         = s.wYear;
	t.Month        = s.wMonth;
	t.Day          = s.wDay;
	t.Hour         = s.wHour;
	t.Minute       = s.wMinute;
	t.Second       = (s.wSecond == 60 ? 59 : s.wSecond);
	t.Milliseconds = s.wMilliseconds;
}

inline void SysQueryPerformanceFrequency(uint64_t* freq) {
	LARGE_INTEGER f;
	QueryPerformanceFrequency(&f);
	*freq = f.QuadPart;
}

inline void SysQueryPerformanceCounter(uint64_t* counter) {
	LARGE_INTEGER c;
	QueryPerformanceCounter(&c);
	*counter = c.QuadPart;
}

#elif KYTY_PLATFORM == KYTY_PLATFORM_LINUX

inline int64_t SysFileTimeToNs(const SysFileTimeStruct& f) {
	return int64_t {f.time} * 1000000000 + f.nanos;
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

#endif

#endif /* KYTY_COMMON_PLATFORM_SYSTIMER_H_ */
