#ifndef KYTY_COMMON_PLATFORM_SYSTIMER_H_
#define KYTY_COMMON_PLATFORM_SYSTIMER_H_

#include "common/common.h"

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
	time_t time;
#endif
	bool is_invalid;
};

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS

// NOLINTNEXTLINE(google-runtime-references)
inline void SysFileToSystemTimeUtc(const SysFileTimeStruct& f, SysTimeStruct& t) {
	SYSTEMTIME s;

	if (f.is_invalid || (FileTimeToSystemTime(&f.time, &s) == 0)) {
		t.is_invalid = true;
		return;
	}

	t.is_invalid   = false;
	t.Year         = s.wYear;
	t.Month        = s.wMonth;
	t.Day          = s.wDay;
	t.Hour         = s.wHour;
	t.Minute       = s.wMinute;
	t.Second       = (s.wSecond == 60 ? 59 : s.wSecond);
	t.Milliseconds = s.wMilliseconds;
}

// NOLINTNEXTLINE(google-runtime-references)
inline void SysTimeTToSystem(time_t t, SysTimeStruct& s) {
	SysFileTimeStruct ft {};
	LONGLONG          ll   = Int32x32To64(t, 10000000) + 116444736000000000;
	ft.time.dwLowDateTime  = static_cast<DWORD>(ll);
	ft.time.dwHighDateTime = static_cast<DWORD>(static_cast<uint64_t>(ll) >> 32u);
	ft.is_invalid          = false;
	SysFileToSystemTimeUtc(ft, s);
}

// NOLINTNEXTLINE(google-runtime-references)
inline void SysSystemToFileTimeUtc(const SysTimeStruct& f, SysFileTimeStruct& t) {
	SYSTEMTIME s;

	s.wYear         = f.Year;
	s.wMonth        = f.Month;
	s.wDay          = f.Day;
	s.wHour         = f.Hour;
	s.wMinute       = f.Minute;
	s.wSecond       = f.Second;
	s.wMilliseconds = f.Milliseconds;

	t.is_invalid = (f.is_invalid || (SystemTimeToFileTime(&s, &t.time) == 0));
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
	t.Milliseconds = 0;
}

inline void SysTimeTToSystem(time_t t, SysTimeStruct& s) {
	SysFileTimeStruct ft {};
	ft.time       = t;
	ft.is_invalid = false;
	SysFileToSystemTimeUtc(ft, s);
}

inline time_t SysTimegm(struct tm* tm) {
	return timegm(tm);
}

inline void SysSystemToFileTimeUtc(const SysTimeStruct& f, SysFileTimeStruct& t) {
	struct tm i {};

	i.tm_year = f.Year - 1900;
	i.tm_mon  = f.Month - 1;
	i.tm_mday = f.Day;
	i.tm_hour = f.Hour;
	i.tm_min  = f.Minute;
	i.tm_sec  = f.Second;

	t.is_invalid = (f.is_invalid || (t.time = SysTimegm(&i)) == static_cast<time_t>(-1));
}

// Retrieves the current local date and time.
inline void SysGetSystemTime(SysTimeStruct& t) {
	time_t    st {};
	struct tm i {};

	if (time(&st) == static_cast<time_t>(-1) || localtime_r(&st, &i) == nullptr) {
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
	t.Milliseconds = 0;
}

// Retrieves the current system date and time in Coordinated Universal Time (UTC).
inline void SysGetSystemTimeUtc(SysTimeStruct& t) {
	time_t    st {};
	struct tm i {};

	if (time(&st) == static_cast<time_t>(-1) || gmtime_r(&st, &i) == nullptr) {
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
	t.Milliseconds = 0;
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
