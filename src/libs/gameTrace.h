#ifndef EMULATOR_INCLUDE_EMULATOR_LIBS_GAMETRACE_H_
#define EMULATOR_INCLUDE_EMULATOR_LIBS_GAMETRACE_H_

// KYTY_DBG_TRACE_GAMEFILES=1: print (deduplicated) game file opens/stats and
// PlayGo / AppContent / SystemService-language calls. Cheap when disabled.

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <unordered_set>

namespace Libs::GameTrace {

inline bool Enabled() {
	static const bool enabled = [] {
		const char* v = std::getenv("KYTY_DBG_TRACE_GAMEFILES");
		return v != nullptr && v[0] != '\0' && v[0] != '0';
	}();
	return enabled;
}

// Prints once per distinct formatted line.
inline void Line(const char* fmt, ...) {
	if (!Enabled()) {
		return;
	}
	char    buf[1024];
	va_list args;
	va_start(args, fmt);
	std::vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);

	static std::mutex                      mutex;
	static std::unordered_set<std::string> seen;
	std::lock_guard                        lock(mutex);
	if (seen.insert(buf).second) {
		std::printf("GAMETRACE: %s\n", buf);
		std::fflush(stdout);
	}
}

} // namespace Libs::GameTrace

#endif
