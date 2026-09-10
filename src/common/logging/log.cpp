
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
#include <spdlog/sinks/rotating_file_sink.h>
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

// Incident, 2026-09-10: an unbounded `spdlog::sinks::basic_file_sink_mt` combined with
// `--log-repeat-limit 0` (documented as "no limit", used deliberately during a diagnostic
// session) let one run's log grow past 500 million lines and fill this box's disk from 41 GB
// free to 0 in minutes -- OS-level "low disk space" warning, every other process on the
// machine put at risk, not just this one. `--log-repeat-limit 0` remains available (a
// deliberate no-cap choice still has legitimate uses for a short, targeted capture) but must
// never again be able to take the disk to zero by itself: this sink is now the hard backstop.
//
// `rotating_file_sink_mt(path, max_size, max_files=0)` is the fix, not a hand-rolled size
// tracker -- already vendored, already tested. With `max_files == 0` specifically, spdlog's
// own rotate_() (rotating_file_sink-inl.h) skips the rename-to-backup chain entirely and just
// truncates and reopens the SAME file in place -- exactly "cap N bytes, then overwrite from
// the start" with no unbounded backup accumulation either. Every `--printf-direction File`
// run is hard-capped at Config::GetLogFileMaxBytes() on disk by default, regardless of any
// other flag.
//
// `--log-file-max-bytes 0` opts back out of the cap entirely (a plain `basic_file_sink_mt`
// instead of the rotating one) for a session that deliberately wants a full, uncut capture --
// that need is real (a short, targeted capture can need more than one call site's worth of
// evidence) but must never again be able to silently take the disk to zero the way it did
// before this cap existed. So the opt-out prints an unsilenceable stdout warning (see
// Initialize() below) naming the real free space on the log's filesystem, per this project's
// own rule that a diagnostic reaching one channel is not enough if the failure it warns about
// can hit every other process on the machine.
//
// This does NOT cover `--printf-direction Console` output redirected to a file by the calling
// shell (`> file.log`) -- KytyPS5 has no visibility into what a caller's shell does with its
// stdout, so that path cannot be capped from inside the emulator. Prefer
// `--printf-direction File --printf-output-file <path>` (capped) over a shell redirect for any
// long or diagnostic run for exactly this reason.

std::shared_ptr<spdlog::logger> MakeFileLogger(std::string                  name,
                                               const std::filesystem::path& path,
                                               uint64_t                     max_bytes) {
	const auto parent = path.parent_path();
	if (!parent.empty()) {
		std::filesystem::create_directories(parent);
	}

	if (max_bytes == 0) {
		auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(Common::PathToString(path));
		return MakeLogger(std::move(name), std::move(sink));
	}

	auto sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
	    Common::PathToString(path), max_bytes, /*max_files=*/0);
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
static uint64_t                        g_log_file_max_bytes = 6'900'000'000ull;
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
				g_logger = MakeFileLogger("kyty", g_output_file, g_log_file_max_bytes);
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

