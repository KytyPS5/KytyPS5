#include "common/file.h"
#include "common/logging/log.h"
#include "emulator.h"
#include "kernel/fileSystem.h"
#include "libs/errno.h"

#include <algorithm>
#include <cerrno>
#include <cstdint>
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
	// Shown on the console too: the output that follows comes from the new program.
	auto message = fmt::format("LoadExec: switching to {}", path);
	for (size_t i = 2; i < arguments.size(); ++i) {
		message += fmt::format(" \"{}\"", arguments[i]);
	}
	Log::WriteToConsoleAndLog(message + "\n");
	// The new program writes to the same outputs, so everything written so far goes out first.
	// Neither exec nor quick_exit writes out buffered streams.
	Log::Flush();
	std::fflush(nullptr);
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
	// The new process writes where this one does: to its console, or to the files or pipes its
	// output was redirected to. Only the standard handles are inherited.
	HANDLE std_handles[3] = {GetStdHandle(STD_INPUT_HANDLE), GetStdHandle(STD_OUTPUT_HANDLE),
	                         GetStdHandle(STD_ERROR_HANDLE)};
	std::vector<HANDLE> inherited;
	for (auto& handle: std_handles) {
		if (handle == nullptr || handle == INVALID_HANDLE_VALUE ||
		    !SetHandleInformation(handle, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT)) {
			handle = nullptr;
		} else if (std::find(inherited.begin(), inherited.end(), handle) == inherited.end()) {
			inherited.push_back(handle);
		}
	}
	SIZE_T attributes_size = 0;
	InitializeProcThreadAttributeList(nullptr, 1, 0, &attributes_size);
	std::vector<uint8_t> attributes_storage(attributes_size);
	auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes_storage.data());
	const bool inherit =
	    !inherited.empty() && InitializeProcThreadAttributeList(attributes, 1, 0, &attributes_size);
	const bool listed =
	    inherit && UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
	                                         inherited.data(), inherited.size() * sizeof(HANDLE),
	                                         nullptr, nullptr);
	STARTUPINFOEXW startup {};
	startup.StartupInfo.cb = sizeof(startup);
	if (listed) {
		startup.lpAttributeList        = attributes;
		startup.StartupInfo.dwFlags    = STARTF_USESTDHANDLES;
		startup.StartupInfo.hStdInput  = std_handles[0];
		startup.StartupInfo.hStdOutput = std_handles[1];
		startup.StartupInfo.hStdError  = std_handles[2];
	}
	// A process with a console shares it with the new one; without one, none is created.
	const DWORD flags = (listed ? EXTENDED_STARTUPINFO_PRESENT : 0u) |
	                    (GetConsoleWindow() != nullptr ? 0u : static_cast<DWORD>(CREATE_NO_WINDOW));
	// The new process continues this one's log file; it inherits the environment.
	(void)SetEnvironmentVariableA(Log::RelaunchedVariable, "1");
	PROCESS_INFORMATION process {};
	const bool          created =
	    CreateProcessW(executable, command.data(), nullptr, nullptr, listed ? TRUE : FALSE, flags,
	                   nullptr, nullptr, &startup.StartupInfo, &process) != 0;
	const auto error = GetLastError();
	if (inherit) {
		DeleteProcThreadAttributeList(attributes);
	}
	if (!created) {
		LOGF("LoadExec: process creation failed: %lu\n", error);
		return SYSTEM_SERVICE_ERROR_INTERNAL;
	}
	CloseHandle(process.hThread);
	CloseHandle(process.hProcess);
	// Nonzero, or the launcher's "cmd /K ... && exit" closes the console the new process shares.
	std::quick_exit(1);
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
	// The new program continues this one's log file. It is given its environment explicitly:
	// setenv is not safe while other threads may read the environment.
	const std::string  relaunched       = std::string(Log::RelaunchedVariable) + "=";
	std::string        relaunched_entry = relaunched + "1";
	std::vector<char*> environment;
	for (char** variable = environ; variable != nullptr && *variable != nullptr; ++variable) {
		if (!std::string_view(*variable).starts_with(relaunched)) {
			environment.push_back(*variable);
		}
	}
	environment.push_back(relaunched_entry.data());
	environment.push_back(nullptr);
	if (close_range(3, ~0u, CLOSE_RANGE_UNSHARE | CLOSE_RANGE_CLOEXEC) == 0) {
		execvpe(host_argv[0], host_argv.data(), environment.data());
	}
	LOGF("LoadExec failed: %s\n", std::strerror(errno));
#endif
	return SYSTEM_SERVICE_ERROR_INTERNAL;
#else
	return SYSTEM_SERVICE_ERROR_UNAVAILABLE;
#endif
}

} // namespace Emulator
