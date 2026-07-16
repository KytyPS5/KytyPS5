#include "common/dateTime.h"

#include "common/assert.h"
#include "common/platform/sysTimer.h"
#include "common/stringUtils.h"

#include <fmt/format.h>

namespace Common {

template <class T>
constexpr T FloorDiv(T a, T b) {
	if (b < 0) {
		if (a < 0) {
			return a / b;
		}
		b = -b;
		a = -a;
	}

	return (a - (a < 0 ? b - 1 : 0)) / b;
}

static void YmdToJd(int year, int month, int day, jd_t* jd) {
	jd_t t_year  = year;
	jd_t t_month = month;
	jd_t t_day   = day;

	if (t_year < 0) {
		t_year++;
	}

	jd_t a = FloorDiv<jd_t>(14 - t_month, 12);
	jd_t y = t_year + 4800 - a;
	jd_t m = t_month + 12 * a - 3;
	*jd    = t_day + FloorDiv<jd_t>(153 * m + 2, 5) + 365 * y + FloorDiv<jd_t>(y, 4) -
	         FloorDiv<jd_t>(y, 100) + FloorDiv<jd_t>(y, 400) - 32045;
}

static void JdToYmd(int* year, int* month, int* day, jd_t jd) {
	jd_t a = jd + 32044;
	jd_t b = FloorDiv<jd_t>(4 * a + 3, 146097);
	jd_t c = a - FloorDiv<jd_t>(146097 * b, 4);
	jd_t d = FloorDiv<jd_t>(4 * c + 3, 1461);
	jd_t e = c - FloorDiv<jd_t>(1461 * d, 4);
	jd_t m = FloorDiv<jd_t>(5 * e + 2, 153);

	int t_day   = e - FloorDiv<jd_t>(153 * m + 2, 5) + 1;
	int t_month = m + 3 - 12 * FloorDiv<jd_t>(m, 10);
	int t_year  = 100 * b + d - 4800 + FloorDiv<jd_t>(m, 10);

	if (t_year <= 0) {
		t_year--;
	}

	if (year != nullptr) {
		*year = t_year;
	}
	if (month != nullptr) {
		*month = t_month;
	}
	if (day != nullptr) {
		*day = t_day;
	}
}

static void HmsToMs(int hour, int minute, int second, int msec, int* ms) {
	*ms = hour * 60 * 60 * 1000 + minute * 60 * 1000 + second * 1000 + msec;
}

static void MsToHms(int* hour, int* minute, int* second, int* msec, int ms) {
	if (hour != nullptr) {
		*hour = ms / (60 * 60 * 1000);
	}
	if (minute != nullptr) {
		*minute = (ms % (60 * 60 * 1000)) / (60 * 1000);
	}
	if (second != nullptr) {
		*second = (ms / 1000) % 60;
	}
	if (msec != nullptr) {
		*msec = ms % 1000;
	}
}

Date::Date(int year, int month, int day) {
	Set(year, month, day);
}

bool Date::IsValid(int year, int month, int day) {
	if (year == 0) {
		return false;
	}

	return (day >= 1) &&
	       (day <= DaysInMonth(month) || (day == 29 && month == 2 && IsLeapYear(year)));
}

int Date::DaysInMonth() const {
	if (IsInvalid()) {
		return 0;
	}

	int year  = 0;
	int month = 0;
	JdToYmd(&year, &month, nullptr, m_jd);

	if (month == 2 && IsLeapYear(year)) {
		return 29;
	}

	return DaysInMonth(month);
}

bool Date::IsLeapYear() const {
	if (IsInvalid()) {
		return false;
	}

	int year = 0;
	JdToYmd(&year, nullptr, nullptr, m_jd);

	return IsLeapYear(year);
}

int Date::DaysInMonth(int month) {
	static const int days[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

	if (month < 1 || month > 12) {
		month = 0;
	}

	return days[month];
}

void Date::Set(int year, int month, int day) {
	if (IsValid(year, month, day)) {
		YmdToJd(year, month, day, &m_jd);
	} else {
		m_jd = DATE_JD_INVALID;
	}
}

void Date::Get(int* year, int* month, int* day) const {
	if (IsInvalid()) {
		if (year != nullptr) {
			*year = 0;
		}
		if (month != nullptr) {
			*month = 0;
		}
		if (day != nullptr) {
			*day = 0;
		}
	} else {
		JdToYmd(year, month, day, m_jd);
	}
}

bool Date::IsLeapYear(int year) {
	if (year < 1) {
		year++;
	}

	return ((year % 4) == 0 && (year % 100) != 0) || (year % 400) == 0;
}

int Date::Year() const {
	if (IsInvalid()) {
		return 0;
	}

	int year = 0;
	JdToYmd(&year, nullptr, nullptr, m_jd);

	return year;
}

int Date::Month() const {
	if (IsInvalid()) {
		return 0;
	}

	int month = 0;
	JdToYmd(nullptr, &month, nullptr, m_jd);

	return month;
}

int Date::Day() const {
	if (IsInvalid()) {
		return 0;
	}

	int day = 0;
	JdToYmd(nullptr, nullptr, &day, m_jd);

	return day;
}

int Date::DaysInYear() const {
	if (IsInvalid()) {
		return 0;
	}

	return IsLeapYear() ? 366 : 365;
}

int Date::DayOfWeek() const {
	if (IsInvalid()) {
		return 0;
	}

	if (m_jd >= 0) {
		return (m_jd % 7) + 1;
	}

	return ((m_jd + 1) % 7) + 7;
}

int Date::DayOfYear() const {
	if (IsInvalid()) {
		return 0;
	}

	jd_t d = 0;
	YmdToJd(Year(), 1, 1, &d);

	return m_jd - d + 1;
}

int Date::DaysInYear(int year) {
	return IsLeapYear(year) ? 366 : 365;
}

// NOLINTNEXTLINE(readability-non-const-parameter)
static bool FormatDate(const Date* d, const std::string& f, size_t* out_i, std::string* out_r) {
	static const char* month_short[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
	                                    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
	static const char* month_long[]  = {"January",   "February", "March",    "April",
	                                    "May",       "June",     "July",     "August",
	                                    "September", "October",  "November", "December"};
	static const char* day_short[]   = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
	static const char* day_long[]    = {"Monday", "Tuesday",  "Wednesday", "Thursday",
	                                    "Friday", "Saturday", "Sunday"};

	auto& i = *out_i;
	auto& r = *out_r;
	if (f.compare(i, 4, "YYYY") == 0) {
		int y = d->Year();
		EXIT_IF(y < 0 || y > 9999);
		r += fmt::format("{:04d}", y);
		i += 3;
	} else if (f.compare(i, 3, "YYY") == 0) {
		int y = d->Year();
		EXIT_IF(y < 0 || y > 9999);
		r += Mid(fmt::format("{:04d}", y), 1);
		i += 2;
	} else if (f.compare(i, 2, "YY") == 0) {
		int y = d->Year();
		EXIT_IF(y < 0 || y > 9999);
		r += Mid(fmt::format("{:04d}", y), 2);
		i += 1;
	} else if (f.compare(i, 1, "Y") == 0) {
		int y = d->Year();
		EXIT_IF(y < 0 || y > 9999);
		r += Mid(fmt::format("{:04d}", y), 3);
	} else if (f.compare(i, 1, "Q") == 0) {
		r += fmt::format("{}", d->QuarterOfYear());
	} else if (f.compare(i, 2, "MM") == 0) {
		r += fmt::format("{:02d}", d->Month());
		i += 1;
	} else if (f.compare(i, 5, "MONTH") == 0) {
		r += month_long[d->Month() - 1];
		i += 4;
	} else if (f.compare(i, 3, "MON") == 0) {
		r += month_short[d->Month() - 1];
		i += 2;
	} else if (f.compare(i, 3, "DDD") == 0) {
		r += fmt::format("{:03d}", d->DayOfYear());
		i += 2;
	} else if (f.compare(i, 2, "DD") == 0) {
		r += fmt::format("{:02d}", d->Day());
		i += 1;
	} else if (f.compare(i, 2, "DY") == 0) {
		r += day_short[d->DayOfWeek() - 1];
		i += 1;
	} else if (f.compare(i, 3, "DAY") == 0) {
		r += day_long[d->DayOfWeek() - 1];
		i += 2;
	} else if (f.compare(i, 1, "D") == 0) {
		r += fmt::format("{}", d->DayOfWeek());
	} else if (f.compare(i, 1, "J") == 0) {
		r += fmt::format("{}", d->JulianDay());
	} else {
		return false;
	}

	return true;
}

/**
 * YYYY       - 4-digit year
 * YYY, YY, Y - Last 3, 2, or 1 digit(s) of year.
 * Q          - Quarter of year (1, 2, 3, 4; JAN-MAR = 1).
 * MM         - Month (01-12; JAN = 01).
 * MON        - Abbreviated name of month.
 * MONTH      - Name of month
 * D          - Day of week (1-7).
 * DAY        - Name of day.
 * DY         - Abbreviated name of day.
 * DD         - Day of month (01-31).
 * DDD        - Day of year (001-366).
 * J          - Julian day
 */
std::string Date::ToString(const char* format) const {
	if (IsInvalid()) {
		return "";
	}

	std::string r;
	std::string f = format;

	for (size_t i = 0; i < f.size(); i++) {
		if (!FormatDate(this, f, &i, &r)) {
			r += f[i];
		}
	}

	return r;
}

int Date::QuarterOfYear() const {
	if (IsInvalid()) {
		return 0;
	}

	return (Month() - 1) / 3 + 1;
}

Date Date::FromMacros(const std::string& date) {
	auto lst = Split(date, ' ');

	if (lst.size() != 3) {
		return {};
	}

	static const char* month_str[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
	                                  "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

	int month = -1;
	for (int i = 0; i < 12; i++) {
		if (lst[0] == month_str[i]) {
			month = i + 1;
			break;
		}
	}

	if (month < 0) {
		return {};
	}

	return Date(ToInt32(lst[2]), month, ToInt32(lst[1]));
}

Time::Time(int hour, int minute, int second, int msec) {
	Set(hour, minute, second, msec);
}

void Time::Set(int hour, int minute, int second, int msec) {
	if (IsValid(hour, minute, second, msec)) {
		HmsToMs(hour, minute, second, msec, &m_ms);
	} else {
		m_ms = TIME_MS_INVALID;
	}
}

void Time::Get(int* hour, int* minute, int* second, int* msec) const {
	if (IsInvalid()) {
		if (hour != nullptr) {
			*hour = -1;
		}
		if (minute != nullptr) {
			*minute = -1;
		}
		if (second != nullptr) {
			*second = -1;
		}
		if (msec != nullptr) {
			*msec = -1;
		}
	} else {
		MsToHms(hour, minute, second, msec, m_ms);
	}
}

bool Time::IsValid(int hour, int minute, int second, int msec) {
	return (hour >= 0 && hour <= 23) && (minute >= 0 && minute <= 59) &&
	       (second >= 0 && second <= 59) && (msec >= 0 && msec <= 999);
}

int Time::Hour24() const {
	if (IsInvalid()) {
		return -1;
	}

	int h = 0;
	MsToHms(&h, nullptr, nullptr, nullptr, m_ms);

	return h;
}

int Time::Hour12() const {
	if (IsInvalid()) {
		return -1;
	}

	int h = 0;
	MsToHms(&h, nullptr, nullptr, nullptr, m_ms);

	h = h % 12;

	return h == 0 ? 12 : h;
}

int Time::Minute() const {
	if (IsInvalid()) {
		return -1;
	}

	int m = 0;
	MsToHms(nullptr, &m, nullptr, nullptr, m_ms);

	return m;
}

int Time::Second() const {
	if (IsInvalid()) {
		return -1;
	}

	int s = 0;
	MsToHms(nullptr, nullptr, &s, nullptr, m_ms);

	return s;
}

int Time::Msec() const {
	if (IsInvalid()) {
		return -1;
	}

	int m = 0;
	MsToHms(nullptr, nullptr, nullptr, &m, m_ms);

	return m;
}

bool Time::IsAM() const {
	if (IsInvalid()) {
		return false;
	}

	int h = 0;
	MsToHms(&h, nullptr, nullptr, nullptr, m_ms);

	return h < 12;
}

bool Time::IsPM() const {
	if (IsInvalid()) {
		return false;
	}

	int h = 0;
	MsToHms(&h, nullptr, nullptr, nullptr, m_ms);

	return h >= 12;
}

// NOLINTNEXTLINE(readability-non-const-parameter)
static bool FormatTime(const Time* t, const std::string& f, size_t* out_i, std::string* out_r) {
	auto& i = *out_i;
	auto& r = *out_r;
	if (f.compare(i, 4, "HH24") == 0) {
		r += fmt::format("{:02d}", t->Hour24());
		i += 3;
	} else if (f.compare(i, 4, "HH12") == 0) {
		r += fmt::format("{:02d}", t->Hour12());
		i += 3;
	} else if (f.compare(i, 2, "HH") == 0) {
		r += fmt::format("{:02d}", t->Hour12());
		i += 1;
	} else if (f.compare(i, 2, "MI") == 0) {
		r += fmt::format("{:02d}", t->Minute());
		i += 1;
	} else if (f.compare(i, 5, "SSSSS") == 0) {
		r += fmt::format("{:05d}", t->MsecTotal() / 1000);
		i += 4;
	} else if (f.compare(i, 2, "SS") == 0) {
		r += fmt::format("{:02d}", t->Second());
		i += 1;
	} else if (f.compare(i, 3, "FFF") == 0) {
		r += fmt::format("{:03d}", t->Msec());
		i += 2;
	} else if (f.compare(i, 2, "AM") == 0) {
		r += t->IsAM() ? "AM" : "PM";
		i += 1;
	} else if (f.compare(i, 4, "A.M.") == 0) {
		r += t->IsAM() ? "A.M." : "P.M.";
		i += 3;
	} else {
		return false;
	}

	return true;
}

/**
 * HH    - Hour of day (1-12)
 * HH12  - Hour of day (1-12)
 * HH24  - Hour of day (0-23)
 * MI    - Minute (0-59)
 * SS    - Second (0-59)
 * SSSSS - Seconds past midnight (0-86399)
 * FFF   - Milliseconds
 * AM    - AM or PM
 * A.M.  - A.M. or P.M.
 */
std::string Time::ToString(const char* format) const {
	if (IsInvalid()) {
		return "";
	}

	std::string r;
	std::string f = format;

	for (size_t i = 0; i < f.size(); i++) {
		if (!FormatTime(this, f, &i, &r)) {
			r += f[i];
		}
	}

	return r;
}

Time Time::operator+(int secs) const {
	int ms_secs = secs * 1000;

	EXIT_IF(ms_secs > TIME_MS_IN_DAY || ms_secs < -TIME_MS_IN_DAY);

	if (IsInvalid()) {
		return {};
	}

	int r = m_ms + ms_secs;

	if (r < 0) {
		r += TIME_MS_IN_DAY;
	}
	if (r >= TIME_MS_IN_DAY) {
		r -= TIME_MS_IN_DAY;
	}

	return Time(r);
}

Time Time::operator-(int secs) const {
	int ms_secs = secs * 1000;

	EXIT_IF(ms_secs > TIME_MS_IN_DAY || ms_secs < -TIME_MS_IN_DAY);

	if (IsInvalid()) {
		return {};
	}

	int r = m_ms - ms_secs;

	if (r < 0) {
		r += TIME_MS_IN_DAY;
	}
	if (r >= TIME_MS_IN_DAY) {
		r -= TIME_MS_IN_DAY;
	}

	return Time(r);
}

Time Time::operator+=(int secs) {
	int ms_secs = secs * 1000;

	EXIT_IF(ms_secs > TIME_MS_IN_DAY || ms_secs < -TIME_MS_IN_DAY);

	if (!IsInvalid()) {
		m_ms += ms_secs;

		if (m_ms < 0) {
			m_ms += TIME_MS_IN_DAY;
		}
		if (m_ms >= TIME_MS_IN_DAY) {
			m_ms -= TIME_MS_IN_DAY;
		}
	}

	return *this;
}

Time Time::operator-=(int secs) {
	int ms_secs = secs * 1000;

	EXIT_IF(ms_secs > TIME_MS_IN_DAY || ms_secs < -TIME_MS_IN_DAY);

	if (!IsInvalid()) {
		m_ms -= ms_secs;

		if (m_ms < 0) {
			m_ms += TIME_MS_IN_DAY;
		}
		if (m_ms >= TIME_MS_IN_DAY) {
			m_ms -= TIME_MS_IN_DAY;
		}
	}

	return *this;
}

DateTime DateTime::FromSystem() {
	SysTimeStruct t {};
	SysGetSystemTime(t);

	if (t.is_invalid) {
		return {};
	}

	return DateTime(Date(t.Year, t.Month, t.Day), Time(t.Hour, t.Minute, t.Second, t.Milliseconds));
}

DateTime DateTime::FromSystemUTC() {
	SysTimeStruct t {};
	SysGetSystemTimeUtc(t);

	if (t.is_invalid) {
		return {};
	}

	return DateTime(Date(t.Year, t.Month, t.Day), Time(t.Hour, t.Minute, t.Second, t.Milliseconds));
}

/**
 * YYYY       - 4-digit year
 * YYY, YY, Y - Last 3, 2, or 1 digit(s) of year.
 * Q          - Quarter of year (1, 2, 3, 4; JAN-MAR = 1).
 * MM         - Month (01-12; JAN = 01).
 * MON        - Abbreviated name of month.
 * MONTH      - Name of month
 * D          - Day of week (1-7).
 * DAY        - Name of day.
 * DY         - Abbreviated name of day.
 * DD         - Day of month (01-31).
 * DDD        - Day of year (001-366).
 * J          - Julian day
 * HH         - Hour of day (1-12)
 * HH12       - Hour of day (1-12)
 * HH24       - Hour of day (0-23)
 * MI         - Minute (0-59)
 * SS         - Second (0-59)
 * SSSSS      - Seconds past midnight (0-86399)
 * FFF        - Milliseconds
 * AM         - AM or PM
 * A.M.       - A.M. or P.M.
 */
std::string DateTime::ToString(const char* format) const {
	if (IsInvalid()) {
		return "";
	}

	std::string r;
	std::string f = format;

	for (size_t i = 0; i < f.size(); i++) {
		if (!FormatDate(&m_date, f, &i, &r) && !FormatTime(&m_time, f, &i, &r)) {
			r += f[i];
		}
	}

	return r;
}

double DateTime::ToUnix() const {
	return (static_cast<double>(GetDate().JulianDay()) - 2440588.0) * 86400.0 +
	       static_cast<double>(GetTime().MsecTotal()) / 1000.0;
}

} // namespace Common
