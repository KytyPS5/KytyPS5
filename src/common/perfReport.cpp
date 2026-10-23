#include "common/perfReport.h"

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Common::PerfReport {

namespace {

constexpr uint64_t IntervalNs    = 5'000'000'000ull;
constexpr uint64_t MinLastIntervalNs = 100'000'000ull;
constexpr size_t   TopEntries    = 15;
constexpr size_t   MaxGpuWrites  = 65536;
constexpr size_t   MaxFaultPages = size_t {1} << 20u;
constexpr int      RegionShift   = 20; // fault regions are 1 MiB
constexpr int      PageShift     = 12;

struct Stat {
	uint64_t count = 0;
	uint64_t ns    = 0;

	void Add(uint64_t elapsed) {
		count++;
		ns += elapsed;
	}
};

struct GpuWrite {
	uint64_t size        = 0;
	uint64_t shader_hash = 0;
};

// Everything below is reset after each interval except `gpu_writes`, `fault_pages`,
// `buffer_count` and `buffer_bytes`, which describe current state.
struct State {
	std::mutex lock;
	FILE*      file           = nullptr;
	uint64_t   start          = 0;
	uint64_t   interval_start = 0;
	// Checked without the lock so MaybeWrite stays cheap between intervals.
	std::atomic<uint64_t> next_write {0};

	// Guarded by `lock`.
	std::unordered_map<std::string, Stat> waits;    // by call site
	std::unordered_map<std::string, Stat> submits;  // by call site
	std::unordered_map<std::string, Stat> download_causes;
	std::unordered_map<uint64_t, Stat>    fault_regions;
	std::unordered_map<size_t, Stat>      fault_threads;
	std::unordered_map<uint64_t, uint64_t> fault_pages; // page -> interval of its last fault
	std::map<uint64_t, GpuWrite>          gpu_writes;   // by base address
	Stat                                  waits_total;
	Stat                                  submits_total;
	Stat                                  faults;
	Stat                                  read_faults;
	Stat                                  download_faults;
	uint64_t                              fault_buffer_ns  = 0;
	uint64_t                              fault_texture_ns = 0;
	uint64_t                              fault_new_pages  = 0;
	uint64_t                              fault_repeats    = 0; // page faulted again in one interval
	uint64_t                              interval         = 0;

	// Lock-free counters.
	std::atomic<uint64_t> flips {0};
	std::atomic<uint64_t> gpu_idle_ns {0};
	std::atomic<uint64_t> gpu_blocked_ns {0};
	std::atomic<uint64_t> protect_calls {0};
	std::atomic<uint64_t> protect_lock_ns {0};
	std::atomic<uint64_t> protect_call_ns {0};
	std::atomic<uint64_t> uploads {0};
	std::atomic<uint64_t> upload_bytes {0};
	std::atomic<uint64_t> bda_calls {0};
	std::atomic<uint64_t> bda_bytes {0};
	std::atomic<uint64_t> gc_ticks {0};
	std::atomic<uint64_t> buffer_count {0};
	std::atomic<uint64_t> buffer_bytes {0};
};

State& Get() {
	static State state;
	return state;
}

uint64_t Elapsed(uint64_t start) {
	return start == 0 ? 0 : Detail::NowNs() - start;
}

std::string SiteName(bool gpu_thread) {
	std::string name = gpu_thread ? "gpu   " : "other ";
	const auto* site = Detail::g_site;
	if (site == nullptr) {
		return name + "(outside an instrumented scheduler call)";
	}
	std::string file  = site->file_name();
	const auto  slash = file.find_last_of("/\\");
	if (slash != std::string::npos) {
		file.erase(0, slash + 1);
	}
	return name + file + ":" + std::to_string(site->line()) + " " + site->function_name();
}

double PerSecond(uint64_t value, double seconds) {
	return static_cast<double>(value) / seconds;
}

double MsPerSecond(uint64_t ns, double seconds) {
	return static_cast<double>(ns) / 1e6 / seconds;
}

double MbPerSecond(uint64_t bytes, double seconds) {
	return static_cast<double>(bytes) / 1e6 / seconds;
}

template <typename Key>
void WriteTop(FILE* f, const char* title, std::unordered_map<Key, Stat>& map, double seconds) {
	std::vector<std::pair<Key, Stat>> entries(map.begin(), map.end());
	map.clear();
	if (entries.empty()) {
		return;
	}
	std::ranges::sort(entries, [](const auto& l, const auto& r) { return l.second.ns > r.second.ns; });
	std::fprintf(f, "  %s:\n", title);
	for (size_t i = 0; i < entries.size() && i < TopEntries; i++) {
		const auto& [key, stat] = entries[i];
		std::fprintf(f, "    %8.1f/s %8.1f ms/s  ", PerSecond(stat.count, seconds), MsPerSecond(stat.ns, seconds));
		if constexpr (std::is_same_v<Key, std::string>) {
			std::fprintf(f, "%s\n", key.c_str());
		} else {
			std::fprintf(f, "0x%010" PRIx64 "\n", static_cast<uint64_t>(key) << RegionShift);
		}
	}
	if (entries.size() > TopEntries) {
		std::fprintf(f, "    (%zu more)\n", entries.size() - TopEntries);
	}
}

void WriteHeader(FILE* f) {
	std::fprintf(f,
	             "Kyty performance report. One block per %.0f s interval; rates are per second of "
	             "wall time.\n"
	             "\"ms/s\" is time spent per second: 1000 ms/s keeps one thread fully busy.\n"
	             "See docs/performance-report.md for what each line measures.\n\n",
	             static_cast<double>(IntervalNs) / 1e9);
}

void WriteInterval(State& s, uint64_t now, bool last = false) {
	auto*        f       = s.file;
	const double seconds = static_cast<double>(now - s.interval_start) / 1e9;
	std::fprintf(f, "=== %.1f s (%sinterval %.1f s)\n", static_cast<double>(now - s.start) / 1e9,
	             last ? "last, partial " : "", seconds);
	std::fprintf(f, "  frames: %.1f/s\n", PerSecond(s.flips.exchange(0), seconds));
	std::fprintf(f, "  GPU command thread: idle %.1f%%, blocked on guest waits %.1f%%\n",
	             100.0 * static_cast<double>(s.gpu_idle_ns.exchange(0)) / 1e9 / seconds,
	             100.0 * static_cast<double>(s.gpu_blocked_ns.exchange(0)) / 1e9 / seconds);
	std::fprintf(f, "  GPU waits: %.1f/s, %.1f ms/s\n", PerSecond(s.waits_total.count, seconds),
	             MsPerSecond(s.waits_total.ns, seconds));
	std::fprintf(f, "  queue submits: %.1f/s, %.1f ms/s\n", PerSecond(s.submits_total.count, seconds),
	             MsPerSecond(s.submits_total.ns, seconds));
	std::fprintf(f,
	             "  write-tracking faults: %.0f/s, %.1f ms/s in the handler (buffer cache %.1f ms/s, "
	             "texture cache %.1f ms/s)\n",
	             PerSecond(s.faults.count, seconds), MsPerSecond(s.faults.ns, seconds),
	             MsPerSecond(s.fault_buffer_ns, seconds), MsPerSecond(s.fault_texture_ns, seconds));
	std::fprintf(f, "    read faults: %.0f/s, %.1f ms/s\n", PerSecond(s.read_faults.count, seconds),
	             MsPerSecond(s.read_faults.ns, seconds));
	std::fprintf(f, "    faults that downloaded GPU data first: %.0f/s, %.1f ms/s\n",
	             PerSecond(s.download_faults.count, seconds), MsPerSecond(s.download_faults.ns, seconds));
	std::fprintf(f, "    pages: %.0f/s faulting for the first time, %.0f/s faulting again within an interval\n",
	             PerSecond(s.fault_new_pages, seconds), PerSecond(s.fault_repeats, seconds));
	{
		std::vector<Stat> threads;
		for (const auto& [id, stat]: s.fault_threads) {
			threads.push_back(stat);
		}
		std::ranges::sort(threads, [](const Stat& l, const Stat& r) { return l.ns > r.ns; });
		std::fprintf(f, "    by thread (%zu):", threads.size());
		for (size_t i = 0; i < threads.size() && i < 4; i++) {
			std::fprintf(f, " %.0f/s %.1f ms/s;", PerSecond(threads[i].count, seconds),
			             MsPerSecond(threads[i].ns, seconds));
		}
		std::fprintf(f, "\n");
	}
	std::fprintf(f, "  tracking protection changes: %.0f/s, %.1f ms/s waiting for the address-space lock, %.1f ms/s changing protection\n",
	             PerSecond(s.protect_calls.exchange(0), seconds), MsPerSecond(s.protect_lock_ns.exchange(0), seconds),
	             MsPerSecond(s.protect_call_ns.exchange(0), seconds));
	std::fprintf(f, "  buffer uploads: %.0f/s, %.1f MB/s\n", PerSecond(s.uploads.exchange(0), seconds),
	             MbPerSecond(s.upload_bytes.exchange(0), seconds));
	std::fprintf(f, "  BDA synchronization queued: %.0f/s, %.1f MB/s\n", PerSecond(s.bda_calls.exchange(0), seconds),
	             MbPerSecond(s.bda_bytes.exchange(0), seconds));
	std::fprintf(f, "  buffer cache: %" PRIu64 " buffers, %.1f MB; %.1f GC ticks/s\n", s.buffer_count.load(),
	             static_cast<double>(s.buffer_bytes.load()) / 1e6, PerSecond(s.gc_ticks.exchange(0), seconds));
	WriteTop(f, "GPU waits by call site", s.waits, seconds);
	WriteTop(f, "queue submits by call site", s.submits, seconds);
	WriteTop(f, "write-tracking faults by 1 MiB region", s.fault_regions, seconds);
	WriteTop(f, "download faults by the shader write that owns the page", s.download_causes, seconds);
	std::fprintf(f, "\n");
	std::fflush(f);

	s.waits_total = s.submits_total = s.faults = s.read_faults = s.download_faults = {};
	s.fault_buffer_ns = s.fault_texture_ns = s.fault_new_pages = s.fault_repeats = 0;
	s.fault_threads.clear();
	s.interval++;
}

} // namespace

uint64_t Detail::NowNs() noexcept {
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
	                                 std::chrono::steady_clock::now().time_since_epoch())
	                                 .count());
}

