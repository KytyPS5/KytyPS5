// Exercise the guest ABI without linking unrelated emulator subsystems.
#include "libs/libSaveData.cpp"

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>

#ifdef _WIN32
#include <process.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {
std::string           g_test_title = "PPSA21564";
std::filesystem::path g_mounted_directory;
std::map<std::string, std::filesystem::path> g_test_mounts;
} // namespace

namespace Loader {
bool SystemContentParamSfoGetString(const char*, std::string* value) {
	*value = g_test_title;
	return true;
}
void SymbolDatabase::Add(const SymbolResolve&, uint64_t, const std::string&) {}
namespace Timer {
double GetTimeMs() {
	return 0.0;
}
} // namespace Timer
} // namespace Loader

namespace Libs::LibKernel::FileSystem {
void Mount(const std::filesystem::path& directory, const std::string& point) {
	g_mounted_directory = directory;
	g_test_mounts[point] = directory;
}
void Umount(const std::string& point) { g_test_mounts.erase(point); }
std::filesystem::path GetRealFilename(const std::string& point) {
	return g_test_mounts.at(point);
}
} // namespace Libs::LibKernel::FileSystem

namespace {
using namespace Libs::SaveData;
namespace fs = std::filesystem;

#define CHECK(condition)                                                                           \
	do {                                                                                           \
		if (!(condition)) {                                                                        \
			std::fprintf(stderr, "SaveDataMemoryTests:%d: %s\n", __LINE__, #condition);            \
			std::abort();                                                                          \
		}                                                                                          \
	} while (false)

SaveDataMemorySetup2 SetupParam(int32_t user = 1, uint32_t slot = 0, size_t size = 16) {
	SaveDataMemorySetup2 setup {};
	setup.user_id     = user;
	setup.slot_id     = slot;
	setup.memory_size = size;
	return setup;
}

size_t Setup(int32_t user = 1, uint32_t slot = 0, size_t size = 16, uint32_t option = 0) {
	auto setup   = SetupParam(user, slot, size);
	setup.option = option;
	SaveDataMemorySetupResult result {};
	CHECK(SaveDataSetupSaveDataMemory2(&setup, &result) == OK);
	return result.existed_memory_size;
}

int Set(void* bytes, size_t size, int64_t offset = 0, int32_t user = 1, uint32_t slot = 0) {
	SaveDataMemoryData data {bytes, size, offset, {}};
	SaveDataMemorySet2 set {};
	set.user_id  = user;
	set.slot_id  = slot;
	set.data     = &data;
	set.data_num = 1;
	return SaveDataSetSaveDataMemory2(&set);
}

std::vector<uint8_t> Get(size_t size = 16, int32_t user = 1, uint32_t slot = 0) {
	std::vector<uint8_t> bytes(size);
	SaveDataMemoryData   data {bytes.data(), size, 0, {}};
	SaveDataMemoryGet2   get {};
	get.user_id = user;
	get.slot_id = slot;
	get.data    = &data;
	CHECK(SaveDataGetSaveDataMemory2(&get) == OK);
	return bytes;
}

void Reset(const char* title) {
	CHECK(SaveDataTerminate() == OK);
	g_test_title = title;
}

void TestValidationAndScatter() {
	Reset("VALIDATE");
	uint8_t            byte = 0x55;
	SaveDataMemoryGet2 get {};
	get.user_id = 1;
	SaveDataMemorySync sync {};
	sync.user_id = 1;
	CHECK(SaveDataGetSaveDataMemory2(&get) == SAVE_DATA_ERROR_MEMORY_NOT_READY);
	CHECK(Set(&byte, 1) == SAVE_DATA_ERROR_MEMORY_NOT_READY);
	CHECK(SaveDataSyncSaveDataMemory(&sync) == SAVE_DATA_ERROR_MEMORY_NOT_READY);
	CHECK(SaveDataSetupSaveDataMemory2(nullptr, nullptr) == SAVE_DATA_ERROR_PARAMETER);
	auto invalid_user = SetupParam(-1);
	CHECK(SaveDataSetupSaveDataMemory2(&invalid_user, nullptr) ==
	      SAVE_DATA_ERROR_INVALID_LOGIN_USER);
	for (auto invalid:
	     {SetupParam(1, 4), SetupParam(1, 0, 0), SetupParam(1, 0, 32 * 1024 * 1024 + 1)}) {
		CHECK(SaveDataSetupSaveDataMemory2(&invalid, nullptr) == SAVE_DATA_ERROR_PARAMETER);
	}
	CHECK(Setup() == 0);
	auto setup = SetupParam();
	CHECK(SaveDataSetupSaveDataMemory2(&setup, nullptr) == SAVE_DATA_ERROR_BUSY);
	std::array<uint8_t, 16> original {};
	for (size_t i = 0; i < original.size(); ++i) {
		original[i] = static_cast<uint8_t>(i);
	}
	CHECK(Set(original.data(), original.size()) == OK);
	SaveDataMemoryData data[] = {{&byte, 1, 2, {}}, {&byte, 1, 16, {}}};
	SaveDataMemorySet2 set {};
	set.user_id  = 1;
	set.data     = data;
	set.data_num = 2;
	CHECK(SaveDataSetSaveDataMemory2(&set) == SAVE_DATA_ERROR_PARAMETER);
	CHECK(Get() == std::vector<uint8_t>(original.begin(), original.end()));
	data[1].offset = 12;
	CHECK(SaveDataSetSaveDataMemory2(&set) == OK);
	original[2] = original[12] = byte;
	CHECK(Get() == std::vector<uint8_t>(original.begin(), original.end()));

	byte         = 0xaa;
	set.data_num = 0;
	CHECK(SaveDataSetSaveDataMemory2(&set) == OK);
	original[2] = byte;
	CHECK(Get() == std::vector<uint8_t>(original.begin(), original.end()));
	set.data = nullptr;
	CHECK(SaveDataSetSaveDataMemory2(&set) == OK);
	CHECK(Get() == std::vector<uint8_t>(original.begin(), original.end()));
	set.data_num = 6;
	CHECK(SaveDataSetSaveDataMemory2(&set) == SAVE_DATA_ERROR_PARAMETER);
	set.data_num = 1;
	set.data     = nullptr;
	CHECK(SaveDataSetSaveDataMemory2(&set) == SAVE_DATA_ERROR_PARAMETER);
	CHECK(Set(&byte, 1, -1) == SAVE_DATA_ERROR_PARAMETER);
	CHECK(Set(&byte, 1, std::numeric_limits<int64_t>::max()) == SAVE_DATA_ERROR_PARAMETER);
	CHECK(Set(&byte, std::numeric_limits<size_t>::max(), 1) == SAVE_DATA_ERROR_PARAMETER);
	SaveDataMemoryData invalid {&byte, std::numeric_limits<size_t>::max(), 1, {}};
	get.data = &invalid;
	CHECK(SaveDataGetSaveDataMemory2(&get) == SAVE_DATA_ERROR_PARAMETER);
	CHECK(byte == 0xaa);
	CHECK(Get() == std::vector<uint8_t>(original.begin(), original.end()));
	CHECK(SaveDataTerminate() == OK);
	CHECK(Setup() == original.size());
	CHECK(Get() == std::vector<uint8_t>(original.begin(), original.end()));
}

void TestIsolationAndSync() {
	Reset("ISOLATE");
	uint8_t byte = 11;
	CHECK(Setup() == 0);
	CHECK(Set(&byte, 1) == OK);
	CHECK(Setup(2) == 0);
	CHECK(Setup(1, 1) == 0);
	CHECK(Get(16, 2) == std::vector<uint8_t>(16));
	CHECK(Get(16, 1, 1) == std::vector<uint8_t>(16));
	byte = 22;
	CHECK(Set(&byte, 1, 0, 2) == OK);
	byte = 33;
	CHECK(Set(&byte, 1, 0, 1, 1) == OK);
	CHECK(Get()[0] == 11);
	CHECK(Get(16, 2)[0] == 22);
	CHECK(Get(16, 1, 1)[0] == 33);

	SaveDataMemorySync sync {};
	sync.user_id = 1;
	sync.slot_id = 1;
	CHECK(SaveDataSyncSaveDataMemory(&sync) == OK);
	SaveDataEvent event {};
	CHECK(SaveDataGetEventResult(nullptr, &event) == OK);
	CHECK(event.type == 3 && event.error_code == OK && event.user_id == 1);
	CHECK(std::string(event.dir_name.data) == "sce_sdmemory1");
	CHECK(SaveDataGetEventResult(nullptr, &event) == SAVE_DATA_ERROR_NOT_FOUND);
	sync.option = 1;
	CHECK(SaveDataSyncSaveDataMemory(&sync) == OK);
	CHECK(SaveDataGetEventResult(nullptr, &event) == SAVE_DATA_ERROR_NOT_FOUND);
	sync.option = 2;
	CHECK(SaveDataSyncSaveDataMemory(&sync) == SAVE_DATA_ERROR_PARAMETER);

	Reset("OTHER");
	CHECK(Setup() == 0);
	CHECK(Get() == std::vector<uint8_t>(16));
	Reset("ISOLATE");
	CHECK(Setup() == 16);
	CHECK(Setup(2) == 16);
	CHECK(Setup(1, 1) == 16);
	CHECK(Get()[0] == 11 && Get(16, 2)[0] == 22 && Get(16, 1, 1)[0] == 33);
	Reset("OTHER");
	SceSaveDataTitleId title {};
	std::strcpy(title.data, "ISOLATE");
	SceSaveDataDirName name {};
	std::strcpy(name.data, "sce_sdmemory");
	struct SaveDataTransferringMount mount {};
	mount.user_id  = 2;
	mount.title_id = &title;
	mount.dir_name = &name;
	SaveDataMountResult result {};
	CHECK(SaveDataTransferringMount(&mount, &result) == OK);
	CHECK(g_mounted_directory == fs::path("_SaveData/ISOLATE/sce_sdmemory/2"));
	CHECK(std::ifstream(g_mounted_directory / "memory.dat", std::ios::binary).get() == 22);
	CHECK(SaveDataUmount2(0, &result.mount_point) == OK);
	struct SaveDataDelete del {};
	del.user_id  = 1;
	del.title_id = &title;
	del.dir_name = &name;
	CHECK(SaveDataDelete(&del) == OK);
	Reset("ISOLATE");
	CHECK(Setup() == 0);
	CHECK(Setup(2) == 16);
	CHECK(Get(16, 2)[0] == 22);
}

void TestFailedWritePreservesSave() {
	Reset("IOFAIL");
	CHECK(Setup() == 0);
	uint8_t byte = 42;
	CHECK(Set(&byte, 1) == OK);
	fs::rename("_SaveData", "save-backup");
	{
		std::ofstream blocker("_SaveData");
		blocker << "not a directory";
	}
	byte = 99;
	CHECK(Set(&byte, 1) < 0);
	CHECK(Get()[0] == 42);
	CHECK(fs::remove("_SaveData"));
	fs::rename("save-backup", "_SaveData");
	CHECK(SaveDataTerminate() == OK);
	CHECK(Setup() == 16);
	CHECK(Get()[0] == 42);
}

void WriteRestartFixture() {
	auto setup   = SetupParam(7, 3);
	setup.option = 1;
	std::array<uint8_t, 4> icon_bytes {1, 2, 3, 4};
	SaveDataIcon           icon {icon_bytes.data(), icon_bytes.size(), icon_bytes.size(), {}};
	setup.init_icon        = &icon;
	setup.icon_memory_size = icon_bytes.size();
	SaveDataParam param {};
	std::strcpy(param.title, "Astro progress");
	param.user_param = 17;
	setup.init_param = &param;
	CHECK(SaveDataSetupSaveDataMemory2(&setup, nullptr) == OK);
	std::array<uint8_t, 4> progress {11, 22, 33, 44};
	SaveDataMemoryData     data {progress.data(), progress.size(), 4, {}};
	SaveDataMemorySet2     set {};
	set.user_id = 7;
	set.slot_id = 3;
	set.data    = &data;
	set.icon    = &icon;
	CHECK(SaveDataSetSaveDataMemory2(&set) == OK);
	set.data         = nullptr;
	param.user_param = 18;
	std::strcpy(param.detail, "Coins, level state, rescued bots");
	set.param = &param;
	CHECK(SaveDataSetSaveDataMemory2(&set) == OK);
	// No Sync or Terminate: Set must survive process exit on its own.
}

void ReadRestartFixture() {
	CHECK(Setup(7, 3, 24, 1) == 16);
	auto expected = std::vector<uint8_t>(24);
	expected[4]   = 11;
	expected[5]   = 22;
	expected[6]   = 33;
	expected[7]   = 44;
	CHECK(Get(24, 7, 3) == expected);
	SaveDataParam          param {};
	std::array<uint8_t, 4> icon_bytes {9, 9, 9, 9};
	SaveDataIcon           icon {icon_bytes.data(), icon_bytes.size(), icon_bytes.size(), {}};
	SaveDataMemoryGet2     get {};
	get.user_id = 7;
	get.slot_id = 3;
	get.param   = &param;
	get.icon    = &icon;
	CHECK(SaveDataGetSaveDataMemory2(&get) == OK);
	CHECK(icon.data_size == 0);
	CHECK(
	    std::all_of(icon_bytes.begin(), icon_bytes.end(), [](uint8_t byte) { return byte == 9; }));
	CHECK(std::string(param.title) == "Astro progress");
	CHECK(std::string(param.detail) == "Coins, level state, rescued bots");
	CHECK(param.user_param == 18);
	CHECK(SaveDataTerminate() == OK);
	CHECK(Setup(7, 3, 6) == 16);
	expected.resize(6);
	CHECK(Get(6, 7, 3) == expected);
}

SceSaveDataDirName DirName(const char* text) {
	SceSaveDataDirName name {};
	std::snprintf(name.data, sizeof(name.data), "%s", text);
	return name;
}

std::vector<std::string> Search(int32_t user, const SceSaveDataTitleId* title = nullptr) {
	SaveDataDirNameSearchCond cond {};
	cond.user_id  = user;
	cond.title_id = title;
	std::array<SceSaveDataDirName, 8> names {};
	SaveDataDirNameSearchResult       result {};
	result.dir_names     = names.data();
	result.dir_names_num = names.size();
	CHECK(SaveDataDirNameSearch(&cond, &result) == OK);
	CHECK(result.hit_num == result.set_num && result.set_num <= names.size());
	std::vector<std::string> found;
	for (uint32_t i = 0; i < result.set_num; ++i) {
		found.emplace_back(names[i].data);
	}
	return found;
}

void TestClassicSavePaths() {
	Reset("CLASSIC");
	const std::vector<std::string> names {"save.1", "save1", "slot@A", "slotA"};
	const auto existing = DirName("save.1");
	struct SaveDataMount3 initial {};
	initial.user_id = 1;
	initial.dir_name = &existing;
	initial.mount_mode = 4;
	initial.blocks = 48;
	SaveDataMountResult first {};
	CHECK(SaveDataMount3(&initial, &first) == OK);
	std::ofstream(g_mounted_directory / "progress") << "save.1";
	CHECK(SaveDataUmount2(0, &first.mount_point) == OK);
	CHECK(Search(1) == std::vector<std::string> {"save.1"});
	for (const auto& text: names) {
		auto                  name = DirName(text.c_str());
		struct SaveDataMount3 mount {};
		mount.user_id    = 1;
		mount.dir_name   = &name;
		mount.mount_mode = text == "save.1" ? 1 : 4;
		mount.blocks     = 48;
		SaveDataMountResult result {};
		CHECK(SaveDataMount3(&mount, &result) == OK);
		CHECK(g_mounted_directory == fs::path("_SaveData") / "CLASSIC" / text);
		if (text != "save.1") {
			std::ofstream saved(g_mounted_directory / "progress");
			saved << text;
		}
		CHECK(SaveDataUmount2(0, &result.mount_point) == OK);
	}
	CHECK(Search(1) == names);
	SceSaveDataTitleId title {};
	std::strcpy(title.data, "CLASSIC");
	Reset("OTHER");
	CHECK(Search(1).empty());
	CHECK(Search(1, &title) == names);
	for (const auto& text: names) {
		auto                             name = DirName(text.c_str());
		struct SaveDataTransferringMount mount {};
		mount.user_id  = 1;
		mount.title_id = &title;
		mount.dir_name = &name;
		SaveDataMountResult result {};
		CHECK(SaveDataTransferringMount(&mount, &result) == OK);
		std::ifstream saved(g_mounted_directory / "progress");
		std::string   progress;
		saved >> progress;
		CHECK(progress == text);
		CHECK(SaveDataUmount2(0, &result.mount_point) == OK);
	}
	Reset("CLASSIC");
	for (const char* text: {"", ".", "..", "../save1", "save/1", "save\\1"}) {
		auto                  name = DirName(text);
		struct SaveDataMount3 mount {};
		mount.user_id    = 1;
		mount.dir_name   = &name;
		mount.mount_mode = 32;
		SaveDataMountResult result {};
		g_mounted_directory.clear();
		CHECK(SaveDataMount3(&mount, &result) == SAVE_DATA_ERROR_PARAMETER);
		CHECK(g_mounted_directory.empty());
		struct SaveDataDelete del {};
		del.user_id  = 1;
		del.dir_name = &name;
		CHECK(SaveDataDelete(&del) == SAVE_DATA_ERROR_PARAMETER);
	}
	CHECK(Search(1) == names);
}

void TestSaveAllocations() {
	Reset("CAPACITY");
	std::array<SceSaveDataDirName, 2> names {DirName("Options"), DirName("Player")};
	const std::array<uint64_t, 2> allocations {48, 96};
	for (const auto blocks : {0u, 47u, 16385u}) {
		struct SaveDataMount3 invalid {};
		invalid.user_id = 1;
		invalid.dir_name = &names[0];
		invalid.mount_mode = 4;
		invalid.blocks = blocks;
		SaveDataMountResult result {};
		CHECK(SaveDataMount3(&invalid, &result) == SAVE_DATA_ERROR_PARAMETER);
		CHECK(!fs::exists("_SaveData/CAPACITY/Options"));
	}
	for (size_t i = 0; i < names.size(); i++) {
		struct SaveDataMount3 mount {};
		mount.user_id = 1;
		mount.dir_name = &names[i];
		mount.mount_mode = 4;
		mount.blocks = allocations[i];
		SaveDataMountResult result {};
		CHECK(SaveDataMount3(&mount, &result) == OK);
		CHECK(result.mount_status == 1);
		std::ofstream(g_mounted_directory / "USR-DATA") << "saved payload";
		CHECK(SaveDataUmount2(0, &result.mount_point) == OK);
		SaveDataMountInfo info {};
		CHECK(SaveDataGetMountInfo(&result.mount_point, &info) == SAVE_DATA_ERROR_NOT_MOUNTED);
	}
	Reset("CAPACITY");
	std::array<SceSaveDataDirName, 2> found {};
	std::array<SaveDataSearchInfo, 2> infos {};
	SaveDataDirNameSearchCond cond {};
	cond.user_id = 1;
	SaveDataDirNameSearchResult search {};
	search.dir_names = found.data();
	search.dir_names_num = found.size();
	search.infos = infos.data();
	CHECK(SaveDataDirNameSearch(&cond, &search) == OK && search.set_num == 2);
	for (size_t i = 0; i < names.size(); i++) {
		CHECK(std::string(found[i].data) == names[i].data);
		CHECK(infos[i].blocks == allocations[i] && infos[i].free_blocks == allocations[i]);
		struct SaveDataMount3 mount {};
		mount.user_id = 1;
		mount.dir_name = &names[i];
		mount.mount_mode = 34;
		mount.blocks = 16384;
		SaveDataMountResult result {};
		CHECK(SaveDataMount3(&mount, &result) == OK && result.mount_status == 0);
		std::ofstream(g_mounted_directory / "USR-DATA", std::ios::app) << "more data";
		SaveDataMountInfo info {};
		CHECK(SaveDataGetMountInfo(&result.mount_point, &info) == OK);
		CHECK(info.blocks == allocations[i] && info.free_blocks == allocations[i]);
		CHECK(SaveDataUmount2(0, &result.mount_point) == OK);
	}
	CHECK(infos[0].blocks + infos[1].blocks == 144);
	const fs::path metadata = "_SaveData/CAPACITY/Options/sce_sys/blocks.bin";
	fs::resize_file(metadata, 7);
	CHECK(SaveDataDirNameSearch(&cond, &search) == SAVE_DATA_ERROR_BROKEN);
	CHECK(fs::file_size(metadata) == 7);
	CHECK(fs::remove(metadata));
	CHECK(SaveDataDirNameSearch(&cond, &search) == SAVE_DATA_ERROR_BROKEN);
	CHECK(!fs::exists(metadata));
}

void TestClassicSaveParams() {
	Reset("PARAMS");
	const auto name = DirName("slot");
	struct SaveDataMount3 mount {};
	mount.user_id    = 1;
	mount.dir_name   = &name;
	mount.mount_mode = 4;
	mount.blocks     = 48;
	SaveDataMountResult mounted {};
	CHECK(SaveDataMount3(&mount, &mounted) == OK);
	SaveDataParam param {};
	std::strcpy(param.title, "First save");
	std::strcpy(param.sub_title, "Chapter 2");
	std::strcpy(param.detail, "Progress");
	param.user_param = 42;
	CHECK(SaveDataSetParam(&mounted.mount_point, 0, nullptr, sizeof(param)) ==
	      SAVE_DATA_ERROR_PARAMETER);
	CHECK(SaveDataSetParam(&mounted.mount_point, 0, &param, sizeof(param) - 1) ==
	      SAVE_DATA_ERROR_PARAMETER);
	CHECK(SaveDataSetParam(&mounted.mount_point, 0, &param, sizeof(param)) == OK);
	const fs::path saved = "_SaveData/PARAMS/slot";
	CHECK(fs::exists(saved / "sce_sys/param.bin"));
	fs::create_directory(saved / "nested");
	std::ofstream(saved / "nested/progress") << "payload";
	SaveDataParam loaded {};
	size_t got = 0;
	CHECK(SaveDataGetParam(&mounted.mount_point, 0, &loaded, sizeof(loaded), &got) == OK);
	CHECK(got == sizeof(loaded) && std::string(loaded.title) == "First save");
	CHECK(std::string(loaded.sub_title) == "Chapter 2" && loaded.user_param == 42);
	CHECK(loaded.mtime > 0);
	fs::last_write_time(saved / "nested/progress",
	                    fs::last_write_time(saved / "sce_sys/param.bin") + std::chrono::hours(2));
	CHECK(SaveDataGetParam(&mounted.mount_point, 0, &loaded, sizeof(loaded), nullptr) == OK);
	const int64_t nested_mtime = loaded.mtime;
	CHECK(nested_mtime > static_cast<int64_t>(std::time(nullptr)) + 3600);
	fs::create_directory(saved / "sce_sys/param.bin.tmp");
	std::strcpy(param.title, "Failed update");
	CHECK(SaveDataSetParam(&mounted.mount_point, 0, &param, sizeof(param)) ==
	      SAVE_DATA_ERROR_INTERNAL);
	CHECK(fs::remove(saved / "sce_sys/param.bin.tmp"));
	CHECK(SaveDataGetParam(&mounted.mount_point, 0, &loaded, sizeof(loaded), nullptr) == OK);
	CHECK(std::string(loaded.title) == "First save");
	std::strcpy(param.title, "Updated save");
	CHECK(SaveDataSetParam(&mounted.mount_point, 0, &param, sizeof(param)) == OK);
	CHECK(SaveDataGetParam(&mounted.mount_point, 0, &loaded, sizeof(loaded), nullptr) == OK);
	CHECK(std::string(loaded.title) == "Updated save");
	CHECK(SaveDataUmount2(0, &mounted.mount_point) == OK);
	CHECK(SaveDataGetParam(&mounted.mount_point, 0, &loaded, sizeof(loaded), nullptr) ==
	      SAVE_DATA_ERROR_NOT_MOUNTED);

	Reset("OTHER");
	SceSaveDataTitleId source_title {};
	std::strcpy(source_title.data, "PARAMS");
	struct SaveDataTransferringMount transfer {};
	transfer.user_id  = 1;
	transfer.title_id = &source_title;
	transfer.dir_name = &name;
	SaveDataMountResult transferred {};
	CHECK(SaveDataTransferringMount(&transfer, &transferred) == OK);
	CHECK(SaveDataGetParam(&transferred.mount_point, 0, &loaded, sizeof(loaded), nullptr) == OK);
	CHECK(std::string(loaded.title) == "Updated save");
	std::error_code alias_error;
	fs::create_directory_symlink("PARAMS", "_SaveData/P_ALIAS", alias_error);
	if (!alias_error) {
		SceSaveDataTitleId alias_title {};
		std::strcpy(alias_title.data, "P_ALIAS");
		transfer.title_id = &alias_title;
		SaveDataMountResult duplicate {};
		CHECK(SaveDataTransferringMount(&transfer, &duplicate) == SAVE_DATA_ERROR_BUSY);
		transfer.title_id = &source_title;
		CHECK(fs::remove("_SaveData/P_ALIAS"));
	}
#ifdef _WIN32
	SceSaveDataTitleId lowercase_title {};
	std::strcpy(lowercase_title.data, "params");
	transfer.title_id = &lowercase_title;
	SaveDataMountResult duplicate {};
	CHECK(SaveDataTransferringMount(&transfer, &duplicate) == SAVE_DATA_ERROR_BUSY);
	transfer.title_id = &source_title;
#endif

	SaveDataMountResult other {};
	CHECK(SaveDataMount3(&mount, &other) == OK);
	std::strcpy(param.title, "Other title");
	CHECK(SaveDataSetParam(&other.mount_point, 0, &param, sizeof(param)) == OK);
	constexpr char typed_title[] = "Typed title";
	CHECK(SaveDataSetParam(&other.mount_point, 1, typed_title, sizeof(typed_title)) == OK);
	char title[32] {};
	CHECK(SaveDataGetParam(&other.mount_point, 1, title, sizeof(title), &got) == OK);
	CHECK(std::string(title) == typed_title && got == sizeof(typed_title));
	const uint32_t user_param = 77;
	CHECK(SaveDataSetParam(&other.mount_point, 4, &user_param, sizeof(user_param)) == OK);
	CHECK(SaveDataGetParam(&other.mount_point, 0, &loaded, sizeof(loaded), nullptr) == OK);
	CHECK(std::string(loaded.title) == typed_title && loaded.user_param == user_param);
	CHECK(SaveDataGetParam(&other.mount_point, 5, &loaded.mtime, sizeof(loaded.mtime), nullptr) == OK);
	CHECK(loaded.mtime > 0);
	CHECK(SaveDataSetParam(&other.mount_point, 5, &loaded.mtime, sizeof(loaded.mtime)) ==
	      SAVE_DATA_ERROR_PARAMETER);
	CHECK(SaveDataGetParam(&transferred.mount_point, 0, &loaded, sizeof(loaded), nullptr) == OK);
	CHECK(std::string(loaded.title) == "Updated save");
	CHECK(SaveDataUmount2(0, &other.mount_point) == OK);
	CHECK(SaveDataUmount2(0, &transferred.mount_point) == OK);

	std::array<SceSaveDataDirName, 1> names {};
	std::array<SaveDataParam, 1> params {};
	SaveDataDirNameSearchCond cond {};
	cond.user_id = 1;
	cond.title_id = &source_title;
	SaveDataDirNameSearchResult result {};
	result.dir_names = names.data();
	result.dir_names_num = names.size();
	result.params = params.data();
	CHECK(SaveDataDirNameSearch(&cond, &result) == OK && result.set_num == 1);
	CHECK(std::string(names[0].data) == "slot" && std::string(params[0].title) == "Updated save");
	CHECK(params[0].mtime == nested_mtime);
}

void RunChild(const fs::path& executable, const char* mode) {
#ifdef _WIN32
	CHECK(_spawnl(_P_WAIT, executable.string().c_str(), executable.string().c_str(), mode,
	              static_cast<char*>(nullptr)) == 0);
#else
	const pid_t pid = fork();
	CHECK(pid >= 0);
	if (pid == 0) {
		execl(executable.c_str(), executable.c_str(), mode, static_cast<char*>(nullptr));
		_exit(127);
	}
	int status = 0;
	CHECK(waitpid(pid, &status, 0) == pid);
	CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
#endif
}
} // namespace

int main(int argc, char** argv) {
	if (argc == 2) {
		if (std::string_view(argv[1]) == "write") {
			WriteRestartFixture();
		} else {
			CHECK(std::string_view(argv[1]) == "read");
			ReadRestartFixture();
		}
		return 0;
	}
	const auto executable = fs::absolute(argv[0]);
	const auto previous   = fs::current_path();
	const auto unique     = std::chrono::steady_clock::now().time_since_epoch().count();
	const auto temp = fs::temp_directory_path() / ("kyty_save_memory_" + std::to_string(unique));
	CHECK(fs::create_directories(temp));
	fs::current_path(temp);
	RunChild(executable, "write");
	RunChild(executable, "read");
	TestValidationAndScatter();
	TestIsolationAndSync();
	TestFailedWritePreservesSave();
	TestClassicSavePaths();
	TestSaveAllocations();
	TestClassicSaveParams();
	CHECK(SaveDataTerminate() == OK);
	fs::current_path(previous);
	fs::remove_all(temp);
	return 0;
}