// Unsilenceable by design: goes straight to stdout via WriteStdout, never through g_logger, so
// no --printf-direction setting can suppress it. Follows the owner's fixed shape for explaining
// a consequence: what happened, what it costs, what is NOT broken, the one action that fixes it.
static void WarnLogCapDisabled(const std::filesystem::path& path) {
	std::string free_space_line = "free space on that filesystem could not be determined";
	std::error_code ec;
	const auto parent = path.has_parent_path() ? path.parent_path() : std::filesystem::path(".");
	const auto info    = std::filesystem::space(parent, ec);
	if (!ec) {
		const double free_gb = static_cast<double>(info.available) / 1'000'000'000.0;
		free_space_line = fmt::format("{:.1f} GB free on the filesystem holding {} right now",
		                               free_gb, Common::PathToString(parent));
	}
	WriteStdout(fmt::format(
	    "Log size cap disabled: this run's log file ({}) can grow without limit.\n"
	    "{}. If the log fills it, every process on this machine fails to write, not\n"
	    "just the emulator. Nothing is wrong yet.\n"
	    "To restore the default cap, drop --log-file-max-bytes (or pass 6900000000).\n",
	    Common::PathToString(path), free_space_line));
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
	g_log_file_max_bytes = Config::GetLogFileMaxBytes();
	if (g_direction == Direction::File && !g_output_file.empty() && g_log_file_max_bytes == 0) {
		WarnLogCapDisabled(g_output_file);
	}
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

// ── Named, hard-capped rate limiting ────────────────────────────────────

namespace {

constexpr size_t MAX_TRACKED_RATE_LIMITS = 256;

std::array<RateLimit*, MAX_TRACKED_RATE_LIMITS> g_rate_limits {};
std::atomic<size_t>                            g_rate_limit_count {0};

void RegisterRateLimit(RateLimit& limiter) {
	// Same one-winner-registers idiom as RegisterSite above: a limiter that loses the race
	// still caps correctly, it just will not be named in the end-of-run summary.
	bool expected = false;
	if (!limiter.registered.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
		return;
	}
	const auto index = g_rate_limit_count.fetch_add(1, std::memory_order_relaxed);
	if (index < MAX_TRACKED_RATE_LIMITS) {
		g_rate_limits[index] = &limiter;
	}
}

} // namespace

std::optional<uint64_t> RateLimit::Hit() {
	const auto index = count.fetch_add(1, std::memory_order_relaxed);
	if (index == 0) {
		RegisterRateLimit(*this);
	}
	if (index < cap) {
		return index;
	}
	if (index == cap) {
		// The occurrence that crosses the cap announces it once, through the normal (Silent-
		// respecting) path - this is routine diagnostic content like anything else this call
		// site would have logged. The end-of-run total below is the part that must survive
		// Silent, because by then it is the only remaining record of what was suppressed.
		WriteImpl(fmt::format(
		    "RateLimit[{}]: cap of {} reached, further occurrences suppressed for this run "
		    "(see the end-of-run summary for the total)\n",
		    name, cap));
	}
	return std::nullopt;
}

void WriteRateLimitSummary() {
	const auto limit = g_repeat_limit.load(std::memory_order_relaxed);

	struct Entry {
		const char* file  = nullptr;
		int         line  = 0;
		uint64_t    count = 0;
	};

	std::vector<Entry> noisy;
	if (limit != 0) {
		const auto tracked = std::min(g_site_count.load(std::memory_order_relaxed),
		                              MAX_TRACKED_SITES);
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
	}

	struct NamedEntry {
		const char* name  = nullptr;
		uint64_t    cap   = 0;
		uint64_t    count = 0;
	};

	std::vector<NamedEntry> capped;
	const auto rl_tracked = std::min(g_rate_limit_count.load(std::memory_order_relaxed),
	                                 MAX_TRACKED_RATE_LIMITS);
	for (size_t i = 0; i < rl_tracked; i++) {
		auto* limiter = g_rate_limits[i];
		if (limiter == nullptr) {
			continue;
		}
		const auto count = limiter->count.load(std::memory_order_relaxed);
		if (count > limiter->cap) {
			capped.push_back({limiter->name, limiter->cap, count});
		}
	}

	if (noisy.empty() && capped.empty()) {
		return;
	}

	// Both blocks below are written through WriteStdout directly rather than WriteImpl/LOGF:
	// this is the run's own account of what evidence its caps threw away, and per this
	// project's logging rule it must reach the same channel an abort's evidence does - never
	// one --printf-direction Silent can suppress on its own. (WriteRateLimitSummary already
	// bypassed LOGF for the site block for a related reason - not rate-limiting itself - but
	// previously still routed through WriteImpl, which silently dropped this exact block under
	// Silent; kept consistent with the named block below instead of leaving that gap.)
	if (!noisy.empty()) {
		std::sort(noisy.begin(), noisy.end(),
		          [](const Entry& a, const Entry& b) { return a.count > b.count; });

		WriteStdout(fmt::format("\n--- log rate limiting: {} sites exceeded {} messages ---\n",
		                        noisy.size(), limit));
		for (const auto& entry: noisy) {
			WriteStdout(
			    fmt::format("  {:>12} messages  {}:{}\n", entry.count, entry.file, entry.line));
		}
		WriteStdout("--- raise or disable this with --log-repeat-limit ---\n");
	}

	if (!capped.empty()) {
		std::sort(capped.begin(), capped.end(),
		          [](const NamedEntry& a, const NamedEntry& b) { return a.count > b.count; });

		WriteStdout(fmt::format("\n--- rate-limited diagnostics: {} sites capped ---\n",
		                        capped.size()));
		for (const auto& entry: capped) {
			WriteStdout(fmt::format("  {:>7} of {:>7} occurrences logged  {}\n", entry.cap,
			                        entry.count, entry.name));
		}
		WriteStdout("--- raise the cap at the call site if more detail is needed ---\n");
	}

	Flush();
}

void Write(std::string_view text) {
	WriteImpl(text);
}

void Write(fmt::text_style style, std::string_view text) {
	WriteImpl(text, style);
}

} // namespace Log
