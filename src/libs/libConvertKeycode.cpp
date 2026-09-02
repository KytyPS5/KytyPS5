#include "common/abi.h"
#include "common/logging/log.h"
#include "libs/errno.h"
#include "libs/libs.h"
#include "loader/symbolDatabase.h"

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

} // namespace ConvertKeycode

LIB_DEFINE(InitConvertKeycode_1) {
	LIB_FUNC("mUuUOWI-C+0", ConvertKeycode::ConvertKeycodeGetImeKeyboardType);
}

} // namespace Libs
