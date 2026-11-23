#include "common/abi.h"
#include "common/common.h"
#include "common/logging/log.h"
#include "libs/libs.h"
#include "loader/symbolDatabase.h"

#include <cstddef>

namespace Libs {

namespace LibSharePlay {

LIB_NAME("SharePlay", "SharePlay");

constexpr int SHARE_PLAY_ERROR_INVALID_ARGS        = -2129788927;
constexpr int SHARE_PLAY_ERROR_ALREADY_INITIALIZED = -2129788925;
constexpr int SHARE_PLAY_ERROR_NOT_INITIALIZED     = -2129788924;

struct SharePlayConnectionInfoA {
	int32_t  status;
	int32_t  mode;
	char     host_online_id[20];
	char     visitor_online_id[20];
	uint64_t host_account_id;
	uint64_t visitor_account_id;
	int32_t  host_user_id;
	int32_t  visitor_user_id;
};

static_assert(sizeof(SharePlayConnectionInfoA) == 72);
static_assert(offsetof(SharePlayConnectionInfoA, host_account_id) == 48);
static_assert(offsetof(SharePlayConnectionInfoA, host_user_id) == 64);

static bool g_share_play_initialized = false;

static int KYTY_SYSV_ABI SharePlayInitialize(void* heap, size_t heap_size) {
	PRINT_NAME();

	LOGF("\t heap      = 0x%016" PRIx64 "\n"
	     "\t heap_size = %" PRIu64 "\n",
	     reinterpret_cast<uint64_t>(heap), static_cast<uint64_t>(heap_size));

	if (g_share_play_initialized) {
		return SHARE_PLAY_ERROR_ALREADY_INITIALIZED;
	}
	if (heap != nullptr && heap_size < 6u * 1024u) {
		return SHARE_PLAY_ERROR_INVALID_ARGS;
	}
	g_share_play_initialized = true;
	return 0;
}

static int KYTY_SYSV_ABI SharePlayTerminate() {
	PRINT_NAME();

	if (!g_share_play_initialized) {
		return SHARE_PLAY_ERROR_NOT_INITIALIZED;
	}
	g_share_play_initialized = false;
	return 0;
}

static int KYTY_SYSV_ABI SharePlayGetCurrentConnectionInfoA(SharePlayConnectionInfoA* info) {
	if (!g_share_play_initialized) {
		return SHARE_PLAY_ERROR_NOT_INITIALIZED;
	}
	if (info == nullptr) {
		return SHARE_PLAY_ERROR_INVALID_ARGS;
	}
	*info = {.status = 0, .host_user_id = -1, .visitor_user_id = -1};
	return 0;
}

LIB_DEFINE(InitPlatform_1_SharePlay) {
	LIB_FUNC("isruqthpYcw", LibSharePlay::SharePlayInitialize);
	LIB_FUNC("UaLjloJinow", LibSharePlay::SharePlayTerminate);
	LIB_FUNC("+MCXJlWdi+s", LibSharePlay::SharePlayGetCurrentConnectionInfoA);
}

} // namespace LibSharePlay

} // namespace Libs
