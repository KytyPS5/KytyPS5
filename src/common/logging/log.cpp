
#include "common/logging/log.h"

#include "common/assert.h"
#include "common/asyncWriter.h"
#include "common/emulatorConfig.h"
#include "common/stringUtils.h"

#include <cstdio>
#include <filesystem>
#include <fmt/format.h>
#include <memory>
#include <mutex>
#include <spdlog/formatter.h>
#include <spdlog/logger.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/null_sink.h>
#include <spdlog/sinks/stdout_sinks.h>
#include <string_view>

namespace {

class RawFormatter final: public spdlog::formatter {
public:
	void format(const spdlog::details::log_msg& msg, spdlog::memory_buf_t& dest) override {
		const std::string_view payload(msg.payload.data(), msg.payload.size());
		dest.append(payload.data(), payload.data() + payload.size());
	}

	std::unique_ptr<spdlog::formatter> clone() const override {
		return std::make_unique<RawFormatter>();
	}
};

std::shared_ptr<spdlog::logger> MakeLogger(std::string name, spdlog::sink_ptr sink) {
	sink->set_formatter(std::make_unique<RawFormatter>());

	auto logger = std::make_shared<spdlog::logger>(std::move(name), std::move(sink));
	logger->set_level(spdlog::level::trace);
	return logger;
}

static bool HasStyle(fmt::text_style style) {
	return style != fmt::text_style {};
}

static bool ShouldLog(Config::LogLevel message_level, Config::LogLevel sink_level,
                      bool sink_enabled) {
	return sink_enabled && static_cast<int>(message_level) >= static_cast<int>(sink_level) &&
	       sink_level != Config::LogLevel::Off;
}

void WriteStdout(std::string_view text, fmt::text_style style = {}) {
	if (HasStyle(style)) {
		fmt::print(stdout, style, "{}", text);
	} else {
		std::fwrite(text.data(), 1, text.size(), stdout);
	}
	std::fflush(stdout);
}

} // namespace

