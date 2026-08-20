
#include "common/logging/log.h"

#include "common/assert.h"
#include "common/emulatorConfig.h"
#include "common/stringUtils.h"

#include <algorithm>
#include <array>
#include <atomic>
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
#include <vector>

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

std::shared_ptr<spdlog::logger> MakeFileLogger(std::string                  name,
                                               const std::filesystem::path& path) {
	const auto parent = path.parent_path();
	if (!parent.empty()) {
		std::filesystem::create_directories(parent);
	}

	auto sink =
	    std::make_shared<spdlog::sinks::basic_file_sink_mt>(Common::PathToString(path), true);
	return MakeLogger(std::move(name), std::move(sink));
}

static bool HasStyle(fmt::text_style style) {
	return style != fmt::text_style {};
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

// Read once at init rather than per message: ShouldWrite runs on every LOGF,
// including before Config exists, and Config accessors dereference a global that
// is null until Config::Initialize().
static constexpr uint64_t     DEFAULT_LOG_REPEAT_LIMIT = 256;
static std::atomic<uint64_t>  g_repeat_limit {DEFAULT_LOG_REPEAT_LIMIT};

void Flush() {
	if (g_logger != nullptr) {
		g_logger->flush();
	}
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
			g_logger = MakeLogger("kyty", std::make_shared<spdlog::sinks::stdout_sink_mt>());
			break;
		case Direction::File:
			if (!g_output_file.empty()) {
				g_logger = MakeFileLogger("kyty", g_output_file);
			}
			break;
	}
}

static std::shared_ptr<spdlog::logger> GetLogger() {
	std::lock_guard lock(g_logger_mutex);
	return g_logger;
}

static void WriteImpl(std::string_view text, fmt::text_style style = {}) {
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

	if (auto logger = GetLogger()) {
		if (g_direction == Direction::Console && HasStyle(style)) {
			const auto styled = fmt::format(style, "{}", text);
			logger->log(spdlog::level::info, spdlog::string_view_t(styled.data(), styled.size()));
		} else {
			logger->log(spdlog::level::info, spdlog::string_view_t(text.data(), text.size()));
		}
	}
}

void WriteFatal(std::string_view text) {
	if (g_direction == Direction::Silent || !g_initialized) {
		WriteStdout(text);
	} else {
		WriteStdout(text);
		WriteImpl(text);
	}
	Flush();
}

void WriteFatal(fmt::text_style style, std::string_view text) {
	if (g_direction == Direction::Silent || !g_initialized) {
		WriteStdout(text, style);
	} else {
		WriteStdout(text, style);
		WriteImpl(text, style);
	}
	Flush();
}

void Initialize() {
	g_initialized = true;
	g_repeat_limit.store(Config::GetLogRepeatLimit(), std::memory_order_relaxed);
	switch (Config::GetPrintfDirection()) {
		case Config::OutputDirection::Silent: g_direction = Direction::Silent; break;
		case Config::OutputDirection::Console: g_direction = Direction::Console; break;
		case Config::OutputDirection::File: g_direction = Direction::File; break;
	}
	g_output_file =
	    (g_direction == Direction::File ? Config::GetPrintfOutputFile() : std::filesystem::path {});
	SetupLogger();
}

void Shutdown() {
	WriteRateLimitSummary();
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

// ── Per-call-site rate limiting (issue #200) ────────────────────────────

namespace {

// Sites are registered on first use so the summary can name them. The list is
// bounded: a build has a fixed number of LOGF sites, and if it somehow had more
// than this, the excess is still rate limited, just not named at the end.
constexpr size_t MAX_TRACKED_SITES = 4096;

std::array<Site*, MAX_TRACKED_SITES> g_sites {};
std::atomic<size_t>                  g_site_count {0};

void RegisterSite(Site& site) {
	// One winner registers; everyone else moves on. The counter still works for
	// a site that loses the race, it just will not be named in the summary.
	bool expected = false;
	if (!site.registered.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
		return;
	}
	const auto index = g_site_count.fetch_add(1, std::memory_order_relaxed);
	if (index < MAX_TRACKED_SITES) {
		g_sites[index] = &site;
	}
}

} // namespace

bool ShouldWrite(Site& site) {
	const auto limit = g_repeat_limit.load(std::memory_order_relaxed);
	if (limit == 0) {
		return true; // rate limiting turned off
	}

	const auto count = site.count.fetch_add(1, std::memory_order_relaxed);
	if (count == 0) {
		RegisterSite(site);
	}

	if (count < limit) {
		return true;
	}

	// Past the limit, keep a thin sample rather than going completely dark, so a
	// long run still shows that the site is still firing and roughly when.
	const auto sample = static_cast<uint64_t>(limit) * 64ULL;
	return (count % sample) == 0;
}

void WriteRateLimitSummary() {
	const auto limit = g_repeat_limit.load(std::memory_order_relaxed);
	if (limit == 0) {
		return;
	}

	const auto tracked = std::min(g_site_count.load(std::memory_order_relaxed),
	                              MAX_TRACKED_SITES);

	struct Entry {
		const char* file  = nullptr;
		int         line  = 0;
		uint64_t    count = 0;
	};

	std::vector<Entry> noisy;
	for (size_t i = 0; i < tracked; i++) {
		auto* site = g_sites[i];
		if (site == nullptr) {
			continue;
		}
		const auto count = site->count.load(std::memory_order_relaxed);
		if (count > limit) {
			noisy.push_back({site->file, site->line, count});
		}
	}

	if (noisy.empty()) {
		return;
	}

	std::sort(noisy.begin(), noisy.end(),
	          [](const Entry& a, const Entry& b) { return a.count > b.count; });

	// Deliberately not written through LOGF: this must not rate limit itself.
	WriteImpl(fmt::format("\n--- log rate limiting: {} sites exceeded {} messages ---\n",
	                      noisy.size(), limit));
	for (const auto& entry: noisy) {
		WriteImpl(fmt::format("  {:>12} messages  {}:{}\n", entry.count, entry.file, entry.line));
	}
	WriteImpl("--- raise or disable this with --log-repeat-limit ---\n");
	Flush();
}

void Write(std::string_view text) {
	WriteImpl(text);
}

void Write(fmt::text_style style, std::string_view text) {
	WriteImpl(text, style);
}

} // namespace Log
