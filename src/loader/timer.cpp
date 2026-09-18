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
	return Common::Time(static_cast<int>(GetTimeMs()));
}

} // namespace Loader::Timer
