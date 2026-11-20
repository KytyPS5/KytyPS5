#include "common/assert.h"

#include "common/subsystems.h"
#include "kytyGitVersion.h"

#include <cstdio>
#include <cstdlib>
#include <fmt/format.h>
#include <string>

namespace Common {

static thread_local bool t_soft_exit = false;

SoftExitScope::SoftExitScope(): m_previous(t_soft_exit) {
	t_soft_exit = true;
}

SoftExitScope::~SoftExitScope() {
	t_soft_exit = m_previous;
}

// Throws instead of reporting while the thread has a SoftExitScope.
static void ThrowIfSoft([[maybe_unused]] const std::string& text) {
#if defined(__cpp_exceptions)
	if (t_soft_exit) {
		throw SoftExitError(text);
	}
#endif
}

[[noreturn]] static void DbgReport(const char* title, std::string_view text, const char* file,
                                   int line) {
	Log::WriteToConsoleAndLog(fmt::format("--- Build ---\n{}\n{}\n{} in {}:{}\n", KYTY_BUILD_LABEL,
	                                      title, text, file, line));
	Subsystems::EmergencyShutdownActive();
	std::fflush(nullptr);
	std::_Exit(321);
}

void DbgExitIfHandler(const char* expr, const char* file, int line) {
	ThrowIfSoft(fmt::format("condition ({}) is true at {}:{}", expr, file, line));
	DbgReport("--- Fatal Error ---", fmt::format("Error: condition ({}) is true", expr), file,
	          line);
}

void DbgNotImplementedHandler(const char* expr, const char* file, int line) {
	ThrowIfSoft(fmt::format("not implemented ({}) at {}:{}", expr, file, line));
	DbgReport("--- Fatal Error ---", fmt::format("Not implemented ({})", expr), file, line);
}

void DbgExitHandler(const char* file, int line, std::string_view text) {
	ThrowIfSoft(fmt::format("{} at {}:{}", text, file, line));
	DbgReport("--- Error ---", text, file, line);
}

} // namespace Common
