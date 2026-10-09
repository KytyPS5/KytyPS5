#pragma once

// KYTY_DBG_DEPTH_ALIAS=1 enables NHL27DEPTH diagnostic lines about depth clears and
// depth/texture image aliasing. Off by default. The env var is read once; output is capped
// at kMaxLines lines in total so it cannot flood the log.

#include <atomic>
#include <cstdint>
#include <cstdlib>

namespace DepthAliasLog {

inline bool Enabled() {
	static const bool enabled = [] {
		const char* value = std::getenv("KYTY_DBG_DEPTH_ALIAS");
		return value != nullptr && value[0] == '1';
	}();
	return enabled;
}

// Returns true while the global line budget is not exhausted.
inline bool TakeLine() {
	constexpr uint32_t kMaxLines = 4000;
	static std::atomic<uint32_t> lines {0};
	if (lines.load(std::memory_order_relaxed) >= kMaxLines) {
		return false;
	}
	return lines.fetch_add(1, std::memory_order_relaxed) < kMaxLines;
}

// Sequence number of depth-target draws seen while logging is enabled.
inline uint64_t NextSeq() {
	static std::atomic<uint64_t> seq {0};
	return seq.fetch_add(1, std::memory_order_relaxed);
}

} // namespace DepthAliasLog
