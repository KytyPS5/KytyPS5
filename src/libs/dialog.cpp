#include "libs/dialog.h"

#include "common/logging/log.h"
#include "common/stringUtils.h"
#include "libs/errno.h"
#include "libs/libs.h"

#include <cstdio>
#include <cstring>

// NOLINTNEXTLINE(modernize-concat-nested-namespaces)
namespace Libs::Dialog {

namespace CommonDialog {

LIB_NAME("CommonDialog", "CommonDialog");

int KYTY_SYSV_ABI CommonDialogInitialize() {
	PRINT_NAME();

	return OK;
}

} // namespace CommonDialog

namespace ImeDialog {

LIB_NAME("ImeDialog", "ImeDialog");

constexpr int IME_STATUS_NONE = 0;

int KYTY_SYSV_ABI ImeDialogGetStatus() {
	PRINT_NAME();

	return IME_STATUS_NONE;
}

} // namespace ImeDialog

namespace LoginDialog {

LIB_NAME("LoginDialog", "LoginDialog");

constexpr int LOGIN_STATUS_NONE        = 0;
constexpr int LOGIN_STATUS_INITIALIZED = 1;
constexpr int LOGIN_STATUS_RUNNING     = 2;
constexpr int LOGIN_STATUS_FINISHED    = 3;
constexpr int LOGIN_RESULT_OK          = 0;
constexpr int LOGIN_MODE_ALL_USERS     = 0;
constexpr int LOGIN_MODE_NOT_LOGGED_IN = 1;
constexpr int LOGIN_USER_INVALID       = -1;
constexpr int LOGIN_USER_ID            = 1000;

constexpr int LOGIN_ERROR_NOT_INITIALIZED     = static_cast<int>(0x81340001u);
constexpr int LOGIN_ERROR_ALREADY_INITIALIZED = static_cast<int>(0x81340002u);
constexpr int LOGIN_ERROR_PARAM_INVALID       = static_cast<int>(0x81340003u);
constexpr int LOGIN_ERROR_INVALID_STATE       = static_cast<int>(0x81340005u);

struct LoginDialogResult {
	int32_t result;
	int32_t selected_user;
	int32_t reserved[2];
};

struct LoginDialogParam {
	int32_t size;
	int32_t mode;
	int32_t exclude_users_from_login_list[4];
	int32_t exclude_users_from_logout_list[4];
	int32_t initial_focus;
	int32_t reserved[5];
};

static_assert(sizeof(LoginDialogResult) == 16);
static_assert(sizeof(LoginDialogParam) == 64);

static int g_login_status        = LOGIN_STATUS_NONE;
static int g_login_selected_user = LOGIN_USER_ID;

int KYTY_SYSV_ABI LoginDialogInitialize() {
	PRINT_NAME();

	if (g_login_status != LOGIN_STATUS_NONE) {
		return LOGIN_ERROR_ALREADY_INITIALIZED;
	}

	g_login_status        = LOGIN_STATUS_INITIALIZED;
	g_login_selected_user = LOGIN_USER_ID;

	return OK;
}

int KYTY_SYSV_ABI LoginDialogTerminate() {
	PRINT_NAME();

	if (g_login_status == LOGIN_STATUS_NONE) {
		return LOGIN_ERROR_NOT_INITIALIZED;
	}

	g_login_status        = LOGIN_STATUS_NONE;
	g_login_selected_user = LOGIN_USER_ID;

	return OK;
}

int KYTY_SYSV_ABI LoginDialogOpen(const void* param) {
	PRINT_NAME();

	if (g_login_status != LOGIN_STATUS_INITIALIZED && g_login_status != LOGIN_STATUS_FINISHED) {
		return LOGIN_ERROR_INVALID_STATE;
	}
	if (param == nullptr) {
		return LOGIN_ERROR_PARAM_INVALID;
	}

	const auto* p = static_cast<const LoginDialogParam*>(param);
	if (p->size != sizeof(LoginDialogParam) ||
	    (p->mode != LOGIN_MODE_ALL_USERS && p->mode != LOGIN_MODE_NOT_LOGGED_IN)) {
		return LOGIN_ERROR_PARAM_INVALID;
	}

	g_login_selected_user =
	    p->initial_focus != LOGIN_USER_INVALID ? p->initial_focus : LOGIN_USER_ID;

	LOGF("\t size          = %d\n"
	     "\t mode          = %d\n"
	     "\t initial_focus = %d\n"
	     "\t selected_user = %d\n",
	     p->size, p->mode, p->initial_focus, g_login_selected_user);

	g_login_status = LOGIN_STATUS_RUNNING;

	return OK;
}

int KYTY_SYSV_ABI LoginDialogClose() {
	PRINT_NAME();

	if (g_login_status == LOGIN_STATUS_NONE) {
		return LOGIN_ERROR_NOT_INITIALIZED;
	}

	g_login_status = LOGIN_STATUS_FINISHED;

	return OK;
}

int KYTY_SYSV_ABI LoginDialogUpdateStatus() {
	PRINT_NAME();

	if (g_login_status == LOGIN_STATUS_RUNNING) {
		g_login_status = LOGIN_STATUS_FINISHED;
	}

	return g_login_status;
}

int KYTY_SYSV_ABI LoginDialogGetStatus() {
	PRINT_NAME();

	return g_login_status;
}

int KYTY_SYSV_ABI LoginDialogGetResult(void* result) {
	PRINT_NAME();

	if (g_login_status == LOGIN_STATUS_NONE) {
		return LOGIN_ERROR_NOT_INITIALIZED;
	}
	if (result == nullptr) {
		return LOGIN_ERROR_PARAM_INVALID;
	}

	auto* r          = static_cast<LoginDialogResult*>(result);
	r->result        = LOGIN_RESULT_OK;
	r->selected_user = g_login_selected_user;
	r->reserved[0]   = 0;
	r->reserved[1]   = 0;

	return OK;
}

void KYTY_SYSV_ABI LoginDialogParamInitialize(void* param) {
	PRINT_NAME();

	if (param == nullptr) {
		return;
	}

	auto* p = static_cast<LoginDialogParam*>(param);
	std::memset(p, 0, sizeof(LoginDialogParam));
	p->size = sizeof(LoginDialogParam);
	for (int i = 0; i < 4; i++) {
		p->exclude_users_from_login_list[i]  = LOGIN_USER_INVALID;
		p->exclude_users_from_logout_list[i] = LOGIN_USER_INVALID;
	}
	p->initial_focus = LOGIN_USER_INVALID;
}

} // namespace LoginDialog

namespace SaveDataDialog {

LIB_NAME("SaveDataDialog", "SaveDataDialog");

constexpr int SAVE_STATUS_NONE        = 0;
constexpr int SAVE_STATUS_INITIALIZED = 1;
constexpr int SAVE_STATUS_FINISHED    = 3;
constexpr int SAVE_RESULT_OK          = 0;
constexpr int SAVE_BUTTON_ID_OK       = 1;

struct SaveDataDialogParam {
	uint8_t  base_param[48];
	int32_t  size;
	int32_t  mode;
	int32_t  disp_type;
	uint32_t pad0;
	void*    anim_param;
	void*    items;
	void*    user_msg_param;
	void*    sys_msg_param;
	void*    error_code_param;
	void*    prog_bar_param;
	void*    user_data;
	void*    option_param;
	void*    wizard_param;
	uint8_t  reserved[16];
};

struct SaveDataDirName {
	char data[32];
};

struct SaveDataDialogItems {
	int32_t                user_id;
	int32_t                pad0;
	const void*            title_id;
	const SaveDataDirName* dir_names;
	uint32_t               dir_names_num;
};

struct SaveDataDialogResult {
	int32_t  mode;
	int32_t  result;
	int32_t  button_id;
	uint32_t pad0;
	void*    dir_name;
	void*    param;
	void*    user_data;
	uint8_t  reserved[32];
};

static int   g_save_status    = SAVE_STATUS_NONE;
static int   g_save_mode      = 0;
static void* g_save_user_data = nullptr;
static char  g_save_dir_name[sizeof(SaveDataDirName::data)] {};

int KYTY_SYSV_ABI SaveDataDialogInitialize() {
	PRINT_NAME();

	g_save_status      = SAVE_STATUS_INITIALIZED;
	g_save_mode        = 0;
	g_save_user_data   = nullptr;
	g_save_dir_name[0] = '\0';

	return OK;
}

int KYTY_SYSV_ABI SaveDataDialogGetStatus() {
	PRINT_NAME();

	return g_save_status;
}

int KYTY_SYSV_ABI SaveDataDialogUpdateStatus() {
	PRINT_NAME();

	return g_save_status;
}

int KYTY_SYSV_ABI SaveDataDialogGetResult(void* result) {
	PRINT_NAME();

	if (result != nullptr) {
		auto* r      = static_cast<SaveDataDialogResult*>(result);
		r->mode      = g_save_mode;
		r->result    = SAVE_RESULT_OK;
		r->button_id = SAVE_BUTTON_ID_OK;
		r->user_data = g_save_user_data;
		if (r->dir_name != nullptr && g_save_dir_name[0] != '\0') {
			std::snprintf(static_cast<SaveDataDirName*>(r->dir_name)->data,
			              sizeof(SaveDataDirName::data), "%s", g_save_dir_name);
		}
	}

	return OK;
}

int KYTY_SYSV_ABI SaveDataDialogOpen(const void* param) {
	PRINT_NAME();

	const auto* p = static_cast<const SaveDataDialogParam*>(param);
	if (p != nullptr) {
		g_save_mode        = p->mode;
		g_save_user_data   = p->user_data;
		g_save_dir_name[0] = '\0';
		LOGF("\t size           = %d\n"
		     "\t mode           = %d\n"
		     "\t disp_type      = %d\n"
		     "\t items          = 0x%016" PRIx64 "\n"
		     "\t user_msg_param = 0x%016" PRIx64 "\n"
		     "\t sys_msg_param  = 0x%016" PRIx64 "\n"
		     "\t prog_bar_param = 0x%016" PRIx64 "\n"
		     "\t user_data      = 0x%016" PRIx64 "\n",
		     p->size, p->mode, p->disp_type, reinterpret_cast<uint64_t>(p->items),
		     reinterpret_cast<uint64_t>(p->user_msg_param),
		     reinterpret_cast<uint64_t>(p->sys_msg_param),
		     reinterpret_cast<uint64_t>(p->prog_bar_param),
		     reinterpret_cast<uint64_t>(p->user_data));

		const auto* items = static_cast<const SaveDataDialogItems*>(p->items);
		if (items != nullptr && items->dir_names != nullptr) {
			for (uint32_t i = 0; i < items->dir_names_num; i++) {
				const auto* name = items->dir_names[i].data;
				if (name[0] != '\0') {
					std::snprintf(g_save_dir_name, sizeof(g_save_dir_name), "%s", name);
					break;
				}
			}
		}
	}

	g_save_status = SAVE_STATUS_FINISHED;

	return OK;
}

int KYTY_SYSV_ABI SaveDataDialogClose(const void* close_param) {
	PRINT_NAME();

	LOGF("\t close_param = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(close_param));

	g_save_status = SAVE_STATUS_FINISHED;

	return OK;
}

int KYTY_SYSV_ABI SaveDataDialogIsReadyToDisplay() {
	PRINT_NAME();

	return 1;
}

int KYTY_SYSV_ABI SaveDataDialogTerminate() {
	PRINT_NAME();

	g_save_status      = SAVE_STATUS_NONE;
	g_save_mode        = 0;
	g_save_user_data   = nullptr;
	g_save_dir_name[0] = '\0';

	return 0;
}

int KYTY_SYSV_ABI SaveDataDialogProgressBarInc(int target, uint32_t delta) {
	PRINT_NAME();

	LOGF("\t target = %d\n"
	     "\t delta  = %u\n",
	     target, delta);

	return OK;
}

int KYTY_SYSV_ABI SaveDataDialogProgressBarSetValue(int target, uint32_t rate) {
	PRINT_NAME();

	LOGF("\t target = %d\n"
	     "\t rate   = %u\n",
	     target, rate);

	return OK;
}

} // namespace SaveDataDialog

namespace MsgDialog {

LIB_NAME("MsgDialog.native", "MsgDialog");

constexpr int STATUS_NONE        = 0;
constexpr int STATUS_INITIALIZED = 1;
constexpr int STATUS_FINISHED    = 3;
constexpr int RESULT_OK          = 0;
constexpr int BUTTON_ID_OK       = 1;

struct MsgDialogParam {
	uint8_t  base_param[48];
	uint64_t size;
	int32_t  mode;
	uint32_t pad0;
	void*    user_msg_param;
	void*    prog_bar_param;
	void*    sys_msg_param;
	int32_t  user_id;
	uint8_t  reserved[40];
	uint32_t pad1;
};

struct MsgDialogResult {
	int32_t mode;
	int32_t result;
	int32_t button_id;
	uint8_t reserved[32];
};

static int g_status = STATUS_NONE;
static int g_mode   = 0;

int KYTY_SYSV_ABI MsgDialogInitialize() {
	PRINT_NAME();

	g_status = STATUS_INITIALIZED;
	g_mode   = 0;

	return OK;
}

int KYTY_SYSV_ABI MsgDialogOpen(const void* param) {
	PRINT_NAME();

	const auto* p = static_cast<const MsgDialogParam*>(param);
	if (p != nullptr) {
		g_mode = p->mode;
		LOGF("\t size           = 0x%016" PRIx64 "\n"
		     "\t mode           = %d\n"
		     "\t user_msg_param = 0x%016" PRIx64 "\n"
		     "\t prog_bar_param = 0x%016" PRIx64 "\n"
		     "\t sys_msg_param  = 0x%016" PRIx64 "\n"
		     "\t user_id        = %d\n",
		     p->size, p->mode, reinterpret_cast<uint64_t>(p->user_msg_param),
		     reinterpret_cast<uint64_t>(p->prog_bar_param),
		     reinterpret_cast<uint64_t>(p->sys_msg_param), p->user_id);
	}

	g_status = STATUS_FINISHED;

	return OK;
}

int KYTY_SYSV_ABI MsgDialogUpdateStatus() {
	PRINT_NAME();

	return g_status;
}

int KYTY_SYSV_ABI MsgDialogGetStatus() {
	PRINT_NAME();

	return g_status;
}

int KYTY_SYSV_ABI MsgDialogGetResult(void* result) {
	PRINT_NAME();

	if (result != nullptr) {
		auto* r      = static_cast<MsgDialogResult*>(result);
		r->mode      = g_mode;
		r->result    = RESULT_OK;
		r->button_id = BUTTON_ID_OK;
	}

	return OK;
}

int KYTY_SYSV_ABI MsgDialogTerminate() {
	PRINT_NAME();

	g_status = STATUS_NONE;
	g_mode   = 0;

	return OK;
}

int KYTY_SYSV_ABI MsgDialogClose() {
	PRINT_NAME();

	g_status = STATUS_FINISHED;

	return OK;
}

int KYTY_SYSV_ABI MsgDialogProgressBarInc(int target, uint32_t delta) {
	PRINT_NAME();

	LOGF("\t target = %d\n"
	     "\t delta  = %u\n",
	     target, delta);

	return OK;
}

int KYTY_SYSV_ABI MsgDialogProgressBarSetValue(int target, uint32_t rate) {
	PRINT_NAME();

	LOGF("\t target = %d\n"
	     "\t rate   = %u\n",
	     target, rate);

	return OK;
}

int KYTY_SYSV_ABI MsgDialogProgressBarSetMsg(int target, const char* msg) {
	PRINT_NAME();

	LOGF("\t target = %d\n"
	     "\t msg    = 0x%016" PRIx64 "\n",
	     target, reinterpret_cast<uint64_t>(msg));

	return OK;
}

} // namespace MsgDialog

namespace ErrorDialog {

LIB_NAME("ErrorDialog", "ErrorDialog");

constexpr int STATUS_NONE        = 0;
constexpr int STATUS_INITIALIZED = 1;
constexpr int STATUS_RUNNING     = 2;
constexpr int STATUS_FINISHED    = 3;

constexpr int ERROR_NOT_INITIALIZED     = static_cast<int>(0x80ed0001);
constexpr int ERROR_ALREADY_INITIALIZED = static_cast<int>(0x80ed0002);
constexpr int ERROR_PARAM_INVALID       = static_cast<int>(0x80ed0003);
constexpr int ERROR_INVALID_STATE       = static_cast<int>(0x80ed0005);

struct ErrorDialogParam {
	int32_t size;
	int32_t error_code;
	int32_t user_id;
	int32_t reserved;
};

static int g_error_status = STATUS_NONE;

int KYTY_SYSV_ABI ErrorDialogInitialize() {
	PRINT_NAME();

	if (g_error_status != STATUS_NONE) {
		return ERROR_ALREADY_INITIALIZED;
	}

	g_error_status = STATUS_INITIALIZED;

	return OK;
}

int KYTY_SYSV_ABI ErrorDialogOpen(const void* param) {
	PRINT_NAME();

	if (g_error_status != STATUS_INITIALIZED && g_error_status != STATUS_FINISHED) {
		return ERROR_INVALID_STATE;
	}
	if (param == nullptr) {
		return ERROR_PARAM_INVALID;
	}

	const auto* p = static_cast<const ErrorDialogParam*>(param);
	if (p->size != sizeof(ErrorDialogParam)) {
		return ERROR_PARAM_INVALID;
	}

	LOGF("\t size       = %d\n"
	     "\t error_code = 0x%08" PRIx32 "\n"
	     "\t user_id    = %d\n",
	     p->size, static_cast<uint32_t>(p->error_code), p->user_id);

	g_error_status = STATUS_RUNNING;

	return OK;
}

int KYTY_SYSV_ABI ErrorDialogClose() {
	PRINT_NAME();

	if (g_error_status != STATUS_RUNNING) {
		return ERROR_INVALID_STATE;
	}

	g_error_status = STATUS_FINISHED;

	return OK;
}

int KYTY_SYSV_ABI ErrorDialogTerminate() {
	PRINT_NAME();

	if (g_error_status == STATUS_NONE) {
		return ERROR_NOT_INITIALIZED;
	}

	g_error_status = STATUS_NONE;

	return OK;
}

int KYTY_SYSV_ABI ErrorDialogUpdateStatus() {
	PRINT_NAME();

	if (g_error_status == STATUS_RUNNING) {
		g_error_status = STATUS_FINISHED;
	}

	return g_error_status;
}

int KYTY_SYSV_ABI ErrorDialogGetStatus() {
	PRINT_NAME();

	return g_error_status;
}

} // namespace ErrorDialog

} // namespace Libs::Dialog
