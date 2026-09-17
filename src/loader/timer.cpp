#include "common/timer.h"

#include "common/abi.h"
#include "common/dateTime.h"
#include "loader/timer.h"

namespace Loader::Timer {

static Common::Timer g_timer;

void Start() {
	g_timer.Start();
}

double GetTimeMs() {
	return g_timer.GetTimeMs();
}

Common::Time GetTime() {
	// Time is a time-of-day value capped at TIME_MS_IN_DAY; wrap the elapsed
	// milliseconds so log timestamps keep formatting after 24h of uptime
	// (and after the 32-bit cast would overflow, ~24.8 days).
	const auto ms = static_cast<uint64_t>(GetTimeMs());
	return Common::Time(static_cast<int>(ms % Common::TIME_MS_IN_DAY));
}

uint64_t GetCounter() {
	return g_timer.GetTicks();
}

uint64_t GetFrequency() {
	return g_timer.GetFrequency();
}

} // namespace Loader::Timer
