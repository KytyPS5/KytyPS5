#include "emulator.h"

#include "common/file.h"
#include "common/logging/log.h"
#include "kernel/fileSystem.h"
#include "libs/errno.h"

#include <cerrno>
#include <cstring>
#include <fstream>
#include <string_view>
#include <vector>

#if KYTY_PLATFORM == KYTY_PLATFORM_LINUX
#include <linux/close_range.h>
#include <unistd.h>
#endif

namespace Emulator {

int LoadExec(const char* path, const char* const argv[]) {
	using namespace Libs::SystemService;
	if (path == nullptr || !std::string_view(path).starts_with("/app0/") ||
	    !Common::File::IsFileExisting(Libs::LibKernel::FileSystem::GetRealFilename(path))) {
		return SYSTEM_SERVICE_ERROR_PARAMETER;
	}
#if KYTY_PLATFORM == KYTY_PLATFORM_LINUX
	std::ifstream command("/proc/self/cmdline", std::ios::binary);
	std::vector<std::string> arguments;
	for (std::string argument; std::getline(command, argument, '\0') && argument != "--guest-exec";) {
		arguments.push_back(std::move(argument));
	}
	if (arguments.empty()) {
		return SYSTEM_SERVICE_ERROR_INTERNAL;
	}
	arguments.insert(arguments.end(), {"--guest-exec", path});
	size_t bytes = 0;
	for (size_t i = 0; argv != nullptr && argv[i] != nullptr; ++i) {
		const auto size = strnlen(argv[i], 4096 - bytes) + 1;
		if (size > 4096 - bytes) {
			return SYSTEM_SERVICE_ERROR_PARAMETER;
		}
		bytes += size;
		arguments.emplace_back(argv[i]);
	}
	std::vector<char*> host_argv;
	for (auto& argument: arguments) {
		host_argv.push_back(argument.data());
	}
	host_argv.push_back(nullptr);
	LOGF("LoadExec: %s\n", path);
	Log::Flush();
	if (close_range(3, ~0u, CLOSE_RANGE_UNSHARE | CLOSE_RANGE_CLOEXEC) == 0) {
		execvp(host_argv[0], host_argv.data());
	}
	LOGF("LoadExec failed: %s\n", std::strerror(errno));
	return SYSTEM_SERVICE_ERROR_INTERNAL;
#else
	return SYSTEM_SERVICE_ERROR_UNAVAILABLE;
#endif
}

} // namespace Emulator
