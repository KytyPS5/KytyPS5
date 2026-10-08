#include "common/platform/sysTimer.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <limits>

namespace {

struct Case {
  int64_t seconds;
  uint16_t year, month, day, hour, minute, second;
};

/// Check Unix timestamps around the signed 32-bit boundary against UTC dates.
bool TestTimeTConversion() {
  static_assert(sizeof(time_t) >= sizeof(int64_t));
  constexpr std::array<Case, 6> cases{{
      {0, 1970, 1, 1, 0, 0, 0},
      {2147483647LL, 2038, 1, 19, 3, 14, 7},
      {2147483648LL, 2038, 1, 19, 3, 14, 8},
      {4102444800LL, 2100, 1, 1, 0, 0, 0},
      {-1, 1969, 12, 31, 23, 59, 59},
      {-2208988800LL, 1900, 1, 1, 0, 0, 0},
  }};
  bool passed = true;
  for (const auto& expected : cases) {
    SysTimeStruct actual{};
    SysTimeTToSystem(static_cast<time_t>(expected.seconds), actual);
    if (actual.is_invalid || actual.Year != expected.year ||
        actual.Month != expected.month || actual.Day != expected.day ||
        actual.Hour != expected.hour || actual.Minute != expected.minute ||
        actual.Second != expected.second || actual.Milliseconds != 0) {
      std::fprintf(stderr,
                   "SystemTimeTests: timestamp %lld produced %u-%02u-%02u %02u:%02u:%02u (invalid=%d)\n",
                   static_cast<long long>(expected.seconds), actual.Year, actual.Month,
                   actual.Day, actual.Hour, actual.Minute, actual.Second, actual.is_invalid);
      passed = false;
    }
  }
  return passed;
}

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
/// Reject seconds outside the signed FILETIME range before multiplying.
bool TestOutOfRangeWindowsTime() {
  for (const int64_t seconds : {std::numeric_limits<int64_t>::min(),
                                int64_t{-11644473601LL}, int64_t{910692730086LL},
                                std::numeric_limits<int64_t>::max()}) {
    SysTimeStruct result{};
    SysTimeTToSystem(static_cast<time_t>(seconds), result);
    if (!result.is_invalid) {
      std::fprintf(stderr, "SystemTimeTests: out-of-range timestamp accepted\n");
      return false;
    }
  }
  return true;
}
#endif

} // namespace

/// Run the UTC conversion regression without changing the host clock or timezone.
int main() {
  if (!TestTimeTConversion()) {
    return 1;
  }
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
  if (!TestOutOfRangeWindowsTime()) {
    return 1;
  }
#endif
  std::puts("SystemTimeTests: all cases passed");
  return 0;
}
