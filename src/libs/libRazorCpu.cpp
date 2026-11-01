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

static int KYTY_SYSV_ABI RazorCpuNamedSync(const char*) {
	return 0;
}

// What it does: It is part of Razor CPU, the CPU profiler/analyzer in the Sony SDK. It is used to
// insert a marker into the code. Implemented with a stub that logs the call and returns OK.
static int KYTY_SYSV_ABI sceRazorCpuPushMarkerStatic(const char*) {
	LOGF("(Lib_RazorCpu): sceRazorCpuPushMarkerStatic() STUBBED called\n");
	return 0;
}

LIB_DEFINE(InitRazorCpu_1) {
	LIB_FUNC("EboejOQvLL4", LibRazorCpu::RazorCpuIsCapturing);
	LIB_FUNC("YpkGsMXP3ew", LibRazorCpu::RazorCpuPopMarker);
	LIB_FUNC("zw+celG7zSI", LibRazorCpu::RazorCpuPushMarker);
	LIB_FUNC("XCuZoBSVFG8", LibRazorCpu::RazorCpuNamedSync);
	LIB_FUNC("uZrOwuNJX-M", LibRazorCpu::sceRazorCpuPushMarkerStatic);
}

} // namespace LibRazorCpu

} // namespace Libs
