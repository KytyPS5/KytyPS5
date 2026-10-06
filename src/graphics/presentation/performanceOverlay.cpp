#include "graphics/presentation/performanceOverlay.h"

#include "common/debugCounters.h"
#include "common/emulatorConfig.h"
#include "common/hostStats.h"
#include "graphics/host_gpu/graphicContext.h"
#include "imgui.h"
#include "kernel/memory.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>
#include <vector>

namespace Libs::Graphics {

namespace {

using Clock   = std::chrono::steady_clock;
using Counter = Common::DebugCounters::Counter;
using Gauge   = Common::DebugCounters::Gauge;

constexpr auto   REFRESH_INTERVAL  = std::chrono::seconds(1);
constexpr size_t FRAME_HISTORY     = 120;
constexpr float  FRAME_GRAPH_MS    = 50.0f;
constexpr float  FRAME_TIME_GOOD   = 17.5f;
constexpr float  FRAME_TIME_MEDIUM = 34.0f;
constexpr size_t COUNTER_COUNT     = static_cast<size_t>(Counter::Count);

// Tables as wide as their columns; the default fills the window, which an auto-resizing
// window then never shrinks.
constexpr ImGuiTableFlags TABLE_FLAGS =
    ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoHostExtendX;

constexpr ImU32 GOOD    = IM_COL32(96, 220, 120, 255);
constexpr ImU32 MEDIUM  = IM_COL32(230, 195, 60, 255);
constexpr ImU32 BAD     = IM_COL32(235, 80, 70, 255);
constexpr ImU32 NEUTRAL = IM_COL32(160, 160, 160, 255);

struct FrameWindow {
	std::array<float, FRAME_HISTORY> intervals_ms {};
	size_t                           next         = 0;
	size_t                           count        = 0;
	uint32_t                         flips        = 0;
	float                            max_ms       = 0.0f;
	uint32_t                         guest_width  = 0;
	uint32_t                         guest_height = 0;
	Clock::time_point                last_flip    = {};
	Clock::time_point                window_start = Clock::now();
};

// Shown values, recomputed once per refresh interval.
struct Snapshot {
	double                                   fps          = 0.0;
	double                                   frame_ms     = 0.0;
	double                                   max_frame_ms = 0.0;
	double                                   low_fps      = 0.0; // From the 99th percentile.
	double                                   stalled_s    = 0.0;
	uint32_t                                 guest_width  = 0;
	uint32_t                                 guest_height = 0;
	std::array<double, COUNTER_COUNT>        rates {};
	std::array<uint64_t, COUNTER_COUNT>      totals {};
	Common::HostStats                        host;
	std::optional<uint64_t>                  device_memory;
	uint64_t                                 device_budget = 0;
	std::vector<GraphicContext::HeapUsage>   heaps;
	LibKernel::Memory::DebugStats            guest_memory;
};

std::mutex            g_frame_mutex;
FrameWindow           g_frames;
std::once_flag        g_enabled_once;
std::atomic<bool>     g_enabled {false};
std::atomic<bool>     g_details {false};
std::atomic<uint64_t> g_revision {0};

std::mutex        g_host_mutex;
Common::HostStats g_host_stats;
std::jthread      g_sampler;

void EnsureEnabledLoaded() {
	std::call_once(g_enabled_once, [] {
		g_enabled.store(Config::PerfOverlayEnabled(), std::memory_order_release);
	});
}

bool GuestStalled(Clock::time_point now) {
	std::scoped_lock lock(g_frame_mutex);
	return now - g_frames.last_flip >= REFRESH_INTERVAL;
}

// Host statistics can block for milliseconds (IOKit, thread enumeration), so sample them off
// the present path. While the guest stops flipping (loading screens), ask for one repaint per
// interval.
void SamplerThread(const std::stop_token& stop) {
	Common::HostStatsSampler    sampler;
	std::mutex                  wait_mutex;
	std::condition_variable_any wait;
	while (!stop.stop_requested()) {
		auto stats = sampler.Sample();
		{
			std::scoped_lock lock(g_host_mutex);
			g_host_stats = std::move(stats);
		}
		if (g_enabled.load(std::memory_order_acquire) && GuestStalled(Clock::now())) {
			g_revision.fetch_add(1, std::memory_order_acq_rel);
		}
		std::unique_lock lock(wait_mutex);
		wait.wait_for(lock, stop, REFRESH_INTERVAL, [] { return false; });
	}
}

void EnsureSampler() {
	static std::once_flag once;
	std::call_once(once, [] { g_sampler = std::jthread(SamplerThread); });
}

ImU32 FrameTimeColor(double ms) {
	return ms <= FRAME_TIME_GOOD ? GOOD : ms <= FRAME_TIME_MEDIUM ? MEDIUM : BAD;
}

// Green below `warn`, yellow below `bad`, red above.
ImU32 LevelColor(double value, double warn, double bad) {
	return value < warn ? GOOD : value < bad ? MEDIUM : BAD;
}

ImVec4 Color(ImU32 color) {
	return ImGui::ColorConvertU32ToFloat4(color);
}

std::string Bytes(uint64_t bytes) {
	constexpr double GIB = 1024.0 * 1024.0 * 1024.0;
	constexpr double MIB = 1024.0 * 1024.0;
	char             buffer[32];
	if (static_cast<double>(bytes) >= GIB) {
		std::snprintf(buffer, sizeof(buffer), "%.2f GB", static_cast<double>(bytes) / GIB);
	} else {
		std::snprintf(buffer, sizeof(buffer), "%.0f MB", static_cast<double>(bytes) / MIB);
	}
	return buffer;
}

std::string Bytes(std::optional<uint64_t> bytes) {
	return bytes ? Bytes(*bytes) : "n/a";
}

std::string Throughput(double bytes_per_second) {
	char buffer[32];
	std::snprintf(buffer, sizeof(buffer), "%.1f MB/s", bytes_per_second / (1024.0 * 1024.0));
	return buffer;
}

std::string Percent(std::optional<double> value) {
	if (!value) {
		return "n/a";
	}
	char buffer[16];
	std::snprintf(buffer, sizeof(buffer), "%.0f%%", *value);
	return buffer;
}

void Row(const char* label, const std::string& value, std::optional<ImU32> color = std::nullopt) {
	ImGui::TableNextRow();
	ImGui::TableNextColumn();
	ImGui::TextDisabled("%s", label);
	ImGui::TableNextColumn();
	if (color) {
		ImGui::TextColored(Color(*color), "%s", value.c_str());
	} else {
		ImGui::TextUnformatted(value.c_str());
	}
}

[[gnu::format(printf, 1, 2)]] std::string Fmt(const char* format, ...) {
	char    buffer[128];
	va_list args;
	va_start(args, format);
	std::vsnprintf(buffer, sizeof(buffer), format, args);
	va_end(args);
	return buffer;
}

bool BeginSection(const char* title) {
	ImGui::SeparatorText(title);
	return ImGui::BeginTable(title, 2, TABLE_FLAGS);
}

void DrawFrameGraph(const std::array<float, FRAME_HISTORY>& history, size_t count, float width,
                    float height) {
	const ImVec2 origin = ImGui::GetCursorScreenPos();
	auto*        draw   = ImGui::GetWindowDrawList();
	draw->AddRectFilled(origin, {origin.x + width, origin.y + height}, IM_COL32(0, 0, 0, 90));
	const auto y_of = [&](float ms) {
		return origin.y + height - std::min(ms, FRAME_GRAPH_MS) / FRAME_GRAPH_MS * height;
	};
	for (const float reference: {1000.0f / 60.0f, 1000.0f / 30.0f}) {
		draw->AddLine({origin.x, y_of(reference)}, {origin.x + width, y_of(reference)},
		              IM_COL32(255, 255, 255, 45));
	}
	const float bar = width / static_cast<float>(FRAME_HISTORY);
	for (size_t i = 0; i < count; i++) {
		const float ms = history[i];
		const float x  = origin.x + width - static_cast<float>(count - i) * bar;
		draw->AddRectFilled({x, y_of(ms)}, {x + std::max(bar - 1.0f, 1.0f), origin.y + height},
		                    FrameTimeColor(ms));
	}
	ImGui::Dummy({width, height});
}

const char* PressureName(Common::MemoryPressure pressure) {
	switch (pressure) {
		case Common::MemoryPressure::Normal: return "Normal";
		case Common::MemoryPressure::Warning: return "Warning";
		case Common::MemoryPressure::Critical: return "Critical";
		case Common::MemoryPressure::Unknown: break;
	}
	return "n/a";
}

ImU32 PressureColor(Common::MemoryPressure pressure) {
	switch (pressure) {
		case Common::MemoryPressure::Normal: return GOOD;
		case Common::MemoryPressure::Warning: return MEDIUM;
		case Common::MemoryPressure::Critical: return BAD;
		case Common::MemoryPressure::Unknown: break;
	}
	return NEUTRAL;
}

const char* ThermalName(Common::ThermalPressure thermal) {
	switch (thermal) {
		case Common::ThermalPressure::Nominal: return "Nominal";
		case Common::ThermalPressure::Moderate: return "Moderate";
		case Common::ThermalPressure::Heavy: return "Heavy (throttling)";
		case Common::ThermalPressure::Critical: return "Critical (throttling)";
		case Common::ThermalPressure::Unknown: break;
	}
	return "n/a";
}

ImU32 ThermalColor(Common::ThermalPressure thermal) {
	switch (thermal) {
		case Common::ThermalPressure::Nominal: return GOOD;
		case Common::ThermalPressure::Moderate: return MEDIUM;
		case Common::ThermalPressure::Heavy:
		case Common::ThermalPressure::Critical: return BAD;
		case Common::ThermalPressure::Unknown: break;
	}
	return NEUTRAL;
}

const char* PresentModeName(Config::PresentMode mode) {
	switch (mode) {
		case Config::PresentMode::Fifo: return "FIFO";
		case Config::PresentMode::Mailbox: return "Mailbox";
		case Config::PresentMode::Immediate: return "Immediate";
	}
	return "?";
}

double Rate(const Snapshot& shown, Counter counter) {
	return shown.rates[static_cast<size_t>(counter)];
}

uint64_t Total(const Snapshot& shown, Counter counter) {
	return shown.totals[static_cast<size_t>(counter)];
}

void Refresh(Snapshot& shown, const GraphicContext& graphics, Clock::time_point now) {
	static std::array<uint64_t, COUNTER_COUNT> previous {};
	static Clock::time_point                   previous_time;
	const double seconds = previous_time == Clock::time_point {}
	                           ? 0.0
	                           : std::chrono::duration<double>(now - previous_time).count();
	for (size_t i = 0; i < COUNTER_COUNT; i++) {
		const auto value = Common::DebugCounters::Get(static_cast<Counter>(i));
		shown.rates[i] =
		    seconds > 0.0 ? static_cast<double>(value - previous[i]) / seconds : 0.0;
		shown.totals[i] = value;
		previous[i]     = value;
	}
	previous_time = now;

	{
		std::scoped_lock lock(g_host_mutex);
		shown.host = g_host_stats;
	}
	shown.device_memory = graphics.CanReportMemoryUsage()
	                          ? std::optional(graphics.GetDeviceMemoryUsage())
	                          : std::nullopt;
	shown.device_budget = graphics.GetTotalMemoryBudget();
	if (g_details.load(std::memory_order_acquire)) {
		shown.heaps        = graphics.GetHeapUsage();
		shown.guest_memory = LibKernel::Memory::GetDebugStats();
	}
}

void DrawCompact(const Snapshot& shown) {
	if (!ImGui::BeginTable("##performance", 2, TABLE_FLAGS)) {
		return;
	}
	const auto& host = shown.host;
	if (shown.max_frame_ms > 0.0) {
		Row("Worst frame", Fmt("%.1f ms", shown.max_frame_ms),
		    FrameTimeColor(shown.max_frame_ms));
	}
	Row("CPU", host.process_cpu_percent
	               ? Fmt("%.0f%%  of %u threads", *host.process_cpu_percent,
	                          host.logical_cores)
	               : std::string("n/a"));
	Row("GPU", Percent(host.gpu_percent));
	if (shown.device_memory) {
		Row("GPU memory", Bytes(*shown.device_memory) + " / " + Bytes(shown.device_budget));
	}
	Row("Emulator RAM", Bytes(host.process_memory));
	if (host.system_memory_used && host.system_memory_total) {
		Row("System RAM", Bytes(*host.system_memory_used) + " / " + Bytes(*host.system_memory_total));
	}
	if (host.swap_used) {
		Row("Swap", Bytes(*host.swap_used));
	}
	Row("Memory pressure", PressureName(host.memory_pressure), PressureColor(host.memory_pressure));
	ImGui::EndTable();
}

void DrawFramesAndCpu(const Snapshot& shown, vk::Extent2D extent) {
	const auto& host = shown.host;
	if (BeginSection("Frames")) {
		if (shown.low_fps > 0.0) {
			Row("1% low", Fmt("%.1f FPS", shown.low_fps),
			    FrameTimeColor(1000.0 / shown.low_fps));
		}
		Row("Worst frame", Fmt("%.1f ms", shown.max_frame_ms),
		    FrameTimeColor(shown.max_frame_ms));
		Row("Guest output", shown.guest_width != 0
		                        ? Fmt("%ux%u", shown.guest_width, shown.guest_height)
		                        : std::string("n/a"));
		Row("Window", Fmt("%ux%u  %s  %u Hz", extent.width, extent.height,
		                       PresentModeName(Config::GetPresentMode()),
		                       Config::GetVblankFrequency()));
		Row("Flips", Fmt("%.0f/s requested", Rate(shown, Counter::FlipsRequested)));
		const auto rejected = Rate(shown, Counter::FlipsRejected);
		Row("Flip queue full", Fmt("%.0f/s", rejected), rejected > 0.0 ? BAD : GOOD);
		const auto late = Rate(shown, Counter::LateVblanks);
		Row("Late vblanks", Fmt("%.0f/s", late), LevelColor(late, 1.0, 10.0));
		Row("Swapchain rebuilds",
		    Fmt("%llu  (dropped %llu)",
		             static_cast<unsigned long long>(Total(shown, Counter::SwapchainRecreations)),
		             static_cast<unsigned long long>(Total(shown, Counter::DroppedPresents))));
		ImGui::EndTable();
	}
	if (BeginSection("CPU")) {
		Row("Emulator", host.process_cpu_percent
		                    ? Fmt("%.0f%% of %u threads  (%.1f cores)",
		                               *host.process_cpu_percent, host.logical_cores,
		                               *host.process_cpu_percent * host.logical_cores / 100.0)
		                    : std::string("n/a"));
		Row("Machine", Percent(host.system_cpu_percent),
		    host.system_cpu_percent ? std::optional(LevelColor(*host.system_cpu_percent, 70, 90))
		                            : std::nullopt);
		const auto created = Total(shown, Counter::GuestThreadsCreated);
		const auto exited  = Total(shown, Counter::GuestThreadsExited);
		Row("Threads", Fmt("%u host  %llu guest", host.thread_count.value_or(0),
		                        static_cast<unsigned long long>(created - std::min(created, exited))));
		for (const auto& thread: host.busiest_threads) {
			Row(Fmt("  %.24s", thread.name.c_str()).c_str(),
			    Fmt("%.0f%%", thread.cpu_percent), LevelColor(thread.cpu_percent, 70, 95));
		}
		if (host.context_switches_per_s) {
			Row("Context switches", Fmt("%.0f/s", *host.context_switches_per_s));
		}
		if (host.page_faults_per_s) {
			Row("Page faults", Fmt("%.0f/s  (%.0f from disk)", *host.page_faults_per_s,
			                            host.page_ins_per_s.value_or(0.0)));
		}
		Row("Thermal", ThermalName(host.thermal_pressure), ThermalColor(host.thermal_pressure));
		const auto emulated = Rate(shown, Counter::EmulatedInstructions);
		Row("Emulated instrs", Fmt("%.0f/s", emulated), LevelColor(emulated, 1000, 100000));
		const auto unresolved = Rate(shown, Counter::UnresolvedImportCalls);
		Row("Missing HLE calls",
		    Fmt("%.0f/s  (%llu total)", unresolved,
		             static_cast<unsigned long long>(Total(shown, Counter::UnresolvedImportCalls))),
		    unresolved > 0.0 ? MEDIUM : GOOD);
		ImGui::EndTable();
	}
}

void DrawGpu(const Snapshot& shown, const GraphicContext& graphics) {
	const auto& host = shown.host;
	if (BeginSection("GPU")) {
		Row("Device", host.gpu_model.empty()
		                  ? std::string(graphics.GetPhysicalDeviceProperties().deviceName.data())
		                  : host.gpu_cores ? Fmt("%s (%u cores)", host.gpu_model.c_str(),
		                                              *host.gpu_cores)
		                                   : host.gpu_model);
		Row("Utilization", Percent(host.gpu_percent),
		    host.gpu_percent ? std::optional(LevelColor(*host.gpu_percent, 80, 95)) : std::nullopt);
		if (host.gpu_renderer_percent || host.gpu_tiler_percent) {
			Row("Renderer / tiler", Percent(host.gpu_renderer_percent) + " / " +
			                            Percent(host.gpu_tiler_percent));
		}
		if (host.gpu_memory_in_use) {
			Row("Driver memory", Bytes(*host.gpu_memory_in_use) + " in use / " +
			                         Bytes(host.gpu_memory_allocated) + " allocated");
		}
		if (host.gpu_recoveries) {
			Row("GPU resets", Fmt("%llu", static_cast<unsigned long long>(*host.gpu_recoveries)),
			    *host.gpu_recoveries > 0 ? BAD : GOOD);
		}
		const double fps   = std::max(shown.fps, 1.0);
		const auto   draws = Rate(shown, Counter::Draws);
		Row("Draws", Fmt("%.0f/frame  %.0f/s", draws / fps, draws));
		Row("Instances", Fmt("%.0f/frame", Rate(shown, Counter::DrawInstances) / fps));
		Row("Indirect draws", Fmt("%.0f/s", Rate(shown, Counter::IndirectDraws)));
		Row("Dispatches", Fmt("%.0f/frame  %.0f/s", Rate(shown, Counter::Dispatches) / fps,
		                           Rate(shown, Counter::Dispatches)));
		Row("Queue submits", Fmt("%.0f/s", Rate(shown, Counter::QueueSubmits)));
		// Includes present pacing waits, so a high value alone is not a stall.
		Row("CPU waiting on GPU", Fmt("%.0f ms/s  (%.0f waits/s)", Rate(shown, Counter::GpuWaitNs) / 1e6,
		                              Rate(shown, Counter::GpuWaits)));
		const auto faults = Rate(shown, Counter::GpuFaults);
		Row("Write-tracking faults", Fmt("%.0f/s", faults), LevelColor(faults, 20000, 100000));
		const auto shaders = Total(shown, Counter::ShadersCompiled);
		Row("Shaders", Fmt("%llu  (avg %.1f ms, %.0f/s now)",
		                        static_cast<unsigned long long>(shaders),
		                        shaders != 0 ? static_cast<double>(Total(shown, Counter::ShaderCompileNs)) /
		                                           1e6 / static_cast<double>(shaders)
		                                     : 0.0,
		                        Rate(shown, Counter::ShadersCompiled)),
		    Rate(shown, Counter::ShadersCompiled) > 0.0 ? MEDIUM : GOOD);
		const auto pipelines = Total(shown, Counter::PipelinesCreated);
		Row("Pipelines", Fmt("%llu  (avg %.1f ms, %.0f/s now)",
		                          static_cast<unsigned long long>(pipelines),
		                          pipelines != 0
		                              ? static_cast<double>(Total(shown, Counter::PipelineCreateNs)) /
		                                    1e6 / static_cast<double>(pipelines)
		                              : 0.0,
		                          Rate(shown, Counter::PipelinesCreated)),
		    Rate(shown, Counter::PipelinesCreated) > 0.0 ? MEDIUM : GOOD);
		ImGui::EndTable();
	}
	if (BeginSection("GPU memory")) {
		if (shown.device_memory) {
			const double share = shown.device_budget != 0
			                         ? static_cast<double>(*shown.device_memory) /
			                               static_cast<double>(shown.device_budget) * 100.0
			                         : 0.0;
			Row("Vulkan budget", Bytes(*shown.device_memory) + " / " + Bytes(shown.device_budget),
			    LevelColor(share, 80, 95));
		}
		for (size_t heap = 0; heap < shown.heaps.size(); heap++) {
			const auto& usage = shown.heaps[heap];
			Row(Fmt("  Heap %zu%s", heap, usage.device_local ? " (device)" : "").c_str(),
			    Bytes(usage.usage) + " / " + Bytes(usage.budget) +
			        Fmt("  %u allocs", usage.allocations));
		}
		Row("Texture cache", Bytes(Common::DebugCounters::Get(Gauge::TextureCacheBytes)) +
		                         Fmt("  %llu images",
		                                  static_cast<unsigned long long>(
		                                      Common::DebugCounters::Get(Gauge::TextureImages))));
		Row("  Upload / download", Throughput(Rate(shown, Counter::TextureUploadBytes)) + " / " +
		                               Throughput(Rate(shown, Counter::TextureDownloadBytes)));
		Row("  Evictions", Fmt("%.0f/s", Rate(shown, Counter::TextureEvictions)));
		Row("Buffer cache", Bytes(Common::DebugCounters::Get(Gauge::BufferCacheBytes)));
		Row("  Upload / download", Throughput(Rate(shown, Counter::BufferUploadBytes)) + " / " +
		                               Throughput(Rate(shown, Counter::BufferDownloadBytes)));
		Row("  Evictions", Fmt("%.0f/s", Rate(shown, Counter::BufferEvictions)));
		ImGui::EndTable();
	}
}

void DrawMemoryAndIo(const Snapshot& shown) {
	const auto& host  = shown.host;
	const auto& guest = shown.guest_memory;
	if (BeginSection("PS5 memory")) {
		const auto direct_used = guest.direct_allocated + guest.pooled_allocated;
		Row("Direct", Bytes(direct_used) + " / " + Bytes(guest.direct_total),
		    LevelColor(guest.direct_total != 0 ? static_cast<double>(direct_used) /
		                                             static_cast<double>(guest.direct_total) * 100.0
		                                       : 0.0,
		               85, 97));
		Row("  Mapped", Bytes(guest.direct_mapped));
		Row("  Pool", Bytes(guest.pooled_allocated) + " (" + Bytes(guest.pool_committed) +
		                  " committed)");
		Row("Flexible", Bytes(guest.flexible_used) + " / " + Bytes(guest.flexible_total));
		Row("Extended", Bytes(guest.automatic_allocated));
		// Past the pool size the guest sees no page-table entries available.
		const auto entries = std::max(guest.cpu_page_entries, guest.gpu_page_entries);
		Row("Page table (2 MB)",
		    Fmt("CPU %llu  GPU %llu  / %llu",
		             static_cast<unsigned long long>(guest.cpu_page_entries),
		             static_cast<unsigned long long>(guest.gpu_page_entries),
		             static_cast<unsigned long long>(guest.page_entries_total)),
		    entries > guest.page_entries_total ? BAD
		    : entries * 10 > guest.page_entries_total * 9 ? MEDIUM
		                                                  : GOOD);
		ImGui::EndTable();
	}
	if (BeginSection("Emulator process")) {
		Row("Footprint", Bytes(host.process_memory));
		Row("Resident", Bytes(host.process_resident));
		if (host.process_compressed) {
			Row("Compressed / swapped", Bytes(*host.process_compressed),
			    *host.process_compressed > (1ull << 30u) ? MEDIUM : GOOD);
		}
		Row("File-backed", Bytes(host.process_file_backed));
		Row("Address space", Bytes(host.process_virtual));
		ImGui::EndTable();
	}
	if (BeginSection("System memory")) {
		if (host.system_memory_used && host.system_memory_total) {
			Row("Used", Bytes(*host.system_memory_used) + " / " + Bytes(*host.system_memory_total));
		}
		if (host.system_wired) {
			Row("Wired", Bytes(*host.system_wired));
		}
		if (host.system_compressed) {
			Row("Compressed", Bytes(*host.system_compressed));
		}
		Row("Cached files", Bytes(host.system_cached));
		Row("Free", Bytes(host.system_free));
		if (host.swap_used) {
			Row("Swap", Bytes(*host.swap_used) + " / " + Bytes(host.swap_total),
			    *host.swap_used > (4ull << 30u) ? MEDIUM : GOOD);
		}
		Row("Pressure", PressureName(host.memory_pressure), PressureColor(host.memory_pressure));
		ImGui::EndTable();
	}
	if (BeginSection("I/O & audio")) {
		Row("Disk read / write", Throughput(host.disk_read_per_s.value_or(0.0)) + " / " +
		                             Throughput(host.disk_write_per_s.value_or(0.0)));
		Row("Guest file read / write", Throughput(Rate(shown, Counter::GuestFileReadBytes)) + " / " +
		                                   Throughput(Rate(shown, Counter::GuestFileWriteBytes)));
		const auto underruns = Rate(shown, Counter::AudioUnderruns);
		Row("Audio underruns",
		    Fmt("%.0f/s  (%llu total)", underruns,
		             static_cast<unsigned long long>(Total(shown, Counter::AudioUnderruns))),
		    underruns > 0.0 ? BAD : GOOD);
		ImGui::EndTable();
	}
}

} // namespace

void PerformanceOverlayRecordFlip(uint32_t guest_width, uint32_t guest_height) noexcept {
	const auto       now = Clock::now();
	std::scoped_lock lock(g_frame_mutex);
	auto&            frames = g_frames;
	if (frames.last_flip != Clock::time_point {}) {
		const float ms = std::chrono::duration<float, std::milli>(now - frames.last_flip).count();
		frames.intervals_ms[frames.next] = ms;
		frames.next                      = (frames.next + 1) % FRAME_HISTORY;
		frames.count                     = std::min(frames.count + 1, FRAME_HISTORY);
		frames.max_ms                    = std::max(frames.max_ms, ms);
	}
	frames.last_flip    = now;
	frames.guest_width  = guest_width;
	frames.guest_height = guest_height;
	frames.flips++;
}

bool PerformanceOverlayEnabled() noexcept {
	EnsureEnabledLoaded();
	return g_enabled.load(std::memory_order_acquire);
}

void TogglePerformanceOverlay() noexcept {
	EnsureEnabledLoaded();
	g_enabled.store(!g_enabled.load(std::memory_order_acquire), std::memory_order_release);
	g_revision.fetch_add(1, std::memory_order_acq_rel);
}

void TogglePerformanceOverlayDetails() noexcept {
	EnsureEnabledLoaded();
	if (g_enabled.load(std::memory_order_acquire)) {
		g_details.store(!g_details.load(std::memory_order_acquire), std::memory_order_release);
	} else {
		g_details.store(true, std::memory_order_release);
		g_enabled.store(true, std::memory_order_release);
	}
	g_revision.fetch_add(1, std::memory_order_acq_rel);
}

uint64_t PerformanceOverlayRevision() noexcept {
	return g_revision.load(std::memory_order_acquire);
}

float DrawPerformanceOverlay(const GraphicContext& graphics, vk::Extent2D extent) {
	if (!PerformanceOverlayEnabled()) {
		return 0.0f;
	}
	EnsureSampler();

	static Snapshot                         shown;
	static Clock::time_point                next_refresh;
	static bool                             shown_details = false;
	static std::array<float, FRAME_HISTORY> history {};
	static size_t                           history_count = 0;
	const bool                              details = g_details.load(std::memory_order_acquire);
	const auto                              now     = Clock::now();
	const bool refresh = now >= next_refresh || details != shown_details;
	{
		std::scoped_lock lock(g_frame_mutex);
		auto&            frames = g_frames;
		history_count           = frames.count;
		for (size_t i = 0; i < frames.count; i++) {
			history[i] = frames.intervals_ms[(frames.next + FRAME_HISTORY - frames.count + i) %
			                                 FRAME_HISTORY];
		}
		if (refresh) {
			const double seconds = std::chrono::duration<double>(now - frames.window_start).count();
			shown.fps            = seconds > 0.0 ? frames.flips / seconds : 0.0;
			shown.frame_ms       = frames.flips > 0 ? seconds * 1000.0 / frames.flips : 0.0;
			shown.max_frame_ms   = frames.max_ms;
			shown.stalled_s      = frames.last_flip == Clock::time_point {}
			                           ? 0.0
			                           : std::chrono::duration<double>(now - frames.last_flip).count();
			shown.guest_width    = frames.guest_width;
			shown.guest_height   = frames.guest_height;
			frames.flips         = 0;
			frames.max_ms        = 0.0f;
			frames.window_start  = now;
		}
	}
	if (refresh) {
		next_refresh  = now + REFRESH_INTERVAL;
		shown_details = details;
		if (history_count > 0) {
			auto sorted = std::vector<float>(history.begin(), history.begin() + history_count);
			const auto percentile = sorted.begin() + static_cast<ptrdiff_t>(sorted.size() * 99 / 100);
			std::nth_element(sorted.begin(), percentile, sorted.end());
			shown.low_fps = *percentile > 0.0f ? 1000.0 / *percentile : 0.0;
		}
		Refresh(shown, graphics, now);
	}

	const float scale = std::clamp(std::min(static_cast<float>(extent.width) / 1920.0f,
	                                        static_cast<float>(extent.height) / 1080.0f),
	                               0.75f, 1.5f);
	ImGui::SetNextWindowPos({static_cast<float>(extent.width) - 12.0f * scale, 12.0f * scale},
	                        ImGuiCond_Always, {1.0f, 0.0f});
	ImGui::SetNextWindowBgAlpha(details ? 0.82f : 0.72f);
	constexpr ImGuiWindowFlags FLAGS =
	    ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
	    ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
	    ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoBringToFrontOnFocus |
	    ImGuiWindowFlags_AlwaysAutoResize;
	ImGui::PushFont(nullptr, (details ? 13.0f : 15.0f) * scale);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f * scale);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {10.0f * scale, 8.0f * scale});
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {8.0f * scale, 2.0f * scale});
	ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, {4.0f * scale, 1.0f * scale});
	if (ImGui::Begin("##kyty_performance_overlay", nullptr, FLAGS)) {
		if (shown.fps > 0.0) {
			ImGui::TextColored(Color(FrameTimeColor(shown.frame_ms)), "%.1f FPS   %.1f ms",
			                   shown.fps, shown.frame_ms);
		} else if (shown.stalled_s >= 1.0) {
			ImGui::TextColored(Color(BAD), "No frames for %.0f s", shown.stalled_s);
		} else {
			ImGui::TextDisabled("Waiting for frames");
		}
		DrawFrameGraph(history, history_count, (details ? 330.0f : 260.0f) * scale,
		               46.0f * scale);
		if (details) {
			// Three columns keep the detailed panel within a 720p window.
			if (ImGui::BeginTable("##details", 3, TABLE_FLAGS)) {
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				DrawFramesAndCpu(shown, extent);
				ImGui::TableNextColumn();
				DrawGpu(shown, graphics);
				ImGui::TableNextColumn();
				DrawMemoryAndIo(shown);
				ImGui::EndTable();
			}
		} else {
			DrawCompact(shown);
		}
#if defined(__APPLE__)
		ImGui::TextDisabled(details ? "Cmd+Shift+P compact   Cmd+P hide"
		                            : "Cmd+Shift+P details   Cmd+P hide");
#else
		ImGui::TextDisabled(details ? "Shift+F2 compact   F2 hide" : "Shift+F2 details   F2 hide");
#endif
	}
	const float bottom = ImGui::GetWindowPos().y + ImGui::GetWindowSize().y;
	ImGui::End();
	ImGui::PopStyleVar(4);
	ImGui::PopFont();
	return bottom;
}

} // namespace Libs::Graphics
