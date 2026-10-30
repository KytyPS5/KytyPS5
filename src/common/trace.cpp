#include "common/trace.h"

#include "common/emulatorConfig.h"
#include "common/perfCounters.h"

#include <algorithm>
#include <array>
#include <cinttypes>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#if KYTY_PLATFORM != KYTY_PLATFORM_WINDOWS
#include <execinfo.h>
#include <pthread.h>
#include <unistd.h>
#endif

#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#endif
#include "stb_image_write.h"
#ifdef __clang__
#pragma clang diagnostic pop
#endif

namespace Trace {

namespace {

constexpr int      MAX_DEPTH            = 8;
constexpr int      RECENT               = 32;
constexpr int      MAX_BACKTRACE        = 48;
constexpr uint64_t REPORT_PERIOD_NS     = 2'000'000'000;
constexpr uint64_t FIRST_FRAME_GRACE_NS = 180'000'000'000;

std::atomic<Site*> g_sites {nullptr};

constexpr std::array<const char*, static_cast<size_t>(Column::Count)> COLUMN_NAMES = {
    "gpu_wait",      "shader_compile", "pipeline_compile",
    "pipeline_wait", "frame_acquire",  "guest_equeue_wait", "gpu_idle",
    "gpu_command",   "gpu_graphics",   "gpu_compute"};

// A thread that entered a timed site: its open sites, for threads.txt and hang.txt.
struct ThreadState {
	int                                          tid = 0;
	char                                         name[32] {};
	std::atomic<int>                             depth {0};
	std::array<std::atomic<Site*>, MAX_DEPTH>    stack {};
	std::array<std::atomic<uint64_t>, MAX_DEPTH> since {};
	std::atomic<bool>                            alive {true};
	// The sites this thread entered last, newest at recent_count - 1 (mod RECENT).
	std::array<std::atomic<Site*>, RECENT> recent {};
	std::atomic<uint32_t>                  recent_count {0};
	// Time in outermost timed sites (mostly waits) since the thread first entered one.
	uint64_t              first_ns = 0;
	std::atomic<uint64_t> in_sites_ns {0};
	// The outermost site the thread spent most time in, roughly (the last long one).
	std::atomic<Site*>    longest_site {nullptr};
	std::atomic<uint64_t> longest_ns {0};
	ThreadState*          next = nullptr;
};

std::atomic<ThreadState*> g_threads {nullptr};

struct ThreadHandle {
	ThreadState* state = nullptr;
	~ThreadHandle() {
		if (state != nullptr) {
			state->alive.store(false, std::memory_order_relaxed);
		}
	}
};

thread_local ThreadHandle g_thread;

struct Anomaly {
	std::string kind;
	std::string key;
	std::string detail;
	std::string backtrace;
	uint64_t    count       = 0;
	uint64_t    first_frame = 0;
	double      first_s     = 0.0;
};

struct State {
	std::filesystem::path                                    dir;
	uint64_t                                                 start_ns      = 0;
	uint64_t                                                 screenshot_ns = 0;
	uint64_t                                                 hang_ns       = 0;
	FILE*                                                    frames        = nullptr;
	uint64_t                                                 last_frame_ns = 0;
	PerfCounters::Snapshot                                   last_counters {};
	std::array<uint64_t, static_cast<size_t>(Column::Count)> last_column_ns {};
	std::array<uint64_t, static_cast<size_t>(Column::Count)> last_column_calls {};
	std::array<uint64_t, static_cast<size_t>(Column::Count)> last_critical_ns {};

	std::mutex                     anomalies_mutex;
	std::map<std::string, Anomaly> anomalies;

