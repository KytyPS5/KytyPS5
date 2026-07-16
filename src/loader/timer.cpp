#include "common/timer.h"

#include "common/abi.h"
#include "common/dateTime.h"
#include "common/subsystems.h"
#include "loader/timer.h"

namespace Loader::Timer {

static Common::Timer g_timer;

KYTY_SUBSYSTEM_INIT(Timer) {
	Start();
}

KYTY_SUBSYSTEM_UNEXPECTED_SHUTDOWN(Timer) {}

KYTY_SUBSYSTEM_DESTROY(Timer) {}

void Start() {
	g_timer.Start();
}

double GetTimeMs() {
	return g_timer.GetTimeMs();
}

Common::Time GetTime() {
	return Common::Time(static_cast<int>(GetTimeMs()));
}

uint64_t GetCounter() {
	return g_timer.GetTicks();
}

uint64_t GetFrequency() {
	return g_timer.GetFrequency();
}

} // namespace Loader::Timer
