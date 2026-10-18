#include "common/abi.h"
#include "common/threads.h"
#include "libs/errno.h"
#include "libs/libs.h"
#include "loader/symbolDatabase.h"

namespace Libs {

LIB_VERSION("Usbd", 1, "Usbd", 1, 1);

namespace Usbd {

constexpr uint32_t POLL_INTERVAL_MS = 16;

static int KYTY_SYSV_ABI UsbdInit() {
	PRINT_NAME();

	return OK;
}

static int KYTY_SYSV_ABI UsbdExit() {
	PRINT_NAME();

	return OK;
}

static int KYTY_SYSV_ABI UsbdHandleEventsTimeout() {
	PRINT_NAME();

	// Avoid a busy loop while USB events are unimplemented.
	Common::Thread::SleepMicro(POLL_INTERVAL_MS * 1000);

	return OK;
}

} // namespace Usbd

LIB_DEFINE(InitUsbd_1) {
	LIB_FUNC("TOhg7P6kTH4", Usbd::UsbdInit);
	LIB_FUNC("Fq6+0Fm55xU", Usbd::UsbdExit);
	LIB_FUNC("+wU6CGuZcWk", Usbd::UsbdHandleEventsTimeout);
}

} // namespace Libs
