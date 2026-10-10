#include "UpdateInstaller.h"

#include <chrono>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <spawn.h>
#include <unistd.h>
extern char** environ;
#endif

namespace {
namespace fs = std::filesystem;

bool WaitForParent(unsigned long pid) {
#ifdef _WIN32
	HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid);
	if (!process) {
		return GetLastError() == ERROR_INVALID_PARAMETER;
	}
	const auto result = WaitForSingleObject(process, 120000);
	CloseHandle(process);
	return result == WAIT_OBJECT_0;
#else
	if (pid > static_cast<unsigned long>(std::numeric_limits<pid_t>::max())) {
		return false;
	}
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(2);
	while (std::chrono::steady_clock::now() < deadline) {
		if (kill(static_cast<pid_t>(pid), 0) != 0) {
			return errno == ESRCH;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}
	return false;
#endif
}

bool Launch(const fs::path& launcher, const fs::path& directory) {
#ifdef _WIN32
	std::wstring command = L"\"" + launcher.wstring() + L"\"";
	STARTUPINFOW startup {};
	startup.cb = sizeof(startup);
	PROCESS_INFORMATION process {};
	if (!CreateProcessW(launcher.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr,
	                    directory.c_str(), &startup, &process)) {
		return false;
	}
	CloseHandle(process.hThread);
	CloseHandle(process.hProcess);
	return true;
#else
	if (chdir(directory.c_str()) != 0) {
		return false;
	}
	setsid();
	pid_t child;
	char* arguments[] = {const_cast<char*>(launcher.c_str()), nullptr};
	return posix_spawn(&child, launcher.c_str(), nullptr, nullptr, arguments, environ) == 0;
#endif
}

void WriteResult(const fs::path& workspace, const fs::path& target,
                 const Kyty::Updater::Result& result) try {
	const auto    temporary = workspace / "update-result.txt";
	std::ofstream stream(temporary, std::ios::trunc);
	stream << (result.success ? "success\n" : "failure\n") << result.message;
	stream.close();
	if (!stream) {
		throw std::runtime_error("Cannot write the update result.");
	}
	const auto destination = target / ".kyty-update-result";
	// Never open the destination: rename safely replaces even an existing symlink.
	std::error_code ignored;
	fs::remove(destination, ignored);
	fs::rename(temporary, destination);
} catch (const std::exception& error) {
	// Reporting must never prevent the original or updated launcher restarting.
	std::cerr << "Cannot record update result: " << error.what() << '\n';
}

void CleanupWorkspace(const fs::path& source, const fs::path& target) try {
	const auto workspace = source.parent_path();
	if (source.filename() != "payload" ||
	    !workspace.filename().string().starts_with(".kyty-update-") ||
	    fs::canonical(workspace.parent_path()) != fs::canonical(target)) {
		return;
	}
	// Windows cannot unlink the running helper. Remove the archive and payload
	// separately so that only this small executable can remain after success.
	for (const auto& entry: fs::directory_iterator(workspace)) {
		std::error_code ignored;
		fs::remove_all(entry.path(), ignored);
	}
	std::error_code ignored;
	fs::remove(workspace, ignored);
} catch (const std::exception& error) {
	std::cerr << "Update installed; temporary files could not be removed: " << error.what() << '\n';
}

int Run(const std::vector<fs::path>& arguments) {
	const bool validate = arguments.size() == 5 && arguments[1] == "--validate";
	const bool install  = arguments.size() == 7 && arguments[1] == "--install";
	if (!validate && !install) {
		std::cerr
		    << "Usage: kyty_updater --validate SOURCE TARGET LAUNCHER_REL\n"
		       "       kyty_updater --install SOURCE TARGET LAUNCHER_REL PARENT_PID WORKDIR\n";
		return 2;
	}
	const auto source   = fs::absolute(arguments[2]);
	const auto target   = fs::absolute(arguments[3]);
	const auto launcher = arguments[4];
	if (validate) {
		const auto result = Kyty::Updater::Validate(source, target, launcher);
		if (!result.success) {
			std::cerr << result.message << '\n';
		}
		return result.success ? 0 : 1;
	}
	const auto  pid_string = arguments[5].string();
	std::size_t consumed   = 0;
	const auto  pid        = std::stoul(pid_string, &consumed);
	if (pid <= 1 || consumed != pid_string.size() || pid_string.front() == '-') {
		throw std::runtime_error("Invalid launcher process identifier.");
	}
	if (!WaitForParent(pid)) {
		WriteResult(source.parent_path(), target,
		            {false, "Update cancelled: the launcher did not exit within two minutes."});
		return 1;
	}
	auto result = Kyty::Updater::Install(source, target, launcher);
	WriteResult(source.parent_path(), target, result);
	if (!Launch(target / launcher, fs::absolute(arguments[6]))) {
		result.message += "\nThe launcher could not restart. Open it manually.";
		WriteResult(source.parent_path(), target, result);
		std::cerr << result.message << '\n';
		return 1;
	}
	if (result.success) {
		CleanupWorkspace(source, target);
	}
	return result.success ? 0 : 1;
}
} // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t* argv[]) {
#else
int main(int argc, char* argv[]) {
#endif
	try {
		return Run(std::vector<fs::path>(argv, argv + argc));
	} catch (const std::exception& error) {
		std::cerr << error.what() << '\n';
		return 1;
	}
}