namespace Log {

static bool                            g_initialized = false;
static Direction                       g_direction   = Direction::Console;
static std::filesystem::path           g_output_file;
static std::mutex                      g_logger_mutex;
static std::shared_ptr<spdlog::logger> g_logger;
static Config::LogLevel                g_console_level   = Config::LogLevel::Info;
static Config::LogLevel                g_file_level      = Config::LogLevel::Info;
static bool                            g_console_enabled = true;
static bool                            g_file_enabled    = true;

void Flush() {
	Common::AsyncWriter::Flush();
	if (g_logger != nullptr) {
		g_logger->flush();
	}
	std::fflush(stdout);
}

static void SetupLogger() {
	std::lock_guard lock(g_logger_mutex);
	Flush();
	g_logger.reset();

	switch (g_direction) {
		case Direction::Silent:
			g_logger = MakeLogger("kyty", std::make_shared<spdlog::sinks::null_sink_mt>());
			break;
		case Direction::Console:
			// Use null logger since AsyncWriter handles console output (3.1)
			g_logger = MakeLogger("kyty", std::make_shared<spdlog::sinks::null_sink_mt>());
			break;
		case Direction::File:
			if (!g_output_file.empty()) {
				Common::AsyncWriter::EnqueueFileWrite(g_output_file, std::string_view {}, false);
			}
			g_logger = MakeLogger("kyty", std::make_shared<spdlog::sinks::null_sink_mt>());
			break;
		case Direction::ConsoleAndFile:
			if (!g_output_file.empty()) {
				Common::AsyncWriter::EnqueueFileWrite(g_output_file, std::string_view {}, false);
			}
			g_logger = MakeLogger("kyty", std::make_shared<spdlog::sinks::null_sink_mt>());
			break;
	}
}

static std::shared_ptr<spdlog::logger> GetLogger() {
	std::lock_guard lock(g_logger_mutex);
	return g_logger;
}

static void WriteImpl(std::string_view text, fmt::text_style style = {},
                      Config::LogLevel level = Config::LogLevel::Info) {
	if (text.empty()) {
		return;
	}

	if (!g_initialized) {
		WriteStdout(text, style);
		return;
	}

	if (g_direction == Direction::Silent) {
		return;
	}

	// Apply level filtering based on direction and enabled flags (3.2)
	bool should_write_console = false;
	bool should_write_file    = false;

	switch (g_direction) {
		case Direction::Console:
			should_write_console = ShouldLog(level, g_console_level, g_console_enabled);
			break;
		case Direction::File:
			should_write_file = ShouldLog(level, g_file_level, g_file_enabled);
			break;
		case Direction::ConsoleAndFile:
			should_write_console = ShouldLog(level, g_console_level, g_console_enabled);
			should_write_file    = ShouldLog(level, g_file_level, g_file_enabled);
			break;
		default: break;
	}

	if (!should_write_console && !should_write_file) {
		return;
	}

	// Handle ConsoleAndFile with optimized dual-output
	if (g_direction == Direction::ConsoleAndFile && should_write_console && should_write_file &&
	    !g_output_file.empty()) {
		std::string console_str;
		if (HasStyle(style)) {
			console_str = fmt::format(style, "{}", text);
		} else {
			console_str = std::string(text);
		}
		Common::AsyncWriter::EnqueueDualOutput(g_output_file, text, console_str, style);
		return;
	}

	// Fallback to separate enqueues
	if (should_write_file && !g_output_file.empty()) {
		Common::AsyncWriter::EnqueueFileWrite(g_output_file, text, true);
	}

	if (should_write_console) {
		if (HasStyle(style)) {
			Common::AsyncWriter::EnqueueTask(
			    [str = fmt::format(style, "{}", text)]() { WriteStdout(str); });
		} else {
			Common::AsyncWriter::EnqueueTask([str = std::string(text)]() { WriteStdout(str); });
		}
	}

	if (auto logger = GetLogger()) {
		logger->log(spdlog::level::info, spdlog::string_view_t(text.data(), text.size()));
	}
}

void WriteToConsoleAndLog(std::string_view text) {
	WriteImpl(text);
	if (g_initialized && g_direction == Direction::File && g_console_enabled) {
		WriteStdout(text);
	}
	Flush();
}

void WriteFatal(std::string_view text) {
	if (!g_initialized || (g_direction != Direction::Silent && g_console_enabled)) {
		WriteStdout(text);
	}
	if ((g_direction == Direction::File || g_direction == Direction::ConsoleAndFile) &&
	    g_file_enabled && !g_output_file.empty()) {
		Common::AsyncWriter::EnqueueFileWrite(g_output_file, text, true);
	}
	Common::AsyncWriter::EmergencyFlush();
	Flush();
}

void WriteFatal(fmt::text_style style, std::string_view text) {
	if (!g_initialized || (g_direction != Direction::Silent && g_console_enabled)) {
		WriteStdout(text, style);
	}
	if ((g_direction == Direction::File || g_direction == Direction::ConsoleAndFile) &&
	    g_file_enabled && !g_output_file.empty()) {
		Common::AsyncWriter::EnqueueFileWrite(g_output_file, text, true);
	}
	Common::AsyncWriter::EmergencyFlush();
	Flush();
}

void Initialize() {
	g_initialized = true;
	switch (Config::GetPrintfDirection()) {
		case Config::LogDirection::Silent: g_direction = Direction::Silent; break;
		case Config::LogDirection::Console: g_direction = Direction::Console; break;
		case Config::LogDirection::File: g_direction = Direction::File; break;
		case Config::LogDirection::ConsoleAndFile: g_direction = Direction::ConsoleAndFile; break;
	}

	// Load log level configuration
	auto log_config   = Config::GetLogConfig();
	g_console_level   = log_config.console.level;
	g_file_level      = log_config.file.level;
	g_console_enabled = log_config.console.enabled;
	g_file_enabled    = log_config.file.enabled;

	g_output_file = ((g_direction == Direction::File || g_direction == Direction::ConsoleAndFile)
	                     ? Config::GetPrintfOutputFile()
	                     : std::filesystem::path {});
	SetupLogger();
}

void Shutdown() {
	Flush();
	std::lock_guard lock(g_logger_mutex);
	g_logger.reset();
}

Direction GetDirection() {
	EXIT_IF(!g_initialized);
	return g_direction;
}

bool IsSilent() {
	// Before init LOGF must keep writing to stdout, so report non-silent.
	return g_initialized && g_direction == Direction::Silent;
}

void Write(std::string_view text) {
	WriteImpl(text);
}

void Write(fmt::text_style style, std::string_view text) {
	WriteImpl(text, style);
}

size_t GetDroppedCount() {
	return Common::AsyncWriter::GetDroppedCount();
}

} // namespace Log
