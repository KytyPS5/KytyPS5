#include "common/abi.h"
#include "common/common.h"
#include "libs/errno.h"
#include "libs/libs.h"
#include "loader/symbolDatabase.h"

#include <algorithm>
#include <cstddef>

namespace Libs {

namespace LibNpCommerce {

LIB_NAME("NpCommerce", "NpCommerce");

constexpr int COMMERCE_STATUS_NONE          = 0;
constexpr int COMMERCE_STATUS_INITIALIZED   = 1;
constexpr int COMMERCE_STATUS_FINISHED      = 3;
constexpr int COMMERCE_RESULT_USER_CANCELED = 1;

constexpr int COMMERCE_ERROR_NOT_INITIALIZED     = static_cast<int>(0x80B80003u);
constexpr int COMMERCE_ERROR_ALREADY_INITIALIZED = static_cast<int>(0x80B80004u);
constexpr int COMMERCE_ERROR_NOT_FINISHED        = static_cast<int>(0x80B80005u);
constexpr int COMMERCE_ERROR_PARAM_INVALID       = static_cast<int>(0x80B8000Au);
constexpr int COMMERCE_ERROR_ARG_NULL            = static_cast<int>(0x80B8000Du);

struct CommerceDialogParam {
	uint8_t            base_param[48];
	int32_t            size;
	int32_t            user_id;
	int32_t            mode;
	uint32_t           service_label;
	const char* const* targets;
	uint32_t           num_targets;
	uint32_t           padding;
	uint64_t           features;
	void*              user_data;
	uint8_t            reserved[32];
};

struct CommerceDialogResult {
	int32_t result;
	bool    authorized;
	void*   user_data;
	uint8_t reserved[32];
};

static_assert(sizeof(CommerceDialogParam) == 128 && offsetof(CommerceDialogParam, user_data) == 88);
static_assert(sizeof(CommerceDialogResult) == 48 && offsetof(CommerceDialogResult, user_data) == 8);

static int                  g_commerce_status = COMMERCE_STATUS_NONE;
static CommerceDialogResult g_commerce_result {};

static int KYTY_SYSV_ABI NpCommerceDialogInitialize() {
	PRINT_NAME();
	if (g_commerce_status != COMMERCE_STATUS_NONE) {
		return COMMERCE_ERROR_ALREADY_INITIALIZED;
	}
	g_commerce_status = COMMERCE_STATUS_INITIALIZED;
	return OK;
}

static int KYTY_SYSV_ABI NpCommerceDialogTerminate() {
	PRINT_NAME();
	if (g_commerce_status == COMMERCE_STATUS_NONE) {
		return COMMERCE_ERROR_NOT_INITIALIZED;
	}
	g_commerce_status = COMMERCE_STATUS_NONE;
	g_commerce_result = {};
	return OK;
}

static int KYTY_SYSV_ABI NpCommerceDialogOpen(const CommerceDialogParam* param) {
	PRINT_NAME();
	if (g_commerce_status == COMMERCE_STATUS_NONE) {
		return COMMERCE_ERROR_NOT_INITIALIZED;
	}
	if (param == nullptr) {
		return COMMERCE_ERROR_ARG_NULL;
	}
	g_commerce_result = {};
	if (param->size == sizeof(CommerceDialogParam)) {
		g_commerce_result.user_data = param->user_data;
	}
	g_commerce_status = COMMERCE_STATUS_FINISHED;
	if (param->size != sizeof(CommerceDialogParam) || param->mode < 0 || param->mode > 5 ||
	    std::ranges::any_of(param->reserved, [](uint8_t value) { return value != 0; })) {
		g_commerce_result.result = COMMERCE_ERROR_PARAM_INVALID;
		return COMMERCE_ERROR_PARAM_INVALID;
	}
	g_commerce_result.result = COMMERCE_RESULT_USER_CANCELED;
	return OK;
}

static int KYTY_SYSV_ABI NpCommerceDialogGetResult(CommerceDialogResult* result) {
	PRINT_NAME();
	if (g_commerce_status == COMMERCE_STATUS_NONE) {
		return COMMERCE_ERROR_NOT_INITIALIZED;
	}
	if (result == nullptr) {
		return COMMERCE_ERROR_ARG_NULL;
	}
	if (g_commerce_status != COMMERCE_STATUS_FINISHED) {
		return COMMERCE_ERROR_NOT_FINISHED;
	}
	*result = g_commerce_result;
	return result->result;
}

static int KYTY_SYSV_ABI NpCommerceDialogUpdateStatus() {
	PRINT_NAME();

	return g_commerce_status;
}

LIB_DEFINE(InitNet_1_NpCommerce) {
	LIB_FUNC("0aR2aWmQal4", NpCommerceDialogInitialize);
	LIB_FUNC("m-I92Ab50W8", NpCommerceDialogTerminate);
	LIB_FUNC("DfSCDRA3EjY", NpCommerceDialogOpen);
	LIB_FUNC("r42bWcQbtZY", NpCommerceDialogGetResult);
	LIB_FUNC("LR5cwFMMCVE", NpCommerceDialogUpdateStatus);
}

} // namespace LibNpCommerce

} // namespace Libs
