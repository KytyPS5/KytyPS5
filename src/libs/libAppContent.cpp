#include "common/abi.h"
#include "common/assert.h"
#include "common/common.h"
#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/stringUtils.h"
#include "libs/errno.h"
#include "libs/libs.h"
#include "loader/symbolDatabase.h"
#include "loader/systemContent.h"

#include <cstring>
#include <filesystem>
#include <system_error>

namespace Libs {

LIB_VERSION("AppContent", 1, "AppContentUtil", 1, 1);

namespace AppContent {

constexpr uint32_t APP_CONTENT_APPPARAM_SKU_FLAG_FULL   = 3;
constexpr int      APP_CONTENT_ERROR_PARAMETER          = -2133262334; /* 0x80D90002 */
constexpr int      APP_CONTENT_ERROR_NOT_MOUNTED        = -2133262332; /* 0x80D90004 */
constexpr int      APP_CONTENT_ERROR_DRM_NO_ENTITLEMENT = -2133262329; /* 0x80D90007 */

static std::string g_title_id;

struct AppContentInitParam {
	char reserved[32];
};

struct AppContentBootParam {
	char     reserved1[4];
	uint32_t attr;
	char     reserved2[32];
};

struct AppContentMountPoint {
	char data[16];
};

struct NpUnifiedEntitlementLabel {
	char data[17];
	char padding[3];
};

static bool TryGetTitleIdFromContentId(std::string* title_id) {
	std::string content_id;
	if (!Loader::SystemContentParamSfoGetString("CONTENT_ID", &content_id) || content_id.empty()) {
		return false;
	}

	const uint32_t dash = Common::FindIndex(content_id, '-');
	if (!Common::IndexValid(content_id, dash)) {
		return false;
	}

	const uint32_t underscore = Common::FindIndex(content_id, '_', dash + 1);
	if (!Common::IndexValid(content_id, underscore) || underscore <= dash + 1) {
		return false;
	}

	*title_id = Common::Mid(content_id, dash + 1, underscore - dash - 1);
	return !title_id->empty();
}

static bool ResolveTitleId(std::string* title_id) {
	if (Loader::SystemContentParamSfoGetString("TITLE_ID", title_id) && !title_id->empty()) {
		return true;
	}

	if (TryGetTitleIdFromContentId(title_id)) {
		LOGF("\t PS5 mode: TITLE_ID missing, using CONTENT_ID title segment\n");
		return true;
	}

	return false;
}

int KYTY_SYSV_ABI AppContentInitialize(const AppContentInitParam* init_param,
                                       AppContentBootParam*       boot_param) {
	PRINT_NAME();

	LOGF("\t init_param = 0x%016" PRIx64 "\n"
	     "\t boot_param = 0x%016" PRIx64 "\n",
	     reinterpret_cast<uint64_t>(init_param), reinterpret_cast<uint64_t>(boot_param));

	if (boot_param != nullptr) {
		std::memset(boot_param, 0, sizeof(*boot_param));
	}

	if (ResolveTitleId(&g_title_id)) {
		LOGF("\t title_id   = %s\n", g_title_id.c_str());
	} else {
		LOGF("\t TITLE_ID missing\n");
	}

	return OK;
}

int KYTY_SYSV_ABI AppContentAppParamGetInt(uint32_t param_id, int32_t* value) {
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(value == nullptr);

	*value     = 0;
	bool found = false;

	LOGF("\t param_id = %u\n", param_id);

	switch (param_id) {
		case 0:
			*value = APP_CONTENT_APPPARAM_SKU_FLAG_FULL;
			found  = true;
			break;
		case 1: found = Loader::SystemContentParamSfoGetInt("USER_DEFINED_PARAM_1", value); break;
		case 2: found = Loader::SystemContentParamSfoGetInt("USER_DEFINED_PARAM_2", value); break;
		case 3: found = Loader::SystemContentParamSfoGetInt("USER_DEFINED_PARAM_3", value); break;
		case 4: found = Loader::SystemContentParamSfoGetInt("USER_DEFINED_PARAM_4", value); break;
		default: EXIT("unknown param_id: %u\n", param_id);
	}

	LOGF("\t value    = %d [%s]\n", *value, found ? "found" : "not found");

	return OK;
}

int KYTY_SYSV_ABI AppContentDownloadDataGetAvailableSpaceKb(const AppContentMountPoint* mount_point,
                                                            size_t* available_space_kb) {
	PRINT_NAME();

	LOGF("\t mount_point        = 0x%016" PRIx64 "\n"
	     "\t available_space_kb = 0x%016" PRIx64 "\n",
	     reinterpret_cast<uint64_t>(mount_point), reinterpret_cast<uint64_t>(available_space_kb));

	if (available_space_kb == nullptr) {
		return APP_CONTENT_ERROR_PARAMETER;
	}

	std::error_code ec;
	const auto      space = std::filesystem::space(std::filesystem::current_path(), ec);
	if (ec) {
		LOGF("\t filesystem::space failed: %s\n", ec.message().c_str());
		*available_space_kb = 1024u * 1024u;
	} else {
		*available_space_kb = static_cast<size_t>(space.available / 1024u);
	}

	if (mount_point != nullptr) {
		char mount_name[sizeof(mount_point->data) + 1] {};
		std::memcpy(mount_name, mount_point->data, sizeof(mount_point->data));
		LOGF("\t mount             = %s\n", mount_name);
	}
	LOGF("\t available_space_kb = %" PRIu64 "\n", static_cast<uint64_t>(*available_space_kb));

	return OK;
}

int KYTY_SYSV_ABI AppContentAddcontMount(uint32_t                         service_label,
                                         const NpUnifiedEntitlementLabel* entitlement_label,
                                         AppContentMountPoint*            mount_point) {
	PRINT_NAME();

	LOGF("\t service_label     = %" PRIu32 "\n"
	     "\t entitlement_label = 0x%016" PRIx64 "\n"
	     "\t mount_point       = 0x%016" PRIx64 "\n",
	     service_label, reinterpret_cast<uint64_t>(entitlement_label),
	     reinterpret_cast<uint64_t>(mount_point));

	if (entitlement_label == nullptr || mount_point == nullptr) {
		return APP_CONTENT_ERROR_PARAMETER;
	}

	std::memset(mount_point->data, 0, sizeof(mount_point->data));

	return APP_CONTENT_ERROR_DRM_NO_ENTITLEMENT;
}

int KYTY_SYSV_ABI AppContentAddcontUnmount(const AppContentMountPoint* mount_point) {
	PRINT_NAME();

	LOGF("\t mount_point = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(mount_point));

	if (mount_point == nullptr) {
		return APP_CONTENT_ERROR_PARAMETER;
	}

	if (mount_point->data[0] == '\0') {
		return APP_CONTENT_ERROR_NOT_MOUNTED;
	}

	return OK;
}

} // namespace AppContent

namespace LibAppContentTemporary {

namespace AppContentTemporary {

using AppContentMountPoint = AppContent::AppContentMountPoint;

static int KYTY_SYSV_ABI AppContentTemporaryDataMount2(uint32_t              option,
                                                       AppContentMountPoint* mount_point) {
	PRINT_NAME();

	LOGF("\t option      = 0x%08" PRIx32 "\n"
	     "\t mount_point = 0x%016" PRIx64 "\n",
	     option, reinterpret_cast<uint64_t>(mount_point));

	if (mount_point == nullptr) {
		return AppContent::APP_CONTENT_ERROR_PARAMETER;
	}

	std::memset(mount_point->data, 0, sizeof(mount_point->data));
	std::memcpy(mount_point->data, "/temp0", sizeof("/temp0") - 1);

	LOGF("\t mount      = %s\n", mount_point->data);

	return OK;
}

static int KYTY_SYSV_ABI AppContentTemporaryDataFormat(const AppContentMountPoint* mount_point) {
	PRINT_NAME();

	LOGF("\t mount_point = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(mount_point));

	if (mount_point == nullptr) {
		return AppContent::APP_CONTENT_ERROR_PARAMETER;
	}

	return OK;
}

} // namespace AppContentTemporary

LIB_DEFINE(InitAppContent_1_Temporary) {
	LIB_FUNC("buYbeLOGWmA", AppContentTemporary::AppContentTemporaryDataMount2);
	LIB_FUNC("a5N7lAG0y2Q", AppContentTemporary::AppContentTemporaryDataFormat);
}

} // namespace LibAppContentTemporary

LIB_DEFINE(InitAppContent_1) {
	LIB_FUNC("R9lA82OraNs", AppContent::AppContentInitialize);
	LIB_FUNC("99b82IKXpH4", AppContent::AppContentAppParamGetInt);
	LIB_FUNC("VANhIWcqYak", AppContent::AppContentAddcontMount);
	LIB_FUNC("3rHWaV-1KC4", AppContent::AppContentAddcontUnmount);
	LIB_FUNC("Gl6w5i0JokY", AppContent::AppContentDownloadDataGetAvailableSpaceKb);
	LibAppContentTemporary::InitAppContent_1_Temporary(s);
}

} // namespace Libs
