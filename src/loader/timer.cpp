#include "common/timer.h"

#include "loader/timer.h"

namespace Loader::Timer {

static Common::Timer g_timer;

void Start() {
	g_timer.Start();
}

double GetTimeMs() {
	return g_timer.GetTimeMs();
}

} // namespace Loader::Timer
