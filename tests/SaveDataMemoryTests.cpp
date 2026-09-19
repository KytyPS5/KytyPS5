#include "libs/saveDataMemory.h"
#include "libs/errno.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {
using namespace Libs::SaveData;
namespace fs = std::filesystem;
constexpr auto Title = "PPSA21564";
constexpr size_t MemorySize = 16384;
using Stage = MemoryTesting::Stage;
Stage g_stage;
int g_calls = 0;
int g_fail_on = 1;
bool g_crash = false;

void Check(bool condition, const char* message) {
	if (!condition) { std::fprintf(stderr, "SaveDataMemoryTests: %s\n", message); std::abort(); }
}
void Expect(int actual, int expected, const char* message) {
	if (actual != expected) {
		std::fprintf(stderr, "%s: got %08x, expected %08x\n", message, unsigned(actual), unsigned(expected));
		std::abort();
	}
}
bool Fail(Stage stage) {
	if (stage != g_stage || ++g_calls != g_fail_on) return false;
	if (g_crash) std::_Exit(73);
	return true;
}
void Inject(Stage stage, int occurrence = 1, bool crash = false) {
	g_stage = stage; g_calls = 0; g_fail_on = occurrence; g_crash = crash;
	MemoryTesting::SetFaultHook(Fail);
}
void NoFault() { MemoryTesting::SetFaultHook(nullptr); }

SaveDataMemorySetup2 SetupRequest(int32_t user = 1, uint32_t slot = 0) {
	SaveDataMemorySetup2 request {};
	request.user_id = user; request.slot_id = slot;
	request.memory_size = MemorySize; request.icon_memory_size = 128;
	return request;
}
void Setup(SaveDataMemoryStore& store, int32_t user = 1, uint32_t slot = 0, std::string_view title = Title) {
	auto request = SetupRequest(user, slot);
	Expect(store.Setup(title, &request, nullptr), OK, "setup");
}
int Write(SaveDataMemoryStore& store, uint8_t value, int32_t user = 1, uint32_t slot = 0,
          std::string_view title = Title) {
	std::vector<uint8_t> bytes(MemorySize, value);
	SaveDataMemoryData data {bytes.data(), bytes.size(), 0, {}};
	SaveDataParam param {};
	param.user_param = value;
	std::snprintf(param.title, sizeof(param.title), "save-%u", value);
	std::array<uint8_t, 8> image; image.fill(value);
	SaveDataIcon icon {image.data(), image.size(), 0, {}};
	SaveDataMemorySet2 request {};
	request.user_id = user; request.slot_id = slot; request.data = &data;
	request.param = &param; request.icon = &icon;
	return store.Set(title, &request);
}
void Verify(SaveDataMemoryStore& store, uint8_t value, int32_t user = 1, uint32_t slot = 0,
            std::string_view title = Title) {
	std::vector<uint8_t> bytes(MemorySize, 255);
	SaveDataMemoryData data {bytes.data(), bytes.size(), 0, {}};
	SaveDataParam param {};
	std::array<uint8_t, 8> image {};
	SaveDataIcon icon {image.data(), image.size(), 0, {}};
	SaveDataMemoryGet2 request {};
	request.user_id = user; request.slot_id = slot; request.data = &data;
	request.param = &param; request.icon = &icon;
	Expect(store.Get(title, &request), OK, "get snapshot");
	for (auto byte : bytes) Check(byte == value, "data survived as a complete snapshot");
	Check(param.user_param == value, "metadata belongs to the same snapshot");
	Check(icon.data_size == image.size(), "icon length survived");
	for (auto byte : image) Check(byte == value, "icon belongs to the same snapshot");
}
fs::path DataPath(const fs::path& root) {
	return root / Title / MemoryDirectoryName(0) / "1" / "memory.sdm";
}
std::vector<char> ReadDisk(const fs::path& path) {
	std::ifstream input(path, std::ios::binary);
	return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
void WriteDisk(const fs::path& path, const std::vector<char>& bytes) {
	std::ofstream output(path, std::ios::binary | std::ios::trunc);
	output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
	Check(bool(output), "test fixture write");
}

int Spawn(const fs::path& executable, const fs::path& root, const std::string& mode) {
#ifdef _WIN32
	std::wstring command = L"\"" + executable.wstring() + L"\" \"" + root.wstring() + L"\" " +
	                       std::wstring(mode.begin(), mode.end());
	STARTUPINFOW startup {}; startup.cb = sizeof(startup);
	PROCESS_INFORMATION process {};
	Check(CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
	                     nullptr, nullptr, &startup, &process) != 0, "spawn child");
	Check(WaitForSingleObject(process.hProcess, 30000) == WAIT_OBJECT_0, "child finished");
	DWORD status = 0;
	Check(GetExitCodeProcess(process.hProcess, &status) != 0, "child exit code");
	CloseHandle(process.hThread); CloseHandle(process.hProcess);
	return static_cast<int>(status);
#else
	const auto child = fork();
	Check(child >= 0, "fork");
	if (child == 0) { execl(executable.c_str(), executable.c_str(), root.c_str(), mode.c_str(), nullptr); std::_Exit(99); }
	int status = 0;
	Check(waitpid(child, &status, 0) == child && WIFEXITED(status), "child finished");
	return WEXITSTATUS(status);
#endif
}

