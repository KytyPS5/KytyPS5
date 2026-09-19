#include "common/crashHandler.h"

#include "common/asyncWriter.h"
#include "common/logging/log.h"

#include <csignal>
#include <cstdlib>
#include <exception>

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#include <windows.h>
#else
#include <signal.h>
#endif

namespace Common::CrashHandler {

namespace {

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
static LPTOP_LEVEL_EXCEPTION_FILTER g_previous_filter = nullptr;

static LONG WINAPI GlobalUnhandledExceptionFilter(PEXCEPTION_POINTERS exception) {
	Common::AsyncWriter::EmergencyFlush();
	Log::Flush();
	if (g_previous_filter != nullptr) {
		return g_previous_filter(exception);
	}
	return EXCEPTION_CONTINUE_SEARCH;
}
#endif

static std::terminate_handler g_previous_terminate = nullptr;

static void GlobalTerminateHandler() {
	Common::AsyncWriter::EmergencyFlush();
	Log::Flush();
	if (g_previous_terminate != nullptr) {
		g_previous_terminate();
	} else {
		std::abort();
	}
}

static void GlobalSignalHandler(int sig) {
	// POSIX signal handlers may not touch AsyncWriter, spdlog, filesystem, or stdio
	// state. The terminate and Windows SEH paths perform the best-effort C++ flush.
	std::signal(sig, SIG_DFL);
	std::raise(sig);
}

static bool g_initialized = false;

} // namespace

void Initialize() {
	if (g_initialized) {
		return;
	}
	g_initialized = true;

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	g_previous_filter = SetUnhandledExceptionFilter(GlobalUnhandledExceptionFilter);
#endif

	g_previous_terminate = std::set_terminate(GlobalTerminateHandler);

	std::signal(SIGABRT, GlobalSignalHandler);
	std::signal(SIGSEGV, GlobalSignalHandler);
	std::signal(SIGFPE, GlobalSignalHandler);
	std::signal(SIGILL, GlobalSignalHandler);
#if defined(SIGBUS)
	std::signal(SIGBUS, GlobalSignalHandler);
#endif
}

void Shutdown() {
	if (!g_initialized) {
		return;
	}
	g_initialized = false;

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	if (g_previous_filter != nullptr) {
		SetUnhandledExceptionFilter(g_previous_filter);
		g_previous_filter = nullptr;
	}
#endif

	if (g_previous_terminate != nullptr) {
		std::set_terminate(g_previous_terminate);
		g_previous_terminate = nullptr;
	}
}

} // namespace Common::CrashHandler
