#include <cstring>

#include "common/abi.h"
#include "common/logging/log.h"
#include "libs/errno.h"
#include "libs/libs.h"
#include "loader/symbolDatabase.h"

namespace Libs {

// Minimal libSceMbus device-service surface. GranTurismo (PPSA24156) calls
// sceDeviceServiceQueryDeviceInfo_ once during its wheel/controller device scan.
// That NID previously fell through the generic unresolved-import stub
// (runtimeLinker.cpp), which returns OK without writing to any output argument.
// The caller then reads its output struct as if it had been filled in ÔÇö and on
// this title that garbage stack/heap content gets used as a device descriptor
// (very likely containing a function pointer / vtable-style field), which is a
// confirmed source of wild-jump crashes on independent guest worker threads
// (Bluetooth wheel main thread, FFB device-event thread) shortly afterward.
//
// The real struct layout isn't confirmed locally (no SDK header available
// offline) ÔÇö this is a defensive fix, not a real implementation: zero the
// output buffer so any embedded pointer/handle reads as null instead of
// garbage, rather than guessing field offsets.
LIB_VERSION("DeviceService", 1, "Mbus", 1, 1);

namespace DeviceService {

constexpr size_t DEVICE_INFO_ZERO_SIZE = 64;

static int KYTY_SYSV_ABI DeviceServiceInitialize() {
	PRINT_NAME();

	return OK;
}

static int KYTY_SYSV_ABI DeviceServiceQueryDeviceInfo(uint32_t device_handle, void* out_info) {
	PRINT_NAME();

	LOGF("\t device_handle = 0x%08" PRIx32 "\n"
	     "\t out_info      = 0x%016" PRIx64 "\n",
	     device_handle, reinterpret_cast<uint64_t>(out_info));

	if (out_info != nullptr) {
		std::memset(out_info, 0, DEVICE_INFO_ZERO_SIZE);
	}

	return OK;
}

} // namespace DeviceService

LIB_DEFINE(InitDeviceService_1) {
	LIB_FUNC("84fDxStrG44", DeviceService::DeviceServiceInitialize);
	LIB_FUNC("UNMEa+5lrUA", DeviceService::DeviceServiceQueryDeviceInfo);
}

} // namespace Libs
