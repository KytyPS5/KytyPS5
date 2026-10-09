#ifndef EMULATOR_INCLUDE_EMULATOR_LIBS_GAMETRACE_H_
#define EMULATOR_INCLUDE_EMULATOR_LIBS_GAMETRACE_H_

// KYTY_DBG_TRACE_GAMEFILES=1: print (deduplicated) game file opens/stats and
// PlayGo / AppContent / SystemService-language calls. Cheap when disabled.

#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <thread>
#include <unordered_map>
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

// ---- File I/O request tracking (GAMEIO) ------------------------------------------------------
// Begin()/End() around APR / pread / read requests. Per-file aggregate stats every 5 s,
// immediate lines for failures, and a pending>2s report. Callers test Enabled() first.
namespace Io {

struct Req {
	std::string                           file;
	const char*                           api;
	uint64_t                              offset;
	uint64_t                              size;
	std::chrono::steady_clock::time_point t0;
	bool                                  slow_reported;
};

struct FileStats {
	uint64_t reqs = 0, bytes = 0, errors = 0;
	double   sum_ms = 0, max_ms = 0;
};

struct State {
	std::mutex                                 mutex;
	std::unordered_map<uint64_t, Req>          pending;
	std::unordered_map<std::string, FileStats> stats;
	uint64_t                                   next_token = 1;
	uint64_t                                   logged     = 0;
	bool                                       started    = false;
};

inline State& St() {
	static State s;
	return s;
}

inline void Reporter() {
	for (;;) {
		std::this_thread::sleep_for(std::chrono::seconds(5));
		auto&           s   = St();
		const auto      now = std::chrono::steady_clock::now();
		std::lock_guard lock(s.mutex);
		std::unordered_map<std::string, uint64_t> pend;
		for (auto& [tok, r]: s.pending) {
			pend[r.file]++;
			const double age_ms = std::chrono::duration<double, std::milli>(now - r.t0).count();
			if (age_ms > 2000.0 && !r.slow_reported) {
				r.slow_reported = true;
				std::printf("GAMEIO: SLOW-PENDING api=%s file=%s off=%llu size=%llu age_ms=%.0f\n", r.api,
				            r.file.c_str(), static_cast<unsigned long long>(r.offset),
				            static_cast<unsigned long long>(r.size), age_ms);
			}
		}
		for (auto& [f, st]: s.stats) {
			const uint64_t p = pend.count(f) ? pend[f] : 0;
			if (st.reqs == 0 && p == 0) {
				continue;
			}
			std::printf("GAMEIO: file=%s reqs=%llu bytes=%llu avg_ms=%.2f max_ms=%.2f pending=%llu errors=%llu\n",
			            f.c_str(), static_cast<unsigned long long>(st.reqs),
			            static_cast<unsigned long long>(st.bytes), st.reqs ? st.sum_ms / st.reqs : 0.0,
			            st.max_ms, static_cast<unsigned long long>(p),
			            static_cast<unsigned long long>(st.errors));
			st.reqs = 0; st.bytes = 0; st.errors = 0; st.sum_ms = 0; st.max_ms = 0;
		}
		std::fflush(stdout);
	}
}

inline uint64_t Begin(const char* api, const std::string& file, uint64_t offset, uint64_t size) {
	if (!Enabled()) {
		return 0;
	}
	auto&           s = St();
	std::lock_guard lock(s.mutex);
	if (!s.started) {
		s.started = true;
		std::thread(Reporter).detach();
	}
	const uint64_t tok = s.next_token++;
	s.pending.emplace(tok, Req {file, api, offset, size, std::chrono::steady_clock::now(), false});
	return tok;
}

// result: bytes read (>=0) or negative error.
inline void End(uint64_t token, int64_t result) {
	if (token == 0) {
		return;
	}
	auto&           s = St();
	std::lock_guard lock(s.mutex);
	const auto      it = s.pending.find(token);
	if (it == s.pending.end()) {
		return;
	}
	const Req r = it->second;
	s.pending.erase(it);
	const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - r.t0).count();
	auto&        st = s.stats[r.file];
	st.reqs++;
	st.sum_ms += ms;
	st.max_ms = ms > st.max_ms ? ms : st.max_ms;
	if (result < 0) {
		st.errors++;
	} else {
		st.bytes += static_cast<uint64_t>(result);
	}
	const bool fail = result < 0;
	if (fail || ms > 2000.0 || s.logged < 300) {
		s.logged++;
		std::printf("GAMEIO: %s api=%s file=%s off=%llu size=%llu result=%lld lat_ms=%.2f\n",
		            fail ? "FAIL" : (ms > 2000.0 ? "SLOW" : "req"), r.api, r.file.c_str(),
		            static_cast<unsigned long long>(r.offset), static_cast<unsigned long long>(r.size),
		            static_cast<long long>(result), ms);
		std::fflush(stdout);
	}
}

} // namespace Io

} // namespace Libs::GameTrace

#endif
