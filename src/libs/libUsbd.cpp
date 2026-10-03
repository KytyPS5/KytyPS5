#include "common/abi.h"
#include "common/threads.h"
#include "libs/errno.h"
#include "libs/libs.h"
#include "loader/symbolDatabase.h"

namespace Libs {

// Minimal libSceUsbd: no real USB devices, so there is never anything to enumerate. My First Gran Turismo
// (PPSA24156) still spins up a worker thread that calls sceUsbdInit() once, then loops calling
// sceUsbdHandleEventsTimeout() (the libusb-style "pump events, block up to a timeout" call) as
// fast as the CPU allows, because the generic unresolved-import stub it used to fall through to
// (runtimeLinker.cpp) returns instantly instead of ever blocking. That turns what should be an
// occasional poll into an unthrottled busy-loop on one CPU core for as long as the title runs,
// which starves other guest threads and time-dependent init logic elsewhere in the title (seen
// as either an indefinite hang or the title's own watchdog giving up and exiting early).
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
