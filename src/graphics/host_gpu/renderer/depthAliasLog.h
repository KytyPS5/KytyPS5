#pragma once

// KYTY_DBG_DEPTH_ALIAS=1 enables NHL27DEPTH diagnostic lines about depth clears and
// depth/texture image aliasing. Off by default. The env var is read once; output is capped
// at kMaxLines lines in total so it cannot flood the log.
//
// KYTY_DBG_DEPTH_ALIAS_ADDR=<hex>[,<hex>...] (0x prefix optional) restricts logging to events
// whose guest range overlaps one of the windows [addr, addr + kWindowSize). The line cap is
// raised to kMaxLinesFiltered while the filter is set.

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <vector>

namespace DepthAliasLog {

constexpr uint64_t kWindowSize        = 0x800000; // 8 MiB
constexpr uint32_t kMaxLines          = 4000;
constexpr uint32_t kMaxLinesFiltered  = 20000;

inline bool Enabled() {
	static const bool enabled = [] {
		const char* value = std::getenv("KYTY_DBG_DEPTH_ALIAS");
		return value != nullptr && value[0] == '1';
	}();
	return enabled;
}

// Window base addresses from KYTY_DBG_DEPTH_ALIAS_ADDR, parsed once.
inline const std::vector<uint64_t>& AddrWindows() {
	static const std::vector<uint64_t> windows = [] {
		std::vector<uint64_t> list;
		const char*           v = std::getenv("KYTY_DBG_DEPTH_ALIAS_ADDR");
		while (v != nullptr && *v != '\0') {
			char*      end  = nullptr;
			const auto addr = std::strtoull(v, &end, 16);
			if (end == v) {
				break;
			}
			list.push_back(addr);
			while (*end == ' ' || *end == '\t') {
				end++;
			}
			v = (*end == ',') ? end + 1 : end;
		}
		return list;
	}();
	return windows;
}

// True when KYTY_DBG_DEPTH_ALIAS_ADDR names at least one window.
inline bool Filtered() {
	return !AddrWindows().empty();
}

// True when the guest range [addr, addr + size) overlaps any filter window.
inline bool InWindow(uint64_t addr, uint64_t size) {
	const uint64_t len = size == 0 ? 1 : size;
	for (const auto base: AddrWindows()) {
		if (addr < base + kWindowSize && base < addr + len) {
			return true;
		}
	}
	return false;
}

// True when an event should be considered for logging: always without a filter, otherwise
// only when its guest range overlaps a window.
inline bool Want(uint64_t addr, uint64_t size) {
	return !Filtered() || InWindow(addr, size);
}

// Two-range variant for events that involve an old and a new image.
inline bool WantEither(uint64_t addr1, uint64_t size1, uint64_t addr2, uint64_t size2) {
	return !Filtered() || InWindow(addr1, size1) || InWindow(addr2, size2);
}

// Returns true while the line budget is not exhausted.
inline bool TakeLine() {
	static const uint32_t cap = Filtered() ? kMaxLinesFiltered : kMaxLines;
	static std::atomic<uint32_t> lines {0};
	if (lines.load(std::memory_order_relaxed) >= cap) {
		return false;
	}
	return lines.fetch_add(1, std::memory_order_relaxed) < cap;
}

// Sequence number of depth-target draws seen while logging is enabled.
inline uint64_t NextSeq() {
	static std::atomic<uint64_t> seq {0};
	return seq.fetch_add(1, std::memory_order_relaxed);
}

} // namespace DepthAliasLog
