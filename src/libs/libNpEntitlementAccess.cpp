#include "common/abi.h"
#include "common/logging/log.h"
#include "libs/libs.h"
#include "loader/additionalContent.h"
#include "loader/symbolDatabase.h"

#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <limits>
#include <map>
#include <mutex>

namespace Libs {

namespace LibNpEntitlementAccess {

LIB_VERSION("NpEntitlementAccess", 1, "NpEntitlementAccess", 1, 1);

constexpr int      NP_ENTITLEMENT_ACCESS_ERROR_NOT_INITIALIZED   = -2122514431; // 0x817D0001
constexpr int      NP_ENTITLEMENT_ACCESS_ERROR_OUT_OF_MEMORY     = -2122514416; // 0x817D0010
constexpr int      NP_ENTITLEMENT_ACCESS_ERROR_REQUEST_NOT_FOUND = -2122514411; // 0x817D0015
constexpr int      NP_ENTITLEMENT_ACCESS_ERROR_PARAMETER         = -2122514430; /* 0x817D0002 */
constexpr int      NP_ENTITLEMENT_ACCESS_ERROR_NO_ENTITLEMENT    = -2122514425; /* 0x817D0007 */
constexpr uint32_t NP_ENTITLEMENT_ACCESS_SKU_FLAG_FULL           = 3;
constexpr uint32_t NP_ENTITLEMENT_ACCESS_GAME_TRIALS_FLAG_OFF    = 0;
constexpr int32_t  NP_ENTITLEMENT_ACCESS_INVALID_OFFSET          = -1;
constexpr int32_t  NP_ENTITLEMENT_ACCESS_ENTITLEMENT_INFO_LIST_MAX_SIZE = 100;

struct NpEntitlementAccessInitParam {
	char reserved[32];
};

struct NpEntitlementAccessBootParam {
	char reserved[32];
};

struct NpUnifiedEntitlementLabel {
	char data[17];
	char padding[3];
};

struct NpEntitlementAccessAddcontEntitlementInfo {
	NpUnifiedEntitlementLabel entitlement_label;
	uint32_t                  package_type;
	uint32_t                  download_status;
};

struct NpEntitlementAccessEntitlementKey {
	uint8_t data[16];
};

struct NpServiceEntitlementLabel {
	char data[7];
	char padding[13];
};

struct NpEntitlementAccessTransactionId {
	char data[65];
	char padding[7];
};

struct NpEntitlementAccessRequestEntitlementInfoListParam {
	size_t   size;
	uint32_t entitlement_type;
	int32_t  offset;
	int32_t  limit;
	uint32_t sort;
	uint32_t direction;
	uint32_t package_type;
};

struct NpEntitlementAccessUnifiedEntitlementInfo {
	NpUnifiedEntitlementLabel entitlement_label;
	uint64_t                  active_date;
	uint64_t                  inactive_date;
	uint32_t                  entitlement_type;
	int32_t                   use_count;
	int32_t                   use_limit;
	uint32_t                  package_type;
	bool                      active_flag;
	int8_t                    reserved[3];
};

struct NpEntitlementAccessServiceEntitlementInfo {
	NpServiceEntitlementLabel entitlement_label;
	uint64_t                  active_date;
	uint64_t                  inactive_date;
	uint32_t                  entitlement_type;
	int32_t                   use_count;
	int32_t                   use_limit;
	uint32_t                  reserved1;
	bool                      active_flag;
	bool                      is_consumable;
	int8_t                    reserved2[2];
};

static_assert(sizeof(NpUnifiedEntitlementLabel) == 20);
static_assert(sizeof(NpServiceEntitlementLabel) == 20);
static_assert(sizeof(NpEntitlementAccessTransactionId) == 72);
static_assert(sizeof(NpEntitlementAccessRequestEntitlementInfoListParam) == 32);
static_assert(sizeof(NpEntitlementAccessUnifiedEntitlementInfo) == 64);
static_assert(offsetof(NpEntitlementAccessUnifiedEntitlementInfo, active_date) == 24);
static_assert(sizeof(NpEntitlementAccessServiceEntitlementInfo) == 64);
static_assert(offsetof(NpEntitlementAccessServiceEntitlementInfo, active_date) == 24);

enum class EntitlementRequestKind { UnifiedInfo, UnifiedList, ServiceList, ConsumeUnified };

struct EntitlementRequest {
	EntitlementRequestKind kind;
	int32_t                result;
};

static std::atomic<bool>                     g_np_entitlement_initialized {false};
static std::mutex                            g_np_entitlement_request_mutex;
static uint64_t                              g_np_entitlement_next_request_id  = 1;
static uint64_t                              g_np_entitlement_next_transaction = 1;
static std::map<int64_t, EntitlementRequest> g_np_entitlement_requests;

static int np_entitlement_create_request(EntitlementRequestKind kind, int32_t result,
                                         int64_t* request_id) {
	if (!g_np_entitlement_initialized.load()) {
		return NP_ENTITLEMENT_ACCESS_ERROR_NOT_INITIALIZED;
	}
	if (request_id == nullptr) {
		return NP_ENTITLEMENT_ACCESS_ERROR_PARAMETER;
	}
	std::scoped_lock lock(g_np_entitlement_request_mutex);
	if (g_np_entitlement_next_request_id >
	    static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
		return NP_ENTITLEMENT_ACCESS_ERROR_OUT_OF_MEMORY;
	}
	const auto id = static_cast<int64_t>(g_np_entitlement_next_request_id);
	g_np_entitlement_requests.emplace(id, EntitlementRequest {kind, result});
	++g_np_entitlement_next_request_id;
	*request_id = id;
	return 0;
}

static int np_entitlement_poll_request(int64_t request_id, EntitlementRequestKind kind,
                                       int32_t* result) {
	if (!g_np_entitlement_initialized.load()) {
		return NP_ENTITLEMENT_ACCESS_ERROR_NOT_INITIALIZED;
	}
	if (result == nullptr) {
		return NP_ENTITLEMENT_ACCESS_ERROR_PARAMETER;
	}
	std::scoped_lock lock(g_np_entitlement_request_mutex);
	const auto       it = g_np_entitlement_requests.find(request_id);
	if (it == g_np_entitlement_requests.end()) {
		return NP_ENTITLEMENT_ACCESS_ERROR_REQUEST_NOT_FOUND;
	}
	if (it->second.kind != kind) {
		return NP_ENTITLEMENT_ACCESS_ERROR_PARAMETER;
	}
	*result = it->second.result;
	return 0;
}

static constexpr NpEntitlementAccessAddcontEntitlementInfo NP_ENTITLEMENT_ACCESS_ADDON_LIST[] = {
    {{{"85y-je"}, {}}, 3, 4}, // GTA V hash 0xf4315381
    {{{"5d5c48"}, {}}, 3, 4}, // GTA V hash 0x961c34b0
    {{{"_mtqu6"}, {}}, 3, 4}, // GTA V hash 0x9cd1bcad
};

static NpEntitlementAccessAddcontEntitlementInfo
MakeAddcontInfo(const Loader::AddContentEntry& entry) {
	NpEntitlementAccessAddcontEntitlementInfo info {};
	std::memcpy(info.entitlement_label.data, entry.entitlement_label.data(),
	            entry.entitlement_label.size());
	const bool extra_data = !entry.mount_dir.empty();
	info.package_type     = extra_data ? 2 : 3; // PSAC (with extra data) or PSAL (license only).
	info.download_status  = extra_data ? 4 : 0; // INSTALLED or NO_EXTRA_DATA.
	return info;
}

static int KYTY_SYSV_ABI NpEntitlementAccessInitialize(
    const NpEntitlementAccessInitParam* init_param, NpEntitlementAccessBootParam* boot_param) {
	PRINT_NAME();

	if (init_param == nullptr || boot_param == nullptr) {
		return NP_ENTITLEMENT_ACCESS_ERROR_PARAMETER;
	}

	std::scoped_lock lock(g_np_entitlement_request_mutex);
	memset(boot_param, 0, sizeof(*boot_param));
	g_np_entitlement_initialized.store(true);

	return 0;
}

static int KYTY_SYSV_ABI NpEntitlementAccessGetSkuFlag(uint32_t* sku_flag) {
	PRINT_NAME();

	if (sku_flag == nullptr) {
		return NP_ENTITLEMENT_ACCESS_ERROR_PARAMETER;
	}

	*sku_flag = NP_ENTITLEMENT_ACCESS_SKU_FLAG_FULL;
	return 0;
}

static int KYTY_SYSV_ABI NpEntitlementAccessGetGameTrialsFlag(uint32_t* flag) {
	PRINT_NAME();
	if (!g_np_entitlement_initialized.load()) {
		return NP_ENTITLEMENT_ACCESS_ERROR_NOT_INITIALIZED;
	}
	if (flag == nullptr) {
		return NP_ENTITLEMENT_ACCESS_ERROR_PARAMETER;
	}
	*flag = NP_ENTITLEMENT_ACCESS_GAME_TRIALS_FLAG_OFF;
	return 0;
}

static int KYTY_SYSV_ABI
NpEntitlementAccessGenerateTransactionId(NpEntitlementAccessTransactionId* transaction_id) {
	PRINT_NAME();
	if (!g_np_entitlement_initialized.load()) {
		return NP_ENTITLEMENT_ACCESS_ERROR_NOT_INITIALIZED;
	}
	if (transaction_id == nullptr) {
		return NP_ENTITLEMENT_ACCESS_ERROR_PARAMETER;
	}
	std::scoped_lock lock(g_np_entitlement_request_mutex);
	if (g_np_entitlement_next_transaction == 0) {
		return NP_ENTITLEMENT_ACCESS_ERROR_OUT_OF_MEMORY;
	}
	memset(transaction_id, 0, sizeof(*transaction_id));
	std::snprintf(transaction_id->data, sizeof(transaction_id->data), "6b797479%024" PRIx64,
	              g_np_entitlement_next_transaction++);
	return 0;
}

static int
np_entitlement_request_list(EntitlementRequestKind kind, const void* list, uint32_t list_num,
                            const NpEntitlementAccessRequestEntitlementInfoListParam* param,
                            int64_t*                                                  request_id) {
	if (!g_np_entitlement_initialized.load()) {
		return NP_ENTITLEMENT_ACCESS_ERROR_NOT_INITIALIZED;
	}
	if (param == nullptr || request_id == nullptr || (list == nullptr && list_num != 0)) {
		return NP_ENTITLEMENT_ACCESS_ERROR_PARAMETER;
	}
	if (param->size != sizeof(*param) || param->offset < 0 || param->limit < 1 ||
	    param->limit > NP_ENTITLEMENT_ACCESS_ENTITLEMENT_INFO_LIST_MAX_SIZE || param->sort > 1 ||
	    param->direction > 2) {
		return NP_ENTITLEMENT_ACCESS_ERROR_PARAMETER;
	}
	const bool unified = kind == EntitlementRequestKind::UnifiedList;
	if (param->entitlement_type != (unified ? 2u : 1u) ||
	    (unified
	         ? (param->package_type != 0 && (param->package_type < 4 || param->package_type > 6))
	         : param->package_type != 0)) {
		return NP_ENTITLEMENT_ACCESS_ERROR_PARAMETER;
	}
	// The offline implementation has no server entitlements. These completed requests
	// remain available to their matching poll function until explicitly deleted.
	return np_entitlement_create_request(kind, 0, request_id);
}

static int KYTY_SYSV_ABI NpEntitlementAccessRequestUnifiedEntitlementInfoList(
    int32_t user_id, uint32_t service_label, const NpUnifiedEntitlementLabel* list,
    uint32_t list_num, const NpEntitlementAccessRequestEntitlementInfoListParam* param,
    int64_t* request_id) {
	PRINT_NAME();
	LOGF("\t user_id = %" PRId32 ", service_label = %" PRIu32 "\n", user_id, service_label);
	return np_entitlement_request_list(EntitlementRequestKind::UnifiedList, list, list_num, param,
	                                   request_id);
}

static int KYTY_SYSV_ABI NpEntitlementAccessRequestServiceEntitlementInfoList(
    int32_t user_id, uint32_t service_label, const NpServiceEntitlementLabel* list,
    uint32_t list_num, const NpEntitlementAccessRequestEntitlementInfoListParam* param,
    int64_t* request_id) {
	PRINT_NAME();
	LOGF("\t user_id = %" PRId32 ", service_label = %" PRIu32 "\n", user_id, service_label);
	return np_entitlement_request_list(EntitlementRequestKind::ServiceList, list, list_num, param,
	                                   request_id);
}

static int KYTY_SYSV_ABI NpEntitlementAccessRequestUnifiedEntitlementInfo(
    int32_t user_id, uint32_t service_label, const NpUnifiedEntitlementLabel* entitlement_label,
    int64_t* request_id) {
	PRINT_NAME();
	LOGF("\t user_id = %" PRId32 ", service_label = %" PRIu32 "\n", user_id, service_label);
	if (!g_np_entitlement_initialized.load()) {
		return NP_ENTITLEMENT_ACCESS_ERROR_NOT_INITIALIZED;
	}
	if (entitlement_label == nullptr) {
		return NP_ENTITLEMENT_ACCESS_ERROR_PARAMETER;
	}
	return np_entitlement_create_request(EntitlementRequestKind::UnifiedInfo,
	                                     NP_ENTITLEMENT_ACCESS_ERROR_NO_ENTITLEMENT, request_id);
}

static int KYTY_SYSV_ABI NpEntitlementAccessRequestConsumeUnifiedEntitlement(
    int32_t user_id, uint32_t service_label, const NpUnifiedEntitlementLabel* entitlement_label,
    const NpEntitlementAccessTransactionId* transaction_id, int32_t use_count,
    int64_t* request_id) {
	PRINT_NAME();
	LOGF("\t user_id = %" PRId32 ", service_label = %" PRIu32 ", use_count = %" PRId32 "\n",
	     user_id, service_label, use_count);
	if (!g_np_entitlement_initialized.load()) {
		return NP_ENTITLEMENT_ACCESS_ERROR_NOT_INITIALIZED;
	}
	if (entitlement_label == nullptr || transaction_id == nullptr || use_count <= 0) {
		return NP_ENTITLEMENT_ACCESS_ERROR_PARAMETER;
	}
	return np_entitlement_create_request(EntitlementRequestKind::ConsumeUnified,
	                                     NP_ENTITLEMENT_ACCESS_ERROR_NO_ENTITLEMENT, request_id);
}

template <typename Info>
static int np_entitlement_poll_list(int64_t request_id, EntitlementRequestKind kind,
                                    int32_t* result, Info* list, uint32_t list_num,
                                    uint32_t* hit_num, int32_t* next_offset,
                                    int32_t* previous_offset) {
	if (!g_np_entitlement_initialized.load()) {
		return NP_ENTITLEMENT_ACCESS_ERROR_NOT_INITIALIZED;
	}
	if (result == nullptr || hit_num == nullptr || next_offset == nullptr ||
	    previous_offset == nullptr || (list == nullptr && list_num != 0)) {
		return NP_ENTITLEMENT_ACCESS_ERROR_PARAMETER;
	}
	if (const int ret = np_entitlement_poll_request(request_id, kind, result); ret != 0) {
		return ret;
	}
	*hit_num         = 0;
	*next_offset     = NP_ENTITLEMENT_ACCESS_INVALID_OFFSET;
	*previous_offset = NP_ENTITLEMENT_ACCESS_INVALID_OFFSET;
	return 0;
}

static int KYTY_SYSV_ABI NpEntitlementAccessPollUnifiedEntitlementInfoList(
    int64_t request_id, int32_t* result, NpEntitlementAccessUnifiedEntitlementInfo* list,
    uint32_t list_num, uint32_t* hit_num, int32_t* next_offset, int32_t* previous_offset) {
	PRINT_NAME();
	return np_entitlement_poll_list(request_id, EntitlementRequestKind::UnifiedList, result, list,
	                                list_num, hit_num, next_offset, previous_offset);
}

static int KYTY_SYSV_ABI NpEntitlementAccessPollServiceEntitlementInfoList(
    int64_t request_id, int32_t* result, NpEntitlementAccessServiceEntitlementInfo* list,
    uint32_t list_num, uint32_t* hit_num, int32_t* next_offset, int32_t* previous_offset) {
	PRINT_NAME();
	return np_entitlement_poll_list(request_id, EntitlementRequestKind::ServiceList, result, list,
	                                list_num, hit_num, next_offset, previous_offset);
}

static int KYTY_SYSV_ABI NpEntitlementAccessPollUnifiedEntitlementInfo(
    int64_t request_id, int32_t* result, NpEntitlementAccessUnifiedEntitlementInfo* info) {
	PRINT_NAME();
	if (!g_np_entitlement_initialized.load()) {
		return NP_ENTITLEMENT_ACCESS_ERROR_NOT_INITIALIZED;
	}
	if (info == nullptr) {
		return NP_ENTITLEMENT_ACCESS_ERROR_PARAMETER;
	}
	// No info is written when the offline request result is NO_ENTITLEMENT.
	return np_entitlement_poll_request(request_id, EntitlementRequestKind::UnifiedInfo, result);
}

static int KYTY_SYSV_ABI NpEntitlementAccessPollConsumeEntitlement(int64_t  request_id,
                                                                   int32_t* result,
                                                                   int32_t* use_limit) {
	PRINT_NAME();
	if (!g_np_entitlement_initialized.load()) {
		return NP_ENTITLEMENT_ACCESS_ERROR_NOT_INITIALIZED;
	}
	if (use_limit == nullptr) {
		return NP_ENTITLEMENT_ACCESS_ERROR_PARAMETER;
	}
	if (const int ret =
	        np_entitlement_poll_request(request_id, EntitlementRequestKind::ConsumeUnified, result);
	    ret != 0) {
		return ret;
	}
	if (*result == 0) {
		*use_limit = 0;
	}
	return 0;
}

static int KYTY_SYSV_ABI NpEntitlementAccessAbortRequest(int64_t request_id) {
	PRINT_NAME();
	if (!g_np_entitlement_initialized.load()) {
		return NP_ENTITLEMENT_ACCESS_ERROR_NOT_INITIALIZED;
	}
	std::scoped_lock lock(g_np_entitlement_request_mutex);
	// Offline requests finish at creation, so there is no running work to abort.
	return g_np_entitlement_requests.contains(request_id)
	           ? 0
	           : NP_ENTITLEMENT_ACCESS_ERROR_REQUEST_NOT_FOUND;
}

static int KYTY_SYSV_ABI NpEntitlementAccessDeleteRequest(int64_t request_id) {
	PRINT_NAME();
	if (!g_np_entitlement_initialized.load()) {
		return NP_ENTITLEMENT_ACCESS_ERROR_NOT_INITIALIZED;
	}
	std::scoped_lock lock(g_np_entitlement_request_mutex);
	return g_np_entitlement_requests.erase(request_id) != 0
	           ? 0
	           : NP_ENTITLEMENT_ACCESS_ERROR_REQUEST_NOT_FOUND;
}

static int KYTY_SYSV_ABI NpEntitlementAccessGetAddcontEntitlementInfoList(
    uint32_t service_label, NpEntitlementAccessAddcontEntitlementInfo* list, uint32_t list_num,
    uint32_t* hit_num) {
	PRINT_NAME();

	LOGF("\t service_label = %" PRIu32 "\n", service_label);
	LOGF("\t list          = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(list));
	LOGF("\t list_num      = %" PRIu32 "\n", list_num);
	LOGF("\t hit_num       = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(hit_num));

	if (hit_num == nullptr) {
		return NP_ENTITLEMENT_ACCESS_ERROR_PARAMETER;
	}

	constexpr auto legacy_count = std::size(NP_ENTITLEMENT_ACCESS_ADDON_LIST);
	const auto&    entries      = Loader::AddContentGetAll();
	*hit_num                    = static_cast<uint32_t>(legacy_count + entries.size());

	if (list != nullptr && list_num != 0) {
		memset(list, 0, sizeof(*list) * list_num);

		const auto copy_num = std::min(list_num, *hit_num);
		for (uint32_t i = 0; i < copy_num; i++) {
			list[i] = i < legacy_count ? NP_ENTITLEMENT_ACCESS_ADDON_LIST[i]
			                          : MakeAddcontInfo(entries[i - legacy_count]);
		}
	}

	return 0;
}

static int KYTY_SYSV_ABI NpEntitlementAccessGetAddcontEntitlementInfo(
    uint32_t service_label, const NpUnifiedEntitlementLabel* entitlement_label,
    NpEntitlementAccessAddcontEntitlementInfo* info) {
	PRINT_NAME();

	LOGF("\t service_label     = %" PRIu32 "\n", service_label);
	LOGF("\t entitlement_label = 0x%016" PRIx64 "\n",
	     reinterpret_cast<uint64_t>(entitlement_label));
	LOGF("\t info              = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(info));

	if (entitlement_label == nullptr || info == nullptr) {
		return NP_ENTITLEMENT_ACCESS_ERROR_PARAMETER;
	}

	memset(info, 0, sizeof(*info));

	for (const auto& entitlement: NP_ENTITLEMENT_ACCESS_ADDON_LIST) {
		if (strncmp(entitlement_label->data, entitlement.entitlement_label.data,
		            sizeof(entitlement_label->data)) == 0) {
			*info = entitlement;
			return 0;
		}
	}

	if (const auto* entry =
	        Loader::AddContentFind(entitlement_label->data, sizeof(entitlement_label->data))) {
		*info = MakeAddcontInfo(*entry);
		return 0;
	}

	return NP_ENTITLEMENT_ACCESS_ERROR_NO_ENTITLEMENT;
}

static int KYTY_SYSV_ABI NpEntitlementAccessGetEntitlementKey(
    uint32_t service_label, const NpUnifiedEntitlementLabel* entitlement_label,
    NpEntitlementAccessEntitlementKey* key) {
	PRINT_NAME();

	LOGF("\t service_label     = %" PRIu32 "\n"
	     "\t entitlement_label = 0x%016" PRIx64 "\n"
	     "\t key               = 0x%016" PRIx64 "\n",
	     service_label, reinterpret_cast<uint64_t>(entitlement_label),
	     reinterpret_cast<uint64_t>(key));

	if (entitlement_label == nullptr || key == nullptr) {
		return NP_ENTITLEMENT_ACCESS_ERROR_PARAMETER;
	}

	return NP_ENTITLEMENT_ACCESS_ERROR_NO_ENTITLEMENT;
}

LIB_DEFINE(InitNet_1_NpEntitlementAccess) {
	LIB_FUNC("jO8DM8oyego", LibNpEntitlementAccess::NpEntitlementAccessInitialize);
	LIB_FUNC("lPDO62PpJIA", LibNpEntitlementAccess::NpEntitlementAccessGetSkuFlag);
	LIB_FUNC("TFyU+KFBv54",
	         LibNpEntitlementAccess::NpEntitlementAccessGetAddcontEntitlementInfoList);
	LIB_FUNC("xddD23+8TfQ", LibNpEntitlementAccess::NpEntitlementAccessGetAddcontEntitlementInfo);
	LIB_FUNC("5LiMEPuW0DQ", LibNpEntitlementAccess::NpEntitlementAccessGetEntitlementKey);
	LIB_FUNC("y7JD1gfwyfA", NpEntitlementAccessGetGameTrialsFlag);
	LIB_FUNC("IQtb-TaIjSM", NpEntitlementAccessGenerateTransactionId);
	LIB_FUNC("uCZf2L27th8", NpEntitlementAccessRequestUnifiedEntitlementInfoList);
	LIB_FUNC("brbRxzr7qyI", NpEntitlementAccessRequestServiceEntitlementInfoList);
	LIB_FUNC("jautSRs4OdQ", NpEntitlementAccessRequestUnifiedEntitlementInfo);
	LIB_FUNC("eOxGbG3sPb0", NpEntitlementAccessRequestConsumeUnifiedEntitlement);
	LIB_FUNC("nAEqawEZG5s", NpEntitlementAccessPollUnifiedEntitlementInfoList);
	LIB_FUNC("aFv8qms6XTM", NpEntitlementAccessPollServiceEntitlementInfoList);
	LIB_FUNC("M4XlLFnzQaQ", NpEntitlementAccessPollUnifiedEntitlementInfo);
	LIB_FUNC("69u+XqsoNd0", NpEntitlementAccessPollConsumeEntitlement);
	LIB_FUNC("HFcQl9TMcFQ", NpEntitlementAccessAbortRequest);
	LIB_FUNC("Z0eQj8m7XA8", NpEntitlementAccessDeleteRequest);
}

} // namespace LibNpEntitlementAccess

namespace LibNpEntitlementAccessPft {

LIB_VERSION("NpEntitlementAccessPft", 1, "NpEntitlementAccess", 1, 1);

// Unknown function; return state 2 for the FC27 investigation.
static int KYTY_SYSV_ABI NpEntitlementAccessPftUnknowneDXKe9FndlE(uint32_t* state) {
	PRINT_NAME();

	if (state == nullptr) {
		return LibNpEntitlementAccess::NP_ENTITLEMENT_ACCESS_ERROR_PARAMETER;
	}

	*state = 2;
	return 0;
}

LIB_DEFINE(InitNet_1_NpEntitlementAccessPft) {
	LIB_FUNC("eDXKe9FndlE", NpEntitlementAccessPftUnknowneDXKe9FndlE);
}

} // namespace LibNpEntitlementAccessPft

} // namespace Libs
