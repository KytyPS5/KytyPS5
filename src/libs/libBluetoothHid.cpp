#include "common/abi.h"
#include "common/logging/log.h"
#include "libs/errno.h"
#include "libs/libs.h"
#include "loader/symbolDatabase.h"

namespace Libs {

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

// Callback signature is unverified; the stub never invokes it.
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