int Child(const fs::path& root, const std::string& mode) {
	SaveDataMemoryStore store(root);
	if (mode == "locked") {
		auto request = SetupRequest();
		Expect(store.Setup(Title, &request, nullptr), SAVE_DATA_ERROR_BUSY, "other process cannot own live slot");
		return 0;
	}
	Setup(store);
	if (mode == "verify-old") { Verify(store, 17); return 0; }
	if (mode == "verify-new") { Verify(store, 29); return 0; }
	if (mode == "write") { Expect(Write(store, 17), OK, "write without shutdown"); std::_Exit(0); }
	if (mode == "crash-write") Inject(Stage::Write, 2, true);
	else if (mode == "crash-flush") Inject(Stage::Flush, 1, true);
	else if (mode == "crash-replace") Inject(Stage::Replace, 1, true);
	else if (mode == "crash-published") Inject(Stage::DirectoryFlush, 1, true);
	else return 99;
	Write(store, 29);
	return 98;
}

void TestValidation(const fs::path& root) {
	SaveDataMemoryStore store(root);
	Expect(store.Setup(Title, nullptr, nullptr), SAVE_DATA_ERROR_PARAMETER, "null setup");
	Expect(store.Get(Title, nullptr), SAVE_DATA_ERROR_PARAMETER, "null get");
	Expect(store.Set(Title, nullptr), SAVE_DATA_ERROR_PARAMETER, "null set");
	Expect(store.Sync(Title, nullptr), SAVE_DATA_ERROR_PARAMETER, "null sync");
	auto setup = SetupRequest();
	const std::string too_long(65, 'A');
	for (auto title : {"", ".", "..", "PPSA../x1", "../PPSA12", "PPSA2156/", "PPSA.21564", "a\\b",
	                   too_long.c_str()})
		Expect(store.Setup(title, &setup, nullptr), SAVE_DATA_ERROR_PARAMETER, "unsafe title");
	setup.slot_id = 4;
	Expect(store.Setup(Title, &setup, nullptr), SAVE_DATA_ERROR_PARAMETER, "invalid slot");
	setup = SetupRequest(); setup.memory_size = SIZE_MAX;
	Expect(store.Setup(Title, &setup, nullptr), SAVE_DATA_ERROR_PARAMETER, "size overflow");
	setup.memory_size = 0;
	Expect(store.Setup(Title, &setup, nullptr), SAVE_DATA_ERROR_PARAMETER, "zero capacity");
	setup = SetupRequest(); setup.option = 4;
	Expect(store.Setup(Title, &setup, nullptr), SAVE_DATA_ERROR_PARAMETER, "unknown option");
	setup = SetupRequest(-1);
	Expect(store.Setup(Title, &setup, nullptr), SAVE_DATA_ERROR_INVALID_LOGIN_USER, "no login user");
	SaveDataMemoryGet2 get {}; get.user_id = 1;
	SaveDataMemorySet2 set {}; set.user_id = 1;
	SaveDataMemorySync sync {1, 0, 1, {}};
	SaveDataMemoryGet2 no_user = get; no_user.user_id = -1;
	SaveDataMemorySet2 no_user_set = set; no_user_set.user_id = -1;
	SaveDataMemorySync no_user_sync {-1, 0, 1, {}};
	Expect(store.Get(Title, &no_user), SAVE_DATA_ERROR_INVALID_LOGIN_USER, "get without login user");
	Expect(store.Set(Title, &no_user_set), SAVE_DATA_ERROR_INVALID_LOGIN_USER, "set without login user");
	Expect(store.Sync(Title, &no_user_sync), SAVE_DATA_ERROR_INVALID_LOGIN_USER, "sync without login user");
	Expect(store.Get(Title, &get), SAVE_DATA_ERROR_MEMORY_NOT_READY, "get before setup");
	Expect(store.Set(Title, &set), SAVE_DATA_ERROR_MEMORY_NOT_READY, "set before setup");
	Expect(store.Sync(Title, &sync), SAVE_DATA_ERROR_MEMORY_NOT_READY, "sync before setup");
	setup = SetupRequest();
	SaveDataMemorySetupResult result {}; result.existed_memory_size = SIZE_MAX;
	Expect(store.Setup(Title, &setup, &result), OK, "new setup");
	Check(result.existed_memory_size == 0, "new save reports no prior data");
	Expect(Write(store, 17), OK, "seed");
	const auto original = ReadDisk(DataPath(root));
	uint8_t byte = 29;
	std::array<SaveDataMemoryData, 2> data {{{&byte, 1, 0, {}}, {&byte, SIZE_MAX, INT64_MAX, {}}}};
	set.data = data.data(); set.data_num = 2;
	Expect(store.Set(Title, &set), SAVE_DATA_ERROR_PARAMETER, "invalid second range rejects entire batch");
	Verify(store, 17);
	Check(ReadDisk(DataPath(root)) == original, "invalid batch did not persist a partial update");
	set.data_num = SAVE_DATA_MEMORY_DATA_NUM_MAX + 1;
	Expect(store.Set(Title, &set), SAVE_DATA_ERROR_PARAMETER, "more entries than the API allows");
	set.data_num = 2;
	SaveDataMemorySet2 empty {}; empty.user_id = 1;
	Expect(store.Set(Title, &empty), OK, "set with nothing to update");
	Check(ReadDisk(DataPath(root)) == original, "empty update did not rewrite the save");
	SaveDataParam stamped {};
	SaveDataMemoryGet2 stamp_get {}; stamp_get.user_id = 1; stamp_get.param = &stamped;
	Expect(store.Get(Title, &stamp_get), OK, "read stamped metadata");
	Check(stamped.mtime > 0, "the store stamps the modification time of an update");
	get.data = &data[1];
	Expect(store.Get(Title, &get), SAVE_DATA_ERROR_PARAMETER, "overflowing get");
	data[1] = {&byte, 1, -1, {}};
	Expect(store.Get(Title, &get), SAVE_DATA_ERROR_PARAMETER, "negative offset");
	data[1] = {nullptr, 0, MemorySize, {}};
	Expect(store.Get(Title, &get), OK, "empty range at end");
	SaveDataIcon icon {nullptr, 10, 0, {}}; set.icon = &icon; set.data = nullptr;
	Expect(store.Set(Title, &set), SAVE_DATA_ERROR_PARAMETER, "null icon bytes");
	sync.option = 2;
	Expect(store.Sync(Title, &sync), SAVE_DATA_ERROR_PARAMETER, "unknown sync option");
	store.Clear();
	Expect(store.Setup(Title, &setup, &result), OK, "reopen");
	Check(result.existed_memory_size == MemorySize, "reopen reports prior capacity");
	Verify(store, 17);
}

