#include "common/abi.h"
#include "common/logging/log.h"
#include "libs/errno.h"
#include "libs/libs.h"
#include "loader/symbolDatabase.h"

namespace Libs {

LIB_VERSION("DeviceService", 1, "Mbus", 1, 1);

namespace DeviceService {

static int KYTY_SYSV_ABI DeviceServiceInitialize() {
	PRINT_NAME();

	return OK;
}

static int KYTY_SYSV_ABI DeviceServiceQueryDeviceInfo(uint32_t device_handle, void* out_info) {
	PRINT_NAME();

	LOGF("\t device_handle = 0x%08" PRIx32 "\n"
	     "\t out_info      = 0x%016" PRIx64 "\n",
	     device_handle, reinterpret_cast<uint64_t>(out_info));

	return OK;
}

} // namespace DeviceService

LIB_DEFINE(InitDeviceService_1) {
	LIB_FUNC("84fDxStrG44", DeviceService::DeviceServiceInitialize);
	LIB_FUNC("UNMEa+5lrUA", DeviceService::DeviceServiceQueryDeviceInfo);
}

} // namespace Libs