void Configure(bool enabled, const std::filesystem::path& file) {
	auto&            s = Get();
	std::scoped_lock guard {s.lock};
	Detail::g_enabled.store(false, std::memory_order_relaxed);
	if (s.file != nullptr) {
		// Keep the time since the last block, so a short session still gets a report. Skip a
		// sliver too short to give meaningful rates.
		const auto now = Detail::NowNs();
		if (now - s.interval_start >= MinLastIntervalNs) {
			WriteInterval(s, now, true);
		}
		std::fclose(s.file);
		s.file = nullptr;
	}
	if (!enabled) {
		return;
	}
	if (file.empty()) {
		std::fprintf(stderr, "performance report: no output file\n");
		return;
	}
#if defined(_WIN32)
	s.file = _wfopen(file.c_str(), L"w");
#else
	s.file = std::fopen(file.c_str(), "w");
#endif
	if (s.file == nullptr) {
		std::fprintf(stderr, "performance report: cannot open %s\n", file.string().c_str());
		return;
	}
	WriteHeader(s.file);
	s.start = s.interval_start = Detail::NowNs();
	s.next_write.store(s.start + IntervalNs, std::memory_order_relaxed);
	Detail::g_enabled.store(true, std::memory_order_relaxed);
}

void NoteGpuWait(bool gpu_thread, uint64_t start) {
	if (!Enabled() || start == 0) {
		return;
	}
	const auto       elapsed = Elapsed(start);
	auto             name    = SiteName(gpu_thread);
	auto&            s       = Get();
	std::scoped_lock guard {s.lock};
	s.waits[std::move(name)].Add(elapsed);
	s.waits_total.Add(elapsed);
}