// A title id is whatever param.sfo carries: homebrew, system titles and the "UNKNOWN" fallback all
// have to be able to save. Only components that could escape the root are refused.
void TestTitleIds(const fs::path& root) {
	SaveDataMemoryStore store(root);
	for (auto title : {"UNKNOWN", "NPXS20001", "CUSA12345", "homebrew-1_0"}) {
		auto setup = SetupRequest();
		Expect(store.Setup(title, &setup, nullptr), OK, "ordinary title id accepted");
		Expect(Write(store, 42, 1, 0, title), OK, "ordinary title id saves");
		Verify(store, 42, 1, 0, title);
	}
}

// init_param and init_icon are only read when the caller declares them with SET_PARAM. A title
// that never asked for them may leave the pointers uninitialized, so following one would be a read
// of whatever the guest happened to leave in the field.
void TestSetupParamOption(const fs::path& root) {
	SaveDataMemoryStore store(root);
	SaveDataParam initial {};
	initial.user_param = 99;
	std::snprintf(initial.title, sizeof(initial.title), "initial");
	std::array<uint8_t, 4> image {1, 2, 3, 4};
	SaveDataIcon initial_icon {image.data(), image.size(), 0, {}};

	auto ignored = SetupRequest(1, 0);
	ignored.init_param = &initial; ignored.init_icon = &initial_icon;
	Expect(store.Setup(Title, &ignored, nullptr), OK, "setup without SET_PARAM");
	SaveDataParam read {};
	SaveDataMemoryGet2 get {}; get.user_id = 1; get.param = &read;
	Expect(store.Get(Title, &get), OK, "read back metadata");
	Check(read.user_param == 0, "init_param not read without SET_PARAM");

	auto applied = SetupRequest(1, 1);
	applied.option = SAVE_DATA_MEMORY_SET_PARAM;
	applied.init_param = &initial; applied.init_icon = &initial_icon;
	Expect(store.Setup(Title, &applied, nullptr), OK, "setup with SET_PARAM");
	read = {};
	std::array<uint8_t, 4> icon_bytes {};
	SaveDataIcon icon {icon_bytes.data(), icon_bytes.size(), 0, {}};
	SaveDataMemoryGet2 applied_get {};
	applied_get.user_id = 1; applied_get.slot_id = 1; applied_get.param = &read; applied_get.icon = &icon;
	Expect(store.Get(Title, &applied_get), OK, "read back initialized metadata");
	Check(read.user_param == 99, "init_param applied with SET_PARAM");
	Check(icon.data_size == image.size() && icon_bytes == image, "init_icon applied with SET_PARAM");

	// Initialization data never replaces what a title has already written.
	Expect(Write(store, 7, 1, 1), OK, "overwrite initialized slot");
	store.Clear();
	Expect(store.Setup(Title, &applied, nullptr), OK, "reopen initialized slot");
	Verify(store, 7, 1, 1);
}

