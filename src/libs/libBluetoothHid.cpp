#include "common/abi.h"
#include "common/logging/log.h"
#include "libs/errno.h"
#include "libs/libs.h"
#include "loader/symbolDatabase.h"

namespace Libs {

// Minimal libSceBluetoothHid: reports "no Bluetooth HID devices", never actually
// pairs anything. Previously these NIDs fell through the generic unresolved-import
// stub (runtimeLinker.cpp), which returns OK without touching any output argument.
// GranTurismo (PPSA24156) calls RegisterCallback with a guest callback pointer in
// rdi expecting it to be retained, then background wheel/FFB threads act on
// whatever the (untouched, garbage) state looks like ÔÇö a confirmed source of
// wild-jump crashes on those threads. Explicit handlers here at least give a
// well-defined, zeroed response instead of leaving guest memory decisions.
LIB_VERSION("BluetoothHid", 1, "BluetoothHid", 1, 1);

namespace BluetoothHid {

static int KYTY_SYSV_ABI BluetoothHidInit() {
	PRINT_NAME();

	return OK;
}

static int KYTY_SYSV_ABI BluetoothHidRegisterDevice() {
	PRINT_NAME();

	return OK;
}

// Real signature isn't confirmed locally (no SDK header available offline); going
// by the calling convention other Sce *RegisterCallback style APIs use, the guest
// callback function pointer is the first argument (rdi). We deliberately do NOT
// store or ever invoke it: firing a synthetic guest callback from here is a much
// bigger behavioral change than this fix is meant to be. Returning OK without
// invoking anything is a strict improvement over the generic stub, which also
// never called it ÔÇö the point here is just to name/track this entry point
// explicitly instead of letting it fall into the generic unresolved-import path.
static int KYTY_SYSV_ABI BluetoothHidRegisterCallback(uint64_t callback_func) {
	PRINT_NAME();

	LOGF("\t callback_func = 0x%016" PRIx64 "\n", callback_func);

	return OK;
}

} // namespace BluetoothHid

LIB_DEFINE(InitBluetoothHid_1) {
	LIB_FUNC("tul3-GzejQc", BluetoothHid::BluetoothHidInit);
	LIB_FUNC("4FUZ+c52d2k", BluetoothHid::BluetoothHidRegisterDevice);
	LIB_FUNC("4Ypfo9RIwfM", BluetoothHid::BluetoothHidRegisterCallback);
}

} // namespace Libs
