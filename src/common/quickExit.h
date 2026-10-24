#ifndef KYTY_COMMON_QUICKEXIT_H_
#define KYTY_COMMON_QUICKEXIT_H_

#include "common/common.h"

#include <cstdlib>

// The C11 quick_exit / at_quick_exit functions terminate the process without running
// the ordinary atexit handlers, which is what the emulator wants while guest threads
// are still running. MinGW's msvcrt-based runtime does not provide them (<stdlib.h>
// only declares them when _UCRT is defined), so emulate them there.
namespace Common {

namespace Detail {
constexpr int QUICK_EXIT_HANDLERS_MAX = 16;

inline void (*g_quick_exit_handlers[QUICK_EXIT_HANDLERS_MAX])() = {};
inline int g_quick_exit_handler_count                           = 0;
} // namespace Detail

inline int AtQuickExit(void (*handler)()) {
#if defined(_WIN32) && defined(__MINGW32__) && !defined(_UCRT)
	if (handler == nullptr || Detail::g_quick_exit_handler_count >= Detail::QUICK_EXIT_HANDLERS_MAX) {
		return -1;
	}
	Detail::g_quick_exit_handlers[Detail::g_quick_exit_handler_count++] = handler;
	return 0;
#else
	return std::at_quick_exit(handler);
#endif
}

[[noreturn]] inline void QuickExit(int status) {
#if defined(_WIN32) && defined(__MINGW32__) && !defined(_UCRT)
	for (int i = Detail::g_quick_exit_handler_count - 1; i >= 0; i--) {
		Detail::g_quick_exit_handlers[i]();
	}
	std::_Exit(status);
#else
	std::quick_exit(status);
#endif
}

} // namespace Common

#endif
