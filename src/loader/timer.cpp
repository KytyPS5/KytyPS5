#include "common/timer.h"

#include "loader/timer.h"

namespace Loader::Timer {

static Common::Timer g_timer;

double GetTimeMs() {
	return g_timer.GetTimeS() * 1000.0;
}

} // namespace Loader::Timer
