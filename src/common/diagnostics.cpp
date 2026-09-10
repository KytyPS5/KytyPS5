#include "common/diagnostics.h"

#include "common/dateTime.h"
#include "common/debug.h"
#include "common/systemInfo.h"
#include "kytyGitVersion.h"

#include <fmt/format.h>
#include <thread>

#if KYTY_PLATFORM == KYTY_PLATFORM_LINUX && !defined(__APPLE__)
#include <cstdio>
#include <gnu/libc-version.h>
#include <sys/utsname.h>
#endif

namespace Common::Diagnostics {

std::string BuildString() {
	Date date = Date::FromMacros(std::string(__DATE__));

#if KYTY_BUILD == KYTY_BUILD_DEBUG
	std::string type = "Debug";
#elif KYTY_BUILD == KYTY_BUILD_RELEASE
	std::string type = "Release";
#else
	std::string type = "????";
#endif

	std::string compiler =
	    Debug::GetCompiler() + "-" + Debug::GetLinker() + "-" + Debug::GetBitness();

	return fmt::format("{}, {}, ver = {}, git = {}, date = {}", type, compiler, KYTY_VERSION,
	                   KYTY_GIT_VERSION, date.ToString());
}

// macOS rides KYTY_PLATFORM_LINUX (see the top-level CMakeLists), so the two
// have to be told apart by the compiler's own macro rather than by KYTY_PLATFORM.
static const char* PlatformName() {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	return "Windows";
#elif defined(__APPLE__)
	return "macOS";
#elif KYTY_PLATFORM == KYTY_PLATFORM_LINUX
	return "Linux";
#else
	return "unknown";
#endif
}

#if KYTY_PLATFORM == KYTY_PLATFORM_LINUX && !defined(__APPLE__)
// Reads a single-line /proc/sys value (e.g. vm.max_map_count), trimming the trailing
// newline. Returns "n/a" if the file cannot be read - an old kernel without the knob,
// a container that hides /proc/sys, or similar - rather than failing the whole report.
static std::string ReadProcSysValue(const char* path) {
	FILE* file = std::fopen(path, "r");
	if (file == nullptr) {
		return "n/a";
	}
	char line[64] = {};
	auto* result  = std::fgets(line, sizeof(line), file);
	std::fclose(file);
	if (result == nullptr) {
		return "n/a";
	}
	std::string value(line);
	while (!value.empty() && (value.back() == '\n' || value.back() == '\r')) {
		value.pop_back();
	}
	return value.empty() ? "n/a" : value;
}
#endif

std::string BuildReport() {
	std::string report;

	report += "KytyPS5 diagnostics\n";
	report += "===================\n\n";

	report += "Build\n";
	report += fmt::format("  {}\n\n", BuildString());

	report += "Host\n";
	report += fmt::format("  os:      {}\n", PlatformName());
	report += fmt::format("  cpu:     {}\n", GetSystemInfo().ProcessorName);
	report += fmt::format("  threads: {}\n\n", std::thread::hardware_concurrency());

#if KYTY_PLATFORM == KYTY_PLATFORM_LINUX && !defined(__APPLE__)
	// The most useful things a Linux bug report can carry that a stack trace does not:
	// the exact kernel/glibc combination, and the three sysctls that make the guest
	// address-space and JIT-mapping code (memoryAddressSpace.inc) either work normally,
	// degrade, or refuse to run at all on a given machine.
	report += "Linux\n";
	utsname uts {};
	if (::uname(&uts) == 0) {
		report += fmt::format("  kernel:  {} {}\n", uts.release, uts.machine);
	} else {
		report += "  kernel:  n/a\n";
	}
	report += fmt::format("  glibc:   {}\n", ::gnu_get_libc_version());
	report += fmt::format("  vm.max_map_count:      {}\n",
	                      ReadProcSysValue("/proc/sys/vm/max_map_count"));
	report += fmt::format("  vm.memfd_noexec_scope: {}\n",
	                      ReadProcSysValue("/proc/sys/vm/memfd_noexec_scope"));
	report += fmt::format("  kernel.yama.ptrace_scope: {}\n\n",
	                      ReadProcSysValue("/proc/sys/kernel/yama/ptrace_scope"));
#endif

	return report;
}

} // namespace Common::Diagnostics
