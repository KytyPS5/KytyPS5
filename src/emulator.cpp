#include "emulator.h"

#include "common/abi.h"
#include "common/assert.h"
#include "common/emulatorConfig.h"
#include "common/file.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "common/singleton.h"
#include "common/stringUtils.h"
#include "common/subsystems.h"
#include "common/systemInfo.h"
#include "common/threads.h"
#include "graphics/presentation/window.h"
#include "kernel/fileSystem.h"
#include "kernel/memory.h"
#include "kernel/pthread.h"
#include "kytyGitVersion.h"
#include "libs/agc.h"
#include "libs/audio.h"
#include "libs/controller.h"
#include "libs/libs.h"
#include "libs/errno.h"
#include <atomic>
#include <fstream>
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#include <windows.h>
#undef DeleteFile
#endif
#include "libs/network.h"
#include "loader/runtimeLinker.h"
#include "loader/systemContent.h"
#include "loader/timer.h"

#include <cstdlib>
#include <filesystem>
#include <thread>

namespace Emulator {


static std::vector<std::string> g_host_arguments;
static std::filesystem::path g_app0;
static std::atomic_flag g_restarting = ATOMIC_FLAG_INIT;

void SetHostArguments(int argc, char* argv[]) {
	g_host_arguments.assign(argv + 1, argv + argc);
}

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
// Quote for the Windows argv parser, never for a shell.
static std::wstring QuoteArgument(const std::wstring& argument) {
	std::wstring result = L"\"";
	size_t slashes = 0;
	for (auto ch : argument) {
		if (ch == L'\\') { ++slashes; continue; }
		result.append(slashes * (ch == L'"' ? 2 : 1), L'\\');
		slashes = 0;
		if (ch == L'"') result += L'\\';
		result += ch;
	}
	result.append(slashes * 2, L'\\');
	return result + L'"';
}
#endif

int LoadExec(const char* path, const char* const argv[]) {
	using namespace Libs;
	using namespace Libs::SystemService;
	if (path == nullptr || *path == '\0') return SYSTEM_SERVICE_ERROR_PARAMETER;
	LOGF("LoadExec: requested %s\n", path);
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	std::error_code ec;
	auto guest_path = Common::PathFromUtf8(Common::FixFilenameSlash(path));
	if (guest_path.is_relative()) guest_path = std::filesystem::path("/app0") / guest_path;
	const auto host_path = std::filesystem::weakly_canonical(
	    LibKernel::FileSystem::GetRealFilename(Common::PathToGenericString(guest_path)), ec);
	if (ec) return SYSTEM_SERVICE_ERROR_PARAMETER;
	const auto root = std::filesystem::weakly_canonical(g_app0, ec);
	if (ec) return SYSTEM_SERVICE_ERROR_PARAMETER;
	const auto relative = host_path.lexically_relative(root);
	// Only a real guest ELF in the current application's root is supported.
	// Reject host executables, traversal, archives, and nested roots explicitly.
	if (relative.empty() || relative.has_parent_path() ||
	    !std::filesystem::is_regular_file(host_path, ec) || ec) {
		LOGF("LoadExec: target is outside application root or missing\n");
		return SYSTEM_SERVICE_ERROR_PARAMETER;
	}
	std::ifstream elf(host_path, std::ios::binary);
	char magic[4] {};
	elf.read(magic, sizeof(magic));
	if (!elf || std::string_view(magic, 4) != std::string_view("\x7f" "ELF", 4))
		return SYSTEM_SERVICE_ERROR_PARAMETER;
	std::vector<std::string> guest_args;
	if (argv != nullptr) {
		for (size_t i = 0; argv[i] != nullptr; ++i) {
			if (i >= 32 || strnlen(argv[i], 4097) > 4096) return SYSTEM_SERVICE_ERROR_PARAMETER;
			guest_args.emplace_back(argv[i]);
			LOGF("LoadExec: argv[%zu] = %s\n", i, argv[i]);
		}
	}
	if (g_restarting.test_and_set()) return SYSTEM_SERVICE_ERROR_UNAVAILABLE;
	std::vector<std::string> args;
	for (size_t i = 0; i < g_host_arguments.size(); ++i) {
		const auto& a = g_host_arguments[i];
		if (a == "--game" || a == "--guest-arg" || a == "--wait-for-process") { ++i; continue; }
		args.push_back(a);
	}
	args.insert(args.end(), {"--game", Common::PathToString(host_path),
	                        "--wait-for-process", std::to_string(GetCurrentProcessId())});
	for (const auto& a : guest_args) args.insert(args.end(), {"--guest-arg", a});
	wchar_t executable[32768] {};
	const auto length = GetModuleFileNameW(nullptr, executable, 32768);
	if (length == 0 || length >= 32768) {
		g_restarting.clear(); return SYSTEM_SERVICE_ERROR_INTERNAL;
	}
	std::wstring command = QuoteArgument(executable);
	for (const auto& a : args) command += L" " + QuoteArgument(Common::PathFromUtf8(a).wstring());
	STARTUPINFOW startup {};
	startup.cb = sizeof(startup);
	PROCESS_INFORMATION process {};
	if (!CreateProcessW(executable, command.data(), nullptr, nullptr, FALSE,
	                    CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
		LOGF("LoadExec: process creation failed: %lu\n", GetLastError());
		g_restarting.clear(); return SYSTEM_SERVICE_ERROR_INTERNAL;
	}
	LOGF("LoadExec: handoff to %s (process %lu)\n", Common::PathToString(host_path).c_str(), process.dwProcessId);
	CloseHandle(process.hThread);
	CloseHandle(process.hProcess);
	std::fflush(nullptr);
	std::quick_exit(0);
#else
	return SYSTEM_SERVICE_ERROR_UNAVAILABLE;
#endif
}

static void PrintSystemInfo() {
	const Common::SystemInfo info = Common::GetSystemInfo();

#if defined(__APPLE__)
	static constexpr auto platform_name = "macOS";
#elif KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	static constexpr auto platform_name = "Windows";
#elif KYTY_PLATFORM == KYTY_PLATFORM_LINUX
	static constexpr auto platform_name = "Linux";
#else
	static constexpr auto platform_name = "Unknown";
#endif

	LOGF("Build\n"
	     "  version: %s\n\n"
	     "Host\n"
	     "  os:      %s\n"
	     "  cpu:     %s\n"
	     "  threads: %u\n\n",
	     KYTY_BUILD_LABEL, platform_name, info.ProcessorName.c_str(),
	     std::thread::hardware_concurrency());
}

static void KytyClose() {
	auto* rt = Common::Singleton<Loader::RuntimeLinker>::Instance();

	rt->Clear();

	LOGF("done!\n");

	Common::Subsystems::EmergencyShutdownActive();
}

static void MountOrCreateDir(const std::filesystem::path& dir, const std::string& point) {
	if (!Common::File::IsDirectoryExisting(dir)) {
		Common::File::CreateDirectories(dir);
	}

	EXIT_NOT_IMPLEMENTED(!Common::File::IsDirectoryExisting(dir));

	Libs::LibKernel::FileSystem::Mount(dir, point);
	auto dir_text = Common::PathToString(dir);
	LOGF("Mounted %s -> %s\n", point.c_str(), dir_text.c_str());
}

static void MountSandboxDirs() {
	std::string title_id;
	if (!Loader::SystemContentParamSfoGetString("TITLE_ID", &title_id) || title_id.empty()) {
		title_id = "UNKNOWN";
	}

	MountOrCreateDir("_DownloadData/" + title_id, "/download0");
	MountOrCreateDir("_TempData/" + title_id, "/temp0");
	MountOrCreateDir("_TempData/" + title_id, "/temp");
}

static bool ClearDirectoryContents(const std::filesystem::path& dir) {
	bool ok = true;

	for (const auto& entry: Common::File::GetDirEntries(dir)) {
		if (entry.name == "." || entry.name == "..") {
			continue;
		}

		auto path = dir / entry.name;

		if (entry.is_file) {
			Common::File::RemoveReadonly(path);
			ok = Common::File::DeleteFile(path) && ok;
		} else {
			ok = ClearDirectoryContents(path) && ok;
			ok = Common::File::DeleteDirectory(path) && ok;
		}
	}

	return ok;
}

static void ClearDebugTextureFolder() {
	const std::string debug_texture_folder = "_Textures";

	if (!Common::File::IsDirectoryExisting(debug_texture_folder)) {
		Common::File::CreateDirectories(debug_texture_folder);
		return;
	}

	if (!ClearDirectoryContents(debug_texture_folder)) {
		LOGF_COLOR(Log::Color::BrightYellow, "TextureDump: failed to completely clear %s\n",
		           debug_texture_folder.c_str());
	}
}

static void Init(const Config::ConfigOptions& cfg, const std::filesystem::path& param_json,
                 Common::Subsystems& subsystems) {
	EXIT_IF(!Common::Thread::IsMainThread());

	subsystems.Initialize<Config::Lifecycle>();
	Config::Load(cfg);
	subsystems.Initialize<Log::Lifecycle>();

	if (Common::File::IsFileExisting(param_json)) {
		Loader::SystemContentLoadParamSfo(param_json);
		if (const auto flexible_memory_size = Loader::SystemContentGetFlexibleMemorySize();
		    flexible_memory_size != 0) {
			Libs::LibKernel::Memory::SetFlexibleMemorySize(flexible_memory_size);
		}
	}

	// Initialization order is explicit; destruction is automatic and reversed.
	subsystems.Initialize<Loader::Timer::Lifecycle>();
	subsystems.Initialize<Libs::LibKernel::PthreadLifecycle>();
	subsystems.Initialize<Profiler::Lifecycle>();
	subsystems.Initialize<Libs::Network::Lifecycle>();
	subsystems.Initialize<Libs::LibKernel::Memory::Lifecycle>();
	subsystems.Initialize<Libs::LibKernel::FileSystem::Lifecycle>();
	subsystems.Initialize<Libs::Controller::Lifecycle>();
	subsystems.Initialize<Libs::Audio::Lifecycle>();
	subsystems.Initialize<Libs::Graphics::Lifecycle>();
}

static void LoadElf(const std::filesystem::path& elf, bool dbg_print_reloc = false,
                    const std::filesystem::path& save_name = {}) {
	auto* rt = Common::Singleton<Loader::RuntimeLinker>::Instance();

	auto* program = rt->LoadProgram(
	    Libs::LibKernel::FileSystem::GetRealFilename(Common::PathToGenericString(elf)));

	if (dbg_print_reloc) {
		program->dbg_print_reloc = true;
	}

	if (!save_name.empty()) {
		rt->SaveProgram(program, Libs::LibKernel::FileSystem::GetRealFilename(
		                             Common::PathToGenericString(save_name)));
	}
}

static void Execute(const std::filesystem::path& game_patch) {
	auto           patch_path = game_patch;
	Common::Thread guest_thread(
	    [](void* param) {
		    auto* rt = Common::Singleton<Loader::RuntimeLinker>::Instance();
		    rt->Execute(*static_cast<const std::filesystem::path*>(param));
	    },
	    &patch_path);
	Libs::Graphics::WindowRun();
	std::quick_exit(0);
}

void Run(const RunOptions& options) {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	if (options.wait_for_process != 0) {
		HANDLE previous = OpenProcess(SYNCHRONIZE, FALSE, options.wait_for_process);
		if (previous != nullptr) {
			const auto waited = WaitForSingleObject(previous, 30000);
			CloseHandle(previous);
			if (waited != WAIT_OBJECT_0) return;
		}
	}
#endif
	g_app0 = options.app0_dir;
	if (options.app0_dir.empty()) {
		EXIT("app0 directory is required\n");
	}

	if (options.elf.empty()) {
		EXIT("ELF is required\n");
	}

	const auto         param_json = options.app0_dir / "sce_sys" / "param.json";
	Common::Subsystems subsystems(true);
	Init(options.config, param_json, subsystems);

	ClearDebugTextureFolder();

	PrintSystemInfo();
	std::string title_id;
	if (Loader::SystemContentParamSfoGetString("TITLE_ID", &title_id) && !title_id.empty()) {
		Log::WriteToConsoleAndLog(fmt::format("Title ID: {}\n", title_id));
	}

	int ok = atexit(KytyClose);
	EXIT_NOT_IMPLEMENTED(ok != 0);

	// Guest threads are still running, so skip KytyClose() and only flush emergency state.
	ok = at_quick_exit(Common::Subsystems::EmergencyShutdownActive);
	EXIT_NOT_IMPLEMENTED(ok != 0);

	Libs::LibKernel::FileSystem::Mount(options.app0_dir, "/app0");
	Libs::LibKernel::FileSystem::Mount(options.app0_dir, "/hostapp");

	MountSandboxDirs();

	auto* rt = Common::Singleton<Loader::RuntimeLinker>::Instance();
	Libs::InitAll(rt->Symbols());

	LoadElf(options.elf);

	Execute(options.game_patch);
}

} // namespace Emulator
