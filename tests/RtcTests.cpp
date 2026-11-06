#include "common/abi.h"
#include "loader/symbolDatabase.h"

#include <cstdint>
#include <ctime>
#include <gtest/gtest.h>

namespace Libs::LibRtc {
void InitRtc_1(Loader::SymbolDatabase* symbols);
} // namespace Libs::LibRtc

namespace {

struct RtcDateTime {
	uint16_t year;
	uint16_t month;
	uint16_t day;
	uint16_t hour;
	uint16_t minute;
	uint16_t second;
	uint32_t microsecond;
};

constexpr int RTC_ERROR_INVALID_DAY = -2135621622;

class Rtc: public testing::Test {
protected:
	void SetUp() override { Libs::LibRtc::InitRtc_1(&m_symbols); }

	template <typename F>
	F Find(const char* nid) {
		const auto* symbol = m_symbols.FindByNid(nid, Loader::SymbolType::Func);
		EXPECT_NE(symbol, nullptr);
		return reinterpret_cast<F>(symbol->vaddr);
	}

private:
	Loader::SymbolDatabase m_symbols;
};

TEST_F(Rtc, CalendarQueriesFollowTheGregorianCalendar) {
	const auto check_valid  = Find<int(KYTY_SYSV_ABI*)(const RtcDateTime*)>("lPEBYdVX0XQ");
	const auto day_of_week  = Find<int(KYTY_SYSV_ABI*)(int, int, int)>("CyIK-i4XdgQ");
	const auto is_leap_year = Find<int(KYTY_SYSV_ABI*)(int)>("Ug8pCwQvh0c");
	const auto days_in      = Find<int(KYTY_SYSV_ABI*)(int, int)>("3O7Ln8AqJ1o");

	EXPECT_EQ(is_leap_year(2024), 1);
	EXPECT_EQ(is_leap_year(1900), 0);
	EXPECT_EQ(is_leap_year(2000), 1);
	EXPECT_EQ(days_in(2024, 2), 29);
	EXPECT_EQ(days_in(2023, 2), 28);
	EXPECT_EQ(days_in(2023, 4), 30);
	EXPECT_EQ(days_in(2023, 12), 31);
	// Sunday is 0.
	EXPECT_EQ(day_of_week(2024, 3, 10), 0);
	EXPECT_EQ(day_of_week(1, 1, 1), 1);

	RtcDateTime time {2024, 2, 29, 23, 59, 59, 999999};
	EXPECT_EQ(check_valid(&time), 0);
	time.year = 2023;
	EXPECT_EQ(check_valid(&time), RTC_ERROR_INVALID_DAY);
	time = {2023, 4, 31, 0, 0, 0, 0};
	EXPECT_EQ(check_valid(&time), RTC_ERROR_INVALID_DAY);
	time.day = 0;
	EXPECT_EQ(check_valid(&time), RTC_ERROR_INVALID_DAY);
	time.day = 257;
	EXPECT_EQ(check_valid(&time), RTC_ERROR_INVALID_DAY);
}

std::tm LocalTime(std::time_t seconds) {
	std::tm local {};
#ifdef _WIN32
	_localtime64_s(&local, &seconds);
#else
	localtime_r(&seconds, &local);
#endif
	return local;
}

bool SameMinute(const RtcDateTime& time, const std::tm& local) {
	return time.year == local.tm_year + 1900 && time.month == local.tm_mon + 1 &&
	       time.day == local.tm_mday && time.hour == local.tm_hour && time.minute == local.tm_min;
}

TEST_F(Rtc, CurrentLocalClockMatchesTheHostLocalTime) {
	const auto local_clock = Find<int(KYTY_SYSV_ABI*)(RtcDateTime*)>("ZPD1YOKI+Kw");

	RtcDateTime time {};
	const auto  before = std::time(nullptr);
	ASSERT_EQ(local_clock(&time), 0);
	const auto after = std::time(nullptr);
	EXPECT_TRUE(SameMinute(time, LocalTime(before)) || SameMinute(time, LocalTime(after)));
}

} // namespace

int main(int argc, char** argv) {
	testing::InitGoogleTest(&argc, argv);
	return RUN_ALL_TESTS();
}
