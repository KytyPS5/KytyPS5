#include "common/assert.h"

#include "common/logging/log.h"
#include "common/subsystems.h"
#include "kytyGitVersion.h"

#include <cstdio>
#include <cstdlib>
#include <fmt/format.h>
#include <mutex>
#include <string>

#ifdef _WIN32
#include <windows.h>
#include <dbghelp.h>
#endif

namespace Common {

std::string HostBacktrace() {
#ifdef _WIN32
	static constexpr ULONG MaxFrames = 48;
	void*                  frames[MaxFrames];
	const auto             count = CaptureStackBackTrace(1, MaxFrames, frames, nullptr);
	if (count == 0) {
		return {};
	}

	const auto process = GetCurrentProcess();
	char       exe_path[MAX_PATH] {};
	GetModuleFileNameA(nullptr, exe_path, MAX_PATH);
	std::string search = exe_path;
	if (const auto slash = search.find_last_of("\\/"); slash != std::string::npos) {
		search.resize(slash);
		std::string parent = search;
		if (const auto up = parent.find_last_of("\\/"); up != std::string::npos) {
			parent.resize(up);
			search += ";" + parent;
		}
	}
	static std::mutex symbol_mutex;
	std::scoped_lock  symbol_lock(symbol_mutex);
	SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME | SYMOPT_LOAD_LINES);
	static const bool initialized = SymInitialize(process, search.c_str(), TRUE) != FALSE;

	std::string out = "--- Host stack ---\n";
	for (ULONG i = 0; i < count; i++) {
		const auto address = reinterpret_cast<DWORD64>(frames[i]);
		out += fmt::format("  #{:<2} 0x{:016x}", i, address);
		if (initialized) {
			alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 512] {};
			auto*                     symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
			symbol->SizeOfStruct             = sizeof(SYMBOL_INFO);
			symbol->MaxNameLen               = 512;
			DWORD64 displacement             = 0;
			if (SymFromAddr(process, address, &displacement, symbol) != FALSE) {
				out += fmt::format(" {}+0x{:x}", symbol->Name, displacement);
			}
			IMAGEHLP_LINE64 line {};
			line.SizeOfStruct = sizeof(line);
			DWORD line_displacement = 0;
			if (SymGetLineFromAddr64(process, address, &line_displacement, &line) != FALSE) {
				out += fmt::format(" [{}:{}]", line.FileName, line.LineNumber);
			}
		}
		out += "\n";
	}
	return out;
#else
	return {};
#endif
}

static std::string BuildFatalReport(const char* title, std::string_view text, const char* file,
                                    int line) {
	return fmt::format("--- Build ---\n{}\n{}\n{} in {}:{}\n{}", KYTY_BUILD_LABEL, title, text, file,
	                   line, HostBacktrace());
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
