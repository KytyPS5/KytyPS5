#include "common/abi.h"
#include "common/common.h"
#include "common/stringUtils.h"
#include "libs/errno.h"
#include "libs/libs.h"
#include "loader/symbolDatabase.h"

#include <atomic>
#include <cinttypes>

namespace Libs {

namespace LibRazorCpu {

LIB_VERSION("RazorCpu", 1, "RazorCpu", 1, 1);

static KYTY_SYSV_ABI uint32_t RazorCpuIsCapturing() {
	PRINT_NAME();

	return 0;
}

// Razor CPU is profiling instrumentation, so recording nothing is the whole behaviour. RE9 calls
// this once per service tick; the first calls' arguments are logged to identify it later.
static KYTY_SYSV_ABI int RazorCpuUnknownAx7NjOzctIM(uint64_t arg0, uint64_t arg1, uint64_t arg2,
                                                    uint64_t arg3, uint64_t arg4, uint64_t arg5) {
	PRINT_NAME();

	static std::atomic_uint32_t logged {0};
	if (logged.fetch_add(1) < 4) {
		LOGF("\t args = (0x%016" PRIx64 ", 0x%016" PRIx64 ", 0x%016" PRIx64 ", 0x%016" PRIx64
		     ", 0x%016" PRIx64 ", 0x%016" PRIx64 ")\n",
		     arg0, arg1, arg2, arg3, arg4, arg5);
	}

	return OK;
}

LIB_DEFINE(InitRazorCpu_1) {
	LIB_FUNC("EboejOQvLL4", LibRazorCpu::RazorCpuIsCapturing);
	LIB_FUNC("Ax7NjOzctIM", LibRazorCpu::RazorCpuUnknownAx7NjOzctIM);
}

} // namespace LibRazorCpu

} // namespace Libs
