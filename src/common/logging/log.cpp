#include "common/logging/log.h"

#include "common/emulatorConfig.h"
#include "kytyGitVersion.h"

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <iterator>
#include <memory>
#include <spdlog/logger.h>
#include <spdlog/pattern_formatter.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_sinks.h>

#if defined(_WIN32)
#include <share.h>
#endif

namespace Log {

static bool g_initialized = false;
static bool g_console     = false;
// Null when silent. Only Initialize sets it, before other threads start. Never destroyed, since
// threads keep logging while exit runs static destructors.
static spdlog::logger* g_logger = nullptr;
// --session-log, opened like g_logger and never closed either. Each fwrite keeps a message whole
// among threads.
static std::FILE* g_session = nullptr;

static void WriteStdout(std::string_view text, fmt::text_style style) {
	if (style == fmt::text_style {}) {
		std::fwrite(text.data(), 1, text.size(), stdout);
	} else {
		fmt::print(stdout, style, "{}", text);
	}
	std::fflush(stdout);
}

// What a session leaves to tell why it ended: the console messages, not the printf log, which
// traces every call of the graphics libraries in each frame (megabytes a second) and slows the game.
static void OpenSessionLog(const std::filesystem::path& path) {
	if (path.empty() || g_session != nullptr) {
		return;
	}
	std::error_code error;
	if (path.has_parent_path()) {
		std::filesystem::create_directories(path.parent_path(), error);
	}
	// The program a LoadExec starts continues the session of the one it replaces.
	const bool relaunched = std::getenv(RelaunchedVariable) != nullptr;
#if defined(_WIN32)
	// Shared, so that it can be read while the game runs.
	std::FILE* file = _wfsopen(path.c_str(), relaunched ? L"ab" : L"wb", _SH_DENYNO);
#else
	std::FILE* file = std::fopen(path.c_str(), relaunched ? "ab" : "wb");
#endif
	if (file == nullptr) {
		WriteStdout("Session log: cannot open the file\n", {});
		return;
	}
	std::setvbuf(file, nullptr, _IOFBF, static_cast<size_t>(64) * 1024);
	g_session = file;

	const std::time_t now = std::time(nullptr);
	std::tm           local {};
#if defined(_WIN32)
	localtime_s(&local, &now);
#else
	localtime_r(&now, &local);
#endif
	char started[32] = "";
	std::strftime(started, sizeof(started), "%Y-%m-%d %H:%M:%S", &local);
	WriteToSessionLog(fmt::format("--- {} {} ---\n{}\n", relaunched ? "Relaunched" : "Session",
	                              started, KYTY_BUILD_LABEL));
	std::fflush(g_session);
}

void Initialize() {
	g_initialized        = true;
	const auto direction = Config::GetPrintfDirection();
	const auto path      = Config::GetPrintfOutputFile();
	g_console            = direction == Config::LogDirection::Console;
	delete g_logger;
	g_logger = nullptr;

	spdlog::sink_ptr sink;
	if (g_console) {
		sink = std::make_shared<spdlog::sinks::stdout_sink_mt>();
	} else if (direction == Config::LogDirection::File && !path.empty()) {
		if (path.has_parent_path()) {
			std::filesystem::create_directories(path.parent_path());
		}
		const bool truncate = std::getenv(RelaunchedVariable) == nullptr;
		sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path.native(), truncate);
	}
	if (sink != nullptr) {
		sink->set_formatter(std::make_unique<spdlog::pattern_formatter>(
		    "%v", spdlog::pattern_time_type::local, ""));
		g_logger = new spdlog::logger("kyty", std::move(sink));
	}
	OpenSessionLog(Config::GetSessionLogFile());
}

void Flush() {
	if (g_logger != nullptr) {
		g_logger->flush();
	}
	if (g_session != nullptr) {
		std::fflush(g_session);
	}
}

bool IsSilent() {
	// Before Initialize the text goes to stdout.
	return g_initialized && g_logger == nullptr;
}

void Write(fmt::text_style style, std::string_view text) {
	if (!g_initialized) {
		WriteStdout(text, style);
		return;
	}
	if (g_logger == nullptr) {
		return;
	}
	if (g_console && style != fmt::text_style {}) {
		fmt::memory_buffer styled;
		fmt::format_to(std::back_inserter(styled), style, "{}", text);
		g_logger->log(spdlog::level::info, spdlog::string_view_t(styled.data(), styled.size()));
	} else {
		g_logger->log(spdlog::level::info, spdlog::string_view_t(text.data(), text.size()));
	}
}

void WriteToConsoleAndLog(std::string_view text) {
	if (g_initialized && !g_console) {
		WriteStdout(text, {});
	}
	Write({}, text);
	WriteToSessionLog(text);
	Flush();
}

void WriteToSessionLog(std::string_view text) {
	if (g_session != nullptr) {
		std::fwrite(text.data(), 1, text.size(), g_session);
	}
}

} // namespace Log
