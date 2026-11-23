#include "common/abi.h"
#include "common/common.h"
#include "common/logging/log.h"
#include "libs/libs.h"
#include "loader/symbolDatabase.h"

#include <cstddef>
#include <map>

namespace Libs {

namespace LibGameUpdate {

LIB_NAME("GameUpdate", "GameUpdate");

constexpr int GAME_UPDATE_ERROR_NOT_INITIALIZED   = static_cast<int>(0x80412801);
constexpr int GAME_UPDATE_ERROR_INVALID_ARG       = static_cast<int>(0x80412803);
constexpr int GAME_UPDATE_ERROR_INVALID_SIZE      = static_cast<int>(0x80412804);
constexpr int GAME_UPDATE_ERROR_REQUEST_NOT_FOUND = static_cast<int>(0x80412805);

struct GameUpdateCheckParam {
	size_t   size;
	uint32_t option;
	uint32_t reserved[9];
};

struct GameUpdateCheckResult {
	size_t   size;
	bool     found;
	bool     addcont_found;
	char     padding[2];
	char     content_version[11];
	char     padding2[1];
	uint32_t reserved[6];
};

struct GameUpdateAddcontVersionInfo {
	size_t   size;
	bool     found;
	char     content_version[11];
	uint32_t reserved[6];
};

static bool                g_game_update_initialized  = false;
static int                 g_game_update_next_request = 1;
static std::map<int, bool> g_game_update_requests;

static int KYTY_SYSV_ABI GameUpdateInitialize() {
	PRINT_NAME();

	g_game_update_initialized = true;

	return 0;
}

static int KYTY_SYSV_ABI GameUpdateTerminate() {
	PRINT_NAME();

	g_game_update_initialized = false;
	g_game_update_requests.clear();

	return 0;
}

static int KYTY_SYSV_ABI GameUpdateCreateRequest() {
	PRINT_NAME();

	if (!g_game_update_initialized) {
		return GAME_UPDATE_ERROR_NOT_INITIALIZED;
	}

	const int request_id               = g_game_update_next_request++;
	g_game_update_requests[request_id] = true;

	return request_id;
}

static int KYTY_SYSV_ABI GameUpdateCheck(int request_id, const GameUpdateCheckParam* param,
                                         GameUpdateCheckResult* result) {
	PRINT_NAME();

	LOGF("\t request_id = %d\n"
	     "\t param      = 0x%016" PRIx64 "\n"
	     "\t result     = 0x%016" PRIx64 "\n",
	     request_id, reinterpret_cast<uint64_t>(param), reinterpret_cast<uint64_t>(result));

	if (!g_game_update_initialized) {
		return GAME_UPDATE_ERROR_NOT_INITIALIZED;
	}
	if (g_game_update_requests.find(request_id) == g_game_update_requests.end()) {
		return GAME_UPDATE_ERROR_REQUEST_NOT_FOUND;
	}
	if (param == nullptr || result == nullptr) {
		return GAME_UPDATE_ERROR_INVALID_ARG;
	}
	if (param->size < sizeof(GameUpdateCheckParam) ||
	    result->size < sizeof(GameUpdateCheckResult)) {
		return GAME_UPDATE_ERROR_INVALID_SIZE;
	}

	const auto result_size = result->size;
	*result                = {};
	result->size           = result_size;
	result->found          = false;
	result->addcont_found  = false;

	return 0;
}

static int KYTY_SYSV_ABI GameUpdateAbortRequest(int request_id) {
	PRINT_NAME();

	LOGF("\t request_id = %d\n", request_id);

	if (!g_game_update_initialized) {
		return GAME_UPDATE_ERROR_NOT_INITIALIZED;
	}
	if (g_game_update_requests.find(request_id) == g_game_update_requests.end()) {
		return GAME_UPDATE_ERROR_REQUEST_NOT_FOUND;
	}

	return 0;
}

static int KYTY_SYSV_ABI GameUpdateDeleteRequest(int request_id) {
	PRINT_NAME();

	LOGF("\t request_id = %d\n", request_id);

	if (!g_game_update_initialized) {
		return GAME_UPDATE_ERROR_NOT_INITIALIZED;
	}
	if (g_game_update_requests.erase(request_id) == 0) {
		return GAME_UPDATE_ERROR_REQUEST_NOT_FOUND;
	}

	return 0;
}

static int KYTY_SYSV_ABI GameUpdateGetAddcontLatestVersion(uint32_t    service_label,
                                                           const void* entitlement_label,
                                                           GameUpdateAddcontVersionInfo* info) {
	PRINT_NAME();

	LOGF("\t service_label     = %" PRIu32 "\n"
	     "\t entitlement_label = 0x%016" PRIx64 "\n"
	     "\t info              = 0x%016" PRIx64 "\n",
	     service_label, reinterpret_cast<uint64_t>(entitlement_label),
	     reinterpret_cast<uint64_t>(info));

	if (!g_game_update_initialized) {
		return GAME_UPDATE_ERROR_NOT_INITIALIZED;
	}
	if (info == nullptr) {
		return GAME_UPDATE_ERROR_INVALID_ARG;
	}
	if (info->size < sizeof(GameUpdateAddcontVersionInfo)) {
		return GAME_UPDATE_ERROR_INVALID_SIZE;
	}

	const auto info_size = info->size;
	*info                = {};
	info->size           = info_size;
	info->found          = false;

	return 0;
}

LIB_DEFINE(InitNet_1_GameUpdate) {
	LIB_FUNC("YJtKLttI9fM", LibGameUpdate::GameUpdateInitialize);
	LIB_FUNC("NSH-C-OmoNI", LibGameUpdate::GameUpdateTerminate);
	LIB_FUNC("UvcvKaFvupA", LibGameUpdate::GameUpdateCreateRequest);
	LIB_FUNC("LYVV9z8+owM", LibGameUpdate::GameUpdateCheck);
	LIB_FUNC("d1CNGEOaK28", LibGameUpdate::GameUpdateAbortRequest);
	LIB_FUNC("bcCyjHN5sn0", LibGameUpdate::GameUpdateDeleteRequest);
	LIB_FUNC("0g0+Oq9xcI0", LibGameUpdate::GameUpdateGetAddcontLatestVersion);
}

} // namespace LibGameUpdate

} // namespace Libs
