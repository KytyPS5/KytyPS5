#ifndef KYTY_COMMON_LOGGING_LOG_H_
#define KYTY_COMMON_LOGGING_LOG_H_

#include "common/common.h"

#include <atomic>
#include <cstdint>
#include <fmt/color.h>
#include <fmt/printf.h>
#include <string_view>

namespace Log {

void Initialize();
void Shutdown();

struct Lifecycle {
	static constexpr const char* name               = "Log";
	static constexpr auto        initialize         = Log::Initialize;
	static constexpr auto        shutdown           = Log::Shutdown;
	static constexpr auto        emergency_shutdown = Log::Shutdown;
};

enum class Direction { Silent, Console, File };

Direction GetDirection();
bool      IsSilent();

// Per-call-site rate limiting.
//
// A KytyPS5 log can reach several gigabytes in two minutes (issue #200), which
// makes it useless for the bug reports the compatibility list runs on - nobody
// can upload it, and nothing in it is findable. Almost all of that volume is a
// handful of lines inside per-draw or per-packet loops, printing millions of
// times with only an address different each time.
//
// Deduplicating message TEXT would not help, because those lines differ every
// time. What repeats is the call site, so that is what is counted. Each LOGF
// gets its own counter: the first messages from a site are written in full,
// after which the site is sampled, and the total is reported at the end. So a
// line that fires once still appears once, and a line that fires ten million
// times costs a few hundred lines instead of a gigabyte.
//
// The repo already does this by hand in the worst offenders, with a local
// static atomic and a `< 2048` check. This is that idiom, applied everywhere,
// without needing to guess in advance which site will be the next problem.
struct Site {
	const char*           file  = nullptr;
	int                   line  = 0;
	std::atomic<uint64_t> count = 0;
	std::atomic<bool>     registered = false;
};

// True when this occurrence should be written. Called BEFORE the message is
// formatted, so a suppressed line costs an atomic increment rather than a
// printf - which is most of the cost of the logging these loops do.
bool ShouldWrite(Site& site);

// One block naming the sites that were rate limited and how often each fired.
// This is the part that keeps the log honest: nothing disappears silently.
void WriteRateLimitSummary();
void      Write(std::string_view text);
void      Write(fmt::text_style style, std::string_view text);
void      WriteFatal(std::string_view text);
void      WriteFatal(fmt::text_style style, std::string_view text);
void      Flush();

namespace Color {

inline constexpr auto Default       = fmt::text_style {};
inline constexpr auto Red           = fmt::fg(fmt::terminal_color::red);
inline constexpr auto Green         = fmt::fg(fmt::terminal_color::green);
inline constexpr auto Yellow        = fmt::fg(fmt::terminal_color::yellow);
inline constexpr auto Magenta       = fmt::fg(fmt::terminal_color::magenta);
inline constexpr auto Cyan          = fmt::fg(fmt::terminal_color::cyan);
inline constexpr auto White         = fmt::fg(fmt::terminal_color::white);
inline constexpr auto BrightRed     = fmt::fg(fmt::terminal_color::bright_red);
inline constexpr auto BrightGreen   = fmt::fg(fmt::terminal_color::bright_green);
inline constexpr auto BrightYellow  = fmt::fg(fmt::terminal_color::bright_yellow);
inline constexpr auto BrightMagenta = fmt::fg(fmt::terminal_color::bright_magenta);
inline constexpr auto BrightWhite   = fmt::fg(fmt::terminal_color::bright_white);

} // namespace Color

} // namespace Log

// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define LOGF(...)                                                                                  \
	do {                                                                                           \
		static ::Log::Site kyty_log_site {__FILE__, __LINE__};                                     \
		if (!::Log::IsSilent() && ::Log::ShouldWrite(kyty_log_site)) {                             \
			::Log::Write(::fmt::sprintf(__VA_ARGS__));                                             \
		}                                                                                          \
	} while (false)
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define LOGF_COLOR(style, ...)                                                                     \
	do {                                                                                           \
		static ::Log::Site kyty_log_site {__FILE__, __LINE__};                                     \
		if (!::Log::IsSilent() && ::Log::ShouldWrite(kyty_log_site)) {                              \
			::Log::Write((style), ::fmt::sprintf(__VA_ARGS__));                                    \
		}                                                                                          \
	} while (false)

#endif /* KYTY_COMMON_LOGGING_LOG_H_ */
