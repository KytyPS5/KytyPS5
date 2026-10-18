#include "common/abi.h"
#include "libs/libs.h"
#include "loader/symbolDatabase.h"

#include <cstdint>

namespace Libs {

namespace LibRazorCpu {

LIB_VERSION("RazorCpu", 1, "RazorCpu", 1, 1);

static KYTY_SYSV_ABI uint32_t RazorCpuIsCapturing() {
	PRINT_NAME();

	return 0;
}

static int KYTY_SYSV_ABI RazorCpuPopMarker() {
	return 0;
}

static int KYTY_SYSV_ABI RazorCpuPushMarker(const char*, uint32_t, uint32_t) {
	return 0;
}

LIB_DEFINE(InitRazorCpu_1) {
	LIB_FUNC("EboejOQvLL4", LibRazorCpu::RazorCpuIsCapturing);
	LIB_FUNC("YpkGsMXP3ew", LibRazorCpu::RazorCpuPopMarker);
	LIB_FUNC("zw+celG7zSI", LibRazorCpu::RazorCpuPushMarker);
}

} // namespace LibRazorCpu

} // namespace Libs
