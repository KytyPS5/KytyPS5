#include "common/abi.h"
#include "common/assert.h"
#include "common/common.h"
#include "common/file.h"
#include "common/logging/log.h"
#include "common/stringUtils.h"
#include "kernel/fileSystem.h"
#include "libs/errno.h"
#include "libs/libs.h"
#include "loader/symbolDatabase.h"
#include "loader/systemContent.h"

#include <algorithm>
#include <cstring>
#include <deque>
#include <vector>

namespace Libs {

LIB_VERSION("SaveData", 1, "SaveData", 1, 1);

namespace SaveData {

// TODO(): specify dir at launcher
static constexpr char     SAVE_DATA_DIR[]      = "_SaveData";
static constexpr char     SAVE_DATA_POINT[]    = "/savedata0";
static constexpr uint64_t SAVE_DATA_BLOCKS_MAX = 16384;

struct SceSaveDataDirName {
	char data[32];
};

struct SceSaveDataTitleId {
	char data[10];
	char padding[6];
};

struct SceSaveDataFingerprint {
	char data[65];
	char padding[15];
};

struct SaveDataParam;

enum class SaveDataSortKey : uint32_t {
	DirName    = 0,
	UserParam  = 1,
	Blocks     = 2,
	Mtime      = 3,
	FreeBlocks = 5,
};

enum class SaveDataSortOrder : uint32_t {
	Ascent  = 0,
	Descent = 1,
};

struct SaveDataDirNameSearchCond {
	int32_t                   user_id;
	int32_t                   pad;
	const SceSaveDataTitleId* title_id;
	const SceSaveDataDirName* dir_name;
	SaveDataSortKey           key;
	SaveDataSortOrder         order;
	uint8_t                   reserved[32];
};

struct SaveDataSearchInfo {
	uint64_t blocks;
	uint64_t free_blocks;
	uint8_t  reserved[32];
};

struct SaveDataDirNameSearchResult {
	uint32_t            hit_num;
	int32_t             pad;
	SceSaveDataDirName* dir_names;
	uint32_t            dir_names_num;
	uint32_t            set_num;
	SaveDataParam*      params;
	SaveDataSearchInfo* infos;
	uint8_t             reserved[12];
	int32_t             pad2;
};

struct SaveDataMountPoint {
	char data[16];
};

struct SaveDataMount3 {
	int                       user_id;
	int                       pad;
	const SceSaveDataDirName* dir_name;
	uint64_t                  blocks;
	uint64_t                  system_blocks;
	uint32_t                  mount_mode;
	int                       pad2;
	int32_t                   resource;
	uint8_t                   reserved[32];
};

struct SaveDataMountResult {
	SaveDataMountPoint mount_point;
	uint64_t           required_blocks;
	uint32_t           unused;
	uint32_t           mount_status;
	uint8_t            reserved[28];
	int                pad;
};

struct SaveDataParam {
	char     title[128];
	char     sub_title[128];
	char     detail[1024];
	uint32_t user_param;
	int      pad;
	int64_t  mtime;
	uint8_t  reserved[32];
};

struct SaveDataMountInfo {
	uint64_t blocks;
	uint64_t free_blocks;
	uint8_t  reserved[32];
};

struct SaveDataIcon {
	void*   buf;
	size_t  buf_size;
	size_t  data_size;
	uint8_t reserved[32];
};

struct SaveDataMemoryData {
	void*   buf;
	size_t  buf_size;
	int64_t offset;
	uint8_t reserved[40];
};

struct SaveDataMemoryGet2 {
	int32_t             user_id;
	uint8_t             padding[4];
	SaveDataMemoryData* data;
	SaveDataParam*      param;
	SaveDataIcon*       icon;
	uint32_t            slot_id;
	uint8_t             reserved[28];
};

struct SaveDataMemorySetup2 {
	uint32_t             option;
	int32_t              user_id;
	size_t               memory_size;
	size_t               icon_memory_size;
	const SaveDataParam* init_param;
	const SaveDataIcon*  init_icon;
	uint32_t             slot_id;
	uint8_t              reserved[20];
};

struct SaveDataMemorySetupResult {
	size_t  existed_memory_size;
	uint8_t reserved[16];
};

struct SaveDataMemorySet2 {
	int32_t                   user_id;
	uint8_t                   padding[4];
	const SaveDataMemoryData* data;
	const SaveDataParam*      param;
	const SaveDataIcon*       icon;
	uint32_t                  data_num;
	uint32_t                  slot_id;
	uint8_t                   reserved[24];
};

struct SaveDataTransferringMount {
	int32_t                   user_id;
	const SceSaveDataTitleId* title_id;
	const SceSaveDataDirName* dir_name;
	const void*               fingerprint;
	uint8_t                   reserved[32];
};

struct SaveDataPrepareParam {
	int32_t  resource;
	uint32_t prepare_mode;
	uint8_t  reserved[32];
};

struct SaveDataCommitParam {
	int32_t  resource;
	uint32_t commit_mode;
	uint8_t  reserved[32];
};

struct SaveDataDelete {
	int32_t                   user_id;
	int32_t                   pad;
	const SceSaveDataTitleId* title_id;
	const SceSaveDataDirName* dir_name;
	uint32_t                  unused;
	uint8_t                   reserved[32];
	int32_t                   pad2;
};

struct SaveDataEvent {
	uint32_t           type;
	int32_t            error_code;
	int32_t            user_id;
	uint8_t            padding[4];
	SceSaveDataTitleId title_id;
	SceSaveDataDirName dir_name;
	uint8_t            reserved[40];
};

struct SaveDataBackup {
	int32_t                   user_id;
	int32_t                   pad;
	const SceSaveDataTitleId* title_id;
	const SceSaveDataDirName* dir_name;
	const void*               fingerprint;
	uint8_t                   reserved[32];
};

static constexpr uint32_t SAVE_DATA_UMOUNT_MODE_BACKUP_ASYNC = (1u << 16u);
static constexpr uint32_t SAVE_DATA_COMMIT_MODE_BACKUP_ASYNC = 1u;

static constexpr uint32_t SAVE_DATA_EVENT_TYPE_UMOUNT_BACKUP_END = 1u;
static constexpr uint32_t SAVE_DATA_EVENT_TYPE_BACKUP_END        = 2u;
static constexpr uint32_t SAVE_DATA_EVENT_TYPE_COMMIT_BACKUP_END = 4u;

static std::vector<uint8_t>      g_save_data_memory(0x10000);
static int32_t                   g_next_transaction_resource = 1;
static std::deque<SaveDataEvent> g_save_data_events;

static std::string get_title_id() {
	std::string title_id;
	if (!Loader::SystemContentParamSfoGetString("TITLE_ID", &title_id) || title_id.empty()) {
		title_id = "UNKNOWN";
	}

	return title_id;
}

static void queue_save_data_event(uint32_t type, int32_t user_id,
                                  const SceSaveDataTitleId* title_id,
                                  const SceSaveDataDirName* dir_name, int32_t error_code = OK) {
	SaveDataEvent event = {};
	event.type          = type;
	event.error_code    = error_code;
	event.user_id       = user_id;
	if (title_id != nullptr) {
		std::memcpy(&event.title_id, title_id, sizeof(event.title_id));
	}
	if (dir_name != nullptr) {
		std::memcpy(&event.dir_name, dir_name, sizeof(event.dir_name));
	}

	g_save_data_events.push_back(event);
	if (g_save_data_events.size() > 20) {
		g_save_data_events.pop_front();
	}
}

static bool dir_name_match(const char* str, const char* pattern) {
	while (*str != '\0' && *pattern != '\0') {
		if (*pattern == '%') {
			for (const char* s = str;; s++) {
				if (dir_name_match(s, pattern + 1)) {
					return true;
				}
				if (*s == '\0') {
					break;
				}
			}
			return false;
		}
		if (*pattern == '_') {
			str++;
			pattern++;
			continue;
		}
		if (*pattern != *str) {
			return false;
		}
		str++;
		pattern++;
	}
	return *str == '\0' && *pattern == '\0';
}

int KYTY_SYSV_ABI SaveDataInitialize3(const void* /*init*/) {
	PRINT_NAME();

	// EXIT_NOT_IMPLEMENTED(init != nullptr);

	return OK;
}

int KYTY_SYSV_ABI SaveDataTerminate() {
	PRINT_NAME();

	g_save_data_events.clear();

	return OK;
}

int KYTY_SYSV_ABI SaveDataCreateTransactionResource(uint32_t size) {
	PRINT_NAME();

	LOGF("\t size = %" PRIu32 "\n", size);

	return g_next_transaction_resource++;
}

int KYTY_SYSV_ABI SaveDataDeleteTransactionResource(int32_t resource) {
	PRINT_NAME();

	LOGF("\t resource = %" PRId32 "\n", resource);

	return OK;
}

int KYTY_SYSV_ABI SaveDataDirNameSearch(const SaveDataDirNameSearchCond* cond,
                                        SaveDataDirNameSearchResult*     result) {
	PRINT_NAME();

	if (cond == nullptr || result == nullptr ||
	    static_cast<uint32_t>(cond->key) > static_cast<uint32_t>(SaveDataSortKey::FreeBlocks) ||
	    static_cast<uint32_t>(cond->order) > static_cast<uint32_t>(SaveDataSortOrder::Descent)) {
		return SAVE_DATA_ERROR_PARAMETER;
	}

	LOGF("\t user_id       = %d\n"
	     "\t title_id      = %s\n"
	     "\t dir_name      = %s\n"
	     "\t key           = %" PRIu32 "\n"
	     "\t order         = %" PRIu32 "\n"
	     "\t dir_names_num = %" PRIu32 "\n",
	     cond->user_id, cond->title_id != nullptr ? cond->title_id->data : "<default>",
	     cond->dir_name != nullptr ? cond->dir_name->data : "<all>",
	     static_cast<uint32_t>(cond->key), static_cast<uint32_t>(cond->order),
	     result->dir_names_num);

	result->hit_num = 0;
	result->pad     = 0;
	result->set_num = 0;
	std::memset(result->reserved, 0, sizeof(result->reserved));
	result->pad2 = 0;

	std::vector<std::string> dir_list;
	std::string              root =
	    Common::FixDirectorySlash((std::string(SAVE_DATA_DIR) + "/" + get_title_id()));

	if (Common::File::IsDirectoryExisting(root)) {
		for (const auto& entry: Common::File::GetDirEntries(root)) {
			if (!entry.is_file && !Common::StartsWith(entry.name, "sce_")) {
				if (cond->dir_name == nullptr || cond->dir_name->data[0] == '\0' ||
				    dir_name_match(Common::ToLower(entry.name).c_str(),
				                   Common::ToLower(std::string(cond->dir_name->data)).c_str())) {
					dir_list.push_back(entry.name);
				}
			}
		}
	}

	std::sort(dir_list.begin(), dir_list.end(), [](const std::string& a, const std::string& b) {
		return std::strcmp(a.c_str(), b.c_str()) < 0;
	});

	if (cond->order == SaveDataSortOrder::Descent) {
		std::vector<std::string> reversed;
		for (size_t i = dir_list.size(); i > 0; i--) {
			reversed.push_back(dir_list[i - 1]);
		}
		dir_list = reversed;
	}

	auto max_count =
	    (result->dir_names_num < dir_list.size() ? result->dir_names_num : dir_list.size());

	result->hit_num = static_cast<uint32_t>(dir_list.size());
	result->set_num = static_cast<uint32_t>(max_count);

	for (size_t i = 0; i < max_count; i++) {
		std::snprintf(result->dir_names[i].data, sizeof(result->dir_names[i].data), "%s",
		              dir_list[i].c_str());
		if (result->params != nullptr) {
			result->params[i] = {};
		}
		if (result->infos != nullptr) {
			result->infos[i]             = {};
			result->infos[i].blocks      = SAVE_DATA_BLOCKS_MAX;
			result->infos[i].free_blocks = SAVE_DATA_BLOCKS_MAX;
		}
	}

	return OK;
}

int KYTY_SYSV_ABI SaveDataMount3(const SaveDataMount3* mount, SaveDataMountResult* mount_result) {
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(mount == nullptr);
	EXIT_NOT_IMPLEMENTED(mount_result == nullptr);
	EXIT_NOT_IMPLEMENTED(mount->dir_name == nullptr);

	LOGF("\t user_id       = %d\n"
	     "\t dir_name      = %s\n"
	     "\t blocks        = %" PRIu64 "\n"
	     "\t system_blocks = %" PRIu64 "\n"
	     "\t mount_mode    = %" PRIu32 "\n"
	     "\t resource      = %" PRId32 "\n",
	     mount->user_id, mount->dir_name->data, mount->blocks, mount->system_blocks,
	     mount->mount_mode, mount->resource);

	*mount_result = {};

	std::string mount_dir   = std::string(SAVE_DATA_DIR) + "/" + get_title_id() + "/" +
	                          std::string(mount->dir_name->data);
	std::string mount_point = SAVE_DATA_POINT;
	bool        create      = ((mount->mount_mode & 4u) != 0);
	bool        create2     = ((mount->mount_mode & 32u) != 0);
	bool        open        = (!create && !create2 && ((mount->mount_mode & 3u) != 0));

	if (!create && !create2 && !open) {
		EXIT("unknown mount mode: %u", mount->mount_mode);
	}

	if (open && !Common::File::IsDirectoryExisting(mount_dir)) {
		return SAVE_DATA_ERROR_NOT_FOUND;
	}

	if (create && Common::File::IsDirectoryExisting(mount_dir)) {
		return SAVE_DATA_ERROR_EXISTS;
	}

	bool created = false;
	if ((create || create2) && !Common::File::IsDirectoryExisting(mount_dir)) {
		Common::File::CreateDirectories(mount_dir);
		created = true;

		EXIT_NOT_IMPLEMENTED((!Common::File::IsDirectoryExisting(mount_dir)));
	}

	LibKernel::FileSystem::Mount(mount_dir, mount_point);

	int s = snprintf(mount_result->mount_point.data, 16, "%s", mount_point.c_str());

	EXIT_NOT_IMPLEMENTED(s >= 16);

	mount_result->required_blocks = 0;
	mount_result->mount_status    = (created ? 1 : 0);

	return OK;
}

int KYTY_SYSV_ABI SaveDataSetupSaveDataMemory2(const SaveDataMemorySetup2* setup_param,
                                               SaveDataMemorySetupResult*  result) {
	PRINT_NAME();

	if (setup_param == nullptr) {
		return SAVE_DATA_ERROR_PARAMETER;
	}

	LOGF("\t option           = 0x%08" PRIx32 "\n"
	     "\t user_id          = %" PRId32 "\n"
	     "\t memory_size      = %" PRIu64 "\n"
	     "\t icon_memory_size = %" PRIu64 "\n"
	     "\t slot_id          = %" PRIu32 "\n",
	     setup_param->option, setup_param->user_id, static_cast<uint64_t>(setup_param->memory_size),
	     static_cast<uint64_t>(setup_param->icon_memory_size), setup_param->slot_id);

	if (setup_param->memory_size > g_save_data_memory.size()) {
		g_save_data_memory.resize(setup_param->memory_size);
	}

	if (result != nullptr) {
		*result                     = {};
		result->existed_memory_size = g_save_data_memory.size();
	}

	return OK;
}

int KYTY_SYSV_ABI SaveDataGetSaveDataMemory2(SaveDataMemoryGet2* get_param) {
	PRINT_NAME();

	if (get_param == nullptr) {
		return SAVE_DATA_ERROR_PARAMETER;
	}

	LOGF("\t user_id  = %" PRId32 "\n"
	     "\t data     = 0x%016" PRIx64 "\n"
	     "\t param    = 0x%016" PRIx64 "\n"
	     "\t icon     = 0x%016" PRIx64 "\n"
	     "\t slot_id  = %" PRIu32 "\n",
	     get_param->user_id, reinterpret_cast<uint64_t>(get_param->data),
	     reinterpret_cast<uint64_t>(get_param->param), reinterpret_cast<uint64_t>(get_param->icon),
	     get_param->slot_id);

	if (get_param->data != nullptr) {
		auto* data = get_param->data;
		if (data->buf == nullptr || data->offset < 0) {
			return SAVE_DATA_ERROR_PARAMETER;
		}

		const auto offset = static_cast<size_t>(data->offset);
		if (offset + data->buf_size > g_save_data_memory.size()) {
			g_save_data_memory.resize(offset + data->buf_size);
		}
		std::memcpy(data->buf, g_save_data_memory.data() + offset, data->buf_size);
	}
	if (get_param->param != nullptr) {
		std::memset(get_param->param, 0, sizeof(*get_param->param));
	}
	if (get_param->icon != nullptr) {
		get_param->icon->data_size = 0;
	}

	return OK;
}

int KYTY_SYSV_ABI SaveDataSetSaveDataMemory2(const SaveDataMemorySet2* set_param) {
	PRINT_NAME();

	if (set_param == nullptr) {
		return SAVE_DATA_ERROR_PARAMETER;
	}

	LOGF("\t user_id  = %" PRId32 "\n"
	     "\t data     = 0x%016" PRIx64 "\n"
	     "\t param    = 0x%016" PRIx64 "\n"
	     "\t icon     = 0x%016" PRIx64 "\n"
	     "\t data_num = %" PRIu32 "\n"
	     "\t slot_id  = %" PRIu32 "\n",
	     set_param->user_id, reinterpret_cast<uint64_t>(set_param->data),
	     reinterpret_cast<uint64_t>(set_param->param), reinterpret_cast<uint64_t>(set_param->icon),
	     set_param->data_num, set_param->slot_id);

	const uint32_t data_num = (set_param->data_num == 0 ? 1 : set_param->data_num);
	if (set_param->data != nullptr) {
		for (uint32_t i = 0; i < data_num; i++) {
			const auto& data = set_param->data[i];
			if (data.buf == nullptr || data.offset < 0) {
				return SAVE_DATA_ERROR_PARAMETER;
			}

			const auto offset = static_cast<size_t>(data.offset);
			if (offset + data.buf_size > g_save_data_memory.size()) {
				g_save_data_memory.resize(offset + data.buf_size);
			}
			std::memcpy(g_save_data_memory.data() + offset, data.buf, data.buf_size);
		}
	}

	return OK;
}

int KYTY_SYSV_ABI SaveDataTransferringMount(const SaveDataTransferringMount* mount,
                                            SaveDataMountResult*             mount_result) {
	PRINT_NAME();

	if (mount == nullptr || mount_result == nullptr || mount->dir_name == nullptr) {
		return SAVE_DATA_ERROR_PARAMETER;
	}

	LOGF("\t user_id  = %" PRId32 "\n"
	     "\t title_id = %s\n"
	     "\t dir_name = %s\n",
	     mount->user_id, mount->title_id != nullptr ? mount->title_id->data : "<null>",
	     mount->dir_name->data);

	*mount_result = {};

	std::string mount_dir   = std::string(SAVE_DATA_DIR) + "/" + get_title_id() + "/" +
	                          std::string(mount->dir_name->data);
	std::string mount_point = SAVE_DATA_POINT;

	if (!Common::File::IsDirectoryExisting(mount_dir)) {
		Common::File::CreateDirectories(mount_dir);
	}

	LibKernel::FileSystem::Mount(mount_dir, mount_point);

	int s = snprintf(mount_result->mount_point.data, 16, "%s", mount_point.c_str());

	EXIT_NOT_IMPLEMENTED(s >= 16);

	mount_result->required_blocks = 0;
	mount_result->mount_status    = 1;

	return OK;
}

int KYTY_SYSV_ABI SaveDataUmount2(uint32_t mode, const SaveDataMountPoint* mount_point) {
	PRINT_NAME();

	if (mount_point == nullptr) {
		return SAVE_DATA_ERROR_PARAMETER;
	}

	LOGF("\t mode        = %" PRIu32 "\n"
	     "\t mount_point = %s\n",
	     mode, mount_point->data);

	if ((mode & SAVE_DATA_UMOUNT_MODE_BACKUP_ASYNC) != 0) {
		queue_save_data_event(SAVE_DATA_EVENT_TYPE_UMOUNT_BACKUP_END, 0, nullptr, nullptr);
	}

	LibKernel::FileSystem::Umount(std::string(mount_point->data));

	return OK;
}

int KYTY_SYSV_ABI SaveDataPrepare(const SaveDataMountPoint*   mount_point,
                                  const SaveDataPrepareParam* param) {
	PRINT_NAME();

	if (mount_point == nullptr || param == nullptr) {
		return SAVE_DATA_ERROR_PARAMETER;
	}

	LOGF("\t mount_point  = %s\n"
	     "\t resource     = %" PRId32 "\n"
	     "\t prepare_mode = %" PRIu32 "\n",
	     mount_point->data, param->resource, param->prepare_mode);

	return OK;
}

int KYTY_SYSV_ABI SaveDataCommit(const SaveDataCommitParam* param) {
	PRINT_NAME();

	if (param == nullptr) {
		return SAVE_DATA_ERROR_PARAMETER;
	}

	LOGF("\t resource    = %" PRId32 "\n"
	     "\t commit_mode = %" PRIu32 "\n",
	     param->resource, param->commit_mode);

	if ((param->commit_mode & SAVE_DATA_COMMIT_MODE_BACKUP_ASYNC) != 0) {
		queue_save_data_event(SAVE_DATA_EVENT_TYPE_COMMIT_BACKUP_END, 0, nullptr, nullptr);
	}

	return OK;
}

int KYTY_SYSV_ABI SaveDataDelete(const SaveDataDelete* del) {
	PRINT_NAME();

	if (del == nullptr || del->dir_name == nullptr) {
		return SAVE_DATA_ERROR_PARAMETER;
	}

	LOGF("\t user_id  = %" PRId32 "\n"
	     "\t title_id = %s\n"
	     "\t dir_name = %s\n",
	     del->user_id, del->title_id != nullptr ? del->title_id->data : "<default>",
	     del->dir_name->data);

	std::string dir =
	    std::string(SAVE_DATA_DIR) + "/" + get_title_id() + "/" + std::string(del->dir_name->data);
	if (Common::File::IsDirectoryExisting(dir)) {
		Common::File::DeleteDirectory(dir);
	}

	return OK;
}

int KYTY_SYSV_ABI SaveDataGetParam(const SaveDataMountPoint* mount_point, uint32_t param_type,
                                   void* param_buf, size_t param_buf_size, size_t* got_size) {
	PRINT_NAME();

	if (mount_point == nullptr || param_buf == nullptr) {
		return SAVE_DATA_ERROR_PARAMETER;
	}

	LOGF("\t mount_point    = %s\n"
	     "\t param_type     = %u\n"
	     "\t param_buf_size = %" PRIu64 "\n",
	     mount_point->data, param_type, param_buf_size);

	if (param_type == 0) {
		if (param_buf_size < sizeof(SaveDataParam)) {
			return SAVE_DATA_ERROR_PARAMETER;
		}
		std::memset(param_buf, 0, sizeof(SaveDataParam));
		if (got_size != nullptr) {
			*got_size = sizeof(SaveDataParam);
		}
		return OK;
	}

	std::memset(param_buf, 0, param_buf_size);
	if (got_size != nullptr) {
		*got_size = param_buf_size;
	}

	return OK;
}

int KYTY_SYSV_ABI SaveDataLoadIcon(const SaveDataMountPoint* mount_point, SaveDataIcon* icon) {
	PRINT_NAME();

	if (mount_point == nullptr || icon == nullptr) {
		return SAVE_DATA_ERROR_PARAMETER;
	}

	LOGF("\t mount_point = %s\n"
	     "\t buf         = %016" PRIx64 "\n"
	     "\t buf_size    = %" PRIu64 "\n",
	     mount_point->data, reinterpret_cast<uint64_t>(icon->buf), icon->buf_size);

	icon->data_size = 0;

	return OK;
}

int KYTY_SYSV_ABI SaveDataSaveIconByPath(const SaveDataMountPoint* mount_point, const char* path) {
	PRINT_NAME();

	if (mount_point == nullptr || path == nullptr) {
		return SAVE_DATA_ERROR_PARAMETER;
	}

	LOGF("\t mount_point = %s\n"
	     "\t path        = %s\n",
	     mount_point->data, path);

	return OK;
}

int KYTY_SYSV_ABI SaveDataSyncSaveDataMemory(const void* sync_param) {
	PRINT_NAME();

	LOGF("\t sync_param = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(sync_param));

	return OK;
}

int KYTY_SYSV_ABI SaveDataGetEventResult(const void* event_param, SaveDataEvent* event) {
	PRINT_NAME();

	LOGF("\t event_param = 0x%016" PRIx64 "\n"
	     "\t event       = 0x%016" PRIx64 "\n",
	     reinterpret_cast<uint64_t>(event_param), reinterpret_cast<uint64_t>(event));

	if (event == nullptr) {
		return SAVE_DATA_ERROR_PARAMETER;
	}

	if (g_save_data_events.empty()) {
		return SAVE_DATA_ERROR_NOT_FOUND;
	}

	*event = g_save_data_events.front();
	g_save_data_events.pop_front();

	LOGF("\t type       = %" PRIu32 "\n"
	     "\t error_code = %" PRId32 "\n"
	     "\t user_id    = %" PRId32 "\n"
	     "\t title_id   = %.10s\n"
	     "\t dir_name   = %.32s\n",
	     event->type, event->error_code, event->user_id, event->title_id.data,
	     event->dir_name.data);

	return OK;
}

int KYTY_SYSV_ABI SaveDataBackup(const SaveDataBackup* backup) {
	PRINT_NAME();

	LOGF("\t backup = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(backup));

	if (backup == nullptr || backup->dir_name == nullptr) {
		return SAVE_DATA_ERROR_PARAMETER;
	}

	LOGF("\t user_id = %" PRId32 "\n"
	     "\t title_id = %.10s\n"
	     "\t dir_name = %.32s\n",
	     backup->user_id, backup->title_id != nullptr ? backup->title_id->data : "<default>",
	     backup->dir_name->data);

	queue_save_data_event(SAVE_DATA_EVENT_TYPE_BACKUP_END, backup->user_id, backup->title_id,
	                      backup->dir_name);

	return OK;
}

int KYTY_SYSV_ABI SaveDataSetParam(const SaveDataMountPoint* mount_point, uint32_t param_type,
                                   const void* param_buf, size_t param_buf_size) {
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(mount_point == nullptr);

	LOGF("\t mount_point    = %s\n"
	     "\t param_type     = %u\n"
	     "\t param_buf_size = %" PRIu64 "\n",
	     mount_point->data, param_type, param_buf_size);

	if (param_type == 0) {
		const auto* p = static_cast<const SaveDataParam*>(param_buf);

		LOGF("\t title      = %s\n"
		     "\t sub_title  = %s\n"
		     "\t detail     = %s\n"
		     "\t user_param = %u\n",
		     p->title, p->sub_title, p->detail, p->user_param);
	} else {
		LOGF("\t unsupported param_type, accepting as no-op\n");
	}

	return OK;
}

int KYTY_SYSV_ABI SaveDataGetMountInfo(const SaveDataMountPoint* mount_point,
                                       SaveDataMountInfo*        info) {
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(mount_point == nullptr);
	EXIT_NOT_IMPLEMENTED(info == nullptr);

	*info = {};

	info->blocks      = SAVE_DATA_BLOCKS_MAX;
	info->free_blocks = SAVE_DATA_BLOCKS_MAX;

	return OK;
}

int KYTY_SYSV_ABI SaveDataSaveIcon(const SaveDataMountPoint* mount_point,
                                   const SaveDataIcon*       icon) {
	EXIT_NOT_IMPLEMENTED(mount_point == nullptr);
	EXIT_NOT_IMPLEMENTED(icon == nullptr);

	LOGF("\t buf       = %016" PRIx64 "\n"
	     "\t buf_size  = %" PRIu64 "\n"
	     "\t data_size = %" PRIu64 "\n",
	     reinterpret_cast<uint64_t>(icon->buf), icon->buf_size, icon->data_size);

	return OK;
}

} // namespace SaveData

namespace LibSaveDataNative {

LIB_VERSION("SaveData_native", 1, "SaveData_native", 1, 1);

LIB_DEFINE(InitSaveDataNative_1) {
	LIB_FUNC("TywrFKCoLGY", ::Libs::SaveData::SaveDataInitialize3);
	LIB_FUNC("dyIhnXq-0SM", ::Libs::SaveData::SaveDataDirNameSearch);
	LIB_FUNC("PHnuI4LhuRk", ::Libs::SaveData::SaveDataDirNameSearch);
	LIB_FUNC("ZP4e7rlzOUk", ::Libs::SaveData::SaveDataMount3);
	LIB_FUNC("gjRZNnw0JPE", ::Libs::SaveData::SaveDataCreateTransactionResource);
	LIB_FUNC("lJUQuaKqoKY", ::Libs::SaveData::SaveDataDeleteTransactionResource);
	LIB_FUNC("sDCBrmc61XU", ::Libs::SaveData::SaveDataPrepare);
	LIB_FUNC("ie7qhZ4X0Cc", ::Libs::SaveData::SaveDataCommit);
	LIB_FUNC("oQySEUfgXRA", ::Libs::SaveData::SaveDataSetupSaveDataMemory2);
	LIB_FUNC("QwOO7vegnV8", ::Libs::SaveData::SaveDataGetSaveDataMemory2);
	LIB_FUNC("cduy9v4YmT4", ::Libs::SaveData::SaveDataSetSaveDataMemory2);
	LIB_FUNC("wiT9jeC7xPw", ::Libs::SaveData::SaveDataSyncSaveDataMemory);
	LIB_FUNC("j8xKtiFj0SY", ::Libs::SaveData::SaveDataGetEventResult);
	LIB_FUNC("WAzWTZm1H+I", ::Libs::SaveData::SaveDataTransferringMount);
	LIB_FUNC("RjMlsR8EXrw", ::Libs::SaveData::SaveDataTransferringMount);
	LIB_FUNC("z1JA8-iJt3k", ::Libs::SaveData::SaveDataBackup);
	LIB_FUNC("uW4vfTwMQVo", ::Libs::SaveData::SaveDataUmount2);
	LIB_FUNC("S1GkePI17zQ", ::Libs::SaveData::SaveDataDelete);
	LIB_FUNC("85zul--eGXs", ::Libs::SaveData::SaveDataSetParam);
	LIB_FUNC("XgvSuIdnMlw", ::Libs::SaveData::SaveDataGetParam);
	LIB_FUNC("65VH0Qaaz6s", ::Libs::SaveData::SaveDataGetMountInfo);
	LIB_FUNC("c88Yy54Mx0w", ::Libs::SaveData::SaveDataSaveIcon);
	LIB_FUNC("Z7z6HXWORJY", ::Libs::SaveData::SaveDataSaveIconByPath);
	LIB_FUNC("cGjO3wM3V28", ::Libs::SaveData::SaveDataLoadIcon);
	LIB_FUNC("X4MYzukPc3g", ::Libs::SaveData::SaveDataDirNameSearch);
	LIB_FUNC("yKDy8S5yLA0", ::Libs::SaveData::SaveDataTerminate);
}

} // namespace LibSaveDataNative

LIB_DEFINE(InitSaveData_1) {
	LIB_FUNC("TywrFKCoLGY", SaveData::SaveDataInitialize3);
	LIB_FUNC("dyIhnXq-0SM", SaveData::SaveDataDirNameSearch);
	LIB_FUNC("ZP4e7rlzOUk", SaveData::SaveDataMount3);
	LIB_FUNC("gjRZNnw0JPE", SaveData::SaveDataCreateTransactionResource);
	LIB_FUNC("lJUQuaKqoKY", SaveData::SaveDataDeleteTransactionResource);
	LIB_FUNC("sDCBrmc61XU", SaveData::SaveDataPrepare);
	LIB_FUNC("ie7qhZ4X0Cc", SaveData::SaveDataCommit);
	LIB_FUNC("oQySEUfgXRA", SaveData::SaveDataSetupSaveDataMemory2);
	LIB_FUNC("QwOO7vegnV8", SaveData::SaveDataGetSaveDataMemory2);
	LIB_FUNC("cduy9v4YmT4", SaveData::SaveDataSetSaveDataMemory2);
	LIB_FUNC("wiT9jeC7xPw", SaveData::SaveDataSyncSaveDataMemory);
	LIB_FUNC("j8xKtiFj0SY", SaveData::SaveDataGetEventResult);
	LIB_FUNC("WAzWTZm1H+I", SaveData::SaveDataTransferringMount);
	LIB_FUNC("RjMlsR8EXrw", SaveData::SaveDataTransferringMount);
	LIB_FUNC("z1JA8-iJt3k", SaveData::SaveDataBackup);
	LIB_FUNC("uW4vfTwMQVo", SaveData::SaveDataUmount2);
	LIB_FUNC("S1GkePI17zQ", SaveData::SaveDataDelete);
	LIB_FUNC("85zul--eGXs", SaveData::SaveDataSetParam);
	LIB_FUNC("XgvSuIdnMlw", SaveData::SaveDataGetParam);
	LIB_FUNC("65VH0Qaaz6s", SaveData::SaveDataGetMountInfo);
	LIB_FUNC("c88Yy54Mx0w", SaveData::SaveDataSaveIcon);
	LIB_FUNC("Z7z6HXWORJY", SaveData::SaveDataSaveIconByPath);
	LIB_FUNC("cGjO3wM3V28", SaveData::SaveDataLoadIcon);
	LIB_FUNC("X4MYzukPc3g", SaveData::SaveDataDirNameSearch);
	LIB_FUNC("yKDy8S5yLA0", SaveData::SaveDataTerminate);

	LibSaveDataNative::InitSaveDataNative_1(s);
}

} // namespace Libs