	std::mutex              reports_mutex;
	std::thread             writer;
	std::mutex              writer_mutex;
	std::condition_variable writer_wake;
	bool                    writer_stop = false;
};

State*                g_state = nullptr;
std::atomic<uint64_t> g_frame {0};
std::atomic<uint64_t> g_frame_end_ns {0};
std::atomic<uint64_t> g_next_screenshot_ns {0};
std::atomic<bool>     g_fatal {false};

std::array<Site, static_cast<size_t>(Column::Count)>& Columns() {
	static std::array<Site, static_cast<size_t>(Column::Count)> columns = {
	    Site {"column", COLUMN_NAMES[0]}, Site {"column", COLUMN_NAMES[1]},
	    Site {"column", COLUMN_NAMES[2]}, Site {"column", COLUMN_NAMES[3]},
	    Site {"column", COLUMN_NAMES[4]}, Site {"column", COLUMN_NAMES[5]},
	    Site {"column", COLUMN_NAMES[6]}, Site {"column", COLUMN_NAMES[7]},
	    Site {"column", COLUMN_NAMES[8]}, Site {"column", COLUMN_NAMES[9]}};
	return columns;
}

// The time of each column on the critical thread (SetCriticalThread).
std::array<std::atomic<uint64_t>, static_cast<size_t>(Column::Count)> g_critical_ns {};
thread_local bool                                                     g_critical = false;

ThreadState* CurrentThread() {
	if (g_thread.state != nullptr) {
		return g_thread.state;
	}
	auto* state     = new ThreadState;
	state->first_ns = NowNs();
#if KYTY_PLATFORM == KYTY_PLATFORM_LINUX && !defined(__APPLE__)
	state->tid = static_cast<int>(gettid());
#endif
#if KYTY_PLATFORM != KYTY_PLATFORM_WINDOWS
	pthread_getname_np(pthread_self(), state->name, sizeof(state->name));
#endif
	auto* head = g_threads.load(std::memory_order_relaxed);
	do {
		state->next = head;
	} while (!g_threads.compare_exchange_weak(head, state, std::memory_order_release,
	                                          std::memory_order_relaxed));
	g_thread.state = state;
	return state;
}

std::string Backtrace(int skip) {
	std::string text;
#if KYTY_PLATFORM != KYTY_PLATFORM_WINDOWS
	std::array<void*, MAX_BACKTRACE> frames {};
	const int                        count   = backtrace(frames.data(), MAX_BACKTRACE);
	char**                           symbols = backtrace_symbols(frames.data(), count);
	for (int i = skip; i < count; i++) {
		text += "    ";
		text += symbols != nullptr ? symbols[i] : "?";
		text += '\n';
	}
	free(symbols); // NOLINT(cppcoreguidelines-no-malloc,hicpp-no-malloc)
#else
	(void)skip;
#endif
	return text;
}

// CSV fields are written quoted, with quotes doubled and line breaks as " | ".
std::string Quote(std::string_view text) {
	std::string quoted = "\"";
	for (const char c: text) {
		if (c == '"') {
			quoted += "\"\"";
		} else if (c == '\n' || c == '\r') {
			if (!quoted.ends_with(" | ") && quoted.size() > 1) {
				quoted += " | ";
			}
		} else {
			quoted += c;
		}
	}
	while (quoted.ends_with(" | ")) {
		quoted.resize(quoted.size() - 3);
	}
	quoted += '"';
	return quoted;
}

// Written whole to a temporary file and renamed, so a reader never sees half a report.
void WriteFile(const std::filesystem::path& path, const std::string& text) {
	auto temporary = path;
	temporary += ".tmp";
	FILE* file = std::fopen(temporary.string().c_str(), "wb");
	if (file == nullptr) {
		return;
	}
	std::fwrite(text.data(), 1, text.size(), file);
	std::fclose(file);
	std::error_code error;
	std::filesystem::rename(temporary, path, error);
}

// The last sites each thread entered, for crash.txt.
std::string RecentText(int per_thread) {
	std::string text;
	for (auto* thread = g_threads.load(std::memory_order_acquire); thread != nullptr;
	     thread       = thread->next) {
		const auto count = thread->recent_count.load(std::memory_order_relaxed);
		if (!thread->alive.load(std::memory_order_relaxed) || count == 0) {
			continue;
		}
		const bool current = thread == g_thread.state;
		const int  shown   = std::min<int>(current ? RECENT : per_thread,
		                                   static_cast<int>(std::min<uint32_t>(count, RECENT)));
		text += "tid " + std::to_string(thread->tid) + " [" + thread->name + "]" +
		        (current ? " (this thread)" : "") + ", oldest first:";
		for (int i = shown; i > 0; i--) {
			const auto* site = thread->recent[(count - i) % RECENT].load(std::memory_order_relaxed);
			text += " ";
			text += site != nullptr ? site->name : "?";
		}
		text += '\n';
	}
	return text;
}

// Each guest thread's share of time outside timed sites: roughly how busy it is.
std::string BusyText(uint64_t now) {
	std::string text = "tid,name,lifetime_s,in_sites_s,busy_pct,last_long_site\n";
	char        line[256];
	for (auto* thread = g_threads.load(std::memory_order_acquire); thread != nullptr;
	     thread       = thread->next) {
		if (!thread->alive.load(std::memory_order_relaxed)) {
			continue;
		}
		const auto  life = static_cast<double>(now - thread->first_ns);
		const auto  in   = static_cast<double>(thread->in_sites_ns.load(std::memory_order_relaxed));
		const auto* site = thread->longest_site.load(std::memory_order_relaxed);
		std::snprintf(line, sizeof(line), "%d,%s,%.3f,%.3f,%.1f,%s::%s\n", thread->tid,
		              thread->name, life / 1e9, in / 1e9,
		              life > 0 ? 100.0 * (1.0 - in / life) : 0.0,
		              site != nullptr ? site->group : "-", site != nullptr ? site->name : "-");
		text += line;
	}
	return text;
}

std::string ThreadsText(uint64_t now) {
	std::string text;
	char        line[512];
	for (auto* thread = g_threads.load(std::memory_order_acquire); thread != nullptr;
	     thread       = thread->next) {
		if (!thread->alive.load(std::memory_order_relaxed)) {
			continue;
		}
		const int depth = std::min(thread->depth.load(std::memory_order_relaxed), MAX_DEPTH);
		std::snprintf(line, sizeof(line), "tid %d [%s]:", thread->tid, thread->name);
		text += line;
		if (depth == 0) {
			text += " (no timed site)\n";
			continue;
		}
		for (int i = 0; i < depth; i++) {
			const auto* site  = thread->stack[i].load(std::memory_order_relaxed);
			const auto  since = thread->since[i].load(std::memory_order_relaxed);
			std::snprintf(line, sizeof(line), " > %s::%s (%.3f s)",
			              site != nullptr ? site->group : "?", site != nullptr ? site->name : "?",
			              static_cast<double>(now - std::min(now, since)) / 1e9);
			text += line;
		}
		text += '\n';
	}
	return text;
}

std::string KeyedText();

void WriteReports(State& state) {
	const auto  now      = NowNs();
	std::string hotspots = "group,name,calls,total_ms,max_ms\n";
	char        line[512];
	for (auto* site = g_sites.load(std::memory_order_acquire); site != nullptr; site = site->next) {
		const auto calls = site->calls.load(std::memory_order_relaxed);
		if (calls == 0) {
			continue;
		}
		std::snprintf(line, sizeof(line), "%s,%s,%" PRIu64 ",%.3f,%.3f\n", site->group, site->name,
		              calls, static_cast<double>(site->nanoseconds.load()) / 1e6,
		              static_cast<double>(site->max_nanoseconds.load()) / 1e6);
		hotspots += line;
	}
	WriteFile(state.dir / "hotspots.csv", hotspots);

	std::string anomalies = "kind,key,count,first_frame,first_s,detail,backtrace\n";
	{
		std::lock_guard lock(state.anomalies_mutex);
		for (const auto& [id, anomaly]: state.anomalies) {
			std::snprintf(line, sizeof(line), ",%" PRIu64 ",%" PRIu64 ",%.3f,", anomaly.count,
			              anomaly.first_frame, anomaly.first_s);
			anomalies += Quote(anomaly.kind) + "," + Quote(anomaly.key) + line +
			             Quote(anomaly.detail) + "," + Quote(anomaly.backtrace) + "\n";
		}
	}
	WriteFile(state.dir / "anomalies.csv", anomalies);
	WriteFile(state.dir / "threads.txt", ThreadsText(now));
	WriteFile(state.dir / "keyed.csv", KeyedText());
	WriteFile(state.dir / "busy.csv", BusyText(now));
	std::snprintf(line, sizeof(line), "frames %" PRIu64 "\nseconds %.3f\n",
	              g_frame.load(std::memory_order_relaxed),
	              static_cast<double>(now - state.start_ns) / 1e9);
	WriteFile(state.dir / "status.txt", line);
	if (state.frames != nullptr) {
		std::fflush(state.frames);
	}
}

void WriterLoop(State& state) {
	bool             hung             = false;
	uint64_t         last_frame       = 0;
	uint64_t         last_progress_ns = NowNs();
	std::unique_lock lock(state.writer_mutex);
	while (!state.writer_wake.wait_for(lock, std::chrono::nanoseconds(REPORT_PERIOD_NS),
	                                   [&state] { return state.writer_stop; })) {
		lock.unlock();
		{
			std::lock_guard reports(state.reports_mutex);
			WriteReports(state);
		}
		const auto now   = NowNs();
		const auto frame = g_frame.load(std::memory_order_relaxed);
		if (frame != last_frame) {
			last_frame       = frame;
			last_progress_ns = now;
			hung             = false;
		}
		const auto limit = frame == 0 ? FIRST_FRAME_GRACE_NS : state.hang_ns;
		if (!hung && now - last_progress_ns > limit) {
			hung = true;
			char head[128];
			std::snprintf(head, sizeof(head), "no frame for %.1f s after frame %" PRIu64 "\n\n",
			              static_cast<double>(now - last_progress_ns) / 1e9, frame);
			WriteFile(state.dir / "hang.txt", head + ThreadsText(now));
		}
		lock.lock();
	}
}

#if KYTY_PLATFORM != KYTY_PLATFORM_WINDOWS
void FatalSignal(int signal_number) {
	char reason[64];
	std::snprintf(reason, sizeof(reason), "host signal %d", signal_number);
	Fatal(reason);
	std::signal(signal_number, SIG_DFL);
	std::raise(signal_number);
}
#endif

} // namespace

Site::Site(const char* group, const char* name) noexcept: group(group), name(name) {
	auto* head = g_sites.load(std::memory_order_relaxed);
	do {
		next = head;
	} while (!g_sites.compare_exchange_weak(head, this, std::memory_order_release,
	                                        std::memory_order_relaxed));
}

Site& ColumnSite(Column column) {
	return Columns()[static_cast<size_t>(column)];
}

void SetCriticalThread() noexcept {
	g_critical = true;
}

namespace {
// KYTY_TRACE_TIMELINE: every timed span of 50 us or more, per thread, for timeline.csv.
struct Span {
	int         tid   = 0;
	const Site* site  = nullptr;
	uint64_t    begin = 0;
	uint64_t    end   = 0;
	uint64_t    frame = 0;
};
constexpr size_t      TIMELINE_SPANS = 1u << 21;
std::vector<Span>     g_spans;
std::atomic<uint64_t> g_spans_next {0};
bool                  g_timeline         = false;
uint64_t              g_timeline_from_ns = 0;

void RecordSpan(const Site& site, uint64_t nanoseconds) {
	const auto end = NowNs();
	if (end < g_timeline_from_ns ||
	    g_spans_next.load(std::memory_order_relaxed) >= TIMELINE_SPANS) {
		return;
	}
	const auto index = g_spans_next.fetch_add(1, std::memory_order_relaxed);
	if (index >= TIMELINE_SPANS) {
		return;
	}
	auto& span = g_spans[index];
	span = {g_thread.state != nullptr ? g_thread.state->tid : 0, &site, end - nanoseconds, end,
	        g_frame.load(std::memory_order_relaxed)};
}

void WriteTimeline(State& state) {
	if (!g_timeline) {
		return;
	}
	FILE* file = std::fopen((state.dir / "timeline.csv").string().c_str(), "wb");
	if (file == nullptr) {
		return;
	}
	std::fprintf(file, "tid,thread,group,name,begin_us,end_us,frame\n");
	std::map<int, std::string> names;
	for (auto* thread = g_threads.load(std::memory_order_acquire); thread != nullptr;
	     thread       = thread->next) {
		names[thread->tid] = thread->name;
	}
	const auto count = std::min<uint64_t>(g_spans_next.load(), TIMELINE_SPANS);
	for (uint64_t i = 0; i < count; i++) {
		const auto& span = g_spans[i];
		if (span.site == nullptr) {
			continue;
		}
		std::fprintf(file, "%d,%s,%s,%s,%.1f,%.1f,%" PRIu64 "\n", span.tid, names[span.tid].c_str(),
		             span.site->group, span.site->name,
		             static_cast<double>(span.begin - state.start_ns) / 1e3,
		             static_cast<double>(span.end - state.start_ns) / 1e3, span.frame);
	}
	std::fclose(file);
}
} // namespace

void Enter(Site& site) noexcept {
	auto*     thread = CurrentThread();
	const int depth  = thread->depth.load(std::memory_order_relaxed);
	if (depth < MAX_DEPTH) {
		thread->stack[depth].store(&site, std::memory_order_relaxed);
		thread->since[depth].store(NowNs(), std::memory_order_relaxed);
	}
	thread->depth.store(depth + 1, std::memory_order_relaxed);
	const auto count = thread->recent_count.load(std::memory_order_relaxed);
	thread->recent[count % RECENT].store(&site, std::memory_order_relaxed);
	thread->recent_count.store(count + 1, std::memory_order_relaxed);
}

void Leave(Site& site, uint64_t nanoseconds) noexcept {
	auto* thread = g_thread.state;
	if (thread != nullptr) {
		if (thread->depth.fetch_sub(1, std::memory_order_relaxed) == 1) {
			thread->in_sites_ns.fetch_add(nanoseconds, std::memory_order_relaxed);
			if (nanoseconds > thread->longest_ns.load(std::memory_order_relaxed) / 2) {
				thread->longest_ns.store(nanoseconds, std::memory_order_relaxed);
				thread->longest_site.store(&site, std::memory_order_relaxed);
			}
		}
	}
	if (g_timeline && nanoseconds >= 50'000) {
		RecordSpan(site, nanoseconds);
	}
	site.calls.fetch_add(1, std::memory_order_relaxed);
	site.nanoseconds.fetch_add(nanoseconds, std::memory_order_relaxed);
	if (g_critical) {
		auto&      columns = Columns();
		const auto index   = &site - columns.data();
		if (index >= 0 && index < static_cast<ptrdiff_t>(columns.size())) {
			g_critical_ns[static_cast<size_t>(index)].fetch_add(nanoseconds,
			                                                    std::memory_order_relaxed);
		}
	}
	auto longest = site.max_nanoseconds.load(std::memory_order_relaxed);
	while (nanoseconds > longest && !site.max_nanoseconds.compare_exchange_weak(
	                                    longest, nanoseconds, std::memory_order_relaxed)) {
	}
}

void Initialize() {
	const auto dir = Config::GetTraceDir();
	if (dir.empty()) {
		return;
	}
	std::error_code error;
	std::filesystem::create_directories(dir / "shots", error);
	auto* state     = new State;
	state->dir      = dir;
	state->start_ns = NowNs();
	state->screenshot_ns =
	    static_cast<uint64_t>(Config::GetTraceScreenshotSeconds()) * 1'000'000'000;
	// KYTY_TRACE_SCREENSHOT_MS=<ms> replaces the seconds: to catch a glitch of a frame or two.
	if (const char* ms = std::getenv("KYTY_TRACE_SCREENSHOT_MS"); ms != nullptr) {
		state->screenshot_ns = static_cast<uint64_t>(std::strtoull(ms, nullptr, 10)) * 1'000'000;
	}
	state->hang_ns = static_cast<uint64_t>(Config::GetTraceHangSeconds()) * 1'000'000'000;
	state->frames  = std::fopen((dir / "frames.csv").string().c_str(), "wb");
	if (state->frames != nullptr) {
		std::fprintf(state->frames, "frame,time_s,frame_ms");
		for (size_t i = 0; i < PerfCounters::CounterCount; i++) {
			std::fprintf(state->frames, ",%s",
			             PerfCounters::NameOf(static_cast<PerfCounters::Counter>(i)));
		}
		for (const auto* name: COLUMN_NAMES) {
			std::fprintf(state->frames, ",%s_ms,%s_n,%s_crit_ms", name, name, name);
		}
		for (size_t i = 0; i < PerfCounters::GaugeCount; i++) {
			std::fprintf(state->frames, ",%s",
			             PerfCounters::NameOf(static_cast<PerfCounters::Gauge>(i)));
		}
		std::fprintf(state->frames, "\n");
	}
	// The counters of each frame row.
	PerfCounters::SetEnabled(true);
	// KYTY_TRACE_TIMELINE=<seconds>: spans from that time on, until the buffer is full.
	if (const char* from = std::getenv("KYTY_TRACE_TIMELINE"); from != nullptr) {
		g_spans.resize(TIMELINE_SPANS);
		g_timeline_from_ns =
		    state->start_ns + static_cast<uint64_t>(std::strtod(from, nullptr) * 1e9);
		g_timeline = true;
	}
	Detail::g_prof_enabled.store(std::getenv("KYTY_TRACE_NO_PROF") == nullptr,
	                             std::memory_order_relaxed);
	Columns();
	g_next_screenshot_ns.store(state->start_ns + state->screenshot_ns, std::memory_order_relaxed);
	g_state = state;
	Detail::g_enabled.store(true, std::memory_order_release);
#if KYTY_PLATFORM != KYTY_PLATFORM_WINDOWS
	std::signal(SIGABRT, FatalSignal);
	std::signal(SIGFPE, FatalSignal);
#endif
	state->writer = std::thread([state] {
#if defined(__APPLE__)
		pthread_setname_np("TraceWriter");
#elif KYTY_PLATFORM == KYTY_PLATFORM_LINUX
		pthread_setname_np(pthread_self(), "TraceWriter");
#endif
		WriterLoop(*state);
	});
	std::printf("Trace: writing to %s\n", dir.string().c_str());
}

void Shutdown() {
	auto* state = g_state;
	if (state == nullptr) {
		return;
	}
	{
		std::lock_guard lock(state->writer_mutex);
		state->writer_stop = true;
	}
	state->writer_wake.notify_all();
	if (state->writer.joinable() && state->writer.get_id() != std::this_thread::get_id()) {
		state->writer.join();
	}
	{
		std::lock_guard lock(state->reports_mutex);
		WriteReports(*state);
		WriteTimeline(*state);
		if (state->frames != nullptr) {
			std::fclose(state->frames);
			state->frames = nullptr;
		}
	}
	Detail::g_enabled.store(false, std::memory_order_release);
	Detail::g_prof_enabled.store(false, std::memory_order_relaxed);
	// The state stays: a timed site may still be running on another thread.
}

void Note(std::string_view kind, std::string_view key, std::string_view detail) {
	auto* state = g_state;
	if (state == nullptr || !Enabled()) {
		return;
	}
	std::string id {kind};
	id += '\x1f';
	id += key;
	{
		std::lock_guard lock(state->anomalies_mutex);
		auto [it, added] = state->anomalies.try_emplace(id);
		auto& anomaly    = it->second;
		anomaly.count++;
		if (!added) {
			return;
		}
		anomaly.kind        = kind;
		anomaly.key         = key;
		anomaly.detail      = detail;
		anomaly.first_frame = g_frame.load(std::memory_order_relaxed);
		anomaly.first_s     = static_cast<double>(NowNs() - state->start_ns) / 1e9;
	}
	auto            backtrace = Backtrace(2);
	std::lock_guard lock(state->anomalies_mutex);
	state->anomalies[id].backtrace = std::move(backtrace);
}

namespace {
struct Keyed {
	uint64_t calls = 0;
	uint64_t value = 0;
};
std::mutex                                                 g_keyed_mutex;
std::map<std::string, std::unordered_map<uint64_t, Keyed>> g_keyed;

std::string KeyedText() {
	std::string     text = "table,key,calls,value\n";
	char            line[160];
	std::lock_guard lock(g_keyed_mutex);
	for (const auto& [table, keys]: g_keyed) {
		std::vector<std::pair<uint64_t, Keyed>> sorted(keys.begin(), keys.end());
		std::sort(sorted.begin(), sorted.end(),
		          [](const auto& a, const auto& b) { return a.second.value > b.second.value; });
		for (size_t i = 0; i < std::min<size_t>(sorted.size(), 40); i++) {
			std::snprintf(line, sizeof(line), "%s,0x%016" PRIx64 ",%" PRIu64 ",%" PRIu64 "\n",
			              table.c_str(), sorted[i].first, sorted[i].second.calls,
			              sorted[i].second.value);
			text += line;
		}
	}
	return text;
}
} // namespace

void AddKeyed(const char* table, uint64_t key, uint64_t value) {
	if (!ProfEnabled()) {
		return;
	}
	std::lock_guard lock(g_keyed_mutex);
	auto&           entry = g_keyed[table][key];
	entry.calls++;
	entry.value += value;
}

StackTimedScope::~StackTimedScope() {
	if (!m_on) {
		return;
	}
	const auto took = NowNs() - m_begin;
	uint64_t   key  = 1469598103934665603ull;
#if KYTY_PLATFORM != KYTY_PLATFORM_WINDOWS
	std::array<void*, 10> frames {};
	const int             count = backtrace(frames.data(), static_cast<int>(frames.size()));
	for (int i = 1; i < count; i++) {
		key = (key ^ reinterpret_cast<uint64_t>(frames[i])) * 1099511628211ull;
	}
#endif
	AddKeyed("wait_stack", key, took);
	char text[32];
	std::snprintf(text, sizeof(text), "0x%016" PRIx64, key);
	Note("wait_stack", text);
}

void NoteSlow(const char* kind, uint64_t key, uint64_t nanoseconds) {
	char text[64];
	std::snprintf(text, sizeof(text), "0x%016" PRIx64, key);
	char detail[64];
	std::snprintf(detail, sizeof(detail), "%.1f ms", static_cast<double>(nanoseconds) / 1e6);
	Note(kind, text, detail);
}

void EndFrame() {
	auto* state = g_state;
	if (state == nullptr || !Enabled()) {
		return;
	}
	const auto now   = NowNs();
	const auto frame = g_frame.fetch_add(1, std::memory_order_relaxed) + 1;
	g_frame_end_ns.store(now, std::memory_order_relaxed);
	const auto counters = PerfCounters::Read();
	if (state->last_frame_ns != 0 && state->frames != nullptr) {
		std::fprintf(state->frames, "%" PRIu64 ",%.4f,%.3f", frame,
		             static_cast<double>(now - state->start_ns) / 1e9,
		             static_cast<double>(now - state->last_frame_ns) / 1e6);
		for (size_t i = 0; i < PerfCounters::CounterCount; i++) {
			std::fprintf(state->frames, ",%" PRIu64, counters[i] - state->last_counters[i]);
		}
		for (size_t i = 0; i < static_cast<size_t>(Column::Count); i++) {
			const auto& site  = Columns()[i];
			const auto  ns    = site.nanoseconds.load(std::memory_order_relaxed);
			const auto  calls = site.calls.load(std::memory_order_relaxed);
			const auto  crit  = g_critical_ns[i].load(std::memory_order_relaxed);
			std::fprintf(state->frames, ",%.3f,%" PRIu64 ",%.3f",
			             static_cast<double>(ns - state->last_column_ns[i]) / 1e6,
			             calls - state->last_column_calls[i],
			             static_cast<double>(crit - state->last_critical_ns[i]) / 1e6);
		}
		const auto gauges = PerfCounters::ReadGauges();
		for (const auto gauge: gauges) {
			std::fprintf(state->frames, ",%lld", static_cast<long long>(gauge));
		}
		std::fprintf(state->frames, "\n");
	}
	for (size_t i = 0; i < static_cast<size_t>(Column::Count); i++) {
		state->last_column_ns[i]    = Columns()[i].nanoseconds.load(std::memory_order_relaxed);
		state->last_column_calls[i] = Columns()[i].calls.load(std::memory_order_relaxed);
		state->last_critical_ns[i]  = g_critical_ns[i].load(std::memory_order_relaxed);
	}
	state->last_counters = counters;
	state->last_frame_ns = now;
}

bool ScreenshotDue() {
	auto* state = g_state;
	if (state == nullptr || !Enabled() || state->screenshot_ns == 0) {
		return false;
	}
	const auto now  = NowNs();
	auto       next = g_next_screenshot_ns.load(std::memory_order_relaxed);
	return now >= next && g_next_screenshot_ns.compare_exchange_strong(
	                          next, now + state->screenshot_ns, std::memory_order_relaxed);
}

void SaveScreenshot(const uint8_t* rgba, uint32_t width, uint32_t height) {
	auto* state = g_state;
	if (state == nullptr) {
		return;
	}
	std::vector<uint8_t> pixels(rgba, rgba + static_cast<size_t>(width) * height * 4u);
	char                 name[64];
	std::snprintf(name, sizeof(name), "frame_%07" PRIu64 ".png",
	              g_frame.load(std::memory_order_relaxed));
	auto path = state->dir / "shots" / name;
	std::thread([pixels = std::move(pixels), path = std::move(path), width, height] {
		stbi_write_png(path.string().c_str(), static_cast<int>(width), static_cast<int>(height), 4,
		               pixels.data(), static_cast<int>(width * 4u));
	}).detach();
}

void Fatal(std::string_view reason) noexcept {
	auto* state = g_state;
	if (state == nullptr || g_fatal.exchange(true)) {
		return;
	}
	const auto  now  = NowNs();
	std::string text = "frame " + std::to_string(g_frame.load(std::memory_order_relaxed)) + "\n\n" +
	                   std::string(reason) + "\n\nBacktrace:\n" + Backtrace(1) + "\nThreads:\n" +
	                   ThreadsText(now) + "\nRecent timed sites:\n" + RecentText(8);
	WriteFile(state->dir / "crash.txt", text);
	// Another thread may be writing the reports, or this thread may hold the lock.
	if (state->reports_mutex.try_lock()) {
		WriteReports(*state);
		state->reports_mutex.unlock();
	}
}

} // namespace Trace
