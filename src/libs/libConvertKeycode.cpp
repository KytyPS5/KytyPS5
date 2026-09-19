#include "common/abi.h"
#include "common/logging/log.h"
#include "libs/errno.h"
#include "libs/libs.h"
#include "loader/symbolDatabase.h"

#include <atomic>
#include <cinttypes>

namespace Libs {

LIB_VERSION("ConvertKeycode", 1, "ConvertKeycode", 1, 0);

namespace ConvertKeycode {

constexpr int CONVERT_KEYCODE_ERROR_INVALID_ADDRESS = -2135162831; // 0x80bc0031
constexpr int CONVERT_KEYCODE_ERROR_INVALID_USER_ID = -2135162864; // 0x80bc0010

static int KYTY_SYSV_ABI ConvertKeycodeGetImeKeyboardType(int32_t user_id, uint32_t* type) {
	PRINT_NAME();

	LOGF("\t user_id = %d\n", user_id);

	if (type == nullptr) {
		return CONVERT_KEYCODE_ERROR_INVALID_ADDRESS;
	}
	if (user_id < 0) {
		return CONVERT_KEYCODE_ERROR_INVALID_USER_ID;
	}
	*type = 0;
	return OK;
}

// Real name and signature unverified. This returns what the unresolved-import stub already
// returned, so behaviour is unchanged; the first calls' arguments are logged to identify it later.
static int KYTY_SYSV_ABI ConvertKeycodeUnknownQjGCaJbRib4(uint64_t arg0, uint64_t arg1,
                                                          uint64_t arg2, uint64_t arg3,
                                                          uint64_t arg4, uint64_t arg5) {
	PRINT_NAME();

	static std::atomic_uint32_t logged {0};
	if (logged.fetch_add(1) < 4) {
		LOGF("\t args = (0x%016" PRIx64 ", 0x%016" PRIx64 ", 0x%016" PRIx64 ", 0x%016" PRIx64
		     ", 0x%016" PRIx64 ", 0x%016" PRIx64 ")\n",
		     arg0, arg1, arg2, arg3, arg4, arg5);
	}

	return OK;
}

} // namespace ConvertKeycode

LIB_DEFINE(InitConvertKeycode_1) {
	LIB_FUNC("mUuUOWI-C+0", ConvertKeycode::ConvertKeycodeGetImeKeyboardType);
	LIB_FUNC("QjGCaJbRib4", ConvertKeycode::ConvertKeycodeUnknownQjGCaJbRib4);
}

} // namespace Libs
