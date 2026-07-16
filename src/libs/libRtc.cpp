#include "common/abi.h"
#include "common/dateTime.h"
#include "libs/errno.h"
#include "libs/libs.h"
#include "loader/symbolDatabase.h"

#include <cstdint>
#include <cstdio>
#include <limits>

namespace Libs {

namespace LibRtc {

LIB_VERSION("Rtc", 1, "Rtc", 1, 1);

namespace Rtc {

constexpr int RTC_ERROR_DATETIME_UNINITIALIZED = 0x7ffef9fe;
constexpr int RTC_ERROR_INVALID_POINTER        = -2135621630; /* 0x80B50002 */
constexpr int RTC_ERROR_INVALID_VALUE          = -2135621629; /* 0x80B50003 */
constexpr int RTC_ERROR_INVALID_YEAR           = -2135621624; /* 0x80B50008 */
constexpr int RTC_ERROR_INVALID_MONTH          = -2135621623; /* 0x80B50009 */
constexpr int RTC_ERROR_INVALID_DAY            = -2135621622; /* 0x80B5000A */
constexpr int RTC_ERROR_INVALID_HOUR           = -2135621621; /* 0x80B5000B */
constexpr int RTC_ERROR_INVALID_MINUTE         = -2135621620; /* 0x80B5000C */
constexpr int RTC_ERROR_INVALID_SECOND         = -2135621619; /* 0x80B5000D */
constexpr int RTC_ERROR_INVALID_MICROSECOND    = -2135621618; /* 0x80B5000E */

constexpr uint64_t RTC_UNIX_EPOCH_TICKS           = 0xdcbffeff2bc000ull;
constexpr uint64_t RTC_WIN32_FILETIME_EPOCH_TICKS = 0xb36168b6a58000ull;

struct RtcDateTime {
	uint16_t year;
	uint16_t month;
	uint16_t day;
	uint16_t hour;
	uint16_t minute;
	uint16_t second;
	uint32_t microsecond;
};

static_assert(sizeof(RtcDateTime) == 16);

struct RtcTick {
	uint64_t tick;
};

static_assert(sizeof(RtcTick) == 8);

static int KYTY_SYSV_ABI RtcCheckValid(const RtcDateTime* time) {
	if (time == nullptr) {
		return RTC_ERROR_INVALID_POINTER;
	}
	if (time->year == 0 || time->year > 9999) {
		return RTC_ERROR_INVALID_YEAR;
	}
	if (time->month == 0 || time->month > 12) {
		return RTC_ERROR_INVALID_MONTH;
	}
	if (time->day == 0 || !Common::Date::IsValid(time->year, time->month, time->day)) {
		return RTC_ERROR_INVALID_DAY;
	}
	if (time->hour >= 24) {
		return RTC_ERROR_INVALID_HOUR;
	}
	if (time->minute >= 60) {
		return RTC_ERROR_INVALID_MINUTE;
	}
	if (time->second >= 60) {
		return RTC_ERROR_INVALID_SECOND;
	}
	if (time->microsecond >= 1000000) {
		return RTC_ERROR_INVALID_MICROSECOND;
	}

	return OK;
}

static int RtcGetDaysInMonth(int year, int month) {
	if (year <= 0 || year > 9999) {
		return RTC_ERROR_INVALID_YEAR;
	}
	if (month <= 0 || month > 12) {
		return RTC_ERROR_INVALID_MONTH;
	}

	if (month == 2 && Common::Date::IsLeapYear(year)) {
		return 29;
	}

	return Common::Date::DaysInMonth(month);
}

static int KYTY_SYSV_ABI RtcIsLeapYear(int year) {
	PRINT_NAME();

	if (year <= 0 || year > 9999) {
		return RTC_ERROR_INVALID_YEAR;
	}

	return Common::Date::IsLeapYear(year) ? 1 : 0;
}

static int KYTY_SYSV_ABI RtcGetDayOfWeek(int year, int month, int day) {
	PRINT_NAME();

	const auto days = RtcGetDaysInMonth(year, month);
	if (days < 0) {
		return days;
	}
	if (day <= 0 || day > days) {
		return RTC_ERROR_INVALID_DAY;
	}

	const auto dow = Common::Date(year, month, day).DayOfWeek();
	return (dow == 7 ? 0 : dow);
}

static int KYTY_SYSV_ABI RtcGetDaysInMonthExport(int year, int month) {
	PRINT_NAME();

	return RtcGetDaysInMonth(year, month);
}

static int KYTY_SYSV_ABI RtcGetTickResolution() {
	PRINT_NAME();

	return 1000000;
}

static int KYTY_SYSV_ABI RtcGetCurrentClockLocalTime(RtcDateTime* time) {
	PRINT_NAME();

	if (time == nullptr) {
		return RTC_ERROR_DATETIME_UNINITIALIZED;
	}

	const auto now  = Common::DateTime::FromSystem();
	const auto date = now.GetDate();
	const auto tod  = now.GetTime();

	time->year        = static_cast<uint16_t>(date.Year());
	time->month       = static_cast<uint16_t>(date.Month());
	time->day         = static_cast<uint16_t>(date.Day());
	time->hour        = static_cast<uint16_t>(tod.Hour24());
	time->minute      = static_cast<uint16_t>(tod.Minute());
	time->second      = static_cast<uint16_t>(tod.Second());
	time->microsecond = static_cast<uint32_t>(tod.Msec() * 1000);

	return OK;
}

static int KYTY_SYSV_ABI RtcSetTick(RtcDateTime* time, const RtcTick* tick) {
	PRINT_NAME();

	if (time == nullptr || tick == nullptr) {
		return RTC_ERROR_INVALID_POINTER;
	}

	auto days = tick->tick / 86400000000ull;
	auto usec = tick->tick % 86400000000ull;

	days += 307;

	auto j  = (days << 2) - 1;
	auto ly = j / 146097;

	j -= 146097 * ly;
	auto ld = j >> 2;

	j  = ((ld << 2) + 3) / 1461;
	ld = (((ld << 2) + 7) - 1461 * j) >> 2;

	auto lm = (5 * ld - 3) / 153;
	ld      = (5 * ld + 2 - 153 * lm) / 5;
	ly      = 100 * ly + j;

	if (lm < 10) {
		lm += 3;
	} else {
		lm -= 9;
		ly++;
	}

	if (ly == 0 || ly > 9999) {
		return RTC_ERROR_INVALID_VALUE;
	}

	time->year  = static_cast<uint16_t>(ly);
	time->month = static_cast<uint16_t>(lm);
	time->day   = static_cast<uint16_t>(ld);

	time->hour = static_cast<uint16_t>(usec / 3600000000ull);
	usec %= 3600000000ull;
	time->minute = static_cast<uint16_t>(usec / 60000000ull);
	usec %= 60000000ull;
	time->second = static_cast<uint16_t>(usec / 1000000ull);
	usec %= 1000000ull;
	time->microsecond = static_cast<uint32_t>(usec);

	return RtcCheckValid(time);
}

static int KYTY_SYSV_ABI RtcGetTick(const RtcDateTime* time, RtcTick* tick) {
	PRINT_NAME();

	if (time == nullptr || tick == nullptr) {
		return RTC_ERROR_INVALID_POINTER;
	}

	const auto valid = RtcCheckValid(time);
	if (valid != OK) {
		return valid;
	}

	uint64_t year  = time->year;
	uint64_t month = time->month;

	if (month > 2) {
		month -= 3;
	} else {
		month += 9;
		year -= 1;
	}

	const auto c  = year / 100;
	const auto ya = year - 100 * c;

	auto days = ((146097 * c) >> 2) + ((1461 * ya) >> 2) + (153 * month + 2) / 5 + time->day;
	days -= 307;
	days *= 86400000000ull;

	const auto usec = static_cast<uint64_t>(time->hour) * 3600000000ull +
	                  static_cast<uint64_t>(time->minute) * 60000000ull +
	                  static_cast<uint64_t>(time->second) * 1000000ull + time->microsecond;

	tick->tick = days + usec;

	return OK;
}

static int KYTY_SYSV_ABI RtcGetCurrentTick(RtcTick* tick) {
	PRINT_NAME();

	if (tick == nullptr) {
		return RTC_ERROR_DATETIME_UNINITIALIZED;
	}

	const auto now  = Common::DateTime::FromSystemUTC();
	const auto date = now.GetDate();
	const auto tod  = now.GetTime();

	RtcDateTime time {};
	time.year        = static_cast<uint16_t>(date.Year());
	time.month       = static_cast<uint16_t>(date.Month());
	time.day         = static_cast<uint16_t>(date.Day());
	time.hour        = static_cast<uint16_t>(tod.Hour24());
	time.minute      = static_cast<uint16_t>(tod.Minute());
	time.second      = static_cast<uint16_t>(tod.Second());
	time.microsecond = static_cast<uint32_t>(tod.Msec() * 1000);

	return RtcGetTick(&time, tick);
}

static int KYTY_SYSV_ABI RtcGetCurrentNetworkTick(RtcTick* tick) {
	PRINT_NAME();

	return RtcGetCurrentTick(tick);
}

static int KYTY_SYSV_ABI RtcGetCurrentClock(RtcDateTime* time, int time_zone_minutes) {
	PRINT_NAME();

	if (time == nullptr) {
		return RTC_ERROR_DATETIME_UNINITIALIZED;
	}

	RtcTick tick {};
	auto    ret = RtcGetCurrentTick(&tick);
	if (ret != OK) {
		return ret;
	}

	tick.tick += static_cast<int64_t>(time_zone_minutes) * 60000000ll;

	return RtcSetTick(time, &tick);
}

static int KYTY_SYSV_ABI RtcConvertUtcToLocalTime(const RtcTick* utc, RtcTick* local_time) {
	PRINT_NAME();

	if (utc == nullptr || local_time == nullptr) {
		return RTC_ERROR_INVALID_POINTER;
	}

	// Keep host-independent behavior for now; Kyty stores RTC ticks in UTC.
	*local_time = *utc;

	return OK;
}

static int KYTY_SYSV_ABI RtcConvertLocalTimeToUtc(const RtcTick* local_time, RtcTick* utc) {
	PRINT_NAME();

	if (local_time == nullptr || utc == nullptr) {
		return RTC_ERROR_INVALID_POINTER;
	}

	// Keep host-independent behavior for now; Kyty stores RTC ticks in UTC.
	*utc = *local_time;

	return OK;
}

static int KYTY_SYSV_ABI RtcFormatRFC3339(char* date_time, const RtcTick* utc,
                                          int time_zone_minutes) {
	PRINT_NAME();

	if (date_time == nullptr) {
		return RTC_ERROR_INVALID_POINTER;
	}

	RtcTick tick {};
	if (utc == nullptr) {
		auto ret = RtcGetCurrentTick(&tick);
		if (ret != OK) {
			return ret;
		}
	} else {
		tick = *utc;
	}

	const auto offset_ticks = static_cast<int64_t>(time_zone_minutes) * 60000000ll;
	if (offset_ticks < 0 && tick.tick < static_cast<uint64_t>(-offset_ticks)) {
		return RTC_ERROR_INVALID_VALUE;
	}
	tick.tick = static_cast<uint64_t>(static_cast<int64_t>(tick.tick) + offset_ticks);

	RtcDateTime time {};
	auto        ret = RtcSetTick(&time, &tick);
	if (ret != OK) {
		return ret;
	}

	char zone[8] {};
	if (time_zone_minutes == 0) {
		std::snprintf(zone, sizeof(zone), "Z");
	} else {
		const auto sign    = (time_zone_minutes < 0 ? '-' : '+');
		auto       minutes = (time_zone_minutes < 0 ? -time_zone_minutes : time_zone_minutes);
		std::snprintf(zone, sizeof(zone), "%c%02d:%02d", sign, minutes / 60, minutes % 60);
	}

	std::snprintf(date_time, 32, "%04u-%02u-%02uT%02u:%02u:%02u.%02u%s",
	              static_cast<unsigned>(time.year), static_cast<unsigned>(time.month),
	              static_cast<unsigned>(time.day), static_cast<unsigned>(time.hour),
	              static_cast<unsigned>(time.minute), static_cast<unsigned>(time.second),
	              static_cast<unsigned>(time.microsecond / 10000), zone);

	return OK;
}

static int KYTY_SYSV_ABI RtcParseRFC3339(RtcTick* utc, const char* date_time) {
	PRINT_NAME();

	if (utc == nullptr || date_time == nullptr) {
		return RTC_ERROR_INVALID_POINTER;
	}

	RtcDateTime time {};
	int         parsed = 0;
	int         count = std::sscanf(date_time, "%hu-%hu-%huT%hu:%hu:%hu%n", &time.year, &time.month,
	                                &time.day, &time.hour, &time.minute, &time.second, &parsed);
	if (count != 6) {
		return RTC_ERROR_INVALID_VALUE;
	}

	const char* pos = date_time + parsed;
	if (*pos == '.') {
		pos++;
		uint32_t microsecond = 0;
		int      digits      = 0;
		while (*pos >= '0' && *pos <= '9') {
			if (digits < 6) {
				microsecond = microsecond * 10 + static_cast<uint32_t>(*pos - '0');
			}
			digits++;
			pos++;
		}
		while (digits > 0 && digits < 6) {
			microsecond *= 10;
			digits++;
		}
		time.microsecond = microsecond;
	}

	auto ret = RtcGetTick(&time, utc);
	if (ret != OK) {
		return ret;
	}

	if (*pos == 'Z' || *pos == '\0') {
		return OK;
	}
	if (*pos != '+' && *pos != '-') {
		return RTC_ERROR_INVALID_VALUE;
	}

	int zone_hour = 0;
	int zone_min  = 0;
	if (std::sscanf(pos + 1, "%02d:%02d", &zone_hour, &zone_min) != 2) {
		return RTC_ERROR_INVALID_VALUE;
	}

	int zone_minutes = zone_hour * 60 + zone_min;
	if (*pos == '-') {
		zone_minutes = -zone_minutes;
	}

	const auto offset_ticks = static_cast<int64_t>(zone_minutes) * 60000000ll;
	if (offset_ticks > 0 && utc->tick < static_cast<uint64_t>(offset_ticks)) {
		return RTC_ERROR_INVALID_VALUE;
	}
	utc->tick = static_cast<uint64_t>(static_cast<int64_t>(utc->tick) - offset_ticks);

	return OK;
}

static int KYTY_SYSV_ABI RtcGetTime_t(const RtcDateTime* time, int64_t* seconds) {
	PRINT_NAME();

	if (time == nullptr || seconds == nullptr) {
		return RTC_ERROR_INVALID_POINTER;
	}

	RtcTick tick {};
	auto    ret = RtcGetTick(time, &tick);
	if (ret != OK) {
		return ret;
	}

	*seconds = (tick.tick < RTC_UNIX_EPOCH_TICKS
	                ? 0
	                : static_cast<int64_t>((tick.tick - RTC_UNIX_EPOCH_TICKS) / 1000000ull));

	return OK;
}

static int KYTY_SYSV_ABI RtcGetWin32FileTime(const RtcDateTime* time, uint64_t* win32_time) {
	PRINT_NAME();

	if (time == nullptr || win32_time == nullptr) {
		return RTC_ERROR_INVALID_POINTER;
	}

	RtcTick tick {};
	auto    ret = RtcGetTick(time, &tick);
	if (ret != OK) {
		return ret;
	}

	*win32_time = (tick.tick < RTC_WIN32_FILETIME_EPOCH_TICKS
	                   ? 0
	                   : (tick.tick - RTC_WIN32_FILETIME_EPOCH_TICKS) * 10ull);

	return OK;
}

static int KYTY_SYSV_ABI RtcSetWin32FileTime(RtcDateTime* time, uint64_t win32_time) {
	PRINT_NAME();

	if (time == nullptr) {
		return RTC_ERROR_INVALID_POINTER;
	}

	if (win32_time / 10ull >
	    std::numeric_limits<uint64_t>::max() - RTC_WIN32_FILETIME_EPOCH_TICKS) {
		return RTC_ERROR_INVALID_VALUE;
	}

	RtcTick tick {};
	tick.tick = win32_time / 10ull + RTC_WIN32_FILETIME_EPOCH_TICKS;

	return RtcSetTick(time, &tick);
}

static int KYTY_SYSV_ABI RtcSetTime_t(RtcDateTime* time, int64_t seconds) {
	PRINT_NAME();

	if (time == nullptr) {
		return RTC_ERROR_INVALID_POINTER;
	}
	if (seconds < 0) {
		return RTC_ERROR_INVALID_VALUE;
	}

	const auto sec = static_cast<uint64_t>(seconds);
	if (sec > (std::numeric_limits<uint64_t>::max() - RTC_UNIX_EPOCH_TICKS) / 1000000ull) {
		return RTC_ERROR_INVALID_VALUE;
	}

	RtcTick tick {};
	tick.tick = sec * 1000000ull + RTC_UNIX_EPOCH_TICKS;

	return RtcSetTick(time, &tick);
}

static int KYTY_SYSV_ABI RtcTickAddTicks(RtcTick* dst, const RtcTick* src, int64_t ticks) {
	PRINT_NAME();

	if (dst == nullptr || src == nullptr) {
		return RTC_ERROR_INVALID_POINTER;
	}

	dst->tick = src->tick + ticks;

	return OK;
}

static int KYTY_SYSV_ABI RtcTickAddMicroseconds(RtcTick* dst, const RtcTick* src, int64_t usec) {
	PRINT_NAME();

	return RtcTickAddTicks(dst, src, usec);
}

static int KYTY_SYSV_ABI RtcTickAddSeconds(RtcTick* dst, const RtcTick* src, int64_t seconds) {
	PRINT_NAME();

	return RtcTickAddTicks(dst, src, seconds * 1000000ll);
}

static int KYTY_SYSV_ABI RtcTickAddMinutes(RtcTick* dst, const RtcTick* src, int64_t minutes) {
	PRINT_NAME();

	return RtcTickAddTicks(dst, src, minutes * 60000000ll);
}

static int KYTY_SYSV_ABI RtcTickAddHours(RtcTick* dst, const RtcTick* src, int32_t hours) {
	PRINT_NAME();

	return RtcTickAddTicks(dst, src, static_cast<int64_t>(hours) * 3600000000ll);
}

static int KYTY_SYSV_ABI RtcTickAddDays(RtcTick* dst, const RtcTick* src, int32_t days) {
	PRINT_NAME();

	return RtcTickAddTicks(dst, src, static_cast<int64_t>(days) * 86400000000ll);
}

static int KYTY_SYSV_ABI RtcTickAddWeeks(RtcTick* dst, const RtcTick* src, int32_t weeks) {
	PRINT_NAME();

	return RtcTickAddTicks(dst, src, static_cast<int64_t>(weeks) * 604800000000ll);
}

} // namespace Rtc

LIB_DEFINE(InitRtc_1_Rtc) {
	LIB_FUNC("lPEBYdVX0XQ", Rtc::RtcCheckValid);
	LIB_FUNC("8Yr143yEnRo", Rtc::RtcConvertLocalTimeToUtc);
	LIB_FUNC("M1TvFst-jrM", Rtc::RtcConvertUtcToLocalTime);
	LIB_FUNC("WJ3rqFwymew", Rtc::RtcFormatRFC3339);
	LIB_FUNC("8lfvnRMqwEM", Rtc::RtcGetCurrentClock);
	LIB_FUNC("ZPD1YOKI+Kw", Rtc::RtcGetCurrentClockLocalTime);
	LIB_FUNC("18B2NS1y9UU", Rtc::RtcGetCurrentTick);
	LIB_FUNC("zO9UL3qIINQ", Rtc::RtcGetCurrentNetworkTick);
	LIB_FUNC("CyIK-i4XdgQ", Rtc::RtcGetDayOfWeek);
	LIB_FUNC("Ug8pCwQvh0c", Rtc::RtcIsLeapYear);
	LIB_FUNC("3O7Ln8AqJ1o", Rtc::RtcGetDaysInMonthExport);
	LIB_FUNC("8w-H19ip48I", Rtc::RtcGetTick);
	LIB_FUNC("jMNwqYr4R-k", Rtc::RtcGetTickResolution);
	LIB_FUNC("BtqmpTRXHgk", Rtc::RtcGetTime_t);
	LIB_FUNC("jfRO0uTjtzA", Rtc::RtcGetWin32FileTime);
	LIB_FUNC("99bMGglFW3I", Rtc::RtcParseRFC3339);
	LIB_FUNC("n5JiAJXsbcs", Rtc::RtcSetWin32FileTime);
	LIB_FUNC("ueega6v3GUw", Rtc::RtcSetTick);
	LIB_FUNC("bDEVVP4bTjQ", Rtc::RtcSetTime_t);
	LIB_FUNC("NR1J0N7L2xY", Rtc::RtcTickAddDays);
	LIB_FUNC("MDc5cd8HfCA", Rtc::RtcTickAddHours);
	LIB_FUNC("XPIiw58C+GM", Rtc::RtcTickAddMicroseconds);
	LIB_FUNC("mn-tf4QiFzk", Rtc::RtcTickAddMinutes);
	LIB_FUNC("07O525HgICs", Rtc::RtcTickAddSeconds);
	LIB_FUNC("AqVMssr52Rc", Rtc::RtcTickAddTicks);
	LIB_FUNC("gI4t194c2W8", Rtc::RtcTickAddWeeks);
}

} // namespace LibRtc

LIB_DEFINE(InitRtc_1) {
	LibRtc::InitRtc_1_Rtc(s);
}

} // namespace Libs
