#include "common/diagnostics.h"

#include "common/dateTime.h"
#include "common/debug.h"
#include "common/systemInfo.h"
#include "kytyGitVersion.h"

#include <fmt/format.h>
#include <thread>

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

	return report;
}

} // namespace Common::Diagnostics