void TestIsolation(const fs::path& root, const fs::path& executable) {
	SaveDataMemoryStore store(root);
	for (int user = 1; user <= 2; user++) {
		for (unsigned slot = 0; slot < 4; slot++) {
			Setup(store, user, slot);
			Expect(Write(store, uint8_t(user * 10 + slot), user, slot), OK, "write isolated slot");
		}
	}
	Setup(store, 1, 0, "PPSA21565");
	Expect(Write(store, 88, 1, 0, "PPSA21565"), OK, "other title");
	Check(Spawn(executable, root, "locked") == 0, "cross-process exclusion");
	store.Clear();
	for (int user = 1; user <= 2; user++) {
		for (unsigned slot = 0; slot < 4; slot++) {
			Setup(store, user, slot);
			Verify(store, uint8_t(user * 10 + slot), user, slot);
		}
	}
	Setup(store, 1, 0, "PPSA21565"); Verify(store, 88, 1, 0, "PPSA21565");
}

void TestFailures(const fs::path& root) {
	SaveDataMemoryStore store(root); Setup(store);
	Expect(Write(store, 17), OK, "seed failure tests");
	for (auto stage : {Stage::Create, Stage::Write, Stage::Flush, Stage::Replace}) {
		Inject(stage, stage == Stage::Write ? 2 : 1);
		Expect(Write(store, 29), SAVE_DATA_ERROR_INTERNAL, "I/O failure reaches caller");
		NoFault(); Verify(store, 17);
		store.Clear(); Setup(store); Verify(store, 17);
	}
	Inject(Stage::DirectoryFlush);
	Expect(Write(store, 29), SAVE_DATA_ERROR_INTERNAL, "post-replacement flush failure");
	NoFault(); Verify(store, 29);
	SaveDataMemorySync sync {1, 0, 1, {}};
	Expect(store.Sync(Title, &sync), OK, "retry failed durability barrier");
	store.Clear(); Setup(store); Verify(store, 29);
	store.Clear(); Inject(Stage::Read);
	auto setup = SetupRequest();
	Expect(store.Setup(Title, &setup, nullptr), SAVE_DATA_ERROR_INTERNAL, "read error is not new-save success");
	NoFault(); Setup(store); Verify(store, 29); store.Clear();
	const auto original = ReadDisk(DataPath(root));
	for (int kind = 0; kind < 3; kind++) {
		auto corrupt = original;
		if (kind == 0) corrupt.resize(10);
		if (kind == 1) corrupt.back() ^= 1;
		if (kind == 2) corrupt.push_back(0);
		WriteDisk(DataPath(root), corrupt);
		Expect(store.Setup(Title, &setup, nullptr), SAVE_DATA_ERROR_BROKEN, "corrupt save rejected");
		Check(ReadDisk(DataPath(root)) == corrupt, "corrupt save not overwritten");
	}
	WriteDisk(DataPath(root), original); Setup(store); Verify(store, 29);
}

