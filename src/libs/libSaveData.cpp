#include "common/abi.h"
#include "common/assert.h"
#include "common/common.h"
#include "common/file.h"
#include "common/logging/log.h"
#include "common/path_util.h"
#include "common/stringUtils.h"
#include "common/threads.h"
#include "kernel/fileSystem.h"
#include "libs/errno.h"
#include "libs/libs.h"
#include "libs/saveDataMountSlots.h"
#include "loader/symbolDatabase.h"
#include "loader/systemContent.h"

#include <algorithm>
#include <cstring>
#include <ctime>
#include <deque>
#include <map>
#include <vector>

namespace Libs {

LIB_VERSION("SaveData", 1, "SaveData", 1, 1);

namespace SaveData {

// TODO(): specify dir at launcher
static constexpr uint64_t SAVE_DATA_BLOCKS_MIN = 48;
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

struct SaveParamTextField {
	char*  data;
	size_t size;
};

static SaveParamTextField param_text_field(SaveDataParam& param, uint32_t type) {
	switch (type) {
		case 1: return {param.title, sizeof(param.title)};
		case 2: return {param.sub_title, sizeof(param.sub_title)};
		case 3: return {param.detail, sizeof(param.detail)};
		default: return {nullptr, 0};
	}
}

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

struct SaveDataMemorySync {
	int32_t  user_id;
	uint32_t slot_id;
	uint32_t option;
	uint8_t  reserved[28];
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
static constexpr uint32_t SAVE_DATA_EVENT_TYPE_MEMORY_SYNC_END   = 3u;
static constexpr uint32_t SAVE_DATA_EVENT_TYPE_COMMIT_BACKUP_END = 4u;

static constexpr size_t   SAVE_DATA_MEMORY_MAX_SIZE      = 32 * 1024 * 1024;
static constexpr uint32_t SAVE_DATA_MEMORY_SET_PARAM     = 1u;
static constexpr uint32_t SAVE_DATA_MEMORY_DOUBLE_BUFFER = 2u;

struct SaveDataMemory {
	std::vector<uint8_t> data;
	SaveDataParam        param {};
	uint32_t             option = 0;
};

static std::map<std::filesystem::path, SaveDataMemory> g_save_data_memory;
static int32_t                                         g_next_transaction_resource = 1;
static std::deque<SaveDataEvent>                       g_save_data_events;
static SaveDataMountSlots                              g_mount_slots;
static Common::Mutex                                   g_mount_mutex;

static bool valid_path_component(std::string_view name) {
	return !name.empty() && name != "." && name != ".." &&
	       name.find_first_not_of(
	           "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-.@") ==
	           std::string_view::npos;
}

template <size_t N>
static bool valid_path_component(const char (&name)[N]) {
	const auto end = std::find(name, name + N, '\0');
	return end != name + N && valid_path_component(std::string_view(name, end - name));
}

static std::string get_title_id() {
	std::string title_id;
	if (!Loader::SystemContentParamSfoGetString("TITLE_ID", &title_id) ||
	    !valid_path_component(title_id)) {
		title_id = "UNKNOWN";
	}

	return title_id;
}

static std::string memory_dir_name(uint32_t slot_id) {
	return "sce_sdmemory" + (slot_id == 0 ? std::string() : std::to_string(slot_id));
}

static std::filesystem::path save_directory(std::string_view title_id, std::string_view dir_name,
                                            int32_t user_id) {
	const auto directory = PathUtil::GetPath(PathUtil::SAVE_DIR) / title_id / dir_name;
	for (uint32_t slot = 0; slot < 4; slot++) {
		if (dir_name == memory_dir_name(slot)) {
			return directory / std::to_string(user_id);
		}
	}
	return directory;
}

static std::filesystem::path memory_directory(int32_t user_id, uint32_t slot_id) {
	return save_directory(get_title_id(), memory_dir_name(slot_id), user_id);
}

static bool valid_memory_range(const SaveDataMemoryData& data, size_t size) {
	return data.buf != nullptr && data.offset >= 0 && static_cast<uint64_t>(data.offset) <= size &&
	       data.buf_size <= size - static_cast<size_t>(data.offset);
}

// Replacing a flushed temporary file protects the
// previous save if writing fails or the emulator exits partway through an update.
static int write_save_file(const std::filesystem::path& path, const void* data, uint32_t size) {
	const auto   temporary = std::filesystem::path(path.string() + ".tmp");
	Common::File file;
	if (!file.Create(temporary)) {
		return SAVE_DATA_ERROR_INTERNAL;
	}
	uint32_t written = 0;
	file.Write(data, size, &written);
	const bool flushed = file.Flush();
	file.Close();
	std::error_code error;
	if (written == size && flushed) {
		// Common::File::RenameFile deliberately refuses to replace an existing file.
		std::filesystem::rename(temporary, path, error);
		if (!error) {
			return OK;
		}
	}
	std::filesystem::remove(temporary, error);
	return SAVE_DATA_ERROR_INTERNAL;
}

static int read_save_blocks(const std::filesystem::path& directory, uint64_t* blocks) {
	Common::File file(directory / "sce_sys" / "blocks.bin", Common::File::Mode::Read);
	if (file.IsInvalid() || file.Size() != sizeof(*blocks)) {
		return SAVE_DATA_ERROR_BROKEN;
	}
	uint64_t value = 0;
	uint32_t read = 0;
	file.Read(&value, sizeof(value), &read);
	if (read != sizeof(value) || value < SAVE_DATA_BLOCKS_MIN || value > SAVE_DATA_BLOCKS_MAX) {
		return SAVE_DATA_ERROR_BROKEN;
	}
	*blocks = value;
	return OK;
}

static int read_save_param(const std::filesystem::path& path, SaveDataParam* param) {
	std::error_code error;
	const bool exists = std::filesystem::exists(path, error);
	if (error) {
		return SAVE_DATA_ERROR_INTERNAL;
	}
	if (!exists) {
		return SAVE_DATA_ERROR_NOT_FOUND;
	}
	Common::File file(path, Common::File::Mode::Read);
	if (file.IsInvalid()) {
		return SAVE_DATA_ERROR_INTERNAL;
	}
	if (file.Size() != sizeof(*param)) {
		return SAVE_DATA_ERROR_BROKEN;
	}
	uint32_t read = 0;
	file.Read(param, sizeof(*param), &read);
	return read == sizeof(*param) ? OK : SAVE_DATA_ERROR_BROKEN;
}

static std::filesystem::path save_param_path(const std::filesystem::path& directory) {
	return directory / "sce_sys" / "param.bin";
}

static int64_t newest_save_time(const std::filesystem::path& directory) {
	int64_t mtime = 0;
	std::error_code error;
	for (std::filesystem::recursive_directory_iterator it(
	         directory, std::filesystem::directory_options::skip_permission_denied, error),
	     end;
	     it != end; it.increment(error)) {
		if (error) {
			error.clear();
			continue;
		}
		if (it->is_regular_file(error) && !error) {
			const auto time = Common::File::GetLastWriteTimeUTC(it->path());
			if (!time.IsInvalid()) {
				mtime = std::max(mtime, static_cast<int64_t>(time.ToUnix()));
			}
		}
		error.clear();
	}
	return mtime;
}

static int load_save_param(const std::filesystem::path& directory, SaveDataParam* param) {
	*param = {};
	const int status = read_save_param(save_param_path(directory), param);
	if (status == OK) {
		param->mtime = newest_save_time(directory);
	}
	return status;
}

static int save_memory(const std::filesystem::path& directory, const SaveDataMemory& memory) {
	std::error_code error;
	std::filesystem::create_directories(directory, error);
	if (error) {
		return SAVE_DATA_ERROR_INTERNAL;
	}
	// Display metadata is a separate host sidecar, leaving memory.dat as raw guest bytes.
	int result = write_save_file(directory / "param.bin", &memory.param, sizeof(memory.param));
	if (result == OK) {
		result =
		    write_save_file(directory / "memory.dat", memory.data.data(), memory.data.size());
	}
	return result;
}

static int load_memory(const std::filesystem::path& directory, SaveDataMemory* memory,
                       size_t* existed_size) {
	std::error_code error;
	const bool      exists = std::filesystem::exists(directory / "memory.dat", error);
	if (error) {
		return SAVE_DATA_ERROR_INTERNAL;
	}
	if (!exists) {
		return OK;
	}
	Common::File file(directory / "memory.dat", Common::File::Mode::Read);
	if (file.IsInvalid()) {
		return SAVE_DATA_ERROR_INTERNAL;
	}
	*existed_size = file.Size();
	if (*existed_size == 0 || *existed_size > SAVE_DATA_MEMORY_MAX_SIZE) {
		return SAVE_DATA_ERROR_BROKEN;
	}
	const auto size = static_cast<uint32_t>(std::min(*existed_size, memory->data.size()));
	uint32_t   read = 0;
	file.Read(memory->data.data(), size, &read);
	if (read != size) {
		return SAVE_DATA_ERROR_BROKEN;
	}
	file.Close();
	return read_save_param(directory / "param.bin", &memory->param);
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

static int mount_save_data(int slot, const std::filesystem::path& directory,
                           uint32_t status, SaveDataMountResult* result) {
	const std::string mount_point = SaveDataMountSlots::MountPoint(static_cast<size_t>(slot));
	LibKernel::FileSystem::Mount(directory, mount_point);
	g_mount_slots.Mount(static_cast<size_t>(slot), directory);
	std::snprintf(result->mount_point.data, sizeof(result->mount_point.data), "%s",
	              mount_point.c_str());
	result->required_blocks = 0;
	result->mount_status    = status;
	return OK;
}

int KYTY_SYSV_ABI SaveDataInitialize3(const void* /*init*/) {
	PRINT_NAME();

	// EXIT_NOT_IMPLEMENTED(init != nullptr);

	return OK;
}

int KYTY_SYSV_ABI SaveDataTerminate() {
	PRINT_NAME();

	Common::LockGuard lock(g_mount_mutex);
	if (!g_mount_slots.Empty()) {
		return SAVE_DATA_ERROR_BUSY;
	}
	g_save_data_events.clear();
	g_save_data_memory.clear();

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
	    (cond->title_id != nullptr && !valid_path_component(cond->title_id->data)) ||
	    (result->dir_names_num != 0 && result->dir_names == nullptr) ||
	    (cond->dir_name != nullptr &&
	     std::memchr(cond->dir_name->data, 0, sizeof(cond->dir_name->data)) == nullptr) ||
	    static_cast<uint32_t>(cond->key) > static_cast<uint32_t>(SaveDataSortKey::FreeBlocks) ||
	    static_cast<uint32_t>(cond->order) > static_cast<uint32_t>(SaveDataSortOrder::Descent)) {
		return SAVE_DATA_ERROR_PARAMETER;
	}
	if (cond->user_id < 0) {
		return SAVE_DATA_ERROR_INVALID_LOGIN_USER;
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

	Common::LockGuard lock(g_mount_mutex);
	const std::string title_id = cond->title_id != nullptr ? cond->title_id->data : get_title_id();
	const auto        root     = PathUtil::GetPath(PathUtil::SAVE_DIR) / title_id;
	std::vector<std::string> dir_list;

	if (Common::File::IsDirectoryExisting(root)) {
		for (const auto& entry: Common::File::GetDirEntries(root)) {
			if (!entry.is_file && entry.name != "." && entry.name != ".." &&
			    !entry.name.starts_with("sce_") && valid_path_component(entry.name) &&
			    Common::File::IsFileExisting(save_param_path(root / entry.name))) {
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
			const int status = load_save_param(root / dir_list[i], &result->params[i]);
			if (status != OK) {
				return status;
			}
		}
		if (result->infos != nullptr) {
			auto& info = result->infos[i];
			info = {};
			const int status = read_save_blocks(root / dir_list[i], &info.blocks);
			if (status != OK) {
				return status;
			}
			// Host-directory saves report their allocation as free space.
			info.free_blocks = info.blocks;
		}
	}

	return OK;
}

int KYTY_SYSV_ABI SaveDataMount3(const SaveDataMount3* mount, SaveDataMountResult* mount_result) {
	PRINT_NAME();

	if (mount == nullptr || mount_result == nullptr || mount->dir_name == nullptr ||
	    !valid_path_component(mount->dir_name->data)) {
		return SAVE_DATA_ERROR_PARAMETER;
	}
	if (mount->user_id < 0) {
		return SAVE_DATA_ERROR_INVALID_LOGIN_USER;
	}

	LOGF("\t user_id       = %d\n"
	     "\t dir_name      = %s\n"
	     "\t blocks        = %" PRIu64 "\n"
	     "\t system_blocks = %" PRIu64 "\n"
	     "\t mount_mode    = %" PRIu32 "\n"
	     "\t resource      = %" PRId32 "\n",
	     mount->user_id, mount->dir_name->data, mount->blocks, mount->system_blocks,
	     mount->mount_mode, mount->resource);

	*mount_result = {};

	Common::LockGuard lock(g_mount_mutex);
	const std::string dir_name  = mount->dir_name->data;
	const auto        mount_dir = save_directory(get_title_id(), dir_name, mount->user_id).string();
	const bool        create    = ((mount->mount_mode & 4u) != 0);
	const bool        create2   = ((mount->mount_mode & 32u) != 0);
	const bool        open      = (!create && !create2 && ((mount->mount_mode & 3u) != 0));

	const int slot = g_mount_slots.FindAvailable(mount_dir);
	if (slot == SaveDataMountSlots::BUSY) {
		return SAVE_DATA_ERROR_BUSY;
	}
	if (slot == SaveDataMountSlots::FULL) {
		return SAVE_DATA_ERROR_MOUNT_FULL;
	}

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
		if (mount->blocks < SAVE_DATA_BLOCKS_MIN || mount->blocks > SAVE_DATA_BLOCKS_MAX) {
			return SAVE_DATA_ERROR_PARAMETER;
		}
		const auto metadata = mount_dir / "sce_sys";
		std::error_code error;
		std::filesystem::create_directories(metadata, error);
		if (error) {
			return SAVE_DATA_ERROR_INTERNAL;
		}
		const int status = write_save_file(metadata / "blocks.bin", &mount->blocks,
		                                   sizeof(mount->blocks));
		if (status != OK) {
			std::filesystem::remove_all(mount_dir, error);
			return status;
		}
		const SaveDataParam initial {};
		const int param_status = write_save_file(save_param_path(mount_dir), &initial,
		                                         sizeof(initial));
		if (param_status != OK) {
			std::filesystem::remove_all(mount_dir, error);
			return param_status;
		}
		created = true;
	}

	return mount_save_data(slot, mount_dir, created ? 1u : 0u, mount_result);
}

int KYTY_SYSV_ABI SaveDataSetupSaveDataMemory2(const SaveDataMemorySetup2* setup_param,
                                               SaveDataMemorySetupResult*  result) {
	PRINT_NAME();

	if (setup_param == nullptr || setup_param->slot_id >= 4 || setup_param->memory_size == 0 ||
	    setup_param->memory_size > SAVE_DATA_MEMORY_MAX_SIZE || (setup_param->option & ~3u) != 0) {
		return SAVE_DATA_ERROR_PARAMETER;
	}
	if (setup_param->user_id < 0) {
		return SAVE_DATA_ERROR_INVALID_LOGIN_USER;
	}

	LOGF("\t option           = 0x%08" PRIx32 "\n"
	     "\t user_id          = %" PRId32 "\n"
	     "\t memory_size      = %" PRIu64 "\n"
	     "\t icon_memory_size = %" PRIu64 "\n"
	     "\t slot_id          = %" PRIu32 "\n",
	     setup_param->option, setup_param->user_id, static_cast<uint64_t>(setup_param->memory_size),
	     static_cast<uint64_t>(setup_param->icon_memory_size), setup_param->slot_id);

	Common::LockGuard lock(g_mount_mutex);
	const auto        directory = memory_directory(setup_param->user_id, setup_param->slot_id);
	if (g_save_data_memory.contains(directory) ||
	    g_mount_slots.FindAvailable(directory) ==
	        SaveDataMountSlots::BUSY) {
		return SAVE_DATA_ERROR_BUSY;
	}
	size_t total = setup_param->memory_size *
	               ((setup_param->option & SAVE_DATA_MEMORY_DOUBLE_BUFFER) != 0 ? 2 : 1);
	for (const auto& [path, memory]: g_save_data_memory) {
		total +=
		    memory.data.size() * ((memory.option & SAVE_DATA_MEMORY_DOUBLE_BUFFER) != 0 ? 2 : 1);
	}
	if (g_save_data_memory.size() >= 4 || total > SAVE_DATA_MEMORY_MAX_SIZE) {
		return SAVE_DATA_ERROR_LIMITATION_OVER;
	}
	SaveDataMemory memory;
	memory.data.resize(setup_param->memory_size);
	memory.option = setup_param->option;
	std::snprintf(memory.param.title, sizeof(memory.param.title), "Saved Data");
	size_t    existed_size = 0;
	const int status       = load_memory(directory, &memory, &existed_size);
	if (status != OK) {
		return status;
	}
	if (existed_size == 0 && setup_param->init_param != nullptr &&
	    (memory.option & SAVE_DATA_MEMORY_SET_PARAM) != 0) {
		memory.param       = *setup_param->init_param;
		memory.param.mtime = 0;
	}
	g_save_data_memory.emplace(directory, std::move(memory));

	if (result != nullptr) {
		*result                     = {};
		result->existed_memory_size = existed_size;
	}

	return OK;
}

int KYTY_SYSV_ABI SaveDataGetSaveDataMemory2(SaveDataMemoryGet2* get_param) {
	PRINT_NAME();

	if (get_param == nullptr || get_param->slot_id >= 4) {
		return SAVE_DATA_ERROR_PARAMETER;
	}
	if (get_param->user_id < 0) {
		return SAVE_DATA_ERROR_INVALID_LOGIN_USER;
	}

	LOGF("\t user_id  = %" PRId32 "\n"
	     "\t data     = 0x%016" PRIx64 "\n"
	     "\t param    = 0x%016" PRIx64 "\n"
	     "\t icon     = 0x%016" PRIx64 "\n"
	     "\t slot_id  = %" PRIu32 "\n",
	     get_param->user_id, reinterpret_cast<uint64_t>(get_param->data),
	     reinterpret_cast<uint64_t>(get_param->param), reinterpret_cast<uint64_t>(get_param->icon),
	     get_param->slot_id);

	Common::LockGuard lock(g_mount_mutex);
	const auto        it =
	    g_save_data_memory.find(memory_directory(get_param->user_id, get_param->slot_id));
	if (it == g_save_data_memory.end()) {
		return SAVE_DATA_ERROR_MEMORY_NOT_READY;
	}
	const auto& memory = it->second;
	if ((get_param->param != nullptr && (memory.option & SAVE_DATA_MEMORY_SET_PARAM) == 0) ||
	    (get_param->data != nullptr && !valid_memory_range(*get_param->data, memory.data.size()))) {
		return SAVE_DATA_ERROR_PARAMETER;
	}
	if (get_param->data != nullptr) {
		const auto& data = *get_param->data;
		std::memcpy(data.buf, memory.data.data() + data.offset, data.buf_size);
	}
	if (get_param->param != nullptr) {
		*get_param->param = memory.param;
	}
	if (get_param->icon != nullptr) {
		get_param->icon->data_size = 0;
	}

	return OK;
}

int KYTY_SYSV_ABI SaveDataSetSaveDataMemory2(const SaveDataMemorySet2* set_param) {
	PRINT_NAME();

	if (set_param == nullptr || set_param->slot_id >= 4 || set_param->data_num > 5 ||
	    (set_param->data == nullptr && set_param->data_num != 0)) {
		return SAVE_DATA_ERROR_PARAMETER;
	}
	if (set_param->user_id < 0) {
		return SAVE_DATA_ERROR_INVALID_LOGIN_USER;
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

	Common::LockGuard lock(g_mount_mutex);
	const auto        it =
	    g_save_data_memory.find(memory_directory(set_param->user_id, set_param->slot_id));
	if (it == g_save_data_memory.end()) {
		return SAVE_DATA_ERROR_MEMORY_NOT_READY;
	}
	if (set_param->param != nullptr && (it->second.option & SAVE_DATA_MEMORY_SET_PARAM) == 0) {
		return SAVE_DATA_ERROR_PARAMETER;
	}
	// Some games supply one descriptor with an implicit zero count.
	const uint32_t data_num =
	    set_param->data != nullptr && set_param->data_num == 0 ? 1 : set_param->data_num;
	for (uint32_t i = 0; i < data_num; i++) {
		if (!valid_memory_range(set_param->data[i], it->second.data.size())) {
			return SAVE_DATA_ERROR_PARAMETER;
		}
	}
	if (data_num == 0 && set_param->param == nullptr) {
		return OK;
	}

	// Save the complete update before publishing it so progress survives process exit.
	auto updated = it->second;
	for (uint32_t i = 0; i < data_num; i++) {
		const auto& data = set_param->data[i];
		std::memcpy(updated.data.data() + data.offset, data.buf, data.buf_size);
	}
	if (set_param->param != nullptr) {
		updated.param = *set_param->param;
	}
	updated.param.mtime = static_cast<int64_t>(std::time(nullptr));
	const int status    = save_memory(it->first, updated);
	if (status == OK) {
		it->second = std::move(updated);
	}
	return status;
}

int KYTY_SYSV_ABI SaveDataTransferringMount(const SaveDataTransferringMount* mount,
                                            SaveDataMountResult*             mount_result) {
	PRINT_NAME();

	if (mount == nullptr || mount_result == nullptr || mount->title_id == nullptr ||
	    mount->dir_name == nullptr || !valid_path_component(mount->title_id->data) ||
	    !valid_path_component(mount->dir_name->data)) {
		return SAVE_DATA_ERROR_PARAMETER;
	}
	if (mount->user_id < 0) {
		return SAVE_DATA_ERROR_INVALID_LOGIN_USER;
	}

	LOGF("\t user_id  = %" PRId32 "\n"
	     "\t title_id = %s\n"
	     "\t dir_name = %s\n",
	     mount->user_id, mount->title_id->data, mount->dir_name->data);

	*mount_result = {};

	Common::LockGuard lock(g_mount_mutex);
	const std::string dir_name = mount->dir_name->data;
	const auto mount_dir = save_directory(mount->title_id->data, dir_name, mount->user_id).string();
	const int  slot      = g_mount_slots.FindAvailable(dir_name);
	if (slot == SaveDataMountSlots::BUSY) {
		return SAVE_DATA_ERROR_BUSY;
	}
	if (slot == SaveDataMountSlots::FULL) {
		return SAVE_DATA_ERROR_MOUNT_FULL;
	}

	if (!Common::File::IsDirectoryExisting(mount_dir)) {
		return SAVE_DATA_ERROR_NOT_FOUND;
	}

	return mount_save_data(slot, mount_dir, 0, mount_result);
}

int KYTY_SYSV_ABI SaveDataUmount2(uint32_t mode, const SaveDataMountPoint* mount_point) {
	PRINT_NAME();

	if (mount_point == nullptr) {
		return SAVE_DATA_ERROR_PARAMETER;
	}

	LOGF("\t mode        = %" PRIu32 "\n"
	     "\t mount_point = %s\n",
	     mode, mount_point->data);

	Common::LockGuard lock(g_mount_mutex);
	const std::string point = mount_point->data;
	const int         slot  = g_mount_slots.Find(point);
	if (slot == SaveDataMountSlots::FULL) {
		return SAVE_DATA_ERROR_NOT_FOUND;
	}

	if ((mode & SAVE_DATA_UMOUNT_MODE_BACKUP_ASYNC) != 0) {
		queue_save_data_event(SAVE_DATA_EVENT_TYPE_UMOUNT_BACKUP_END, 0, nullptr, nullptr);
	}
	LibKernel::FileSystem::Umount(point);
	g_mount_slots.Release(static_cast<size_t>(slot));

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
		Common::LockGuard lock(g_mount_mutex);
		queue_save_data_event(SAVE_DATA_EVENT_TYPE_COMMIT_BACKUP_END, 0, nullptr, nullptr);
	}

	return OK;
}

int KYTY_SYSV_ABI SaveDataDelete(const SaveDataDelete* del) {
	PRINT_NAME();

	if (del == nullptr || del->dir_name == nullptr || !valid_path_component(del->dir_name->data) ||
	    (del->title_id != nullptr && !valid_path_component(del->title_id->data))) {
		return SAVE_DATA_ERROR_PARAMETER;
	}
	if (del->user_id < 0) {
		return SAVE_DATA_ERROR_INVALID_LOGIN_USER;
	}

	LOGF("\t user_id  = %" PRId32 "\n"
	     "\t title_id = %s\n"
	     "\t dir_name = %s\n",
	     del->user_id, del->title_id != nullptr ? del->title_id->data : "<default>",
	     del->dir_name->data);

	Common::LockGuard lock(g_mount_mutex);
	const auto dir = save_directory(del->title_id != nullptr ? del->title_id->data : get_title_id(),
	                                del->dir_name->data, del->user_id);
	if (g_mount_slots.FindAvailable(dir) == SaveDataMountSlots::BUSY ||
	    g_save_data_memory.contains(dir)) {
		return SAVE_DATA_ERROR_BUSY;
	}
	std::error_code error;
	std::filesystem::remove_all(dir, error);

	return error ? SAVE_DATA_ERROR_INTERNAL : OK;
}

int KYTY_SYSV_ABI SaveDataGetParam(const SaveDataMountPoint* mount_point, uint32_t param_type,
                                   void* param_buf, size_t param_buf_size, size_t* got_size) {
	PRINT_NAME();

	if (mount_point == nullptr || param_buf == nullptr || param_type > 5 ||
	    std::memchr(mount_point->data, 0, sizeof(mount_point->data)) == nullptr) {
		return SAVE_DATA_ERROR_PARAMETER;
	}

	LOGF("\t mount_point    = %s\n"
	     "\t param_type     = %u\n"
	     "\t param_buf_size = %" PRIu64 "\n",
	     mount_point->data, param_type, param_buf_size);
	Common::LockGuard lock(g_mount_mutex);
	const int slot = g_mount_slots.Find(mount_point->data);
	if (slot == SaveDataMountSlots::FULL) {
		return SAVE_DATA_ERROR_NOT_MOUNTED;
	}

	SaveDataParam param {};
	const int status = load_save_param(g_mount_slots.Directory(static_cast<size_t>(slot)), &param);
	if (status != OK) {
		return status;
	}
	size_t size = 0;
	if (param_type == 0) {
		size = sizeof(param);
		if (param_buf_size < size) {
			return SAVE_DATA_ERROR_PARAMETER;
		}
		std::memcpy(param_buf, &param, size);
	} else if (param_type == 4 || param_type == 5) {
		size = param_type == 4 ? sizeof(param.user_param) : sizeof(param.mtime);
		if (param_buf_size < size) {
			return SAVE_DATA_ERROR_PARAMETER;
		}
		std::memcpy(param_buf, param_type == 4 ? static_cast<const void*>(&param.user_param)
		                                         : static_cast<const void*>(&param.mtime), size);
	} else {
		if (param_buf_size == 0) {
			return SAVE_DATA_ERROR_PARAMETER;
		}
		const auto field = param_text_field(param, param_type);
		size = std::min(static_cast<size_t>(
		                    std::find(field.data, field.data + field.size, '\0') - field.data),
		                param_buf_size - 1);
		std::memcpy(param_buf, field.data, size);
		static_cast<char*>(param_buf)[size++] = '\0';
	}
	if (got_size != nullptr) {
		*got_size = size;
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

int KYTY_SYSV_ABI SaveDataSyncSaveDataMemory(const SaveDataMemorySync* sync_param) {
	PRINT_NAME();

	if (sync_param == nullptr || sync_param->slot_id >= 4 || sync_param->option > 1) {
		return SAVE_DATA_ERROR_PARAMETER;
	}
	if (sync_param->user_id < 0) {
		return SAVE_DATA_ERROR_INVALID_LOGIN_USER;
	}
	Common::LockGuard lock(g_mount_mutex);
	const auto        it =
	    g_save_data_memory.find(memory_directory(sync_param->user_id, sync_param->slot_id));
	if (it == g_save_data_memory.end()) {
		return SAVE_DATA_ERROR_MEMORY_NOT_READY;
	}
	const auto dir_name = memory_dir_name(sync_param->slot_id);
	if (g_mount_slots.FindAvailable(it->first) == SaveDataMountSlots::BUSY) {
		return SAVE_DATA_ERROR_BUSY;
	}
	const int status = save_memory(it->first, it->second);
	if (sync_param->option == 1) {
		return status;
	}
	SceSaveDataTitleId title {};
	SceSaveDataDirName directory {};
	std::snprintf(title.data, sizeof(title.data), "%s", get_title_id().c_str());
	std::snprintf(directory.data, sizeof(directory.data), "%s", dir_name.c_str());
	queue_save_data_event(SAVE_DATA_EVENT_TYPE_MEMORY_SYNC_END, sync_param->user_id, &title,
	                      &directory, status);

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

	Common::LockGuard lock(g_mount_mutex);
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

	Common::LockGuard lock(g_mount_mutex);
	queue_save_data_event(SAVE_DATA_EVENT_TYPE_BACKUP_END, backup->user_id, backup->title_id,
	                      backup->dir_name);

	return OK;
}

int KYTY_SYSV_ABI SaveDataSetParam(const SaveDataMountPoint* mount_point, uint32_t param_type,
                                   const void* param_buf, size_t param_buf_size) {
	PRINT_NAME();

	if (mount_point == nullptr || param_buf == nullptr || param_type > 4 ||
	    std::memchr(mount_point->data, 0, sizeof(mount_point->data)) == nullptr ||
	    (param_type == 0 && param_buf_size < sizeof(SaveDataParam)) ||
	    (param_type == 4 && param_buf_size < sizeof(uint32_t)) ||
	    (param_type >= 1 && param_type <= 3 && param_buf_size == 0)) {
		return SAVE_DATA_ERROR_PARAMETER;
	}

	LOGF("\t mount_point    = %s\n"
	     "\t param_type     = %u\n"
	     "\t param_buf_size = %" PRIu64 "\n",
	     mount_point->data, param_type, param_buf_size);
	Common::LockGuard lock(g_mount_mutex);
	const int slot = g_mount_slots.Find(mount_point->data);
	if (slot == SaveDataMountSlots::FULL) {
		return SAVE_DATA_ERROR_NOT_MOUNTED;
	}

	if (param_type == 0) {
		const auto* p = static_cast<const SaveDataParam*>(param_buf);

		LOGF("\t title      = %.128s\n"
		     "\t sub_title  = %.128s\n"
		     "\t detail     = %.1024s\n"
		     "\t user_param = %u\n",
		     p->title, p->sub_title, p->detail, p->user_param);
		return write_save_file(save_param_path(g_mount_slots.Directory(static_cast<size_t>(slot))),
		                       p, sizeof(*p));
	}
	SaveDataParam param {};
	const auto path = save_param_path(g_mount_slots.Directory(static_cast<size_t>(slot)));
	const int status = read_save_param(path, &param);
	if (status != OK) {
		return status;
	}
	if (param_type == 4) {
		std::memcpy(&param.user_param, param_buf, sizeof(param.user_param));
	} else {
		const auto field = param_text_field(param, param_type);
		const auto* text = static_cast<const char*>(param_buf);
		const size_t length = std::min(param_buf_size, field.size - 1);
		const auto* end = static_cast<const char*>(std::memchr(text, '\0', length));
		const size_t count = end != nullptr ? static_cast<size_t>(end - text) : length;
		std::memset(field.data, 0, field.size);
		std::memcpy(field.data, text, count);
	}
	return write_save_file(path, &param, sizeof(param));
}

int KYTY_SYSV_ABI SaveDataGetMountInfo(const SaveDataMountPoint* mount_point,
                                       SaveDataMountInfo*        info) {
	PRINT_NAME();

	if (mount_point == nullptr || info == nullptr ||
	    std::memchr(mount_point->data, 0, sizeof(mount_point->data)) == nullptr) {
		return SAVE_DATA_ERROR_PARAMETER;
	}
	Common::LockGuard lock(g_mount_mutex);
	const int slot = g_mount_slots.Find(mount_point->data);
	if (slot == SaveDataMountSlots::FULL) {
		return SAVE_DATA_ERROR_NOT_MOUNTED;
	}
	uint64_t blocks = 0;
	const int status = read_save_blocks(g_mount_slots.Directory(static_cast<size_t>(slot)), &blocks);
	if (status != OK) {
		return status;
	}
	*info = {};
	info->blocks = blocks;
	info->free_blocks = blocks;

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
