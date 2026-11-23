#include "common/abi.h"
#include "common/common.h"
#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "libs/libs.h"
#include "libs/systemService.h"
#include "loader/symbolDatabase.h"

#include <atomic>
#include <cstddef>
#include <cstring>
#include <nlohmann/json.hpp>
#include <string>

namespace Libs {

namespace LibNpGameIntent {

LIB_NAME("NpGameIntent", "NpGameIntent");

constexpr int    NP_GAME_INTENT_ERROR_INVALID_ARGUMENT    = -2141898748; /* 0x80553804 */
constexpr int    NP_GAME_INTENT_ERROR_INSUFFICIENT_BUFFER = -2141898747; /* 0x80553805 */
constexpr int    NP_GAME_INTENT_ERROR_INTENT_NOT_FOUND    = -2141898746; /* 0x80553806 */
constexpr int    NP_GAME_INTENT_ERROR_VALUE_NOT_FOUND     = -2141898745; /* 0x80553807 */
constexpr int    NP_GAME_INTENT_USER_ID_INVALID           = -1;
constexpr size_t NP_GAME_INTENT_TYPE_MAX_SIZE             = 33;
constexpr size_t NP_GAME_INTENT_DATA_MAX_SIZE             = 16 * 1024 + 1;

struct NpGameIntentData {
	uint8_t data[NP_GAME_INTENT_DATA_MAX_SIZE];
	uint8_t padding[7];
};

struct NpGameIntentInfo {
	size_t           size;
	int32_t          user_id;
	char             intent_type[NP_GAME_INTENT_TYPE_MAX_SIZE];
	uint8_t          padding[7];
	uint8_t          reserved[256];
	NpGameIntentData intent_data;
};

static_assert(sizeof(NpGameIntentData) == 16392);
static_assert(sizeof(NpGameIntentInfo) == 16704);

static std::atomic_int g_launch_intent = 0; // 1: event queued, 2: intent received

static int KYTY_SYSV_ABI NpGameIntentInitialize(const void* init_param) {
	PRINT_NAME();

	LOGF("\t init_param = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(init_param));

	int idle = 0;
	if (!Config::GetLaunchActivity().empty() && g_launch_intent.compare_exchange_strong(idle, 1)) {
		SystemService::NotifyEvent(SystemService::SYSTEM_SERVICE_EVENT_GAME_INTENT);
	}
	return 0;
}

static int KYTY_SYSV_ABI NpGameIntentTerminate() {
	PRINT_NAME();

	return 0;
}

static int KYTY_SYSV_ABI NpGameIntentReceiveIntent(NpGameIntentInfo* intent_info) {
	PRINT_NAME();

	LOGF("\t intent_info = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(intent_info));

	if (intent_info == nullptr || intent_info->size != sizeof(NpGameIntentInfo)) {
		return NP_GAME_INTENT_ERROR_INVALID_ARGUMENT;
	}

	intent_info->user_id = NP_GAME_INTENT_USER_ID_INVALID;
	std::memset(intent_info->intent_type, 0, sizeof(intent_info->intent_type));
	std::memset(&intent_info->intent_data, 0, sizeof(intent_info->intent_data));

	int queued = 1;
	if (!g_launch_intent.compare_exchange_strong(queued, 2)) {
		return NP_GAME_INTENT_ERROR_INTENT_NOT_FOUND;
	}
	const auto data      = nlohmann::json {{"activityId", Config::GetLaunchActivity()}}.dump();
	intent_info->user_id = Config::GetUserId();
	std::strcpy(intent_info->intent_type, "launchActivity");
	std::memcpy(intent_info->intent_data.data, data.data(), data.size());
	return 0;
}

static int KYTY_SYSV_ABI NpGameIntentGetPropertyValueString(const NpGameIntentData* intent_data,
                                                            const char* key, char* value_buf,
                                                            size_t buf_size) {
	PRINT_NAME();

	LOGF("\t intent_data = 0x%016" PRIx64 "\n"
	     "\t key         = %s\n"
	     "\t value_buf   = 0x%016" PRIx64 "\n"
	     "\t buf_size    = 0x%016" PRIx64 "\n",
	     reinterpret_cast<uint64_t>(intent_data), key != nullptr ? key : "(null)",
	     reinterpret_cast<uint64_t>(value_buf), buf_size);

	if (intent_data == nullptr || key == nullptr || key[0] == '\0' || value_buf == nullptr ||
	    buf_size == 0) {
		return NP_GAME_INTENT_ERROR_INVALID_ARGUMENT;
	}

	std::memset(value_buf, 0, buf_size);
	const auto* json  = reinterpret_cast<const char*>(intent_data->data);
	const auto  data  = nlohmann::json::parse(json, json + strnlen(json, sizeof(intent_data->data)),
	                                          nullptr, false);
	const auto  it    = data.find(key);
	const auto* value = it != data.end() ? it->get_ptr<const std::string*>() : nullptr;
	if (value == nullptr || value->empty()) {
		return NP_GAME_INTENT_ERROR_VALUE_NOT_FOUND;
	}
	if (value->size() >= buf_size) {
		return NP_GAME_INTENT_ERROR_INSUFFICIENT_BUFFER;
	}
	std::memcpy(value_buf, value->data(), value->size());
	return 0;
}

LIB_DEFINE(InitNet_1_NpGameIntent) {
	LIB_FUNC("m87BHxt-H60", LibNpGameIntent::NpGameIntentInitialize);
	LIB_FUNC("0HBYxYAjmf0", LibNpGameIntent::NpGameIntentTerminate);
	LIB_FUNC("jEIXUAr9XE8", LibNpGameIntent::NpGameIntentReceiveIntent);
	LIB_FUNC("rPl0INNc-M8", LibNpGameIntent::NpGameIntentGetPropertyValueString);
}

} // namespace LibNpGameIntent

} // namespace Libs
