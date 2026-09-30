#include "common/assert.h"

#include "common/logging/log.h"
#include "common/subsystems.h"
#include "kytyGitVersion.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fmt/format.h>
#include <string>
#include <thread>

namespace Common {

// An emergency hook can wait on a thread that waits on the failing one; exit regardless.
static void ArmExitWatchdog(int status) {
	static std::atomic_flag armed;
	if (armed.test_and_set()) {
		return;
	}
	std::thread([status] {
		std::this_thread::sleep_for(std::chrono::seconds(10));
		std::_Exit(status);
	}).detach();
}

static std::string BuildFatalReport(const char* title, std::string_view text, const char* file,
                                    int line) {
	return fmt::format("--- Build ---\n{}\n{}\n{} in {}:{}\n", KYTY_BUILD_LABEL, title, text, file,
	                   line);
}

static int DbgReport(const char* title, std::string_view text, const char* file, int line) {
	ArmExitWatchdog(321);
	Log::WriteFatal(BuildFatalReport(title, text, file, line));
	Subsystems::EmergencyShutdownActive();
	return 1;
}

int DbgExitIfHandler(const char* expr, const char* file, int line) {
	return DbgReport("--- Fatal Error ---", fmt::format("Error: condition ({}) is true", expr),
	                 file, line);
}

int DbgNotImplementedHandler(const char* expr, const char* file, int line) {
	return DbgReport("--- Fatal Error ---", fmt::format("Not implemented ({})", expr), file, line);
}

int DbgExitHandler(const char* file, int line, std::string_view text) {
	ArmExitWatchdog(321);
	Log::WriteFatal(BuildFatalReport("--- Error ---", text, file, line));
	return 1;
}

int DbgExitHandler(const char* file, int line, fmt::text_style style, std::string_view text) {
	ArmExitWatchdog(321);
	Log::WriteFatal(style, BuildFatalReport("--- Error ---", text, file, line));
	return 1;
}

void DbgExit(int status) {
	ArmExitWatchdog(status);
	Subsystems::EmergencyShutdownActive();
	std::fflush(nullptr);
	std::_Exit(status);
}

} // namespace Common
