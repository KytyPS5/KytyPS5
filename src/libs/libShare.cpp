#include "common/abi.h"
#include "common/logging/log.h"
#include "common/stringUtils.h"
#include "libs/errno.h"
#include "libs/libs.h"
#include "loader/symbolDatabase.h"

#include <cinttypes>
#include <cstring>

namespace Libs {

LIB_VERSION("Share", 1, "Share", 1, 1);

namespace Share {

constexpr int SHARE_ERROR_INVALID_PARAM = -2120876030; /* 0x81960002 */
constexpr int SHARE_ERROR_NOT_SUPPORTED = -2120876025; /* 0x81960007 */
constexpr int SHARE_REQUEST_ID_INVALID  = -1;

struct ShareCurrentRecordingStatus {
	uint32_t recording_2k_status;
	uint32_t recording_4k_status;
	uint8_t  reserved[8];
};

union ShareCurrentStatus {
	ShareCurrentRecordingStatus recording_status;
	uint8_t                     reserved[16];
};

static bool share_feature_flag_valid(uint32_t feature_flags) {
	constexpr uint32_t share_feature_flag_all = 0xffffffffu;
	return feature_flags != 0 && (feature_flags & share_feature_flag_all) == feature_flags;
}

static KYTY_SYSV_ABI int ShareCaptureScreenshot(const void* param, int32_t* req_id) {
	PRINT_NAME();

	LOGF("\t param  = 0x%016" PRIx64 "\n"
	     "\t req_id = 0x%016" PRIx64 "\n",
	     reinterpret_cast<uint64_t>(param), reinterpret_cast<uint64_t>(req_id));

	if (req_id != nullptr) {
		*req_id = SHARE_REQUEST_ID_INVALID;
	}

	return SHARE_ERROR_NOT_SUPPORTED;
}

static KYTY_SYSV_ABI int ShareCaptureScreenshotExtended(const void* extended_param,
                                                        int32_t*    req_id) {
	PRINT_NAME();

	LOGF("\t extended_param = 0x%016" PRIx64 "\n"
	     "\t req_id         = 0x%016" PRIx64 "\n",
	     reinterpret_cast<uint64_t>(extended_param), reinterpret_cast<uint64_t>(req_id));

	if (req_id != nullptr) {
		*req_id = SHARE_REQUEST_ID_INVALID;
	}

	return SHARE_ERROR_NOT_SUPPORTED;
}

static KYTY_SYSV_ABI int ShareCaptureVideoClip(const void* param, int32_t* req_id) {
	PRINT_NAME();

	LOGF("\t param  = 0x%016" PRIx64 "\n"
	     "\t req_id = 0x%016" PRIx64 "\n",
	     reinterpret_cast<uint64_t>(param), reinterpret_cast<uint64_t>(req_id));

	if (req_id != nullptr) {
		*req_id = SHARE_REQUEST_ID_INVALID;
	}

	return SHARE_ERROR_NOT_SUPPORTED;
}

static KYTY_SYSV_ABI int ShareCaptureVideoClipExtended(const void* extended_param,
                                                       int32_t*    req_id) {
	PRINT_NAME();

	LOGF("\t extended_param = 0x%016" PRIx64 "\n"
	     "\t req_id         = 0x%016" PRIx64 "\n",
	     reinterpret_cast<uint64_t>(extended_param), reinterpret_cast<uint64_t>(req_id));

	if (req_id != nullptr) {
		*req_id = SHARE_REQUEST_ID_INVALID;
	}

	return SHARE_ERROR_NOT_SUPPORTED;
}

static KYTY_SYSV_ABI int ShareUnregisterContentEventCallback(void* callback) {
	PRINT_NAME();

	LOGF("\t callback = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(callback));

	return OK;
}

static KYTY_SYSV_ABI int ShareOpenMenuForContent(const void* content_id) {
	PRINT_NAME();

	LOGF("\t content_id = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(content_id));

	return SHARE_ERROR_NOT_SUPPORTED;
}

static KYTY_SYSV_ABI int ShareSetScreenshotOverlayImage(const char* file_path, int32_t margin_x,
                                                        int32_t margin_y, int32_t origin) {
	PRINT_NAME();

	LOGF("\t file_path = %s\n"
	     "\t margin_x  = %d\n"
	     "\t margin_y  = %d\n"
	     "\t origin    = %d\n",
	     (file_path != nullptr ? file_path : "<null>"), margin_x, margin_y, origin);

	return OK;
}

static KYTY_SYSV_ABI int ShareSetCaptureSource(uint32_t feature_flags, const void* tap_point) {
	PRINT_NAME();

	LOGF("\t feature_flags = 0x%08" PRIx32 "\n"
	     "\t tap_point     = 0x%016" PRIx64 "\n",
	     feature_flags, reinterpret_cast<uint64_t>(tap_point));

	if (!share_feature_flag_valid(feature_flags)) {
		return SHARE_ERROR_INVALID_PARAM;
	}

	return OK;
}

static KYTY_SYSV_ABI int ShareSetContentParam(const char* content_param) {
	PRINT_NAME();

	LOGF("\t content_param = %s\n", (content_param != nullptr ? content_param : "<null>"));

	if (content_param == nullptr) {
		return SHARE_ERROR_INVALID_PARAM;
	}

	return OK;
}

static KYTY_SYSV_ABI int ShareSetContentParamForApplicationTitle(const char* application_title) {
	PRINT_NAME();

	LOGF("\t application_title = %s\n",
	     (application_title != nullptr ? application_title : "<null>"));

	if (application_title == nullptr) {
		return SHARE_ERROR_INVALID_PARAM;
	}

	return OK;
}

static KYTY_SYSV_ABI int ShareRegisterContentEventCallback(void* callback, void* user_data) {
	PRINT_NAME();

	LOGF("\t callback  = 0x%016" PRIx64 "\n"
	     "\t user_data = 0x%016" PRIx64 "\n",
	     reinterpret_cast<uint64_t>(callback), reinterpret_cast<uint64_t>(user_data));

	return OK;
}

static KYTY_SYSV_ABI int ShareGetCurrentStatus(uint32_t feature_flag, ShareCurrentStatus* status) {
	PRINT_NAME();

	LOGF("\t feature_flag = 0x%08" PRIx32 "\n"
	     "\t status       = 0x%016" PRIx64 "\n",
	     feature_flag, reinterpret_cast<uint64_t>(status));

	if (!share_feature_flag_valid(feature_flag) || status == nullptr) {
		return SHARE_ERROR_INVALID_PARAM;
	}

	std::memset(status, 0, sizeof(*status));
	return OK;
}

static KYTY_SYSV_ABI int ShareGetRunningStatus(uint32_t* feature_flags) {
	PRINT_NAME();

	LOGF("\t feature_flags = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(feature_flags));

	if (feature_flags == nullptr) {
		return SHARE_ERROR_INVALID_PARAM;
	}

	*feature_flags = 0;
	return OK;
}

static KYTY_SYSV_ABI int ShareFeaturePermit(uint32_t feature_flags) {
	PRINT_NAME();

	LOGF("\t feature_flags = 0x%08" PRIx32 "\n", feature_flags);

	if (!share_feature_flag_valid(feature_flags)) {
		return SHARE_ERROR_INVALID_PARAM;
	}

	return OK;
}

static KYTY_SYSV_ABI int ShareFeatureProhibit(uint32_t feature_flags) {
	PRINT_NAME();

	LOGF("\t feature_flags = 0x%08" PRIx32 "\n", feature_flags);

	if (!share_feature_flag_valid(feature_flags)) {
		return SHARE_ERROR_INVALID_PARAM;
	}

	return OK;
}

} // namespace Share

LIB_DEFINE(InitShare_1) {
	LIB_FUNC("4jt8pMDudgk", Share::ShareCaptureVideoClip);
	LIB_FUNC("5wjxESwX68I", Share::ShareFeatureProhibit);
	LIB_FUNC("7QZtURYnXG4", Share::ShareSetContentParam);
	LIB_FUNC("8qAJ0Jd58-Q", Share::ShareOpenMenuForContent);
	LIB_FUNC("AcDNpEpoT9U", Share::ShareCaptureVideoClipExtended);
	LIB_FUNC("ErH6tKS7fzE", Share::ShareCaptureScreenshot);
	LIB_FUNC("GQTObcITIXI", Share::ShareCaptureScreenshotExtended);
	LIB_FUNC("KnsfHKmZqFA", Share::ShareUnregisterContentEventCallback);
	LIB_FUNC("ORspsWDXPps", Share::ShareSetContentParamForApplicationTitle);
	LIB_FUNC("QNop2YAtIDE", Share::ShareGetCurrentStatus);
	LIB_FUNC("Sygnk9dr5WQ", Share::ShareRegisterContentEventCallback);
	LIB_FUNC("T64o-315wbg", Share::ShareSetScreenshotOverlayImage);
	LIB_FUNC("YBiIdcDPrxs", Share::ShareFeaturePermit);
	LIB_FUNC("crFxyW3HdK0", Share::ShareGetRunningStatus);
	LIB_FUNC("kCurUZVFqcI", Share::ShareSetCaptureSource);
}

} // namespace Libs
