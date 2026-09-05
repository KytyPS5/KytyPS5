#include "common/assert.h"

#include "common/debug.h"
#include "common/logging/log.h"
#include "common/platform/sysDbg.h"
#include "common/subsystems.h"
#include "kytyGitVersion.h"

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fmt/format.h>
#include <string>

namespace Common {

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS && KYTY_BUILD == KYTY_BUILD_DEBUG &&                    \
    KYTY_COMPILER == KYTY_COMPILER_CLANG
constexpr int PRINT_STACK_FROM = 4;
#else
constexpr int PRINT_STACK_FROM = 2;
#endif

static std::string BuildFatalReport(const char* title, std::string_view text, const char* file,
                                    int line) {
	DebugStack stack;
	DebugStack::Trace(&stack);

	std::string report = "--- Build ---\n" KYTY_BUILD_LABEL "\n--- Stack Trace ---\n";
	for (int i = PRINT_STACK_FROM; i < stack.depth; i++) {
		report += fmt::format("[{}] {:016x}\n", i - PRINT_STACK_FROM,
		                      static_cast<uint64_t>(stack.GetAddr(i)));
	}
	report += fmt::format("{}\n{} in {}:{}\n", title, text, file, line);
	return report;
}

// A fault that reaches none of the emulator's own handlers used to end the process with nothing
// written: the log tail was still in the file buffer and no report was produced. These hooks
// leave the same kind of record EXIT() leaves, then hand the fault back to the default handling
// so dump collection is unchanged.
static std::atomic_bool g_reported {false};

static std::string BuildCrashReport(std::string_view headline, std::string_view detail) {
	DebugStack stack;
	DebugStack::Trace(&stack);

	std::string report = "--- Build ---\n" KYTY_BUILD_LABEL "\n--- Stack Trace ---\n";
	for (int i = PRINT_STACK_FROM; i < stack.depth; i++) {
		report += fmt::format("[{}] {:016x}\n", i - PRINT_STACK_FROM,
		                      static_cast<uint64_t>(stack.GetAddr(i)));
	}
	report += fmt::format("--- Crash ---\n{}\n", headline);
	if (!detail.empty()) {
		report += fmt::format("{}\n", detail);
	}
	return report;
}

// Faults the guest page-fault handler did not claim: stack overflow, divide by zero and the rest.
// Access violations and illegal instructions are already reported by KytyExceptionHandler and
// never reach here.
static void ReportCrash(const sys_dbg_crash_info_t& info) {
	if (g_reported.exchange(true)) {
		return;
	}

	auto headline = fmt::format("{} (0x{:08x}) at 0x{:016x}", info.description, info.native_code,
	                            info.fault_addr);
	std::string detail;
	if (info.access != nullptr) {
		detail = fmt::format("{} of 0x{:016x}", info.access, info.access_addr);
	}

	// Writes to stdout and the log file, then flushes. Without the flush the tail of the log is
	// still sitting in the file buffer when the process dies.
	Log::WriteFatal(BuildCrashReport(headline, detail));
}

// An uncaught throw never reaches the fault hook, it goes to std::terminate. The Microsoft CRT
// keeps the terminate handler per thread, so this alone would only cover the thread that
// installed it. The exception text is not read back here: the project builds with
// -fno-exceptions on clang, and by the time SIGABRT arrives there is no active exception to
// describe anyway.
static void ReportTerminate() {
	if (!g_reported.exchange(true)) {
		Log::WriteFatal(BuildCrashReport("terminate called", {}));
	}
	std::abort(); // what the default handler would have done
}

// SIGABRT is process-wide, so this is what actually covers an uncaught throw on a guest or
// graphics thread, along with any direct abort().
static void ReportAbort(int sig) {
	if (!g_reported.exchange(true)) {
		Log::WriteFatal(BuildCrashReport("aborted", {}));
	}
	std::signal(sig, SIG_DFL);
}

void InstallCrashReporter() {
	SysInstallCrashHandler(ReportCrash);
	std::set_terminate(ReportTerminate);
	std::signal(SIGABRT, ReportAbort);
}

static int DbgReport(const char* title, std::string_view text, const char* file, int line) {
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
	Log::WriteFatal(BuildFatalReport("--- Error ---", text, file, line));
	return 1;
}

int DbgExitHandler(const char* file, int line, fmt::text_style style, std::string_view text) {
	Log::WriteFatal(style, BuildFatalReport("--- Error ---", text, file, line));
	return 1;
}

void DbgExit(int status) {
	Subsystems::EmergencyShutdownActive();
	std::fflush(nullptr);
	std::_Exit(status);
}

} // namespace Common
