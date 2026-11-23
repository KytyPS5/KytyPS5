#ifndef KYTY_COMMON_LOGGING_LOG_H_
#define KYTY_COMMON_LOGGING_LOG_H_

#include "common/common.h"

#include <fmt/color.h>
#include <fmt/printf.h>
#include <string_view>

namespace Log {

void Initialize();
void Flush();

struct Lifecycle {
	static constexpr const char* name               = "Log";
	static constexpr auto        initialize         = Log::Initialize;
	static constexpr auto        shutdown           = Log::Flush;
	static constexpr auto        emergency_shutdown = Log::Flush;
};

// Set by LoadExec for the program it starts, which continues the log file of the one it replaces
// instead of starting it over.
inline constexpr const char* RelaunchedVariable = "KYTY_RELAUNCHED";

bool IsSilent();
void Write(fmt::text_style style, std::string_view text);
// Also shown on the console when the log goes to a file or nowhere.
void WriteToConsoleAndLog(std::string_view text);

namespace Color {

inline constexpr auto Default       = fmt::text_style {};
inline constexpr auto Red           = fmt::fg(fmt::terminal_color::red);
inline constexpr auto Green         = fmt::fg(fmt::terminal_color::green);
inline constexpr auto Yellow        = fmt::fg(fmt::terminal_color::yellow);
inline constexpr auto Cyan          = fmt::fg(fmt::terminal_color::cyan);
inline constexpr auto White         = fmt::fg(fmt::terminal_color::white);
inline constexpr auto BrightRed     = fmt::fg(fmt::terminal_color::bright_red);
inline constexpr auto BrightYellow  = fmt::fg(fmt::terminal_color::bright_yellow);
inline constexpr auto BrightMagenta = fmt::fg(fmt::terminal_color::bright_magenta);
inline constexpr auto BrightWhite   = fmt::fg(fmt::terminal_color::bright_white);

} // namespace Color

// Out of line, so callers on small guest stacks (fibers) do not reserve the formatting frame.
template <typename... Args>
[[gnu::noinline]] void Printf(fmt::text_style style, const char* format, const Args&... args) {
	Write(style, fmt::sprintf(format, args...));
}

} // namespace Log

// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define LOGF(...)                                                                                  \
	do {                                                                                           \
		if (!::Log::IsSilent()) {                                                                  \
			::Log::Printf({}, __VA_ARGS__);                                                        \
		}                                                                                          \
	} while (false)
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define LOGF_COLOR(style, ...)                                                                     \
	do {                                                                                           \
		if (!::Log::IsSilent()) {                                                                  \
			::Log::Printf((style), __VA_ARGS__);                                                   \
		}                                                                                          \
	} while (false)

#endif /* KYTY_COMMON_LOGGING_LOG_H_ */