void NoteQueueSubmit(bool gpu_thread, uint64_t start) {
	if (!Enabled() || start == 0) {
		return;
	}
	const auto       elapsed = Elapsed(start);
	auto             name    = SiteName(gpu_thread);
	auto&            s       = Get();
	std::scoped_lock guard {s.lock};
	s.submits[std::move(name)].Add(elapsed);
	s.submits_total.Add(elapsed);
}

void NoteGpuIdle(uint64_t start) {
	if (Enabled() && start != 0) {
		Get().gpu_idle_ns.fetch_add(Elapsed(start), std::memory_order_relaxed);
	}
}

void NoteGpuBlocked(uint64_t start) {
	if (Enabled() && start != 0) {
		Get().gpu_blocked_ns.fetch_add(Elapsed(start), std::memory_order_relaxed);
	}
}

void NoteFlip() {
	if (Enabled()) {
		Get().flips.fetch_add(1, std::memory_order_relaxed);
	}
}

void NoteFault(uint64_t address, bool write, uint64_t start, uint64_t buffer_done, bool downloaded) {
	if (!Enabled() || start == 0) {
		return;
	}
	const auto       now     = Detail::NowNs();
	const auto       elapsed = now - start;
	const auto       thread  = std::hash<std::thread::id> {}(std::this_thread::get_id());
	auto&            s       = Get();
	std::scoped_lock guard {s.lock};
	s.faults.Add(elapsed);
	if (write) {
		if (buffer_done != 0) {
			s.fault_buffer_ns += buffer_done - start;
			s.fault_texture_ns += now - buffer_done;
		}
	} else {
		s.read_faults.Add(elapsed);
	}
	s.fault_regions[address >> RegionShift].Add(elapsed);
	s.fault_threads[thread].Add(elapsed);
	if (s.fault_pages.size() >= MaxFaultPages) {
		s.fault_pages.clear(); // bounds memory in long sessions; only the repeat counts restart
	}
	const auto [page, inserted] = s.fault_pages.try_emplace(address >> PageShift, s.interval);
	if (inserted) {
		s.fault_new_pages++;
	} else {
		if (page->second == s.interval) {
			s.fault_repeats++;
		}
		page->second = s.interval;
	}
	if (!downloaded) {
		return;
	}
	s.download_faults.Add(elapsed);
	// Find the GPU-written binding that covers the page. Bindings can nest, so look back a few.
	const GpuWrite* owner = nullptr;
	uint64_t        base  = 0;
	auto            it    = s.gpu_writes.upper_bound(address);
	for (int i = 0; i < 64 && it != s.gpu_writes.begin(); i++) {
		--it;
		if (address < it->first + it->second.size) {
			owner = &it->second;
			base  = it->first;
			break;
		}
	}
	char key[160];
	if (owner != nullptr) {
		std::snprintf(key, sizeof(key), "%s in 0x%010" PRIx64 "+0x%" PRIx64 " written by shader 0x%016" PRIx64,
		              write ? "write" : "read ", base, owner->size, owner->shader_hash);
	} else {
		std::snprintf(key, sizeof(key), "%s at 0x%010" PRIx64 " (no GPU-written binding seen)",
		              write ? "write" : "read ", address & ~((uint64_t {1} << PageShift) - 1));
	}
	s.download_causes[key].Add(elapsed);
}

