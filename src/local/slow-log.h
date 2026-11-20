#pragma once
// Local diagnostic: one stdout line for every call of an instrumented renderer operation that
// takes longer than KYTY_SLOW_LOG_MS (default off). The hitches of the walk route (hundreds of
// milliseconds in one texture lookup, page-watcher update or tile setup) are single calls; the
// line names the resource so the case can be reproduced.
// KYTY_HITCH_LOG_MS (default 100; 0: off) covers only what runs once per frame or on a miss (frames,
// shader translations, pipeline creations, image uploads, unmaps), cheap enough for every session: what
// a stutter was.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <utility>
#include <x86intrin.h>

namespace SlowLog {

inline double Threshold() {
	static const double ms = [] {
		const char* text = std::getenv("KYTY_SLOW_LOG_MS");
		return text != nullptr ? std::atof(text) : 0.0;
	}();
	return ms;
}

inline double HitchThreshold() {
	static const double ms = [] {
		const char* text = std::getenv("KYTY_HITCH_LOG_MS");
#if defined(KYTY_STATIC_PRECOMPILE)
		constexpr double fallback = 0.0; // (the precompile program: compiles that long are its work, not stutters)
#else
		constexpr double fallback = 0.0; // (off: a normal session prints no diagnostics; KYTY_HITCH_LOG_MS=100 turns the stutter lines on)
#endif
		return text != nullptr ? std::atof(text) : fallback;
	}();
	return ms;
}

// Calls report(elapsed_ms) on destruction when the scope took longer than the threshold.
template <typename Report>
class Scope {
public:
	explicit Scope(Report report, double threshold = Threshold())
	    : m_threshold(threshold), m_report(std::move(report)) {
		if (m_threshold > 0.0) m_start = std::chrono::steady_clock::now();
	}
	~Scope() {
		if (m_threshold <= 0.0) return;
		const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - m_start).count();
		if (ms >= m_threshold) {
			// The TSC at the end of the call places the line on the live trace's timeline.
			std::printf("[tsc %llu] ", static_cast<unsigned long long>(__rdtsc()));
			m_report(ms);
			std::fflush(stdout);
		}
	}
	Scope(const Scope&)            = delete;
	Scope& operator=(const Scope&) = delete;

private:
	double                                m_threshold;
	Report                                m_report;
	std::chrono::steady_clock::time_point m_start {};
};

} // namespace SlowLog
