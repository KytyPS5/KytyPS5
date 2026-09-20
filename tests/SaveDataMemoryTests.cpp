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
} // namespace

namespace Loader {
bool SystemContentParamSfoGetString(const char*, std::string* value) {
	*value = g_test_title;
	return true;
}
void SymbolDatabase::Add(const SymbolResolve&, uint64_t, const std::string&) {}
namespace Timer {
Common::Time GetTime() {
	return Common::Time(0);
}
} // namespace Timer
} // namespace Loader

namespace Libs::LibKernel::FileSystem {
void Mount(const std::filesystem::path& directory, const std::string&) {
	g_mounted_directory = directory;
}
void Umount(const std::string&) {}
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
	fs::create_directories("_SaveData/CLASSIC/save.1");
	{
		std::ofstream saved("_SaveData/CLASSIC/save.1/progress");
		saved << "save.1";
	}
	CHECK(Search(1) == std::vector<std::string> {"save.1"});
	for (const auto& text: names) {
		auto                  name = DirName(text.c_str());
		struct SaveDataMount3 mount {};
		mount.user_id    = 1;
		mount.dir_name   = &name;
		mount.mount_mode = text == "save.1" ? 1 : 4;
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
	CHECK(SaveDataTerminate() == OK);
	fs::current_path(previous);
	fs::remove_all(temp);
	return 0;
}