void NoteGpuWrite(uint64_t address, uint64_t size, uint64_t shader_hash) {
	if (!Enabled()) {
		return;
	}
	auto&            s = Get();
	std::scoped_lock guard {s.lock};
	if (s.gpu_writes.size() >= MaxGpuWrites) {
		s.gpu_writes.clear();
	}
	auto& entry       = s.gpu_writes[address];
	entry.size        = std::max(entry.size, size);
	entry.shader_hash = shader_hash;
}

void NoteProtect(uint64_t lock_start, uint64_t call_start) {
	if (!Enabled() || lock_start == 0 || call_start == 0) {
		return;
	}
	auto& s = Get();
	s.protect_calls.fetch_add(1, std::memory_order_relaxed);
	s.protect_lock_ns.fetch_add(call_start - lock_start, std::memory_order_relaxed);
	s.protect_call_ns.fetch_add(Elapsed(call_start), std::memory_order_relaxed);
}

void NoteBufferUpload(uint64_t bytes) {
	if (Enabled()) {
		auto& s = Get();
		s.uploads.fetch_add(1, std::memory_order_relaxed);
		s.upload_bytes.fetch_add(bytes, std::memory_order_relaxed);
	}
}

void NoteBdaQueue(uint64_t bytes) {
	if (Enabled()) {
		auto& s = Get();
		s.bda_calls.fetch_add(1, std::memory_order_relaxed);
		s.bda_bytes.fetch_add(bytes, std::memory_order_relaxed);
	}
}

void NoteBufferCache(uint64_t buffers, uint64_t bytes) {
	if (Enabled()) {
		auto& s = Get();
		s.buffer_count.store(buffers, std::memory_order_relaxed);
		s.buffer_bytes.store(bytes, std::memory_order_relaxed);
		s.gc_ticks.fetch_add(1, std::memory_order_relaxed);
	}
}

void MaybeWrite() {
	if (!Enabled()) {
		return;
	}
	auto&      s   = Get();
	const auto now = Detail::NowNs();
	if (now < s.next_write.load(std::memory_order_relaxed)) {
		return;
	}
	std::scoped_lock guard {s.lock};
	if (s.file == nullptr || now - s.interval_start < IntervalNs) {
		return;
	}
	WriteInterval(s, now);
	s.interval_start = now;
	s.next_write.store(now + IntervalNs, std::memory_order_relaxed);
}

} // namespace Common::PerfReport
