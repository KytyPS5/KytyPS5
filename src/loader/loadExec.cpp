#include "common/file.h"
#include "common/logging/log.h"
#include "emulator.h"
#include "kernel/fileSystem.h"
#include "libs/errno.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string_view>
#include <vector>

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif defined(__linux__)
#include <linux/close_range.h>
#include <unistd.h>
#endif

namespace Emulator {

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
static std::wstring QuoteArgument(std::wstring_view argument) {
	std::wstring quoted  = L"\"";
	size_t       slashes = 0;
	for (wchar_t c: argument) {
		if (c == L'\\') {
			++slashes;
			continue;
		}
		quoted.append(c == L'"' ? slashes * 2 + 1 : slashes, L'\\');
		quoted += c;
		slashes = 0;
	}
	quoted.append(slashes * 2, L'\\');
	return quoted + L'"';
}
#endif

int LoadExec(const char* path, const char* const argv[]) {
	using namespace Libs::SystemService;
	if (path == nullptr || !std::string_view(path).starts_with("/app0/") ||
	    !Common::File::IsFileExisting(Libs::LibKernel::FileSystem::GetRealFilename(path))) {
		return SYSTEM_SERVICE_ERROR_PARAMETER;
	}
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS || defined(__linux__)
	std::vector<std::string> arguments {"--guest-exec", path};
	size_t                   bytes = 0;
	for (size_t i = 0; argv != nullptr && argv[i] != nullptr; ++i) {
		const auto size = strnlen(argv[i], 4096 - bytes) + 1;
		if (size > 4096 - bytes) {
			return SYSTEM_SERVICE_ERROR_PARAMETER;
		}
		bytes += size;
		arguments.emplace_back(argv[i], size - 1);
	}
	LOGF("LoadExec: %s\n", path);
	Log::Flush();
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	wchar_t    executable[32768] {};
	const auto length = GetModuleFileNameW(nullptr, executable, 32768);
	if (length == 0 || length >= 32768) {
		return SYSTEM_SERVICE_ERROR_INTERNAL;
	}
	if (__wargv == nullptr) {
		return SYSTEM_SERVICE_ERROR_INTERNAL;
	}
	std::wstring command = QuoteArgument(executable);
	for (int i = 1; i < __argc && __wargv[i] != nullptr; ++i) {
		if (std::wstring_view(__wargv[i]) == L"--guest-exec") {
			break;
		}
		command += L" " + QuoteArgument(__wargv[i]);
	}
	for (const auto& argument: arguments) {
		const int size =
		    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, argument.c_str(), -1, nullptr, 0);
		if (size == 0) {
			return SYSTEM_SERVICE_ERROR_PARAMETER;
		}
		std::wstring wide(size, L'\0');
		if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, argument.c_str(), -1, wide.data(),
		                        size) == 0) {
			return SYSTEM_SERVICE_ERROR_PARAMETER;
		}
		wide.resize(size - 1);
		command += L" " + QuoteArgument(wide);
	}
	STARTUPINFOW startup {};
	startup.cb = sizeof(startup);
	PROCESS_INFORMATION process {};
	if (!CreateProcessW(executable, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
	                    nullptr, nullptr, &startup, &process)) {
		LOGF("LoadExec: process creation failed: %lu\n", GetLastError());
		return SYSTEM_SERVICE_ERROR_INTERNAL;
	}
	CloseHandle(process.hThread);
	CloseHandle(process.hProcess);
	std::fflush(nullptr);
	std::quick_exit(0);
#else
	std::ifstream            command("/proc/self/cmdline", std::ios::binary);
	std::vector<std::string> host_arguments;
	for (std::string argument;
	     std::getline(command, argument, '\0') && argument != "--guest-exec";) {
		host_arguments.push_back(std::move(argument));
	}
	if (host_arguments.empty()) {
		return SYSTEM_SERVICE_ERROR_INTERNAL;
	}
	host_arguments.insert(host_arguments.end(), arguments.begin(), arguments.end());
	std::vector<char*> host_argv;
	for (auto& argument: host_arguments) {
		host_argv.push_back(argument.data());
	}
	host_argv.push_back(nullptr);
	if (close_range(3, ~0u, CLOSE_RANGE_UNSHARE | CLOSE_RANGE_CLOEXEC) == 0) {
		execvp(host_argv[0], host_argv.data());
	}
	LOGF("LoadExec failed: %s\n", std::strerror(errno));
#endif
	return SYSTEM_SERVICE_ERROR_INTERNAL;
#else
	return SYSTEM_SERVICE_ERROR_UNAVAILABLE;
#endif
}

} // namespace Emulator
