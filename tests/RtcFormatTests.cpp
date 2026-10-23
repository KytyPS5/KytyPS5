#include "libs/libRtc.cpp"

#include <array>
#include <cstring>
#include <map>
#include <string>

namespace {

std::map<std::string, uint64_t>* g_rtc_symbols = nullptr;

} // namespace

namespace Loader {
void SymbolDatabase::Add(const SymbolResolve& s, uint64_t vaddr, const std::string&) {
	if (g_rtc_symbols != nullptr) {
		(*g_rtc_symbols)[s.name] = vaddr;
	}
}
namespace Timer {
double GetTimeMs() {
	return 0.0;
}
} // namespace Timer
} // namespace Loader

namespace {

using namespace Libs::LibRtc;

int failures = 0;

#define CHECK(condition)                                                                           \
	do {                                                                                           \
		if (!(condition)) {                                                                        \
			std::fprintf(stderr, "RtcFormatTests:%d: %s\n", __LINE__, #condition);                 \
			++failures;                                                                            \
		}                                                                                          \
	} while (false)

using FormatFunc      = int(KYTY_SYSV_ABI*)(char*, const Rtc::RtcTick*, int);
using LocalFormatFunc = int(KYTY_SYSV_ABI*)(char*, const Rtc::RtcTick*);

// 2026-10-07 21:43:05.123456 UTC, a Wednesday.
constexpr uint64_t TEST_TICK = 63927006185000000ull + 123456ull;
// 2026-10-04 12:00:00 UTC, a Sunday.
constexpr uint64_t SUNDAY_TICK = 63926712000000000ull;
// 9999-12-31 23:59:59 UTC, the last second RtcSetTick accepts.
constexpr uint64_t LAST_TICK = 315537897599000000ull;

// The formatters write at most 32 bytes; the rest of the buffer must stay untouched.
using Buffer = std::array<char, 48>;

Buffer MakeBuffer() {
	Buffer buf {};
	buf.fill('\x7f');
	return buf;
}

bool Untouched(const Buffer& buf, size_t from) {
	for (size_t i = from; i < buf.size(); i++) {
		if (buf[i] != '\x7f') {
			return false;
		}
	}
	return true;
}

void CheckFormat(FormatFunc func, uint64_t tick, int tz, const char* expected) {
	auto         buf = MakeBuffer();
	Rtc::RtcTick utc {tick};
	CHECK(func(buf.data(), &utc, tz) == OK);
	CHECK(std::strcmp(buf.data(), expected) == 0);
	if (std::strcmp(buf.data(), expected) != 0) {
		std::fprintf(stderr, "  got \"%s\", expected \"%s\"\n", buf.data(), expected);
	}
	CHECK(Untouched(buf, 32));
}

void TestRFC2822(FormatFunc format, LocalFormatFunc local) {
	CheckFormat(format, TEST_TICK, 0, "Wed, 07 Oct 2026 21:43:05 +0000");
	CheckFormat(format, TEST_TICK, -240, "Wed, 07 Oct 2026 17:43:05 -0400");
	CheckFormat(format, TEST_TICK, 330, "Thu, 08 Oct 2026 03:13:05 +0530");
	CheckFormat(format, TEST_TICK, -570, "Wed, 07 Oct 2026 12:13:05 -0930");
	CheckFormat(format, TEST_TICK, -1, "Wed, 07 Oct 2026 21:42:05 -0001");
	CheckFormat(format, SUNDAY_TICK, 0, "Sun, 04 Oct 2026 12:00:00 +0000");
	CheckFormat(format, 0, 0, "Mon, 01 Jan 0001 00:00:00 +0000");
	CheckFormat(format, LAST_TICK, 0, "Fri, 31 Dec 9999 23:59:59 +0000");

	auto         buf = MakeBuffer();
	Rtc::RtcTick utc {TEST_TICK};
	CHECK(format(nullptr, &utc, 0) == Rtc::RTC_ERROR_INVALID_POINTER);
	CHECK(local(nullptr, &utc) == Rtc::RTC_ERROR_INVALID_POINTER);

	// Offsets that move the tick outside 0001-01-01 .. 9999-12-31 are rejected.
	Rtc::RtcTick first {0};
	Rtc::RtcTick last {LAST_TICK};
	CHECK(format(buf.data(), &first, -1) == Rtc::RTC_ERROR_INVALID_VALUE);
	CHECK(format(buf.data(), &last, 1) == Rtc::RTC_ERROR_INVALID_VALUE);
	Rtc::RtcTick wraps {~0ull};
	CHECK(format(buf.data(), &wraps, 1) == Rtc::RTC_ERROR_INVALID_VALUE);
	CHECK(Untouched(buf, 0));

	// "+9959" is the longest offset that fits; "+10000" would need 32 characters, so it is
	// rejected instead of truncated, and nothing is written.
	CheckFormat(format, TEST_TICK, 5999, "Mon, 12 Oct 2026 01:42:05 +9959");
	CheckFormat(format, TEST_TICK, -5999, "Sat, 03 Oct 2026 17:44:05 -9959");
	CHECK(format(buf.data(), &utc, 6000) == Rtc::RTC_ERROR_INVALID_VALUE);
	CHECK(format(buf.data(), &utc, -6000) == Rtc::RTC_ERROR_INVALID_VALUE);
	CHECK(Untouched(buf, 0));

	// Kyty keeps RTC local time equal to UTC.
	CHECK(local(buf.data(), &utc) == OK);
	CHECK(std::strcmp(buf.data(), "Wed, 07 Oct 2026 21:43:05 +0000") == 0);

	// A null tick formats the current time.
	buf = MakeBuffer();
	CHECK(format(buf.data(), nullptr, 0) == OK);
	CHECK(std::strlen(buf.data()) == 31);
	CHECK(buf[3] == ',' && buf[28] == '0' && std::strcmp(buf.data() + 26, "+0000") == 0);
	buf = MakeBuffer();
	CHECK(local(buf.data(), nullptr) == OK);
	CHECK(std::strlen(buf.data()) == 31);
}

void TestRFC3339(FormatFunc format, LocalFormatFunc local) {
	CheckFormat(format, TEST_TICK, 0, "2026-10-07T21:43:05.12Z");
	CheckFormat(format, TEST_TICK, -240, "2026-10-07T17:43:05.12-04:00");
	CheckFormat(format, TEST_TICK, 345, "2026-10-08T03:28:05.12+05:45");

	auto         buf = MakeBuffer();
	Rtc::RtcTick utc {TEST_TICK};
	CHECK(local(buf.data(), &utc) == OK);
	CHECK(std::strcmp(buf.data(), "2026-10-07T21:43:05.12Z") == 0);
	CHECK(Untouched(buf, 32));
	CHECK(local(nullptr, &utc) == Rtc::RTC_ERROR_INVALID_POINTER);

	// RFC 3339 offset hours stop at 23, so 24 hours or more is rejected.
	CheckFormat(format, TEST_TICK, 1439, "2026-10-08T21:42:05.12+23:59");
	CheckFormat(format, TEST_TICK, -1439, "2026-10-06T21:44:05.12-23:59");
	buf = MakeBuffer();
	CHECK(format(buf.data(), &utc, 1440) == Rtc::RTC_ERROR_INVALID_VALUE);
	CHECK(format(buf.data(), &utc, -1440) == Rtc::RTC_ERROR_INVALID_VALUE);
	CHECK(format(buf.data(), &utc, 60000) == Rtc::RTC_ERROR_INVALID_VALUE);
	CHECK(Untouched(buf, 0));
}

} // namespace

int main() {
	std::map<std::string, uint64_t> symbols;
	g_rtc_symbols = &symbols;
	Loader::SymbolDatabase db;
	InitRtc_1(&db);
	g_rtc_symbols = nullptr;

	const auto find = [&](const char* nid) {
		auto it = symbols.find(nid);
		CHECK(it != symbols.end());
		return it == symbols.end() ? 0 : it->second;
	};

	// sceRtcFormatRFC2822, sceRtcFormatRFC2822LocalTime, sceRtcFormatRFC3339 and
	// sceRtcFormatRFC3339LocalTime.
	const auto rfc2822       = reinterpret_cast<FormatFunc>(find("eiuobaF-hK4"));
	const auto rfc2822_local = reinterpret_cast<LocalFormatFunc>(find("AxHBk3eat04"));
	const auto rfc3339       = reinterpret_cast<FormatFunc>(find("WJ3rqFwymew"));
	const auto rfc3339_local = reinterpret_cast<LocalFormatFunc>(find("DwuHIlLGW8I"));

	if (rfc2822 != nullptr && rfc2822_local != nullptr) {
		TestRFC2822(rfc2822, rfc2822_local);
	}
	if (rfc3339 != nullptr && rfc3339_local != nullptr) {
		TestRFC3339(rfc3339, rfc3339_local);
	}

	if (failures == 0) {
		std::printf("RtcFormatTests: all checks passed\n");
	}
	return failures == 0 ? 0 : 1;
}
