#include "common/abi.h"
#include "common/assert.h"
#include "common/common.h"
#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/stringUtils.h"
#include "kernel/fileSystem.h"
#include "libs/errno.h"
#include "libs/libs.h"
#include "libs/systemService.h"
#include "loader/additionalContent.h"
#include "loader/symbolDatabase.h"
#include "loader/systemContent.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <system_error>
#include <vector>

namespace Libs {

namespace AprShared {
void InvalidateFailedResolves();
}

LIB_VERSION("AppContent", 1, "AppContentUtil", 1, 1);

namespace AppContent {

constexpr uint32_t APP_CONTENT_APPPARAM_SKU_FLAG_FULL = 3;
constexpr int      APP_CONTENT_ERROR_PARAMETER        = -2133262334; /* 0x80D90002 */
constexpr int      APP_CONTENT_ERROR_BUSY             = -2133262333; /* 0x80D90003 */
constexpr int      APP_CONTENT_ERROR_NOT_MOUNTED      = -2133262332; /* 0x80D90004 */
constexpr int      APP_CONTENT_ERROR_NOT_FOUND        = -2133262331; /* 0x80D90005 */
constexpr int      APP_CONTENT_ERROR_MOUNT_FULL       = -2133262330; /* 0x80D90006 */
constexpr size_t   APP_CONTENT_ADDCONT_MOUNT_MAXNUM   = 64;

struct AddcontMount {
	std::string label;
	std::string point;
};

static std::mutex                g_addcont_mutex;
static std::vector<AddcontMount> g_addcont_mounts;

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

	const auto dash = content_id.find('-');
	if (dash == std::string::npos) {
		return false;
	}

	const auto underscore = content_id.find('_', dash + 1);
	if (underscore == std::string::npos || underscore <= dash + 1) {
		return false;
	}

	*title_id = content_id.substr(dash + 1, underscore - dash - 1);
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

	SystemService::NotifyEntitlementUpdate();
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

static int GetAvailableSpaceKb(const AppContentMountPoint* mount_point,
                               size_t*                     available_space_kb) {
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

int KYTY_SYSV_ABI AppContentDownloadDataGetAvailableSpaceKb(const AppContentMountPoint* mount_point,
                                                            size_t* available_space_kb) {
	PRINT_NAME();
	return GetAvailableSpaceKb(mount_point, available_space_kb);
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

	if (service_label != 0 || entitlement_label == nullptr || mount_point == nullptr) {
		return APP_CONTENT_ERROR_PARAMETER;
	}

	std::memset(mount_point->data, 0, sizeof(mount_point->data));

	const auto* entry =
	    Loader::AddContentFind(entitlement_label->data, sizeof(entitlement_label->data));
	if (entry == nullptr) {
		return APP_CONTENT_ERROR_NOT_FOUND;
	}

	std::lock_guard lock(g_addcont_mutex);

	for (const auto& m: g_addcont_mounts) {
		if (m.label == entry->entitlement_label) {
			LOGF("\t already mounted %s\n", m.point.c_str());
			return APP_CONTENT_ERROR_BUSY;
		}
	}

	if (g_addcont_mounts.size() == APP_CONTENT_ADDCONT_MOUNT_MAXNUM) {
		return APP_CONTENT_ERROR_MOUNT_FULL;
	}
	std::error_code ec;
	if (!std::filesystem::is_directory(entry->mount_dir, ec)) {
		return APP_CONTENT_ERROR_NOT_FOUND;
	}

	uint32_t index = 0;
	while (
	    std::any_of(g_addcont_mounts.begin(), g_addcont_mounts.end(), [&](const AddcontMount& m) {
		    return m.point == "/addcont" + std::to_string(index) + "/";
	    })) {
		index++;
	}

	AddcontMount mount {entry->entitlement_label, "/addcont" + std::to_string(index) + "/"};
	LibKernel::FileSystem::Mount(entry->mount_dir, mount.point);
	LOGF("\t mounted %s -> %s\n", mount.point.c_str(),
	     Common::PathToString(entry->mount_dir).c_str());

	AprShared::InvalidateFailedResolves();

	// Titles scan /download0/pdlc for downloaded content; it must exist even when empty.
	if (const auto pdlc = LibKernel::FileSystem::GetRealFilename("/download0/pdlc"); !pdlc.empty()) {
		std::filesystem::create_directories(pdlc, ec);
	}

	const auto guest_point = mount.point.substr(0, mount.point.size() - 1);
	std::strncpy(mount_point->data, guest_point.c_str(), sizeof(mount_point->data) - 1);
	g_addcont_mounts.push_back(std::move(mount));

	return OK;
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

	const std::string point = Common::FixDirectorySlash(
	    std::string_view(mount_point->data, strnlen(mount_point->data, sizeof(mount_point->data))));

	std::lock_guard lock(g_addcont_mutex);

	const auto it = std::find_if(g_addcont_mounts.begin(), g_addcont_mounts.end(),
	                             [&](const AddcontMount& m) { return m.point == point; });
	if (it == g_addcont_mounts.end()) {
		return APP_CONTENT_ERROR_NOT_MOUNTED;
	}
	LibKernel::FileSystem::Umount(point);
	g_addcont_mounts.erase(it);
	AprShared::InvalidateFailedResolves();

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

static int KYTY_SYSV_ABI AppContentTemporaryDataGetAvailableSpaceKb(
    const AppContentMountPoint* mount_point, size_t* available_space_kb) {
	PRINT_NAME();
	return AppContent::GetAvailableSpaceKb(mount_point, available_space_kb);
}

} // namespace AppContentTemporary

LIB_DEFINE(InitAppContent_1_Temporary) {
	LIB_FUNC("buYbeLOGWmA", AppContentTemporary::AppContentTemporaryDataMount2);
	LIB_FUNC("a5N7lAG0y2Q", AppContentTemporary::AppContentTemporaryDataFormat);
	LIB_FUNC("SaKib2Ug0yI", AppContentTemporary::AppContentTemporaryDataGetAvailableSpaceKb);
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
