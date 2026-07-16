#include "common/abi.h"
#include "common/common.h"
#include "common/stringUtils.h"
#include "libs/libs.h"
#include "loader/symbolDatabase.h"

namespace Libs {

namespace LibRazorCpu {

LIB_VERSION("RazorCpu", 1, "RazorCpu", 1, 1);

static KYTY_SYSV_ABI uint32_t RazorCpuIsCapturing() {
	PRINT_NAME();

	return 0;
}

LIB_DEFINE(InitLibRazorCpu_1) {
	LIB_FUNC("EboejOQvLL4", LibRazorCpu::RazorCpuIsCapturing);
}

} // namespace LibRazorCpu

LIB_DEFINE(InitDebug_1) {
	LibRazorCpu::InitLibRazorCpu_1(s);
}

} // namespace Libs
