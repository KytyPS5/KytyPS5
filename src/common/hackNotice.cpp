#include "common/hackNotice.h"

// The unit tests that compile the walkers do not link the logging library: they print to stdout.
#ifdef KYTY_HACK_NOTICE_STDOUT
#include <cstdio>
#define HACK_LOG(...) std::printf(__VA_ARGS__)
#else
#include "common/logging/log.h"
// WriteToConsoleAndLog reaches stdout and emulator.log whatever the log direction (LOGF is silent
// unless the printf log is enabled).
#define HACK_LOG(...) ::Log::WriteToConsoleAndLog(::fmt::sprintf(__VA_ARGS__))
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>

namespace Common::HackNotice {
namespace {

constexpr size_t   TABLE_SIZE    = 512;
constexpr size_t   PROBE_LIMIT   = 24;
constexpr int64_t  SUMMARY_NS    = 10'000'000'000;
constexpr size_t   SUMMARY_LINES = 6;

struct Entry {
	std::atomic<uint64_t> key {0};
	std::atomic<bool>     ready {false};
	std::atomic<uint64_t> count {0};
	std::atomic<uint64_t> reported {0};
	// Written once by the thread that claimed the entry, before `ready`.
	const char*           hack        = nullptr;
	const char*           site        = nullptr;
	uint64_t              shader_hash = 0;
	uint64_t              where       = 0;
};

std::array<Entry, TABLE_SIZE> g_table;
std::atomic<uint64_t>         g_overflow {0};
std::atomic<int64_t>          g_next_summary_ns {0};
std::atomic<uint64_t>         g_calls {0};

uint64_t Mix(uint64_t x) {
	x += 0x9e3779b97f4a7c15ULL;
	x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
	x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
	return x ^ (x >> 31);
}

int64_t NowNs() {
	return std::chrono::duration_cast<std::chrono::nanoseconds>(
	           std::chrono::steady_clock::now().time_since_epoch())
	    .count();
}

std::string Shader(uint64_t shader_hash) {
	char text[32];
	if (shader_hash == 0) return "unknown";
	std::snprintf(text, sizeof(text), "0x%016llx", static_cast<unsigned long long>(shader_hash));
	return text;
}

void Summarize() {
	struct Line {
		const Entry* entry;
		uint64_t     delta;
		uint64_t     total;
	};
	std::array<Line, TABLE_SIZE> lines {};
	size_t                       line_count = 0;
	uint64_t                     period     = 0;
	for (auto& entry: g_table) {
		if (!entry.ready.load(std::memory_order_acquire)) continue;
		const auto total = entry.count.load(std::memory_order_relaxed);
		const auto delta = total - entry.reported.exchange(total, std::memory_order_relaxed);
		if (delta == 0) continue;
		period += delta;
		lines[line_count++] = {&entry, delta, total};
	}
	const auto overflow = g_overflow.exchange(0, std::memory_order_relaxed);
	period += overflow;
	if (period == 0) return;
	const auto shown = std::min(line_count, SUMMARY_LINES);
	std::partial_sort(lines.begin(), lines.begin() + static_cast<std::ptrdiff_t>(shown),
	                  lines.begin() + static_cast<std::ptrdiff_t>(line_count),
	                  [](const Line& a, const Line& b) { return a.delta > b.delta; });
	HACK_LOG("[HACK] summary of the last 10 s: %llu applications in %zu sources\n",
	     static_cast<unsigned long long>(period), line_count + (overflow != 0 ? 1u : 0u));
	for (size_t i = 0; i < shown; ++i) {
		const auto& line = lines[i];
		HACK_LOG("[HACK]   %s at %s shader=%s where=0x%llx: +%llu (total %llu)\n", line.entry->hack,
		     line.entry->site, Shader(line.entry->shader_hash).c_str(),
		     static_cast<unsigned long long>(line.entry->where),
		     static_cast<unsigned long long>(line.delta),
		     static_cast<unsigned long long>(line.total));
	}
	if (line_count > shown) {
		HACK_LOG("[HACK]   (+%zu more sources)\n", line_count - shown);
	}
}

} // namespace

void Report(const char* hack, const char* site, uint64_t shader_hash, uint64_t where,
            std::string_view detail) {
	if (shader_hash == 0) shader_hash = CurrentShaderSlot();
	auto key = Mix(reinterpret_cast<uintptr_t>(hack));
	key      = Mix(key ^ reinterpret_cast<uintptr_t>(site));
	key      = Mix(key ^ shader_hash);
	key      = Mix(key ^ where);
	if (key == 0) key = 1;

	Entry* found = nullptr;
	bool   first = false;
	for (size_t i = 0; i < PROBE_LIMIT; ++i) {
		auto&    entry    = g_table[(key + i) % TABLE_SIZE];
		uint64_t expected = entry.key.load(std::memory_order_acquire);
		if (expected == 0 && entry.key.compare_exchange_strong(expected, key,
		                                                       std::memory_order_acq_rel)) {
			entry.hack        = hack;
			entry.site        = site;
			entry.shader_hash = shader_hash;
			entry.where       = where;
			entry.ready.store(true, std::memory_order_release);
			found = &entry;
			first = true;
			break;
		}
		if (expected == key) {
			found = &entry;
			break;
		}
	}
	if (found == nullptr) {
		g_overflow.fetch_add(1, std::memory_order_relaxed);
	} else {
		found->count.fetch_add(1, std::memory_order_relaxed);
		if (first) {
			// The line below counts as reported; later applications go to the summaries.
			found->reported.store(1, std::memory_order_relaxed);
			const std::string suffix = detail.empty() ? std::string() : ", " + std::string(detail);
			HACK_LOG("[HACK] %s: first at %s, shader=%s, where=0x%llx%s\n", hack, site,
			     Shader(shader_hash).c_str(), static_cast<unsigned long long>(where),
			     suffix.c_str());
		}
	}

	// The first report arms the summary timer; any thread that sees it expire prints the summary.
	// Reading the clock on every report would be the main cost of a busy key, so after the first
	// few reports it is read once in 16.
	const auto calls = g_calls.fetch_add(1, std::memory_order_relaxed);
	if (calls >= 64 && (calls & 15u) != 0) return;
	const auto now      = NowNs();
	auto       deadline = g_next_summary_ns.load(std::memory_order_relaxed);
	if (deadline == 0) {
		g_next_summary_ns.compare_exchange_strong(deadline, now + SUMMARY_NS,
		                                          std::memory_order_relaxed);
	} else if (now >= deadline &&
	           g_next_summary_ns.compare_exchange_strong(deadline, now + SUMMARY_NS,
	                                                     std::memory_order_acq_rel)) {
		Summarize();
	}
}

} // namespace Common::HackNotice