#ifdef _WIN32
// Windows refuses to replace a file while someone else holds a handle that does not permit
// deletion, which is what a virus scanner or search indexer does for a moment after every close.
// Reporting that as a failed save loses the title's progress over a condition that clears itself,
// so the store waits it out. The handle here stands in for the scanner.
void TestSharingConflict(const fs::path& root) {
	SaveDataMemoryStore store(root);
	Setup(store);
	Expect(Write(store, 11), OK, "seed sharing test");
	const HANDLE blocker = CreateFileW(DataPath(root).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
	                                   OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	Check(blocker != INVALID_HANDLE_VALUE, "opened the save the way a scanner would");
	std::thread release([blocker] {
		std::this_thread::sleep_for(std::chrono::milliseconds(150));
		CloseHandle(blocker);
	});
	Expect(Write(store, 12), OK, "replacement outlasts a transient sharing conflict");
	release.join();
	Verify(store, 12);
	store.Clear();
	Setup(store);
	Verify(store, 12);
}
#endif

void TestConcurrency(const fs::path& root) {
	SaveDataMemoryStore store(root); Setup(store);
	std::vector<std::thread> threads;
	for (int i = 0; i < 8; i++) threads.emplace_back([&, i] {
		uint8_t value = uint8_t(i + 1);
		SaveDataMemoryData data {&value, 1, i, {}};
		SaveDataMemorySet2 set {}; set.user_id = 1; set.data = &data;
		for (int repeat = 0; repeat < 4; repeat++) Expect(store.Set(Title, &set), OK, "concurrent partial write");
	});
	for (auto& thread : threads) thread.join();
	store.Clear(); Setup(store);
	std::array<uint8_t, 8> bytes {};
	SaveDataMemoryData data {bytes.data(), bytes.size(), 0, {}};
	SaveDataMemoryGet2 get {}; get.user_id = 1; get.data = &data;
	Expect(store.Get(Title, &get), OK, "read concurrent updates");
	for (size_t i = 0; i < bytes.size(); i++) Check(bytes[i] == i + 1, "no lost partial update");
}

void TestRestarts(const fs::path& root, const fs::path& executable) {
	Check(Spawn(executable, root, "write") == 0, "write child");
	Check(Spawn(executable, root, "verify-old") == 0, "fresh process loads save without terminate");
	for (auto mode : {"crash-write", "crash-flush", "crash-replace"}) {
		Check(Spawn(executable, root, mode) == 73, "interrupted child reached fault boundary");
		Check(Spawn(executable, root, "verify-old") == 0, "old complete save survives interruption");
	}
	Check(Spawn(executable, root, "crash-published") == 73, "crash after replacement");
	Check(Spawn(executable, root, "verify-new") == 0, "new complete save survives publication");
}
} // namespace

int main(int argc, char** argv) {
	if (argc == 3) return Child(fs::path(argv[1]), argv[2]);
	const auto path = fs::temp_directory_path() /
	                  ("kyty-save-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	Check(fs::create_directory(path), "unique test directory");
	const auto executable = fs::absolute(argv[0]);
	TestValidation(path / "validation");
	TestTitleIds(path / "titles");
	TestSetupParamOption(path / "options");
	TestIsolation(path / "isolation", executable);
	TestFailures(path / "failures");
#ifdef _WIN32
	TestSharingConflict(path / "sharing");
#endif
	TestConcurrency(path / "concurrency");
	TestRestarts(path / "restarts", executable);
	fs::remove_all(path);
	std::puts("SaveDataMemoryTests: all passed (validation, title ids, setup options, isolation, "
	          "faults, concurrency, process restarts)");
}
