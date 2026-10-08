#include "common/assert.h"

#include "common/logging/log.h"
#include "common/subsystems.h"
#include "kytyGitVersion.h"

#include <cstdio>
#include <cstdlib>
#include <fmt/format.h>
#include <string>

namespace Common {

static thread_local int g_soft_exit_depth = 0;

SoftExitScope::SoftExitScope(bool enable): m_enable(enable) {
	if (m_enable) {
		g_soft_exit_depth++;
	}
}

SoftExitScope::~SoftExitScope() {
	if (m_enable) {
		g_soft_exit_depth--;
	}
}

static void ThrowIfSoft(std::string_view text, const char* file, int line) {
	if (g_soft_exit_depth > 0) {
		throw SoftExitError(fmt::format("{} ({}:{})", text, file, line));
	}
}

static std::string BuildFatalReport(const char* title, std::string_view text, const char* file,
                                    int line) {
	return fmt::format("--- Build ---\n{}\n{}\n{} in {}:{}\n", KYTY_BUILD_LABEL, title, text, file,
	                   line);
}

static int DbgReport(const char* title, std::string_view text, const char* file, int line) {
	Log::WriteFatal(BuildFatalReport(title, text, file, line));
	Subsystems::EmergencyShutdownActive();
	return 1;
}

int DbgExitIfHandler(const char* expr, const char* file, int line) {
	ThrowIfSoft(fmt::format("Error: condition ({}) is true", expr), file, line);
	return DbgReport("--- Fatal Error ---", fmt::format("Error: condition ({}) is true", expr),
	                 file, line);
}

int DbgNotImplementedHandler(const char* expr, const char* file, int line) {
	ThrowIfSoft(fmt::format("Not implemented ({})", expr), file, line);
	return DbgReport("--- Fatal Error ---", fmt::format("Not implemented ({})", expr), file, line);
}

int DbgExitHandler(const char* file, int line, std::string_view text) {
	ThrowIfSoft(text, file, line);
	Log::WriteFatal(BuildFatalReport("--- Error ---", text, file, line));
	return 1;
}

int DbgExitHandler(const char* file, int line, fmt::text_style style, std::string_view text) {
	ThrowIfSoft(text, file, line);
	Log::WriteFatal(style, BuildFatalReport("--- Error ---", text, file, line));
	return 1;
}

void DbgExit(int status) {
	Subsystems::EmergencyShutdownActive();
	std::fflush(nullptr);
	std::_Exit(status);
}

} // namespace Common
